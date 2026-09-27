#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include "window_capture.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *capture, *output, *audio, *format, *codec, *preset;
    GtkWidget *fps, *crf, *folder, *device, *pixel, *filter, *params;
    GtkWidget *no_damage, *no_dmabuf, *status, *start, *stop;
    GSubprocess *recording;
    WindowCapture *window_capture;
    gboolean selecting;
    gchar *filename;
} Recorder;

static gboolean codec_allowed(const gchar *format, const gchar *codec) {
    if (!g_strcmp0(format, "webm"))
        return !g_strcmp0(codec, "libvpx-vp9") || !g_strcmp0(codec, "libaom-av1");
    if (!g_strcmp0(format, "mp4"))
        return !g_strcmp0(codec, "libx264") || !g_strcmp0(codec, "libx265") ||
               !g_strcmp0(codec, "h264_vaapi") || !g_strcmp0(codec, "hevc_vaapi");
    return !g_strcmp0(format, "mkv");
}

static void set_status(Recorder *r, const gchar *message) {
    gtk_label_set_text(GTK_LABEL(r->status), message);
}

static GtkWidget *combo(const gchar *const choices[][2], gsize count) {
    GtkWidget *widget = gtk_combo_box_text_new();
    for (gsize i = 0; i < count; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), choices[i][0], choices[i][1]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), 0);
    return widget;
}

static GtkWidget *option_row(const gchar *label, GtkWidget *control) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_size_request(control, 235, -1);
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), control);
    return box;
}

static gchar *run_and_read(const gchar *const argv[]) {
    gchar *stdout_text = NULL, *stderr_text = NULL;
    GError *error = NULL;
    gint status = 0;
    if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &stdout_text, &stderr_text, &status, &error)) {
        g_clear_error(&error);
        g_clear_pointer(&stdout_text, g_free);
    }
    g_free(stderr_text);
    return stdout_text;
}

static GtkWidget *output_combo(void) {
    GtkWidget *widget = gtk_combo_box_text_new();
    const gchar *argv[] = {"wf-recorder", "-L", NULL};
    gchar *text = run_and_read(argv);
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    guint count = 0;
    for (guint i = 0; lines[i]; i++) {
        gchar *name = strstr(lines[i], "Name: ");
        if (!name) continue;
        name = g_strstrip(name + 6);
        if (*name) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), name, name);
            count++;
        }
    }
    if (!count) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), "", "No display found");
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), 0);
    g_strfreev(lines);
    g_free(text);
    return widget;
}

static GtkWidget *audio_combo(void) {
    const gchar *const initial[][2] = {{"none", "None"}, {"default", "Default source"}};
    GtkWidget *widget = combo(initial, G_N_ELEMENTS(initial));
    const gchar *argv[] = {"pactl", "list", "short", "sources", NULL};
    gchar *text = run_and_read(argv);
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar **fields = g_strsplit(lines[i], "\t", 3);
        if (fields[1] && *fields[1])
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), fields[1], fields[1]);
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(text);
    return widget;
}

static const gchar *selected(GtkWidget *widget) {
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
    return id ? id : "";
}

static void add_arg(GPtrArray *args, const gchar *value) {
    g_ptr_array_add(args, g_strdup(value));
}

static void add_pair(GPtrArray *args, const gchar *flag, const gchar *value) {
    add_arg(args, flag);
    add_arg(args, value);
}

static void add_pair_owned(GPtrArray *args, const gchar *flag, gchar *value) {
    add_arg(args, flag);
    g_ptr_array_add(args, value);
}

static gchar *next_filename(const gchar *folder, const gchar *format) {
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    gchar *path = g_strdup_printf("%s/Recording_%s.%s", folder, stamp, format);
    for (guint number = 2; g_file_test(path, G_FILE_TEST_EXISTS); number++) {
        g_free(path);
        path = g_strdup_printf("%s/Recording_%s_%u.%s", folder, stamp, number, format);
    }
    g_free(stamp);
    g_date_time_unref(now);
    return path;
}

static gboolean append_params(GPtrArray *args, const gchar *input, GError **error) {
    gchar **params = g_strsplit(input, ";", -1);
    for (guint i = 0; params[i]; i++) {
        gchar *param = g_strstrip(params[i]);
        if (!*param) continue;
        gchar *equals = strchr(param, '=');
        if (!equals || equals == param || !equals[1] || strpbrk(param, "\n\r")) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Extra codec parameters need key=value; key=value");
            g_strfreev(params);
            return FALSE;
        }
        add_pair(args, "-p", param);
    }
    g_strfreev(params);
    return TRUE;
}

static GPtrArray *build_command(Recorder *r, const gchar *geometry, GError **error) {
    const gchar *format = selected(r->format);
    const gchar *codec = selected(r->codec);
    const gchar *capture = selected(r->capture);
    const gchar *folder = gtk_editable_get_text(GTK_EDITABLE(r->folder));
    if (!*folder) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a save folder");
        return NULL;
    }
    if (!codec_allowed(format, codec)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Selected codec is not supported by this format");
        return NULL;
    }
    if (!g_strcmp0(capture, "region") && (!geometry || !*geometry)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Select a region first");
        return NULL;
    }
    if (!g_strcmp0(capture, "output") && !*selected(r->output)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No display selected");
        return NULL;
    }
    if (g_mkdir_with_parents(folder, 0755) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Cannot create save folder: %s", g_strerror(errno));
        return NULL;
    }
    g_clear_pointer(&r->filename, g_free);
    r->filename = next_filename(folder, format);
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    add_arg(args, "wf-recorder");
    add_pair(args, "-f", r->filename);
    if (!g_strcmp0(capture, "region")) add_pair(args, "-g", geometry);
    else add_pair(args, "-o", selected(r->output));
    const gchar *audio = selected(r->audio);
    if (!g_strcmp0(audio, "default")) add_arg(args, "-a");
    else if (g_strcmp0(audio, "none")) g_ptr_array_add(args, g_strdup_printf("--audio=%s", audio));
    add_pair(args, "-c", codec);
    add_pair_owned(args, "-r", g_strdup_printf("%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->fps))));
    if (!g_strcmp0(codec, "libx264") || !g_strcmp0(codec, "libx265")) {
        add_pair_owned(args, "-p", g_strdup_printf("crf=%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->crf))));
        add_pair_owned(args, "-p", g_strdup_printf("preset=%s", selected(r->preset)));
    } else if (!g_strcmp0(codec, "libvpx-vp9") || !g_strcmp0(codec, "libaom-av1")) {
        add_pair_owned(args, "-p", g_strdup_printf("crf=%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->crf))));
        add_pair(args, "-p", "b=0");
    }
    const struct { GtkWidget *widget; const gchar *flag; } optional[] = {
        {r->device, "-d"}, {r->pixel, "-x"}, {r->filter, "-F"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(optional); i++) {
        const gchar *value = gtk_editable_get_text(GTK_EDITABLE(optional[i].widget));
        if (*value) add_pair(args, optional[i].flag, value);
    }
    if (!append_params(args, gtk_editable_get_text(GTK_EDITABLE(r->params)), error)) {
        g_ptr_array_free(args, TRUE);
        return NULL;
    }
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(r->no_damage))) add_arg(args, "--no-damage");
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(r->no_dmabuf))) add_arg(args, "--no-dmabuf");
    g_ptr_array_add(args, NULL);
    return args;
}

static void recording_finished(GObject *source, GAsyncResult *result, gpointer data) {
    Recorder *r = data;
    gchar *stdout_text = NULL, *stderr_text = NULL;
    GError *error = NULL;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                          &stdout_text, &stderr_text, &error);
    g_clear_object(&r->recording);
    gtk_widget_set_sensitive(r->start, TRUE);
    gtk_widget_set_sensitive(r->stop, FALSE);
    GStatBuf info;
    if (r->filename && g_stat(r->filename, &info) == 0 && info.st_size > 0) {
        gchar *message = g_strdup_printf("Saved: %s", r->filename);
        set_status(r, message);
        g_free(message);
    } else {
        const gchar *detail = error ? error->message : (stderr_text && *stderr_text ? stderr_text : "No video produced");
        gchar *message = g_strdup_printf("Recording failed: %.240s", detail);
        set_status(r, message);
        g_free(message);
    }
    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
}

static void window_event(gboolean started, const gchar *error, gpointer data) {
    Recorder *r = data;
    r->selecting = FALSE;
    if (started) {
        gtk_widget_set_sensitive(r->stop, TRUE);
        gchar *message = g_strdup_printf("Recording selected window to %s", r->filename);
        set_status(r, message);
        g_free(message);
        return;
    }
    r->window_capture = NULL;
    gtk_widget_set_sensitive(r->start, TRUE);
    gtk_widget_set_sensitive(r->stop, FALSE);
    GStatBuf info;
    if (!error && r->filename && g_stat(r->filename, &info) == 0 && info.st_size > 0) {
        gchar *message = g_strdup_printf("Saved: %s", r->filename);
        set_status(r, message);
        g_free(message);
    } else set_status(r, error ? error : "Window selection cancelled");
}

static void launch_window_capture(Recorder *r) {
    const gchar *folder = gtk_editable_get_text(GTK_EDITABLE(r->folder));
    if (!*folder) {
        set_status(r, "Choose a save folder");
        gtk_widget_set_sensitive(r->start, TRUE);
        return;
    }
    if (g_mkdir_with_parents(folder, 0755) != 0) {
        gchar *message = g_strdup_printf("Cannot create save folder: %s", g_strerror(errno));
        set_status(r, message);
        g_free(message);
        gtk_widget_set_sensitive(r->start, TRUE);
        return;
    }
    g_clear_pointer(&r->filename, g_free);
    r->filename = next_filename(folder, selected(r->format));
    r->selecting = TRUE;
    set_status(r, "Choose a window in the niri portal…");
    r->window_capture = window_capture_begin(GTK_WINDOW(r->window), r->filename,
        selected(r->format), selected(r->codec),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->fps)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->crf)),
        selected(r->preset), selected(r->audio), window_event, r);
}

static void launch_recording(Recorder *r, const gchar *geometry) {
    GError *error = NULL;
    GPtrArray *args = build_command(r, geometry, &error);
    if (!args) goto failed;
    r->recording = g_subprocess_newv((const gchar *const *)args->pdata,
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_ptr_array_free(args, TRUE);
    if (!r->recording) goto failed;
    gtk_widget_set_sensitive(r->stop, TRUE);
    gchar *message = g_strdup_printf("Recording to %s", r->filename);
    set_status(r, message);
    g_free(message);
    g_subprocess_communicate_utf8_async(r->recording, NULL, NULL, recording_finished, r);
    return;
failed:
    set_status(r, error ? error->message : "Could not start recording");
    gtk_widget_set_sensitive(r->start, TRUE);
    g_clear_error(&error);
}

static void region_selected(GObject *source, GAsyncResult *result, gpointer data) {
    Recorder *r = data;
    gchar *geometry = NULL, *stderr_text = NULL;
    GError *error = NULL;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                          &geometry, &stderr_text, &error);
    r->selecting = FALSE;
    gtk_widget_set_visible(r->window, TRUE);
    gtk_window_present(GTK_WINDOW(r->window));
    if (!error && g_subprocess_get_successful(G_SUBPROCESS(source)) && geometry && *g_strstrip(geometry))
        launch_recording(r, geometry);
    else {
        set_status(r, "Region selection cancelled");
        gtk_widget_set_sensitive(r->start, TRUE);
    }
    g_object_unref(source);
    g_clear_error(&error);
    g_free(geometry);
    g_free(stderr_text);
}

static gboolean start_slurp(gpointer data) {
    Recorder *r = data;
    const gchar *argv[] = {"slurp", NULL};
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                            G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    if (!process) {
        gtk_widget_set_visible(r->window, TRUE);
        r->selecting = FALSE;
        set_status(r, error->message);
        gtk_widget_set_sensitive(r->start, TRUE);
        g_clear_error(&error);
    } else g_subprocess_communicate_utf8_async(process, NULL, NULL, region_selected, r);
    return G_SOURCE_REMOVE;
}

static void start_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (r->recording || r->window_capture || r->selecting) return;
    gtk_widget_set_sensitive(r->start, FALSE);
    if (!g_strcmp0(selected(r->capture), "window")) launch_window_capture(r);
    else if (!g_strcmp0(selected(r->capture), "region")) {
        r->selecting = TRUE;
        set_status(r, "Select a region…");
        gtk_widget_set_visible(r->window, FALSE);
        g_timeout_add(180, start_slurp, r);
    } else launch_recording(r, NULL);
}

static void stop_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (r->recording) {
        gtk_widget_set_sensitive(r->stop, FALSE);
        set_status(r, "Finalizing recording…");
        g_subprocess_send_signal(r->recording, SIGINT);
    } else if (r->window_capture && !r->selecting) {
        gtk_widget_set_sensitive(r->stop, FALSE);
        set_status(r, "Finalizing recording…");
        window_capture_stop(r->window_capture);
    }
}

static gboolean close_requested(GtkWindow *window, gpointer data) {
    Recorder *r = data;
    (void)window;
    if (!r->recording && !r->window_capture && !r->selecting) return FALSE;
    set_status(r, "Stop and save before closing");
    return TRUE;
}

static void codec_changed(GtkComboBox *widget, gpointer data) {
    Recorder *r = data;
    (void)widget;
    const gchar *codec = selected(r->codec);
    gboolean x26x = !g_strcmp0(codec, "libx264") || !g_strcmp0(codec, "libx265");
    gtk_widget_set_sensitive(r->crf, x26x || !g_strcmp0(codec, "libvpx-vp9") || !g_strcmp0(codec, "libaom-av1"));
    gtk_widget_set_sensitive(r->preset, x26x);
}

static void format_changed(GtkComboBox *widget, gpointer data) {
    Recorder *r = data;
    (void)widget;
    const gchar *format = selected(r->format);
    gchar *previous = g_strdup(selected(r->codec));
    const gchar *const codecs[][2] = {
        {"libx264", "H.264 software"}, {"libx265", "H.265 software"},
        {"libvpx-vp9", "VP9 software"}, {"libaom-av1", "AV1 software"},
        {"h264_vaapi", "H.264 VA-API"}, {"hevc_vaapi", "H.265 VA-API"}
    };
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(r->codec));
    for (guint i = 0; i < G_N_ELEMENTS(codecs); i++)
        if (codec_allowed(format, codecs[i][0]))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(r->codec), codecs[i][0], codecs[i][1]);
    if (codec_allowed(format, previous))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(r->codec), previous);
    else gtk_combo_box_set_active(GTK_COMBO_BOX(r->codec), 0);
    g_free(previous);
}

static void capture_changed(GtkComboBox *widget, gpointer data) {
    Recorder *r = data;
    (void)widget;
    gboolean window = !g_strcmp0(selected(r->capture), "window");
    gtk_widget_set_sensitive(r->output, !g_strcmp0(selected(r->capture), "output"));
    gtk_widget_set_sensitive(r->device, !window);
    gtk_widget_set_sensitive(r->pixel, !window);
    gtk_widget_set_sensitive(r->filter, !window);
    gtk_widget_set_sensitive(r->params, !window);
    gtk_widget_set_sensitive(r->no_damage, !window);
    gtk_widget_set_sensitive(r->no_dmabuf, !window);
}

static void folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    Recorder *r = data;
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (folder) {
        gchar *path = g_file_get_path(folder);
        if (path) gtk_editable_set_text(GTK_EDITABLE(r->folder), path);
        g_free(path);
        g_object_unref(folder);
    }
    g_clear_error(&error);
}

static void browse_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose recording folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(r->window), NULL, folder_chosen, r);
    g_object_unref(dialog);
}

static void activated(GtkApplication *app, gpointer data) {
    Recorder *r = data;
    if (r->window) {
        gtk_window_present(GTK_WINDOW(r->window));
        return;
    }
    r->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(r->window), "Screen Recorder");
    gtk_window_set_default_size(GTK_WINDOW(r->window), 520, 650);
    g_signal_connect(r->window, "close-request", G_CALLBACK(close_requested), r);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_window_set_child(GTK_WINDOW(r->window), scroll);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(outer, 18);
    gtk_widget_set_margin_bottom(outer, 18);
    gtk_widget_set_margin_start(outer, 18);
    gtk_widget_set_margin_end(outer, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), outer);

    const gchar *const captures[][2] = {
        {"region", "Select region with slurp"},
        {"window", "Select a window"},
        {"output", "Entire display"}
    };
    r->capture = combo(captures, G_N_ELEMENTS(captures));
    gtk_box_append(GTK_BOX(outer), option_row("Capture", r->capture));
    r->output = output_combo();
    gtk_box_append(GTK_BOX(outer), option_row("Display", r->output));
    r->audio = audio_combo();
    gtk_box_append(GTK_BOX(outer), option_row("Audio", r->audio));
    const gchar *const formats[][2] = {{"mp4", "MP4"}, {"mkv", "Matroska"}, {"webm", "WebM"}};
    r->format = combo(formats, G_N_ELEMENTS(formats));
    gtk_box_append(GTK_BOX(outer), option_row("Format", r->format));
    r->codec = gtk_combo_box_text_new();
    gtk_box_append(GTK_BOX(outer), option_row("Video codec", r->codec));
    r->fps = gtk_spin_button_new_with_range(1, 240, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->fps), 30);
    gtk_box_append(GTK_BOX(outer), option_row("Frame rate", r->fps));
    r->crf = gtk_spin_button_new_with_range(0, 51, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->crf), 18);
    gtk_box_append(GTK_BOX(outer), option_row("CRF (lower = higher quality)", r->crf));
    const gchar *const presets[][2] = {
        {"ultrafast", "Ultrafast"}, {"veryfast", "Very fast"}, {"fast", "Fast"},
        {"medium", "Medium"}, {"slow", "Slow"}, {"veryslow", "Very slow"}
    };
    r->preset = combo(presets, G_N_ELEMENTS(presets));
    gtk_combo_box_set_active(GTK_COMBO_BOX(r->preset), 3);
    gtk_box_append(GTK_BOX(outer), option_row("Encoding preset", r->preset));
    g_signal_connect(r->codec, "changed", G_CALLBACK(codec_changed), r);
    g_signal_connect(r->format, "changed", G_CALLBACK(format_changed), r);
    format_changed(NULL, r);
    GtkWidget *folder_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    r->folder = gtk_entry_new();
    gchar *videos = g_build_filename(g_get_home_dir(), "Videos", NULL);
    gtk_editable_set_text(GTK_EDITABLE(r->folder), videos);
    g_free(videos);
    gtk_widget_set_hexpand(r->folder, TRUE);
    gtk_box_append(GTK_BOX(folder_box), r->folder);
    GtkWidget *browse = gtk_button_new_with_label("Browse");
    g_signal_connect(browse, "clicked", G_CALLBACK(browse_clicked), r);
    gtk_box_append(GTK_BOX(folder_box), browse);
    gtk_box_append(GTK_BOX(outer), option_row("Save folder", folder_box));

    GtkWidget *advanced = gtk_expander_new("Advanced encoding options");
    gtk_box_append(GTK_BOX(outer), advanced);
    GtkWidget *advanced_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(advanced_box, 10);
    gtk_expander_set_child(GTK_EXPANDER(advanced), advanced_box);
    r->device = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->device), "/dev/dri/renderD128");
    gtk_box_append(GTK_BOX(advanced_box), option_row("Encoder device", r->device));
    r->pixel = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->pixel), "Optional pixel format");
    gtk_box_append(GTK_BOX(advanced_box), option_row("Pixel format", r->pixel));
    r->filter = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->filter), "Optional FFmpeg filter");
    gtk_box_append(GTK_BOX(advanced_box), option_row("Video filter", r->filter));
    r->params = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->params), "key=value; key=value");
    gtk_box_append(GTK_BOX(advanced_box), option_row("Extra codec parameters", r->params));
    r->no_damage = gtk_check_button_new_with_label("Record unchanged frames (--no-damage)");
    gtk_box_append(GTK_BOX(advanced_box), r->no_damage);
    r->no_dmabuf = gtk_check_button_new_with_label("Disable DMA-BUF copying (--no-dmabuf)");
    gtk_box_append(GTK_BOX(advanced_box), r->no_dmabuf);
    g_signal_connect(r->capture, "changed", G_CALLBACK(capture_changed), r);
    capture_changed(NULL, r);

    r->status = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(r->status), 0);
    gtk_label_set_wrap(GTK_LABEL(r->status), TRUE);
    gtk_label_set_selectable(GTK_LABEL(r->status), TRUE);
    gtk_box_append(GTK_BOX(outer), r->status);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(outer), buttons);
    r->start = gtk_button_new_with_label("Start recording");
    gtk_widget_add_css_class(r->start, "suggested-action");
    g_signal_connect(r->start, "clicked", G_CALLBACK(start_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->start);
    r->stop = gtk_button_new_with_label("Stop and save");
    gtk_widget_set_sensitive(r->stop, FALSE);
    g_signal_connect(r->stop, "clicked", G_CALLBACK(stop_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->stop);
    gtk_window_present(GTK_WINDOW(r->window));
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    Recorder recorder = {0};
    GtkApplication *app = gtk_application_new("local.wf_recorder_control", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activated), &recorder);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_clear_pointer(&recorder.filename, g_free);
    g_object_unref(app);
    return status;
}
