/* PulseVideo
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_TESSERACT_H_
#define _GST_TESSERACT_H_

#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>

#include <tesseract/baseapi.h>

G_BEGIN_DECLS

#define GST_TYPE_TESSERACT   (gst_tesseract_get_type())
#define GST_TESSERACT(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TESSERACT,GstTesseract))
#define GST_TESSERACT_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_TESSERACT,GstTesseractClass))
#define GST_IS_TESSERACT(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_TESSERACT))
#define GST_IS_TESSERACT_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_TESSERACT))

typedef struct _GstTesseract GstTesseract;
typedef struct _GstTesseractClass GstTesseractClass;

struct _GstTesseract
{
  GstVideoFilter base_tesseract;

  tesseract::TessBaseAPI *api;
};

struct _GstTesseractClass
{
  GstVideoFilterClass base_tesseract_class;
};

GType gst_tesseract_get_type (void);

G_END_DECLS

#endif
