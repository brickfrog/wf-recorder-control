#pragma once

#include <gtk/gtk.h>

#define WINDOW_CAPTURE_PREVIEW_WIDTH 640

typedef struct WindowCapture WindowCapture;
typedef enum {
    WINDOW_CAPTURE_SELECTED, /* window chosen; call window_capture_record() to start */
    WINDOW_CAPTURE_STARTED,  /* first window frame arrived; the recording timeline starts here */
    WINDOW_CAPTURE_FINISHED, /* capture freed; message is NULL on success */
} WindowCaptureState;
typedef void (*WindowCaptureEvent)(WindowCaptureState state, const gchar *message, gpointer data);

WindowCapture *window_capture_begin(GtkWindow *parent, const gchar *filename,
                                     const gchar *format, const gchar *codec,
                                     gint fps, gint quantizer, gint bitrate, const gchar *preset,
                                     const gchar *audio, WindowCaptureEvent event,
                                     gpointer data);
/* Opens the stream and waits for the first window frame before recording (STARTED). */
void window_capture_record(WindowCapture *capture);
/* Finalizes a recording, or cancels a capture that has not started recording. */
void window_capture_stop(WindowCapture *capture);
/* Tears the capture down immediately without finalizing the file (FINISHED follows). */
void window_capture_cancel(WindowCapture *capture);
/* TRUE once the first frame arrived and the recording timeline started. */
gboolean window_capture_recording(WindowCapture *capture);
/* Returns the newest preview frame since the previous call, or NULL. Main thread only. */
GdkTexture *window_capture_preview(WindowCapture *capture);
