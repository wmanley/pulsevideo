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

/* The protocol between fdpay (in pulsevideo) and fddepay (in clients).  It's
 * carried over a SOCK_SEQPACKET socket, so each message arrives as a single
 * packet.  Every message starts with a PvMessage header, and its type says
 * what follows:
 *
 * PV_MSG_FD_BUFFER: Just the header, with two file-descriptors attached:
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
 * which maps the memfd and keeps a mapping of memory_id to the new GstMemory.
 * It creates downstream buffers with the pipe attached.  If it receives a
 * message with the same memory_id as one it has already received, it reuses
 * the GstMemory it created for the first memfd, avoiding a costly remapping.
 *
 * The fd may also be a dma-buf, e.g. a buffer that a V4L2 driver captured
 * into, exported with VIDIOC_EXPBUF.
 *
 * PV_MSG_INLINE_BUFFER: The header is followed by size bytes of data, and no
 * file-descriptors are attached.  For small buffers (up to
 * PV_MSG_MAX_INLINE bytes, e.g. audio or encoded video) copying the data
 * through the socket is cheaper than passing, mapping and tracking fds.
 */
typedef struct {
  /* PV_MSG_* */
  uint32_t type;
  /* PV_MSG_FLAG_* */
  uint32_t flags;
  /* Time at which the frame was originally captured against the CLOCK_MONOTONIC
   * in ns */
  uint64_t capture_timestamp;
  /* PV_MSG_FD_BUFFER only: offset of the data in the memfd */
  uint64_t offset;
  uint64_t size;
  /* PV_MSG_FD_BUFFER with PV_MSG_FLAG_MAY_REPEAT only: identifies the memory
   * the fd refers to.  A later message with the same memory_id refers to the
   * same memory, so the receiver can reuse its mapping rather than mmapping
   * the new fd.  The sender never reuses an id for different memory: they
   * start from a random number in each process.
   *
   * We can't use the fd's (st_dev, st_ino) for this because on older kernels
   * (including Linux 3.10 and 4.9) all dma-bufs share a single anonymous
   * inode. */
  uint64_t memory_id;
} PvMessage;

#define PV_MSG_FD_BUFFER 1
#define PV_MSG_INLINE_BUFFER 2

/* Buffers up to this size are sent inline */
#define PV_MSG_MAX_INLINE (64 * 1024)

/* PV_MSG_FD_BUFFER: The memfd may be sent again in a later message (it's from a
 * buffer pool), so it's worth the receiver keeping it mapped.  If not set the
 * memfd is a one-off. */
#define PV_MSG_FLAG_MAY_REPEAT 1

#endif
