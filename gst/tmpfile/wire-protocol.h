/* GStreamer
 * Copyright (C) 2014-2016 William Manley <will@williammanley.net>
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

#ifndef _GST_FDPAY_WIRE_PROTOCOL_H_
#define _GST_FDPAY_WIRE_PROTOCOL_H_

#include <stdint.h>

/* A simple FD passing protocol.  Each message should have two file-descriptors
 * attached:
 *
 * 1. A memfd (or file) which should be mmapable.  The data in the FD can
 *    be found at offset and is size bytes long.
 * 2. The read end of a pipe which should be closed when the receiver is done
 *    with the data in the memfd.
 *
 * On the sender side there is a gstreamer element named fdpay.  It proposes a
 * bufferpool upstream which allocates memfd backing memory for upstream
 * elements.  When it receives a buffer from upstream it uses `pipe` to create a
 * new pair of file descriptors.  It adds the write end of the pipe (wfd) to an
 * epoll instance and sends the memfd and the read end of the pipe downstream.
 * It keeps a mapping of wfd to GstBuffer.  When the read end is closed by the
 * receiver, the sender receives an event on the epoll instance and will then
 * remove the entry in the map, allowing the buffer to be reused.
 *
 * On the reciever side there is an element called fddepay.  This element
 * receives the memfd and the read end of the pipe.  It creates a GstFdMemory
 * which maps the memfd and keeps a mapping of (st_dev, st_ino) to the new
 * GstMemory.  It creates downstream buffers with the pipe attached.  If it
 * receives a memfd with the same (st_dev, st_ino) as a memfd it has already
 * received, it reuses the GstMemory it created for the first memfd, avoiding
 * a costly remapping.
 */
typedef struct {
  /* Time at which the frame was originally captured against the CLOCK_MONOTONIC
   * in ns */
  uint64_t capture_timestamp;
  uint64_t offset;
  uint64_t size;
} FDMessage;

#endif
