/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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

#define _GNU_SOURCE

#include "gsttmpfileallocator.h"
#include <gst/allocators/gstfdmemory.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_ALIGN 4095

GST_DEBUG_CATEGORY_STATIC (gst_tmpfileallocator_debug);
#define GST_CAT_DEFAULT gst_tmpfileallocator_debug

#define GST_TYPE_TMPFILE_ALLOCATOR    (gst_tmpfile_allocator_get_type ())

typedef struct
{
  GstAllocator parent;
  GstAllocator *fd_allocator;
  uint32_t frame_count;
  uint32_t pid;
} GstTmpFileAllocator;

typedef struct
{
  GstAllocatorClass parent_class;
} GstTmpFileAllocatorClass;

GType gst_tmpfile_allocator_get_type (void);
G_DEFINE_TYPE (GstTmpFileAllocator, gst_tmpfile_allocator, GST_TYPE_ALLOCATOR);

static int
tmpfile_create (GstTmpFileAllocator * allocator)
{
  char filename[] = "/dev/shm/gsttmpfilepay.PPPPP.NNNNNNNNNN.XXXXXX";
  int fd;

  /* This should not be strictly necessary, but it can be useful to know more
     about where an fd came from when looking in /proc/<PID>/fd/ */
  snprintf(filename, sizeof(filename),
      "/dev/shm/gsttmpfilepay.%05d.%010d.XXXXXX",
      allocator->pid, allocator->frame_count++);

  fd = mkostemp (filename, O_CLOEXEC);
  if (fd == -1) {
    GST_WARNING_OBJECT (allocator, "Failed to create temporary file: %s",
        strerror (errno));
    return -1;
  }
  unlink (filename);

  return fd;
}

static void
gst_tmpfile_allocator_init (GstTmpFileAllocator * alloc)
{
  alloc->fd_allocator = gst_fd_allocator_new ();
  alloc->frame_count = 0;
  alloc->pid = getpid();
}

static void
gst_tmpfile_allocator_dispose (GObject * obj)
{
  GstTmpFileAllocator *alloc = (GstTmpFileAllocator *) obj;
  if (alloc->fd_allocator)
    g_object_unref (alloc->fd_allocator);
  alloc->fd_allocator = NULL;
}

GstAllocator *
gst_tmpfile_allocator_new (void)
{
  GObject *obj = g_object_new (GST_TYPE_TMPFILE_ALLOCATOR, NULL);
  return (GstAllocator *) obj;
}

GstMemory *
gst_tmpfile_allocator_copy_alloc (GstAllocator * allocator,
    const void * data, size_t n)
{
  GstTmpFileAllocator *alloc = (GstTmpFileAllocator *) allocator;

  GstMemory * mem = NULL;

  int fd = tmpfile_create (alloc);
  if (fd == -1)
    goto out;

  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, data + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      else
        goto out;
    } else {
      off += w;
    }
  }

  mem = gst_fd_allocator_alloc (alloc->fd_allocator, fd, n,
      GST_FD_MEMORY_FLAG_KEEP_MAPPED);
  fd = -1;
out:
  if (fd > 0)
    close (fd);
  return mem;
}

inline static gsize
pad (gsize off, gsize align)
{
  return (off + align) / (align + 1) * (align + 1);
}

static GstMemory *
gst_tmpfile_allocator_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  GstTmpFileAllocator *alloc = (GstTmpFileAllocator *) allocator;
  GstMemory *mem = NULL;
  int fd;
  gsize maxsize;

  g_return_val_if_fail (params != NULL, NULL);

  GST_DEBUG_OBJECT (allocator, "gst_tmpfile_allocator_alloc(%p, %"
      G_GSIZE_FORMAT ")", allocator, size);

  maxsize =
      pad (size + pad (params->prefix, params->align) + params->padding,
      PAGE_ALIGN);

  if (alloc->fd_allocator == NULL)
    return NULL;

  fd = tmpfile_create (alloc);
  if (fd < 0)
    return NULL;

  int result = fallocate (fd, 0, 0, maxsize);
  if (result == -1) {
    GST_WARNING_OBJECT (allocator, "Failed to resize temporary file: %s",
        strerror (errno));
    goto out;
  }

  mem = gst_fd_allocator_alloc (alloc->fd_allocator, fd, maxsize,
      GST_FD_MEMORY_FLAG_KEEP_MAPPED);
  fd = -1;
  gst_memory_resize (mem, pad (params->prefix, params->align), size);
out:
  if (fd != 0)
    close (fd);
  return mem;
}

static void
gst_tmpfile_allocator_class_init (GstTmpFileAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = (GstAllocatorClass *) klass;
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->dispose = gst_tmpfile_allocator_dispose;

  allocator_class->alloc = gst_tmpfile_allocator_alloc;

  GST_DEBUG_CATEGORY_INIT (gst_tmpfileallocator_debug, "tmpfileallocator", 0,
    "GstTmpFileAllocator");
}
