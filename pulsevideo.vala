using GLib;
using Gst;
using Posix;


[DBus (name = "com.stbtester.VideoSource1")]
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
    private Gst.Pipeline pipeline;

    public VideoSource (string caps, Gst.Pipeline pipeline,
            Gst.Element multisocketsink)
    {
        this.caps_ = caps;
        this.multisocketsink = multisocketsink;
        this.pipeline = pipeline;

        var bus = pipeline.get_bus();
        bus.add_signal_watch();
        bus.message.connect(handle_message);
    }

    public GLib.UnixInputStream attach () throws Error
    {
        GLib.stderr.printf("Attaching client, entering PLAYING state\n");
        pipeline.set_state(Gst.State.PLAYING);

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

    public void handle_message (Gst.Message message)
    {
        switch (message.type) {
        case Gst.MessageType.ERROR:
            GLib.Error error;
            string debug;
            message.parse_error(out error, out debug);
            GLib.stderr.printf("Error: %s\n%s\n", error.message, debug);
            exit (1);
            break;
        case Gst.MessageType.WARNING:
            GLib.Error error;
            string debug;
            message.parse_warning(out error, out debug);
            GLib.stderr.printf("Warning: %s\n%s\n", error.message, debug);
            break;
        }
    }
}

void create_videosource(string source, GLib.DBusConnection dbus,
        string object_path) throws Error
{
    // Creating pipeline and elements
    var caps = "video/x-raw,format=RGB,width=1280,height=720,framerate=30/1";
    var pipeline = (Gst.Pipeline) Gst.parse_launch(
        source + " ! watchdog ! videoconvert ! " + caps
        + " ! multisocketsink buffers-max=1 name=sink sync=false");

    var sink = pipeline.get_by_name("sink");

    dbus.register_object(object_path,
        (VideoSource1) new VideoSource(caps, pipeline, sink));
}

int main (string[] args) {
    string? source_pipeline = "v4l2src";
    string? name = "dev_video0";
    GLib.OptionEntry[] options = {
        GLib.OptionEntry () {
            long_name = "source-pipeline", short_name = 0, flags = 0,
            arg = OptionArg.STRING, arg_data = &source_pipeline,
            description = "GStreamer pipeline to use as video source",
            arg_description = "SOURCE PIPELINE" },
        GLib.OptionEntry () {
            long_name = "bus-name-suffix", short_name = 0, flags = 0,
            arg = OptionArg.STRING, arg_data = &name,
            description = "DBus bus-name suffix",
            arg_description = "BUS NAME SUFFIX" },
        GLib.OptionEntry ()
    };

    try {
        var opt_context = new OptionContext (null);
        opt_context.set_help_enabled (true);
        opt_context.add_main_entries (options, null);
        opt_context.parse (ref args);
    } catch (OptionError e) {
        GLib.stderr.printf ("error: %s\n", e.message);
        GLib.stderr.printf ("Run '%s --help' to see a full list of available " +
                       "command line options.\n", args[0]);
        return 1;
    }

    // Initializing GStreamer
    Gst.init (ref args);

    try {
        var dbus = GLib.Bus.get_sync (BusType.SESSION);

        create_videosource(source_pipeline, dbus, "/com/stbtester/VideoSource");

        uint32 request_name_result = 0;
        dbus.call_sync (
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName",
            new Variant ("(su)", "com.stbtester.VideoSource." + name, 0x4),
            null, 0, -1).get("(u)", &request_name_result);
        if (request_name_result == 0) {
            GLib.stderr.printf ("Could not register name on DBus");
            return 1;
        }
    }
    catch (Error e) {
        GLib.stderr.printf ("Error: %s", e.message);
        return 1;
    }

    // Creating and starting a GLib main loop
    new MainLoop().run();
    return 0;
}

