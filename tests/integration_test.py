import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time

import pytest


@pytest.yield_fixture
def tmpdir():
    dir_ = tempfile.mkdtemp(prefix='pulsevideo-tests-')
    try:
        yield dir_
    finally:
        shutil.rmtree(dir_, ignore_errors=True)


@pytest.yield_fixture
def pulsevideo(tmpdir):
    socket_path = '%s/dbus_socket' % tmpdir

    with open('%s/session.conf' % tmpdir, 'w') as out, \
            open('%s/session.conf.in' % os.path.dirname(__file__)) as in_:
        out.write(in_.read()
                  .replace('@DBUS_SOCKET@', socket_path)
                  .replace('@SERVICEDIR@', "%s/services" % tmpdir))

    os.mkdir('%s/services' % tmpdir)
    with open('%s/services/com.stbtester.VideoSource.test.service' % tmpdir,
              'w') as out, \
            open('%s/com.stbtester.VideoSource.test.service.in'
                 % os.path.dirname(__file__)) as in_:
        out.write(
            in_.read()
            .replace('@PULSEVIDEO@',
                     'env GST_PLUGIN_PATH=%s/../build '
                     'LD_LIBRARY_PATH=%s/../build/ '
                     'G_DEBUG=fatal_warnings %s/../pulsevideo '
                     '--caps=video/x-raw,format=RGB,width=320,'
                     'height=240,framerate=10/1 '
                     '--source-pipeline="videotestsrc is-live=true"' % (
                         os.path.dirname(__file__), os.path.dirname(__file__),
                         os.path.dirname(__file__)))
            .replace('@TMPDIR@', tmpdir))

    os.environ['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=%s' % socket_path

    dbus = subprocess.Popen(
        ['dbus-daemon', '--config-file=%s/session.conf' % tmpdir, '--nofork'])
    for _ in range(100):
        if os.path.exists(socket_path):
            break
        assert dbus.poll() is None, "dbus-daemon failed to start up"
        time.sleep(0.1)
    else:
        assert False, "dbus-daemon didn't take socket-path"

    try:
        yield
    finally:
        del os.environ['DBUS_SESSION_BUS_ADDRESS']
        dbus.kill()
        os.remove(socket_path)


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


def test_with_dbus(pulsevideo):
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', 'dbusvideosourcesrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
        stdout=subprocess.PIPE)
    fc = FrameCounter(gst_launch.stdout)
    fc.start()
    time.sleep(1)
    count = fc.count
    assert count >= 5 and count <= 15
