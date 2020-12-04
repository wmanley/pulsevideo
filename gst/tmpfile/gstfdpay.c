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
#include <gst/video/video.h>
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
static gboolean gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);
static GstFlowReturn gst_fdpay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

/* pad templates */

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
  base_transform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_fdpay_propose_allocation);
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

static gboolean
pay_caps (GstCapsFeatures * features, GstStructure * structure, gpointer user_data)
{
  gst_structure_set (structure, "payloaded-name", G_TYPE_STRING,
      gst_structure_get_name (structure), NULL);
  gst_structure_set_name (structure, "application/x-fd");
  return TRUE;
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
    othercaps = gst_caps_copy (caps);
    gst_caps_map_in_place (othercaps, pay_caps, fdpay);
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

/* propose allocation query parameters for input buffers */
static gboolean
gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstFdpay *fdpay = GST_FDPAY (trans);

  GstBufferPool *pool = NULL;
  GstStructure *pool_config = NULL;
  GstCaps *caps = NULL;
  gboolean need_pool = FALSE;
  GstVideoInfo info;

  GST_DEBUG_OBJECT (fdpay, "propose_allocation");

  gst_query_parse_allocation (query, &caps, &need_pool);

  /* Plain Allocator */
  gst_query_add_allocation_param (query, fdpay->allocator, NULL);

  if (!need_pool) {
    GST_INFO_OBJECT (fdpay, "No pool requested.  Not proposing pool");
    goto no_pool;
  }

  if (caps == NULL) {
    GST_WARNING_OBJECT (fdpay, "Pool requested but have no configured caps to "
        "determine size.  Not proposing pool");
    goto no_pool;
  }

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (fdpay, "Don't know the appropriate size for caps %"
        GST_PTR_FORMAT ".  Not proposing pool", caps);
    goto no_pool;
  }

  pool_config = gst_structure_new_empty ("pool_config");
  gst_buffer_pool_config_set_params (pool_config, caps, info.size, 0, 0);
  gst_buffer_pool_config_set_allocator (pool_config, fdpay->allocator, NULL);

  pool = gst_buffer_pool_new ();
  if (!gst_buffer_pool_set_config (pool, pool_config)) {
    GST_WARNING_OBJECT (fdpay, "Failed to set buffer pool config during "
        "allocation query: Not proposing pool");
    goto no_pool;
  }

  GST_INFO_OBJECT (fdpay, "Proposing pool");
  gst_query_add_allocation_pool (query, pool, 0, 0, 0);

  if (!GST_BASE_TRANSFORM_CLASS (gst_fdpay_parent_class)->propose_allocation (trans,
          decide_query, query))
    return FALSE;

no_pool:
  g_clear_object (&pool);

  return TRUE;
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

#ifdef __arm__
/*
 * Faster memcpy on with ARM NEON chips.  See
 * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.faqs/ka13544.html
 * for more information.
 */
static inline void
fast_memcpy(void * restrict dest, const void * restrict src, int n)
{
  int remainder = n % 64;
  n -= remainder;
  asm volatile (
      "NEONCopyPLD:                \n"
      "    PLD [%[src], #0xC0]     \n"
      "    VLDM %[src]!,{d0-d7}    \n"
      "    VSTM %[dest]!,{d0-d7}   \n"
      "    SUBS %[n],%[n],#0x40    \n"
      "    BGT NEONCopyPLD         \n"
      : [dest]"+r"(dest), [src]"+r"(src), [n]"+r"(n)
      :
      : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
  memcpy ((guint8*)dest, (const guint8*)src, remainder);
}
#else
static inline void
fast_memcpy(void * restrict dest, const void * restrict src, int n)
{
  memcpy(dest, src, n);
}
#endif


static GstMemory *
gst_fdpay_get_fd_memory (GstFdpay * tmpfilepay, GstBuffer * buffer)
{
  GstMemory *out = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0)))
    out = gst_buffer_get_memory (buffer, 0);
  else {
    GstMapInfo src_info, dest_info;
    GstMemory *mem = NULL;
    GstAllocationParams params = {0, 0, 0, 0};
    GST_INFO_OBJECT (tmpfilepay, "Buffer cannot be payloaded without copying");
    if (!gst_buffer_map (buffer, &src_info, GST_MAP_READ)) {
      GST_ERROR_OBJECT (tmpfilepay, "Failed to map input buffer");
      goto out2;
    }
    mem = gst_allocator_alloc (tmpfilepay->allocator, src_info.size, &params);
    if (mem == NULL) {
      GST_ERROR_OBJECT (tmpfilepay, "Failed to create new GstMemory");
      goto out;
    }
    if (!gst_memory_map (mem, &dest_info, GST_MAP_WRITE)) {
      GST_ERROR_OBJECT (tmpfilepay, "Failed to map output buffer");
      goto out;
    }

    fast_memcpy(dest_info.data, src_info.data, src_info.size);

    gst_memory_unmap (mem, &dest_info);

    out = mem;
    mem = NULL;
out:
    gst_buffer_unmap (buffer, &src_info);
out2:
    if (mem)
      gst_memory_unref (mem);
  }
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
