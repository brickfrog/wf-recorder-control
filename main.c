#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include "window_capture.h"
#include "encoding.h"

#define COUNTDOWN_SECONDS 3

typedef struct {
    GtkWidget *window;
    GtkWidget *output, *audio, *format, *codec, *quality, *preset;
    GtkWidget *fps, *crf, *bitrate, *folder, *device, *pixel, *filter, *params;
    GtkWidget *no_damage, *no_dmabuf, *status, *timer, *rec_dot, *start, *stop, *discard, *live_badge;
    GtkWidget *controls, *pages, *advanced, *capture_buttons[3];
    GtkWidget *preview, *preview_placeholder, *live_target, *live_size, *live_rate, *level, *level_row;
    GtkWidget *last_card, *last_video, *last_name, *last_meta, *countdown, *countdown_label;
    const gchar *capture_mode;
    GSubprocess *recording;
    WindowCapture *window_capture;
    gboolean selecting, counting, discarding, finalizing;
    gchar *filename, *geometry;
    gint64 started_us;
    guint timer_source, preview_source, meter_watch, live_generation, countdown_source, countdown_left;
    gboolean grim_busy;
    GstElement *meter;
} Recorder;

static const struct { const gchar *id, *label, *icon; } capture_modes[] = {
    {"region", "Region", "edit-select-all-symbolic"},
    {"window", "Window", "focus-windows-symbolic"},
    {"output", "Display", "video-display-symbolic"},
};

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
        ".rec-dot { min-width: 10px; min-height: 10px; border-radius: 999px; background: #e5484d; animation: rec-pulse 1.4s ease-in-out infinite; }"
        "@keyframes rec-pulse { 0% { opacity: 1; } 50% { opacity: 0.2; } 100% { opacity: 1; } }"
        ".capture-tile { padding: 10px 6px; border-radius: 10px; }"
        ".capture-tile:checked { background: alpha(@theme_selected_bg_color, 0.22); box-shadow: inset 0 0 0 1px @theme_selected_bg_color; color: @theme_fg_color; }"
        ".preview-frame { background: #111; border-radius: 10px; }"
        ".live-badge { background: #e5484d; color: white; font-size: 0.75em; font-weight: 800; border-radius: 6px; padding: 2px 7px; margin: 10px; }"
        ".preview-placeholder { color: alpha(white, 0.6); }"
        ".stat-value { font-feature-settings: 'tnum'; }"
        ".last-video { border-radius: 10px; background: #111; }"
        ".countdown { font-size: 120px; font-weight: 800; color: white; background: alpha(black, 0.6); font-feature-settings: 'tnum'; }"
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
        name += 6;
        gchar *description = strstr(name, " Description: ");
        if (description) {
            *description = '\0';
            description += 14;
        }
        name = g_strstrip(name);
        if (*name) {
            gchar *label = description && *g_strstrip(description)
                ? g_strdup_printf("%s (%s)", name, description) : g_strdup(name);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), name, label);
            g_free(label);
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
        {"output", r->output}, {"audio", r->audio},
        {"format", r->format}, {"codec", r->codec}, {"quality", r->quality},
        {"preset", r->preset}
    };
    g_key_file_set_string(key, "recording", "capture", r->capture_mode);
    for (guint i = 0; i < G_N_ELEMENTS(combos); i++)
        g_key_file_set_string(key, "recording", combos[i].name, selected(combos[i].widget));
    const struct { const gchar *name; GtkWidget *widget; } entries[] = {
        {"folder", r->folder}, {"device", r->device}, {"pixel", r->pixel},
        {"filter", r->filter}, {"params", r->params}
    };
    g_key_file_set_boolean(key, "recording", "countdown", gtk_check_button_get_active(GTK_CHECK_BUTTON(r->countdown)));
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

static void set_capture_mode(Recorder *r, const gchar *id) {
    for (guint i = 0; i < G_N_ELEMENTS(capture_modes); i++)
        if (!g_strcmp0(capture_modes[i].id, id))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->capture_buttons[i]), TRUE);
}

static void load_settings(Recorder *r) {
    gchar *path = settings_path();
    GKeyFile *key = g_key_file_new();
    if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL)) goto done;
    gchar *capture = g_key_file_get_string(key, "recording", "capture", NULL);
    if (capture) set_capture_mode(r, capture);
    g_free(capture);
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
    if (g_key_file_has_key(key, "recording", "countdown", NULL))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r->countdown), g_key_file_get_boolean(key, "recording", "countdown", NULL));
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

static gchar *format_rate(gdouble bits_per_second) {
    if (bits_per_second >= 1e6) return g_strdup_printf("%.1f Mb/s", bits_per_second / 1e6);
    return g_strdup_printf("%.0f kb/s", bits_per_second / 1e3);
}

static gboolean update_timer(gpointer data) {
    Recorder *r = data;
    gint64 elapsed_us = g_get_monotonic_time() - r->started_us;
    gint64 seconds = elapsed_us / G_USEC_PER_SEC;
    gchar *text = g_strdup_printf("REC  %02ld:%02ld:%02ld", (long)(seconds / 3600),
                                  (long)((seconds / 60) % 60), (long)(seconds % 60));
    gtk_label_set_text(GTK_LABEL(r->timer), text);
    g_free(text);
    GStatBuf info;
    if (r->filename && g_stat(r->filename, &info) == 0) {
        gchar *size = g_format_size(info.st_size);
        gtk_label_set_text(GTK_LABEL(r->live_size), size);
        g_free(size);
        if (elapsed_us > G_USEC_PER_SEC) {
            gchar *rate = format_rate(info.st_size * 8.0 * G_USEC_PER_SEC / elapsed_us);
            gtk_label_set_text(GTK_LABEL(r->live_rate), rate);
            g_free(rate);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void show_preview_frame(Recorder *r, GdkTexture *frame) {
    gtk_picture_set_paintable(GTK_PICTURE(r->preview), GDK_PAINTABLE(frame));
    gtk_widget_set_visible(r->preview_placeholder, frame == NULL);
}

typedef struct {
    Recorder *r;
    guint generation;
} GrimRequest;

static void grim_finished(GObject *source, GAsyncResult *result, gpointer data) {
    GrimRequest *request = data;
    Recorder *r = request->r;
    GBytes *image = NULL;
    g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &image, NULL, NULL);
    r->grim_busy = FALSE;
    if (request->generation == r->live_generation && r->preview_source && image &&
        g_subprocess_get_successful(G_SUBPROCESS(source))) {
        GdkTexture *frame = gdk_texture_new_from_bytes(image, NULL);
        if (frame) {
            show_preview_frame(r, frame);
            g_object_unref(frame);
        }
    }
    if (image) g_bytes_unref(image);
    g_object_unref(source);
    g_free(request);
}

static void request_grim_frame(Recorder *r) {
    if (r->grim_busy) return;
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    add_arg(args, "grim");
    add_pair(args, "-l", "0");
    if (r->geometry) {
        gint x = 0, y = 0, width = 0, height = 0;
        gdouble scale = 0.35;
        if (sscanf(r->geometry, "%d,%d %dx%d", &x, &y, &width, &height) == 4 && width > 0)
            scale = MIN(1.0, (gdouble)WINDOW_CAPTURE_PREVIEW_WIDTH / width);
        add_pair_owned(args, "-s", g_strdup_printf("%.3f", scale));
        add_pair(args, "-g", r->geometry);
    } else {
        add_pair(args, "-s", "0.35");
        add_pair(args, "-o", selected(r->output));
    }
    add_arg(args, "-");
    g_ptr_array_add(args, NULL);
    GSubprocess *process = g_subprocess_newv((const gchar *const *)args->pdata,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_ptr_array_free(args, TRUE);
    if (!process) return;
    r->grim_busy = TRUE;
    GrimRequest *request = g_new(GrimRequest, 1);
    request->r = r;
    request->generation = r->live_generation;
    g_subprocess_communicate_async(process, NULL, NULL, grim_finished, request);
}

static gboolean update_preview(gpointer data) {
    Recorder *r = data;
    if (r->window_capture) {
        GdkTexture *frame = window_capture_preview(r->window_capture);
        if (frame) {
            show_preview_frame(r, frame);
            g_object_unref(frame);
        }
    } else request_grim_frame(r);
    return G_SOURCE_CONTINUE;
}

static gboolean meter_message(GstBus *bus, GstMessage *message, gpointer data) {
    Recorder *r = data;
    (void)bus;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        gtk_widget_set_visible(r->level_row, FALSE);
        r->meter_watch = 0;
        return G_SOURCE_REMOVE;
    }
    const GstStructure *structure = gst_message_get_structure(message);
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT || !gst_structure_has_name(structure, "level"))
        return G_SOURCE_CONTINUE;
    const GValue *peaks = gst_structure_get_value(structure, "peak");
    GValueArray *channels = peaks ? g_value_get_boxed(peaks) : NULL;
    gdouble peak = -100;
    for (guint i = 0; channels && i < channels->n_values; i++)
        peak = MAX(peak, g_value_get_double(g_value_array_get_nth(channels, i)));
    gtk_level_bar_set_value(GTK_LEVEL_BAR(r->level), CLAMP((peak + 60) / 60, 0, 1));
    return G_SOURCE_CONTINUE;
}

static void start_meter(Recorder *r) {
    const gchar *audio = selected(r->audio);
    gboolean enabled = g_strcmp0(audio, "none") != 0;
    gtk_widget_set_visible(r->level_row, enabled);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(r->level), 0);
    if (!enabled) return;
    r->meter = gst_parse_launch("pulsesrc name=source client-name=\"Screen Recorder meter\" ! "
                                "level interval=50000000 post-messages=true ! fakesink sync=false", NULL);
    if (!r->meter) {
        gtk_widget_set_visible(r->level_row, FALSE);
        return;
    }
    if (g_strcmp0(audio, "default")) {
        GstElement *source = gst_bin_get_by_name(GST_BIN(r->meter), "source");
        g_object_set(source, "device", audio, NULL);
        gst_object_unref(source);
    }
    GstBus *bus = gst_element_get_bus(r->meter);
    r->meter_watch = gst_bus_add_watch(bus, meter_message, r);
    gst_object_unref(bus);
    if (gst_element_set_state(r->meter, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        gtk_widget_set_visible(r->level_row, FALSE);
}

static void stop_live(Recorder *r) {
    r->live_generation++;
    if (r->preview_source) {
        g_source_remove(r->preview_source);
        r->preview_source = 0;
    }
    if (r->meter_watch) {
        g_source_remove(r->meter_watch);
        r->meter_watch = 0;
    }
    if (r->meter) {
        gst_element_set_state(r->meter, GST_STATE_NULL);
        g_clear_pointer(&r->meter, gst_object_unref);
    }
    g_clear_pointer(&r->geometry, g_free);
}

static void start_live(Recorder *r) {
    show_preview_frame(r, NULL);
    gtk_label_set_text(GTK_LABEL(r->live_size), "0 bytes");
    gtk_label_set_text(GTK_LABEL(r->live_rate), "—");
    gchar *target = r->window_capture ? g_strdup("Selected window")
                  : r->geometry ? g_strdup_printf("Region %s", r->geometry)
                  : g_strdup_printf("Display %s", selected(r->output));
    gtk_label_set_text(GTK_LABEL(r->live_target), target);
    g_free(target);
    gboolean grim = !r->window_capture && g_find_program_in_path("grim") != NULL;
    gtk_label_set_text(GTK_LABEL(r->preview_placeholder),
        r->window_capture || grim ? "Waiting for the first frame…" : "Install grim to preview region and display recordings");
    if (r->window_capture) r->preview_source = g_timeout_add(66, update_preview, r);
    else if (grim) {
        r->preview_source = g_timeout_add(1000, update_preview, r);
        request_grim_frame(r);
    }
    start_meter(r);
}

static void recording_state(Recorder *r, gboolean active) {
    gtk_widget_set_sensitive(r->controls, !active);
    gtk_widget_set_sensitive(r->start, !active);
    gtk_widget_set_visible(r->start, !active);
    gtk_widget_set_visible(r->stop, active);
    gtk_widget_set_sensitive(r->stop, active);
    gtk_widget_set_visible(r->discard, active);
    gtk_widget_set_sensitive(r->discard, active);
    r->finalizing = FALSE;
    gtk_widget_set_visible(r->timer, active);
    gtk_widget_set_visible(r->rec_dot, active);
    gtk_widget_set_visible(r->live_badge, active);
    gtk_button_set_label(GTK_BUTTON(r->stop), "Stop and save");
    gtk_stack_set_visible_child_name(GTK_STACK(r->pages), active ? "live" : "settings");
    if (r->timer_source) {
        g_source_remove(r->timer_source);
        r->timer_source = 0;
    }
    if (active) {
        r->started_us = g_get_monotonic_time();
        update_timer(r);
        r->timer_source = g_timeout_add_seconds(1, update_timer, r);
        start_live(r);
    } else stop_live(r);
}

static void update_last_meta(Recorder *r) {
    GtkMediaStream *stream = gtk_video_get_media_stream(GTK_VIDEO(r->last_video));
    GString *meta = g_string_new(NULL);
    if (stream && gtk_media_stream_is_prepared(stream)) {
        gint64 seconds = gtk_media_stream_get_duration(stream) / G_USEC_PER_SEC;
        g_string_append_printf(meta, "%ld:%02ld · ", (long)(seconds / 60), (long)(seconds % 60));
        gint width = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(stream));
        gint height = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(stream));
        if (width > 0 && height > 0) g_string_append_printf(meta, "%d×%d · ", width, height);
    }
    GStatBuf info;
    if (r->filename && g_stat(r->filename, &info) == 0) {
        gchar *size = g_format_size(info.st_size);
        g_string_append(meta, size);
        g_free(size);
    }
    gtk_label_set_text(GTK_LABEL(r->last_meta), meta->str);
    g_string_free(meta, TRUE);
}

static void last_video_prepared(GObject *stream, GParamSpec *pspec, gpointer data) {
    Recorder *r = data;
    (void)pspec;
    if (stream == G_OBJECT(gtk_video_get_media_stream(GTK_VIDEO(r->last_video)))) update_last_meta(r);
}

static void reveal_saved_file(Recorder *r, gboolean saved) {
    gtk_widget_set_visible(r->last_card, saved);
    if (!saved) {
        gtk_video_set_file(GTK_VIDEO(r->last_video), NULL);
        return;
    }
    gtk_video_set_filename(GTK_VIDEO(r->last_video), r->filename);
    GtkMediaStream *stream = gtk_video_get_media_stream(GTK_VIDEO(r->last_video));
    if (stream) g_signal_connect(stream, "notify::prepared", G_CALLBACK(last_video_prepared), r);
    gchar *name = g_path_get_basename(r->filename);
    gtk_label_set_text(GTK_LABEL(r->last_name), name);
    g_free(name);
    update_last_meta(r);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(r->controls)), 0);
}

/* After a discarded capture ends: delete the partial file. Returns TRUE if it handled the end. */
static gboolean finish_discard(Recorder *r) {
    if (!r->discarding) return FALSE;
    r->discarding = FALSE;
    if (r->filename) g_unlink(r->filename);
    reveal_saved_file(r, FALSE);
    set_status(r, "Recording discarded");
    return TRUE;
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
    const gchar *capture = r->capture_mode;
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
    } else if (!g_strcmp0(codec, "libvpx-vp9")) {
        /* Default VP9/AV1 settings encode far slower than real time; wf-recorder then queues
         * frames in memory and Stop waits minutes while it drains them. */
        add_pair_owned(args, "-p", g_strdup_printf("crf=%d", quantizer));
        add_pair(args, "-p", "b=0");
        add_pair(args, "-p", "deadline=realtime");
        add_pair(args, "-p", "cpu-used=8");
        add_pair(args, "-p", "row-mt=1");
        add_pair(args, "-p", "threads=0");
    } else if (!g_strcmp0(codec, "libaom-av1")) {
        add_pair_owned(args, "-p", g_strdup_printf("crf=%d", quantizer));
        add_pair(args, "-p", "b=0");
        add_pair(args, "-p", "usage=realtime");
        add_pair(args, "-p", "cpu-used=8");
        add_pair(args, "-p", "row-mt=1");
        add_pair(args, "-p", "threads=0");
    }
    const struct { GtkWidget *widget; const gchar *flag; } optional[] = {
        {r->device, "-d"}, {r->pixel, "-x"}, {r->filter, "-F"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(optional); i++) {
        const gchar *value = gtk_editable_get_text(GTK_EDITABLE(optional[i].widget));
        if (*value) add_pair(args, optional[i].flag, value);
    }
    /* Without -x, wf-recorder feeds VP9/AV1 4:4:4 RGB (gbrp), which encodes several times slower
     * and plays in fewer players. */
    if (!*gtk_editable_get_text(GTK_EDITABLE(r->pixel)) && !info->hardware)
        add_pair(args, "-x", "yuv420p");
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
    if (finish_discard(r)) goto done;
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
done:
    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
}

static void begin_recording(Recorder *r);

static void window_event(WindowCaptureState state, const gchar *error, gpointer data) {
    Recorder *r = data;
    r->selecting = FALSE;
    if (state == WINDOW_CAPTURE_SELECTED) {
        begin_recording(r);
        return;
    }
    if (state == WINDOW_CAPTURE_STARTED) {
        recording_state(r, TRUE);
        gchar *message = g_strdup_printf("Recording selected window to %s", r->filename);
        set_status(r, message);
        g_free(message);
        return;
    }
    r->window_capture = NULL;
    recording_state(r, FALSE);
    if (finish_discard(r)) return;
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

static void launch_recording(Recorder *r) {
    GError *error = NULL;
    GPtrArray *args = build_command(r, r->geometry, &error);
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
    g_clear_pointer(&r->geometry, g_free);
    set_status(r, error ? error->message : "Could not start recording");
    gtk_widget_set_sensitive(r->start, TRUE);
    gtk_widget_set_sensitive(r->controls, TRUE);
    g_clear_error(&error);
}

static void countdown_state(Recorder *r, gboolean active) {
    r->counting = active;
    gtk_widget_set_visible(r->countdown_label, active);
    gtk_widget_set_visible(r->start, !active);
    gtk_widget_set_visible(r->stop, active);
    gtk_widget_set_sensitive(r->stop, active);
    gtk_button_set_label(GTK_BUTTON(r->stop), active ? "Cancel" : "Stop and save");
    if (r->countdown_source) {
        g_source_remove(r->countdown_source);
        r->countdown_source = 0;
    }
}

static void countdown_show(Recorder *r) {
    gchar *number = g_strdup_printf("%u", r->countdown_left);
    gtk_label_set_text(GTK_LABEL(r->countdown_label), number);
    g_free(number);
    gchar *message = g_strdup_printf("Recording starts in %u…", r->countdown_left);
    set_status(r, message);
    g_free(message);
}

static void start_now(Recorder *r) {
    if (!r->window_capture) {
        launch_recording(r);
        return;
    }
    window_capture_record(r->window_capture);
    if (!r->window_capture) return;
    /* Recording begins on the first window frame; until then Stop cancels. */
    gtk_widget_set_visible(r->start, FALSE);
    gtk_widget_set_visible(r->stop, TRUE);
    gtk_widget_set_sensitive(r->stop, TRUE);
    gtk_button_set_label(GTK_BUTTON(r->stop), "Cancel");
    set_status(r, "Waiting for the selected window to appear on screen…");
}

static gboolean countdown_tick(gpointer data) {
    Recorder *r = data;
    if (--r->countdown_left > 0) {
        countdown_show(r);
        return G_SOURCE_CONTINUE;
    }
    r->countdown_source = 0;
    countdown_state(r, FALSE);
    start_now(r);
    return G_SOURCE_REMOVE;
}

/* Starts the selected target, after the optional countdown. Region geometry is in r->geometry. */
static void begin_recording(Recorder *r) {
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(r->countdown))) {
        start_now(r);
        return;
    }
    countdown_state(r, TRUE);
    r->countdown_left = COUNTDOWN_SECONDS;
    countdown_show(r);
    r->countdown_source = g_timeout_add_seconds(1, countdown_tick, r);
}

static void cancel_countdown(Recorder *r) {
    countdown_state(r, FALSE);
    if (r->window_capture) {
        window_capture_stop(r->window_capture);
        return;
    }
    g_clear_pointer(&r->geometry, g_free);
    set_status(r, "Recording cancelled");
    gtk_widget_set_sensitive(r->start, TRUE);
    gtk_widget_set_sensitive(r->controls, TRUE);
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
    if (!error && g_subprocess_get_successful(G_SUBPROCESS(source)) && geometry && *g_strstrip(geometry)) {
        r->geometry = g_strdup(geometry);
        begin_recording(r);
    } else {
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
    if (r->recording || r->window_capture || r->selecting || r->counting) return;
    if (!*selected(r->codec)) {
        set_status(r, "No encoder is available for this format and capture mode");
        return;
    }
    save_settings(r);
    gtk_widget_set_sensitive(r->start, FALSE);
    gtk_widget_set_sensitive(r->controls, FALSE);
    reveal_saved_file(r, FALSE);
    if (!g_strcmp0(r->capture_mode, "window")) launch_window_capture(r);
    else if (!g_strcmp0(r->capture_mode, "region")) {
        r->selecting = TRUE;
        set_status(r, "Select a region…");
        gtk_widget_set_visible(r->window, FALSE);
        g_timeout_add(180, start_slurp, r);
    } else begin_recording(r);
}

/* Stop pressed: the capture is over, so freeze the live view while the encoder finishes. */
static void finalizing_state(Recorder *r) {
    r->finalizing = TRUE;
    gtk_widget_set_sensitive(r->stop, FALSE);
    if (r->timer_source) {
        g_source_remove(r->timer_source);
        r->timer_source = 0;
    }
    stop_live(r);
    gtk_widget_set_visible(r->rec_dot, FALSE);
    gtk_widget_set_visible(r->timer, FALSE);
    gtk_widget_set_visible(r->live_badge, FALSE);
    show_preview_frame(r, NULL);
    gtk_label_set_text(GTK_LABEL(r->preview_placeholder), "Saving the recording…");
    set_status(r, "Saving the recording… Press Discard to abort without saving.");
}

static void stop_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (r->finalizing) return;
    if (r->counting) cancel_countdown(r);
    else if (r->window_capture && !r->selecting && !window_capture_recording(r->window_capture))
        window_capture_stop(r->window_capture);
    else if (r->recording) {
        finalizing_state(r);
        g_subprocess_send_signal(r->recording, SIGINT);
    } else if (r->window_capture && !r->selecting) {
        finalizing_state(r);
        window_capture_stop(r->window_capture);
    }
}

/* Aborts recording or saving at once and deletes the partial file. */
static void discard_clicked(GtkButton *button, gpointer data) {
    Recorder *r = data;
    (void)button;
    if (r->counting) {
        cancel_countdown(r);
        return;
    }
    if (!r->recording && !r->window_capture) return;
    r->discarding = TRUE;
    gtk_widget_set_sensitive(r->discard, FALSE);
    gtk_widget_set_sensitive(r->stop, FALSE);
    set_status(r, "Discarding…");
    if (r->recording) g_subprocess_force_exit(r->recording);
    else window_capture_cancel(r->window_capture);
}

static gboolean shortcut_pressed(GtkEventControllerKey *controller, guint keyval,
                                 guint keycode, GdkModifierType state, gpointer data) {
    Recorder *r = data;
    (void)controller;
    (void)keycode;
    if (keyval != GDK_KEY_r && keyval != GDK_KEY_R) return FALSE;
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    if (state & GDK_SHIFT_MASK) {
        if (r->counting || r->recording || (r->window_capture && !r->selecting)) stop_clicked(NULL, r);
    } else if (!r->recording && !r->window_capture && !r->selecting && !r->counting) start_clicked(NULL, r);
    return TRUE;
}

static gboolean close_requested(GtkWindow *window, gpointer data) {
    Recorder *r = data;
    (void)window;
    if (!r->recording && !r->window_capture && !r->selecting && !r->counting) {
        save_settings(r);
        return FALSE;
    }
    set_status(r, "Stop and save, or discard, before closing");
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
    gboolean window = !g_strcmp0(r->capture_mode, "window");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(r->codec));
    for (gsize i = 0; i < count; i++)
        if (encoding_allowed(format, codecs[i].id) && encoding_available(&codecs[i], window))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(r->codec), codecs[i].id, codecs[i].label);
    if (encoding_allowed(format, previous) && encoding_available(encoding_find(previous), window))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(r->codec), previous);
    else gtk_combo_box_set_active(GTK_COMBO_BOX(r->codec), 0);
    g_free(previous);
}

static void capture_changed(Recorder *r) {
    gboolean window = !g_strcmp0(r->capture_mode, "window");
    gtk_widget_set_sensitive(r->output, !g_strcmp0(r->capture_mode, "output"));
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

static void capture_toggled(GtkToggleButton *button, gpointer data) {
    Recorder *r = data;
    if (!gtk_toggle_button_get_active(button)) return;
    r->capture_mode = g_object_get_data(G_OBJECT(button), "capture-mode");
    capture_changed(r);
}

static void dismiss_last_clicked(GtkButton *button, gpointer data) {
    (void)button;
    reveal_saved_file(data, FALSE);
}

static GtkWidget *stat_row(const gchar *label, GtkWidget **value) {
    *value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*value), 1);
    gtk_label_set_ellipsize(GTK_LABEL(*value), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(*value, "stat-value");
    return option_row(label, *value);
}

static GtkWidget *live_page(Recorder *r) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(page, 18);
    gtk_widget_set_margin_bottom(page, 18);
    gtk_widget_set_margin_start(page, 18);
    gtk_widget_set_margin_end(page, 18);
    GtkWidget *preview_card = section(page, "Live preview");
    gtk_widget_set_vexpand(gtk_widget_get_parent(preview_card), TRUE);
    gtk_widget_set_vexpand(preview_card, TRUE);
    GtkWidget *frame = gtk_overlay_new();
    gtk_widget_add_css_class(frame, "preview-frame");
    gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_vexpand(frame, TRUE);
    gtk_widget_set_size_request(frame, -1, 200);
    gtk_box_append(GTK_BOX(preview_card), frame);
    r->preview = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(r->preview), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(r->preview), TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(frame), r->preview);
    r->preview_placeholder = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(r->preview_placeholder), TRUE);
    gtk_label_set_justify(GTK_LABEL(r->preview_placeholder), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(r->preview_placeholder, "preview-placeholder");
    gtk_overlay_add_overlay(GTK_OVERLAY(frame), r->preview_placeholder);
    GtkWidget *badge = gtk_label_new("LIVE");
    r->live_badge = badge;
    gtk_widget_add_css_class(badge, "live-badge");
    gtk_widget_set_halign(badge, GTK_ALIGN_START);
    gtk_widget_set_valign(badge, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(frame), badge);
    GtkWidget *stats = section(page, "Recording");
    gtk_box_append(GTK_BOX(stats), stat_row("Target", &r->live_target));
    gtk_box_append(GTK_BOX(stats), stat_row("File size", &r->live_size));
    gtk_box_append(GTK_BOX(stats), stat_row("Average bitrate", &r->live_rate));
    r->level = gtk_level_bar_new_for_interval(0, 1);
    gtk_widget_set_valign(r->level, GTK_ALIGN_CENTER);
    r->level_row = option_row("Audio level", r->level);
    gtk_box_append(GTK_BOX(stats), r->level_row);
    return page;
}

static void last_recording_card(Recorder *r, GtkWidget *parent) {
    GtkWidget *card = section(parent, "Last recording");
    r->last_card = gtk_widget_get_parent(card);
    gtk_widget_set_visible(r->last_card, FALSE);
    r->last_video = gtk_video_new();
    gtk_video_set_autoplay(GTK_VIDEO(r->last_video), FALSE);
    gtk_widget_add_css_class(r->last_video, "last-video");
    gtk_widget_set_overflow(r->last_video, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(r->last_video, -1, 240);
    gtk_box_append(GTK_BOX(card), r->last_video);
    r->last_name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(r->last_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(r->last_name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(r->last_name, "heading");
    gtk_box_append(GTK_BOX(card), r->last_name);
    r->last_meta = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(r->last_meta), 0);
    gtk_widget_add_css_class(r->last_meta, "dim-label");
    gtk_widget_add_css_class(r->last_meta, "stat-value");
    gtk_box_append(GTK_BOX(card), r->last_meta);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(card), buttons);
    GtkWidget *open_video = gtk_button_new_with_label("Open video");
    g_signal_connect(open_video, "clicked", G_CALLBACK(open_video_clicked), r);
    gtk_box_append(GTK_BOX(buttons), open_video);
    GtkWidget *open_folder = gtk_button_new_with_label("Open folder");
    g_signal_connect(open_folder, "clicked", G_CALLBACK(open_folder_clicked), r);
    gtk_box_append(GTK_BOX(buttons), open_folder);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(buttons), spacer);
    GtkWidget *dismiss = gtk_button_new_with_label("Dismiss");
    gtk_widget_add_css_class(dismiss, "flat");
    g_signal_connect(dismiss, "clicked", G_CALLBACK(dismiss_last_clicked), r);
    gtk_box_append(GTK_BOX(buttons), dismiss);
}

static void capture_tiles(Recorder *r, GtkWidget *parent) {
    GtkWidget *tiles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(tiles), TRUE);
    gtk_box_append(GTK_BOX(parent), tiles);
    for (guint i = 0; i < G_N_ELEMENTS(capture_modes); i++) {
        GtkWidget *button = gtk_toggle_button_new();
        gtk_widget_add_css_class(button, "capture-tile");
        g_object_set_data(G_OBJECT(button), "capture-mode", (gpointer)capture_modes[i].id);
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *icon = gtk_image_new_from_icon_name(capture_modes[i].icon);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 28);
        gtk_box_append(GTK_BOX(content), icon);
        gtk_box_append(GTK_BOX(content), gtk_label_new(capture_modes[i].label));
        gtk_button_set_child(GTK_BUTTON(button), content);
        if (i) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(r->capture_buttons[0]));
        r->capture_buttons[i] = button;
        gtk_box_append(GTK_BOX(tiles), button);
    }
    r->capture_mode = capture_modes[0].id;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->capture_buttons[0]), TRUE);
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
    GtkWidget *page_overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(page_overlay, TRUE);
    gtk_box_append(GTK_BOX(root), page_overlay);
    r->pages = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(r->pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_overlay_set_child(GTK_OVERLAY(page_overlay), r->pages);
    r->countdown_label = gtk_label_new("");
    gtk_widget_add_css_class(r->countdown_label, "countdown");
    gtk_widget_set_visible(r->countdown_label, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(page_overlay), r->countdown_label);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_stack_add_named(GTK_STACK(r->pages), scroll, "settings");
    gtk_stack_add_named(GTK_STACK(r->pages), live_page(r), "live");
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
    last_recording_card(r, outer);
    GtkWidget *capture_group = section(outer, "Capture");
    capture_tiles(r, capture_group);
    r->output = output_combo();
    gtk_box_append(GTK_BOX(capture_group), option_row("Display", r->output));
    r->countdown = gtk_check_button_new_with_label("Count down 3 seconds before recording");
    gtk_box_append(GTK_BOX(capture_group), r->countdown);
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
    gtk_combo_box_set_active(GTK_COMBO_BOX(r->preset), 1);
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
    for (guint i = 0; i < G_N_ELEMENTS(capture_modes); i++)
        g_signal_connect(r->capture_buttons[i], "toggled", G_CALLBACK(capture_toggled), r);
    capture_changed(r);
    load_settings(r);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(footer, "recorder-footer");
    gtk_box_append(GTK_BOX(root), footer);
    GtkWidget *state_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(footer), state_line);
    r->rec_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(r->rec_dot, "rec-dot");
    gtk_widget_set_valign(r->rec_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(r->rec_dot, FALSE);
    gtk_box_append(GTK_BOX(state_line), r->rec_dot);
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
    r->discard = gtk_button_new_with_label("Discard");
    gtk_widget_set_tooltip_text(r->discard, "Stop immediately and delete this recording");
    gtk_widget_set_visible(r->discard, FALSE);
    g_signal_connect(r->discard, "clicked", G_CALLBACK(discard_clicked), r);
    gtk_box_append(GTK_BOX(buttons), r->discard);
    gtk_window_set_focus(GTK_WINDOW(r->window), r->start);
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
