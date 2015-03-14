PulseVideo
==========

Allows multiplexing access to webcams such that more than one application can
read video from a single piece of hardware at a time.

Features:

* Multiplexes access to video devices - applications no longer need to fight
  over access to `/dev/video0`.

* Robust

    * Each stream runs in its own process so a problem with one stream will
      not affect others.

    * Clients can automatically recover if the server crashes with minimal
      interruption.

* Fast - single or possibly zero-copy full frame video using FD passing.  No
  additional copies for each additional connected client.

* Zero cost when not in used - relies on DBus activation so can be started
  on-demand.

* Simple wire format - It should be easy to implement a client with no
  dependencies on GStreamer or GLib.

* Secure to use between security domains[1] - designed for inter-container
  video transfer.

[1]: (with memfd, in theory)

Build & Run
-----------

### Dependencies

pulsevideo is intended to be buildable and runnable on Ubuntu 14.04 LTS and
later.  This means that it depends on GStreamer >1.2 and Vala >0.22.  See
.travis.yml for a list of `apt-get`able dependencies.

### Build and install

    make
    make install

### Run

Start pulsevideo server (defaults on using v4l2src device=/dev/video0 in 720p30):

    pulsevideo

Start pulsevideo with a custom pipeline and custom caps:

    pulsevideo --source-pipeline="videotestsrc is-live=true" --caps="video/x-raw, format=I420, width=320, height=240, framerate=25/1"

Show stream on screen:

    gst-launch-1.0 pulsevideosrc ! queue ! videoconvert ! xvimagesink

Design
------

* Server written in vala using GStreamer
* GStreamer elements written in C
* Uses DBus for discovery, activation and negotiation.
* Uses temporary files, mmap and FD passing for zero-copy video transfer hidden
  behind GStreamer allocators, payloaders and depayloaders.  See [this GStreamer
  lightning talk][gsttalk] for more information.

A client connects to a stream by calling `com.stbtester.VideoSource.attach`.
This returns a unix domain socket over which the video stream will be sent.
Multiple clients can connect and they will all be sent the same video stream.

We don't actually send the video data directly over the socket.  Instead each
video frame is written into a memfd[2] and the file description is sent over
the unix domain socket using the [SCM_RIGHTS] mechanism.  These video frames
can then be `mmap`ed by the clients.  If the GStreamer video-source supports
using downstream allocators this enables zero-copy video, otherwise a
single-copy is still required, although no additional copies are required for
each additional client.

A client will attempt to reconnect if the server shuts down the connection
before sending EOS downstream.  This offers an oppertunity to renegotiate and
in combination with DBus activation makes clients robust to pulsevideo servers
crashing or dieing for any other reason.  This allows us to include a `watchdog`
element in pulsevideo itself to kill the process if video stops flowing for any
reason, safe in the knowledge that it will recover if possible.

A pulsevideo server exposes an object on the bus with a bus name like
`com.stbtester.VideoSource.XXXX`.  This service provides an object at
`/com/stbtester/VideoSource/XXXX` that implements the
`com.stbtester.VideoSource` interface.

It is expected that pulsevideo will be launched by DBus activation when
required.  This means that when not required pulsevideo doesn't need to be
running, and if it crashes it will be automatically restarted when the client
reconnects.

TODO: Document wire format

[2]: For systems that don't support memfd[3] an unlinked temporary file on tmpfs
     is used.  This isn't secure however so should not be used between security
     domains.

[3]: memfd support is not actually implemented yet in pulsevideo, so actually we
     currently always "fall-back" to temporary files.

[SCM_RIGHTS]: http://keithp.com/blogs/fd-passing/

TODO
----

* Rename VideoSource to pulsevideo everywhere.
* Implement memfd support and add "insecure=false" property.
* Protocol documentation - this should be independantly implementable without
  requiring that clients use GStreamer.
* Push some of the elements upstream into GStreamer.
* Don't require specifying caps server-side.  Have clients ask for caps and
  choose a format that satisfies them all.

[gsttalk]: http://gstconf.ubicast.tv/videos/zero-copy-video-with-file-descriptor-passing/
