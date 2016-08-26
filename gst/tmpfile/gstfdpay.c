/* GStreamer
 * Copyright (C) 2014-2016 William Manley <will@williammanley.net>
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
 * SECTION:element-gstfdpay
 *
 * The tmpfilepay element enables zero-copy passing of buffers between
 * processes by allocating memory in a temporary file.  This is a proof of
 * concept example.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v videotestsrc ! video/x-raw,format=RGB,width=1920,height=1080 \
 *         ! fdpay ! fdsink fd=1 \
 *     | gst-launch-1.0 fdsrc fd=0 ! fddepay \
 *         ! video/x-raw,format=RGB,width=1920,height=1080 ! autovideosink
 * ]|
 * Video frames are created in the first gst-launch-1.0 process and displayed
 * by the second with no copying.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "wire-protocol.h"
#include "../fault.h"

#include <gst/gst.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/base/gstbasetransform.h>
#include "gstfdpay.h"
#include "gsttmpfileallocator.h"

#include "../gstnetcontrolmessagemeta.h"
#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

GST_DEBUG_CATEGORY_STATIC (gst_fdpay_debug_category);
#define GST_CAT_DEFAULT gst_fdpay_debug_category

#define GST_UNREF(x) \
  do { \
    if ( x ) \
      gst_object_unref ( x ); \
    x = NULL; \
  } while (0);


/* prototypes */

static void gst_fdpay_dispose (GObject * object);

static gboolean gst_fdpay_set_clock (GstElement * element, GstClock * clock);

static GstCaps *gst_fdpay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstFlowReturn gst_fdpay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

/* pad templates */

static GstStaticCaps fd_caps = GST_STATIC_CAPS ("application/x-fd");

static GstStaticPadTemplate gst_fdpay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-fd"));

static GstStaticPadTemplate gst_fdpay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstFdpay, gst_fdpay, GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_fdpay_debug_category, "fdpay", 0,
        "debug category for fdpay element"));

static void
gst_fdpay_class_init (GstFdpayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gst_element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (gst_element_class,
      gst_static_pad_template_get (&gst_fdpay_src_template));
  gst_element_class_add_pad_template (gst_element_class,
      gst_static_pad_template_get (&gst_fdpay_sink_template));

  gst_element_class_set_static_metadata (gst_element_class,
      "Simple FD Payloader", "Generic",
      "Simple File-descriptor Payloader for zero-copy video IPC",
      "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_fdpay_dispose;
  gst_element_class->set_clock = GST_DEBUG_FUNCPTR (gst_fdpay_set_clock);
  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform_caps);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform_ip);
}

static void
gst_fdpay_init (GstFdpay * fdpay)
{
  GST_OBJECT_FLAG_SET (fdpay, GST_ELEMENT_FLAG_REQUIRE_CLOCK);

  fdpay->allocator = gst_tmpfile_allocator_new ();
  fdpay->monotonic_clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
      "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);
  GST_OBJECT_FLAG_SET (fdpay->monotonic_clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
}

void
gst_fdpay_dispose (GObject * object)
{
  GstFdpay *fdpay = GST_FDPAY (object);

  GST_DEBUG_OBJECT (fdpay, "dispose");

  /* clean up as possible.  may be called multiple times */
  GST_UNREF(fdpay->allocator);
  GST_UNREF (fdpay->monotonic_clock);

  G_OBJECT_CLASS (gst_fdpay_parent_class)->dispose (object);
}

static GstCaps *
gst_fdpay_transform_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstFdpay *fdpay = GST_FDPAY (trans);
  GstCaps *othercaps;

  GST_DEBUG_OBJECT (fdpay, "transform_caps");


  if (direction == GST_PAD_SRC) {
    /* transform caps going upstream */
    othercaps = gst_caps_new_any ();
  } else {
    /* transform caps going downstream */
    othercaps = gst_static_caps_get (&fd_caps);
  }

  if (filter) {
    GstCaps *intersect;

    intersect = gst_caps_intersect (othercaps, filter);
    gst_caps_unref (othercaps);

    return intersect;
  } else {
    return othercaps;
  }
}

static gboolean
gst_fdpay_set_clock (GstElement * element, GstClock * clock)
{
  GstFdpay *fdpay = GST_FDPAY (element);

  GST_DEBUG_OBJECT (fdpay, "set_clock (%" GST_PTR_FORMAT ")", clock);

  if (gst_clock_set_master (fdpay->monotonic_clock, clock)) {
    if (clock) {
      /* gst_clock_set_master is asynchronous and may take some time to sync.
       * To give it a helping hand we'll initialise it here so we don't send
       * through spurious timings with the first buffer. */
      gst_clock_set_calibration (fdpay->monotonic_clock,
          gst_clock_get_internal_time (fdpay->monotonic_clock),
          gst_clock_get_time (clock), 1, 1);
    }
  } else {
    GST_WARNING_OBJECT (element, "Failed to slave internal MONOTONIC clock %"
        GST_PTR_FORMAT " to master clock %" GST_PTR_FORMAT,
        fdpay->monotonic_clock, clock);
  }

  return GST_ELEMENT_CLASS (gst_fdpay_parent_class)->set_clock (element,
      clock);
}

static GstMemory *
gst_fdpay_get_fd_memory (GstFdpay * tmpfilepay, GstBuffer * buffer)
{
  GstMemory *out = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0)))
    out = gst_buffer_get_memory (buffer, 0);
  else {
    GstMapInfo src_info;
    GST_INFO_OBJECT (tmpfilepay, "Buffer cannot be payloaded without copying");
    if (!gst_buffer_map (buffer, &src_info, GST_MAP_READ)) {
      GST_ERROR_OBJECT (tmpfilepay, "Failed to map input buffer");
      goto out;
    }

    out = gst_tmpfile_allocator_copy_alloc(tmpfilepay->allocator,
        src_info.data, src_info.size);
    if (out == NULL)
      GST_ERROR_OBJECT (tmpfilepay, "Failed to create new GstMemory");

    gst_buffer_unmap (buffer, &src_info);
  }
out:
  return out;
}

static GstFlowReturn
gst_fdpay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstFdpay *fdpay = GST_FDPAY (trans);
  GstAllocator *downstream_allocator = NULL;
  GstMemory *fdmem = NULL;
  GstMemory *msgmem;
  GstMapInfo info;
  GError *err = NULL;
  GSocketControlMessage *fdmsg = NULL;
  FDMessage msg = { 0, 0, 0 };
  GstClockTime pipeline_clock_time;

  GST_DEBUG_OBJECT (fdpay, "transform_ip");

  fdmem = gst_fdpay_get_fd_memory (fdpay, buf);
  gst_buffer_remove_all_memory (buf);

  msg.size = fdmem->size;
  msg.offset = fdmem->offset;

  fdmsg = g_unix_fd_message_new ();
  if (!g_unix_fd_message_append_fd ((GUnixFDMessage*) fdmsg,
          gst_fd_memory_get_fd (fdmem), &err)) {
    goto append_fd_failed;
  }
  gst_memory_unref(fdmem);
  fdmem = NULL;

  gst_buffer_add_net_control_message_meta (buf, fdmsg);
  g_clear_object (&fdmsg);

  gst_base_transform_get_allocator (trans, &downstream_allocator, NULL);
  msgmem = gst_allocator_alloc (downstream_allocator, sizeof (FDMessage), NULL);

  if (trans->segment.format == GST_FORMAT_TIME &&
      GST_CLOCK_TIME_IS_VALID (GST_BUFFER_PTS (buf))) {
    pipeline_clock_time = GST_ELEMENT (trans)->base_time +
        gst_segment_to_running_time (
            &trans->segment, GST_FORMAT_TIME, GST_BUFFER_PTS (buf));
    GST_OBJECT_LOCK (fdpay->monotonic_clock);
    msg.capture_timestamp = gst_clock_unadjust_unlocked (
        fdpay->monotonic_clock, pipeline_clock_time);
    GST_OBJECT_UNLOCK (fdpay->monotonic_clock);
  } else {
    msg.capture_timestamp = 0;
  }
  gst_memory_map (msgmem, &info, GST_MAP_WRITE);
  memcpy (info.data, &msg, sizeof (msg));
  gst_memory_unmap (msgmem, &info);
  GST_UNREF (downstream_allocator);

  gst_buffer_append_memory (buf, msgmem);
  msgmem = NULL;

  static struct FaultInjectionPoint fdpay_buffer = FAULT_INJECTION_POINT("fdpay_buffer");
  if (!inject_fault (&fdpay_buffer, NULL)) {
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (trans, "transform_ip: Pushing {"
      "capture_timestamp: %" G_GUINT64_FORMAT ", "
      "offset: %" G_GUINT64_FORMAT ", "
      "size: %" G_GUINT64_FORMAT "}", msg.capture_timestamp, msg.offset,
      msg.size);

  return GST_FLOW_OK;
append_fd_failed:
  GST_WARNING_OBJECT (trans, "Appending fd failed: %s", err->message);
  gst_memory_unref(fdmem);
  g_clear_error (&err);
  g_clear_object (&fdmsg);
  return GST_FLOW_ERROR;
}
