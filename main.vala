using GLib;
using Gst;
using Posix;


[DBus (name = "com.stb-tester.VideoSource1")]
public interface VideoSource1 : GLib.Object {

    public abstract string caps { owned get; }
    public abstract GLib.UnixInputStream attach () throws Error;
}

GLib.Error ioerror_from_errno (int err_no, string msg)
{
    return new GLib.Error(IOError.quark(), g_io_error_from_errno(err_no),
        msg);
}

public class VideoSource : GLib.Object, VideoSource1 {

    public string caps { owned get { return caps_; } }
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
        var error = Posix.socketpair (Posix.AF_UNIX, Posix.SOCK_STREAM, 0,
            fds);
        if (error != 0) {
            throw ioerror_from_errno(GLib.errno, "socketpair failed");
        }
        var wtr = new GLib.Socket.from_fd(fds[0]);
        GLib.Signal.emit_by_name(multisocketsink, "add", wtr, null);

        return new GLib.UnixInputStream(fds[1], true);
    }
}

void create_videosource(string source, GLib.DBusConnection dbus,
        string object_path) throws Error
{
    // Creating pipeline and elements
    var caps = "video/x-raw,format=RGB,width=1280,height=720,framerate=30/1";
    var pipeline = (Gst.Pipeline) Gst.parse_launch(
        source + " ! videoconvert ! " + caps + " ! multisocketsink name=sink");

    var sink = pipeline.get_by_name("sink");

    // Set pipeline state to PLAYING
    pipeline.set_state (State.PLAYING);

    dbus.register_object(object_path, (VideoSource1) new VideoSource(caps, sink));
}

int main (string[] args) {
    // Initializing GStreamer
    Gst.init (ref args);

    try {
        var dbus = GLib.Bus.get_sync (BusType.SESSION);

        create_videosource("v4l2src", dbus, "/com/stb-tester/VideoSource");

        uint32 request_name_result = (uint32) dbus.call_sync (
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName",
            new Variant ("(su)", "com.stb-tester.VideoSource1", 0x4),
            null, 0, -1);
        if (request_name_result == 0) {
            GLib.stderr.printf ("Could not register name on DBus");
            return 1;
        }
    }
    catch (Error e) {
        GLib.stderr.printf ("Error: %s".printf(e.message));
        return 1;
    }

    // Creating and starting a GLib main loop
    new MainLoop().run();
    return 0;
}

