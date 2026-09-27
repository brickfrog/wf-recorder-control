#include "portal_pipewire.h"

#include <gst/app/gstappsrc.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <linux/dma-buf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

struct PortalPipeWire {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook listener;
    GstElement *appsrc;
    struct spa_video_info_raw video;
    gint failed;
    gchar *failure;
    gint frames_pushed;
    GMutex frame_lock;
    GstBuffer *last_frame;
};

static void fail_source(PortalPipeWire *source, const gchar *message) {
    if (!g_atomic_int_get(&source->failed)) {
        source->failure = g_strdup(message);
        g_atomic_int_set(&source->failed, TRUE);
    }
}

static void state_changed(void *data, enum pw_stream_state old,
                          enum pw_stream_state state, const char *error) {
    (void)old;
    if (state == PW_STREAM_STATE_ERROR)
        fail_source(data, error ? error : "PipeWire stream failed");
}

static const gchar *gst_format(enum spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    default: return NULL;
    }
}

static void format_changed(void *data, uint32_t id, const struct spa_pod *param) {
    PortalPipeWire *source = data;
    if (id != SPA_PARAM_Format || !param) return;
    if (spa_format_video_raw_parse(param, &source->video) < 0) {
        fail_source(source, "Could not read selected-window video format");
        return;
    }
    const gchar *format = gst_format(source->video.format);
    if (!format || !source->video.size.width || !source->video.size.height) {
        fail_source(source, "Unsupported selected-window video format");
        return;
    }
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, format,
        "width", G_TYPE_INT, (gint)source->video.size.width,
        "height", G_TYPE_INT, (gint)source->video.size.height,
        "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
    gst_app_src_set_caps(GST_APP_SRC(source->appsrc), caps);
    gst_caps_unref(caps);
}

static void process_frame(void *data) {
    PortalPipeWire *source = data;
    struct pw_buffer *frame = pw_stream_dequeue_buffer(source->stream);
    if (!frame) return;
    struct spa_buffer *buffer = frame->buffer;
    guint width = source->video.size.width, height = source->video.size.height;
    if (buffer && buffer->n_datas && width && height && !g_atomic_int_get(&source->failed)) {
        struct spa_data *plane = &buffer->datas[0];
        struct spa_chunk *chunk = plane->chunk;
        guint row = width * 4;
        guint stride = chunk && chunk->stride > 0 ? (guint)chunk->stride : row;
        void *mapping = NULL;
        gsize available = plane->maxsize;
        const guint8 *pixels = plane->data;
        if (!pixels && (plane->type == SPA_DATA_DmaBuf || plane->type == SPA_DATA_MemFd)) {
            struct stat info;
            if (fstat(plane->fd, &info) == 0 && info.st_size > plane->mapoffset)
                available = (gsize)(info.st_size - plane->mapoffset);
            mapping = available ? mmap(NULL, available, PROT_READ, MAP_SHARED,
                                       plane->fd, plane->mapoffset) : MAP_FAILED;
            if (mapping == MAP_FAILED) {
                mapping = NULL;
                fail_source(source, "Could not map selected-window video buffer");
            } else {
                pixels = mapping;
                if (plane->type == SPA_DATA_DmaBuf) {
                    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
                    ioctl(plane->fd, DMA_BUF_IOCTL_SYNC, &sync);
                }
            }
        }
        if (pixels && chunk && stride >= row &&
            (guint64)chunk->offset + (guint64)stride * (height - 1) + row <= available) {
            GstBuffer *out = gst_buffer_new_allocate(NULL, (gsize)row * height, NULL);
            GstMapInfo map;
            if (gst_buffer_map(out, &map, GST_MAP_WRITE)) {
                const guint8 *from = pixels + chunk->offset;
                for (guint y = 0; y < height; y++)
                    memcpy(map.data + (gsize)y * row, from + (gsize)y * stride, row);
                gst_buffer_unmap(out, &map);
                g_mutex_lock(&source->frame_lock);
                gst_clear_buffer(&source->last_frame);
                source->last_frame = gst_buffer_ref(out);
                g_mutex_unlock(&source->frame_lock);
                if (gst_app_src_push_buffer(GST_APP_SRC(source->appsrc), out) == GST_FLOW_OK)
                    g_atomic_int_inc(&source->frames_pushed);
            } else gst_buffer_unref(out);
        }
        if (mapping) {
            if (plane->type == SPA_DATA_DmaBuf) {
                struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
                ioctl(plane->fd, DMA_BUF_IOCTL_SYNC, &sync);
            }
            munmap(mapping, available);
        }
    }
    pw_stream_queue_buffer(source->stream, frame);
}

static const struct pw_stream_events events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = state_changed,
    .param_changed = format_changed,
    .process = process_frame,
};

PortalPipeWire *portal_pipewire_new(int fd, guint node, GstElement *appsrc, GError **error) {
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        pw_init(NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
    PortalPipeWire *source = g_new0(PortalPipeWire, 1);
    g_mutex_init(&source->frame_lock);
    source->appsrc = gst_object_ref(appsrc);
    source->loop = pw_thread_loop_new("wf-recorder-window", NULL);
    if (!source->loop || pw_thread_loop_start(source->loop) < 0) goto failed;
    pw_thread_loop_lock(source->loop);
    source->context = pw_context_new(pw_thread_loop_get_loop(source->loop), NULL, 0);
    if (!source->context) goto unlock_failed;
    int owned_fd = dup(fd);
    if (owned_fd < 0) goto unlock_failed;
    source->core = pw_context_connect_fd(source->context, owned_fd, NULL, 0);
    if (!source->core) goto unlock_failed; // PipeWire closes owned_fd on error.
    source->stream = pw_stream_new(source->core, "wf-recorder-control",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", NULL));
    if (!source->stream) goto unlock_failed;
    pw_stream_add_listener(source->stream, &source->listener, &events, source);

    guint8 bytes[4096];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    const struct spa_pod *params[4];
    struct spa_rectangle preferred = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle minimum = SPA_RECTANGLE(1, 1);
    struct spa_rectangle maximum = SPA_RECTANGLE(8192, 8192);
    struct spa_fraction rate = SPA_FRACTION(0, 1);
    struct spa_fraction maximum_rate = SPA_FRACTION(360, 1);
    const guint formats[] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA};
    const guint64 modifiers[] = {0, 0x00ffffffffffffffULL};
    guint count = 0;
    for (guint i = 0; i < G_N_ELEMENTS(modifiers); i++)
        for (guint j = 0; j < G_N_ELEMENTS(formats); j++)
            params[count++] = spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_Id(formats[j]),
                SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(modifiers[i]),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &rate, &maximum_rate));
    int result = pw_stream_connect(source->stream, PW_DIRECTION_INPUT, node,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, count);
    pw_thread_loop_unlock(source->loop);
    if (result < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Cannot connect selected-window stream: %s", spa_strerror(result));
        portal_pipewire_free(source);
        return NULL;
    }
    return source;
unlock_failed:
    pw_thread_loop_unlock(source->loop);
failed:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not start PipeWire window stream");
    portal_pipewire_free(source);
    return NULL;
}

const gchar *portal_pipewire_error(PortalPipeWire *source) {
    return source && g_atomic_int_get(&source->failed) ? source->failure : NULL;
}

guint portal_pipewire_frames(PortalPipeWire *source) {
    return source ? (guint)g_atomic_int_get(&source->frames_pushed) : 0;
}

void portal_pipewire_finish_frame(PortalPipeWire *source) {
    if (!source) return;
    g_mutex_lock(&source->frame_lock);
    GstBuffer *last = source->last_frame ? gst_buffer_copy_deep(source->last_frame) : NULL;
    g_mutex_unlock(&source->frame_lock);
    if (!last) return;
    GST_BUFFER_PTS(last) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(last) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(last) = GST_CLOCK_TIME_NONE;
    if (gst_app_src_push_buffer(GST_APP_SRC(source->appsrc), last) == GST_FLOW_OK)
        g_atomic_int_inc(&source->frames_pushed);
}

void portal_pipewire_free(PortalPipeWire *source) {
    if (!source) return;
    if (source->loop) pw_thread_loop_stop(source->loop);
    if (source->stream) pw_stream_destroy(source->stream);
    if (source->core) pw_core_disconnect(source->core);
    if (source->context) pw_context_destroy(source->context);
    if (source->loop) pw_thread_loop_destroy(source->loop);
    if (source->appsrc) gst_object_unref(source->appsrc);
    gst_clear_buffer(&source->last_frame);
    g_mutex_clear(&source->frame_lock);
    g_free(source->failure);
    g_free(source);
}
