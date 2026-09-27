#pragma once

#include <gio/gio.h>
#include <gst/gst.h>

typedef struct PortalPipeWire PortalPipeWire;

// The caller retains ownership of fd and appsrc.
PortalPipeWire *portal_pipewire_new(int fd, guint node, GstElement *appsrc, GError **error);
const gchar *portal_pipewire_error(PortalPipeWire *source);
guint portal_pipewire_frames(PortalPipeWire *source);
void portal_pipewire_finish_frame(PortalPipeWire *source);
void portal_pipewire_free(PortalPipeWire *source);
