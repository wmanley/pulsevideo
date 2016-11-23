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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsocketsrc.h"
#include "gstpulsevideosink.h"
#include "gstpulsevideosrc.h"
#include "gstrawvideovalidate.h"
#include "debugutils/gstwatchdog.h"
#include "tcp/gstmultisocketsink.h"
#include "tmpfile/gstfddepay.h"
#include "tmpfile/gstfdpay.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return
    gst_element_register (plugin, "pvsocketsrc", GST_RANK_NONE,
          GST_TYPE_SOCKET_SRC) &&
    gst_element_register (plugin, "pulsevideosink", GST_RANK_NONE,
          GST_TYPE_PULSEVIDEO_SINK) &&
    gst_element_register (plugin, "pulsevideosrc", GST_RANK_NONE,
          GST_TYPE_PULSEVIDEO_SRC) &&
    gst_element_register (plugin, "pvmultisocketsink", GST_RANK_NONE,
          GST_TYPE_MULTI_SOCKET_SINK) &&
    gst_element_register (plugin, "pvfdpay", GST_RANK_NONE,
          GST_TYPE_FDPAY) &&
    gst_element_register (plugin, "pvfddepay", GST_RANK_NONE,
          GST_TYPE_FDDEPAY) &&
    gst_element_register (plugin, "pvwatchdog", GST_RANK_NONE,
          GST_TYPE_WATCHDOG) &&
    gst_element_register (plugin, "rawvideovalidate", GST_RANK_NONE,
          GST_TYPE_RAW_VIDEO_VALIDATE);

}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    pulsevideo,
    "PulseVideo GStreamer elements",
    plugin_init, VERSION, "LGPL", "pulsevideo",
    "http://github.com/wmanley/pulsevideo")
