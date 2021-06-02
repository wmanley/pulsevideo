using GLib;
using Gst;
using Posix;

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

Gst.Pipeline create_videosource(string source, string caps, string object_path,
    string bus_name_suffix) throws Error
{
    // Creating pipeline and elements
    var pipeline = (Gst.Pipeline) Gst.parse_launch(
        source + " ! pulsevideosink caps=\"" + caps
        + "\" object-path=\"" + object_path + "\" " +
        "bus-name=\"com.stbtester.VideoSource." + bus_name_suffix + "\"");

    var bus = pipeline.get_bus();
    bus.add_signal_watch();
    bus.message.connect(handle_message);

    pipeline.set_state(Gst.State.PLAYING);

    return pipeline;
}

int main (string[] args) {
    string? source_pipeline = "v4l2src";
    string? caps =
        "video/x-raw,format=BGR,width=1280,height=720,framerate=30/1,interlace-mode=progressive";
    string? name = "dev_video0";
    Gst.Pipeline pipeline;
    GLib.OptionEntry[] options = {
        GLib.OptionEntry () {
            long_name = "caps", short_name = 0, flags = 0,
            arg = OptionArg.STRING, arg_data = &caps,
            description = "Video format to use",
            arg_description = "CAPS" },
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
        pipeline = create_videosource(
            source_pipeline, caps, "/com/stbtester/VideoSource", name);
    }
    catch (Error e) {
        GLib.stderr.printf ("Error: %s", e.message);
        return 1;
    }

    // Creating and starting a GLib main loop
    new MainLoop().run();
    pipeline.set_state(Gst.State.NULL);
    return 0;
}

