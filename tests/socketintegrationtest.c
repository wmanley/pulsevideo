/* GStreamer
 *
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

#include <unistd.h>
#include <sys/socket.h>

#include <gio/gio.h>
#include <gst/check/gstcheck.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gio/gunixfdmessage.h>
#include "../build/gstnetcontrolmessagemeta.h"

#include "sys/types.h"
#include "sys/stat.h"
#include "fcntl.h"

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
  caps = gst_caps_from_string ("application/x-gst-check");
  gst_app_src_set_caps (st->sink_src, caps);
  gst_caps_unref (caps);

  gst_bin_add_many (GST_BIN (st->sink_pipeline), GST_ELEMENT (st->sink_src),
      st->sink, NULL);
  fail_unless (gst_element_link_many (GST_ELEMENT (st->sink_src), st->sink,
          NULL));

  st->src_sink = GST_APP_SINK (gst_element_factory_make ("appsink", NULL));
  fail_unless (st->src_sink != NULL);
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
      "fdpay ! pvmultisocketsink name=socketsink", TRUE, NULL);
  zerocopysrc = gst_parse_bin_from_description (
      "pvsocketsrc name=socketsrc ! fddepay", TRUE, NULL);

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
  symmetry_test_assert_passthrough (&st,
      gst_buffer_new_wrapped (g_strdup ("hello"), 5));

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
on_socket_eos (GstElement *socketsrc, gpointer user_data)
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

  return s;
}

GST_CHECK_MAIN (socketintegrationtest);
