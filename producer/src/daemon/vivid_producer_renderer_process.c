#include "vivid_producer_renderer.h"

#include "vivid_renderer_manager.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

typedef enum
{
    VIVID_PROJECT_NONE,
    VIVID_PROJECT_SCENE,
    VIVID_PROJECT_WEB,
    VIVID_PROJECT_VIDEO,
} VividProjectKind;

typedef struct
{
    VividProjectKind kind;
    gchar* resource_path;
} VividProjectTarget;

struct _VividProducerRenderer
{
    gint ref_count;
    const VividRendererRegistry* registry;
    VividRendererManager* manager;
    VividRendererProcess* process;
    gchar* route_id;

    VividGpuDeviceList gpu_devices;
    VividGpuDevice resolved_gpu;
    gboolean resolved_gpu_valid;

    gchar* project_path;
    gchar* resource_path;
    gchar* user_properties_json;
    gchar* media_state_json;
    GVariant* audio_samples;
    gchar* render_device;
    VividProjectKind project_kind;
    const VividRendererDescriptor* descriptor;
    gboolean muted;
    gint volume;
    gint content_fit;
    gint fps;
    gboolean gfx_reflections;
    gboolean playback_paused;
    gboolean playback_stopped;
    guint32 target_width;
    guint32 target_height;
    gdouble target_scale;

    gchar* desired_identity_hash;
    gchar* startup_error;
    guint64 generation;
    gboolean waiting_for_unbind;
    gboolean process_was_active;
    gboolean process_stop_requested;
    gboolean negotiation_sent;
    guint32 negotiated_fourcc;
    guint64 negotiated_modifier;
    guint32 negotiated_plane_count;
    guint32 negotiated_memory_source;
    guint32 negotiated_pool_size;

    guint64 last_published_release_point;
    guint64 last_signaled_release_point;
    GHashTable* completed_release_points;
    GMutex release_lock;
    VividProducerRendererProgressFunc progress_callback;
    gpointer progress_data;
};

static void renderer_reconcile(VividProducerRenderer* renderer,
                               const gchar* reason);
static void renderer_send_runtime_state(VividProducerRenderer* renderer);

static void
renderer_notify_progress(VividProducerRenderer* renderer)
{
    if (renderer && renderer->progress_callback)
        renderer->progress_callback(renderer, renderer->progress_data);
}

static gboolean
renderer_collect_reaped_idle(gpointer user_data)
{
    VividProducerRenderer* renderer = user_data;
    vivid_renderer_manager_collect_reaped(renderer->manager);
    return G_SOURCE_REMOVE;
}

static void
renderer_schedule_reaped_collection(VividProducerRenderer* renderer)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    renderer_collect_reaped_idle,
                    vivid_producer_renderer_ref(renderer),
                    (GDestroyNotify)vivid_producer_renderer_free);
}

static const gchar*
default_media_state_json(void)
{
    return "{\"title\":\"\",\"artist\":\"\",\"albumTitle\":\"\","
           "\"albumArtist\":\"\",\"subTitle\":\"\",\"genres\":\"\","
           "\"contentType\":\"\",\"hasThumbnail\":false,"
           "\"playbackState\":0,\"primaryColor\":[0,0,0],"
           "\"secondaryColor\":[1,1,1],\"tertiaryColor\":[1,1,1],"
           "\"textColor\":[1,1,1],\"highContrastColor\":[1,1,1],"
           "\"thumbnailPath\":\"\"}";
}

static GVariant*
new_silent_audio_samples(void)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ad"));
    for (guint32 i = 0; i < VIVID_RENDERER_MAX_AUDIO_SAMPLES; i++)
        g_variant_builder_add(&builder, "d", 0.0);
    return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static gboolean
path_has_suffix(const gchar* path, const gchar* suffix)
{
    if (!path || !suffix)
        return FALSE;
    g_autofree gchar* lower = g_ascii_strdown(path, -1);
    return g_str_has_suffix(lower, suffix);
}

static gboolean
path_is_scene_entry(const gchar* path)
{
    return path_has_suffix(path, ".scene") ||
        path_has_suffix(path, ".pkg") ||
        path_has_suffix(path, ".json");
}

static gboolean
path_is_web_entry(const gchar* path)
{
    return path_has_suffix(path, ".html") || path_has_suffix(path, ".htm");
}

static gchar*
project_entry_path(const gchar* project_dir, const gchar* entry)
{
    if (!entry || !*entry)
        return NULL;
    gchar* path = g_path_is_absolute(entry)
        ? g_strdup(entry)
        : g_build_filename(project_dir, entry, NULL);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(path);
        return NULL;
    }
    return path;
}

static VividProjectTarget
resolve_project_target(const gchar* project_path)
{
    VividProjectTarget target = {0};
    if (!project_path || !*project_path)
        return target;
    if (g_file_test(project_path, G_FILE_TEST_IS_REGULAR)) {
        target.kind = path_is_scene_entry(project_path)
            ? VIVID_PROJECT_SCENE
            : path_is_web_entry(project_path)
                ? VIVID_PROJECT_WEB
                : VIVID_PROJECT_VIDEO;
        target.resource_path = g_strdup(project_path);
        return target;
    }
    if (!g_file_test(project_path, G_FILE_TEST_IS_DIR))
        return target;

    g_autofree gchar* project_json =
        g_build_filename(project_path, "project.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    if (g_file_test(project_json, G_FILE_TEST_IS_REGULAR) &&
        json_parser_load_from_file(parser, project_json, &error)) {
        JsonNode* root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject* object = json_node_get_object(root);
            const gchar* type = json_object_has_member(object, "type")
                ? json_object_get_string_member(object, "type")
                : NULL;
            const gchar* entry = json_object_has_member(object, "file")
                ? json_object_get_string_member(object, "file")
                : NULL;
            g_autofree gchar* entry_path = project_entry_path(project_path, entry);
            if (type && g_ascii_strcasecmp(type, "video") == 0 && entry_path) {
                target.kind = VIVID_PROJECT_VIDEO;
                target.resource_path = g_steal_pointer(&entry_path);
            } else if (type && g_ascii_strcasecmp(type, "web") == 0) {
                target.kind = VIVID_PROJECT_WEB;
                target.resource_path = g_strdup(project_path);
            } else if ((type && g_ascii_strcasecmp(type, "scene") == 0) ||
                       (!type && entry_path && path_is_scene_entry(entry_path))) {
                target.kind = VIVID_PROJECT_SCENE;
                target.resource_path = g_strdup(project_path);
            } else if (!type && entry_path && path_is_web_entry(entry_path)) {
                target.kind = VIVID_PROJECT_WEB;
                target.resource_path = g_strdup(project_path);
            }
        }
    } else if (error) {
        g_warning("VividProducerRenderer: project manifest parse failed path=%s: %s",
                  project_json,
                  error->message);
    }
    if (target.kind == VIVID_PROJECT_NONE) {
        g_autofree gchar* scene_pkg =
            g_build_filename(project_path, "scene.pkg", NULL);
        if (g_file_test(scene_pkg, G_FILE_TEST_IS_REGULAR)) {
            target.kind = VIVID_PROJECT_SCENE;
            target.resource_path = g_strdup(project_path);
        }
    }
    return target;
}

static const gchar*
project_kind_name(VividProjectKind kind)
{
    switch (kind) {
    case VIVID_PROJECT_SCENE: return "scene";
    case VIVID_PROJECT_WEB: return "web";
    case VIVID_PROJECT_VIDEO: return "video";
    case VIVID_PROJECT_NONE:
    default: return "none";
    }
}

static void
renderer_resolve_gpu(VividProducerRenderer* renderer)
{
    renderer->resolved_gpu_valid = vivid_gpu_devices_resolve(
        &renderer->gpu_devices,
        renderer->render_device,
        &renderer->resolved_gpu);
    if (!renderer->resolved_gpu_valid) {
        g_warning("VividProducerRenderer: route=%s render-device='%s' did not resolve",
                  renderer->route_id,
                  renderer->render_device);
        memset(&renderer->resolved_gpu, 0, sizeof(renderer->resolved_gpu));
    }
}

static gchar*
settings_json(VividProducerRenderer* renderer,
              gboolean include_identity,
              gboolean include_properties)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);

    /*
     * The registry owns backend-specific settings and defaults. Keeping the
     * merge generic means a new renderer can declare its settings without
     * adding kind checks to the daemon-side process orchestration path.
     */
    const gchar* default_documents[] = {
        vivid_renderer_descriptor_runtime_defaults_json(renderer->descriptor),
        include_identity
            ? vivid_renderer_descriptor_identity_defaults_json(renderer->descriptor)
            : NULL,
    };
    for (guint document_i = 0;
         document_i < G_N_ELEMENTS(default_documents);
         document_i++) {
        const gchar* document = default_documents[document_i];
        if (!document)
            continue;
        g_autoptr(JsonParser) parser = json_parser_new();
        if (!json_parser_load_from_data(parser, document, -1, NULL))
            continue;
        JsonObject* defaults = json_node_get_object(json_parser_get_root(parser));
        g_autoptr(GList) members = json_object_get_members(defaults);
        for (const GList* item = members; item; item = item->next) {
            const gchar* name = item->data;
            json_builder_set_member_name(builder, name);
            json_builder_add_value(
                builder,
                json_node_copy(json_object_get_member(defaults, name)));
        }
    }
    if (vivid_renderer_descriptor_has_runtime_setting(renderer->descriptor,
                                                       "content-fit")) {
        json_builder_set_member_name(builder, "content-fit");
        json_builder_add_int_value(builder, renderer->content_fit);
    }
    if (vivid_renderer_descriptor_has_runtime_setting(renderer->descriptor, "fps")) {
        json_builder_set_member_name(builder, "fps");
        json_builder_add_int_value(builder, renderer->fps);
    }
    if (vivid_renderer_descriptor_has_runtime_setting(renderer->descriptor,
                                                       "gfx-reflections")) {
        json_builder_set_member_name(builder, "gfx-reflections");
        json_builder_add_boolean_value(builder, renderer->gfx_reflections);
    }
    if (include_properties &&
        vivid_renderer_descriptor_has_runtime_setting(renderer->descriptor,
                                                       "user-properties")) {
        g_autoptr(JsonParser) parser = json_parser_new();
        if (json_parser_load_from_data(parser,
                                       renderer->user_properties_json,
                                       -1,
                                       NULL)) {
            json_builder_set_member_name(builder, "user-properties");
            json_builder_add_value(
                builder,
                json_node_copy(json_parser_get_root(parser)));
        }
    }
    json_builder_end_object(builder);
    JsonNode* root = json_builder_get_root(builder);
    g_autofree gchar* json = json_to_string(root, FALSE);
    json_node_free(root);
    return g_steal_pointer(&json);
}

static gchar*
renderer_build_identity_hash(VividProducerRenderer* renderer)
{
    if (!renderer->descriptor || !renderer->resource_path ||
        !renderer->resolved_gpu_valid || renderer->target_width == 0 ||
        renderer->target_height == 0) {
        return NULL;
    }
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
#define HASH_TEXT(value) \
    g_checksum_update(checksum, (const guchar*)(value), strlen(value) + 1)
    HASH_TEXT(vivid_renderer_descriptor_id(renderer->descriptor));
    HASH_TEXT(renderer->resource_path);
    HASH_TEXT(renderer->resolved_gpu.render_node);
    g_checksum_update(checksum,
                      renderer->resolved_gpu.uuid,
                      sizeof(renderer->resolved_gpu.uuid));
    g_checksum_update(checksum,
                      (const guchar*)&renderer->target_width,
                      sizeof(renderer->target_width));
    g_checksum_update(checksum,
                      (const guchar*)&renderer->target_height,
                      sizeof(renderer->target_height));
    g_checksum_update(checksum,
                      (const guchar*)&renderer->target_scale,
                      sizeof(renderer->target_scale));
    HASH_TEXT(vivid_renderer_descriptor_identity_defaults_json(
        renderer->descriptor));
#undef HASH_TEXT
    return g_strdup(g_checksum_get_string(checksum));
}

static gboolean
renderer_should_run(const VividProducerRenderer* renderer)
{
    return renderer && renderer->descriptor && renderer->resource_path &&
        renderer->resolved_gpu_valid && renderer->target_width != 0 &&
        renderer->target_height != 0 && !renderer->playback_stopped;
}

static guint8*
build_init_payload(VividProducerRenderer* renderer, gsize* out_length)
{
    g_autofree gchar* renderer_settings = settings_json(renderer, TRUE, FALSE);
    const gchar* renderer_id = vivid_renderer_descriptor_id(renderer->descriptor);
    const gchar* properties = renderer->user_properties_json
        ? renderer->user_properties_json
        : "{}";
    const guint32 lengths[] = {
        (guint32)strlen(renderer_id),
        (guint32)strlen(renderer->resource_path),
        (guint32)strlen(renderer->resolved_gpu.render_node),
        (guint32)strlen(renderer_settings),
        (guint32)strlen(properties),
    };
    *out_length = VIVID_RENDERER_INIT_FIXED_BYTES;
    for (guint i = 0; i < G_N_ELEMENTS(lengths); i++)
        *out_length += lengths[i];
    guint8* payload = g_malloc0(*out_length);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_INIT_RENDERER_KIND_OFFSET,
                                  vivid_renderer_descriptor_kind(renderer->descriptor));
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_INIT_WIDTH_OFFSET,
                                  renderer->target_width);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_INIT_HEIGHT_OFFSET,
                                  renderer->target_height);
    vivid_renderer_wire_write_f32(payload + VIVID_RENDERER_INIT_SCALE_OFFSET,
                                  (gfloat)renderer->target_scale);
    vivid_renderer_wire_write_f32(payload + VIVID_RENDERER_INIT_FPS_OFFSET,
                                  (gfloat)renderer->fps);
    guint32 playback_flags = renderer->playback_paused
        ? 0u
        : VIVID_RENDERER_PLAYBACK_FLAG_PLAYING;
    if (renderer->muted)
        playback_flags |= VIVID_RENDERER_PLAYBACK_FLAG_MUTED;
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_INIT_PLAYBACK_FLAGS_OFFSET,
                                  playback_flags);
    vivid_renderer_wire_write_f32(payload + VIVID_RENDERER_INIT_VOLUME_OFFSET,
                                  (gfloat)renderer->volume / 100.0f);
    memcpy(payload + VIVID_RENDERER_INIT_EXPECTED_DEVICE_UUID_OFFSET,
           renderer->resolved_gpu.uuid,
           sizeof(renderer->resolved_gpu.uuid));
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_INIT_RENDERER_ID_LENGTH_OFFSET,
        lengths[0]);
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_INIT_PROJECT_PATH_LENGTH_OFFSET,
        lengths[1]);
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_INIT_RENDER_NODE_LENGTH_OFFSET,
        lengths[2]);
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_INIT_SETTINGS_JSON_LENGTH_OFFSET,
        lengths[3]);
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_INIT_PROPERTIES_JSON_LENGTH_OFFSET,
        lengths[4]);
    const gchar* strings[] = {
        renderer_id,
        renderer->resource_path,
        renderer->resolved_gpu.render_node,
        renderer_settings,
        properties,
    };
    guint8* cursor = payload + VIVID_RENDERER_INIT_FIXED_BYTES;
    for (guint i = 0; i < G_N_ELEMENTS(strings); i++) {
        memcpy(cursor, strings[i], lengths[i]);
        cursor += lengths[i];
    }
    return payload;
}

static void
renderer_process_state_changed(VividRendererProcess* process,
                               VividRendererProcessState old_state,
                               VividRendererProcessState new_state,
                               gpointer user_data)
{
    (void)old_state;
    VividProducerRenderer* renderer = user_data;
    if (renderer->process != process)
        return;
    if (new_state == VIVID_RENDERER_PROCESS_INITIALIZING) {
        gsize payload_length = 0;
        g_autofree guint8* payload = build_init_payload(renderer, &payload_length);
        g_autoptr(GError) error = NULL;
        if (!vivid_renderer_process_send_init(process,
                                              payload,
                                              payload_length,
                                              NULL,
                                              &error)) {
            g_warning("VividProducerRenderer: route=%s INIT failed: %s",
                      renderer->route_id,
                      error->message);
            vivid_renderer_process_terminate(process, error->message);
        }
    } else if (new_state == VIVID_RENDERER_PROCESS_NEGOTIATING) {
        renderer_send_runtime_state(renderer);
    } else if (new_state == VIVID_RENDERER_PROCESS_ACTIVE) {
        renderer->process_was_active = TRUE;
    } else if (new_state == VIVID_RENDERER_PROCESS_UNBINDING) {
        renderer->waiting_for_unbind = TRUE;
        renderer->generation++;
    } else if (new_state == VIVID_RENDERER_PROCESS_CRASHED) {
        if (renderer->process_was_active && !renderer->waiting_for_unbind) {
            renderer->waiting_for_unbind = TRUE;
            renderer->generation++;
        }
    }
    if ((new_state == VIVID_RENDERER_PROCESS_FAILED ||
         new_state == VIVID_RENDERER_PROCESS_CRASHED) &&
        !renderer->process_was_active &&
        !renderer->process_stop_requested) {
        g_free(renderer->startup_error);
        renderer->startup_error = g_strdup(
            vivid_renderer_process_last_error(process)
                ? vivid_renderer_process_last_error(process)
                : "renderer worker exited before publishing its first frame");
    }
    renderer_notify_progress(renderer);
}

static void
renderer_process_packet(VividRendererProcess* process,
                        VividRendererPacket* packet,
                        gpointer user_data)
{
    VividProducerRenderer* renderer = user_data;
    if (renderer->process != process)
        return;
    if (packet->header.opcode == VIVID_RENDERER_MSG_BIND_BUFFERS) {
        renderer->generation++;
        g_message("VividProducerRenderer: route=%s instance=%" G_GUINT64_FORMAT
                  " accepted pool generation=%" G_GUINT64_FORMAT,
                  renderer->route_id,
                  vivid_renderer_process_instance_id(process),
                  renderer->generation);
    }

    /*
     * vivid_renderer_process has already validated and stored the packet.
     * Notify the route from the same main-context transaction.  FORMAT_CAPS
     * and BIND_BUFFERS must wake the route before a first frame can exist;
     * FRAME_READY then wakes delivery using the exact same edge-driven path.
     */
    renderer_notify_progress(renderer);
}

static void
renderer_process_reaped(VividRendererProcess* process,
                        gint wait_status,
                        gpointer user_data)
{
    VividProducerRenderer* renderer = user_data;
    if (renderer->process != process)
        return;
    g_message("VividProducerRenderer: route=%s instance=%" G_GUINT64_FORMAT
              " reaped status=0x%x waiting-unbind=%s",
              renderer->route_id,
              vivid_renderer_process_instance_id(process),
              wait_status,
              renderer->waiting_for_unbind ? "true" : "false");

    /*
     * The manager owns the process object and releases reaped entries from a
     * later main-context dispatch.  This renderer only borrows that object,
     * so detach the pointer before scheduling collection.  The consumer's
     * UNBIND_DONE may arrive after the child has already exited; its barrier
     * is represented by waiting_for_unbind and must not keep a dead process
     * address alive.
     */
    const gboolean waiting_for_unbind = renderer->waiting_for_unbind;
    g_mutex_lock(&renderer->release_lock);
    renderer->process = NULL;
    g_mutex_unlock(&renderer->release_lock);
    renderer_schedule_reaped_collection(renderer);
    if (!waiting_for_unbind)
        renderer_reconcile(renderer, "worker-reaped");
}

static void
renderer_reset_process_contract(VividProducerRenderer* renderer)
{
    renderer->negotiation_sent = FALSE;
    renderer->negotiated_fourcc = 0;
    renderer->negotiated_modifier = 0;
    renderer->negotiated_plane_count = 0;
    renderer->negotiated_memory_source = 0;
    renderer->negotiated_pool_size = 0;
    renderer->process_was_active = FALSE;
    renderer->process_stop_requested = FALSE;
    g_mutex_lock(&renderer->release_lock);
    renderer->last_published_release_point = 0;
    renderer->last_signaled_release_point = 0;
    g_hash_table_remove_all(renderer->completed_release_points);
    g_mutex_unlock(&renderer->release_lock);
}

static void
renderer_spawn(VividProducerRenderer* renderer)
{
    if (!renderer_should_run(renderer) || renderer->process || !renderer->manager ||
        renderer->startup_error)
        return;
    renderer_reset_process_contract(renderer);
    g_autoptr(GError) error = NULL;
    VividRendererProcess* process = vivid_renderer_manager_spawn(
        renderer->manager,
        renderer->descriptor,
        renderer->route_id,
        renderer->desired_identity_hash,
        &error);
    if (!process) {
        g_free(renderer->startup_error);
        renderer->startup_error = g_strdup(error->message);
        g_warning("VividProducerRenderer: route=%s spawn failed: %s",
                  renderer->route_id,
                  error->message);
        renderer_notify_progress(renderer);
        return;
    }
    g_mutex_lock(&renderer->release_lock);
    renderer->process = process;
    g_mutex_unlock(&renderer->release_lock);
}

static void
renderer_reconcile(VividProducerRenderer* renderer, const gchar* reason)
{
    g_autofree gchar* next_hash = renderer_build_identity_hash(renderer);
    const gboolean should_run = renderer_should_run(renderer);
    const gboolean same_identity = renderer->process && next_hash &&
        g_strcmp0(vivid_renderer_process_identity_hash(renderer->process),
                  next_hash) == 0;
    g_free(renderer->desired_identity_hash);
    renderer->desired_identity_hash = g_steal_pointer(&next_hash);

    if (!renderer->process) {
        if (should_run && !renderer->startup_error)
            renderer_spawn(renderer);
        return;
    }
    const VividRendererProcessState state =
        vivid_renderer_process_state(renderer->process);
    if (same_identity && should_run &&
        state != VIVID_RENDERER_PROCESS_FAILED &&
        state != VIVID_RENDERER_PROCESS_CRASHED &&
        state != VIVID_RENDERER_PROCESS_EXITED) {
        renderer_send_runtime_state(renderer);
        return;
    }
    if (renderer->waiting_for_unbind ||
        state == VIVID_RENDERER_PROCESS_QUIESCING ||
        state == VIVID_RENDERER_PROCESS_SHUTTING_DOWN ||
        state == VIVID_RENDERER_PROCESS_FAILED ||
        state == VIVID_RENDERER_PROCESS_CRASHED ||
        state == VIVID_RENDERER_PROCESS_EXITED) {
        return;
    }

    g_autoptr(GError) error = NULL;
    if (state == VIVID_RENDERER_PROCESS_NEGOTIATING ||
        state == VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME ||
        state == VIVID_RENDERER_PROCESS_ACTIVE) {
        if (!vivid_renderer_process_request_quiesce(renderer->process, &error)) {
            g_warning("VividProducerRenderer: route=%s quiesce failed reason=%s: %s",
                      renderer->route_id,
                      reason,
                      error->message);
            vivid_renderer_process_terminate(renderer->process, error->message);
        }
    } else {
        g_autofree gchar* diagnostic =
            g_strdup_printf("configuration superseded during %s: %s",
                            vivid_renderer_process_state_name(state),
                            reason ? reason : "configuration update");

        /*
         * A desired-state change can arrive before the worker reaches the
         * quiesce-capable states.  Terminating that obsolete instance is an
         * owner-requested stop, not a startup failure of the desired
         * generation.  Keep this intent attached to the current process
         * until it is reaped; the next spawn resets the process contract.
         */
        renderer->process_stop_requested = TRUE;
        vivid_renderer_process_terminate(renderer->process, diagnostic);
    }
}

static VividProducerRenderer*
renderer_new_internal(const VividRendererRegistry* registry,
                      const gchar* route_id,
                      const VividGpuDeviceList* gpu_devices)
{
    g_return_val_if_fail(registry != NULL, NULL);
    g_return_val_if_fail(route_id != NULL && *route_id != '\0', NULL);
    VividProducerRenderer* renderer = g_new0(VividProducerRenderer, 1);
    renderer->ref_count = 1;
    renderer->registry = registry;
    renderer->route_id = g_strdup(route_id);
    renderer->render_device = g_strdup("auto");
    renderer->project_path = g_strdup("");
    renderer->user_properties_json = g_strdup("{}");
    renderer->media_state_json = g_strdup(default_media_state_json());
    renderer->audio_samples = new_silent_audio_samples();
    renderer->volume = 50;
    renderer->content_fit = 1;
    renderer->fps = 30;
    renderer->gfx_reflections = TRUE;
    renderer->target_scale = 1.0;
    renderer->generation = 1;
    renderer->completed_release_points =
        g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    g_mutex_init(&renderer->release_lock);
    if (gpu_devices)
        renderer->gpu_devices = *gpu_devices;
    else if (!vivid_gpu_devices_enumerate(&renderer->gpu_devices))
        g_warning("VividProducerRenderer: GPU enumeration failed for route=%s",
                  route_id);
    renderer_resolve_gpu(renderer);
    const VividRendererProcessObserver observer = {
        .state_changed = renderer_process_state_changed,
        .packet_received = renderer_process_packet,
        .reaped = renderer_process_reaped,
    };
    g_autoptr(GError) error = NULL;
    renderer->manager = vivid_renderer_manager_new(registry,
                                                    NULL,
                                                    NULL,
                                                    &observer,
                                                    renderer,
                                                    &error);
    if (!renderer->manager) {
        g_warning("VividProducerRenderer: manager creation failed route=%s: %s",
                  route_id,
                  error->message);
    }
    return renderer;
}

VividProducerRenderer*
vivid_producer_renderer_new(const VividRendererRegistry* registry,
                            const gchar* route_id)
{
    return renderer_new_internal(registry, route_id, NULL);
}

VividProducerRenderer*
vivid_producer_renderer_new_from_gpu_devices(
    const VividRendererRegistry* registry,
    const gchar* route_id,
    const VividGpuDeviceList* gpu_devices)
{
    return renderer_new_internal(registry, route_id, gpu_devices);
}

VividProducerRenderer*
vivid_producer_renderer_ref(VividProducerRenderer* renderer)
{
    if (renderer)
        g_atomic_int_inc(&renderer->ref_count);
    return renderer;
}

void
vivid_producer_renderer_free(VividProducerRenderer* renderer)
{
    if (!renderer)
        return;
    if (!g_atomic_int_dec_and_test(&renderer->ref_count))
        return;
    if (renderer->process && !vivid_renderer_process_is_reaped(renderer->process))
        vivid_renderer_process_terminate(renderer->process, "renderer owner is shutting down");
    g_clear_pointer(&renderer->manager, vivid_renderer_manager_free);
    g_clear_pointer(&renderer->completed_release_points, g_hash_table_unref);
    g_mutex_clear(&renderer->release_lock);
    g_clear_pointer(&renderer->audio_samples, g_variant_unref);
    g_free(renderer->desired_identity_hash);
    g_free(renderer->startup_error);
    g_free(renderer->render_device);
    g_free(renderer->media_state_json);
    g_free(renderer->user_properties_json);
    g_free(renderer->resource_path);
    g_free(renderer->project_path);
    g_free(renderer->route_id);
    g_free(renderer);
}

void
vivid_producer_renderer_apply_config(VividProducerRenderer* renderer,
                                     const VividProducerConfig* config,
                                     const gchar* project_path,
                                     const gchar* user_properties_json)
{
    g_return_if_fail(renderer != NULL);
    g_return_if_fail(config != NULL);
    const gchar* next_project = project_path ? project_path : "";
    const gchar* next_properties = user_properties_json
        ? user_properties_json
        : "{}";
    const gchar* next_render_device = config->render_device && *config->render_device
        ? config->render_device
        : "auto";
    const gboolean identity_input_changed =
        g_strcmp0(renderer->project_path, next_project) != 0 ||
        g_strcmp0(renderer->render_device, next_render_device) != 0;

    renderer->muted = config->mute;
    renderer->volume = CLAMP(config->volume, 0, 100);
    renderer->content_fit = CLAMP(config->content_fit, 1, 3);
    renderer->fps = CLAMP(config->scene_fps, 5, 240);
    renderer->gfx_reflections = config->gfx_reflections;
    g_free(renderer->user_properties_json);
    renderer->user_properties_json = g_strdup(next_properties);
    if (identity_input_changed) {
        g_clear_pointer(&renderer->startup_error, g_free);
        g_free(renderer->project_path);
        renderer->project_path = g_strdup(next_project);
        g_free(renderer->render_device);
        renderer->render_device = g_strdup(next_render_device);
        renderer_resolve_gpu(renderer);
        VividProjectTarget target = resolve_project_target(next_project);
        renderer->project_kind = target.kind;
        g_free(renderer->resource_path);
        renderer->resource_path = target.resource_path;
        renderer->descriptor = target.kind == VIVID_PROJECT_NONE
            ? NULL
            : vivid_renderer_registry_lookup_wallpaper_type(
                  renderer->registry,
                  project_kind_name(target.kind));
        if (target.kind != VIVID_PROJECT_NONE && !renderer->descriptor) {
            g_warning("VividProducerRenderer: route=%s registry has no renderer for type=%s",
                      renderer->route_id,
                      project_kind_name(target.kind));
        }
        g_message("VividProducerRenderer: route=%s resolved project=%s type=%s resource=%s",
                  renderer->route_id,
                  next_project,
                  project_kind_name(target.kind),
                  renderer->resource_path ? renderer->resource_path : "(none)");
    }
    renderer_reconcile(renderer,
                       identity_input_changed ? "identity-config" : "runtime-config");
}

void
vivid_producer_renderer_set_target_extent(VividProducerRenderer* renderer,
                                          guint32 width,
                                          guint32 height,
                                          gdouble scale)
{
    if (!renderer)
        return;
    scale = MAX(scale, 1.0);
    if (renderer->target_width == width && renderer->target_height == height &&
        fabs(renderer->target_scale - scale) < 0.0001)
        return;
    g_clear_pointer(&renderer->startup_error, g_free);
    renderer->target_width = width;
    renderer->target_height = height;
    renderer->target_scale = scale;
    renderer_reconcile(renderer, "target-extent");
}

static void
renderer_send_json_command(VividProducerRenderer* renderer,
                           guint16 opcode,
                           const gchar* json,
                           guint32 fixed_bytes,
                           guint32 length_offset)
{
    if (!renderer->process || !json)
        return;
    const gsize length = strlen(json);
    g_autofree guint8* payload = g_malloc0((gsize)fixed_bytes + length);
    vivid_renderer_wire_write_u32(payload + length_offset, (guint32)length);
    memcpy(payload + fixed_bytes, json, length);
    g_autoptr(GError) error = NULL;
    if (!vivid_renderer_process_send_runtime(renderer->process,
                                             opcode,
                                             payload,
                                             (gsize)fixed_bytes + length,
                                             NULL,
                                             &error)) {
        g_debug("VividProducerRenderer: route=%s runtime opcode=0x%04x deferred: %s",
                renderer->route_id,
                opcode,
                error->message);
    }
}

static void
renderer_send_playback(VividProducerRenderer* renderer)
{
    if (!renderer->process)
        return;
    guint8 payload[VIVID_RENDERER_SET_PLAYBACK_FIXED_BYTES] = {0};
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_SET_PLAYBACK_PLAYING_OFFSET,
        renderer->playback_paused || renderer->playback_stopped ? 0u : 1u);
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_SET_PLAYBACK_MUTED_OFFSET,
        renderer->muted ? 1u : 0u);
    vivid_renderer_wire_write_f32(
        payload + VIVID_RENDERER_SET_PLAYBACK_VOLUME_OFFSET,
        (gfloat)renderer->volume / 100.0f);
    (void)vivid_renderer_process_send_runtime(renderer->process,
                                              VIVID_RENDERER_MSG_SET_PLAYBACK,
                                              payload,
                                              sizeof(payload),
                                              NULL,
                                              NULL);
}

static void
renderer_send_audio(VividProducerRenderer* renderer)
{
    if (!renderer->process || !renderer->descriptor ||
        !vivid_renderer_descriptor_supports_event(renderer->descriptor,
                                                  "audio-samples"))
        return;
    const gsize count = MIN(g_variant_n_children(renderer->audio_samples),
                            (gsize)VIVID_RENDERER_MAX_AUDIO_SAMPLES);
    const gsize payload_length = VIVID_RENDERER_SET_AUDIO_SAMPLES_FIXED_BYTES +
        count * sizeof(float);
    g_autofree guint8* payload = g_malloc0(payload_length);
    vivid_renderer_wire_write_u32(
        payload + VIVID_RENDERER_SET_AUDIO_SAMPLES_COUNT_OFFSET,
        (guint32)count);
    for (gsize i = 0; i < count; i++) {
        gdouble sample = 0.0;
        g_variant_get_child(renderer->audio_samples, i, "d", &sample);
        vivid_renderer_wire_write_f32(
            payload + VIVID_RENDERER_SET_AUDIO_SAMPLES_FIXED_BYTES +
                i * sizeof(float),
            (gfloat)MAX(sample, 0.0));
    }
    (void)vivid_renderer_process_send_runtime(renderer->process,
                                              VIVID_RENDERER_MSG_SET_AUDIO_SAMPLES,
                                              payload,
                                              payload_length,
                                              NULL,
                                              NULL);
}

static void
renderer_send_runtime_state(VividProducerRenderer* renderer)
{
    if (!renderer->process)
        return;
    const VividRendererProcessState state =
        vivid_renderer_process_state(renderer->process);
    if (state != VIVID_RENDERER_PROCESS_NEGOTIATING &&
        state != VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME &&
        state != VIVID_RENDERER_PROCESS_ACTIVE)
        return;
    g_autofree gchar* json = settings_json(renderer, FALSE, TRUE);
    renderer_send_json_command(renderer,
                               VIVID_RENDERER_MSG_SET_RUNTIME,
                               json,
                               VIVID_RENDERER_SET_RUNTIME_FIXED_BYTES,
                               VIVID_RENDERER_SET_RUNTIME_SETTINGS_JSON_LENGTH_OFFSET);
    renderer_send_playback(renderer);
    if (renderer->descriptor &&
        vivid_renderer_descriptor_supports_event(renderer->descriptor,
                                                  "media-state")) {
        renderer_send_json_command(
            renderer,
            VIVID_RENDERER_MSG_SET_MEDIA_STATE,
            renderer->media_state_json,
            VIVID_RENDERER_SET_MEDIA_STATE_FIXED_BYTES,
            VIVID_RENDERER_SET_MEDIA_STATE_MEDIA_STATE_JSON_LENGTH_OFFSET);
    }
    renderer_send_audio(renderer);
}

void
vivid_producer_renderer_set_playback_paused(VividProducerRenderer* renderer,
                                            gboolean paused)
{
    if (!renderer || renderer->playback_paused == !!paused)
        return;
    renderer->playback_paused = !!paused;
    renderer_send_playback(renderer);
}

void
vivid_producer_renderer_set_playback_stopped(VividProducerRenderer* renderer,
                                             gboolean stopped)
{
    if (!renderer || renderer->playback_stopped == !!stopped)
        return;
    renderer->playback_stopped = !!stopped;
    renderer_reconcile(renderer, "playback-stop-policy");
}

void
vivid_producer_renderer_set_media_state_json(VividProducerRenderer* renderer,
                                             const gchar* media_state_json)
{
    if (!renderer)
        return;
    g_free(renderer->media_state_json);
    renderer->media_state_json = g_strdup(media_state_json
                                              ? media_state_json
                                              : default_media_state_json());
    renderer_send_runtime_state(renderer);
}

void
vivid_producer_renderer_set_audio_samples(VividProducerRenderer* renderer,
                                          GVariant* audio_samples)
{
    if (!renderer)
        return;
    g_clear_pointer(&renderer->audio_samples, g_variant_unref);
    renderer->audio_samples = audio_samples
        ? g_variant_ref(audio_samples)
        : new_silent_audio_samples();
    renderer_send_audio(renderer);
}

guint64
vivid_producer_renderer_generation(VividProducerRenderer* renderer)
{
    return renderer ? renderer->generation : 0;
}

const gchar*
vivid_producer_renderer_project_path(VividProducerRenderer* renderer)
{
    return renderer ? renderer->project_path : NULL;
}

const gchar*
vivid_producer_renderer_user_properties_json(VividProducerRenderer* renderer)
{
    return renderer ? renderer->user_properties_json : NULL;
}

const VividGpuDeviceList*
vivid_producer_renderer_gpu_devices(VividProducerRenderer* renderer)
{
    return renderer ? &renderer->gpu_devices : NULL;
}

gboolean
vivid_producer_renderer_resolved_gpu(VividProducerRenderer* renderer,
                                     VividGpuDevice* out_device)
{
    if (!renderer || !renderer->resolved_gpu_valid || !out_device)
        return FALSE;
    *out_device = renderer->resolved_gpu;
    return TRUE;
}

gboolean
vivid_producer_renderer_write_frame(VividProducerRenderer* renderer,
                                    guint8* pixels,
                                    guint32 stride,
                                    guint32 width,
                                    guint32 height,
                                    guint64 sequence)
{
    (void)renderer;
    if (!pixels || stride < width * 4u)
        return FALSE;
    for (guint32 y = 0; y < height; y++) {
        guint8* row = pixels + (gsize)y * stride;
        for (guint32 x = 0; x < width; x++) {
            guint8* pixel = row + (gsize)x * 4u;
            pixel[0] = (guint8)(((x ^ y) + sequence * 7u) & 0xffu);
            pixel[1] = (guint8)((y + sequence * 5u) & 0xffu);
            pixel[2] = (guint8)((x + sequence * 3u) & 0xffu);
            pixel[3] = 0xffu;
        }
    }
    return TRUE;
}

gboolean
vivid_producer_renderer_prefers_dmabuf_buffers(VividProducerRenderer* renderer)
{
    return renderer && renderer->descriptor != NULL;
}

gboolean
vivid_producer_renderer_is_ready_for_dmabuf_negotiation(
    VividProducerRenderer* renderer)
{
    if (!renderer || !renderer->process || renderer->waiting_for_unbind ||
        !renderer->desired_identity_hash ||
        g_strcmp0(vivid_renderer_process_identity_hash(renderer->process),
                  renderer->desired_identity_hash) != 0) {
        return FALSE;
    }
    const VividRendererProcessState state =
        vivid_renderer_process_state(renderer->process);
    return state == VIVID_RENDERER_PROCESS_NEGOTIATING ||
        state == VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME ||
        state == VIVID_RENDERER_PROCESS_ACTIVE;
}

gboolean
vivid_producer_renderer_query_dmabuf_caps(
    VividProducerRenderer* renderer,
    VividProducerRendererDmaBufCaps* out_caps)
{
    if (!renderer || !out_caps || !renderer->process)
        return FALSE;
    const VividRendererProcessFormatCaps* caps =
        vivid_renderer_process_format_caps(renderer->process);
    if (!caps)
        return FALSE;
    memset(out_caps, 0, sizeof(*out_caps));
    out_caps->n_caps = MIN(caps->n_caps,
                           (guint32)VIVID_PRODUCER_RENDERER_DMABUF_MAX_CAPS);
    for (guint32 i = 0; i < out_caps->n_caps; i++) {
        out_caps->caps[i] = (VividProducerRendererDmaBufFormatCap) {
            .fourcc = caps->caps[i].fourcc,
            .modifier = caps->caps[i].modifier,
            .plane_count = caps->caps[i].plane_count,
        };
    }
    out_caps->memory_preference =
        (caps->memory_hints & VIVID_RENDERER_MEMORY_HINT_DEVICE_LOCAL) != 0
        ? VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
        : VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    return out_caps->n_caps != 0;
}

static void
producer_buffer_set_init(VividProducerRendererBufferSet* set)
{
    memset(set, 0, sizeof(*set));
    for (guint32 buffer = 0; buffer < VIVID_PRODUCER_RENDERER_MAX_BUFFERS; buffer++) {
        for (guint32 plane = 0; plane < VIVID_PRODUCER_RENDERER_MAX_PLANES; plane++)
            set->buffers[buffer].planes[plane].fd = -1;
    }
}

void
vivid_producer_renderer_buffer_set_clear(VividProducerRendererBufferSet* set)
{
    if (!set)
        return;
    for (guint32 buffer = 0;
         buffer < MIN(set->n_buffers,
                      (guint32)VIVID_PRODUCER_RENDERER_MAX_BUFFERS);
         buffer++) {
        for (guint32 plane = 0;
             plane < MIN(set->buffers[buffer].n_planes,
                         (guint32)VIVID_PRODUCER_RENDERER_MAX_PLANES);
             plane++) {
            if (set->buffers[buffer].planes[plane].fd >= 0)
                close(set->buffers[buffer].planes[plane].fd);
        }
    }
    producer_buffer_set_init(set);
}

static gboolean
request_allows_fourcc(const VividProducerRendererDmaBufRequest* request,
                      guint32 fourcc)
{
    if (!request || request->n_allowed_fourccs == 0)
        return TRUE;
    for (guint32 i = 0; i < request->n_allowed_fourccs; i++) {
        if (request->allowed_fourccs[i] == fourcc)
            return TRUE;
    }
    return FALSE;
}

static guint32
memory_source_from_request(const VividProducerRendererDmaBufRequest* request,
                           const VividRendererProcessFormatCaps* caps)
{
    if (request &&
        request->memory_preference == VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL)
        return VIVID_RENDERER_MEMORY_SOURCE_GPU_NATIVE;
    if (request &&
        request->memory_preference == VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE)
        return VIVID_RENDERER_MEMORY_SOURCE_GPU_LINEAR;
    return (caps->memory_hints & VIVID_RENDERER_MEMORY_HINT_DEVICE_LOCAL) != 0
        ? VIVID_RENDERER_MEMORY_SOURCE_GPU_NATIVE
        : VIVID_RENDERER_MEMORY_SOURCE_GPU_LINEAR;
}

VividProducerRendererDmaBufPrepareStatus
vivid_producer_renderer_prepare_dmabuf_buffers_ex(
    VividProducerRenderer* renderer,
    guint32 width,
    guint32 height,
    gdouble render_scale,
    const VividProducerRendererDmaBufRequest* request,
    VividProducerRendererBufferSet* out_set)
{
    if (!renderer || !out_set)
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    producer_buffer_set_init(out_set);
    vivid_producer_renderer_set_target_extent(renderer, width, height, render_scale);
    if (!vivid_producer_renderer_is_ready_for_dmabuf_negotiation(renderer))
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY;
    const VividRendererProcessFormatCaps* caps =
        vivid_renderer_process_format_caps(renderer->process);
    if (!caps)
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY;

    const VividRendererProcessState state =
        vivid_renderer_process_state(renderer->process);
    if (!renderer->negotiation_sent && state == VIVID_RENDERER_PROCESS_NEGOTIATING) {
        guint32 fourcc = request && request->n_allowed_fourccs != 0
            ? request->allowed_fourccs[0]
            : caps->caps[0].fourcc;
        guint64 modifier = request && request->require_modifier
            ? request->required_modifier
            : caps->caps[0].modifier;
        guint32 plane_count = request && request->required_plane_count != 0
            ? request->required_plane_count
            : caps->caps[0].plane_count;
        gboolean supported = FALSE;
        for (guint32 i = 0; i < caps->n_caps; i++) {
            if (caps->caps[i].fourcc == fourcc &&
                caps->caps[i].modifier == modifier &&
                caps->caps[i].plane_count == plane_count) {
                supported = TRUE;
                break;
            }
        }
        if (!supported)
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        guint8 payload[VIVID_RENDERER_NEGOTIATE_BUFFERS_FIXED_BYTES] = {0};
        const guint32 memory_source = memory_source_from_request(request, caps);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_FOURCC_OFFSET,
            fourcc);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_WIDTH_OFFSET,
            width);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_HEIGHT_OFFSET,
            height);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_PLANE_COUNT_OFFSET,
            plane_count);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_POOL_SIZE_OFFSET,
            caps->pool_size_min);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_MEMORY_SOURCE_OFFSET,
            memory_source);
        vivid_renderer_wire_write_u32(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_SYNC_MODE_OFFSET,
            VIVID_RENDERER_SYNC_CAP_SYNCOBJ_TIMELINE);
        vivid_renderer_wire_write_u64(
            payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_MODIFIER_OFFSET,
            modifier);
        g_autoptr(GError) error = NULL;
        if (!vivid_renderer_process_send_negotiate_buffers(renderer->process,
                                                           payload,
                                                           sizeof(payload),
                                                           NULL,
                                                           &error)) {
            g_warning("VividProducerRenderer: route=%s negotiation failed: %s",
                      renderer->route_id,
                      error->message);
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        }
        renderer->negotiation_sent = TRUE;
        renderer->negotiated_fourcc = fourcc;
        renderer->negotiated_modifier = modifier;
        renderer->negotiated_plane_count = plane_count;
        renderer->negotiated_memory_source = memory_source;
        renderer->negotiated_pool_size = caps->pool_size_min;
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY;
    }

    VividRendererProcessPool pool;
    g_autoptr(GError) error = NULL;
    if (!vivid_renderer_process_copy_pool(renderer->process, &pool, &error))
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY;
    if (pool.width != width || pool.height != height ||
        !request_allows_fourcc(request, pool.fourcc) ||
        (request && request->require_modifier &&
         request->required_modifier != pool.modifier) ||
        (request && request->required_plane_count != 0 &&
         request->required_plane_count != pool.buffers[0].n_planes)) {
        vivid_renderer_process_pool_clear(&pool);
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }
    out_set->width = pool.width;
    out_set->height = pool.height;
    out_set->fourcc = pool.fourcc;
    out_set->modifier = pool.modifier;
    out_set->premultiplied =
        (pool.flags & VIVID_RENDERER_BUFFER_FLAG_PREMULTIPLIED) != 0;
    out_set->n_buffers = pool.n_buffers;
    for (guint32 buffer = 0; buffer < pool.n_buffers; buffer++) {
        out_set->buffers[buffer].index = pool.buffers[buffer].index;
        out_set->buffers[buffer].n_planes = pool.buffers[buffer].n_planes;
        for (guint32 plane = 0; plane < pool.buffers[buffer].n_planes; plane++) {
            out_set->buffers[buffer].planes[plane].fd =
                pool.buffers[buffer].planes[plane].fd;
            pool.buffers[buffer].planes[plane].fd = -1;
            out_set->buffers[buffer].planes[plane].stride =
                pool.buffers[buffer].planes[plane].stride;
            out_set->buffers[buffer].planes[plane].offset =
                pool.buffers[buffer].planes[plane].offset;
            out_set->buffers[buffer].size = MAX(
                out_set->buffers[buffer].size,
                pool.buffers[buffer].planes[plane].size);
        }
    }
    vivid_renderer_process_pool_clear(&pool);
    return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK;
}

gboolean
vivid_producer_renderer_prepare_dmabuf_buffers(
    VividProducerRenderer* renderer,
    guint32 width,
    guint32 height,
    gdouble render_scale,
    const VividProducerRendererDmaBufRequest* request,
    VividProducerRendererBufferSet* out_set)
{
    return vivid_producer_renderer_prepare_dmabuf_buffers_ex(renderer,
                                                              width,
                                                              height,
                                                              render_scale,
                                                              request,
                                                              out_set) ==
        VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK;
}

gboolean
vivid_producer_renderer_next_dmabuf_frame(VividProducerRenderer* renderer,
                                          VividProducerRendererFrame* out_frame)
{
    if (!renderer || !renderer->process || !out_frame ||
        renderer->waiting_for_unbind)
        return FALSE;
    VividRendererProcessFrame frame = { .acquire_sync_fd = -1 };
    if (!vivid_renderer_process_take_frame(renderer->process, &frame))
        return FALSE;
    *out_frame = (VividProducerRendererFrame) {
        .buffer_index = frame.buffer_index,
        .source_frame_id = (gint32)frame.buffer_index,
        .sequence = frame.sequence,
        .target_time_usec = frame.target_time_usec,
        .acquire_sync_fd = frame.acquire_sync_fd,
        .release_point = frame.release_point,
        .renderer_instance_id =
            vivid_renderer_process_instance_id(renderer->process),
    };
    frame.acquire_sync_fd = -1;
    g_mutex_lock(&renderer->release_lock);
    renderer->last_published_release_point = MAX(
        renderer->last_published_release_point,
        out_frame->release_point);
    g_mutex_unlock(&renderer->release_lock);
    return TRUE;
}

guint
vivid_producer_renderer_pending_dmabuf_frame_count(
    VividProducerRenderer* renderer)
{
    return renderer && renderer->process
        ? vivid_renderer_process_pending_frame_count(renderer->process)
        : 0;
}

void
vivid_producer_renderer_set_progress_callback(
    VividProducerRenderer* renderer,
    VividProducerRendererProgressFunc callback,
    gpointer user_data)
{
    if (!renderer)
        return;
    renderer->progress_callback = callback;
    renderer->progress_data = user_data;
}

gboolean
vivid_producer_renderer_requires_worker(VividProducerRenderer* renderer)
{
    return renderer && renderer->project_path && *renderer->project_path &&
        renderer->target_width != 0 && renderer->target_height != 0 &&
        !renderer->playback_stopped;
}

gboolean
vivid_producer_renderer_worker_is_active(VividProducerRenderer* renderer)
{
    if (!renderer || !renderer->process || renderer->waiting_for_unbind ||
        !renderer->desired_identity_hash)
        return FALSE;
    return vivid_renderer_process_state(renderer->process) ==
            VIVID_RENDERER_PROCESS_ACTIVE &&
        g_strcmp0(vivid_renderer_process_identity_hash(renderer->process),
                  renderer->desired_identity_hash) == 0;
}

gboolean
vivid_producer_renderer_has_live_worker(VividProducerRenderer* renderer)
{
    return renderer && renderer->process &&
        !vivid_renderer_process_is_reaped(renderer->process);
}

const gchar*
vivid_producer_renderer_startup_error(VividProducerRenderer* renderer)
{
    if (!renderer)
        return "renderer is unavailable";
    if (renderer->startup_error)
        return renderer->startup_error;
    if (vivid_producer_renderer_requires_worker(renderer)) {
        if (!renderer->resource_path)
            return "configured project does not resolve to a renderer resource";
        if (!renderer->descriptor)
            return "renderer registry has no worker for the configured project type";
        if (!renderer->resolved_gpu_valid)
            return "configured render device did not resolve to a GPU";
        if (!renderer->manager)
            return "renderer process manager is unavailable";
    }
    return NULL;
}

void
vivid_producer_renderer_clear_startup_error(VividProducerRenderer* renderer)
{
    if (renderer)
        g_clear_pointer(&renderer->startup_error, g_free);
}

gboolean
vivid_producer_renderer_request_dmabuf_frame(VividProducerRenderer* renderer,
                                             const gchar* reason)
{
    (void)reason;
    return renderer && renderer->process && !renderer->waiting_for_unbind;
}

void
vivid_producer_renderer_set_release_gate(VividProducerRenderer* renderer,
                                         const VividRendererReleaseGate* gate)
{
    (void)renderer;
    (void)gate;
}

gboolean
vivid_producer_renderer_get_clear_rgba(VividProducerRenderer* renderer,
                                       gfloat rgba[4])
{
    if (!renderer || !rgba)
        return FALSE;
    const gboolean web = renderer->project_kind == VIVID_PROJECT_WEB;
    rgba[0] = web ? 1.0f : 0.0f;
    rgba[1] = web ? 1.0f : 0.0f;
    rgba[2] = web ? 1.0f : 0.0f;
    rgba[3] = 1.0f;
    return TRUE;
}

static void
renderer_send_pointer(VividProducerRenderer* renderer,
                      guint16 opcode,
                      const guint8* payload,
                      gsize payload_length,
                      const gchar* event_name)
{
    if (!renderer || !renderer->process || !renderer->descriptor ||
        !vivid_renderer_descriptor_supports_event(renderer->descriptor, event_name))
        return;
    (void)vivid_renderer_process_send_runtime(renderer->process,
                                              opcode,
                                              payload,
                                              payload_length,
                                              NULL,
                                              NULL);
}

void
vivid_producer_renderer_pointer_motion(VividProducerRenderer* renderer,
                                       gdouble x,
                                       gdouble y)
{
    guint8 payload[VIVID_RENDERER_POINTER_MOTION_FIXED_BYTES] = {0};
    vivid_renderer_wire_write_f64(payload + VIVID_RENDERER_POINTER_MOTION_X_OFFSET, x);
    vivid_renderer_wire_write_f64(payload + VIVID_RENDERER_POINTER_MOTION_Y_OFFSET, y);
    renderer_send_pointer(renderer,
                          VIVID_RENDERER_MSG_POINTER_MOTION,
                          payload,
                          sizeof(payload),
                          "pointer-motion");
}

void
vivid_producer_renderer_pointer_button(VividProducerRenderer* renderer,
                                       guint32 button,
                                       gboolean pressed)
{
    guint8 payload[VIVID_RENDERER_POINTER_BUTTON_FIXED_BYTES] = {0};
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_POINTER_BUTTON_BUTTON_OFFSET,
                                  button);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_POINTER_BUTTON_STATE_OFFSET,
                                  pressed ? 1u : 0u);
    renderer_send_pointer(renderer,
                          VIVID_RENDERER_MSG_POINTER_BUTTON,
                          payload,
                          sizeof(payload),
                          "pointer-button");
}

void
vivid_producer_renderer_pointer_axis(VividProducerRenderer* renderer,
                                     gdouble delta_x,
                                     gdouble delta_y)
{
    guint8 payload[VIVID_RENDERER_POINTER_AXIS_FIXED_BYTES] = {0};
    vivid_renderer_wire_write_f64(payload + VIVID_RENDERER_POINTER_AXIS_DELTA_X_OFFSET,
                                  delta_x);
    vivid_renderer_wire_write_f64(payload + VIVID_RENDERER_POINTER_AXIS_DELTA_Y_OFFSET,
                                  delta_y);
    renderer_send_pointer(renderer,
                          VIVID_RENDERER_MSG_POINTER_AXIS,
                          payload,
                          sizeof(payload),
                          "pointer-axis");
}

gboolean
vivid_producer_renderer_is_waiting_for_unbind(VividProducerRenderer* renderer)
{
    return renderer && renderer->waiting_for_unbind;
}

static gboolean
renderer_signal_point(VividProducerRenderer* renderer,
                      guint64 release_point,
                      GError** error)
{
    if (!renderer->process || vivid_renderer_process_is_reaped(renderer->process))
        return TRUE;
    return vivid_renderer_process_signal_release_point(renderer->process,
                                                       release_point,
                                                       error);
}

gboolean
vivid_producer_renderer_complete_frame_release(
    VividProducerRenderer* renderer,
    guint64 renderer_instance_id,
    guint64 release_point,
    GError** error)
{
    if (!renderer || release_point == 0)
        return FALSE;
    g_mutex_lock(&renderer->release_lock);
    if (!renderer->process ||
        vivid_renderer_process_instance_id(renderer->process) != renderer_instance_id) {
        g_mutex_unlock(&renderer->release_lock);
        return TRUE;
    }
    if (release_point <= renderer->last_signaled_release_point) {
        g_mutex_unlock(&renderer->release_lock);
        return TRUE;
    }
    guint64* completed = g_new(guint64, 1);
    *completed = release_point;
    g_hash_table_add(renderer->completed_release_points, completed);
    guint64 next = renderer->last_signaled_release_point + 1;
    guint64 highest = renderer->last_signaled_release_point;
    while (g_hash_table_contains(renderer->completed_release_points, &next)) {
        g_hash_table_remove(renderer->completed_release_points, &next);
        highest = next++;
    }
    if (highest == renderer->last_signaled_release_point) {
        g_mutex_unlock(&renderer->release_lock);
        return TRUE;
    }
    if (!renderer_signal_point(renderer, highest, error)) {
        g_mutex_unlock(&renderer->release_lock);
        return FALSE;
    }
    renderer->last_signaled_release_point = highest;
    g_mutex_unlock(&renderer->release_lock);
    return TRUE;
}

gboolean
vivid_producer_renderer_complete_unbind(VividProducerRenderer* renderer,
                                        GError** error)
{
    if (!renderer || !renderer->waiting_for_unbind)
        return TRUE;

    VividRendererProcess* process_to_shutdown = NULL;
    g_mutex_lock(&renderer->release_lock);
    if (renderer->process && !vivid_renderer_process_is_reaped(renderer->process)) {
        /*
         * QUIESCED proves that no later FRAME_READY can arrive. The process
         * queue may still contain frames that were accepted after the final
         * consumer detached but before QUIESCE reached the worker. Signal the
         * highest protocol-validated point, not merely the highest frame that
         * was forwarded, so those unpublished slots cannot hold shutdown open.
         */
        const guint64 final_release_point = MAX(
            renderer->last_published_release_point,
            vivid_renderer_process_last_release_point(renderer->process));
        if (final_release_point != 0 &&
            !renderer_signal_point(renderer,
                                   final_release_point,
                                   error)) {
            g_mutex_unlock(&renderer->release_lock);
            return FALSE;
        }
        renderer->last_signaled_release_point = final_release_point;
        g_hash_table_remove_all(renderer->completed_release_points);
        if (vivid_renderer_process_state(renderer->process) ==
            VIVID_RENDERER_PROCESS_UNBINDING) {
            process_to_shutdown = renderer->process;
        }

        /*
         * request_shutdown() transitions synchronously and invokes the process
         * observer. Clear the barrier before dropping the mutex and sending the
         * command so that observer re-entry cannot attempt to acquire this lock
         * for the same completed UNBIND transaction.
         */
        renderer->waiting_for_unbind = FALSE;
        g_mutex_unlock(&renderer->release_lock);
        if (process_to_shutdown &&
            !vivid_renderer_process_request_shutdown(process_to_shutdown, error)) {
            g_mutex_lock(&renderer->release_lock);
            if (renderer->process == process_to_shutdown &&
                vivid_renderer_process_state(process_to_shutdown) ==
                    VIVID_RENDERER_PROCESS_UNBINDING) {
                renderer->waiting_for_unbind = TRUE;
            }
            g_mutex_unlock(&renderer->release_lock);
            return FALSE;
        }
        return TRUE;
    }
    renderer->waiting_for_unbind = FALSE;
    renderer->process = NULL;
    g_mutex_unlock(&renderer->release_lock);
    renderer_reconcile(renderer, "crashed-worker-unbound");
    return TRUE;
}
