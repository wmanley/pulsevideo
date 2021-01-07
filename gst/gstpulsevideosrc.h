/* GStreamer
 * Copyright (C) <2014> William Manley <will@williammanley.net>
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


#ifndef __GST_PULSEVIDEO_SRC_H__
#define __GST_PULSEVIDEO_SRC_H__

#include <gst/gst.h>
#include <gst/gstbin.h>

#include <gio/gio.h>

G_BEGIN_DECLS

#define GST_TYPE_PULSEVIDEO_SRC \
  (gst_pulsevideo_src_get_type())
#define GST_PULSEVIDEO_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PULSEVIDEO_SRC,GstPulseVideoSrc))
#define GST_PULSEVIDEO_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PULSEVIDEO_SRC,GstPulseVideoSrcClass))
#define GST_IS_PULSEVIDEO_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PULSEVIDEO_SRC))
#define GST_IS_PULSEVIDEO_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PULSEVIDEO_SRC))

typedef struct _GstPulseVideoSrc GstPulseVideoSrc;
typedef struct _GstPulseVideoSrcClass GstPulseVideoSrcClass;

struct _GstPulseVideoSrc {
  GstBin element;

  /*< private >*/
  GstElement *socketsrc;
  GstElement *fddepay;
  GstElement *capsfilter;
  GDBusConnection *dbus;
  gchar *bus_name;
  gchar *object_path;
};

struct _GstPulseVideoSrcClass {
  GstBinClass parent_class;
};

GType gst_pulsevideo_src_get_type (void);

G_END_DECLS

#endif /* __GST_PULSEVIDEO_SRC_H__ */
