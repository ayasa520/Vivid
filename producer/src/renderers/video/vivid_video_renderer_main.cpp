#include "vivid_video_producer.h"

#include "vivid_renderer_host.h"
#include "vivid_renderer_worker_common.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cmath>

namespace {

struct VideoWorker {
    VividRendererWorkerCommon common {};
    bool common_initialized { false };
    VividVideoProducer* producer { nullptr };
    VividVideoProducerBufferSet pool {};
    GThread* frame_thread { nullptr };
    GMutex backend_lock;
    GMutex state_lock;
    bool locks_initialized { false };
    bool frame_thread_stopping { false };
    bool playing { true };
    bool looping { true };
    bool muted { false };
    double volume { 1.0 };
    double render_scale { 1.0 };
    int content_fit { 1 };
    int fps { 30 };
};

bool json_int_member(JsonObject* object, const char* name, int& value)
{
    if (!json_object_has_member(object, name))
        return true;
    JsonNode* node = json_object_get_member(object, name);
    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64) {
        return false;
    }
    value = static_cast<int>(json_node_get_int(node));
    return true;
}

bool json_bool_member(JsonObject* object, const char* name, bool& value)
{
    if (!json_object_has_member(object, name))
        return true;
    JsonNode* node = json_object_get_member(object, name);
    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
        return false;
    }
    value = json_node_get_boolean(node);
    return true;
}

bool apply_settings_json(VideoWorker* worker, const char* json, GError** error)
{
    if (!json || !*json)
        return true;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, error))
        return false;
    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "video runtime settings must be a JSON object");
        return false;
    }
    JsonObject* object = json_node_get_object(root);
    int fps = worker->fps;
    int content_fit = worker->content_fit;
    bool looping = worker->looping;
    if (!json_int_member(object, "fps", fps) ||
        !json_int_member(object, "content-fit", content_fit) ||
        !json_bool_member(object, "loop", looping) ||
        fps < 5 || fps > 240 || content_fit < 1 || content_fit > 3) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "video runtime settings are outside their declared ranges");
        return false;
    }
    g_mutex_lock(&worker->state_lock);
    worker->fps = fps;
    worker->content_fit = content_fit;
    worker->looping = looping;
    g_mutex_unlock(&worker->state_lock);
    return true;
}

bool configure_video(VideoWorker* worker, GError** error)
{
    g_mutex_lock(&worker->state_lock);
    const int content_fit = worker->content_fit;
    const int fps = worker->fps;
    const bool looping = worker->looping;
    g_mutex_unlock(&worker->state_lock);
    g_mutex_lock(&worker->backend_lock);
    const gboolean configured = vivid_video_producer_configure(
        worker->producer,
        worker->common.project_path,
        worker->muted,
        worker->volume,
        content_fit,
        fps,
        worker->common.render_node,
        &worker->common.gpu);
    if (configured) {
        vivid_video_producer_set_looping(worker->producer, looping);
        vivid_video_producer_set_playing(worker->producer, worker->playing);
    }
    g_mutex_unlock(&worker->backend_lock);
    if (!configured) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "video backend rejected project '%s'",
                    worker->common.project_path);
        return false;
    }
    return true;
}

bool publish_video_caps(VideoWorker* worker, guint64 request_id, GError** error)
{
    VividVideoProducerDmaBufCaps video_caps {};
    g_mutex_lock(&worker->backend_lock);
    const gboolean queried =
        vivid_video_producer_query_dmabuf_caps(worker->producer, &video_caps);
    g_mutex_unlock(&worker->backend_lock);
    if (!queried || video_caps.n_caps == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "video backend returned no DMA-BUF capabilities");
        return false;
    }
    VividRendererWorkerFormatCap caps[VIVID_VIDEO_PRODUCER_DMABUF_MAX_CAPS] {};
    for (guint32 i = 0; i < video_caps.n_caps; i++) {
        caps[i] = {
            .fourcc = video_caps.caps[i].fourcc,
            .modifier = video_caps.caps[i].modifier,
            .plane_count = video_caps.caps[i].plane_count,
        };
    }
    const guint32 memory_hints =
        video_caps.memory_preference ==
                VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE
        ? VIVID_RENDERER_MEMORY_HINT_HOST_VISIBLE
        : VIVID_RENDERER_MEMORY_HINT_DEVICE_LOCAL;
    return vivid_renderer_worker_common_publish_handshake(
        &worker->common,
        request_id,
        caps,
        video_caps.n_caps,
        memory_hints,
        VIVID_RENDERER_COLOR_CAP_SRGB |
            VIVID_RENDERER_COLOR_CAP_RANGE_LIMITED |
            VIVID_RENDERER_COLOR_CAP_ALPHA_STRAIGHT,
        VIVID_RENDERER_RELAY_MODE_SHADOW_COPY,
        8192,
        8192,
        VIVID_VIDEO_PRODUCER_MAX_BUFFERS,
        VIVID_VIDEO_PRODUCER_MAX_BUFFERS,
        error);
}

gboolean video_initialize(VividRendererHost* host,
                          const VividRendererHostInit* init,
                          guint64 request_id,
                          gpointer backend_data,
                          GError** error)
{
    auto* worker = static_cast<VideoWorker*>(backend_data);
    g_mutex_init(&worker->backend_lock);
    g_mutex_init(&worker->state_lock);
    worker->locks_initialized = true;
    worker->playing =
        (init->playback_flags & VIVID_RENDERER_PLAYBACK_FLAG_PLAYING) != 0;
    worker->muted =
        (init->playback_flags & VIVID_RENDERER_PLAYBACK_FLAG_MUTED) != 0;
    worker->volume = std::clamp(static_cast<double>(init->volume), 0.0, 1.0);
    worker->render_scale = std::max(1.0, static_cast<double>(init->scale));
    worker->fps = std::clamp(static_cast<int>(std::lround(init->fps)), 5, 240);
    if (!vivid_renderer_worker_common_init(&worker->common, host, init, error))
        return FALSE;
    worker->common_initialized = true;
    if (!apply_settings_json(worker, worker->common.settings_json, error))
        return FALSE;
    worker->producer = vivid_video_producer_new();
    if (!worker->producer) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "video backend allocation failed");
        return FALSE;
    }
    const VividRendererReleaseGate gate =
        vivid_renderer_worker_common_release_gate(&worker->common);
    vivid_video_producer_set_release_gate(worker->producer, &gate);
    if (!configure_video(worker, error))
        return FALSE;
    return publish_video_caps(worker, request_id, error);
}

VividVideoProducerDmaBufMemoryPreference memory_preference(
    VividRendererMemorySource source)
{
    return source == VIVID_RENDERER_MEMORY_SOURCE_GPU_NATIVE
        ? VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
        : VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
}

bool prepare_video_pool(VideoWorker* worker,
                        const VividRendererWorkerNegotiation& negotiation,
                        GError** error)
{
    vivid_video_producer_buffer_set_clear(&worker->pool);
    const VividVideoProducerDmaBufRequest request {
        .fourcc = negotiation.fourcc,
        .modifier = negotiation.modifier,
        .plane_count = negotiation.plane_count,
        .require_modifier = TRUE,
        .memory_preference = memory_preference(negotiation.memory_source),
    };
    const gint64 deadline = g_get_monotonic_time() +
        static_cast<gint64>(VIVID_RENDERER_LIFECYCLE_INIT_TIMEOUT_MS) * 1000;
    while (g_get_monotonic_time() < deadline) {
        g_mutex_lock(&worker->backend_lock);
        const gboolean prepared = vivid_video_producer_prepare_buffers_with_request(
            worker->producer,
            negotiation.width,
            negotiation.height,
            worker->render_scale,
            &request,
            &worker->pool);
        if (!prepared)
            vivid_video_producer_request_frame(worker->producer, "worker-negotiation");
        g_mutex_unlock(&worker->backend_lock);
        if (prepared)
            return true;
        g_usleep(static_cast<gulong>(VIVID_RENDERER_BACKEND_READY_POLL_INTERVAL_MS) *
                  1000u);
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "video DMA-BUF pool did not become ready for %ux%u modifier=0x%016"
                G_GINT64_MODIFIER "x",
                negotiation.width,
                negotiation.height,
                negotiation.modifier);
    return false;
}

bool convert_video_pool(const VividVideoProducerBufferSet& source,
                        VividRendererWorkerPool& target)
{
    if (source.n_buffers == 0 ||
        source.n_buffers > VIVID_RENDERER_MAX_POOL_BUFFERS)
        return false;
    target = {
        .width = source.width,
        .height = source.height,
        .fourcc = source.fourcc,
        .modifier = source.modifier,
        .premultiplied = source.premultiplied,
        .n_buffers = source.n_buffers,
    };
    for (guint32 i = 0; i < source.n_buffers; i++) {
        const auto& source_buffer = source.buffers[i];
        auto& target_buffer = target.buffers[i];
        if (source_buffer.n_planes == 0 ||
            source_buffer.n_planes > VIVID_RENDERER_MAX_PLANES)
            return false;
        target_buffer.index = source_buffer.index;
        target_buffer.n_planes = source_buffer.n_planes;
        for (guint32 plane = 0; plane < source_buffer.n_planes; plane++) {
            target_buffer.planes[plane] = {
                .fd = source_buffer.planes[plane].fd,
                .stride = source_buffer.planes[plane].stride,
                .offset = source_buffer.planes[plane].offset,
                .size = source_buffer.size,
            };
        }
    }
    return true;
}

gpointer video_frame_thread(gpointer user_data)
{
    auto* worker = static_cast<VideoWorker*>(user_data);
    for (;;) {
        g_mutex_lock(&worker->state_lock);
        const bool stopping = worker->frame_thread_stopping;
        const int fps = worker->fps;
        g_mutex_unlock(&worker->state_lock);
        if (stopping)
            break;
        VividVideoProducerFrame frame {};
        frame.acquire_sync_fd = -1;
        g_mutex_lock(&worker->backend_lock);
        const gboolean ready = vivid_video_producer_next_frame(worker->producer, &frame);
        g_mutex_unlock(&worker->backend_lock);
        if (ready) {
            g_autoptr(GError) error = nullptr;
            if (!vivid_renderer_worker_common_publish_frame(
                    &worker->common,
                    frame.buffer_index,
                    frame.target_time_usec,
                    &frame.acquire_sync_fd,
                    &error)) {
                g_warning("VividVideoRenderer: frame publish stopped: %s",
                          error->message);
                g_mutex_lock(&worker->state_lock);
                worker->frame_thread_stopping = true;
                g_mutex_unlock(&worker->state_lock);
                break;
            }
        }
        g_usleep(G_USEC_PER_SEC / static_cast<guint64>(std::max(fps, 1)));
    }
    return nullptr;
}

void stop_frame_thread(VideoWorker* worker)
{
    g_mutex_lock(&worker->state_lock);
    worker->frame_thread_stopping = true;
    GThread* thread = worker->frame_thread;
    worker->frame_thread = nullptr;
    g_mutex_unlock(&worker->state_lock);
    if (thread)
        g_thread_join(thread);
}

gboolean video_negotiate(VividRendererHost* host,
                         const VividRendererPacket* packet,
                         gpointer backend_data,
                         GError** error)
{
    (void)host;
    auto* worker = static_cast<VideoWorker*>(backend_data);
    stop_frame_thread(worker);
    VividRendererWorkerNegotiation negotiation {};
    if (!vivid_renderer_worker_common_parse_negotiation(packet,
                                                        &negotiation,
                                                        error) ||
        negotiation.memory_source ==
            VIVID_RENDERER_MEMORY_SOURCE_DMABUF_HEAP_RESERVED ||
        !prepare_video_pool(worker, negotiation, error)) {
        return FALSE;
    }
    VividRendererWorkerPool pool {};
    if (!convert_video_pool(worker->pool, pool)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "video backend produced invalid pool metadata");
        return FALSE;
    }
    if (!vivid_renderer_worker_common_publish_pool(&worker->common,
                                                   packet->header.request_id,
                                                   &negotiation,
                                                   &pool,
                                                   error)) {
        return FALSE;
    }
    g_mutex_lock(&worker->state_lock);
    worker->frame_thread_stopping = false;
    worker->frame_thread =
        g_thread_new("vivid-video-frame", video_frame_thread, worker);
    g_mutex_unlock(&worker->state_lock);
    g_mutex_lock(&worker->backend_lock);
    vivid_video_producer_request_frame(worker->producer, "worker-first-frame");
    g_mutex_unlock(&worker->backend_lock);
    return TRUE;
}

gboolean video_runtime(VividRendererHost* host,
                       const VividRendererPacket* packet,
                       gpointer backend_data,
                       GError** error)
{
    (void)host;
    auto* worker = static_cast<VideoWorker*>(backend_data);
    switch (packet->header.opcode) {
    case VIVID_RENDERER_MSG_SET_RUNTIME: {
        const guint32 length = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_SET_RUNTIME_SETTINGS_JSON_LENGTH_OFFSET);
        g_autofree gchar* json = g_strndup(
            reinterpret_cast<const gchar*>(packet->payload) +
                VIVID_RENDERER_SET_RUNTIME_FIXED_BYTES,
            length);
        if (!apply_settings_json(worker, json, error) ||
            !configure_video(worker, error))
            return FALSE;
        break;
    }
    case VIVID_RENDERER_MSG_SET_PLAYBACK: {
        const guint32 playing = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_SET_PLAYBACK_PLAYING_OFFSET);
        const guint32 muted = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_SET_PLAYBACK_MUTED_OFFSET);
        const float volume = vivid_renderer_wire_read_f32(
            packet->payload + VIVID_RENDERER_SET_PLAYBACK_VOLUME_OFFSET);
        if (playing > 1 || muted > 1 || !std::isfinite(volume) ||
            volume < 0.0f || volume > 1.0f) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "SET_PLAYBACK contains invalid video state");
            return FALSE;
        }
        worker->playing = playing != 0;
        worker->muted = muted != 0;
        worker->volume = volume;
        g_mutex_lock(&worker->backend_lock);
        vivid_video_producer_set_audio_state(worker->producer,
                                             worker->muted,
                                             worker->volume);
        vivid_video_producer_set_playing(worker->producer, worker->playing);
        g_mutex_unlock(&worker->backend_lock);
        break;
    }
    default:
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "video renderer does not support this runtime opcode");
        return FALSE;
    }
    return TRUE;
}

gboolean video_quiesce(VividRendererHost* host,
                       gpointer backend_data,
                       GError** error)
{
    (void)host;
    (void)error;
    auto* worker = static_cast<VideoWorker*>(backend_data);
    stop_frame_thread(worker);
    g_mutex_lock(&worker->backend_lock);
    if (worker->producer)
        vivid_video_producer_set_playing(worker->producer, FALSE);
    g_mutex_unlock(&worker->backend_lock);
    return TRUE;
}

void video_shutdown(VividRendererHost* host, gpointer backend_data)
{
    (void)host;
    auto* worker = static_cast<VideoWorker*>(backend_data);
    if (!worker->locks_initialized)
        return;
    stop_frame_thread(worker);
    if (worker->producer) {
        g_mutex_lock(&worker->backend_lock);
        vivid_video_producer_buffer_set_clear(&worker->pool);
        vivid_video_producer_free(worker->producer);
        worker->producer = nullptr;
        g_mutex_unlock(&worker->backend_lock);
    }
    if (worker->common_initialized) {
        vivid_renderer_worker_common_clear(&worker->common);
        worker->common_initialized = false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    VideoWorker worker;
    const VividRendererHostBackend backend {
        .renderer_id = "vivid.video",
        .kind = VIVID_RENDERER_KIND_VIDEO,
        .initialize = video_initialize,
        .negotiate_buffers = video_negotiate,
        .runtime_command = video_runtime,
        .quiesce = video_quiesce,
        .shutdown = video_shutdown,
    };
    const int result = vivid_renderer_host_run(argc, argv, &backend, &worker);
    video_shutdown(nullptr, &worker);
    if (worker.locks_initialized) {
        g_mutex_clear(&worker.state_lock);
        g_mutex_clear(&worker.backend_lock);
    }
    return result;
}
