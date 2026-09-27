#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include "window_capture.h"
#include "encoding.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *capture, *output, *audio, *format, *codec, *quality, *preset;
    GtkWidget *fps, *crf, *bitrate, *folder, *device, *pixel, *filter, *params;
    GtkWidget *no_damage, *no_dmabuf, *status, *timer, *start, *stop;
    GtkWidget *controls, *open_video, *open_folder, *advanced;
    GSubprocess *recording;
    WindowCapture *window_capture;
    gboolean selecting;
    gchar *filename;
    gint64 started_us;
    guint timer_source;
} Recorder;

static void set_status(Recorder *r, const gchar *message) {
    gtk_label_set_text(GTK_LABEL(r->status), message);
}

static gboolean update_timer(gpointer data) {
    Recorder *r = data;
    gint64 seconds = (g_get_monotonic_time() - r->started_us) / G_USEC_PER_SEC;
    gchar *text = g_strdup_printf("● REC  %02ld:%02ld:%02ld", (long)(seconds / 3600),
                                  (long)((seconds / 60) % 60), (long)(seconds % 60));
    gtk_label_set_text(GTK_LABEL(r->timer), text);
    g_free(text);
    return G_SOURCE_CONTINUE;
}

static void recording_state(Recorder *r, gboolean active) {
    gtk_widget_set_sensitive(r->controls, !active);
    gtk_widget_set_sensitive(r->start, !active);
    gtk_widget_set_visible(r->start, !active);
    gtk_widget_set_visible(r->stop, active);
    gtk_widget_set_sensitive(r->stop, active);
    gtk_widget_set_visible(r->timer, active);
    if (r->timer_source) {
        g_source_remove(r->timer_source);
        r->timer_source = 0;
    }
    if (active) {
        r->started_us = g_get_monotonic_time();
        update_timer(r);
        r->timer_source = g_timeout_add_seconds(1, update_timer, r);
    }
}

static void reveal_saved_file(Recorder *r, gboolean saved) {
    gtk_widget_set_visible(r->open_video, saved);
    gtk_widget_set_visible(r->open_folder, saved);
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

static GtkWidget *section(GtkWidget *parent, const gchar *title) {
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(parent), group);
    GtkWidget *heading = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "section-heading");
    gtk_box_append(GTK_BOX(group), heading);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 11);
    gtk_widget_add_css_class(card, "section-card");
    gtk_box_append(GTK_BOX(group), card);
    return card;
}

static void install_style(void) {
    const gchar *css =
        ".section-heading { font-size: 1.08em; font-weight: 700; margin-left: 4px; }"
        ".section-card { background: alpha(@theme_fg_color, 0.045); border: 1px solid alpha(@theme_fg_color, 0.10); border-radius: 12px; padding: 14px; }"
        ".recorder-footer { border-top: 1px solid alpha(@theme_fg_color, 0.12); padding: 12px 18px; }"
        ".record-timer { color: #e5484d; font-weight: 800; font-feature-settings: 'tnum'; }"
        ".recorder-title { font-size: 1.4em; font-weight: 750; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
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

static gchar *settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "wf-recorder-control", "settings.ini", NULL);
}

static void save_settings(Recorder *r) {
    GKeyFile *key = g_key_file_new();
    const struct { const gchar *name; GtkWidget *widget; } combos[] = {
        {"capture", r->capture}, {"output", r->output}, {"audio", r->audio},
        {"format", r->format}, {"codec", r->codec}, {"quality", r->quality},
        {"preset", r->preset}
    };
    for (guint i = 0; i < G_N_ELEMENTS(combos); i++)
        g_key_file_set_string(key, "recording", combos[i].name, selected(combos[i].widget));
    const struct { const gchar *name; GtkWidget *widget; } entries[] = {
        {"folder", r->folder}, {"device", r->device}, {"pixel", r->pixel},
        {"filter", r->filter}, {"params", r->params}
    };
    for (guint i = 0; i < G_N_ELEMENTS(entries); i++)
        g_key_file_set_string(key, "recording", entries[i].name,
                              gtk_editable_get_text(GTK_EDITABLE(entries[i].widget)));
    g_key_file_set_integer(key, "recording", "fps", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->fps)));
    g_key_file_set_integer(key, "recording", "custom_quality", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->crf)));
    g_key_file_set_integer(key, "recording", "bitrate", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->bitrate)));
    g_key_file_set_boolean(key, "recording", "no_damage", gtk_check_button_get_active(GTK_CHECK_BUTTON(r->no_damage)));
    g_key_file_set_boolean(key, "recording", "no_dmabuf", gtk_check_button_get_active(GTK_CHECK_BUTTON(r->no_dmabuf)));
    gchar *path = settings_path();
    gchar *directory = g_path_get_dirname(path);
    GError *error = NULL;
    if (g_mkdir_with_parents(directory, 0700) != 0 || !g_key_file_save_to_file(key, path, &error))
        g_warning("Could not save recorder settings: %s", error ? error->message : g_strerror(errno));
    g_clear_error(&error);
    g_free(directory);
    g_free(path);
    g_key_file_unref(key);
}

static void restore_combo(GKeyFile *key, const gchar *name, GtkWidget *widget) {
    gchar *value = g_key_file_get_string(key, "recording", name, NULL);
    if (value) gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), value);
    g_free(value);
}

static void restore_spin(GKeyFile *key, const gchar *name, GtkWidget *widget) {
    if (g_key_file_has_key(key, "recording", name, NULL))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), g_key_file_get_integer(key, "recording", name, NULL));
}

static void load_settings(Recorder *r) {
    gchar *path = settings_path();
    GKeyFile *key = g_key_file_new();
    if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL)) goto done;
    restore_combo(key, "capture", r->capture);
    restore_combo(key, "output", r->output);
    restore_combo(key, "audio", r->audio);
    restore_combo(key, "format", r->format);
    restore_combo(key, "codec", r->codec);
    restore_combo(key, "quality", r->quality);
    restore_combo(key, "preset", r->preset);
    const struct { const gchar *name; GtkWidget *widget; } entries[] = {
        {"folder", r->folder}, {"device", r->device}, {"pixel", r->pixel},
        {"filter", r->filter}, {"params", r->params}
    };
    for (guint i = 0; i < G_N_ELEMENTS(entries); i++) {
        gchar *value = g_key_file_get_string(key, "recording", entries[i].name, NULL);
        if (value) gtk_editable_set_text(GTK_EDITABLE(entries[i].widget), value);
        g_free(value);
    }
    restore_spin(key, "fps", r->fps);
    restore_spin(key, "custom_quality", r->crf);
    restore_spin(key, "bitrate", r->bitrate);
    if (g_key_file_has_key(key, "recording", "no_damage", NULL))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r->no_damage), g_key_file_get_boolean(key, "recording", "no_damage", NULL));
    if (g_key_file_has_key(key, "recording", "no_dmabuf", NULL))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r->no_dmabuf), g_key_file_get_boolean(key, "recording", "no_dmabuf", NULL));
done:
    g_key_file_unref(key);
    g_free(path);
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
    if (!encoding_allowed(format, codec)) {
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
    const CodecInfo *info = encoding_find(codec);
    gint quantizer = encoding_quantizer(info, selected(r->quality),
                                       gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->crf)));
    gint bitrate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->bitrate));
    if (info->hardware) {
        if (bitrate > 0) {
            add_pair(args, "-p", "rc_mode=VBR");
            add_pair_owned(args, "-p", g_strdup_printf("b=%dk", bitrate));
        } else {
            add_pair(args, "-p", "rc_mode=CQP");
            add_pair_owned(args, "-p", g_strdup_printf("qp=%d", quantizer));
        }
    } else if (!g_strcmp0(codec, "libx264") || !g_strcmp0(codec, "libx265")) {
        add_pair_owned(args, "-p", g_strdup_printf("crf=%d", quantizer));
        add_pair_owned(args, "-p", g_strdup_printf("preset=%s", selected(r->preset)));
    } else if (!g_strcmp0(codec, "libvpx-vp9") || !g_strcmp0(codec, "libaom-av1")) {
        add_pair_owned(args, "-p", g_strdup_printf("crf=%d", quantizer));
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
    recording_state(r, FALSE);
    GStatBuf info;
    gboolean saved = r->filename && g_stat(r->filename, &info) == 0 && info.st_size > 0;
    reveal_saved_file(r, saved);
    if (saved) {
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
        recording_state(r, TRUE);
        gchar *message = g_strdup_printf("Recording selected window to %s", r->filename);
        set_status(r, message);
        g_free(message);
        return;
    }
    r->window_capture = NULL;
    recording_state(r, FALSE);
    GStatBuf info;
    gboolean saved = !error && r->filename && g_stat(r->filename, &info) == 0 && info.st_size > 0;
    reveal_saved_file(r, saved);
    if (saved) {
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
        gtk_widget_set_sensitive(r->controls, TRUE);
        return;
    }
    if (g_mkdir_with_parents(folder, 0755) != 0) {
        gchar *message = g_strdup_printf("Cannot create save folder: %s", g_strerror(errno));
        set_status(r, message);
        g_free(message);
        gtk_widget_set_sensitive(r->start, TRUE);
        gtk_widget_set_sensitive(r->controls, TRUE);
        return;
    }
    g_clear_pointer(&r->filename, g_free);
    r->filename = next_filename(folder, selected(r->format));
    r->selecting = TRUE;
    set_status(r, "Choose a window in the niri portal…");
    r->window_capture = window_capture_begin(GTK_WINDOW(r->window), r->filename,
        selected(r->format), selected(r->codec),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->fps)),
        encoding_quantizer(encoding_find(selected(r->codec)), selected(r->quality),
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->crf))),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->bitrate)),
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
    recording_state(r, TRUE);
    gchar *message = g_strdup_printf("Recording to %s", r->filename);
    set_status(r, message);
    g_free(message);
    g_subprocess_communicate_utf8_async(r->recording, NULL, NULL, recording_finished, r);
    return;
failed:
    set_status(r, error ? error->message : "Could not start recording");
    gtk_widget_set_sensitive(r->start, TRUE);
    gtk_widget_set_sensitive(r->controls, TRUE);
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
        gtk_widget_set_sensitive(r->controls, TRUE);
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
        gtk_widget_set_sensitive(r->controls, TRUE);
        g_clear_error(&error);
    } else g_subprocess_communicate_utf8_async(process, NULL, NULL, region_selected, r);
    return G_SOURCE_REMOVE;
}

static void start_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (r->recording || r->window_capture || r->selecting) return;
    if (!*selected(r->codec)) {
        set_status(r, "No encoder is available for this format and capture mode");
        return;
    }
    save_settings(r);
    gtk_widget_set_sensitive(r->start, FALSE);
    gtk_widget_set_sensitive(r->controls, FALSE);
    reveal_saved_file(r, FALSE);
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

static gboolean shortcut_pressed(GtkEventControllerKey *controller, guint keyval,
                                 guint keycode, GdkModifierType state, gpointer data) {
    Recorder *r = data;
    (void)controller;
    (void)keycode;
    if (keyval != GDK_KEY_r && keyval != GDK_KEY_R) return FALSE;
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    if (state & GDK_SHIFT_MASK) {
        if (r->recording || (r->window_capture && !r->selecting)) stop_clicked(NULL, r);
    } else if (!r->recording && !r->window_capture && !r->selecting) start_clicked(NULL, r);
    return TRUE;
}

static gboolean close_requested(GtkWindow *window, gpointer data) {
    Recorder *r = data;
    (void)window;
    if (!r->recording && !r->window_capture && !r->selecting) {
        save_settings(r);
        return FALSE;
    }
    set_status(r, "Stop and save before closing");
    return TRUE;
}

static void codec_changed(GtkComboBox *widget, gpointer data) {
    Recorder *r = data;
    (void)widget;
    const gchar *codec = selected(r->codec);
    gboolean x26x = !g_strcmp0(codec, "libx264") || !g_strcmp0(codec, "libx265");
    gtk_widget_set_sensitive(r->crf, !g_strcmp0(selected(r->quality), "custom"));
    gtk_widget_set_sensitive(r->bitrate, encoding_find(codec) && encoding_find(codec)->hardware);
    gtk_widget_set_sensitive(r->preset, x26x);
}

static void quality_changed(GtkComboBox *widget, gpointer data) {
    Recorder *r = data;
    (void)widget;
    gtk_widget_set_sensitive(r->crf, !g_strcmp0(selected(r->quality), "custom"));
    if (!g_strcmp0(selected(r->quality), "custom") && r->advanced)
        gtk_expander_set_expanded(GTK_EXPANDER(r->advanced), TRUE);
}

static void format_changed(GtkComboBox *widget, gpointer data) {
    Recorder *r = data;
    (void)widget;
    const gchar *format = selected(r->format);
    gchar *previous = g_strdup(selected(r->codec));
    gsize count = 0;
    const CodecInfo *codecs = encoding_codecs(&count);
    gboolean window = !g_strcmp0(selected(r->capture), "window");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(r->codec));
    for (gsize i = 0; i < count; i++)
        if (encoding_allowed(format, codecs[i].id) && encoding_available(&codecs[i], window))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(r->codec), codecs[i].id, codecs[i].label);
    if (encoding_allowed(format, previous) && encoding_available(encoding_find(previous), window))
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
    format_changed(NULL, r);
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

static void video_opened(GObject *source, GAsyncResult *result, gpointer data) {
    Recorder *r = data;
    GError *error = NULL;
    gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error);
    if (error) set_status(r, error->message);
    g_clear_error(&error);
}

static void folder_opened(GObject *source, GAsyncResult *result, gpointer data) {
    Recorder *r = data;
    GError *error = NULL;
    gtk_file_launcher_open_containing_folder_finish(GTK_FILE_LAUNCHER(source), result, &error);
    if (error) set_status(r, error->message);
    g_clear_error(&error);
}

static void open_video_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (!r->filename) return;
    GFile *file = g_file_new_for_path(r->filename);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(r->window), NULL, video_opened, r);
    g_object_unref(launcher);
    g_object_unref(file);
}

static void open_folder_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (!r->filename) return;
    GFile *file = g_file_new_for_path(r->filename);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(r->window), NULL, folder_opened, r);
    g_object_unref(launcher);
    g_object_unref(file);
}

static void activated(GtkApplication *app, gpointer data) {
    Recorder *r = data;
    if (r->window) {
        gtk_window_present(GTK_WINDOW(r->window));
        return;
    }
    install_style();
    r->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(r->window), "Screen Recorder");
    gtk_window_set_default_size(GTK_WINDOW(r->window), 540, 680);
    g_signal_connect(r->window, "close-request", G_CALLBACK(close_requested), r);
    GtkEventController *shortcuts = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(shortcuts, GTK_PHASE_CAPTURE);
    g_signal_connect(shortcuts, "key-pressed", G_CALLBACK(shortcut_pressed), r);
    gtk_widget_add_controller(r->window, shortcuts);
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(r->window), header);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(r->window), root);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    r->controls = scroll;
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(outer, 18);
    gtk_widget_set_margin_bottom(outer, 24);
    gtk_widget_set_margin_start(outer, 18);
    gtk_widget_set_margin_end(outer, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), outer);
    GtkWidget *title = gtk_label_new("Screen recorder");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(title, "recorder-title");
    gtk_box_append(GTK_BOX(outer), title);
    GtkWidget *capture_group = section(outer, "Capture");

    const gchar *const captures[][2] = {
        {"region", "Select region with slurp"},
        {"window", "Select a window"},
        {"output", "Entire display"}
    };
    r->capture = combo(captures, G_N_ELEMENTS(captures));
    gtk_box_append(GTK_BOX(capture_group), option_row("Source", r->capture));
    r->output = output_combo();
    gtk_box_append(GTK_BOX(capture_group), option_row("Display", r->output));
    GtkWidget *video_group = section(outer, "Video");
    r->audio = audio_combo();
    const gchar *const formats[][2] = {{"mp4", "MP4"}, {"mkv", "Matroska"}, {"webm", "WebM"}};
    r->format = combo(formats, G_N_ELEMENTS(formats));
    gtk_box_append(GTK_BOX(video_group), option_row("Format", r->format));
    r->codec = gtk_combo_box_text_new();
    gtk_box_append(GTK_BOX(video_group), option_row("Codec", r->codec));
    r->fps = gtk_spin_button_new_with_range(1, 240, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->fps), 30);
    gtk_box_append(GTK_BOX(video_group), option_row("Frame rate", r->fps));
    const gchar *const qualities[][2] = {
        {"high", "High quality"}, {"balanced", "Balanced"},
        {"compact", "Smaller file"}, {"custom", "Custom"}
    };
    r->quality = combo(qualities, G_N_ELEMENTS(qualities));
    gtk_box_append(GTK_BOX(video_group), option_row("Quality", r->quality));
    r->crf = gtk_spin_button_new_with_range(0, 51, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->crf), 18);
    r->bitrate = gtk_spin_button_new_with_range(0, 100000, 500);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->bitrate), 0);
    gtk_widget_set_tooltip_text(r->bitrate, "Hardware codecs: 0 uses constant quality; a value uses target bitrate in kb/s");
    const gchar *const presets[][2] = {
        {"ultrafast", "Ultrafast"}, {"veryfast", "Very fast"}, {"fast", "Fast"},
        {"medium", "Medium"}, {"slow", "Slow"}, {"veryslow", "Very slow"}
    };
    r->preset = combo(presets, G_N_ELEMENTS(presets));
    gtk_combo_box_set_active(GTK_COMBO_BOX(r->preset), 3);
    g_signal_connect(r->codec, "changed", G_CALLBACK(codec_changed), r);
    g_signal_connect(r->quality, "changed", G_CALLBACK(quality_changed), r);
    g_signal_connect(r->format, "changed", G_CALLBACK(format_changed), r);
    format_changed(NULL, r);
    GtkWidget *audio_group = section(outer, "Audio");
    gtk_box_append(GTK_BOX(audio_group), option_row("Source", r->audio));
    GtkWidget *output_group = section(outer, "Save");
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
    gtk_box_append(GTK_BOX(output_group), option_row("Folder", folder_box));

    GtkWidget *advanced = gtk_expander_new("Advanced encoding options");
    r->advanced = advanced;
    gtk_box_append(GTK_BOX(video_group), advanced);
    GtkWidget *advanced_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(advanced_box, 10);
    gtk_expander_set_child(GTK_EXPANDER(advanced), advanced_box);
    gtk_box_append(GTK_BOX(advanced_box), option_row("Custom quality value", r->crf));
    gtk_box_append(GTK_BOX(advanced_box), option_row("Hardware bitrate (kb/s)", r->bitrate));
    gtk_box_append(GTK_BOX(advanced_box), option_row("Encoding preset", r->preset));
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
    load_settings(r);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(footer, "recorder-footer");
    gtk_box_append(GTK_BOX(root), footer);
    GtkWidget *state_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(footer), state_line);
    r->timer = gtk_label_new("");
    gtk_widget_add_css_class(r->timer, "record-timer");
    gtk_widget_set_visible(r->timer, FALSE);
    gtk_box_append(GTK_BOX(state_line), r->timer);
    r->status = gtk_label_new("Ready to record");
    gtk_label_set_xalign(GTK_LABEL(r->status), 0);
    gtk_label_set_wrap(GTK_LABEL(r->status), TRUE);
    gtk_label_set_selectable(GTK_LABEL(r->status), TRUE);
    gtk_widget_set_hexpand(r->status, TRUE);
    gtk_box_append(GTK_BOX(state_line), r->status);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(footer), buttons);
    r->start = gtk_button_new_with_label("Start recording");
    gtk_widget_add_css_class(r->start, "suggested-action");
    g_signal_connect(r->start, "clicked", G_CALLBACK(start_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->start);
    r->stop = gtk_button_new_with_label("Stop and save");
    gtk_widget_add_css_class(r->stop, "destructive-action");
    gtk_widget_set_visible(r->stop, FALSE);
    g_signal_connect(r->stop, "clicked", G_CALLBACK(stop_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->stop);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(buttons), spacer);
    r->open_video = gtk_button_new_with_label("Open video");
    gtk_widget_set_visible(r->open_video, FALSE);
    g_signal_connect(r->open_video, "clicked", G_CALLBACK(open_video_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->open_video);
    r->open_folder = gtk_button_new_with_label("Open folder");
    gtk_widget_set_visible(r->open_folder, FALSE);
    g_signal_connect(r->open_folder, "clicked", G_CALLBACK(open_folder_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->open_folder);
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
