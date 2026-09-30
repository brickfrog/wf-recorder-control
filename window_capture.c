#include "window_capture.h"
#include "portal_pipewire.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <libportal-gtk4/portal-gtk4.h>
#include <unistd.h>

struct WindowCapture {
    GtkWindow *parent;
    XdpPortal *portal;
    XdpParent *portal_parent;
    XdpSession *session;
    GstElement *pipeline;
    PortalPipeWire *source;
    guint bus_watch;
    guint source_watch;
    gint pipewire_fd;
    guint node;
    gboolean playing;
    gchar *filename, *format, *codec, *preset, *audio;
    gint fps, quantizer, bitrate;
    WindowCaptureEvent event;
    gpointer data;
};

static void capture_finish(WindowCapture *capture, const gchar *message) {
    if (capture->source_watch) {
        g_source_remove(capture->source_watch);
        capture->source_watch = 0;
    }
    if (capture->bus_watch) {
        g_source_remove(capture->bus_watch);
        capture->bus_watch = 0;
    }
    portal_pipewire_free(capture->source);
    if (capture->pipeline) {
        gst_element_set_state(capture->pipeline, GST_STATE_NULL);
        gst_object_unref(capture->pipeline);
    }
    if (capture->pipewire_fd >= 0) close(capture->pipewire_fd);
    if (capture->session) {
        xdp_session_close(capture->session);
        g_object_unref(capture->session);
    }
    if (capture->portal_parent) xdp_parent_free(capture->portal_parent);
    if (capture->portal) g_object_unref(capture->portal);
    capture->event(WINDOW_CAPTURE_FINISHED, message, capture->data);
    g_free(capture->filename);
    g_free(capture->format);
    g_free(capture->codec);
    g_free(capture->preset);
    g_free(capture->audio);
    g_free(capture);
}

static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data) {
    WindowCapture *capture = data;
    (void)bus;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        gboolean has_video = portal_pipewire_frames(capture->source) > 0;
        capture->bus_watch = 0;
        capture_finish(capture, has_video ? NULL : "No video frames were captured");
        return G_SOURCE_REMOVE;
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        gchar *text = g_strdup(error ? error->message : "Window recording failed");
        capture->bus_watch = 0;
        capture_finish(capture, text);
        g_free(text);
        g_clear_error(&error);
        g_free(debug);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gchar *state_failure(GstElement *pipeline) {
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *failure = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!failure) return g_strdup("Could not start window capture pipeline");
    GError *error = NULL;
    gst_message_parse_error(failure, &error, NULL);
    gchar *message = g_strdup_printf("Could not start window capture pipeline: %s",
                                     error ? error->message : "unknown GStreamer error");
    g_clear_error(&error);
    gst_message_unref(failure);
    return message;
}

/* Recording starts on the first window frame, so audio and video share a zero start
 * and a window that is off screen when recording begins adds no empty lead-in. */
static gboolean start_playing(WindowCapture *capture) {
    if (gst_element_set_state(capture->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gchar *message = state_failure(capture->pipeline);
        capture->source_watch = 0;
        capture_finish(capture, message);
        g_free(message);
        return FALSE;
    }
    capture->playing = TRUE;
    portal_pipewire_activate(capture->source);
    capture->event(WINDOW_CAPTURE_STARTED, NULL, capture->data);
    return TRUE;
}

static gboolean source_health(gpointer data) {
    WindowCapture *capture = data;
    const gchar *error = portal_pipewire_error(capture->source);
    if (error) {
        gchar *message = g_strdup_printf("Window capture failed: %s", error);
        capture->source_watch = 0;
        capture_finish(capture, message);
        g_free(message);
        return G_SOURCE_REMOVE;
    }
    if (!capture->playing && portal_pipewire_frames_seen(capture->source) > 0 && !start_playing(capture))
        return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

static const gchar *encoder_name(const gchar *codec) {
    if (!g_strcmp0(codec, "libx264")) return "x264enc";
    if (!g_strcmp0(codec, "libx265")) return "x265enc";
    if (!g_strcmp0(codec, "libvpx-vp9")) return "vp9enc";
    if (!g_strcmp0(codec, "libaom-av1")) return "av1enc";
    if (!g_strcmp0(codec, "h264_vaapi")) return "vah264enc";
    if (!g_strcmp0(codec, "hevc_vaapi")) return "vah265enc";
    return NULL;
}

static const gchar *parser_name(const gchar *codec) {
    if (g_str_has_suffix(codec, "x264") || !g_strcmp0(codec, "h264_vaapi")) return "h264parse";
    if (g_str_has_suffix(codec, "x265") || !g_strcmp0(codec, "hevc_vaapi")) return "h265parse";
    if (!g_strcmp0(codec, "libvpx-vp9")) return "vp9parse";
    return "av1parse";
}

static const gchar *mux_name(const gchar *format) {
    if (!g_strcmp0(format, "mp4")) return "mp4mux";
    if (!g_strcmp0(format, "mkv")) return "matroskamux";
    if (!g_strcmp0(format, "webm")) return "webmmux";
    return NULL;
}

static gboolean start_pipeline(WindowCapture *capture, GError **error) {
    const gchar *encoder = encoder_name(capture->codec);
    const gchar *parser = parser_name(capture->codec);
    const gchar *mux = mux_name(capture->format);
    if (!encoder || !mux) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Unsupported window recording format");
        return FALSE;
    }
    gboolean with_audio = g_strcmp0(capture->audio, "none") != 0;
    const gchar *audio_encoder = !g_strcmp0(capture->format, "webm") ? "opusenc" : "avenc_aac";
    gchar *description = g_strdup_printf(
        "%s name=mux ! filesink name=output sync=false "
        "appsrc name=video_src is-live=true format=time do-timestamp=true max-buffers=4 leaky-type=downstream ! videoconvert ! videorate ! "
        "video/x-raw,framerate=%d/1 ! tee name=split "
        "split. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! videorate drop-only=true max-rate=15 ! "
        "videoscale ! videoconvert ! video/x-raw,format=RGBA,width=%d,pixel-aspect-ratio=1/1 ! "
        "appsink name=preview max-buffers=1 drop=true sync=false async=false wait-on-eos=false "
        "split. ! queue ! %s name=video_encoder ! %s ! queue ! mux. %s",
        mux, capture->fps, WINDOW_CAPTURE_PREVIEW_WIDTH, encoder, parser,
        with_audio ? "pulsesrc name=audio_src ! audioconvert ! audioresample ! queue ! " : "");
    if (with_audio) {
        gchar *with_encoder = g_strconcat(description, audio_encoder, " ! queue ! mux.", NULL);
        g_free(description);
        description = with_encoder;
    }
    capture->pipeline = gst_parse_launch(description, error);
    g_free(description);
    if (!capture->pipeline || (error && *error)) return FALSE;
    GstElement *source = gst_bin_get_by_name(GST_BIN(capture->pipeline), "video_src");
    GstElement *video = gst_bin_get_by_name(GST_BIN(capture->pipeline), "video_encoder");
    GstElement *output = gst_bin_get_by_name(GST_BIN(capture->pipeline), "output");
    if (!source || !video || !output) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Missing GStreamer recording element");
        goto fail;
    }
    g_object_set(output, "location", capture->filename, NULL);
    if (!g_strcmp0(capture->codec, "libx264")) {
        g_object_set(video, "pass", 5, "quantizer", capture->quantizer, NULL);
        gst_util_set_object_arg(G_OBJECT(video), "speed-preset", capture->preset);
    } else if (!g_strcmp0(capture->codec, "libx265")) {
        gchar *options = g_strdup_printf("crf=%d", capture->quantizer);
        g_object_set(video, "option-string", options, NULL);
        gst_util_set_object_arg(G_OBJECT(video), "speed-preset", capture->preset);
        g_free(options);
    } else if (!g_strcmp0(capture->codec, "libvpx-vp9")) {
        /* Realtime speed settings: the default VP9/AV1 modes cannot keep up with screen capture. */
        g_object_set(video, "end-usage", 2, "cq-level", capture->quantizer,
                     "deadline", (gint64)1, "cpu-used", 8, "row-mt", TRUE,
                     "threads", (gint)MIN(g_get_num_processors(), 64), NULL);
    } else if (!g_strcmp0(capture->codec, "libaom-av1")) {
        g_object_set(video, "end-usage", 3,
                     "min-quantizer", (guint)capture->quantizer,
                     "max-quantizer", (guint)capture->quantizer,
                     "cpu-used", 8, "row-mt", TRUE, "threads", 0u, NULL);
        gst_util_set_object_arg(G_OBJECT(video), "usage-profile", "realtime");
    } else if (!g_strcmp0(capture->codec, "h264_vaapi") ||
               !g_strcmp0(capture->codec, "hevc_vaapi")) {
        if (capture->bitrate > 0)
            g_object_set(video, "rate-control", 4, "bitrate", (guint)capture->bitrate, NULL);
        else
            g_object_set(video, "rate-control", 16,
                         "qpi", (guint)capture->quantizer,
                         "qpp", (guint)capture->quantizer,
                         "qpb", (guint)capture->quantizer, NULL);
    }
    if (with_audio) {
        GstElement *audio = gst_bin_get_by_name(GST_BIN(capture->pipeline), "audio_src");
        if (!audio) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Missing audio capture element");
            goto fail;
        }
        if (g_strcmp0(capture->audio, "default")) g_object_set(audio, "device", capture->audio, NULL);
        gst_object_unref(audio);
    }
    GstBus *bus = gst_element_get_bus(capture->pipeline);
    capture->bus_watch = gst_bus_add_watch(bus, bus_message, capture);
    gst_object_unref(bus);
    if (gst_element_set_state(capture->pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
        gchar *message = state_failure(capture->pipeline);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
        g_free(message);
        goto fail;
    }
    capture->source = portal_pipewire_new(capture->pipewire_fd, capture->node, source, error);
    if (!capture->source) goto fail;
    close(capture->pipewire_fd);
    capture->pipewire_fd = -1;
    capture->source_watch = g_timeout_add(100, source_health, capture);
    gst_object_unref(source);
    gst_object_unref(video);
    gst_object_unref(output);
    return TRUE;
fail:
    if (source) gst_object_unref(source);
    if (video) gst_object_unref(video);
    if (output) gst_object_unref(output);
    return FALSE;
}

static void session_started(GObject *source, GAsyncResult *result, gpointer data) {
    WindowCapture *capture = data;
    GError *error = NULL;
    if (!xdp_session_start_finish(XDP_SESSION(source), result, &error)) {
        capture_finish(capture, error ? error->message : "Window selection cancelled");
        g_clear_error(&error);
        return;
    }
    GVariant *streams = xdp_session_get_streams(capture->session);
    if (!streams || !g_variant_n_children(streams)) {
        if (streams) g_variant_unref(streams);
        capture_finish(capture, "No window selected");
        return;
    }
    GVariant *first = g_variant_get_child_value(streams, 0);
    GVariant *properties = NULL;
    g_variant_get(first, "(u@a{sv})", &capture->node, &properties);
    g_variant_unref(properties);
    g_variant_unref(first);
    g_variant_unref(streams);
    capture->pipewire_fd = xdp_session_open_pipewire_remote(capture->session);
    if (capture->pipewire_fd < 0) {
        capture_finish(capture, "Could not open window video stream");
        return;
    }
    capture->event(WINDOW_CAPTURE_SELECTED, NULL, capture->data);
}

void window_capture_record(WindowCapture *capture) {
    GError *error = NULL;
    if (!start_pipeline(capture, &error)) {
        capture_finish(capture, error ? error->message : "Could not start window recorder");
        g_clear_error(&error);
    }
}

static void session_created(GObject *source, GAsyncResult *result, gpointer data) {
    WindowCapture *capture = data;
    GError *error = NULL;
    capture->session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &error);
    if (!capture->session) {
        capture_finish(capture, error ? error->message : "Could not create window capture session");
        g_clear_error(&error);
        return;
    }
    capture->portal_parent = xdp_parent_new_gtk(capture->parent);
    xdp_session_start(capture->session, capture->portal_parent, NULL, session_started, capture);
}

WindowCapture *window_capture_begin(GtkWindow *parent, const gchar *filename,
                                     const gchar *format, const gchar *codec,
                                     gint fps, gint quantizer, gint bitrate, const gchar *preset,
                                     const gchar *audio, WindowCaptureEvent event,
                                     gpointer data) {
    WindowCapture *capture = g_new0(WindowCapture, 1);
    capture->parent = parent;
    capture->portal = xdp_portal_new();
    capture->pipewire_fd = -1;
    capture->filename = g_strdup(filename);
    capture->format = g_strdup(format);
    capture->codec = g_strdup(codec);
    capture->preset = g_strdup(preset);
    capture->audio = g_strdup(audio);
    capture->fps = fps;
    capture->quantizer = quantizer;
    capture->bitrate = bitrate;
    capture->event = event;
    capture->data = data;
    xdp_portal_create_screencast_session(capture->portal, XDP_OUTPUT_WINDOW,
                                         XDP_SCREENCAST_FLAG_NONE,
                                         XDP_CURSOR_MODE_EMBEDDED,
                                         XDP_PERSIST_MODE_NONE, NULL, NULL,
                                         session_created, capture);
    return capture;
}

void window_capture_stop(WindowCapture *capture) {
    if (!capture) return;
    if (!capture->playing) {
        if (capture->session) capture_finish(capture, "Recording cancelled");
        return;
    }
    portal_pipewire_finish_frame(capture->source);
    GstElement *source = gst_bin_get_by_name(GST_BIN(capture->pipeline), "video_src");
    if (source) {
        gst_app_src_end_of_stream(GST_APP_SRC(source));
        gst_object_unref(source);
    }
    /* The muxer finalizes only after every branch ends, including live audio. */
    GstElement *audio = gst_bin_get_by_name(GST_BIN(capture->pipeline), "audio_src");
    if (audio) {
        gst_element_send_event(audio, gst_event_new_eos());
        gst_object_unref(audio);
    }
}

void window_capture_cancel(WindowCapture *capture) {
    if (capture) capture_finish(capture, "Recording discarded");
}

gboolean window_capture_recording(WindowCapture *capture) {
    return capture && capture->playing;
}

GdkTexture *window_capture_preview(WindowCapture *capture) {
    if (!capture || !capture->pipeline) return NULL;
    GstElement *sink = gst_bin_get_by_name(GST_BIN(capture->pipeline), "preview");
    if (!sink) return NULL;
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0);
    gst_object_unref(sink);
    if (!sample) return NULL;
    GdkTexture *texture = NULL;
    GstStructure *caps = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    gint width = 0, height = 0;
    GstMapInfo map;
    if (gst_structure_get_int(caps, "width", &width) && gst_structure_get_int(caps, "height", &height) &&
        width > 0 && height > 0 && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gsize stride = (gsize)width * 4;
        if (map.size >= stride * (gsize)height) {
            GBytes *bytes = g_bytes_new(map.data, stride * (gsize)height);
            texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, stride);
            g_bytes_unref(bytes);
        }
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return texture;
}
