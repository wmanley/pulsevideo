PulseVideo
==========

Allows multiplexing access to webcams such that more than one application can
read video from a single piece of hardware at a time.

Build & Run
-----------

    make
    make install

Start pulsevideo server:

    pulsevideo

Show webcam on screen:

    gst-launch-1.0 dbusvideosourcesrc \
        ! videoparse width=1280 height=720 format=rgb ! queue ! videoconvert \
        ! xvimagesink

Design
------

TODO: Go into more detail

* Written in C and vala using GStreamer
* Uses DBus
* One process/bus-name per device to enable activation

TODO
----

* Come up with a better name
* Write tests
* Use [zero-copy video via file-descriptor passing][1]
* Deal with v4l errors by restarting the pipeline
* Protocol documentation - this should be independantly implementable without
  requiring that clients use GStreamer.
* Other Documentation
* Payloading

[1]: http://gstconf.ubicast.tv/videos/zero-copy-video-with-file-descriptor-passing/
