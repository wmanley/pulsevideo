/* GStreamer
 * Copyright (C) <2016> William Manley <will@williammanley.net>
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

#ifndef __GST_TEXT_META_H__
#define __GST_TEXT_META_H__

#include <gst/gst.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GstTextMeta GstTextMeta;

/**
 * GstTextMeta:
 * @meta: the parent type
 * @text: the text read from the frame stored as metadata
 *
 * Buffer metadata for OCR elements.
 */
struct _GstTextMeta {
  GstMeta       meta;

  gchar *text;
};

GType gst_text_meta_api_get_type (void);
#define GST_TEXT_META_API_TYPE \
  (gst_text_meta_api_get_type())

#define gst_buffer_get_text_meta(b) ((GstTextMeta*)\
  gst_buffer_get_meta((b),GST_TEXT_META_API_TYPE))

/* implementation */
const GstMetaInfo *gst_text_meta_get_info (void);
#define GST_TEXT_META_INFO (gst_text_meta_get_info())

GstTextMeta * gst_buffer_add_text_meta (GstBuffer   * buffer,
                                        const gchar * text);
gchar * gst_buffer_dup_text_from_text_meta (GstBuffer * buffer);

G_END_DECLS

#endif /* __GST_TEXT_META_H__ */

