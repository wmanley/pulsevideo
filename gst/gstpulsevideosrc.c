/* GStreamer
 * Copyright (C) <2014> William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-pulsevideosrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * # server:
 * pulsevideo videotestsrc
 * # client:
 * gst-launch-1.0 pulsevideosrc ! autovideosink
 * ]|
 * </refsect2>
 */

#include "gstpulsevideosrc.h"
#include "gstvideosource2.h"
#include <string.h>
#include <gio/gunixfdlist.h>
#include <gst/base/gstbasesrc.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC (pulsevideosrc_debug);
#define GST_CAT_DEFAULT pulsevideosrc_debug

#define SWAP(x,y) do \
   { unsigned char swap_temp[sizeof(x) == sizeof(y) ? (signed)sizeof(x) : -1]; \
     memcpy(swap_temp,&y,sizeof(x)); \
     memcpy(&y,&x,       sizeof(x)); \
     memcpy(&x,swap_temp,sizeof(x)); \
    } while(0)

enum
{
  PROP_0,
  PROP_DBUS_CONNECTION,
  PROP_BUS_NAME,
  PROP_OBJECT_PATH
};

typedef enum {
  PV_INIT_SUCCESS = 0,
  PV_INIT_FAILURE,
  PV_INIT_NOOBJECT,
} PvInitResult;

#define gst_pulsevideo_src_parent_class parent_class
G_DEFINE_TYPE (GstPulseVideoSrc, gst_pulsevideo_src, GST_TYPE_BIN);


static void gst_pulsevideo_src_finalize (GObject * gobject);

static gboolean gst_pulsevideo_src_stop (GstPulseVideoSrc * bsrc);
static gboolean gst_pulsevideo_src_start (GstPulseVideoSrc * bsrc);

static void gst_pulsevideo_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_pulsevideo_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static PvInitResult gst_pulsevideo_src_reinit (
    GstPulseVideoSrc * src, GError **error);

static void on_socket_eos (GstElement *socketsrc, gpointer user_data);
static GstStateChangeReturn gst_pulsevideo_src_change_state (
    GstElement * element, GstStateChange transition);

static void
gst_pulsevideo_src_class_init (GstPulseVideoSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_pulsevideo_src_set_property;
  gobject_class->get_property = gst_pulsevideo_src_get_property;
  gobject_class->finalize = gst_pulsevideo_src_finalize;

  g_object_class_install_property (gobject_class, PROP_DBUS_CONNECTION,
      g_param_spec_object ("dbus-connection", "DBus Connection",
          "The connection to the DBus bus that the video is being served on",
          G_TYPE_DBUS_CONNECTION, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BUS_NAME,
      g_param_spec_string ("bus-name", "Bus Name",
          "The DBus bus name of the video source",
          "com.stbtester.VideoSource.dev_video0",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
  g_object_class_install_property (gobject_class, PROP_OBJECT_PATH,
      g_param_spec_string ("object-path", "Object Path",
          "The DBus object path of the video source",
          "/com/stbtester/VideoSource",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  gst_element_class_set_static_metadata (gstelement_class,
      "PulseVideo source", "Source/DBus",
      "Receive data from an object on DBus that exposes the "
      "com.stbtester.VideoSource2 interface",
      "William Manley <will@williammanley.net>");

  gstelement_class->change_state = gst_pulsevideo_src_change_state;
  GST_DEBUG_CATEGORY_INIT (pulsevideosrc_debug, "pulsevideosrc", 0,
      "PulseVideo Source");
}

static void
on_socket_eos (GstElement *socketsrc, gpointer user_data)
{
  GstPulseVideoSrc *src = (GstPulseVideoSrc *) user_data;

  GST_INFO_OBJECT (src, "VideoSource has gone away, retrying connection");

  switch (gst_pulsevideo_src_reinit (src, NULL)) {
  case PV_INIT_SUCCESS:
    GST_INFO_OBJECT (src, "Successfully reconnected");
    break;
  case PV_INIT_NOOBJECT:
    GST_INFO_OBJECT (src, "Videosource has gone away for good, EOS");
    break;
  case PV_INIT_FAILURE:
    GST_WARNING_OBJECT (src, "Error reconnecting to Videosource");
    break;
  }
}

static void
gst_pulsevideo_src_init (GstPulseVideoSrc * this)
{
  GstPad *internal_pad, *external_pad;
  GstElement *rawvideovalidate = NULL;

  this->cancellable = g_cancellable_new ();
  this->socketsrc = gst_element_factory_make ("pvsocketsrc", NULL);
  gst_base_src_set_live (GST_BASE_SRC (this->socketsrc), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (this->socketsrc), GST_FORMAT_TIME);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->socketsrc));
  this->fddepay = gst_element_factory_make ("pvfddepay", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->fddepay));
  this->capsfilter = gst_element_factory_make ("capsfilter", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->capsfilter));
  rawvideovalidate = gst_element_factory_make ("rawvideovalidate", NULL);
  gst_bin_add (GST_BIN (this), rawvideovalidate);
  gst_element_link_many (
        this->socketsrc, this->fddepay, this->capsfilter, rawvideovalidate,
        NULL);

  internal_pad = gst_element_get_static_pad (rawvideovalidate, "src");
  external_pad = gst_ghost_pad_new ("src", internal_pad);
  gst_element_add_pad (GST_ELEMENT (this), external_pad);
  gst_object_unref (internal_pad);

  g_signal_connect (this->socketsrc, "on-socket-eos",
      G_CALLBACK (on_socket_eos), this);
}

static void
gst_pulsevideo_src_finalize (GObject * gobject)
{
  GstPulseVideoSrc *this = GST_PULSEVIDEO_SRC (gobject);

  g_free (this->bus_name);
  this->bus_name = NULL;
  g_free (this->object_path);
  g_clear_object (&this->cancellable);
  g_clear_object (&this->dbus);
  g_clear_object (&this->videosource);
  g_clear_object (&this->socketsrc);
  g_clear_object (&this->fddepay);
  g_clear_object (&this->capsfilter);

  G_OBJECT_CLASS (parent_class)->finalize (gobject);
}

static void
gst_pulsevideo_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPulseVideoSrc *src = GST_PULSEVIDEO_SRC (object);

  switch (prop_id) {
    case PROP_DBUS_CONNECTION: {
      GDBusConnection *conn;
      conn = g_value_get_object (value);
      GST_OBJECT_LOCK (src);
      SWAP (conn, src->dbus);
      GST_OBJECT_UNLOCK (src);
      g_clear_object (&conn);
      break;
    }
    case PROP_BUS_NAME: {
      gchar * bus_name = g_value_dup_string (value);
      GST_OBJECT_LOCK (src);
      SWAP (bus_name, src->bus_name);
      GST_OBJECT_UNLOCK (src);
      g_free (bus_name);
      break;
    }
    case PROP_OBJECT_PATH: {
      gchar * object_path = g_value_dup_string (value);
      if (g_variant_is_object_path(object_path)) {
        GST_OBJECT_LOCK (src);
        SWAP (object_path, src->object_path);
        GST_OBJECT_UNLOCK (src);
      }
      else
        GST_WARNING_OBJECT (src, "Not setting property \"object-path\": \"%s\" "
            "is not a valid object path", object_path);
      g_free (object_path);
      break;
    }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pulsevideo_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPulseVideoSrc *pulsevideosrc = GST_PULSEVIDEO_SRC (object);

  switch (prop_id) {
    case PROP_DBUS_CONNECTION:
      GST_OBJECT_LOCK (pulsevideosrc);
      g_value_set_object (value, pulsevideosrc->dbus);
      GST_OBJECT_UNLOCK (pulsevideosrc);
      break;
    case PROP_BUS_NAME:
      GST_OBJECT_LOCK (pulsevideosrc);
      g_value_set_string (value, pulsevideosrc->bus_name);
      GST_OBJECT_UNLOCK (pulsevideosrc);
      break;
    case PROP_OBJECT_PATH: {
      GST_OBJECT_LOCK (pulsevideosrc);
      g_value_set_string (value, pulsevideosrc->object_path);
      GST_OBJECT_UNLOCK (pulsevideosrc);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_pulsevideo_src_change_state (GstElement * element,
    GstStateChange transition)
{
  GstPulseVideoSrc *src;
  GstStateChangeReturn result;

  src = GST_PULSEVIDEO_SRC (element);

  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
    if (!gst_pulsevideo_src_start ((GstPulseVideoSrc*) element)) {
      result = GST_STATE_CHANGE_FAILURE;
      goto failure;
    }
  }

  if ((result = GST_ELEMENT_CLASS (parent_class)->change_state (element,
              transition)) == GST_STATE_CHANGE_FAILURE)
    goto failure;

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_cancellable_cancel (src->cancellable);
    gst_pulsevideo_src_stop ((GstPulseVideoSrc *)element);
  }

  return result;

  /* ERRORS */
failure:
  {
    GST_DEBUG_OBJECT (src, "parent failed state change");
    return result;
  }
}

static PvInitResult
gst_pulsevideo_src_reinit (GstPulseVideoSrc * src, GError **error)
{
  GDBusConnection *dbus = NULL;
  gchar *bus_name = NULL;
  gchar *object_path = NULL;
  GError *err = NULL;

  GstVideoSource2 *videosource = NULL;
  gboolean ret = PV_INIT_FAILURE;
  const gchar *scaps = NULL;
  GstCaps *caps = NULL;
  GUnixFDList *fdlist = NULL;
  gint *fds = NULL;
  GSocket *socket = NULL;
  
  GST_OBJECT_LOCK (src);
  if (src->dbus)
    dbus = g_object_ref (src->dbus);
  bus_name = g_strdup (src->bus_name);
  object_path = g_strdup (src->object_path);
  GST_OBJECT_UNLOCK (src);

  if (!dbus) {
    dbus = g_bus_get_sync (G_BUS_TYPE_SESSION, src->cancellable, &err);
    if (!dbus) {
      GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND, (NULL),
          ("Failed connecting to DBus: %s", err->message));
      goto done;
    }
  }

  videosource = gst_video_source2_proxy_new_sync (dbus,
      G_DBUS_PROXY_FLAGS_NONE, bus_name, object_path, src->cancellable, &err);
  if (!videosource) {
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND, (NULL),
        ("Could not create VideoSource DBus proxy: %s", err->message));
    goto done;
  }

  scaps = gst_video_source2_get_caps (videosource);
  if (!scaps) {
    ret = PV_INIT_NOOBJECT;
    g_set_error(&err, g_quark_from_static_string ("pv-read-caps-error-quark"),
        PV_INIT_FAILURE, "Could not read remote caps from %s on %s",
        object_path, bus_name);
    goto done;
  }
  caps = gst_caps_from_string (scaps);
  scaps = NULL;
  g_object_set (src->capsfilter, "caps", caps, NULL);

  GST_INFO_OBJECT (src, "Received remote caps %" GST_PTR_FORMAT, caps);
  gst_caps_unref (caps);
  caps = NULL;

  if (!gst_video_source2_call_attach_sync (videosource, NULL, NULL, &fdlist,
          src->cancellable, &err)) {
    ret = PV_INIT_NOOBJECT;
    goto done;
  }

  fds = g_unix_fd_list_steal_fds (fdlist, NULL);
  socket = g_socket_new_from_fd (fds[0], &err);
  if (!socket) {
    GST_ELEMENT_ERROR (videosource, RESOURCE, FAILED,
        (NULL), ("Failed to create socket: %s", err->message));
    goto done;
  }

  g_object_set (src->socketsrc, "socket", socket, "do-timestamp", TRUE, NULL);

  ret = PV_INIT_SUCCESS;

done:
  g_clear_object (&dbus);
  g_clear_object (&videosource);
  g_clear_object (&fdlist);
  g_clear_object (&socket);

  g_free (bus_name);
  g_free (object_path);
  g_free (fds);

  if (err)
    g_propagate_error (error, err);

  return ret;
}

/* create a socket for connecting to remote server */
static gboolean
gst_pulsevideo_src_start (GstPulseVideoSrc * src)
{
  GError *error = NULL;
  gboolean ret = FALSE;

  switch (gst_pulsevideo_src_reinit (src, &error)) {
  case PV_INIT_SUCCESS:
    ret = TRUE;
    break;
  case PV_INIT_NOOBJECT:
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND, (NULL),
        ("Call to VideoSource failed: %s", error->message));
    ret = FALSE;
    break;
  case PV_INIT_FAILURE:
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND, (NULL),
        ("Call to VideoSource failed: %s", error->message));
    ret = FALSE;
    break;
  default:
    g_return_val_if_reached(FALSE);
  }

  return ret;
}

static gboolean
gst_pulsevideo_src_stop (GstPulseVideoSrc * bsrc)
{
  GstPulseVideoSrc *src = GST_PULSEVIDEO_SRC (bsrc);

  GDBusProxy *videosource = NULL;

  GST_OBJECT_LOCK (src);
  SWAP (videosource, src->videosource);
  GST_OBJECT_UNLOCK (src);

  g_clear_object (&videosource);

  return TRUE;
}
