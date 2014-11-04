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


#ifndef __GST_DBUS_VIDEOSOURCE_SRC_H__
#define __GST_DBUS_VIDEOSOURCE_SRC_H__

#include <gst/gst.h>
#include "gstsocketsrc.h"

#include <gio/gio.h>

G_BEGIN_DECLS

#define GST_TYPE_DBUS_VIDEOSOURCE_SRC \
  (gst_dbus_videosource_src_get_type())
#define GST_DBUS_VIDEOSOURCE_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_DBUS_VIDEOSOURCE_SRC,GstDBusVideoSourceSrc))
#define GST_DBUS_VIDEOSOURCE_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_DBUS_VIDEOSOURCE_SRC,GstDBusVideoSourceSrcClass))
#define GST_IS_DBUS_VIDEOSOURCE_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_DBUS_VIDEOSOURCE_SRC))
#define GST_IS_DBUS_VIDEOSOURCE_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_DBUS_VIDEOSOURCE_SRC))

typedef struct _GstDBusVideoSourceSrc GstDBusVideoSourceSrc;
typedef struct _GstDBusVideoSourceSrcClass GstDBusVideoSourceSrcClass;

struct _GstDBusVideoSourceSrc {
  GstSocketSrc element;

  /*< private >*/
  GCancellable *cancellable;
  GDBusConnection *dbus;
  gchar *bus_name;
  gchar *object_path;

  GDBusProxy *videosource;
};

struct _GstDBusVideoSourceSrcClass {
  GstSocketSrcClass parent_class;
};

GType gst_dbus_videosource_src_get_type (void);

G_END_DECLS

#endif /* __GST_DBUS_VIDEOSOURCE_SRC_H__ */
