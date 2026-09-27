#pragma once

#include <glib.h>

typedef struct {
    const gchar *id;
    const gchar *label;
    const gchar *gst_encoder;
    guint formats;
    gint high, balanced, compact;
    gboolean hardware;
} CodecInfo;

enum { FORMAT_MP4 = 1, FORMAT_MKV = 2, FORMAT_WEBM = 4 };

const CodecInfo *encoding_codecs(gsize *count);
const CodecInfo *encoding_find(const gchar *id);
gboolean encoding_allowed(const gchar *format, const gchar *codec);
gboolean encoding_available(const CodecInfo *codec, gboolean window_mode);
gint encoding_quantizer(const CodecInfo *codec, const gchar *quality, gint custom);
