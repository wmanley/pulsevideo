/* GStreamer
 * Copyright (C) 2014-2020 William Manley <will@williammanley.net>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstfddepay
 *
 * The fddepay element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! fddepay ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstfddepay.h"
#include "wire-protocol.h"
#include "../gstnetcontrolmessagemeta.h"

#include <gst/gst.h>
#include <gst/base/gstbaseparse.h>
#include <gst/allocators/gstfdmemory.h>
#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


GST_DEBUG_CATEGORY_STATIC (gst_fddepay_debug_category);
#define GST_CAT_DEFAULT gst_fddepay_debug_category

/* prototypes */


static gboolean gst_fddepay_set_clock (GstElement * element,
    GstClock * clock);
static gboolean gst_fddepay_set_sink_caps (GstBaseParse * parse,
    GstCaps * caps);
static void gst_fddepay_dispose (GObject * object);
static GstFlowReturn gst_fddepay_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skipsize);

/* pad templates */

static GstStaticPadTemplate gst_fddepay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_fddepay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-fd"));


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstFddepay, gst_fddepay, GST_TYPE_BASE_PARSE,
    GST_DEBUG_CATEGORY_INIT (gst_fddepay_debug_category, "fddepay", 0,
        "debug category for fddepay element"));

static void
gst_fddepay_class_init (GstFddepayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseParseClass *base_parse_class = GST_BASE_PARSE_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_fddepay_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_fddepay_sink_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "Simple FD Deplayloder", "Generic",
      "Simple File-descriptor Depayloader for zero-copy video IPC",
      "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_fddepay_dispose;
  gstelement_class->set_clock = GST_DEBUG_FUNCPTR (gst_fddepay_set_clock);
  base_parse_class->set_sink_caps =
      GST_DEBUG_FUNCPTR (gst_fddepay_set_sink_caps);
  base_parse_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_fddepay_handle_frame);
}

static void
gst_fddepay_init (GstFddepay * fddepay)
{
  GST_OBJECT_FLAG_SET (fddepay, GST_ELEMENT_FLAG_REQUIRE_CLOCK);

  fddepay->fd_allocator = gst_fd_allocator_new ();
  fddepay->monotonic_clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
      "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);
  GST_OBJECT_FLAG_SET (fddepay->monotonic_clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
  gst_base_parse_set_min_frame_size (&fddepay->base_fddepay, sizeof (FDMessage));
}

void
gst_fddepay_dispose (GObject * object)
{
  GstFddepay *fddepay = GST_FDDEPAY (object);

  GST_DEBUG_OBJECT (fddepay, "dispose");

  /* clean up as possible.  may be called multiple times */
  if (fddepay->fd_allocator != NULL) {
    g_object_unref (G_OBJECT (fddepay->fd_allocator));
    fddepay->fd_allocator = NULL;
  }
  g_clear_object (&fddepay->monotonic_clock);

  G_OBJECT_CLASS (gst_fddepay_parent_class)->dispose (object);
}

static gboolean
depay_caps (GstCapsFeatures * features, GstStructure * structure, gpointer user_data)
{
  const gchar * name = gst_structure_get_string (structure, "payloaded-name");
  if (name) {
    gst_structure_set_name (structure, name);
    gst_structure_remove_field (structure, "payloaded-name");
  } else {
    GST_WARNING_OBJECT (user_data, "Caps didn't contain payloaded-name");
  }
  return TRUE;
}

static gboolean
gst_fddepay_set_sink_caps (GstBaseParse * parse, GstCaps * caps)
{
  GST_WARNING_OBJECT (parse, "set_sink_caps ()");
  g_autoptr(GstCaps) outcaps = gst_caps_copy (caps);
  gst_caps_map_in_place (outcaps, depay_caps, parse);
  return gst_pad_push_event (GST_BASE_PARSE_SRC_PAD (parse), gst_event_new_caps (outcaps));
}

static gboolean
gst_fddepay_set_clock (GstElement * element, GstClock * clock)
{
  GstFddepay *fddepay = GST_FDDEPAY (element);

  GST_DEBUG_OBJECT (fddepay, "set_clock (%" GST_PTR_FORMAT ")", clock);

  if (gst_clock_set_master (fddepay->monotonic_clock, clock)) {
    if (clock) {
      /* gst_clock_set_master is asynchronous and may take some time to sync.
       * To give it a helping hand we'll initialise it here so we don't send
       * through spurious timings with the first buffer. */
      gst_clock_set_calibration (fddepay->monotonic_clock,
          gst_clock_get_internal_time (fddepay->monotonic_clock),
          gst_clock_get_time (clock), 1, 1);
    }
  } else {
    GST_WARNING_OBJECT (element, "Failed to slave internal MONOTONIC clock %"
        GST_PTR_FORMAT " to master clock %" GST_PTR_FORMAT,
        fddepay->monotonic_clock, clock);
  }

  return GST_ELEMENT_CLASS (gst_fddepay_parent_class)->set_clock (element,
      clock);
}

static GstFlowReturn
gst_fddepay_handle_frame (GstBaseParse * parse, GstBaseParseFrame * frame, gint * skipsize)
{
  GstFddepay *fddepay = GST_FDDEPAY (parse);
  FDMessage msg;
  GstMemory *fdmem = NULL;
  g_autoptr(GstBuffer) outbuf = NULL;
  GstNetControlMessageMeta * meta;
  GUnixFDList *fds = NULL;
  int fd = -1;
  struct stat statbuf;
  GstClockTime pipeline_clock_time, running_time;
  gsize consumed_bytes = 0;

  GST_DEBUG_OBJECT (fddepay, "handle_frame");

  if (gst_buffer_extract (frame->buffer, 0, &msg, sizeof (msg)) < sizeof (msg)) {
    /* Need more data, this will be called again once more data is available */
    return GST_FLOW_OK;
  }

  if (msg.offset == -1) {
    /* No FD payloading, the data is inline */
    if (gst_buffer_get_size (frame->buffer) < msg.size + sizeof (msg)) {
      /* Need more data */
      gst_base_parse_set_min_frame_size (parse, msg.size + sizeof (msg));
      return GST_FLOW_OK;
    }
    outbuf = gst_buffer_copy_region (frame->buffer, GST_BUFFER_COPY_ALL,
                                     sizeof (msg), msg.size);
    gst_base_parse_set_min_frame_size (parse, sizeof (msg));
    consumed_bytes = msg.size + sizeof (msg);
  } else {
    consumed_bytes = sizeof (msg);
    meta = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
        frame->buffer, GST_NET_CONTROL_MESSAGE_META_API_TYPE));

    if (meta &&
        g_socket_control_message_get_msg_type (meta->message) == SCM_RIGHTS) {
      fds = g_unix_fd_message_get_fd_list ((GUnixFDMessage*) meta->message);
      meta = NULL;
    }

    if (g_unix_fd_list_get_length (fds) != 1) {
      GST_WARNING_OBJECT (fddepay, "fddepay: Expect to receive 1 FD for each "
          "buffer, received %i", g_unix_fd_list_get_length (fds));
      goto error;
    }

    fd = g_unix_fd_list_get (fds, 0, NULL);
    fds = NULL;

    if (fd == -1) {
      GST_WARNING_OBJECT (fddepay, "fddepay: Could not get FD from buffer's "
          "GUnixFDList");
      goto error;
    }

    if (G_UNLIKELY (fstat (fd, &statbuf) != 0)) {
      GST_WARNING_OBJECT (fddepay, "fddepay: Could not stat received fd %i: %s",
          fd, strerror(errno));
      goto error;
    }
    if (G_UNLIKELY (statbuf.st_size < msg.offset + msg.size)) {
      /* Note: This is for sanity and debugging rather than security.  To be
         secure we'd first need to check that it was a sealed memfd. */
      GST_WARNING_OBJECT (fddepay, "fddepay: Received fd %i is too small to "
          "contain data (%zi < %" G_GUINT64_FORMAT " + %" G_GUINT64_FORMAT ")",
          fd, (ssize_t) statbuf.st_size, msg.offset, msg.size);
      goto error;
    }
    fdmem = gst_fd_allocator_alloc (fddepay->fd_allocator, fd,
        msg.offset + msg.size, GST_FD_MEMORY_FLAG_NONE);
    fd = -1;
    gst_memory_resize (fdmem, msg.offset, msg.size);
    GST_MINI_OBJECT_FLAG_SET (fdmem, GST_MEMORY_FLAG_READONLY);

    outbuf = gst_buffer_copy_region (frame->buffer,
        GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, 0);
    gst_buffer_append_memory (outbuf, fdmem);
    fdmem = NULL;
  }

  if (parse->segment.format == GST_FORMAT_TIME) {
    GST_OBJECT_LOCK (fddepay->monotonic_clock);
    pipeline_clock_time = gst_clock_adjust_unlocked (fddepay->monotonic_clock,
        msg.capture_timestamp);
    GST_OBJECT_UNLOCK (fddepay->monotonic_clock);
    if (GST_ELEMENT (parse)->base_time < pipeline_clock_time) {
      running_time = pipeline_clock_time - GST_ELEMENT (parse)->base_time;
    } else {
      GST_INFO_OBJECT (parse, "base time < clock time! %" GST_TIME_FORMAT " < "
          "%" GST_TIME_FORMAT, GST_TIME_ARGS (GST_ELEMENT (parse)->base_time),
          GST_TIME_ARGS (pipeline_clock_time));
      running_time = 0;
    }
    GST_BUFFER_PTS (outbuf) = gst_segment_to_position (
        &parse->segment, GST_FORMAT_TIME, running_time);

    GST_DEBUG_OBJECT (parse, "CLOCK_MONOTONIC capture timestamp %"
        GST_TIME_FORMAT " -> pipeline clock time %" GST_TIME_FORMAT " -> "
        "running time %" GST_TIME_FORMAT " -> PTS %" GST_TIME_FORMAT,
        GST_TIME_ARGS (msg.capture_timestamp),
        GST_TIME_ARGS (pipeline_clock_time), GST_TIME_ARGS (running_time),
        GST_TIME_ARGS (GST_BUFFER_PTS (outbuf)));

  } else {
    GST_INFO_OBJECT (parse, "Can't apply timestamp to buffer: segment.format "
        "!= GST_FORMAT_TIME");
  }
  frame->out_buffer = g_steal_pointer (&outbuf);
  return gst_base_parse_finish_frame (parse, frame, consumed_bytes);
error:
  if (fd >= 0)
    close (fd);
  return GST_FLOW_ERROR;
}
