/* GStreamer
 * Copyright (C) 2015 William Manley <will@williammanley.net>
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
 * SECTION:element-gstrawvideovalidate
 *
 * The rawvideovalidate element validates the size of raw video buffers,
 * dropping them if they are too small.  This works around issues in v4l2src
 * which can produce buffers that are too small that in-turn will confuse
 * downstream elements.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v v4l2src ! rawvideovalidate ! videoconvert ! xvimagesink
 * ]|
 * Displays video from a v4l2 device in a way that won't crash if v4l2src
 * produces badly sized buffers.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include "gstrawvideovalidate.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_raw_video_validate_debug_category);
#define GST_CAT_DEFAULT gst_raw_video_validate_debug_category

/* prototypes */


static void gst_raw_video_validate_dispose (GObject * object);

static gboolean gst_raw_video_validate_start (GstBaseTransform * trans);
static GstFlowReturn gst_raw_video_validate_transform_ip (
    GstBaseTransform * trans, GstBuffer * buf);
static gboolean gst_raw_video_validate_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstRawVideoValidate, gst_raw_video_validate, GST_TYPE_BASE_TRANSFORM,
  GST_DEBUG_CATEGORY_INIT (gst_raw_video_validate_debug_category, "rawvideovalidate", 0,
  "debug category for rawvideovalidate element"));

static void
gst_raw_video_validate_class_init (GstRawVideoValidateClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
        gst_caps_new_any ()));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
        gst_caps_new_any ()));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Raw Video Validate", "Generic",
      "validates the size of raw video buffers dropping them if they are too "
      "small for the given caps", "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_raw_video_validate_dispose;
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_raw_video_validate_start);
  base_transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_raw_video_validate_set_caps);
  base_transform_class->transform_ip = GST_DEBUG_FUNCPTR (
      gst_raw_video_validate_transform_ip);

}

static void
gst_raw_video_validate_init (GstRawVideoValidate *rawvideovalidate)
{
  rawvideovalidate->caps = NULL;
}

void
gst_raw_video_validate_dispose (GObject * object)
{
  GstRawVideoValidate *rawvideovalidate = GST_RAW_VIDEO_VALIDATE (object);

  GST_DEBUG_OBJECT (rawvideovalidate, "dispose");

  /* clean up as possible.  may be called multiple times */
  if (rawvideovalidate->caps)
    gst_caps_unref (rawvideovalidate->caps);
  rawvideovalidate->caps = NULL;

  G_OBJECT_CLASS (gst_raw_video_validate_parent_class)->dispose (object);
}

static gboolean
gst_raw_video_validate_start (GstBaseTransform * trans)
{
  gst_base_transform_set_in_place (trans, TRUE);
  return TRUE;
}

static gboolean
gst_raw_video_validate_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstRawVideoValidate * rawvideovalidate = GST_RAW_VIDEO_VALIDATE (trans);

  gst_caps_replace (&rawvideovalidate->caps, incaps);
  if (!gst_video_info_from_caps (&rawvideovalidate->video_info, incaps))
    memset (&rawvideovalidate->video_info, 0,
        sizeof (rawvideovalidate->video_info));
  if (rawvideovalidate->video_info.size == 0)
    GST_DEBUG_OBJECT (rawvideovalidate, "Output caps %" GST_PTR_FORMAT " has "
        "unknown size", incaps);

  GST_LOG_OBJECT (rawvideovalidate, "InvalidBufferDropper: Seen caps \"%"
      GST_PTR_FORMAT "\" Expecting buffer size %zu", incaps,
      rawvideovalidate->video_info.size);

  return TRUE;
}

static GstFlowReturn
gst_raw_video_validate_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRawVideoValidate *rawvideovalidate = GST_RAW_VIDEO_VALIDATE (trans);

  gsize buffer_size = gst_buffer_get_size(buf);

  if (G_UNLIKELY (rawvideovalidate->video_info.size
      && buffer_size != rawvideovalidate->video_info.size))
  {
    /* Sometimes I've seen v4l2src produce buffers that are smaller than you
       would expect based on the caps.  I don't think it's technically an
       error, but it can certainly surprise downstream elements. */
    GST_WARNING_OBJECT (rawvideovalidate, "Received buffer isn't "
        "the right size we'd expect based caps \"%" GST_PTR_FORMAT "\" "
        "(%zu != %zu).  Dropping this buffer", rawvideovalidate->caps,
        buffer_size, rawvideovalidate->video_info.size);
    return GST_BASE_TRANSFORM_FLOW_DROPPED;
  } else {
    return GST_FLOW_OK;
  }
}
