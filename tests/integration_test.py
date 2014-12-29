import os
import pipes
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from contextlib import contextmanager

import dbus
import pytest


@pytest.yield_fixture(scope='function')
def tmpdir():
    dir_ = tempfile.mkdtemp(prefix='pulsevideo-tests-')
    try:
        yield dir_
    finally:
        shutil.rmtree(dir_, ignore_errors=True)


def pulsevideo_cmdline():
    return ['/usr/bin/env',
            'GST_PLUGIN_PATH=%s/../build' % os.path.dirname(__file__),
            'LD_LIBRARY_PATH=%s/../build/' % os.path.dirname(__file__),
            'G_DEBUG=fatal_warnings',
            '%s/../pulsevideo' % os.path.dirname(__file__),
            '--caps=video/x-raw,format=RGB,width=320,height=240,framerate=10/1',
            '--source-pipeline=videotestsrc is-live=true',
            '--bus-name-suffix=test']


@pytest.yield_fixture(scope='function')
def pulsevideo_via_activation(tmpdir):
    mkdir_p('%s/services' % tmpdir)

    with open('%s/services/com.stbtester.VideoSource.test.service' % tmpdir,
              'w') as out, \
            open('%s/com.stbtester.VideoSource.test.service.in'
                 % os.path.dirname(__file__)) as in_:
        out.write(
            in_.read()
            .replace('@PULSEVIDEO@',
                     ' '.join(pipes.quote(x) for x in pulsevideo_cmdline()))
            .replace('@TMPDIR@', tmpdir))

    with dbus_ctx(tmpdir):
        yield


@pytest.yield_fixture(scope='function')
def pulsevideo(tmpdir):
    with dbus_ctx(tmpdir) as dbus_daemon:
        pulsevideod = subprocess.Popen(pulsevideo_cmdline())
        sbus = dbus.SessionBus()
        bus = sbus.get_object('org.freedesktop.DBus', '/')
        assert dbus_daemon.poll() is None
        wait_until(
            lambda: 'com.stbtester.VideoSource.capture' in bus.ListNames())
        yield pulsevideod
        pulsevideod.kill()
        pulsevideod.wait()


@pytest.yield_fixture(scope='function')
def dbus_fixture(tmpdir):
    with dbus_ctx(tmpdir):
        yield


def mkdir_p(dirs):
    import errno
    try:
        os.makedirs(dirs)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise


@contextmanager
def dbus_ctx(tmpdir):
    socket_path = '%s/dbus_socket' % tmpdir
    mkdir_p('%s/services' % tmpdir)

    with open('%s/session.conf' % tmpdir, 'w') as out, \
            open('%s/session.conf.in' % os.path.dirname(__file__)) as in_:
        out.write(in_.read()
                  .replace('@DBUS_SOCKET@', socket_path)
                  .replace('@SERVICEDIR@', "%s/services" % tmpdir))

    os.environ['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=%s' % socket_path

    dbus_daemon = subprocess.Popen(
        ['dbus-daemon', '--config-file=%s/session.conf' % tmpdir, '--nofork'])
    for _ in range(100):
        if os.path.exists(socket_path):
            break
        assert dbus_daemon.poll() is None, "dbus-daemon failed to start up"
        time.sleep(0.1)
    else:
        assert False, "dbus-daemon didn't take socket-path"

    try:
        yield dbus_daemon
    finally:
        os.remove(socket_path)
        dbus_daemon.kill()
        dbus_daemon.wait()
        del os.environ['DBUS_SESSION_BUS_ADDRESS']


class FrameCounter(object):
    def __init__(self, file_, frame_size=320 * 240 * 3):
        self.file = file_
        self.count = 0
        self.frame_size = 320 * 240 * 3
        self.thread = threading.Thread(target=self._read_in_loop)
        self.thread.daemon = True

    def start(self):
        self.thread.start()

    def _read_in_loop(self):
        bytes_read = 0
        while True:
            new_bytes_read = len(self.file.read(self.frame_size))
            bytes_read += new_bytes_read

            # GIL makes this thread safe :)
            self.count = bytes_read // self.frame_size


def test_with_dbus(pulsevideo_via_activation):
    os.environ['GST_DEBUG'] = "3,*videosource*:9"
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', 'dbusvideosourcesrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
        stdout=subprocess.PIPE)
    fc = FrameCounter(gst_launch.stdout)
    fc.start()
    time.sleep(1)
    count = fc.count
    assert count >= 5 and count <= 15


def wait_until(f, timeout_secs=10):
    expiry_time = time.time() + timeout_secs
    while True:
        val = f()
        if val:
            return val  # truthy
        if time.time() > expiry_time:
            return val  # falsy


def test_that_dbusvideosourcesrc_recovers_if_pulsevideo_crashes(
        pulsevideo_via_activation):
    os.environ['GST_DEBUG'] = "3,*videosource*:9"
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', '-e', '-q', 'dbusvideosourcesrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
        stdout=subprocess.PIPE)
    fc = FrameCounter(gst_launch.stdout)
    fc.start()
    assert wait_until(lambda: fc.count > 1)
    obj = dbus.SessionBus().get_object('org.freedesktop.DBus', '/')
    pulsevideo_pid = obj.GetConnectionUnixProcessID(
        'com.stbtester.VideoSource.test')
    os.kill(pulsevideo_pid, 9)
    oldcount = fc.count
    assert wait_until(lambda: fc.count > oldcount + 20, 3)

    gst_launch.kill()
    gst_launch.wait()


def test_that_dbusvideosourcesrc_fails_if_pulsevideo_is_not_available(
        dbus_fixture):
    os.environ['GST_DEBUG'] = "3,*videosource*:9"
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', 'dbusvideosourcesrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'])
    assert wait_until(lambda: gst_launch.poll() is not None, 2)
    assert gst_launch.returncode != 0


def test_that_dbusvideosourcesrc_gets_eos_if_pulsevideo_crashes_and_cant_be_activated(
        pulsevideo):
    print time.time()
    os.environ['GST_DEBUG'] = "3,*videosource*:9"
    print time.time()
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', '-q', 'dbusvideosourcesrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
        stdout=subprocess.PIPE)
    fc = FrameCounter(gst_launch.stdout)
    fc.start()
    assert wait_until(lambda: fc.count > 1)
    pulsevideo.kill()
    assert wait_until(lambda: gst_launch.poll() is not None, 2)
    assert gst_launch.returncode == 0
