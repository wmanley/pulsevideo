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
#include <gst/base/gstbasetransform.h>
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
static GstCaps *gst_fddepay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static void gst_fddepay_dispose (GObject * object);

static GstFlowReturn gst_fddepay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

static gboolean gst_fddepay_stop (GstBaseTransform * trans);

static GQuark pipe_read_fd_quark;

/* Forget about memfds that we haven't received for this many messages.  The
 * sender recycles a pool of buffers, so in steady state we see each one every
 * few frames, but a client that holds on to frames (like stbt) makes the pool
 * bigger.  We don't want to keep memory alive for long after the sender has
 * freed a buffer.  A miss costs an mmap.  One-off copies aren't cached at all
 * (see PV_MSG_FLAG_MAY_REPEAT).
 * Can be overridden with $PULSEVIDEO_FDDEPAY_CACHE_MESSAGES. */
static guint64 fd_cache_evict_after_messages = 100;

typedef struct
{
  /* PvMessage.memory_id */
  guint64 memory_id;
  /* A GstFdMemory covering the whole file with GST_FD_MEMORY_FLAG_KEEP_MAPPED.
   * Buffers we push contain sub-memories shared from this, so the file is only
   * mapped once. */
  GstMemory *mem;
  guint64 last_seen;
} FdCacheEntry;

static void
fd_cache_entry_free (gpointer data)
{
  FdCacheEntry *entry = data;
  gst_memory_unref (entry->mem);
  g_slice_free (FdCacheEntry, entry);
}

static gboolean
fd_cache_entry_is_stale (gpointer key, gpointer value, gpointer user_data)
{
  FdCacheEntry *entry = value;
  guint64 n_messages = *(guint64 *) user_data;
  return n_messages - entry->last_seen > fd_cache_evict_after_messages;
}

static void
gst_fddepay_close_pipe_fd (gpointer data)
{
  int fd = GPOINTER_TO_INT (data);
  if (fd >= 0) {
    close (fd);
  }
}

/* pad templates */

static GstStaticCaps fd_caps = GST_STATIC_CAPS ("application/x-fd");

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

G_DEFINE_TYPE_WITH_CODE (GstFddepay, gst_fddepay, GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_fddepay_debug_category, "fddepay", 0,
        "debug category for fddepay element"));

static void
gst_fddepay_class_init (GstFddepayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

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
  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_fddepay_transform_caps);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_fddepay_transform_ip);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_fddepay_stop);

  pipe_read_fd_quark = g_quark_from_static_string ("gst-fddepay-pipe-read-fd");

  {
    const gchar *env = g_getenv ("PULSEVIDEO_FDDEPAY_CACHE_MESSAGES");
    if (env && *env)
      fd_cache_evict_after_messages = g_ascii_strtoull (env, NULL, 10);
  }
}

static void
gst_fddepay_init (GstFddepay * fddepay)
{
  GST_OBJECT_FLAG_SET (fddepay, GST_ELEMENT_FLAG_REQUIRE_CLOCK);

  fddepay->fd_allocator = gst_fd_allocator_new ();
  fddepay->monotonic_clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
      "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);
  GST_OBJECT_FLAG_SET (fddepay->monotonic_clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
  fddepay->fd_cache = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      NULL, fd_cache_entry_free);
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
  g_clear_pointer (&fddepay->fd_cache, g_hash_table_unref);

  G_OBJECT_CLASS (gst_fddepay_parent_class)->dispose (object);
}

static gboolean
gst_fddepay_stop (GstBaseTransform * trans)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);

  g_hash_table_remove_all (fddepay->fd_cache);
  fddepay->n_messages = 0;

  return TRUE;
}

/* Takes ownership of fd.  file_size is the size to map if we're going to keep
 * the mapping.  memory_id is 0 if the memory won't be sent again. */
static GstMemory *
gst_fddepay_get_memory (GstFddepay * fddepay, int fd, guint64 file_size,
    guint64 offset, guint64 size, guint64 memory_id)
{
  FdCacheEntry *entry;
  GstMemory *mem;

  if (memory_id == 0) {
    /* A one-off copy: there's no point caching it */
    mem = gst_fd_allocator_alloc (fddepay->fd_allocator, fd,
        offset + size, GST_FD_MEMORY_FLAG_KEEP_MAPPED);
    gst_memory_resize (mem, offset, size);
    GST_MINI_OBJECT_FLAG_SET (mem, GST_MEMORY_FLAG_READONLY);
    return mem;
  }

  entry = g_hash_table_lookup (fddepay->fd_cache, &memory_id);
  if (entry && entry->mem->maxsize < offset + size) {
    /* The file has grown since we mapped it */
    g_hash_table_remove (fddepay->fd_cache, &memory_id);
    entry = NULL;
  }

  if (entry) {
    GST_LOG_OBJECT (fddepay, "Reusing mapping of fd %i (memory_id %"
        G_GUINT64_FORMAT ")", gst_fd_memory_get_fd (entry->mem), memory_id);
    close (fd);
  } else {
    GST_DEBUG_OBJECT (fddepay, "New fd %i (memory_id %" G_GUINT64_FORMAT
        ", size %" G_GUINT64_FORMAT ")", fd, memory_id, file_size);
    entry = g_slice_new0 (FdCacheEntry);
    entry->memory_id = memory_id;
    entry->mem = gst_fd_allocator_alloc (fddepay->fd_allocator, fd,
        file_size, GST_FD_MEMORY_FLAG_KEEP_MAPPED);
    GST_MINI_OBJECT_FLAG_SET (entry->mem, GST_MEMORY_FLAG_READONLY);
    g_hash_table_insert (fddepay->fd_cache, &entry->memory_id, entry);
  }

  entry->last_seen = ++fddepay->n_messages;
  mem = gst_memory_share (entry->mem, offset, size);

  g_hash_table_foreach_remove (fddepay->fd_cache, fd_cache_entry_is_stale,
      &fddepay->n_messages);

  return mem;
}

static GstCaps *
gst_fddepay_transform_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);
  GstCaps *othercaps;

  GST_DEBUG_OBJECT (fddepay, "transform_caps");


  if (direction == GST_PAD_SRC) {
    /* transform caps going upstream */
    othercaps = gst_static_caps_get (&fd_caps);
  } else {
    /* transform caps going downstream */
    othercaps = gst_caps_new_any ();
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

static void
gst_fddepay_set_pts (GstFddepay * fddepay, GstBuffer * buf,
    guint64 capture_timestamp)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM (fddepay);
  GstClockTime pipeline_clock_time, running_time;

  if (trans->segment.format == GST_FORMAT_TIME) {
    GST_OBJECT_LOCK (fddepay->monotonic_clock);
    pipeline_clock_time = gst_clock_adjust_unlocked (fddepay->monotonic_clock,
        capture_timestamp);
    GST_OBJECT_UNLOCK (fddepay->monotonic_clock);
    if (GST_ELEMENT (trans)->base_time < pipeline_clock_time) {
      running_time = pipeline_clock_time - GST_ELEMENT (trans)->base_time;
    } else {
      GST_INFO_OBJECT (trans, "base time < clock time! %" GST_TIME_FORMAT " < "
          "%" GST_TIME_FORMAT, GST_TIME_ARGS (GST_ELEMENT (trans)->base_time),
          GST_TIME_ARGS (pipeline_clock_time));
      running_time = 0;
    }
    GST_BUFFER_PTS (buf) = gst_segment_to_position (
        &trans->segment, GST_FORMAT_TIME, running_time);

    GST_DEBUG_OBJECT (trans, "CLOCK_MONOTONIC capture timestamp %"
        GST_TIME_FORMAT " -> pipeline clock time %" GST_TIME_FORMAT " -> "
        "running time %" GST_TIME_FORMAT " -> PTS %" GST_TIME_FORMAT,
        GST_TIME_ARGS (capture_timestamp),
        GST_TIME_ARGS (pipeline_clock_time), GST_TIME_ARGS (running_time),
        GST_TIME_ARGS (GST_BUFFER_PTS (buf)));

  } else {
    GST_INFO_OBJECT (trans, "Can't apply timestamp to buffer: segment.format "
        "!= GST_FORMAT_TIME");
  }
}

static GstFlowReturn
gst_fddepay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);
  PvMessage msg;
  GstMemory *fdmem = NULL;
  GstNetControlMessageMeta * meta;
  GUnixFDList *fds = NULL;
  int fd = -1;
  int pipe_read_fd = -1;
  struct stat statbuf;

  GST_DEBUG_OBJECT (fddepay, "transform_ip");

  if (gst_buffer_get_size (buf) < sizeof (msg)) {
    GST_WARNING_OBJECT (fddepay, "fddepay: Message too short (%" G_GSIZE_FORMAT
        " bytes)", gst_buffer_get_size (buf));
    goto error;
  }
  gst_buffer_extract (buf, 0, &msg, sizeof (msg));

  if (msg.type == PV_MSG_INLINE_BUFFER) {
    if (gst_buffer_get_size (buf) != sizeof (msg) + msg.size) {
      GST_WARNING_OBJECT (fddepay, "fddepay: Inline message is the wrong size "
          "(%" G_GSIZE_FORMAT " != %" G_GSIZE_FORMAT " + %" G_GUINT64_FORMAT
          ")", gst_buffer_get_size (buf), sizeof (msg), msg.size);
      goto error;
    }
    gst_buffer_resize (buf, sizeof (msg), msg.size);
    gst_fddepay_set_pts (fddepay, buf, msg.capture_timestamp);
    return GST_FLOW_OK;
  }

  if (msg.type != PV_MSG_FD_BUFFER) {
    GST_WARNING_OBJECT (fddepay, "fddepay: Unknown message type %u", msg.type);
    goto error;
  }
  if (gst_buffer_get_size (buf) != sizeof (msg)) {
    GST_WARNING_OBJECT (fddepay, "fddepay: FD message is the wrong size (%"
        G_GSIZE_FORMAT " != %" G_GSIZE_FORMAT ")", gst_buffer_get_size (buf),
        sizeof (msg));
    goto error;
  }

  meta = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE));

  if (meta &&
      g_socket_control_message_get_msg_type (meta->message) == SCM_RIGHTS) {
    fds = g_unix_fd_message_get_fd_list ((GUnixFDMessage*) meta->message);
    meta = NULL;
  }

  if (g_unix_fd_list_get_length (fds) != 2) {
    GST_WARNING_OBJECT (fddepay, "fddepay: Expect to receive 2 FDs for each "
        "buffer (memfd + pipe read end), received %i", g_unix_fd_list_get_length (fds));
    goto error;
  }

  fd = g_unix_fd_list_get (fds, 0, NULL);
  pipe_read_fd = g_unix_fd_list_get (fds, 1, NULL);
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
  /* dma-bufs aren't regular files, and on older kernels st_size is 0.  If one
   * is too small mmap will fail. */
  if (G_UNLIKELY (S_ISREG (statbuf.st_mode) &&
          statbuf.st_size < msg.offset + msg.size)) {
    /* Note: This is for sanity and debugging rather than security.  To be
       secure we'd first need to check that it was a sealed memfd. */
    GST_WARNING_OBJECT (fddepay, "fddepay: Received fd %i is too small to "
        "contain data (%zi < %" G_GUINT64_FORMAT " + %" G_GUINT64_FORMAT ")",
        fd, (ssize_t) statbuf.st_size, msg.offset, msg.size);
    goto error;
  }
  fdmem = gst_fddepay_get_memory (fddepay, fd,
      MAX ((guint64) statbuf.st_size, msg.offset + msg.size), msg.offset,
      msg.size, (msg.flags & PV_MSG_FLAG_MAY_REPEAT) ? msg.memory_id : 0);
  fd = -1;

  gst_buffer_remove_all_memory (buf);
  gst_buffer_remove_meta (buf,
      gst_buffer_get_meta (buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE));
  gst_buffer_append_memory (buf, fdmem);
  fdmem = NULL;

  gst_fddepay_set_pts (fddepay, buf, msg.capture_timestamp);

  /* Attach pipe_read_fd to buffer so it gets closed when buffer is destroyed */
  if (pipe_read_fd >= 0) {
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf), pipe_read_fd_quark,
        GINT_TO_POINTER (pipe_read_fd), gst_fddepay_close_pipe_fd);
  }

  return GST_FLOW_OK;
error:
  if (fd >= 0)
    close (fd);
  if (pipe_read_fd >= 0)
    close (pipe_read_fd);
  return GST_FLOW_ERROR;
}
