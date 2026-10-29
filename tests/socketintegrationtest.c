/* GStreamer
 *
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

#define _GNU_SOURCE /* for memfd_create */
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <gio/gio.h>
#include <gst/check/gstcheck.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gio/gunixfdmessage.h>
#include "../build/gstnetcontrolmessagemeta.h"
#include "../build/tmpfile/wire-protocol.h"

#include "sys/types.h"
#include "sys/stat.h"
#include "fcntl.h"
#include <sys/mman.h>

#define GST_UNREF(x) \
  do { \
    if ( x ) \
      gst_object_unref ( x ); \
    x = NULL; \
  } while (0);

static gboolean
g_socketpair (GSocketFamily family, GSocketType type, GSocketProtocol protocol,
    GSocket * gsv[2], GError ** error);

typedef struct
{
  GstElement *sink;
  GstElement *src;

  GstPipeline *sink_pipeline;
  GstPipeline *src_pipeline;
  GstAppSrc *sink_src;
  GstAppSink *src_sink;
} SymmetryTest;

static void
symmetry_test_setup (SymmetryTest * st, GstElement * sink, GstElement * src)
{
  GstCaps *caps;
  st->sink = sink;
  st->src = src;

  st->sink_pipeline = GST_PIPELINE (gst_pipeline_new (NULL));
  st->src_pipeline = GST_PIPELINE (gst_pipeline_new (NULL));

  st->sink_src = GST_APP_SRC (gst_element_factory_make ("appsrc", NULL));
  fail_unless (st->sink_src != NULL);

  g_object_set (st->sink_src, "format", GST_FORMAT_TIME, NULL);

  caps = gst_caps_from_string ("application/x-gst-check");
  gst_app_src_set_caps (st->sink_src, caps);
  gst_caps_unref (caps);

  gst_bin_add_many (GST_BIN (st->sink_pipeline), GST_ELEMENT (st->sink_src),
      st->sink, NULL);
  fail_unless (gst_element_link_many (GST_ELEMENT (st->sink_src), st->sink,
          NULL));

  st->src_sink = GST_APP_SINK (gst_element_factory_make ("appsink", NULL));
  fail_unless (st->src_sink != NULL);
  g_object_set (st->src_sink, "enable-last-sample", FALSE, NULL);

  gst_bin_add_many (GST_BIN (st->src_pipeline), st->src,
      GST_ELEMENT (st->src_sink), NULL);
  fail_unless (gst_element_link_many (st->src, GST_ELEMENT (st->src_sink),
          NULL));

  gst_element_set_state (GST_ELEMENT (st->sink_pipeline), GST_STATE_PLAYING);
  gst_element_set_state (GST_ELEMENT (st->src_pipeline), GST_STATE_PLAYING);
}

static void
symmetry_test_teardown (SymmetryTest * st)
{
  fail_unless (gst_element_set_state (GST_ELEMENT (st->sink_pipeline),
          GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);
  fail_unless (gst_element_set_state (GST_ELEMENT (st->src_pipeline),
          GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);

  gst_object_unref (st->sink_pipeline);
  gst_object_unref (st->src_pipeline);

  memset (st, 0, sizeof (*st));
}

static void
symmetry_test_assert_passthrough (SymmetryTest * st, GstBuffer * in)
{
  gpointer copy;
  gsize data_size;
  GstSample *out;

  gst_buffer_extract_dup (in, 0, -1, &copy, &data_size);

  fail_unless (gst_app_src_push_buffer (st->sink_src, in) == GST_FLOW_OK);
  in = NULL;
  out = gst_app_sink_pull_sample (st->src_sink);
  fail_unless (out != NULL);

  fail_unless (gst_buffer_get_size (gst_sample_get_buffer (out)) == data_size);
  fail_unless (gst_buffer_memcmp (gst_sample_get_buffer (out), 0, copy,
          data_size) == 0);
  g_free (copy);
  gst_sample_unref (out);
}

static gboolean
g_socketpair (GSocketFamily family, GSocketType type, GSocketProtocol protocol,
    GSocket * gsv[2], GError ** error)
{
  int ret;
  int sv[2];

  ret = socketpair (family, type, protocol, sv);
  if (ret != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "socketpair failed: %s",
        g_strerror (errno));
    return FALSE;
  }

  gsv[0] = g_socket_new_from_fd (sv[0], error);
  if (gsv[0] == NULL) {
    close (sv[0]);
    close (sv[1]);
    return FALSE;
  }
  gsv[1] = g_socket_new_from_fd (sv[1], error);
  if (gsv[1] == NULL) {
    g_clear_object (&gsv[0]);
    gsv[0] = NULL;
    close (sv[1]);
    return FALSE;
  }
  return TRUE;
}

static void
setup_multisocketsink_and_socketsrc (SymmetryTest * st)
{
  GSocket *sockets[2] = { NULL, NULL };
  GError *err = NULL;
  GstElement *socketsink = NULL, *socketsrc = NULL;

  socketsink = gst_check_setup_element ("pvmultisocketsink");
  g_object_set (socketsink, "enable-last-sample", FALSE, NULL);
  socketsrc = gst_check_setup_element ("pvsocketsrc");

  fail_unless (g_socketpair (G_SOCKET_FAMILY_UNIX,
          G_SOCKET_TYPE_STREAM | SOCK_CLOEXEC, G_SOCKET_PROTOCOL_DEFAULT,
          sockets, &err));

  g_object_set (socketsrc, "socket", sockets[0], NULL);
  g_clear_object (&sockets[0]);
  sockets[0] = NULL;

  symmetry_test_setup (st, socketsink, socketsrc);

  g_signal_emit_by_name (socketsink, "add", sockets[1], NULL);
  g_clear_object (&sockets[1]);
  sockets[1] = NULL;
}

GST_START_TEST (test_that_socketsrc_and_multisocketsink_are_symmetrical)
{
  SymmetryTest st = { 0 };
  setup_multisocketsink_and_socketsrc (&st);
  symmetry_test_assert_passthrough (&st,
      gst_buffer_new_wrapped (g_strdup ("hello"), 5));
  symmetry_test_teardown (&st);
}

GST_END_TEST;

GST_START_TEST (test_that_multisocketsink_and_socketsrc_preserve_meta)
{
  GstBuffer *buf;
  GSocketControlMessage *msg, *outmsg;
  SymmetryTest st = { 0 };
  int devnull = -1;
  gint *fds, fds_len = 0;
  struct stat stat_orig, stat_new;
  GstSample *sample;

  setup_multisocketsink_and_socketsrc (&st);

  devnull = open ("/dev/null", O_RDONLY);

  msg = g_unix_fd_message_new ();
  g_unix_fd_message_append_fd((GUnixFDMessage*) msg, devnull, NULL);

  buf = gst_buffer_new_wrapped (g_strdup ("hello"), 5);
  gst_buffer_add_net_control_message_meta (buf, msg);
  g_clear_object (&msg);

  fail_unless (gst_app_src_push_buffer (st.sink_src, buf) == GST_FLOW_OK);

  sample = gst_app_sink_pull_sample (st.src_sink);
  buf = gst_sample_get_buffer(sample);
  fail_unless (buf != NULL);

  outmsg = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE))->message;
  fail_unless (outmsg != NULL);
  fail_unless (g_socket_control_message_get_msg_type (outmsg) == SCM_RIGHTS);

  fds = g_unix_fd_message_steal_fds ((GUnixFDMessage*) outmsg, &fds_len);
  fail_unless (fds_len == 1);
  gst_sample_unref (sample);
  sample = NULL;
  buf = NULL;
  outmsg = NULL;

  fail_unless (fstat (devnull, &stat_orig) == 0);
  fail_unless (fstat (fds[0], &stat_new) == 0);
  fail_unless (stat_orig.st_dev == stat_new.st_dev);
  fail_unless (stat_orig.st_ino == stat_new.st_ino);

  close (fds[0]);
  g_free (fds);
  close (devnull);

  symmetry_test_teardown (&st);
}

GST_END_TEST;

static void
setup_zerocopy_symmetry_test (SymmetryTest * st)
{
  GSocket *sockets[2] = { NULL, NULL };
  GError *err = NULL;
  GstElement *zerocopysink, *zerocopysrc, *socketsrc, *socketsink;

  zerocopysink = gst_parse_bin_from_description (
      "pvfdpay ! pvmultisocketsink name=socketsink", TRUE, NULL);
  zerocopysrc = gst_parse_bin_from_description (
      "pvsocketsrc name=socketsrc do-timestamp=true ! pvfddepay", TRUE, NULL);

  fail_unless (zerocopysink != NULL);
  fail_unless (zerocopysrc != NULL);
  fail_unless (g_socketpair (G_SOCKET_FAMILY_UNIX,
          G_SOCKET_TYPE_STREAM | SOCK_CLOEXEC, G_SOCKET_PROTOCOL_DEFAULT,
          sockets, &err));

  socketsrc = gst_bin_get_by_name (GST_BIN (zerocopysrc), "socketsrc");
  g_object_set (socketsrc, "socket", sockets[0], NULL);
  GST_UNREF (socketsrc);
  g_clear_object (&sockets[0]);

  symmetry_test_setup (st, zerocopysink, zerocopysrc);

  socketsink = gst_bin_get_by_name (GST_BIN (zerocopysink), "socketsink");
  g_signal_emit_by_name (socketsink, "add", sockets[1], NULL);
  GST_UNREF (socketsink);
  g_clear_object (&sockets[1]);
}

GST_START_TEST (test_that_fdpay_and_fddepay_are_symmetrical)
{
  SymmetryTest st = { 0 };
  setup_zerocopy_symmetry_test (&st);
  symmetry_test_assert_passthrough (&st,
      gst_buffer_new_wrapped (g_strdup ("hello"), 5));
  symmetry_test_teardown (&st);

}

GST_END_TEST


static gsize
count_fds(void)
{
  gsize count = 0;
  GDir *dir = g_dir_open ("/proc/self/fd", 0, NULL);
  fail_unless (dir != NULL);
  while (g_dir_read_name(dir))
    count++;
  g_dir_close (dir);
  return count;
}

GST_START_TEST (test_that_zerocopy_doesnt_leak_fds)
{
  gint i;
  gsize fd_count;

  SymmetryTest st = { 0 };
  setup_zerocopy_symmetry_test (&st);

  /* We're only interested if the number of fds used increases with time once
     the pipeline is properly going.  Push a couple of buffers through to
     allocate all those fds that will be reused for the lifetime of the
     pipeline.  I have to run this twice becuase the first buffer and the last
     two buffers are remembered.  I'm not quite sure why the first one is
     remembered */
  for (i = 0; i < 3; i++) {
    symmetry_test_assert_passthrough (&st,
        gst_buffer_new_wrapped (g_strdup ("hello"), 5));
  }

  fd_count = count_fds();
  for (i = 0; i < 1000; i++) {
    symmetry_test_assert_passthrough (&st,
        gst_buffer_new_wrapped (g_strdup ("hello"), 5));
  }
  fail_unless_equals_int (fd_count, count_fds());

  symmetry_test_teardown (&st);
}

GST_END_TEST

static void
on_socket_eos (GstElement *socketsrc, GCancellable *cancellable, gpointer user_data)
{
  GSocket * socket = (GSocket*) user_data;

  g_object_set (socketsrc, "socket", socket, NULL);
}

GST_START_TEST (test_that_we_can_provide_new_socketsrc_sockets_during_signal)
{
  GSocket *sockets[4] = { NULL, NULL };

  GstPipeline * pipeline = NULL;
  GstAppSink * appsink = NULL;
  GstElement * socketsrc = NULL;
  GstSample * sample = NULL;

  socketsrc = gst_check_setup_element ("pvsocketsrc");

  fail_unless (g_socketpair (G_SOCKET_FAMILY_UNIX,
          G_SOCKET_TYPE_STREAM | SOCK_CLOEXEC, G_SOCKET_PROTOCOL_DEFAULT,
          &sockets[0], NULL));

  fail_unless (g_socket_send (sockets[0], "hello", 5, NULL, NULL) == 5);
  fail_unless (g_socket_shutdown (sockets[0], FALSE, TRUE, NULL));

  fail_unless (g_socketpair (G_SOCKET_FAMILY_UNIX,
          G_SOCKET_TYPE_STREAM | SOCK_CLOEXEC, G_SOCKET_PROTOCOL_DEFAULT,
          &sockets[2], NULL));
  fail_unless (g_socket_send (sockets[2], "goodbye", 7, NULL, NULL) == 7);
  fail_unless (g_socket_shutdown (sockets[2], FALSE, TRUE, NULL));

  g_object_set (socketsrc, "socket", sockets[1], NULL);

  g_signal_connect (socketsrc, "on-socket-eos", G_CALLBACK (on_socket_eos),
      sockets[3]);

  pipeline = (GstPipeline*) gst_pipeline_new (NULL);
  appsink = GST_APP_SINK (gst_check_setup_element ("appsink"));
  gst_bin_add_many (GST_BIN (pipeline), socketsrc, GST_ELEMENT (appsink), NULL);
  fail_unless (gst_element_link_many (socketsrc, GST_ELEMENT (appsink), NULL));

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);

  fail_unless ((sample = gst_app_sink_pull_sample (appsink)) != NULL);
  gst_buffer_memcmp (gst_sample_get_buffer (sample), 0, "hello", 5);
  gst_sample_unref (sample);

  fail_unless ((sample = gst_app_sink_pull_sample (appsink)) != NULL);
  gst_buffer_memcmp (gst_sample_get_buffer (sample), 0, "goodbye", 7);
  gst_sample_unref (sample);

  fail_unless (NULL == gst_app_sink_pull_sample (appsink));
  fail_unless (gst_app_sink_is_eos (appsink));

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  g_clear_object (&sockets[0]);
  g_clear_object (&sockets[1]);
  g_clear_object (&sockets[2]);
  g_clear_object (&sockets[3]);
  gst_object_unref (pipeline);
}

GST_END_TEST


static GstSample*
send_buffer_through_pipeline (GstPipeline * pipeline, GstBuffer * buffer)
{
  GstAppSink * appsink = NULL;
  GstAppSrc * appsrc = NULL;
  GstSample * sample = NULL;

  appsrc = GST_APP_SRC (gst_bin_get_by_name (GST_BIN (pipeline), "src"));
  fail_unless (appsrc != NULL);
  appsink = GST_APP_SINK (gst_bin_get_by_name (GST_BIN (pipeline), "sink"));
  fail_unless (appsink != NULL);

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);

  gst_app_src_push_buffer (appsrc, buffer);
  gst_app_src_end_of_stream (appsrc);

  fail_unless ((sample = gst_app_sink_pull_sample (appsink)) != NULL);

  fail_unless (gst_app_sink_pull_sample (appsink) == NULL);
  fail_unless (gst_app_sink_is_eos (appsink));

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  g_clear_object (&appsrc);
  g_clear_object (&appsink);

  return sample;
}

static void
check_monotonic_timestamp_with_clock(GstClock * clock)
{
  GstPipeline * pipeline = NULL;
  GstSample * sample = NULL;
  PvMessage header;
  gchar payload[5];
  guint64 before, after;
  struct timespec ts;

  /* Test setup: send a simple buffer through fdpay */
  pipeline = GST_PIPELINE (gst_parse_launch (
      "appsrc name=src format=GST_FORMAT_TIME is-live=true caps=application/x-gst-check "
      "! pvfdpay "
      "! appsink name=sink sync=false async=true", NULL));

  if (clock)
    gst_pipeline_use_clock (pipeline, clock);

  clock_gettime (CLOCK_MONOTONIC, &ts);
  before = ts.tv_sec * 1000000000 + ts.tv_nsec;
  sample = send_buffer_through_pipeline (pipeline, gst_buffer_new_wrapped (
      g_strdup ("hello"), 5));
  clock_gettime (CLOCK_MONOTONIC, &ts);
  after = ts.tv_sec * 1000000000 + ts.tv_nsec;

  g_clear_object (&pipeline);

  /* Now we check the data.  "hello" is small so it's sent inline: */
  fail_unless_equals_uint64 (
      gst_buffer_get_size (gst_sample_get_buffer (sample)),
      sizeof (header) + 5);
  gst_buffer_extract (gst_sample_get_buffer (sample), 0, &header,
      sizeof (header));
  gst_buffer_extract (gst_sample_get_buffer (sample), sizeof (header),
      payload, 5);
  gst_sample_unref (sample);
  sample = NULL;

  fail_unless_equals_int (header.type, PV_MSG_INLINE_BUFFER);
  fail_unless (header.capture_timestamp >= before);
  fail_unless (header.capture_timestamp <= after);
  fail_unless_equals_uint64 (header.size, 5);
  fail_unless (memcmp (payload, "hello", 5) == 0);
}

GST_START_TEST (test_that_fdpay_attaches_a_monotonic_timestamp)
{
  GstClock * clock = NULL;

  check_monotonic_timestamp_with_clock (NULL);

  clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
      "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
  check_monotonic_timestamp_with_clock (clock);
  g_clear_object (&clock);
}

GST_END_TEST

/* With a CLOCK_MONOTONIC pipeline clock the conversion from PTS to
 * CLOCK_MONOTONIC should be exact, honouring the clock's calibration.  Going
 * via a slaved clock would only approximate it.  The calibration here runs
 * at half speed so that the conversion isn't simply the identity. */
GST_START_TEST (test_that_fdpay_converts_exactly_with_a_monotonic_clock)
{
  GstClock *clock;
  GstPipeline *pipeline;
  GstSample *sample;
  GstBuffer *buf;
  PvMessage header;
  GstClockTime expected;

  clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
      "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);
  gst_clock_set_calibration (clock, gst_clock_get_internal_time (clock),
      1000 * GST_SECOND, 1, 2);

  pipeline = GST_PIPELINE (gst_parse_launch (
      "appsrc name=src format=GST_FORMAT_TIME is-live=true caps=application/x-gst-check "
      "! pvfdpay "
      "! appsink name=sink sync=false async=true", NULL));
  gst_pipeline_use_clock (pipeline, clock);

  buf = gst_buffer_new_wrapped (g_strdup ("hello"), 5);
  GST_BUFFER_PTS (buf) = GST_SECOND;
  sample = send_buffer_through_pipeline (pipeline, buf);

  GST_OBJECT_LOCK (clock);
  expected = gst_clock_unadjust_unlocked (clock,
      gst_element_get_base_time (GST_ELEMENT (pipeline)) + GST_SECOND);
  GST_OBJECT_UNLOCK (clock);

  gst_buffer_extract (gst_sample_get_buffer (sample), 0, &header,
      sizeof (header));
  fail_unless_equals_uint64 (header.capture_timestamp, expected);

  gst_sample_unref (sample);
  g_clear_object (&pipeline);
  g_clear_object (&clock);
}

GST_END_TEST

/* e.g. from v4l2src io-mode=dmabuf.  pulsevideo has its own copy of
 * GstFdMemory, so these aren't "fd memory" as far as it's concerned. */
GST_START_TEST (test_that_fdpay_sends_dmabufs_without_copying)
{
  GstPipeline *pipeline;
  GstAllocator *dmabuf_allocator;
  GstSample *sample;
  GstBuffer *buf;
  GSocketControlMessage *msg;
  PvMessage header;
  gint *fds, fds_len = 0;
  struct stat stat_orig, stat_new;
  gsize size = PV_MSG_MAX_INLINE * 2;
  int fd;

  fd = memfd_create ("test", MFD_CLOEXEC);
  fail_unless (fd >= 0);
  fail_unless (ftruncate (fd, size) == 0);
  fail_unless (fstat (fd, &stat_orig) == 0);

  dmabuf_allocator = gst_dmabuf_allocator_new ();
  buf = gst_buffer_new ();
  gst_buffer_append_memory (buf,
      gst_dmabuf_allocator_alloc (dmabuf_allocator, dup (fd), size));
  gst_object_unref (dmabuf_allocator);

  pipeline = GST_PIPELINE (gst_parse_launch (
      "appsrc name=src caps=application/x-gst-check "
      "! pvfdpay "
      "! appsink name=sink", NULL));
  sample = send_buffer_through_pipeline (pipeline, buf);
  g_clear_object (&pipeline);

  buf = gst_sample_get_buffer (sample);
  fail_unless_equals_uint64 (gst_buffer_get_size (buf), sizeof (header));
  gst_buffer_extract (buf, 0, &header, sizeof (header));
  fail_unless_equals_int (header.type, PV_MSG_FD_BUFFER);
  fail_unless_equals_uint64 (header.size, size);

  msg = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE))->message;
  fds = g_unix_fd_message_steal_fds ((GUnixFDMessage*) msg, &fds_len);
  fail_unless_equals_int (fds_len, 2);

  /* It's the file we sent rather than a copy */
  fail_unless (fstat (fds[0], &stat_new) == 0);
  fail_unless (stat_orig.st_dev == stat_new.st_dev);
  fail_unless (stat_orig.st_ino == stat_new.st_ino);

  close (fds[0]);
  close (fds[1]);
  g_free (fds);
  gst_sample_unref (sample);
  close (fd);
}

GST_END_TEST

/* fddepay tells fdpay that it's done with a buffer by closing a pipe.  It mustn't
 * do that while anything still refers to the memory: e.g. elements that make a
 * buffer writable get a shallow copy that shares the memory with the original,
 * and drop the original. */
GST_START_TEST (test_that_fddepay_holds_the_pipe_open_while_memory_is_in_use)
{
  GstPipeline *pipeline;
  GstSample *sample;
  GstBuffer *big, *copy;
  gsize fds_with_copy;

  pipeline = GST_PIPELINE (gst_parse_launch (
      "appsrc name=src caps=application/x-gst-check "
      "! pvfdpay "
      "! pvfddepay "
      "! appsink name=sink caps=application/x-gst-check", NULL));
  /* Too big to be sent inline */
  big = gst_buffer_new_allocate (NULL, PV_MSG_MAX_INLINE + 1, NULL);
  gst_buffer_memset (big, 0, 'x', PV_MSG_MAX_INLINE + 1);
  sample = send_buffer_through_pipeline (pipeline, big);

  copy = gst_buffer_copy (gst_sample_get_buffer (sample));
  gst_sample_unref (sample);
  /* appsink holds on to the sample until it's freed */
  g_clear_object (&pipeline);

  /* The original buffer is gone, but we're still using its memory, so the
   * memfd and the read end of the pipe should both still be open */
  fds_with_copy = count_fds ();
  gst_buffer_unref (copy);
  fail_unless_equals_int (count_fds (), fds_with_copy - 2);
}

GST_END_TEST

/* From gsttmpfileallocator.h, which we can't include alongside
 * <gst/allocators/gstdmabuf.h> */
GstAllocator *gst_tmpfile_allocator_new (void);

/* Receivers can hold on to buffers for as long as they like.  If upstream's
 * pool has a maximum size (like v4l2src's) fdpay must start copying before it
 * runs out, or upstream would stall. */
GST_START_TEST (test_that_fdpay_copies_rather_than_exhausting_upstreams_pool)
{
  GstPipeline *pipeline;
  GstAppSrc *appsrc;
  GstAppSink *appsink;
  GstBufferPool *pool;
  GstAllocator *allocator;
  GstStructure *config;
  GstCaps *caps;
  GstSample *held[16];
  GstBufferPoolAcquireParams params = {.flags =
        GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT };
  /* Too big to be sent inline */
  const gsize size = 320 * 320;
  int i;

  caps = gst_caps_from_string (
      "video/x-raw,format=GRAY8,width=320,height=320,framerate=1/1");
  pipeline = GST_PIPELINE (gst_parse_launch (
      "appsrc name=src format=time ! pvfdpay ! appsink name=sink sync=false",
      NULL));
  appsrc = GST_APP_SRC (gst_bin_get_by_name (GST_BIN (pipeline), "src"));
  appsink = GST_APP_SINK (gst_bin_get_by_name (GST_BIN (pipeline), "sink"));
  gst_app_src_set_caps (appsrc, caps);

  allocator = gst_tmpfile_allocator_new ();
  pool = gst_buffer_pool_new ();
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, 0, 8);
  gst_buffer_pool_config_set_allocator (config, allocator, NULL);
  fail_unless (gst_buffer_pool_set_config (pool, config));
  fail_unless (gst_buffer_pool_set_active (pool, TRUE));

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);

  /* Hold on to everything that we receive, so fdpay can't release any of
   * the buffers it sent without copying */
  for (i = 0; i < G_N_ELEMENTS (held); i++) {
    GstBuffer *buf = NULL;

    fail_unless_equals_int (
        gst_buffer_pool_acquire_buffer (pool, &buf, &params), GST_FLOW_OK);
    gst_buffer_memset (buf, 0, i, size);
    GST_BUFFER_PTS (buf) = i * GST_SECOND;
    fail_unless_equals_int (gst_app_src_push_buffer (appsrc, buf),
        GST_FLOW_OK);
    held[i] = gst_app_sink_pull_sample (appsink);
    fail_unless (held[i] != NULL);
  }

  for (i = 0; i < G_N_ELEMENTS (held); i++)
    gst_sample_unref (held[i]);
  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
  gst_object_unref (allocator);
  gst_caps_unref (caps);
  gst_object_unref (appsrc);
  gst_object_unref (appsink);
  gst_object_unref (pipeline);
}

GST_END_TEST

GST_START_TEST (test_that_buffers_from_fddepay_are_read_only)
{
  GstPipeline * pipeline;
  GstSample * sample;
  GstMemory * mem;
  GstMapInfo info;
  GstBuffer * big;

  pipeline = GST_PIPELINE (gst_parse_launch (
      "appsrc name=src caps=application/x-gst-check "
      "! pvfdpay "
      "! pvfddepay "
      "! appsink name=sink caps=application/x-gst-check", NULL));
  /* Too big to be sent inline, so it's shared memory that other clients may
   * also be reading: */
  big = gst_buffer_new_allocate (NULL, PV_MSG_MAX_INLINE + 1, NULL);
  gst_buffer_memset (big, 0, 'x', PV_MSG_MAX_INLINE + 1);
  sample = send_buffer_through_pipeline (pipeline, big);
  g_clear_object (&pipeline);

  mem = gst_buffer_peek_memory (gst_sample_get_buffer(sample), 0);

  fail_unless (GST_MEMORY_IS_READONLY (mem));
  fail_unless (!gst_memory_map (mem, &info, GST_MAP_READWRITE));

  /* Make sure this test is valid by trying to map at all: */
  fail_unless (gst_memory_map (mem, &info, GST_MAP_READ));
  gst_memory_unmap (mem, &info);

  gst_sample_unref (sample);
}

GST_END_TEST

static Suite *
socketintegrationtest_suite (void)
{
  Suite *s = suite_create ("socketintegrationtest");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain,
      test_that_socketsrc_and_multisocketsink_are_symmetrical);
  tcase_add_test (tc_chain,
      test_that_multisocketsink_and_socketsrc_preserve_meta);
  tcase_add_test (tc_chain,
      test_that_fdpay_and_fddepay_are_symmetrical);
  tcase_add_test (tc_chain,
      test_that_zerocopy_doesnt_leak_fds);
  tcase_add_test (tc_chain,
      test_that_we_can_provide_new_socketsrc_sockets_during_signal);
  tcase_add_test (tc_chain,
      test_that_fdpay_attaches_a_monotonic_timestamp);
  tcase_add_test (tc_chain,
      test_that_fdpay_converts_exactly_with_a_monotonic_clock);
  tcase_add_test (tc_chain,
      test_that_fdpay_sends_dmabufs_without_copying);
  tcase_add_test (tc_chain,
      test_that_buffers_from_fddepay_are_read_only);
  tcase_add_test (tc_chain,
      test_that_fdpay_copies_rather_than_exhausting_upstreams_pool);
  tcase_add_test (tc_chain,
      test_that_fddepay_holds_the_pipe_open_while_memory_is_in_use);

  return s;
}

GST_CHECK_MAIN (socketintegrationtest);
