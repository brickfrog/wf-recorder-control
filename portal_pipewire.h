#pragma once

#include <gio/gio.h>
#include <gst/gst.h>

typedef struct PortalPipeWire PortalPipeWire;

// The caller retains ownership of fd and appsrc.
PortalPipeWire *portal_pipewire_new(int fd, guint node, GstElement *appsrc, GError **error);
const gchar *portal_pipewire_error(PortalPipeWire *source);
/* Frames pushed to appsrc; frames are pushed only after portal_pipewire_activate(). */
guint portal_pipewire_frames(PortalPipeWire *source);
/* Frames received from PipeWire, including those before activation. */
guint portal_pipewire_frames_seen(PortalPipeWire *source);
/* Starts pushing frames and repeating the newest one when no fresh frame arrives. Main thread. */
void portal_pipewire_activate(PortalPipeWire *source);
/* Stops pushing and sends the newest frame once more so the video covers the full duration. Main thread. */
void portal_pipewire_finish_frame(PortalPipeWire *source);
void portal_pipewire_free(PortalPipeWire *source);
