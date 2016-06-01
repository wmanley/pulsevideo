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

/**
 * SECTION:gsttextmeta
 * @short_description: OCRed Text Meta
 *
 * #GstTextMeta can be used to attach the text read from a frame using OCR to a
 * buffer.  This can then be retrieved downstream.
 */

#include <string.h>

#include "gsttextmeta.h"

static gboolean
gst_text_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstTextMeta *nmeta = (GstTextMeta *) meta;

  nmeta->text = NULL;

  return TRUE;
}

static gboolean
gst_text_meta_transform (GstBuffer * transbuf, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstTextMeta *smeta, *dmeta;
  smeta = (GstTextMeta *) meta;

  /* we always copy no matter what transform */
  dmeta = gst_buffer_add_text_meta (transbuf, smeta->text);
  if (!dmeta)
    return FALSE;

  return TRUE;
}

static void
gst_text_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstTextMeta *nmeta = (GstTextMeta *) meta;

  if (nmeta->text)
    g_free (nmeta->text);
  nmeta->text = NULL;
}

GType
gst_text_meta_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] = { "origin", NULL };

  if (g_once_init_enter (&type)) {
    GType _type =
        gst_meta_api_type_register ("GstTextMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

const GstMetaInfo *
gst_text_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter (&meta_info)) {
    const GstMetaInfo *mi =
        gst_meta_register (GST_TEXT_META_API_TYPE,
        "GstTextMeta",
        sizeof (GstTextMeta),
        gst_text_meta_init,
        gst_text_meta_free,
        gst_text_meta_transform);
    g_once_init_leave (&meta_info, mi);
  }
  return meta_info;
}

/**
 * gst_buffer_add_text_meta:
 * @buffer: a #GstBuffer
 * @text: a string to attach to @buffer
 *
 * Attaches @text as metadata in a #GstTextMeta to @buffer.
 *
 * Returns: (transfer none): a #GstTextMeta connected to @buffer
 */
GstTextMeta *
gst_buffer_add_text_meta (GstBuffer * buffer, const gchar * text)
{
  GstTextMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (text, NULL);

  meta = (GstTextMeta *) gst_buffer_add_meta (buffer, GST_TEXT_META_INFO, NULL);

  meta->text = g_strdup (text);

  return meta;
}

gchar *
gst_buffer_dup_text_from_text_meta (GstBuffer * buffer)
{
  GstTextMeta * meta = gst_buffer_get_text_meta (buffer);
  if (meta)
    return g_strdup (meta->text);
  else
    return NULL;
}
