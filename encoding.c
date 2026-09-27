#include "encoding.h"

#include <gst/gst.h>
#include <libavcodec/avcodec.h>

static const CodecInfo codecs[] = {
    {"libx264", "H.264 software", "x264enc", FORMAT_MP4 | FORMAT_MKV, 18, 23, 28, FALSE},
    {"libx265", "H.265 software", "x265enc", FORMAT_MP4 | FORMAT_MKV, 20, 25, 30, FALSE},
    {"libvpx-vp9", "VP9 software", "vp9enc", FORMAT_MKV | FORMAT_WEBM, 24, 30, 36, FALSE},
    {"libaom-av1", "AV1 software", "av1enc", FORMAT_MKV | FORMAT_WEBM, 22, 28, 34, FALSE},
    {"h264_vaapi", "H.264 VA-API", "vah264enc", FORMAT_MP4 | FORMAT_MKV, 18, 24, 30, TRUE},
    {"hevc_vaapi", "H.265 VA-API", "vah265enc", FORMAT_MP4 | FORMAT_MKV, 20, 26, 32, TRUE},
};

const CodecInfo *encoding_codecs(gsize *count) {
    if (count) *count = G_N_ELEMENTS(codecs);
    return codecs;
}

const CodecInfo *encoding_find(const gchar *id) {
    for (gsize i = 0; i < G_N_ELEMENTS(codecs); i++)
        if (!g_strcmp0(codecs[i].id, id)) return &codecs[i];
    return NULL;
}

static guint format_mask(const gchar *format) {
    if (!g_strcmp0(format, "mp4")) return FORMAT_MP4;
    if (!g_strcmp0(format, "mkv")) return FORMAT_MKV;
    if (!g_strcmp0(format, "webm")) return FORMAT_WEBM;
    return 0;
}

gboolean encoding_allowed(const gchar *format, const gchar *codec) {
    const CodecInfo *info = encoding_find(codec);
    return info && (info->formats & format_mask(format));
}

gboolean encoding_available(const CodecInfo *codec, gboolean window_mode) {
    if (!codec) return FALSE;
    if (!window_mode) return avcodec_find_encoder_by_name(codec->id) != NULL;
    GstElementFactory *factory = gst_element_factory_find(codec->gst_encoder);
    if (!factory) return FALSE;
    gst_object_unref(factory);
    return TRUE;
}

gint encoding_quantizer(const CodecInfo *codec, const gchar *quality, gint custom) {
    if (!codec) return custom;
    if (!g_strcmp0(quality, "high")) return codec->high;
    if (!g_strcmp0(quality, "balanced")) return codec->balanced;
    if (!g_strcmp0(quality, "compact")) return codec->compact;
    return CLAMP(custom, 0, 51);
}
