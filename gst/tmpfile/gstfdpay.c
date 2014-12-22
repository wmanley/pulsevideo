/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
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

static GstCaps *gst_fdpay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);
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
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fdpay_src_template));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fdpay_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Simple FD Payloader", "Generic",
      "Simple File-descriptor Payloader for zero-copy video IPC",
      "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_fdpay_dispose;
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
  fdpay->allocator = gst_tmpfile_allocator_new ();
}

void
gst_fdpay_dispose (GObject * object)
{
  GstFdpay *fdpay = GST_FDPAY (object);

  GST_DEBUG_OBJECT (fdpay, "dispose");

  /* clean up as possible.  may be called multiple times */
  GST_UNREF(fdpay->allocator);

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

/* propose allocation query parameters for input buffers */
static gboolean
gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstFdpay *fdpay = GST_FDPAY (trans);

  GST_DEBUG_OBJECT (fdpay, "propose_allocation");

  gst_query_add_allocation_param (query, fdpay->allocator, NULL);

  return TRUE;
}

static GstMemory *
gst_fdpay_get_dmabuf_memory (GstFdpay * tmpfilepay, GstBuffer * buffer)
{
  GstMemory *mem = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_memory_is_type (gst_buffer_peek_memory (buffer, 0),
          GST_ALLOCATOR_DMABUF))
    mem = gst_buffer_get_memory (buffer, 0);
  else {
    GstMapInfo info;
    gsize size = gst_buffer_get_size (buffer);
    GST_INFO_OBJECT (tmpfilepay, "Buffer cannot be payloaded without copying");
    mem = gst_allocator_alloc (tmpfilepay->allocator, size, NULL);
    if (!gst_memory_map (mem, &info, GST_MAP_WRITE))
      return NULL;
    gst_buffer_extract (buffer, 0, info.data, size);
    gst_memory_unmap (mem, &info);
  }
  return mem;
}

static GstFlowReturn
gst_fdpay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstFdpay *fdpay = GST_FDPAY (trans);
  GstAllocator *downstream_allocator = NULL;
  GstMemory *dmabufmem;
  GstMemory *msgmem;
  GstMapInfo info;
  GError *err = NULL;
  GSocketControlMessage *fdmsg = NULL;
  FDMessage msg = { 0, 0 };
  int fd;

  GST_DEBUG_OBJECT (fdpay, "transform_ip");

  dmabufmem = gst_fdpay_get_dmabuf_memory (fdpay, buf);
  gst_buffer_remove_all_memory (buf);

  msg.size = dmabufmem->size;
  msg.offset = dmabufmem->offset;

  fd = dup (gst_dmabuf_memory_get_fd (dmabufmem));
  fcntl (fd, F_SETFD, FD_CLOEXEC);
  gst_memory_unref(dmabufmem);

  fdmsg = g_unix_fd_message_new ();
  if (!g_unix_fd_message_append_fd((GUnixFDMessage*) fdmsg, fd, &err)) {
    goto append_fd_failed;
  }
  gst_buffer_add_net_control_message_meta (buf, fdmsg);
  g_clear_object (&fdmsg);

  gst_base_transform_get_allocator (trans, &downstream_allocator, NULL);
  msgmem = gst_allocator_alloc (downstream_allocator, sizeof (FDMessage), NULL);
  gst_memory_map (msgmem, &info, GST_MAP_WRITE);
  memcpy (info.data, &msg, sizeof (msg));
  gst_memory_unmap (msgmem, &info);
  GST_UNREF (downstream_allocator);

  gst_buffer_append_memory (buf, msgmem);
  msgmem = NULL;

  return GST_FLOW_OK;
append_fd_failed:
  GST_WARNING_OBJECT (trans, "Appending fd failed: %s", err->message);
  g_clear_error (&err);
  g_clear_object (&fdmsg);
  return GST_FLOW_ERROR;
}
