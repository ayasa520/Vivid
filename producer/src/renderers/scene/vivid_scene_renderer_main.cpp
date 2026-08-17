#include "vivid_scene_producer.h"

#include "vivid_renderer_host.h"
#include "vivid_renderer_worker_common.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct SceneWorker {
    VividRendererWorkerCommon common {};
    bool common_initialized { false };
    VividSceneProducer* producer { nullptr };
    VividSceneProducerBufferSet pool {};
    GThread* frame_thread { nullptr };
    GMutex backend_lock;
    GMutex state_lock;
    bool locks_initialized { false };
    bool frame_thread_stopping { false };
    bool playing { true };
    bool muted { false };
    double volume { 1.0 };
    double render_scale { 1.0 };
    int content_fit { 1 };
    int fps { 30 };
    bool gfx_reflections { true };
    int gfx_volumetrics { 2 };
    int gfx_shadows { 2 };
    int gfx_postprocessing { 1 };
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

bool apply_settings_json(SceneWorker* worker,
                         const char* json,
                         bool allow_properties,
                         GError** error)
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
                            "scene runtime settings must be a JSON object");
        return false;
    }
    JsonObject* object = json_node_get_object(root);
    int fps = worker->fps;
    int content_fit = worker->content_fit;
    bool gfx_reflections = worker->gfx_reflections;
    int gfx_volumetrics = worker->gfx_volumetrics;
    int gfx_shadows = worker->gfx_shadows;
    int gfx_postprocessing = worker->gfx_postprocessing;
    if (!json_int_member(object, "fps", fps) ||
        !json_int_member(object, "content-fit", content_fit) ||
        !json_bool_member(object, "gfx-reflections", gfx_reflections) ||
        !json_int_member(object, "gfx-volumetrics", gfx_volumetrics) ||
        !json_int_member(object, "gfx-shadows", gfx_shadows) ||
        !json_int_member(object, "gfx-postprocessing", gfx_postprocessing) ||
        fps < 5 || fps > 240 || content_fit < 1 || content_fit > 3 ||
        gfx_volumetrics < 0 || gfx_volumetrics > 4 ||
        gfx_shadows < 0 || gfx_shadows > 4 ||
        gfx_postprocessing < 0 || gfx_postprocessing > 3) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "scene runtime fps/content-fit/gfx-reflections/gfx-volumetrics/gfx-shadows/gfx-postprocessing is invalid");
        return false;
    }
    g_mutex_lock(&worker->state_lock);
    worker->fps = fps;
    worker->content_fit = content_fit;
    worker->gfx_reflections = gfx_reflections;
    worker->gfx_volumetrics = gfx_volumetrics;
    worker->gfx_shadows = gfx_shadows;
    worker->gfx_postprocessing = gfx_postprocessing;
    g_mutex_unlock(&worker->state_lock);
    if (allow_properties && json_object_has_member(object, "user-properties")) {
        JsonNode* properties = json_object_get_member(object, "user-properties");
        g_autofree gchar* serialized = json_to_string(properties, FALSE);
        g_free(worker->common.properties_json);
        worker->common.properties_json = g_steal_pointer(&serialized);
    }
    return true;
}

bool configure_scene(SceneWorker* worker, GError** error)
{
    g_mutex_lock(&worker->state_lock);
    const int content_fit = worker->content_fit;
    const int fps = worker->fps;
    const bool gfx_reflections = worker->gfx_reflections;
    const int gfx_volumetrics = worker->gfx_volumetrics;
    const int gfx_shadows = worker->gfx_shadows;
    const int gfx_postprocessing = worker->gfx_postprocessing;
    g_mutex_unlock(&worker->state_lock);
    g_mutex_lock(&worker->backend_lock);
    const gboolean configured = vivid_scene_producer_configure(
        worker->producer,
        worker->common.project_path,
        worker->common.properties_json,
        worker->muted,
        worker->volume,
        content_fit,
        fps,
        gfx_reflections,
        gfx_volumetrics,
        gfx_shadows,
        gfx_postprocessing,
        worker->common.render_node,
        &worker->common.gpu);
    if (configured)
        vivid_scene_producer_set_playing(worker->producer, worker->playing);
    g_mutex_unlock(&worker->backend_lock);
    if (!configured) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "scene backend rejected project '%s'",
                    worker->common.project_path);
        return false;
    }
    return true;
}

bool publish_scene_caps(SceneWorker* worker,
                        guint64 request_id,
                        GError** error)
{
    const VividGpuDevice& gpu = worker->common.gpu;
    if (gpu.scene_dmabuf_n_caps == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "resolved GPU returned no Scene DMA-BUF capabilities");
        return false;
    }

    /*
     * Keep modifier ownership at the renderer-process boundary. Worker-common
     * has already enumerated the exact render node from INIT and rejected a
     * deviceUUID mismatch, so these cached tuples describe the same physical
     * device that SceneWallpaper will receive during configure and pool
     * creation. vivid_gpu_devices also filters every tuple by the Vulkan image
     * features required by the Scene producer before it reaches this point.
     *
     * This restores the historical in-process contract and follows waywallen's
     * pool model: the renderer publishes immutable capabilities from its own
     * resolved GPU, the daemon selects an exact tuple, and the renderer creates
     * that tuple when handling NEGOTIATE_BUFFERS. Calling the Scene module's
     * pre-pool query here would collapse the declaration to LINEAR because the
     * module deliberately cannot inspect its internal Vulkan device until the
     * negotiated pool is created.
     */
    VividRendererWorkerFormatCap caps[VIVID_GPU_DEVICE_DMABUF_CAPS_MAX] {};
    for (guint32 i = 0; i < gpu.scene_dmabuf_n_caps; i++) {
        caps[i] = {
            .fourcc = gpu.scene_dmabuf_caps[i].fourcc,
            .modifier = gpu.scene_dmabuf_caps[i].modifier,
            .plane_count = gpu.scene_dmabuf_caps[i].plane_count,
        };
    }
    const guint32 memory_hints = gpu.scene_dmabuf_n_caps > 1
        ? VIVID_RENDERER_MEMORY_HINT_DEVICE_LOCAL
        : VIVID_RENDERER_MEMORY_HINT_HOST_VISIBLE;
    g_message("VividSceneRenderer: publishing %u GPU-filtered DMA-BUF capabilities "
              "render-node=%s memory-hints=0x%x",
              gpu.scene_dmabuf_n_caps,
              worker->common.render_node,
              memory_hints);
    return vivid_renderer_worker_common_publish_handshake(
        &worker->common,
        request_id,
        caps,
        gpu.scene_dmabuf_n_caps,
        memory_hints,
        VIVID_RENDERER_COLOR_CAP_SRGB |
            VIVID_RENDERER_COLOR_CAP_RANGE_LIMITED |
            VIVID_RENDERER_COLOR_CAP_ALPHA_STRAIGHT,
        VIVID_RENDERER_RELAY_MODE_SHADOW_COPY,
        8192,
        8192,
        VIVID_SCENE_PRODUCER_MAX_BUFFERS,
        VIVID_SCENE_PRODUCER_MAX_BUFFERS,
        error);
}

gboolean scene_initialize(VividRendererHost* host,
                          const VividRendererHostInit* init,
                          guint64 request_id,
                          gpointer backend_data,
                          GError** error)
{
    auto* worker = static_cast<SceneWorker*>(backend_data);
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
    if (!apply_settings_json(worker,
                             worker->common.settings_json,
                             false,
                             error)) {
        return FALSE;
    }
    worker->producer = vivid_scene_producer_new();
    if (!worker->producer) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "scene backend allocation failed");
        return FALSE;
    }
    const VividRendererReleaseGate gate =
        vivid_renderer_worker_common_release_gate(&worker->common);
    vivid_scene_producer_set_release_gate(worker->producer, &gate);
    if (!configure_scene(worker, error))
        return FALSE;
    return publish_scene_caps(worker, request_id, error);
}

VividSceneProducerDmaBufMemoryPreference memory_preference(
    VividRendererMemorySource source)
{
    return source == VIVID_RENDERER_MEMORY_SOURCE_GPU_NATIVE
        ? VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
        : VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
}

bool prepare_scene_pool(SceneWorker* worker,
                        const VividRendererWorkerNegotiation& negotiation,
                        GError** error)
{
    vivid_scene_producer_buffer_set_clear(&worker->pool);
    const VividSceneProducerDmaBufRequest request {
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
        const gboolean prepared = vivid_scene_producer_prepare_buffers_with_request(
            worker->producer,
            negotiation.width,
            negotiation.height,
            worker->render_scale,
            &request,
            &worker->pool);
        const VividSceneProducerDmaBufPrepareStatus status =
            vivid_scene_producer_get_last_dmabuf_prepare_status(worker->producer);
        if (!prepared)
            vivid_scene_producer_request_frame(worker->producer, "worker-negotiation");
        g_mutex_unlock(&worker->backend_lock);
        if (prepared)
            return true;
        if (status == VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED)
            break;
        g_usleep(static_cast<gulong>(VIVID_RENDERER_BACKEND_READY_POLL_INTERVAL_MS) *
                  1000u);
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "scene DMA-BUF pool did not become ready for %ux%u modifier=0x%016"
                G_GINT64_MODIFIER "x",
                negotiation.width,
                negotiation.height,
                negotiation.modifier);
    return false;
}

bool convert_scene_pool(const VividSceneProducerBufferSet& source,
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

gpointer scene_frame_thread(gpointer user_data)
{
    auto* worker = static_cast<SceneWorker*>(user_data);
    for (;;) {
        g_mutex_lock(&worker->state_lock);
        const bool stopping = worker->frame_thread_stopping;
        const int fps = worker->fps;
        g_mutex_unlock(&worker->state_lock);
        if (stopping)
            break;

        VividSceneProducerFrame frame {};
        frame.acquire_sync_fd = -1;
        g_mutex_lock(&worker->backend_lock);
        const gboolean ready =
            vivid_scene_producer_next_frame(worker->producer, &frame);
        g_mutex_unlock(&worker->backend_lock);
        if (ready) {
            g_autoptr(GError) error = nullptr;
            if (!vivid_renderer_worker_common_publish_frame(
                    &worker->common,
                    frame.buffer_index,
                    frame.target_time_usec,
                    &frame.acquire_sync_fd,
                    &error)) {
                g_warning("VividSceneRenderer: frame publish stopped: %s",
                          error->message);
                g_mutex_lock(&worker->state_lock);
                worker->frame_thread_stopping = true;
                g_mutex_unlock(&worker->state_lock);
                break;
            }
        }
        const guint64 interval_usec = G_USEC_PER_SEC /
            static_cast<guint64>(std::max(fps, 1));
        g_usleep(interval_usec);
    }
    return nullptr;
}

void stop_frame_thread(SceneWorker* worker)
{
    g_mutex_lock(&worker->state_lock);
    worker->frame_thread_stopping = true;
    GThread* thread = worker->frame_thread;
    worker->frame_thread = nullptr;
    g_mutex_unlock(&worker->state_lock);
    if (thread)
        g_thread_join(thread);
}

gboolean scene_negotiate(VividRendererHost* host,
                         const VividRendererPacket* packet,
                         gpointer backend_data,
                         GError** error)
{
    (void)host;
    auto* worker = static_cast<SceneWorker*>(backend_data);
    stop_frame_thread(worker);
    VividRendererWorkerNegotiation negotiation {};
    if (!vivid_renderer_worker_common_parse_negotiation(packet,
                                                        &negotiation,
                                                        error) ||
        negotiation.memory_source ==
            VIVID_RENDERER_MEMORY_SOURCE_DMABUF_HEAP_RESERVED ||
        !prepare_scene_pool(worker, negotiation, error)) {
        return FALSE;
    }
    VividRendererWorkerPool pool {};
    if (!convert_scene_pool(worker->pool, pool)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "scene backend produced invalid pool metadata");
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
        g_thread_new("vivid-scene-ipc", scene_frame_thread, worker);
    g_mutex_unlock(&worker->state_lock);
    g_mutex_lock(&worker->backend_lock);
    vivid_scene_producer_request_frame(worker->producer, "worker-first-frame");
    g_mutex_unlock(&worker->backend_lock);
    return TRUE;
}

bool parse_runtime_json(const VividRendererPacket* packet, gchar** out_json)
{
    const guint32 length = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_SET_RUNTIME_SETTINGS_JSON_LENGTH_OFFSET);
    *out_json = g_strndup(
        reinterpret_cast<const gchar*>(packet->payload) +
            VIVID_RENDERER_SET_RUNTIME_FIXED_BYTES,
        length);
    return true;
}

gboolean scene_runtime(VividRendererHost* host,
                       const VividRendererPacket* packet,
                       gpointer backend_data,
                       GError** error)
{
    (void)host;
    auto* worker = static_cast<SceneWorker*>(backend_data);
    switch (packet->header.opcode) {
    case VIVID_RENDERER_MSG_SET_RUNTIME: {
        g_autofree gchar* json = nullptr;
        parse_runtime_json(packet, &json);
        if (!apply_settings_json(worker, json, true, error) ||
            !configure_scene(worker, error))
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
                                "SET_PLAYBACK contains invalid scene state");
            return FALSE;
        }
        worker->playing = playing != 0;
        worker->muted = muted != 0;
        worker->volume = volume;
        if (!configure_scene(worker, error))
            return FALSE;
        break;
    }
    case VIVID_RENDERER_MSG_SET_MEDIA_STATE: {
        const guint32 length = vivid_renderer_wire_read_u32(
            packet->payload +
                VIVID_RENDERER_SET_MEDIA_STATE_MEDIA_STATE_JSON_LENGTH_OFFSET);
        g_autofree gchar* json = g_strndup(
            reinterpret_cast<const gchar*>(packet->payload) +
                VIVID_RENDERER_SET_MEDIA_STATE_FIXED_BYTES,
            length);
        g_mutex_lock(&worker->backend_lock);
        vivid_scene_producer_set_media_state_json(worker->producer, json);
        g_mutex_unlock(&worker->backend_lock);
        break;
    }
    case VIVID_RENDERER_MSG_SET_AUDIO_SAMPLES: {
        const guint32 count = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_SET_AUDIO_SAMPLES_COUNT_OFFSET);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("ad"));
        for (guint32 i = 0; i < count; i++) {
            const float sample = vivid_renderer_wire_read_f32(
                packet->payload + VIVID_RENDERER_SET_AUDIO_SAMPLES_FIXED_BYTES +
                static_cast<gsize>(i) * sizeof(float));
            if (!std::isfinite(sample) || sample < 0.0f) {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_DATA,
                                    "SET_AUDIO_SAMPLES contains an invalid sample");
                return FALSE;
            }
            g_variant_builder_add(&builder, "d", static_cast<double>(sample));
        }
        g_autoptr(GVariant) samples =
            g_variant_ref_sink(g_variant_builder_end(&builder));
        g_mutex_lock(&worker->backend_lock);
        vivid_scene_producer_set_audio_samples(worker->producer, samples);
        g_mutex_unlock(&worker->backend_lock);
        break;
    }
    case VIVID_RENDERER_MSG_POINTER_MOTION: {
        const double x = vivid_renderer_wire_read_f64(
            packet->payload + VIVID_RENDERER_POINTER_MOTION_X_OFFSET);
        const double y = vivid_renderer_wire_read_f64(
            packet->payload + VIVID_RENDERER_POINTER_MOTION_Y_OFFSET);
        g_mutex_lock(&worker->backend_lock);
        vivid_scene_producer_set_pointer_motion(worker->producer, x, y);
        g_mutex_unlock(&worker->backend_lock);
        break;
    }
    case VIVID_RENDERER_MSG_POINTER_BUTTON: {
        const guint32 button = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_POINTER_BUTTON_BUTTON_OFFSET);
        const guint32 state = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_POINTER_BUTTON_STATE_OFFSET);
        if (state > 1) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "POINTER_BUTTON contains an invalid state");
            return FALSE;
        }
        g_mutex_lock(&worker->backend_lock);
        vivid_scene_producer_set_pointer_button(worker->producer,
                                                button,
                                                state != 0);
        g_mutex_unlock(&worker->backend_lock);
        break;
    }
    default:
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "scene renderer does not support this runtime opcode");
        return FALSE;
    }
    return TRUE;
}

gboolean scene_quiesce(VividRendererHost* host,
                       gpointer backend_data,
                       GError** error)
{
    (void)host;
    (void)error;
    auto* worker = static_cast<SceneWorker*>(backend_data);
    stop_frame_thread(worker);
    g_mutex_lock(&worker->backend_lock);
    if (worker->producer)
        vivid_scene_producer_set_playing(worker->producer, FALSE);
    g_mutex_unlock(&worker->backend_lock);
    return TRUE;
}

void scene_shutdown(VividRendererHost* host, gpointer backend_data)
{
    (void)host;
    auto* worker = static_cast<SceneWorker*>(backend_data);
    if (!worker->locks_initialized)
        return;
    stop_frame_thread(worker);
    if (worker->producer) {
        g_mutex_lock(&worker->backend_lock);
        vivid_scene_producer_buffer_set_clear(&worker->pool);
        vivid_scene_producer_free(worker->producer);
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
    SceneWorker worker;
    const VividRendererHostBackend backend {
        .renderer_id = "vivid.scene",
        .kind = VIVID_RENDERER_KIND_SCENE,
        .initialize = scene_initialize,
        .negotiate_buffers = scene_negotiate,
        .runtime_command = scene_runtime,
        .quiesce = scene_quiesce,
        .shutdown = scene_shutdown,
    };
    const int result = vivid_renderer_host_run(argc, argv, &backend, &worker);
    scene_shutdown(nullptr, &worker);
    if (worker.locks_initialized) {
        g_mutex_clear(&worker.state_lock);
        g_mutex_clear(&worker.backend_lock);
    }
    return result;
}
