[DBus (name = "com.example.VideoSource")]
interface VideoSource : GLib.Object {

    public abstract string caps { owned get; }
    public abstract GLib.UnixInputStream attach () throws Error;
}

void run() throws Error
{
    VideoSource demo = Bus.get_proxy_sync (
        BusType.SESSION, "com.example.VideoSource", "/com/example/videosource");

    var caps = demo.caps;
    var fd = demo.attach();
    stdout.printf("CAPS: %s\n".printf(caps));
    stdout.printf("FD: %i\n".printf(fd.fd));
    var pipeline_desc =
        "fdsrc fd=%i ! application/x-fd ! fddepay ! %s ! videoconvert ! filesink location=/dev/null".printf(fd.fd, caps);
/*    var pipeline_desc =
        "fdsrc fd=%i blocksize=%i ! %s ! videoconvert ! filesink location=/dev/null".printf(fd.fd, 1920*1080*3, caps);*/
    stdout.printf("Pipeline: %s\n".printf(pipeline_desc));
    var pipeline = (Gst.Pipeline) Gst.parse_launch(pipeline_desc);

    pipeline.set_state (Gst.State.PLAYING);

    // Creating and starting a GLib main loop
    new MainLoop ().run ();
}

void main (string[] args) {
    // Initializing GStreamer
    Gst.init (ref args);

    try {
        run();
    } catch (Error error) {
        warning ("%s", error.message);
    }
}
