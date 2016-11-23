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


#ifndef __GST_PULSEVIDEO_SINK_H__
#define __GST_PULSEVIDEO_SINK_H__

#include <gst/gst.h>
#include <gst/gstbin.h>

#include <gio/gio.h>
#include "gstvideosource2.h"

G_BEGIN_DECLS

#define GST_TYPE_PULSEVIDEO_SINK \
  (gst_pulsevideo_sink_get_type())
#define GST_PULSEVIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PULSEVIDEO_SINK,GstPulseVideoSink))
#define GST_PULSEVIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PULSEVIDEO_SINK,GstPulseVideoSinkClass))
#define GST_IS_PULSEVIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PULSEVIDEO_SINK))
#define GST_IS_PULSEVIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PULSEVIDEO_SINK))

typedef struct _GstPulseVideoSink GstPulseVideoSink;
typedef struct _GstPulseVideoSinkClass GstPulseVideoSinkClass;

struct _GstPulseVideoSink {
  GstBin element;

  /*< private >*/
  GstElement *capsfilter;
  GstElement *fdpay;
  GstElement *socketsink;

  GDBusConnection *dbus;
  gchar *bus_name;
  gchar *object_path;
  gboolean wait_for_connection;

  GDBusConnection *connection_in_use;
  GstVideoSource2 *dbus_interface;
  gint bus_name_token;
};

struct _GstPulseVideoSinkClass {
  GstBinClass parent_class;
};

GType gst_pulsevideo_sink_get_type (void);

G_END_DECLS

#endif /* __GST_PULSEVIDEO_SINK_H__ */
