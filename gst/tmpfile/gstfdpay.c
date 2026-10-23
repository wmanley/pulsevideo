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
#include <errno.h>
#include <sys/epoll.h>
#include <glib/gstdio.h>

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
static void gst_fdpay_free_buffers (GstFdpay * fdpay);

static gboolean gst_fdpay_set_clock (GstElement * element, GstClock * clock);

static GstCaps *gst_fdpay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);
static gboolean gst_fdpay_transform_size (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, gsize size, GstCaps * othercaps,
    gsize * othersize);
static GstFlowReturn gst_fdpay_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf);

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
  base_transform_class->transform_size =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform_size);
  base_transform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_fdpay_propose_allocation);
  base_transform_class->transform =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform);
}

static void
gst_fdpay_close_pipe_fd (gpointer data)
{
  int fd = GPOINTER_TO_INT (data);
  if (fd >= 0) {
    close (fd);
  }
}

static void
gst_fdpay_init (GstFdpay * fdpay)
{
  GST_OBJECT_FLAG_SET (fdpay, GST_ELEMENT_FLAG_REQUIRE_CLOCK);

  fdpay->allocator = gst_tmpfile_allocator_new ();
  fdpay->monotonic_clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
      "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);
  GST_OBJECT_FLAG_SET (fdpay->monotonic_clock, GST_CLOCK_FLAG_CAN_SET_MASTER);

  g_mutex_init (&fdpay->pipe_write_fds_lock);
  fdpay->pipe_write_fds = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      (GDestroyNotify) gst_fdpay_close_pipe_fd, (GDestroyNotify) gst_buffer_unref);

  fdpay->epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
  if (fdpay->epoll_fd == -1) {
    GST_WARNING_OBJECT (fdpay, "Failed to create epoll fd: %s",
        g_strerror (errno));
  }
}

static void
gst_fdpay_free_buffers (GstFdpay * fdpay)
{
  struct epoll_event events[32];
  int num_events;
  int i;

  if (fdpay->epoll_fd == -1) {
    return;
  }

  /* Poll for events with zero timeout (non-blocking) */
  num_events = epoll_wait (fdpay->epoll_fd, events, 32, 0);
  if (num_events == -1) {
    GST_WARNING_OBJECT (fdpay, "epoll_wait failed: %s", g_strerror (errno));
    return;
  }

  /* Process each event */
  g_mutex_lock (&fdpay->pipe_write_fds_lock);
  for (i = 0; i < num_events; i++) {
    int pipe_fd = events[i].data.fd;
    
    /* Check if this is an error or hangup event */
    if (events[i].events & (EPOLLHUP | EPOLLERR)) {
      GST_DEBUG_OBJECT (fdpay, "Pipe fd %d closed by receiver, removing from hash table",
          pipe_fd);
      
      /* Remove the fd from epoll and the hash table */
      if (epoll_ctl (fdpay->epoll_fd, EPOLL_CTL_DEL, pipe_fd, NULL) == -1) {
        GST_WARNING_OBJECT (fdpay, "Failed to remove fd %d from epoll: %s",
            pipe_fd, g_strerror (errno));
      }
      
      /* Remove from hash table (this will also close the fd and unref the buffer) */
      if (!g_hash_table_remove (fdpay->pipe_write_fds, GINT_TO_POINTER (pipe_fd))) {
        GST_WARNING_OBJECT (fdpay, "Fd %d not found in hash table", pipe_fd);
      }
    }
  }
  g_mutex_unlock (&fdpay->pipe_write_fds_lock);
}

void
gst_fdpay_dispose (GObject * object)
{
  GstFdpay *fdpay = GST_FDPAY (object);

  GST_DEBUG_OBJECT (fdpay, "dispose");

  /* clean up as possible.  may be called multiple times */
  GST_UNREF(fdpay->allocator);
  GST_UNREF (fdpay->monotonic_clock);

  if (fdpay->epoll_fd != -1) {
    close (fdpay->epoll_fd);
    fdpay->epoll_fd = -1;
  }

  if (fdpay->pipe_write_fds) {
    g_hash_table_unref (fdpay->pipe_write_fds);
    fdpay->pipe_write_fds = NULL;
  }
  if (fdpay->copy_pool) {
    gst_buffer_pool_set_active (fdpay->copy_pool, FALSE);
    g_clear_object (&fdpay->copy_pool);
  }
  g_mutex_clear (&fdpay->pipe_write_fds_lock);

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
gst_fdpay_transform_size (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, gsize size, GstCaps * othercaps, gsize * othersize)
{
  if (direction == GST_PAD_SINK) {
    *othersize = sizeof (PvMessage);
    if (size <= PV_MSG_MAX_INLINE)
      *othersize += size;
    return TRUE;
  } else {
    return FALSE;
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

  /* We only propose our allocator along with a pool.  Without a pool every
   * buffer would be a new tmpfile, which is more expensive than copying into
   * our own copy pool, and receivers couldn't keep them mapped. */
  gst_query_add_allocation_param (query, fdpay->allocator, NULL);

  GST_INFO_OBJECT (fdpay, "Proposing pool");
  gst_query_add_allocation_pool (query, pool, info.size, 8, 0);

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

static GstBuffer *
gst_fdpay_copy_to_pool (GstFdpay * fdpay, const void *data, gsize size)
{
  GstBuffer *copy = NULL;

  if (fdpay->copy_pool == NULL || fdpay->copy_pool_size < size) {
    GstStructure *config;
    gsize pool_size = (size + 4095) & ~(gsize) 4095;

    if (fdpay->copy_pool) {
      gst_buffer_pool_set_active (fdpay->copy_pool, FALSE);
      g_clear_object (&fdpay->copy_pool);
    }
    fdpay->copy_pool = gst_buffer_pool_new ();
    config = gst_buffer_pool_get_config (fdpay->copy_pool);
    gst_buffer_pool_config_set_params (config, NULL, pool_size, 0, 0);
    gst_buffer_pool_config_set_allocator (config, fdpay->allocator, NULL);
    if (!gst_buffer_pool_set_config (fdpay->copy_pool, config) ||
        !gst_buffer_pool_set_active (fdpay->copy_pool, TRUE)) {
      GST_ERROR_OBJECT (fdpay, "Failed to configure copy pool");
      g_clear_object (&fdpay->copy_pool);
      return NULL;
    }
    fdpay->copy_pool_size = pool_size;
  }

  if (gst_buffer_pool_acquire_buffer (fdpay->copy_pool, &copy, NULL) !=
      GST_FLOW_OK) {
    GST_ERROR_OBJECT (fdpay, "Failed to acquire buffer from copy pool");
    return NULL;
  }
  gst_buffer_fill (copy, 0, data, size);
  gst_buffer_set_size (copy, size);
  return copy;
}

/* Raw audio and video come in buffers of the same size over and over, so
 * it's worth recycling the memory we copy them into.  Encoded data varies in
 * size and receivers may hold on to lots of it, so we don't. */
static gboolean
gst_fdpay_is_raw (GstFdpay * fdpay)
{
  GstCaps *caps;
  const gchar *name;
  gboolean raw = FALSE;

  caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM_SINK_PAD (fdpay));
  if (caps && gst_caps_get_size (caps) > 0) {
    name = gst_structure_get_name (gst_caps_get_structure (caps, 0));
    raw = g_str_equal (name, "video/x-raw") || g_str_equal (name, "audio/x-raw");
  }
  if (caps)
    gst_caps_unref (caps);
  return raw;
}

/* Returns the fd memory to send and sets *hold to the buffer that we need to
 * keep alive until the receiver is done with it.  *may_repeat is set if the
 * memory belongs to a buffer pool, so we may send it again. */
static GstMemory *
gst_fdpay_get_fd_memory (GstFdpay * fdpay, GstBuffer * buffer,
    gboolean * may_repeat, GstBuffer ** hold)
{
  GstMemory *out = NULL;
  GstMapInfo src_info;
  GstBuffer *copy;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    *may_repeat = buffer->pool != NULL;
    *hold = gst_buffer_ref (buffer);
    return gst_buffer_get_memory (buffer, 0);
  }

  GST_LOG_OBJECT (fdpay, "Buffer cannot be payloaded without copying");
  if (!gst_buffer_map (buffer, &src_info, GST_MAP_READ)) {
    GST_ERROR_OBJECT (fdpay, "Failed to map input buffer");
    return NULL;
  }
  if (gst_fdpay_is_raw (fdpay)) {
    copy = gst_fdpay_copy_to_pool (fdpay, src_info.data, src_info.size);
    gst_buffer_unmap (buffer, &src_info);
    if (copy == NULL)
      return NULL;
    out = gst_buffer_get_memory (copy, 0);
    *may_repeat = TRUE;
    *hold = copy;
  } else {
    out = gst_tmpfile_allocator_copy_alloc (fdpay->allocator, src_info.data,
        src_info.size);
    gst_buffer_unmap (buffer, &src_info);
    if (out == NULL)
      GST_ERROR_OBJECT (fdpay, "Failed to create new GstMemory");
    *may_repeat = FALSE;
    *hold = gst_buffer_ref (buffer);
  }
  return out;
}

/* The time at which inbuf was captured against CLOCK_MONOTONIC */
static guint64
gst_fdpay_capture_timestamp (GstFdpay * fdpay, GstBuffer * inbuf)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM (fdpay);
  GstClockTime pipeline_clock_time;
  guint64 ts;

  if (trans->segment.format != GST_FORMAT_TIME ||
      !GST_CLOCK_TIME_IS_VALID (GST_BUFFER_PTS (inbuf)))
    return 0;

  pipeline_clock_time = GST_ELEMENT (trans)->base_time +
      gst_segment_to_running_time (
          &trans->segment, GST_FORMAT_TIME, GST_BUFFER_PTS (inbuf));
  GST_OBJECT_LOCK (fdpay->monotonic_clock);
  ts = gst_clock_unadjust_unlocked (fdpay->monotonic_clock,
      pipeline_clock_time);
  GST_OBJECT_UNLOCK (fdpay->monotonic_clock);
  return ts;
}

static GstFlowReturn
gst_fdpay_transform_inline (GstFdpay * fdpay, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  PvMessage msg = { 0, };
  GstMapInfo info;
  gsize size = gst_buffer_get_size (inbuf);

  msg.type = PV_MSG_INLINE_BUFFER;
  msg.capture_timestamp = gst_fdpay_capture_timestamp (fdpay, inbuf);
  msg.size = size;

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_WARNING_OBJECT (fdpay, "Failed to map output buffer for writing");
    return GST_FLOW_ERROR;
  }
  if (info.size != sizeof (msg) + size) {
    GST_WARNING_OBJECT (fdpay, "Output buffer is the wrong size (%"
        G_GSIZE_FORMAT " != %" G_GSIZE_FORMAT ")", info.size,
        sizeof (msg) + size);
    gst_buffer_unmap (outbuf, &info);
    return GST_FLOW_ERROR;
  }
  memcpy (info.data, &msg, sizeof (msg));
  gst_buffer_extract (inbuf, 0, info.data + sizeof (msg), size);
  gst_buffer_unmap (outbuf, &info);

  GST_DEBUG_OBJECT (fdpay, "transform: Pushing inline {capture_timestamp: %"
      G_GUINT64_FORMAT ", size: %" G_GUINT64_FORMAT "}",
      msg.capture_timestamp, msg.size);
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_fdpay_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstFdpay *fdpay = GST_FDPAY (trans);
  GstMemory *fdmem = NULL;
  GstMapInfo info;
  GError *err = NULL;
  GSocketControlMessage *fdmsg = NULL;
  PvMessage msg = { 0, };
  gboolean may_repeat;
  GstBuffer *hold = NULL;
  int pipe_fds[2];

  GST_DEBUG_OBJECT (fdpay, "transform");

  gst_fdpay_free_buffers (fdpay);

  if (gst_buffer_get_size (inbuf) <= PV_MSG_MAX_INLINE)
    return gst_fdpay_transform_inline (fdpay, inbuf, outbuf);

  msg.type = PV_MSG_FD_BUFFER;
  msg.capture_timestamp = gst_fdpay_capture_timestamp (fdpay, inbuf);

  fdmem = gst_fdpay_get_fd_memory (fdpay, inbuf, &may_repeat, &hold);
  if (fdmem == NULL) {
    GST_ERROR_OBJECT (fdpay, "Failed to get fd memory for buffer");
    return GST_FLOW_ERROR;
  }

  msg.size = fdmem->size;
  msg.flags = may_repeat ? PV_MSG_FLAG_MAY_REPEAT : 0;
  msg.offset = fdmem->offset;

  fdmsg = g_unix_fd_message_new ();
  if (!g_unix_fd_message_append_fd ((GUnixFDMessage*) fdmsg,
          gst_fd_memory_get_fd (fdmem), &err)) {
    goto append_fd_failed;
  }
  gst_memory_unref(fdmem);
  fdmem = NULL;

  if (pipe (pipe_fds) == -1) {
    GST_WARNING_OBJECT (trans, "Failed to create pipe: %s", g_strerror (errno));
    g_clear_object (&fdmsg);
    g_clear_pointer (&hold, gst_buffer_unref);
    return GST_FLOW_ERROR;
  }

  if (!g_unix_fd_message_append_fd ((GUnixFDMessage*) fdmsg, pipe_fds[0], &err)) {
    goto append_pipe_fd_failed;
  }
  close (pipe_fds[0]);

  /* Register write end with epoll to detect when receiver closes read end (EPIPE) */
  if (fdpay->epoll_fd != -1) {
    struct epoll_event ev;
    ev.events = EPOLLHUP | EPOLLERR;
    ev.data.fd = pipe_fds[1];
    if (epoll_ctl (fdpay->epoll_fd, EPOLL_CTL_ADD, pipe_fds[1], &ev) == -1) {
      GST_WARNING_OBJECT (trans, "Failed to register fd with epoll: %s",
          g_strerror (errno));
    }
  }

  /* Store mapping of write fd to buffer for later lifecycle tracking */
  g_mutex_lock (&fdpay->pipe_write_fds_lock);
  g_hash_table_insert (fdpay->pipe_write_fds, GINT_TO_POINTER (pipe_fds[1]),
      hold);
  hold = NULL;
  pipe_fds[1] = -1; /* ownership transferred to hash table */
  g_mutex_unlock (&fdpay->pipe_write_fds_lock);

  gst_buffer_add_net_control_message_meta (outbuf, fdmsg);
  g_clear_object (&fdmsg);

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (fdpay, "Failed to map output buffer");
    return GST_FLOW_ERROR;
  }
  memcpy (info.data, &msg, sizeof (msg));
  gst_buffer_unmap (outbuf, &info);

  static struct FaultInjectionPoint fdpay_buffer = FAULT_INJECTION_POINT("fdpay_buffer");
  if (!inject_fault (&fdpay_buffer, NULL)) {
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (trans, "transform: Pushing {"
      "capture_timestamp: %" G_GUINT64_FORMAT ", "
      "offset: %" G_GUINT64_FORMAT ", "
      "size: %" G_GUINT64_FORMAT "}", msg.capture_timestamp, msg.offset,
      msg.size);

  return GST_FLOW_OK;
append_pipe_fd_failed:
  GST_WARNING_OBJECT (trans, "Appending pipe fd failed: %s", err->message);
  g_clear_pointer (&hold, gst_buffer_unref);
  close (pipe_fds[0]);
  close (pipe_fds[1]);
  g_clear_error (&err);
  g_clear_object (&fdmsg);
  return GST_FLOW_ERROR;
append_fd_failed:
  GST_WARNING_OBJECT (trans, "Appending fd failed: %s", err->message);
  g_clear_pointer (&hold, gst_buffer_unref);
  gst_memory_unref(fdmem);
  g_clear_error (&err);
  g_clear_object (&fdmsg);
  return GST_FLOW_ERROR;
}
