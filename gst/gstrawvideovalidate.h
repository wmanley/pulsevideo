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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_RAW_VIDEO_VALIDATE_H_
#define _GST_RAW_VIDEO_VALIDATE_H_

#include <gst/video/video.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RAW_VIDEO_VALIDATE   (gst_raw_video_validate_get_type())
#define GST_RAW_VIDEO_VALIDATE(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RAW_VIDEO_VALIDATE,GstRawVideoValidate))
#define GST_RAW_VIDEO_VALIDATE_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RAW_VIDEO_VALIDATE,GstRawVideoValidateClass))
#define GST_IS_RAW_VIDEO_VALIDATE(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RAW_VIDEO_VALIDATE))
#define GST_IS_RAW_VIDEO_VALIDATE_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RAW_VIDEO_VALIDATE))

typedef struct _GstRawVideoValidate GstRawVideoValidate;
typedef struct _GstRawVideoValidateClass GstRawVideoValidateClass;

struct _GstRawVideoValidate
{
  GstBaseTransform base_rawvideovalidate;

  GstCaps * caps;
  GstVideoInfo video_info;
};

struct _GstRawVideoValidateClass
{
  GstBaseTransformClass base_rawvideovalidate_class;
};

GType gst_raw_video_validate_get_type (void);

G_END_DECLS

#endif
