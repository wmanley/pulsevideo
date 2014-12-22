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
#include <gst/allocators/gstdmabuf.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_ALIGN 4095

#define GST_TYPE_TMPFILE_ALLOCATOR    (gst_tmpfile_allocator_get_type ())

typedef struct
{
  GstAllocator parent;
  GstAllocator *dmabuf_allocator;
} GstTmpFileAllocator;

typedef struct
{
  GstAllocatorClass parent_class;
} GstTmpFileAllocatorClass;

GType gst_tmpfile_allocator_get_type (void);
G_DEFINE_TYPE (GstTmpFileAllocator, gst_tmpfile_allocator, GST_TYPE_ALLOCATOR);

static int
tmpfile_create (GstTmpFileAllocator * allocator, gsize size)
{
  char filename[] = "/dev/shm/tmpfilepay.XXXXXX";
  int fd, result;

  GST_DEBUG_OBJECT (allocator, "tmpfile_create");

  fd = mkostemp (filename, O_CLOEXEC);
  if (fd == -1) {
    GST_WARNING_OBJECT (allocator, "Failed to create temporary file: %s",
        strerror (errno));
    return -1;
  }
  unlink (filename);

  result = ftruncate (fd, size);
  if (result == -1) {
    GST_WARNING_OBJECT (allocator, "Failed to resize temporary file: %s",
        strerror (errno));
    close (fd);
    return -1;
  }

  return fd;
}

static void
gst_tmpfile_allocator_init (GstTmpFileAllocator * alloc)
{
  alloc->dmabuf_allocator = gst_dmabuf_allocator_new ();
}

static void
gst_tmpfile_allocator_dispose (GObject * obj)
{
  GstTmpFileAllocator *alloc = (GstTmpFileAllocator *) obj;
  if (alloc->dmabuf_allocator)
    g_object_unref (alloc->dmabuf_allocator);
  alloc->dmabuf_allocator = NULL;
}

GstAllocator *
gst_tmpfile_allocator_new (void)
{
  GObject *obj = g_object_new (GST_TYPE_TMPFILE_ALLOCATOR, NULL);
  return (GstAllocator *) obj;
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
  GstMemory *mem;
  int fd;
  gsize maxsize =
      pad (size + pad (params->prefix, params->align) + params->padding,
      PAGE_ALIGN);

  if (alloc->dmabuf_allocator == NULL)
    return NULL;

  fd = tmpfile_create (alloc, maxsize);
  if (fd < 0)
    return NULL;
  mem = gst_dmabuf_allocator_alloc (alloc->dmabuf_allocator, fd, maxsize);
  gst_memory_resize (mem, pad (params->prefix, params->align), size);
  return mem;
}

static void
gst_tmpfile_allocator_class_init (GstTmpFileAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = (GstAllocatorClass *) klass;
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->dispose = gst_tmpfile_allocator_dispose;

  allocator_class->alloc = gst_tmpfile_allocator_alloc;
}
