#!/usr/bin/python

import socket
import subprocess
import os

rdr, wtr = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM, 0)

caps = 'video/x-raw,format=RGB,width=1920,height=1080,framerate=30/1'

subprocess.Popen("gst-launch-1.0 fdsrc fd=%i ! fddepay ! %s ! videoconvert ! autovideosink" % (rdr.fileno(), caps), shell=True)

cmd = ['gst-launch-1.0', 'videotestsrc', 'is-live=true', '!', caps, '!', 'fdpay',
    '!', 'fdsink', 'fd=%i' % wtr.fileno()]

if False:
    cmd = ['gdb', '--args'] + cmd
if True:
    cmd = ['valgrind'] + cmd
os.execvp(cmd[0], cmd)
