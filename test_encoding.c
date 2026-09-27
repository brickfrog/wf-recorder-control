#include "encoding.h"

#include <gst/gst.h>

static void test_format_matrix(void) {
    g_assert_true(encoding_allowed("mp4", "libx264"));
    g_assert_true(encoding_allowed("mp4", "hevc_vaapi"));
    g_assert_false(encoding_allowed("mp4", "libvpx-vp9"));
    g_assert_true(encoding_allowed("webm", "libvpx-vp9"));
    g_assert_true(encoding_allowed("webm", "libaom-av1"));
    g_assert_false(encoding_allowed("webm", "h264_vaapi"));
    g_assert_true(encoding_allowed("mkv", "libaom-av1"));
}

static void test_quality_mapping(void) {
    gsize count = 0;
    const CodecInfo *codecs = encoding_codecs(&count);
    g_assert_cmpuint(count, >, 0);
    for (gsize i = 0; i < count; i++) {
        const CodecInfo *codec = &codecs[i];
        g_assert_cmpint(encoding_quantizer(codec, "high", 0), <,
                        encoding_quantizer(codec, "balanced", 0));
        g_assert_cmpint(encoding_quantizer(codec, "balanced", 0), <,
                        encoding_quantizer(codec, "compact", 0));
        g_assert_cmpint(encoding_quantizer(codec, "custom", 31), ==, 31);
    }
}

static void test_available_encoder(void) {
    const CodecInfo *codec = encoding_find("libx264");
    g_assert_nonnull(codec);
    g_assert_true(encoding_available(codec, FALSE));
    g_assert_true(encoding_available(codec, TRUE));
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/encoding/formats", test_format_matrix);
    g_test_add_func("/encoding/quality", test_quality_mapping);
    g_test_add_func("/encoding/availability", test_available_encoder);
    return g_test_run();
}
