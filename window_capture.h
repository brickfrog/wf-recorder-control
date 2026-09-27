#pragma once

#include <gtk/gtk.h>

typedef struct WindowCapture WindowCapture;
typedef void (*WindowCaptureEvent)(gboolean started, const gchar *message, gpointer data);

WindowCapture *window_capture_begin(GtkWindow *parent, const gchar *filename,
                                     const gchar *format, const gchar *codec,
                                     gint fps, gint quantizer, gint bitrate, const gchar *preset,
                                     const gchar *audio, WindowCaptureEvent event,
                                     gpointer data);
void window_capture_stop(WindowCapture *capture);
