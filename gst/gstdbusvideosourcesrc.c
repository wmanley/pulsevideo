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
 * SECTION:element-dbusvideosourcesrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * # server:
 * pulsevideo videotestsrc
 * # client:
 * gst-launch-1.0 dbusvideosourcesrc ! autovideosink
 * ]|
 * </refsect2>
 */

#include "gstdbusvideosourcesrc.h"
#include "gstvideosource1.h"
#include <string.h>
#include <gio/gunixfdlist.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC (dbusvideosourcesrc_debug);
#define GST_CAT_DEFAULT dbusvideosourcesrc_debug

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

#define gst_dbus_videosource_src_parent_class parent_class
G_DEFINE_TYPE (GstDBusVideoSourceSrc, gst_dbus_videosource_src, GST_TYPE_BIN);


static void gst_dbus_videosource_src_finalize (GObject * gobject);

static gboolean gst_dbus_videosource_src_stop (GstDBusVideoSourceSrc * bsrc);
static gboolean gst_dbus_videosource_src_start (GstDBusVideoSourceSrc * bsrc);

static void gst_dbus_videosource_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_dbus_videosource_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_dbus_videosource_src_change_state (
    GstElement * element, GstStateChange transition);

static void
gst_dbus_videosource_src_class_init (GstDBusVideoSourceSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_dbus_videosource_src_set_property;
  gobject_class->get_property = gst_dbus_videosource_src_get_property;
  gobject_class->finalize = gst_dbus_videosource_src_finalize;

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
      "DBus VideoSource source", "Source/DBus",
      "Receive data from an object on DBus that exposes the "
      "com.stbtester.VideoSource1 interface",
      "William Manley <will@williammanley.net>");

  gstelement_class->change_state = gst_dbus_videosource_src_change_state;
  GST_DEBUG_CATEGORY_INIT (dbusvideosourcesrc_debug, "dbusvideosourcesrc", 0,
      "DBus VideoSource Source");
}

static void
gst_dbus_videosource_src_init (GstDBusVideoSourceSrc * this)
{
  GstPad *pad;

  this->cancellable = g_cancellable_new ();
  this->socketsrc = gst_element_factory_make ("pvsocketsrc", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->socketsrc));
  this->fddepay = gst_element_factory_make ("pvfddepay", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->fddepay));
  this->capsfilter = gst_element_factory_make ("capsfilter", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->capsfilter));
  gst_element_link_many (
        this->socketsrc, this->fddepay, this->capsfilter, NULL);

  pad = gst_element_get_static_pad (this->capsfilter, "src");
  gst_element_add_pad (GST_ELEMENT (this), gst_ghost_pad_new ("src", pad));
  gst_object_unref (pad);

}

static void
gst_dbus_videosource_src_finalize (GObject * gobject)
{
  GstDBusVideoSourceSrc *this = GST_DBUS_VIDEOSOURCE_SRC (gobject);

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
gst_dbus_videosource_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDBusVideoSourceSrc *src = GST_DBUS_VIDEOSOURCE_SRC (object);

  switch (prop_id) {
    case PROP_DBUS_CONNECTION: {
      GDBusConnection *conn;
      conn = g_value_get_object (value);
      SWAP (conn, src->dbus);
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
gst_dbus_videosource_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDBusVideoSourceSrc *dbusvideosourcesrc = GST_DBUS_VIDEOSOURCE_SRC (object);

  switch (prop_id) {
    case PROP_DBUS_CONNECTION:
      GST_OBJECT_LOCK (dbusvideosourcesrc);
      g_value_set_object (value, dbusvideosourcesrc->dbus);
      GST_OBJECT_UNLOCK (dbusvideosourcesrc);
      break;
    case PROP_BUS_NAME:
      GST_OBJECT_LOCK (dbusvideosourcesrc);
      g_value_set_string (value, dbusvideosourcesrc->bus_name);
      GST_OBJECT_UNLOCK (dbusvideosourcesrc);
      break;
    case PROP_OBJECT_PATH: {
      GST_OBJECT_LOCK (dbusvideosourcesrc);
      g_value_set_string (value, dbusvideosourcesrc->object_path);
      GST_OBJECT_UNLOCK (dbusvideosourcesrc);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_dbus_videosource_src_change_state (GstElement * element,
    GstStateChange transition)
{
  GstDBusVideoSourceSrc *src;
  GstStateChangeReturn result;

  src = GST_DBUS_VIDEOSOURCE_SRC (element);

  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
    if (!gst_dbus_videosource_src_start ((GstDBusVideoSourceSrc*) element)) {
      result = GST_STATE_CHANGE_FAILURE;
      goto failure;
    }
  }

  if ((result = GST_ELEMENT_CLASS (parent_class)->change_state (element,
              transition)) == GST_STATE_CHANGE_FAILURE)
    goto failure;

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_cancellable_cancel (src->cancellable);
    gst_dbus_videosource_src_stop ((GstDBusVideoSourceSrc *)element);
  }

  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
    result = GST_STATE_CHANGE_NO_PREROLL;
  }
  return result;

  /* ERRORS */
failure:
  {
    GST_DEBUG_OBJECT (src, "parent failed state change");
    return result;
  }
}

/* create a socket for connecting to remote server */
static gboolean
gst_dbus_videosource_src_start (GstDBusVideoSourceSrc * bsrc)
{
  GstDBusVideoSourceSrc *src = GST_DBUS_VIDEOSOURCE_SRC (bsrc);
  GDBusConnection *dbus = NULL;
  gchar *bus_name = NULL;
  gchar *object_path = NULL;

  GstVideoSource1 *videosource = NULL;
  GError *error = NULL;
  gboolean ret = FALSE;
  const gchar *scaps = NULL;
  GstCaps *caps = NULL;
  GUnixFDList *fdlist = NULL;
  gint *fds = NULL;
  GSocket *socket = NULL;
  
  GstVideoInfo video_info;

  GST_OBJECT_LOCK (src);
  if (src->dbus)
    dbus = g_object_ref (src->dbus);
  bus_name = g_strdup (src->bus_name);
  object_path = g_strdup (src->object_path);
  GST_OBJECT_UNLOCK (src);

  if (!dbus) {
    dbus = g_bus_get_sync (G_BUS_TYPE_SESSION, src->cancellable, &error);
    if (!dbus) {
      GST_ERROR_OBJECT (src, "Failed connecting to DBus: %s", error->message);
      goto done;
    }
  }

  videosource = gst_video_source1_proxy_new_sync (dbus,
      G_DBUS_PROXY_FLAGS_NONE, bus_name, object_path, src->cancellable, &error);
  if (!videosource) {
    GST_ERROR_OBJECT (src, "Could not create VideoSource DBus proxy: %s",
        error->message);
    goto done;
  }

  scaps = gst_video_source1_get_caps (videosource);
  if (!scaps) {
    GST_ERROR_OBJECT (src, "Could not read remote caps from %s on %s",
        object_path, bus_name);
    goto done;
  }
  caps = gst_caps_from_string (scaps);
  scaps = NULL;
  g_object_set (bsrc->capsfilter, "caps", caps, NULL);

  GST_INFO_OBJECT (src, "Received remote caps %" GST_PTR_FORMAT, caps);
  if (gst_video_info_from_caps (&video_info, caps)) {
    /* Best effort set the block size for raw video.  Really a proper parser or
     * payloading would be better.  Don't really care if it fails. */
    g_object_set (src->socketsrc, "blocksize", video_info.size, NULL);
    GST_DEBUG_OBJECT (src, "Buffer size is %u", (unsigned) video_info.size);
  } else {
    GST_DEBUG_OBJECT (src, "Unknown buffer size");
  }

  gst_caps_unref (caps);
  caps = NULL;

  if (!gst_video_source1_call_attach_sync (videosource, NULL, NULL, &fdlist,
          src->cancellable, &error)) {
    GST_ERROR_OBJECT (videosource, "Call to attach() failed: %s",
        error->message);
    goto done;
  }

  fds = g_unix_fd_list_steal_fds (fdlist, NULL);
  socket = g_socket_new_from_fd (fds[0], &error);
  if (!socket) {
    GST_ERROR_OBJECT (videosource, "Failed to create socket: %s",
        error->message);
    goto done;
  }

  g_object_set (src->socketsrc, "socket", socket, "do-timestamp", TRUE, NULL);

  ret = TRUE;

done:
  g_clear_object (&dbus);
  g_clear_object (&videosource);
  g_clear_object (&fdlist);
  g_clear_object (&socket);

  g_free (bus_name);
  g_free (object_path);
  g_free (fds);

  if (error)
    g_error_free (error);

  return ret;
}

static gboolean
gst_dbus_videosource_src_stop (GstDBusVideoSourceSrc * bsrc)
{
  GstDBusVideoSourceSrc *src = GST_DBUS_VIDEOSOURCE_SRC (bsrc);

  GDBusProxy *videosource = NULL;

  GST_OBJECT_LOCK (src);
  SWAP (videosource, src->videosource);
  GST_OBJECT_UNLOCK (src);

  g_clear_object (&videosource);

  return TRUE;
}
