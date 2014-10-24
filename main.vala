using GLib;
using Gst;
using Posix;

[DBus (name = "com.example.VideoSource")]
public class VideoSource : GLib.Object {

    public string caps { get { return caps_; } }
    private string caps_;
    private Gst.Element multisocketsink;

    public VideoSource (string caps, Gst.Element multisocketsink)
    {
        this.caps_ = caps;
        this.multisocketsink = multisocketsink;
    }

    public GLib.UnixInputStream attach () throws Error
    {
        var fds = new int[2];
        var success = Posix.socketpair (Posix.AF_UNIX, Posix.SOCK_STREAM, 0, fds);
        var wtr = new GLib.Socket.from_fd(fds[0]);
        GLib.Signal.emit_by_name(multisocketsink, "add", wtr, null);

        return new GLib.UnixInputStream(fds[1], true);
    }
}

void main (string[] args) {
    // Initializing GStreamer
    Gst.init (ref args);

    // Creating pipeline and elements
    var caps = "video/x-raw,format=RGB,width=1920,height=1080,framerate=30/1";
    var pipeline = (Gst.Pipeline) Gst.parse_launch("videotestsrc is-live=true ! " + caps + " ! " 
        + "fdpay ! multisocketsink name=sink");

    var sink = pipeline.get_by_name("sink");

    // Set pipeline state to PLAYING
    pipeline.set_state (State.PLAYING);

    var conn = GLib.Bus.get_sync (BusType.SESSION);
    conn.register_object("/com/example/videosource", new VideoSource(caps, sink));

    var request_result = conn.call_sync ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                                          new Variant ("(su)", "com.example.VideoSource", 0x4), null, 0, -1);

    // Creating and starting a GLib main loop
    new MainLoop ().run ();
}

