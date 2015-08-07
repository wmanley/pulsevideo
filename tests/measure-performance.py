#!/usr/bin/python

from __future__ import division, unicode_literals

import argparse
import os
import subprocess
import sys
import time
from collections import namedtuple


def main(argv):
    parser = argparse.ArgumentParser()
    parser.parse_args(argv[1:])

    subprocess.check_call(['make'], stdout=sys.stderr)
    version = subprocess.check_output(['git', 'describe', '--always']).strip()

    env = dict(os.environ)
    env['GST_PLUGIN_PATH'] = os.path.abspath('build')
    env['LD_LIBRARY_PATH'] = os.path.abspath('build')

    pulsevideo = subprocess.Popen(
        ['./pulsevideo',
         '--source-pipeline=videotestsrc is-live=true num-buffers=250',
         '--caps=video/x-raw, format=I420, width=1280, height=720, '
         'framerate=25/1'], env=env, stdout=sys.stderr)
    time.sleep(1)
    client = subprocess.Popen(
        ['gst-launch-1.0', 'pulsevideosrc', '!', 'queue',
         '!', 'xvimagesink'], env=env, stdout=sys.stderr)

    time.sleep(15)
    pv_info = read_stat_info(pulsevideo.pid)
    client_info = read_stat_info(client.pid)
    
    assert pv_info.state == 'Z'
    assert client_info.state == 'Z'

    ticks_per_second = os.sysconf(b'SC_CLK_TCK')

    print "%s %f %f %f %f" % (
        version,
        pv_info.stime / ticks_per_second,
        pv_info.utime / ticks_per_second,
        client_info.stime / ticks_per_second,
        client_info.utime / ticks_per_second)

    return 0

# From man proc:
STAT_FIELDS = (
    'pid comm state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt '
    'cmajflt utime stime cutime cstime priority nice num_threads itrealvalue '
    'starttime vsize rss rsslim startcode endcode startstack kstkesp kstkeip '
    'signal blocked sigignore sigcatch wchan nswap cnswap exit_signal '
    'processor rt_priority policy delayacct_blkio_ticks guest_time cguest_time '
    'start_data end_data start_brk arg_start arg_end env_start env_end '
    'exit_code').split()

StatData = namedtuple("StatData", STAT_FIELDS)


def read_stat_info(pid):
    with open('/proc/%i/stat' % pid) as f:
        statinfo = f.read().split()
    for n in [0] + range(3, len(STAT_FIELDS)):
        statinfo[n] = int(statinfo[n])
    return StatData(*statinfo)

if __name__ == '__main__':
    sys.exit(main(sys.argv))
