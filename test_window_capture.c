/* Exercise the real preview and encoding code with synthetic window frames.
 * Replace only the PipeWire connection so this runs without a portal picker. */
#include "portal_pipewire.h"
static PortalPipeWire *fake_source(int fd, guint node, GstElement *appsrc, GError **error);
#define portal_pipewire_new fake_source
#include "window_capture.c"
#undef portal_pipewire_new
#include "portal_pipewire.c"
#include <glib/gstdio.h>

static PortalPipeWire *fake_source(int fd, guint node, GstElement *appsrc, GError **error) {
    (void)fd; (void)node; (void)error;
    PortalPipeWire *source = g_new0(PortalPipeWire, 1);
    g_mutex_init(&source->frame_lock);
    source->appsrc = gst_object_ref(appsrc);
    source->frame_width = 320;
    source->frame_height = 240;
    source->last_frame = gst_buffer_new_allocate(NULL, 320 * 240 * 4, NULL);
    gst_buffer_memset(source->last_frame, 0, 100, 320 * 240 * 4);
    source->frames_seen = 1;
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRx",
        "width", G_TYPE_INT, 320, "height", G_TYPE_INT, 240,
        "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
    gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
    gst_caps_unref(caps);
    return source;
}
static gboolean finished;
static guint started;
static const gchar *expected_message;
static void event(WindowCaptureState state, const gchar *message, gpointer data) {
    (void)data;
    g_assert_cmpstr(message, ==, expected_message);
    if (state == WINDOW_CAPTURE_STARTED) started++;
    if (state == WINDOW_CAPTURE_FINISHED) finished = TRUE;
}
static void drain(guint ms) {
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}
static WindowCapture *new_capture(const gchar *path) {
    WindowCapture *capture = g_new0(WindowCapture, 1);
    capture->pipewire_fd = -1;
    capture->filename = g_strdup(path);
    capture->format = g_strdup("mp4");
    capture->codec = g_strdup("libx264");
    capture->preset = g_strdup("ultrafast");
    capture->audio = g_strdup("none");
    capture->fps = 30;
    capture->quantizer = 23;
    capture->event = event;
    return capture;
}

static void preview_before_start(void) {
    const gchar *plugins[] = {"x264enc", "h264parse", "mp4mux"};
    for (guint i = 0; i < G_N_ELEMENTS(plugins); i++) {
        GstElementFactory *factory = gst_element_factory_find(plugins[i]);
        if (!factory) {
            g_test_skip("H.264 encoder/parser/muxer plugins are unavailable");
            return;
        }
        gst_object_unref(factory);
    }
    GError *error = NULL;
    gchar *dir = g_dir_make_tmp("wf-recorder-preview-test-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *path = g_build_filename(dir, "window.mp4", NULL);
    WindowCapture *capture = new_capture(path);
    g_assert_true(start_pipeline(capture, &error));
    g_assert_no_error(error);
    drain(300);
    g_assert_false(capture->playing);
    g_assert_cmpuint(started, ==, 0);
    g_assert_cmpuint(portal_pipewire_frames(capture->source), ==, 0);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    GdkTexture *preview = window_capture_preview(capture);
    g_assert_nonnull(preview);
    g_assert_cmpint(gdk_texture_get_width(preview), ==, 320);
    g_object_unref(preview);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(capture->pipeline), "preview");
    g_assert_null(sink);
    window_capture_record(capture);
    g_assert_null(window_capture_preview(capture));
    drain(700);
    g_assert_true(capture->playing);
    g_assert_cmpuint(started, ==, 1);
    g_assert_cmpuint(portal_pipewire_frames(capture->source), >, 0);
    window_capture_stop(capture);
    for (guint i = 0; i < 50 && !finished; i++) drain(100);
    g_assert_true(finished);
    GStatBuf info;
    g_assert_cmpint(g_stat(path, &info), ==, 0);
    g_assert_cmpint(info.st_size, >, 0);
    g_unlink(path);

    /* Canceling preview must also tear down the stream without creating a file. */
    finished = FALSE;
    started = 0;
    expected_message = "Recording discarded";
    capture = new_capture(path);
    g_assert_true(start_pipeline(capture, &error));
    g_assert_no_error(error);
    drain(200);
    window_capture_cancel(capture);
    g_assert_true(finished);
    g_assert_cmpuint(started, ==, 0);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/window-capture/preview-before-start", preview_before_start);
    return g_test_run();
}
