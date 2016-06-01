/* GStreamer
 * Copyright (C) 2016 William Manley <will@williammanley.net>
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
 * SECTION:element-gsttesseract
 *
 * The tesseract element performs OCR on the incoming image buffers.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! textoverlay ! tesseract ! fakesink
 * ]|
 * Reads the text from textoverlay posting messages on the bus.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include "gsttesseract.h"
#include "gsttextmeta.h"

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

GST_DEBUG_CATEGORY_STATIC (gst_tesseract_debug_category);
#define GST_CAT_DEFAULT gst_tesseract_debug_category

/* prototypes */

static void gst_tesseract_dispose (GObject * object);
static void gst_tesseract_finalize (GObject * object);

static gboolean gst_tesseract_start (GstBaseTransform * trans);
static gboolean gst_tesseract_stop (GstBaseTransform * trans);
static gboolean gst_tesseract_set_info (GstVideoFilter * filter, GstCaps * incaps,
    GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info);
static GstFlowReturn gst_tesseract_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * inframe, GstVideoFrame * outframe);
static GstFlowReturn gst_tesseract_transform_frame_ip (GstVideoFilter * filter,
    GstVideoFrame * frame);

enum
{
  PROP_0
};

/* pad templates */

#define VIDEO_SRC_CAPS \
    GST_VIDEO_CAPS_MAKE("{ RGBA }")

#define VIDEO_SINK_CAPS \
    GST_VIDEO_CAPS_MAKE("{ RGBA }")


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstTesseract, gst_tesseract, GST_TYPE_VIDEO_FILTER,
  GST_DEBUG_CATEGORY_INIT (gst_tesseract_debug_category, "tesseract", 0,
  "debug category for tesseract element"));

static void
gst_tesseract_class_init (GstTesseractClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
        gst_caps_from_string (VIDEO_SRC_CAPS)));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
        gst_caps_from_string (VIDEO_SINK_CAPS)));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "tesseract", "Generic", "Read text on video frames with tesseract OCR "
      "engine", "William Manley <will@williammanley.net>");

  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_tesseract_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_tesseract_stop);
  video_filter_class->transform_frame_ip = GST_DEBUG_FUNCPTR (gst_tesseract_transform_frame_ip);

}

static void
gst_tesseract_init (GstTesseract *tesseract)
{
  tesseract->api = NULL;
}

static gboolean
gst_tesseract_start (GstBaseTransform * trans)
{
  GstTesseract *tesseract = GST_TESSERACT (trans);

  GST_DEBUG_OBJECT (tesseract, "start");

  tesseract->api = new tesseract::TessBaseAPI();

  if (tesseract->api->Init(NULL, "eng")) {
    GST_ERROR_OBJECT (tesseract, "Could not initialize tesseract.");
    goto error;
  }

  return TRUE;
error:
  delete tesseract->api;
  tesseract->api = NULL;
  return FALSE;
}

static gboolean
gst_tesseract_stop (GstBaseTransform * trans)
{
  GstTesseract *tesseract = GST_TESSERACT (trans);

  GST_DEBUG_OBJECT (tesseract, "stop");

  if (tesseract->api) {
    tesseract->api->End();

    delete tesseract->api;
    tesseract->api = NULL;
  }

  return TRUE;
}

static GstFlowReturn
gst_tesseract_transform_frame_ip (GstVideoFilter * filter, GstVideoFrame * frame)
{
  GstTesseract *tesseract = GST_TESSERACT (filter);
  GstVideoInfo info = frame->info;
  char * out_text = NULL;

  GST_DEBUG_OBJECT (tesseract, "transform_frame_ip");

  tesseract->api->SetImage((const unsigned char*) frame->data[0], info.width,
      info.height, info.size / info.width / info.height, info.stride[0]);
  out_text = tesseract->api->GetUTF8Text();
  gst_buffer_add_text_meta (frame->buffer, out_text);
  delete[] out_text;

  return GST_FLOW_OK;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "tesseract", GST_RANK_NONE,
      GST_TYPE_TESSERACT);
}

#ifndef VERSION
#define VERSION "0.0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "pulsevideo-tests"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "pulsevideo_tests"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://stb-tester.com/"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    tesseract,
    "Elements for testing PulseVideo",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)

