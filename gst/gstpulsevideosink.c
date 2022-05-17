/* GStreamer
 * Copyright (C) <2016> William Manley <will@williammanley.net>
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
 * SECTION:element-pulsevideosink
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * # server:
 * gst-launch-1.0 videotestsrc ! pulsevideosink
 * # client:
 * gst-launch-1.0 pulsevideosrc ! xvimagesink
 * ]|
 * </refsect2>
 */

#include "fault.h"
#include "glib_compat.h"
#include "gstpulsevideosink.h"
#include <string.h>
#include <gio/gunixfdlist.h>
#include <gst/base/gstbasesink.h>
#include <gst/video/video.h>

#include <sys/socket.h>

GST_DEBUG_CATEGORY_STATIC (pulsevideosink_debug);
#define GST_CAT_DEFAULT pulsevideosink_debug

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
  PROP_OBJECT_PATH,
  PROP_CAPS,
};

#define gst_pulsevideo_sink_parent_class parent_class
G_DEFINE_TYPE (GstPulseVideoSink, gst_pulsevideo_sink, GST_TYPE_BIN);


static void gst_pulsevideo_sink_dispose (GObject * gobject);
static void gst_pulsevideo_sink_finalize (GObject * gobject);

static gboolean gst_pulsevideo_sink_deregister_dbus (GstPulseVideoSink * bsink);
static gboolean gst_pulsevideo_sink_register_dbus (GstPulseVideoSink * bsink);

static void gst_pulsevideo_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_pulsevideo_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_pulsevideo_sink_change_state (
    GstElement * element, GstStateChange transition);
static gboolean on_handle_attach (GstVideoSource2 *interface,
    GDBusMethodInvocation *invocation, GUnixFDList* fdlist, gpointer user_data);

static GstCaps *wait_get_caps (GstPad *pad, guint64 end_time, GError** err);

static void
gst_pulsevideo_sink_class_init (GstPulseVideoSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_pulsevideo_sink_set_property;
  gobject_class->get_property = gst_pulsevideo_sink_get_property;
  gobject_class->dispose = gst_pulsevideo_sink_dispose;
  gobject_class->finalize = gst_pulsevideo_sink_finalize;

  g_object_class_install_property (gobject_class, PROP_DBUS_CONNECTION,
      g_param_spec_object ("dbus-connection", "DBus Connection",
          "The connection to the DBus bus that the video should be served on",
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
  g_object_class_install_property (gobject_class, PROP_CAPS,
      g_param_spec_string ("caps", "Caps", "Caps to use",
          "video/x-raw,format=BGR,width=1280,height=720,framerate=30/1,interlace-mode=progressive",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  gst_element_class_set_static_metadata (gstelement_class,
      "PulseVideo sink", "Source/DBus",
      "Publish data on DBus by exposing the com.stbtester.VideoSource2 "
      "interface", "William Manley <will@williammanley.net>");

  gstelement_class->change_state = gst_pulsevideo_sink_change_state;
  GST_DEBUG_CATEGORY_INIT (pulsevideosink_debug, "pulsevideosink", 0,
      "PulseVideo Source");
}

static void
gst_pulsevideo_sink_init (GstPulseVideoSink * this)
{
  GstPad *internal_pad, *external_pad;
  GstElement *rawvideovalidate = NULL;

  this->capsfilter = gst_element_factory_make ("capsfilter", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->capsfilter));
  this->fdpay = gst_element_factory_make ("pvfdpay", NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->fdpay));
  this->socketsink = gst_parse_bin_from_description_full (
      "pvmultisocketsink buffers-max=2"
      "                  buffers-soft-max=1"
      "                  recover-policy=latest"
      "                  sync-method=latest"
      "                  sync=FALSE"
      "                  enable-last-sample=FALSE",
      TRUE, NULL, GST_PARSE_FLAG_NO_SINGLE_ELEMENT_BINS, NULL);
  gst_bin_add (GST_BIN (this), gst_object_ref (this->socketsink));
  gst_element_link_many (
        this->capsfilter, this->fdpay, this->socketsink, rawvideovalidate,
        NULL);

  internal_pad = gst_element_get_static_pad (this->capsfilter, "sink");
  external_pad = gst_ghost_pad_new ("sink", internal_pad);
  gst_element_add_pad (GST_ELEMENT (this), external_pad);
  gst_object_unref (internal_pad);


  this->dbus_interface = gst_video_source2_skeleton_new ();

  g_signal_connect_object (this->dbus_interface,
                           "handle-attach",
                           G_CALLBACK (on_handle_attach),
                           this, 0);
}

static void
gst_pulsevideo_sink_dispose (GObject * gobject)
{
  GstPulseVideoSink *this = GST_PULSEVIDEO_SINK (gobject);

  g_clear_object (&this->dbus);
  g_clear_object (&this->dbus_interface);
  g_clear_object (&this->capsfilter);
  g_clear_object (&this->fdpay);
  g_clear_object (&this->socketsink);

  G_OBJECT_CLASS (parent_class)->dispose (gobject);
}

static void
gst_pulsevideo_sink_finalize (GObject * gobject)
{
  GstPulseVideoSink *this = GST_PULSEVIDEO_SINK (gobject);

  g_free (g_steal_pointer (&this->bus_name));
  g_free (g_steal_pointer (&this->object_path));

  G_OBJECT_CLASS (parent_class)->finalize (gobject);
}

static void
gst_pulsevideo_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPulseVideoSink *sink = GST_PULSEVIDEO_SINK (object);

  switch (prop_id) {
    case PROP_DBUS_CONNECTION: {
      GDBusConnection *conn;
      conn = g_value_get_object (value);
      GST_OBJECT_LOCK (sink);
      SWAP (conn, sink->dbus);
      GST_OBJECT_UNLOCK (sink);
      g_clear_object (&conn);
      break;
    }
    case PROP_BUS_NAME: {
      gchar * bus_name = g_value_dup_string (value);
      GST_OBJECT_LOCK (sink);
      SWAP (bus_name, sink->bus_name);
      GST_OBJECT_UNLOCK (sink);
      g_free (bus_name);
      break;
    }
    case PROP_OBJECT_PATH: {
      gchar * object_path = g_value_dup_string (value);
      if (g_variant_is_object_path(object_path)) {
        GST_OBJECT_LOCK (sink);
        SWAP (object_path, sink->object_path);
        GST_OBJECT_UNLOCK (sink);
      }
      else
        GST_WARNING_OBJECT (sink, "Not setting property \"object-path\": \"%s\" "
            "is not a valid object path", object_path);
      g_free (object_path);
      break;
    }
    case PROP_CAPS: {
      GstCaps * caps = gst_caps_from_string (g_value_get_string (value));

      /* Convert back to normalise the caps: */
      gchar* capsstring = gst_caps_to_string (caps);

      g_object_set (sink->capsfilter, "caps", caps, NULL);
      g_object_set (G_OBJECT (sink->dbus_interface), "caps", capsstring, NULL);
      g_free (capsstring);
      gst_caps_unref (g_steal_pointer (&caps));
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pulsevideo_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPulseVideoSink *pulsevideosink = GST_PULSEVIDEO_SINK (object);

  switch (prop_id) {
    case PROP_DBUS_CONNECTION:
      GST_OBJECT_LOCK (pulsevideosink);
      g_value_set_object (value, pulsevideosink->dbus);
      GST_OBJECT_UNLOCK (pulsevideosink);
      break;
    case PROP_BUS_NAME:
      GST_OBJECT_LOCK (pulsevideosink);
      g_value_set_string (value, pulsevideosink->bus_name);
      GST_OBJECT_UNLOCK (pulsevideosink);
      break;
    case PROP_OBJECT_PATH: {
      GST_OBJECT_LOCK (pulsevideosink);
      g_value_set_string (value, pulsevideosink->object_path);
      GST_OBJECT_UNLOCK (pulsevideosink);
      break;
    }
    case PROP_CAPS: {
      GST_OBJECT_LOCK (pulsevideosink);
      g_object_get_property (G_OBJECT (pulsevideosink->dbus_interface), "caps",
          value);
      GST_OBJECT_UNLOCK (pulsevideosink);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_pulsevideo_sink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstPulseVideoSink *sink;
  GstStateChangeReturn result;

  sink = GST_PULSEVIDEO_SINK (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    gst_pulsevideo_sink_deregister_dbus ((GstPulseVideoSink *)element);
  }

  if ((result = GST_ELEMENT_CLASS (parent_class)->change_state (element,
              transition)) == GST_STATE_CHANGE_FAILURE)
    goto failure;

  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
    if (!gst_pulsevideo_sink_register_dbus ((GstPulseVideoSink*) element)) {
      result = GST_STATE_CHANGE_FAILURE;
      goto failure;
    }
  }

  return result;

  /* ERRORS */
failure:
  {
    GST_DEBUG_OBJECT (sink, "parent failed state change");
    return result;
  }
}

#if GST_VERSION_MINOR < 16
static void
gst_clear_caps (GstCaps **pcaps)
{
  if (*pcaps) {
    gst_caps_unref (*pcaps);
    *pcaps = NULL;
  }
}
#endif

static gboolean
on_handle_attach (GstVideoSource2         *interface,
                  GDBusMethodInvocation   *invocation,
                  GUnixFDList             *fdlist,
                  gpointer                user_data)
{
  GstPulseVideoSink * sink = (GstPulseVideoSink*) user_data;
  int fds[2] = {-1, -1};

  GSocket* our_socket = NULL;
  GVariant *their_socket_idx = NULL;
  GUnixFDList *their_socket_list = NULL;
  GError * gerror = NULL;
  int error = 0;
  GstPad *inpad = NULL;
  GstCaps *caps = NULL;
  gchar *caps_str = NULL;

  GST_DEBUG_OBJECT (sink, "Attaching client");

  error = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
  if (error) {
    g_set_error (&gerror, G_IO_ERROR, g_io_error_from_errno (errno),
        "socketpair failed with errno %i (%s)", errno, strerror(errno));
    goto out;
  }

  our_socket = g_socket_new_from_fd (fds[0], &gerror);
  if (our_socket == NULL) {
    goto out;
  }
  fds[0] = -1;

  their_socket_idx = g_variant_new_handle(0);
  their_socket_list = g_unix_fd_list_new_from_array(&fds[1], 1);
  fds[1] = -1;

  g_signal_emit_by_name (sink->socketsink, "add", our_socket, NULL);

  inpad = gst_element_get_static_pad (sink->fdpay, "sink");
  g_assert (inpad);
  caps = wait_get_caps (inpad,
      g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND, &gerror);
  if (!caps)
    goto out;
  caps_str = gst_caps_to_string (caps);

  static struct FaultInjectionPoint pre_attach = FAULT_INJECTION_POINT("pre_attach");
  if (!inject_fault (&pre_attach, &gerror))
    goto out;

  gst_video_source2_complete_attach (
      g_steal_pointer(&interface), invocation, their_socket_list,
      their_socket_idx, caps_str);

out:
  if (gerror) {
    GST_WARNING_OBJECT(sink, "Attach failed: %s", gerror->message);
    g_dbus_method_invocation_return_gerror(
        g_steal_pointer(&invocation), gerror);
    g_clear_error (&gerror);
  }
  g_free (caps_str);
  gst_clear_caps (&caps);
  g_clear_object (&inpad);
  close (fds[0]);
  close (fds[1]);
  g_clear_object (&our_socket);
//  if (their_socket_idx)
//    g_variant_unref (their_socket_idx);
  g_clear_object (&their_socket_list);
  return TRUE;
}

static GstCaps *
wait_get_caps (GstPad *pad, guint64 end_time, GError** err)
{
  /* Sleeping is a bit of a hack, but should be robust in its simplicicity */
  GstCaps *caps = NULL;
  while ((caps = gst_pad_get_current_caps (pad)) == NULL) {
    if (g_get_monotonic_time () > end_time) {
      g_set_error (err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
          "Timeout waiting for caps");
      break;
    }
    g_usleep (10000);
  }
  return caps;
}

GDBusConnection * connect_to_dbus(GDBusConnection * connection, GError ** error)
{
  if (connection)
    return g_object_ref (connection);
  connection = g_bus_get_sync (G_BUS_TYPE_STARTER, NULL, NULL);
  if (connection)
    return connection;
  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  return connection;
}

/* create a socket for connecting to remote server */
static gboolean
gst_pulsevideo_sink_register_dbus (GstPulseVideoSink * sink)
{
  GError *error = NULL;
  gboolean ret = FALSE;

  GST_OBJECT_LOCK (sink);
  sink->connection_in_use = connect_to_dbus (sink->dbus, &error);
  if (sink->connection_in_use == NULL) {
    GST_ERROR_OBJECT (sink, "Failed to connect to DBus");
    goto out;
  }
  if (!g_dbus_interface_skeleton_export (
          G_DBUS_INTERFACE_SKELETON (sink->dbus_interface),
          sink->connection_in_use, sink->object_path, &error))
  {
    GST_ERROR_OBJECT (sink, "Failed to export dbus object on path %s: %s",
        sink->object_path, error->message);
    goto out;
  }

  sink->bus_name_token = g_bus_own_name_on_connection (sink->connection_in_use,
      sink->bus_name, 0, NULL, NULL, NULL, NULL);

  ret = TRUE;
out:
  GST_OBJECT_UNLOCK (sink);
  g_clear_error (&error);
  return ret;
}

static gboolean
gst_pulsevideo_sink_deregister_dbus (GstPulseVideoSink * bsink)
{
  GstPulseVideoSink *sink = GST_PULSEVIDEO_SINK (bsink);

  GST_OBJECT_LOCK (sink);
  g_bus_unown_name (sink->bus_name_token);
  sink->bus_name_token = 0;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (
      sink->dbus_interface));
  g_clear_object (&sink->connection_in_use);
  GST_OBJECT_UNLOCK (sink);
  
  return TRUE;
}
