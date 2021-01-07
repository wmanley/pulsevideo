import os
import pipes
import re
import shutil
import signal
import socket
import tempfile
import threading
import time
from collections import namedtuple
from contextlib import contextmanager
from textwrap import dedent

import dbus
import pytest
import subprocess32 as subprocess


@pytest.yield_fixture(scope='function')
def tmpdir():
    dir_ = tempfile.mkdtemp(prefix='pulsevideo-tests-')
    try:
        yield dir_
    finally:
        shutil.rmtree(dir_, ignore_errors=True)

DEFAULT_SOURCE_PIPELINE = 'videotestsrc is-live=true'


def pulsevideo_cmdline(source_pipeline=None):
    if source_pipeline is None:
        source_pipeline = DEFAULT_SOURCE_PIPELINE
    return ['/usr/bin/env',
            'GST_PLUGIN_PATH=%s/../build' % os.path.dirname(__file__),
            'LD_LIBRARY_PATH=%s/../build/' % os.path.dirname(__file__),
            'G_DEBUG=fatal-warnings',
            '%s/../pulsevideo' % os.path.dirname(__file__),
            '--caps=video/x-raw,format=RGB,width=320,height=240,framerate=10/1',
            '--source-pipeline=%s' % source_pipeline,
            '--bus-name-suffix=test']


TestCtx = namedtuple('TestCtx', 'bus pulsevideod')


@contextmanager
def pulsevideo_via_activation(tmpdir):
    mkdir_p('%s/services' % tmpdir)

    with open('%s/pulsevideo' % tmpdir, 'w') as f:
        f.write(
            "#!/bin/sh -ex\n"
            "exec %s\n" % (shquote(pulsevideo_cmdline())))
        os.fchmod(f.fileno(), 0o755)

    with open('%s/services/com.stbtester.VideoSource.test.service' % tmpdir,
              'w') as out, \
            open('%s/com.stbtester.VideoSource.test.service.in'
                 % os.path.dirname(__file__)) as in_:
        out.write(in_.read().replace('@TMPDIR@', tmpdir))

    with dbus_ctx(tmpdir) as (_, bus_address):
        import dbus
        yield TestCtx(dbus.bus.BusConnection(bus_address), None)


def shquote(l):
    return ' '.join(pipes.quote(x) for x in l)


@contextmanager
def pulsevideo_ctx(dir_, source_pipeline=None):
    with dbus_ctx(dir_) as (dbus_daemon, bus_address):
        pulsevideod = subprocess.Popen(pulsevideo_cmdline(source_pipeline))
        sbus = dbus.bus.BusConnection(bus_address)
        bus = sbus.get_object('org.freedesktop.DBus', '/')
        assert dbus_daemon.poll() is None
        assert wait_until(
            lambda: 'com.stbtester.VideoSource.test' in bus.ListNames())
        yield TestCtx(sbus, pulsevideod)
        pulsevideod.kill()
        pulsevideod.wait()
        sbus.close()


@pytest.yield_fixture(scope='function')
def pulsevideo(tmpdir):
    with pulsevideo_ctx(tmpdir) as pulsevideod:
        yield pulsevideod


@pytest.yield_fixture(scope='function')
def dbus_fixture(tmpdir):
    with dbus_ctx(tmpdir) as x:
        yield x


def mkdir_p(dirs):
    import errno
    try:
        os.makedirs(dirs)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise


@contextmanager
def dbus_ctx(dir_):
    socket_path = '%s/dbus_socket' % dir_
    mkdir_p('%s/services' % dir_)

    with open('%s/session.conf' % dir_, 'w') as out, \
            open('%s/session.conf.in' % os.path.dirname(__file__)) as in_:
        out.write(in_.read()
                  .replace('@DBUS_SOCKET@', socket_path)
                  .replace('@SERVICEDIR@', "%s/services" % dir_))

    dbus_daemon = subprocess.Popen(
        ['dbus-daemon', '--config-file=%s/session.conf' % dir_, '--nofork'])
    for _ in range(100):
        if os.path.exists(socket_path):
            break
        assert dbus_daemon.poll() is None, "dbus-daemon failed to start up.  Failed with exit status %i" % dbus_daemon.returncode
        time.sleep(0.1)
    else:
        assert False, "dbus-daemon didn't take socket-path"

    address = 'unix:path=%s' % socket_path
    os.environ['DBUS_SESSION_BUS_ADDRESS'] = address

    try:
        yield dbus_daemon, address
    finally:
        os.remove(socket_path)
        dbus_daemon.kill()
        dbus_daemon.wait()
        del os.environ['DBUS_SESSION_BUS_ADDRESS']


class FrameCounter(object):
    def __init__(self, file_, frame_size=320 * 240 * 3, echo=False):
        self.file = file_
        self.count = 0
        self.frame_size = 320 * 240 * 3
        self.thread = threading.Thread(target=self._read_in_loop)
        self.thread.daemon = True
        self.echo = echo

    def start(self):
        self.thread.start()

    def _read_in_loop(self):
        bytes_read = 0
        while True:
            new_data = self.file.read(self.frame_size)
            if self.echo:
                print new_data,

            new_bytes_read = len(new_data)
            bytes_read += new_bytes_read

            # GIL makes this thread safe :)
            self.count = bytes_read // self.frame_size


def test_with_dbus(tmpdir):
    with pulsevideo_via_activation(tmpdir):
        os.environ['GST_DEBUG'] = "3,*videosource*:9"
        gst_launch = subprocess.Popen(
            ['gst-launch-1.0', 'pulsevideosrc',
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


def test_that_pulsevideosrc_recovers_if_pulsevideo_crashes(tmpdir, capfdbinary):
    with pulsevideo_via_activation(tmpdir):
        cmd = shquote(pulsevideo_cmdline())
        with open("%s/pulsevideo" % tmpdir, 'w') as f:
            f.write(dedent("""\
                #!/bin/bash -ex

                progress=$(cat {tmpdir}/progress || echo 0)
                echo $((progress + 1)) >{tmpdir}/progress
                case $progress in
                0)
                    # Simulate crash before grabbing bus name
                    exit 1
                ;;
                1)
                    # Simulate crash during DBus "Attach" call
                    export pre_attach=abort
                ;;
                2)
                    # Should trigger the watchdog
                    export fdpay_buffer="skip skip skip skip usleep=10000000"
                ;;
                3)
                    # Streaming error on 3rd buffer, should exit and be restarted
                    export fdpay_buffer="skip skip gerror"
                ;;
                esac
                exec {cmd}
                """.format(tmpdir=tmpdir, cmd=cmd)))

        os.environ['GST_DEBUG'] = "3,*videosource*:9"
        gst_launch = subprocess.Popen(
            ['gst-launch-1.0', '-e', '-q', 'pulsevideosrc',
             'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
            stdout=subprocess.PIPE)
        fc = FrameCounter(gst_launch.stdout)
        fc.start()
        assert wait_until(lambda: fc.count > 20, 20)

        gst_launch.kill()
        gst_launch.wait()

        # Make sure each of our failure scenarios actually happened:
        stderr = "".join(capfdbinary.readouterr().err)
        assert "Activated service 'com.stbtester.VideoSource.test' failed" in stderr
        assert "Aborted" in stderr
        assert "Error: Watchdog triggered" in stderr
        assert "Error: Internal data stream error." in stderr


def test_that_pulsevideosrc_fails_if_pulsevideo_is_not_available(
        dbus_fixture):
    os.environ['GST_DEBUG'] = "3,*videosource*:9"
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', 'pulsevideosrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'])
    assert wait_until(lambda: gst_launch.poll() is not None, 2)
    assert gst_launch.returncode != 0


def test_that_pulsevideosrc_gets_eos_if_pulsevideo_crashes_and_cant_be_activated(
        pulsevideo):
    print time.time()
    os.environ['GST_DEBUG'] = "3,*videosource*:9"
    print time.time()
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', '-q', 'pulsevideosrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
        stdout=subprocess.PIPE)
    fc = FrameCounter(gst_launch.stdout)
    fc.start()
    assert wait_until(lambda: fc.count > 1)
    pulsevideo.pulsevideod.kill()
    assert wait_until(lambda: gst_launch.poll() is not None, 2)
    assert gst_launch.returncode == 0


def test_that_pulsevideo_doesnt_leak_fds(pulsevideo):
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', '-q', 'pulsevideosrc',
         'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
        stdout=subprocess.PIPE)
    count_fds = lambda pid: len(os.listdir('/proc/%i/fd/' % pid))
    fc = FrameCounter(gst_launch.stdout)
    fc.start()
    assert wait_until(lambda: fc.count > 1)
    client_fd_count_1 = count_fds(gst_launch.pid)
    server_fd_count_1 = count_fds(pulsevideo.pulsevideod.pid)
    assert wait_until(lambda: fc.count > 20)
    client_fd_count_20 = count_fds(gst_launch.pid)
    server_fd_count_20 = count_fds(pulsevideo.pulsevideod.pid)

    assert (client_fd_count_20 - client_fd_count_1) < 5
    assert (server_fd_count_20 - server_fd_count_1) < 5


def test_that_we_can_tee_fddepay():
    CAPS = 'video/x-raw,format=RGB,width=320,height=240,framerate=10/1'
    gst_launch = subprocess.Popen(
        ['gst-launch-1.0', '-q', 'videotestsrc', 'is-live=true', '!', CAPS,
         '!', 'pvfdpay', '!', 'tee', 'name=t',
         't.', '!', 'queue', '!', 'pvfddepay', '!', CAPS, '!', 'fdsink', 'fd=1',
         't.', '!', 'queue', '!', 'pvfddepay', '!', CAPS, '!', 'fdsink', 'fd=2'],
         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    fc1 = FrameCounter(gst_launch.stdout)
    fc1.start()

    fc2 = FrameCounter(gst_launch.stderr)
    fc2.start()
    assert wait_until(lambda: fc1.count > 1)
    assert wait_until(lambda: fc2.count > 1)


def test_that_backing_memory_is_not_reused(pulsevideo):
    """
    This is a regression test.  There used to be a problem where only one
    temporary file was created and sent and it was then reused by pulsevideo.
    This had the effect that even after the client had received a video frame
    the contents would change.
    """
    from gi.repository import Gst
    Gst.init([])
    pipeline = Gst.parse_launch(
        'pulsevideosrc bus-name=com.stbtester.VideoSource.test '
        '! appsink name=appsink')
    appsink = pipeline.get_by_name('appsink')
    pipeline.set_state(Gst.State.PLAYING)

    buf = appsink.emit("pull-sample").get_buffer()
    initial_data = buf.extract_dup(0, 10000000)

    # Force a second buffer to have been populated but don't do anything with
    # it:
    appsink.emit("pull-sample")

    after_next_data = buf.extract_dup(0, 10000000)
    pipeline.set_state(Gst.State.NULL)
    assert initial_data == after_next_data


def test_that_invalid_sized_buffers_are_dropped(tmpdir):
    expected_buffer_size = 3 * 320 * 240

    # Should mean that only half the buffers (~50) get through:
    source_pipeline = (
        'videotestsrc pattern=white '
        '! rndbuffersize min=%i max=%i'
        % (expected_buffer_size - 1, expected_buffer_size + 1))
    with pulsevideo_ctx(tmpdir, source_pipeline=source_pipeline):
        os.environ['GST_DEBUG'] = '3'
        try:
            subprocess.check_output(
                ['gst-launch-1.0', '-q', 'pulsevideosrc',
                 'bus-name=com.stbtester.VideoSource.test',
                 '!', 'identity', 'error-after=101',
                 '!', 'checksumsink'])
        except subprocess.CalledProcessError as e:
            output = e.output
        buffers = [x.split() for x in output.strip().split('\n')]
        assert len(buffers) == 100

        # If they were of different sizes the checksum would be different:
        assert all(x[1] == 'b2fa672f8bba0b9c504e83c5b17ac848f0c14977'
                   for x in buffers)


def test_that_teardown_succeeds_during_error_recovery(tmpdir):
    with pulsevideo_via_activation(tmpdir):
        cmd = shquote(pulsevideo_cmdline())

        # Pipe used to signal back to the test that we've entered error-recovery
        os.mkfifo("%s/fifo" % tmpdir)
        with open("%s/pulsevideo" % tmpdir, 'w') as f:
            f.write(dedent("""\
                #!/bin/bash -ex

                progress=$(cat {tmpdir}/progress || echo 0)
                echo $((progress + 1)) >{tmpdir}/progress
                case $progress in
                0)
                    export fdpay_buffer="skip skip gerror"
                ;;
                *)
                    echo " " >{tmpdir}/fifo
                    exit 0
                ;;
                esac
                exec {cmd}
                """.format(tmpdir=tmpdir, cmd=cmd)))

        os.environ['GST_DEBUG'] = "3,*videosource*:9"
        gst_launch = subprocess.Popen(
            ['gst-launch-1.0', '-q', 'pulsevideosrc',
             'bus-name=com.stbtester.VideoSource.test', '!', 'fdsink'],
            stdout=subprocess.PIPE)
        fc = FrameCounter(gst_launch.stdout)
        fc.start()
        assert wait_until(lambda: fc.count > 0)

        # Wait for pulsevideosrc error recovery mode to kick in:
        with open("%s/fifo" % tmpdir) as f:
            f.read()

        # Now we know that the pulsevideosrc is in error recovery mode blocked
        # on a DBus call.  Tell it to stop:
        gst_launch.send_signal(signal.SIGINT)

        # DBus timeout takes 25s, we should be done much quicker than that:
        assert gst_launch.wait(5) is not None


def parse_gst_timestamp(timestamp):
    m = re.match(r"(\d+):(\d\d):(\d\d\.\d+)", timestamp)
    if m:
        return (int(m.group(1)) * 60 * 60 + int(m.group(2)) * 60
                + float(m.group(3)))


def test_that_timestamps_are_preserved(tmpdir):
    with pulsevideo_ctx(tmpdir, source_pipeline='videotestsrc is-live=false'):
        try:
            output = subprocess.check_output(
                ['gst-launch-1.0', '-q', 'pulsevideosrc',
                 'bus-name=com.stbtester.VideoSource.test', '!', 'identity',
                 'error-after=11', '!', 'checksumsink'])
        except subprocess.CalledProcessError as e:
            output = e.output
    buffers = [x.split() for x in output.strip().split('\n')]
    timestamps = [parse_gst_timestamp(x[0]) for x in buffers]
    assert len(timestamps) == 10
    for n in range(1, 9):
        # The caps specify framerate=1/10, so there should be exactly 0.1s
        # between frames
        assert abs(timestamps[n] + 0.1 - timestamps[n + 1]) < 1e-9
