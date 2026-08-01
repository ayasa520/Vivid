/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#define _GNU_SOURCE

#include "vivid_auto_policy.h"
#include "vivid_producer_config.h"
#include "vivid_route_table.h"
#include "vivid_window_facts.h"

#include <glib.h>
#include "vivid_dmabuf_negotiation.h"
#include "vivid_producer_renderer.h"
#include "vivid_render_layout.h"
#include "vivid_unbind_ack_tracker.h"

#include "../protocol/vivid_display_protocol.h"

#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glib-unix.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>

#define DEFAULT_FPS 30u
#define DRM_RENDER_NODE_FIRST 128
#define DRM_RENDER_NODE_LAST 191
#define OUTPUT_MAX_DIMENSION 8192u
#define RENDERER_DMABUF_RETRY_INTERVAL_USEC G_USEC_PER_SEC
#define MEDIA_AUDIO_SAMPLE_MAX_VALUES 512u
#define RELEASE_REAPER_TIMEOUT_MSEC 500u
#define RELEASE_GATE_WAIT_TIMEOUT_MSEC 600u
#define UNBIND_ACK_TIMEOUT_MSEC 150u
#define CLIENT_SOCKET_SOURCE_PRIORITY G_PRIORITY_HIGH
#define CLIENT_OUTBOX_MAX_BYTES (256u * 1024u)
#define DMABUF_CAPS_VERSION 3u
#define DMABUF_RELAY_MODE_SHADOW_COPY "shadow-copy-v1"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#define VIVID_PRODUCER_BUFFER_ERROR (vivid_producer_buffer_error_quark())

typedef enum
{
    VIVID_PRODUCER_BUFFER_ERROR_FAILED,
} VividProducerBufferError;

typedef struct _Producer Producer;
typedef struct _Client Client;
typedef struct _Output Output;
typedef struct _RenderRoute RenderRoute;
typedef struct _ReleaseGroup ReleaseGroup;
typedef struct _RenderRouteReleaseGateContext RenderRouteReleaseGateContext;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} ConsumerDmaBufModifier;

typedef struct
{
    gboolean present;
    guint32 version;
    gboolean skips_external_only_modifiers;
    gchar* backend;
    gchar* probe;
    gchar* render_node;
    gchar* vendor;
    gchar* pci_address;
    gchar* device_uuid;
    gchar* driver_uuid;
    gchar* identity_mode;
    GArray* fourccs;                 /* guint32 */
    GArray* implicit_linear_fourccs;  /* guint32 */
    GArray* modifiers;               /* ConsumerDmaBufModifier */
    VividDmaBufPeerCaps peer_caps;
    gboolean hint_host_visible;
    gboolean hint_device_local;
    gboolean hint_implicit_linear;
    gboolean sync_explicit_acquire;
    gboolean sync_release_syncobj;
    gboolean relay_shadow_copy;
} ConsumerDmaBufCaps;

typedef struct
{
    GSource source;
    guint fps;
    guint64 tick_index;
    gint64 start_time_usec;
    gint64 next_due_usec;
} FrameClockSource;

typedef enum
{
    OUTPUT_MEMORY_GBM,
    OUTPUT_MEMORY_RENDERER_DMABUF,
} OutputMemoryKind;

typedef enum
{
    OUTPUT_DMABUF_PATH_UNNEGOTIATED,
    OUTPUT_DMABUF_PATH_OPTIMIZED_SAME_DEVICE,
    OUTPUT_DMABUF_PATH_COMPAT_LINEAR,
    OUTPUT_DMABUF_PATH_COMPAT_CPU_READBACK_RESERVED,
} OutputDmaBufPath;

typedef enum
{
    OUTPUT_REBIND_PREPARE_READY,
    OUTPUT_REBIND_PREPARE_NOT_READY,
    OUTPUT_REBIND_PREPARE_FAILED,
} OutputRebindPrepareState;

typedef enum
{
    OUTPUT_REBIND_RESULT_READY,
    OUTPUT_REBIND_RESULT_NOT_READY,
    OUTPUT_REBIND_RESULT_FAILED,
} OutputRebindResult;

typedef struct
{
    guint32 index;
    gsize   size;
    guint32 n_planes;
    gint    plane_fds[VIVID_DISPLAY_DMABUF_MAX_PLANES];
    guint32 plane_strides[VIVID_DISPLAY_DMABUF_MAX_PLANES];
    guint32 plane_offsets[VIVID_DISPLAY_DMABUF_MAX_PLANES];
    guint64 release_point;
    guint64 release_sequence;
    gint64  release_created_usec;
} OutputBuffer;

typedef struct
{
    gboolean shutdown;
    guint32  binary_handle;
    guint32  output_id;
    guint64  generation;
    guint32  buffer_index;
    guint64  sequence;
    guint64  release_point;
    gint64   created_usec;
    ReleaseGroup* release_group;
} ReleaseReaperRecord;

typedef struct
{
    guint16 opcode;
    gsize   body_len;
    guint8* body;
} ClientDeferredFrame;

typedef struct
{
    GByteArray* data;
} ClientOutboxEntry;

typedef enum
{
    CLIENT_PROTOCOL_EXPECT_HELLO,
    CLIENT_PROTOCOL_READY,
} ClientProtocolState;

struct _Output
{
    Client* client;
    RenderRoute* route;
    guint32 consumer_output_id;
    guint32 monitor_index;
    guint32 output_id;
    gint32 display_x;
    gint32 display_y;
    guint32 logical_width;
    guint32 logical_height;
    guint32 target_width;
    guint32 target_height;
    guint32 width;
    guint32 height;
    gdouble scale;
    guint32 refresh_rate_mhz;
    guint64 generation;
    guint64 sequence;
    guint64 next_renderer_retry_time_usec;
    guint64 renderer_generation;
    guint32 renderer_first_frame_miss_count;
    gboolean needs_renderer_rebind;
    gboolean needs_renderer_first_frame;
    gboolean active;
    OutputMemoryKind memory_kind;
    OutputDmaBufPath dmabuf_path;
    VividProducerDmaBufMemoryPreference memory_preference;
    gboolean consumer_same_render_node;
    guint32 n_producer_blacklist;
    VividDmaBufModifierCap producer_blacklist[VIVID_DMABUF_NEGOTIATION_MAX_BLACKLIST];
    struct gbm_bo* bo;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 stride;
    guint32 n_buffers;
    gchar* desktop;
    gchar* display_key;
    gchar* display_name;
    guint32 register_transform;
    VividRenderRect source_rect;
    VividRenderRect dest_rect;
    guint32 window_state_flags;
    VividPolicyContribution policy;
    VividAutoReplayState auto_replay;
    OutputBuffer buffers[VIVID_PRODUCER_RENDERER_MAX_BUFFERS];
};

typedef struct
{
    VividFillMode       fillmode;
    VividLayoutLocation location;
    VividRotation       rotation;
} OutputLayoutParams;

struct _RenderRoute
{
    Producer* producer;
    guint32 route_id;
    guint32 consumer_output_id;
    gchar* display_key;
    gboolean clone_route;
    gboolean owns_renderer;
    gboolean policy_paused;
    VividAutoAction synthesized_action;
    VividProducerRenderer* renderer;
    VividRendererReleaseGate release_gate;
    RenderRouteReleaseGateContext* release_gate_context;
    guint32 width;
    guint32 height;
    gdouble scale;
    guint   frame_source_id;
    guint   orphan_reap_source_id;
};

/*
 * The renderer backends copy the release-gate callback into work items that
 * may outlive the RenderRoute.  Keep the callback's user-data separate from
 * the route and retain it until the renderer itself is destroyed.  A route
 * can therefore be retired immediately without leaving an async callback
 * with a dangling RenderRoute*.
 */
struct _RenderRouteReleaseGateContext
{
    Producer* producer;
    guint32 route_id;
    GMutex lock;
    gboolean active;
    guint64 release_points_renderer_generation;
    guint64 buffer_release_points[VIVID_PRODUCER_RENDERER_MAX_BUFFERS];
};

struct _ReleaseGroup
{
    Producer* producer;
    guint32 route_id;
    guint32 buffer_index;
    guint64 sequence;
    guint64 aggregate_point;
    guint expected;
    guint completed;
    gint ref_count;
    gboolean signaled;
    GMutex lock;
};

struct _Client
{
    Producer* producer;
    gint fd;
    guint source_id;
    guint write_source_id;
    guint frame_source_id;
    guint32 declared_output_count;
    guint32 negotiated_version;
    VividDisplayClientRole role;
    ClientProtocolState protocol_state;
    VividDisplayRecvState recv_state;
    GPtrArray* outputs;
    ConsumerDmaBufCaps dmabuf_caps;
    VividUnbindAckTracker unbind_acks;
    GQueue deferred_frames;
    GQueue outbox;
    gsize outbox_pending_bytes;
    gsize outbox_offset;
    gboolean processing_deferred_frames;
    JsonObject* cached_window_facts_json;
};

struct _Producer
{
    gchar* socket_path;
    gchar* lock_path;
    gint lock_fd;
    gboolean socket_bound;
    gint listen_fd;
    gint render_fd;
    gchar* render_node_path;
    struct gbm_device* gbm_device;
    guint accept_source_id;
    guint fps;
    guint32 next_output_id;
    guint64 next_config_generation;
    GPtrArray* routes;
    GPtrArray* release_gate_contexts;
    VividRouteTable route_table;
    guint32 next_route_id;
    GPtrArray* clients;
    GMainLoop* loop;
    VividProducerConfig config;
    VividProducerRenderer* renderer;
    guint32 release_timeline_handle;
    guint64 next_release_point;
    guint64 buffer_release_points[VIVID_PRODUCER_RENDERER_MAX_BUFFERS];
    guint64 release_points_renderer_generation;
    GMutex release_lock;
    GAsyncQueue* release_queue;
    GThread* release_thread;
    gboolean release_thread_stopping;
    gboolean user_playing;
    gboolean policy_muted;
    gboolean policy_paused;
    gboolean policy_stopped;
    guint64 media_state_received;
    guint32 renderer_rebind_barrier_output_id;
};

static void client_free(Client* client);
static void client_disconnect(Client* client, const gchar* reason);
static gboolean client_write_ready(gint fd, GIOCondition condition, gpointer user_data);
static gboolean client_queue_frame_bytes(Client* client, GByteArray* frame);
static gboolean client_send_frame(Client* client,
                                   guint16 opcode,
                                   const guint8* body,
                                   gsize body_len,
                                   const int* fds,
                                   gsize n_fds);
static const gchar* output_memory_kind_name(OutputMemoryKind kind);
static void producer_apply_playback_policy(Producer* producer);
static void producer_apply_config_to_global_renderer(Producer* producer);
static void producer_apply_config_to_routes(Producer* producer);
static void producer_apply_config_to_renderers(Producer* producer);
static void producer_relayout_outputs(Producer* producer, gboolean mark_rebind);
static void producer_rebind_all_outputs(Producer* producer);
static RenderRoute* producer_route_for_output(Producer* producer, Output* output);
static RenderRoute* producer_find_clone_route(Producer* producer);
static RenderRoute* producer_find_independent_route(Producer* producer, const Output* output);
static RenderRoute* producer_get_or_create_clone_route(Producer* producer);
static void producer_render_route_apply_config(RenderRoute* route);
static gboolean route_send_frame(RenderRoute* route);
static void route_ensure_frame_source(RenderRoute* route);
static void route_stop_frame_source(RenderRoute* route);
static void route_schedule_orphan_reap(RenderRoute* route);
static void producer_destroy_orphan_route(RenderRoute* route);
static void producer_sync_route_frame_sources(Producer* producer);
static Output* producer_primary_output(Producer* producer);
static Output* producer_output_for_display_key(Producer* producer, const gchar* display_key);
static Output* producer_find_consumer_output(Producer* producer, guint32 consumer_output_id);
static gboolean send_set_config(Client* client, const Output* output);
static gboolean render_route_release_gate_wait(gpointer user_data,
                                               guint32  buffer_index,
                                               guint32  timeout_ms);
static gboolean handle_frame(Client* client, guint16 opcode, const guint8* body, gsize body_len);
static gchar* gpu_uuid_to_hex(const guint8 uuid[VIVID_GPU_DEVICE_UUID_BYTES]);
static void set_buffer_error(GError** error, const gchar* format, ...) G_GNUC_PRINTF(2, 3);
static guint32 json_object_get_uint_default(JsonObject* object,
                                            const gchar* member,
                                            guint32      fallback);
static gint32 json_object_get_int_default(JsonObject* object,
                                          const gchar* member,
                                          gint32       fallback);
static gdouble json_object_get_double_default(JsonObject* object,
                                              const gchar* member,
                                              gdouble      fallback);
static gboolean json_object_get_boolean_default(JsonObject* object,
                                                const gchar* member,
                                                gboolean     fallback);
static const gchar* json_object_get_string_default(JsonObject* object,
                                                   const gchar* member,
                                                   const gchar* fallback);
static JsonObject* json_object_get_object_member_or_null(JsonObject* object,
                                                         const gchar* member);
static gboolean json_value_to_uint64(JsonNode* node, guint64* out_value);

static VividDisplayClientRole
client_role_from_text(const gchar* text)
{
    if (g_strcmp0(text, "consumer") == 0 || g_strcmp0(text, "display") == 0)
        return VIVID_DISPLAY_CLIENT_CONSUMER;
    if (g_strcmp0(text, "controller") == 0)
        return VIVID_DISPLAY_CLIENT_CONTROLLER;
    if (g_strcmp0(text, "consumer+controller") == 0 ||
        g_strcmp0(text, "display+controller") == 0)
        return VIVID_DISPLAY_CLIENT_CONSUMER_AND_CONTROLLER;
    if (g_strcmp0(text, "producer") == 0)
        return VIVID_DISPLAY_CLIENT_PRODUCER;
    return 0;
}

static gboolean
client_role_includes_consumer(const Client* client)
{
    if (!client)
        return FALSE;
    return client->role == VIVID_DISPLAY_CLIENT_CONSUMER ||
        client->role == VIVID_DISPLAY_CLIENT_CONSUMER_AND_CONTROLLER;
}

static const gchar*
producer_effective_render_device(const VividProducerConfig* config)
{
    return config && config->render_device && *config->render_device
        ? config->render_device
        : "auto";
}

static gchar*
json_node_to_compact_string(JsonNode* node)
{
    JsonGenerator* generator = json_generator_new();
    json_generator_set_root(generator, node);
    json_generator_set_pretty(generator, FALSE);
    gchar* text = json_generator_to_data(generator, NULL);
    g_object_unref(generator);
    return text;
}

static gboolean
producer_uses_clone_routes(const Producer* producer)
{
    return !producer ||
        g_strcmp0(producer->config.multi_display_mode, "independent") != 0;
}

static Output*
producer_first_output_by_id(Producer* producer)
{
    Output* best = NULL;
    for (guint client_index = 0; producer && client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (!output)
                continue;
            if (!best || output->output_id < best->output_id)
                best = output;
        }
    }
    return best;
}

static Output*
producer_output_for_display_key(Producer* producer, const gchar* display_key)
{
    if (!producer || !display_key || !*display_key)
        return NULL;

    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (output && output->display_key &&
                g_strcmp0(output->display_key, display_key) == 0) {
                return output;
            }
        }
    }

    return NULL;
}

static VividProducerRenderer*
output_renderer(const Output* output)
{
    return output && output->route ? output->route->renderer : NULL;
}

static gdouble
output_render_scale(const Output* output)
{
    if (output && output->route && output->route->scale > 0.0)
        return output->route->scale;
    return output ? output->scale : 1.0;
}

static RenderRouteReleaseGateContext*
render_route_release_gate_context_new(Producer* producer, guint32 route_id)
{
    RenderRouteReleaseGateContext* context = g_new0(RenderRouteReleaseGateContext, 1);
    context->producer = producer;
    context->route_id = route_id;
    context->active = TRUE;
    g_mutex_init(&context->lock);
    if (producer->release_gate_contexts)
        g_ptr_array_add(producer->release_gate_contexts, context);
    return context;
}

static void
render_route_release_gate_context_disable(RenderRouteReleaseGateContext* context)
{
    if (!context)
        return;

    g_mutex_lock(&context->lock);
    context->active = FALSE;
    g_mutex_unlock(&context->lock);
}

static void
render_route_release_gate_context_free(gpointer data)
{
    RenderRouteReleaseGateContext* context = data;
    if (!context)
        return;
    g_mutex_clear(&context->lock);
    g_free(context);
}

static void
render_route_attach_release_gate(RenderRoute* route)
{
    if (!route || !route->renderer)
        return;

    route->release_gate_context =
        render_route_release_gate_context_new(route->producer, route->route_id);
    route->release_gate = (VividRendererReleaseGate) {
        .abi_version = VIVID_RENDERER_RELEASE_GATE_ABI_VERSION,
        .user_data = route->release_gate_context,
        .wait_release = render_route_release_gate_wait,
    };
    vivid_producer_renderer_set_release_gate(route->renderer, &route->release_gate);
}

RenderRoute*
producer_render_route_new(Producer* producer, gboolean clone_route, gboolean owns_renderer)
{
    RenderRoute* route = g_new0(RenderRoute, 1);
    route->producer = producer;
    route->route_id = ++producer->next_route_id;
    route->clone_route = clone_route;
    route->owns_renderer = owns_renderer;
    route->scale = 1.0;
    route->renderer = owns_renderer
        ? vivid_producer_renderer_new_from_gpu_devices(
              producer && producer->renderer
                  ? vivid_producer_renderer_gpu_devices(producer->renderer)
                  : NULL)
        : producer->renderer;
    vivid_producer_renderer_set_playback_paused(
        route->renderer,
        producer->policy_paused || producer->policy_stopped || !producer->user_playing);
    render_route_attach_release_gate(route);
    g_ptr_array_add(producer->routes, route);
    g_debug("VividProducer: created render route=%u clone=%s owns-renderer=%s",
            route->route_id,
            route->clone_route ? "true" : "false",
            route->owns_renderer ? "true" : "false");
    return route;
}

static void
render_route_free(RenderRoute* route)
{
    if (!route)
        return;

    route_stop_frame_source(route);
    producer_route_cancel_orphan_reap(route);
    render_route_release_gate_context_disable(route->release_gate_context);
    if (route->renderer)
        vivid_producer_renderer_set_release_gate(route->renderer, NULL);
    if (route->owns_renderer)
        vivid_producer_renderer_free(route->renderer);
    g_free(route->display_key);
    g_free(route);
}

static void
producer_sync_link_clear_rgba(RouteLink* link, RenderRoute* route)
{
    if (!link)
        return;

    gfloat rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (route && route->renderer &&
        vivid_producer_renderer_get_clear_rgba(route->renderer, rgba)) {
        link->clear_rgba[0] = rgba[0];
        link->clear_rgba[1] = rgba[1];
        link->clear_rgba[2] = rgba[2];
        link->clear_rgba[3] = rgba[3];
    } else {
        link->clear_rgba[0] = 0.0f;
        link->clear_rgba[1] = 0.0f;
        link->clear_rgba[2] = 0.0f;
        link->clear_rgba[3] = 1.0f;
    }
}

static void
producer_resync_set_config_for_output(Producer* producer, Output* output)
{
    if (!producer || !output || !output->active || !output->client)
        return;

    if (output->n_buffers > 0)
        send_set_config(output->client, output);
}

static void
producer_resync_route_set_configs(Producer* producer, RenderRoute* route)
{
    if (!producer || !route || !producer->route_table.links)
        return;

    for (guint i = 0; i < producer->route_table.links->len; i++) {
        RouteLink* link = g_ptr_array_index(producer->route_table.links, i);
        if (!link || link->route != route || !link->output)
            continue;
        producer_resync_set_config_for_output(producer, link->output);
    }
}

static RenderRoute*
producer_route_for_output(Producer* producer, Output* output)
{
    if (!producer || !output)
        return NULL;

    const gboolean want_clone = producer_uses_clone_routes(producer);
    RouteLink* existing = route_link_for_output(&producer->route_table, output);
    if (existing && existing->route) {
        if (existing->route->clone_route == want_clone)
            return existing->route;
        route_link_detach_output(&producer->route_table, output);
    }

    if (want_clone) {
        RenderRoute* route = producer_get_or_create_clone_route(producer);
        route_link_attach(&producer->route_table, output, route);
        return route;
    }

    RenderRoute* route = producer_find_independent_route(producer, output);
    if (!route)
        route = producer_render_route_new(producer, FALSE, TRUE);
    route->consumer_output_id = output->consumer_output_id;
    g_free(route->display_key);
    route->display_key = g_strdup(output->display_key ? output->display_key : "");
    producer_render_route_apply_config(route);
    route_link_attach(&producer->route_table, output, route);
    return route;
}

static RenderRoute*
producer_find_independent_route(Producer* producer, const Output* output)
{
    if (!producer || !producer->routes || !output)
        return NULL;

    for (guint i = 0; i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (!route || route->clone_route || !route->owns_renderer)
            continue;

        if (output->display_key && *output->display_key &&
            route->display_key && *route->display_key &&
            g_strcmp0(route->display_key, output->display_key) == 0) {
            return route;
        }

        if ((!output->display_key || !*output->display_key) &&
            route->consumer_output_id == output->consumer_output_id) {
            return route;
        }
    }

    return NULL;
}

static RenderRoute*
producer_find_clone_route(Producer* producer)
{
    if (!producer || !producer->routes)
        return NULL;

    for (guint i = 0; i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (route && route->clone_route)
            return route;
    }

    return NULL;
}

static RenderRoute*
producer_get_or_create_clone_route(Producer* producer)
{
    RenderRoute* route = producer_find_clone_route(producer);
    if (route)
        return route;

    route = producer_render_route_new(producer, TRUE, FALSE);
    producer_render_route_apply_config(route);
    return route;
}

gboolean
route_link_apply_layout(RouteLink*          link,
                        Output*             output,
                        guint32             route_width,
                        guint32             route_height,
                        VividFillMode       fillmode,
                        VividLayoutLocation location,
                        VividRotation       rotation)
{
    if (!link || !output)
        return FALSE;

    const guint32 old_width = output->width;
    const guint32 old_height = output->height;
    const VividRenderRect old_source = link->src_rect;
    const VividRenderRect old_dest = link->dst_rect;
    const guint32 old_transform = link->transform;

    output->width = CLAMP(route_width, 1u, OUTPUT_MAX_DIMENSION);
    output->height = CLAMP(route_height, 1u, OUTPUT_MAX_DIMENSION);

    VividRenderSize display_size = {
        .width = output->target_width,
        .height = output->target_height,
    };
    if (rotation == VIVID_ROTATION_CW90 || rotation == VIVID_ROTATION_CW270) {
        display_size.width = output->target_height;
        display_size.height = output->target_width;
    }

    vivid_render_layout_compute(fillmode,
                                location,
                                (VividRenderSize) {
                                    .width = output->width,
                                    .height = output->height,
                                },
                                display_size,
                                &link->src_rect,
                                &link->dst_rect);

    link->transform = (guint32)rotation;

    output->source_rect = link->src_rect;
    output->dest_rect = link->dst_rect;

    return old_width != output->width ||
        old_height != output->height ||
        old_source.x != link->src_rect.x ||
        old_source.y != link->src_rect.y ||
        old_source.width != link->src_rect.width ||
        old_source.height != link->src_rect.height ||
        old_dest.x != link->dst_rect.x ||
        old_dest.y != link->dst_rect.y ||
        old_dest.width != link->dst_rect.width ||
        old_dest.height != link->dst_rect.height ||
        old_transform != link->transform;
}

gboolean
route_link_apply_clone_layout(RouteLink* link,
                              Output*    output,
                              guint32    route_width,
                              guint32    route_height)
{
    if (!link || !output)
        return FALSE;

    const guint32 old_width = output->width;
    const guint32 old_height = output->height;
    const VividRenderRect old_source = link->src_rect;
    const VividRenderRect old_dest = link->dst_rect;
    const guint32 old_transform = link->transform;

    output->width = CLAMP(route_width, 1u, OUTPUT_MAX_DIMENSION);
    output->height = CLAMP(route_height, 1u, OUTPUT_MAX_DIMENSION);

    link->dst_rect = (VividRenderRect) {
        .x = 0.0,
        .y = 0.0,
        .width = output->target_width,
        .height = output->target_height,
    };
    vivid_render_layout_cover_source(output->width,
                                     output->height,
                                     output->target_width,
                                     output->target_height,
                                     &link->src_rect);
    link->transform = 0;

    output->source_rect = link->src_rect;
    output->dest_rect = link->dst_rect;

    return old_width != output->width ||
        old_height != output->height ||
        old_source.x != link->src_rect.x ||
        old_source.y != link->src_rect.y ||
        old_source.width != link->src_rect.width ||
        old_source.height != link->src_rect.height ||
        old_dest.x != link->dst_rect.x ||
        old_dest.y != link->dst_rect.y ||
        old_dest.width != link->dst_rect.width ||
        old_dest.height != link->dst_rect.height ||
        old_transform != link->transform;
}

static JsonNode*
json_object_get_member_alias(JsonObject* object,
                             const gchar* kebab_member,
                             const gchar* camel_member)
{
    if (!object)
        return NULL;
    if (kebab_member && json_object_has_member(object, kebab_member))
        return json_object_get_member(object, kebab_member);
    if (camel_member && json_object_has_member(object, camel_member))
        return json_object_get_member(object, camel_member);
    return NULL;
}

static const gchar*
json_object_get_string_alias(JsonObject* object,
                             const gchar* kebab_member,
                             const gchar* camel_member,
                             const gchar* fallback)
{
    JsonNode* node = json_object_get_member_alias(object, kebab_member, camel_member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return fallback;
    const gchar* value = json_node_get_string(node);
    return value ? value : fallback;
}

static gint
json_object_get_int_alias(JsonObject* object,
                          const gchar* kebab_member,
                          const gchar* camel_member,
                          gint         fallback,
                          gint         min_value,
                          gint         max_value)
{
    JsonNode* node = json_object_get_member_alias(object, kebab_member, camel_member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    const GType value_type = json_node_get_value_type(node);
    if (value_type == G_TYPE_INT64 || value_type == G_TYPE_DOUBLE)
        return CLAMP((gint)json_node_get_double(node), min_value, max_value);
    if (value_type == G_TYPE_STRING) {
        const gchar* text = json_node_get_string(node);
        if (!text)
            return fallback;
        gchar* end = NULL;
        const gint64 parsed = g_ascii_strtoll(text, &end, 10);
        if (end == text)
            return fallback;
        while (end && g_ascii_isspace(*end))
            end++;
        if (end && *end != '\0')
            return fallback;
        return CLAMP((gint)parsed, min_value, max_value);
    }

    return fallback;
}

static OutputLayoutParams
producer_output_layout_params(const Producer* producer, const Output* output)
{
    OutputLayoutParams params = {
        .fillmode = vivid_render_layout_fillmode_from_content_fit(
            producer ? producer->config.content_fit : 1),
        .location = { 50.0, 50.0 },
        .rotation = VIVID_ROTATION_NORMAL,
    };

    if (!producer || !output || !output->display_key || !*output->display_key)
        return params;

    const VividDisplayPrefs* prefs =
        vivid_producer_config_display_prefs(&producer->config, output->display_key);
    if (!prefs)
        return params;

    if (prefs->fillmode != VIVID_DISPLAY_PREFS_UNSET)
        params.fillmode = (VividFillMode)CLAMP(prefs->fillmode, 0, 3);
    if (prefs->location != VIVID_DISPLAY_PREFS_UNSET)
        params.location = vivid_render_layout_location_from_preset(prefs->location);
    if (prefs->rotation != VIVID_DISPLAY_PREFS_UNSET)
        params.rotation = (VividRotation)CLAMP(prefs->rotation, 0, 3);

    const JsonObject* project = prefs->project;
    if (project) {
        const gint per_output_fit =
            json_object_get_int_alias((JsonObject*)project,
                                      "content-fit",
                                      "contentFit",
                                      -1,
                                      -1,
                                      3);
        if (per_output_fit >= 1)
            params.fillmode = vivid_render_layout_fillmode_from_content_fit(per_output_fit);
    }

    return params;
}

static gchar*
json_member_to_compact_string(JsonObject* object,
                              const gchar* kebab_member,
                              const gchar* camel_member)
{
    JsonNode* node = json_object_get_member_alias(object, kebab_member, camel_member);
    if (!node)
        return NULL;
    if (JSON_NODE_HOLDS_VALUE(node) &&
        json_node_get_value_type(node) == G_TYPE_STRING)
        return g_strdup(json_node_get_string(node));
    return json_node_to_compact_string(node);
}

static gboolean
json_object_has_member_alias(JsonObject* object,
                             const gchar* kebab_member,
                             const gchar* camel_member)
{
    return object &&
        ((kebab_member && json_object_has_member(object, kebab_member)) ||
         (camel_member && json_object_has_member(object, camel_member)));
}

static JsonObject*
json_node_get_object_or_null(JsonNode* node)
{
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static gboolean
control_set_state_target_only_changed(JsonObject* payload)
{
    return payload &&
        json_object_get_size(payload) == 1 &&
        json_object_has_member_alias(payload,
                                     "current-config-display-key",
                                     "currentConfigDisplayKey");
}

static gboolean
control_set_state_requires_output_rebind(const Producer* producer,
                                         JsonObject*      payload,
                                         GHashTable*      previous_display_prefs,
                                         const gchar*     previous_multi_display_mode,
                                         const gchar*     previous_primary_display_key,
                                         gint             previous_content_fit)
{
    if (!producer || !payload)
        return FALSE;

    if (control_set_state_target_only_changed(payload))
        return FALSE;

    if (json_object_get_boolean_default(
            payload,
            "reset-defaults",
            json_object_get_boolean_default(payload, "resetDefaults", FALSE))) {
        return TRUE;
    }

    const gchar* current_multi_display_mode = producer->config.multi_display_mode
        ? producer->config.multi_display_mode
        : "clone";
    if (g_strcmp0(previous_multi_display_mode, current_multi_display_mode) != 0 ||
        g_strcmp0(previous_primary_display_key ? previous_primary_display_key : "",
                  producer->config.primary_display_key
                      ? producer->config.primary_display_key
                      : "") != 0) {
        return TRUE;
    }

    if (previous_content_fit != producer->config.content_fit &&
        json_object_has_member_alias(payload, "content-fit", "contentFit")) {
        return TRUE;
    }

    if (json_object_has_member_alias(payload,
                                     "per-output-projects",
                                     "perOutputProjects")) {
        return vivid_producer_config_display_prefs_render_contract_changed(
            previous_display_prefs,
            &producer->config);
    }

    return FALSE;
}

static gboolean
per_output_project_has_wallpaper(const JsonObject* per_output)
{
    const gchar* project_path =
        json_object_get_string_alias((JsonObject*)per_output,
                                     "project-path",
                                     "projectPath",
                                     NULL);
    return project_path && *project_path;
}

static gboolean
producer_output_has_wallpaper(const Producer* producer, const Output* output)
{
    if (!producer || !output)
        return FALSE;

    return per_output_project_has_wallpaper(
        vivid_producer_config_display_project_view(&producer->config, output->display_key));
}

static gboolean
producer_output_should_be_active(Producer* producer, Output* output)
{
    if (!producer || !output)
        return FALSE;

    if (!producer_uses_clone_routes(producer))
        return producer_output_has_wallpaper(producer, output);

    Output* primary = producer_primary_output(producer);
    return primary && producer_output_has_wallpaper(producer, primary);
}

static gboolean
render_route_is_used(const Producer* producer, const RenderRoute* route)
{
    return route_link_count_for_route(&((Producer*)producer)->route_table, route) > 0;
}

static void
producer_prune_unused_routes(Producer* producer)
{
    if (!producer || !producer->routes)
        return;

    for (gint i = (gint)producer->routes->len - 1; i >= 0; i--) {
        RenderRoute* route = g_ptr_array_index(producer->routes, (guint)i);
        if (!route || render_route_is_used(producer, route))
            continue;

        /*
         * A detached route owns a renderer/swapchain (or references the
         * producer's clone renderer).  Keeping it as an idle cache pins its
         * exported Vulkan images and DMA-BUF descriptors across every
         * clone/independent or wallpaper switch.  Waywallen tears down the
         * retired pool instead of retaining an unused route, so schedule the
         * same bounded orphan reap for every detached route.  The route-table
         * attach path cancels this timer if the display is reattached before
         * the grace period expires.
         */
        if (route->orphan_reap_source_id == 0)
            route_schedule_orphan_reap(route);
    }
}

static void
consumer_dmabuf_caps_init(ConsumerDmaBufCaps* caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->fourccs = g_array_new(FALSE, FALSE, sizeof(guint32));
    caps->implicit_linear_fourccs = g_array_new(FALSE, FALSE, sizeof(guint32));
    caps->modifiers = g_array_new(FALSE, FALSE, sizeof(ConsumerDmaBufModifier));
    vivid_dmabuf_peer_caps_init(&caps->peer_caps);
}

static void
consumer_dmabuf_caps_clear(ConsumerDmaBufCaps* caps)
{
    if (!caps)
        return;

    g_clear_pointer(&caps->backend, g_free);
    g_clear_pointer(&caps->probe, g_free);
    g_clear_pointer(&caps->render_node, g_free);
    g_clear_pointer(&caps->vendor, g_free);
    g_clear_pointer(&caps->pci_address, g_free);
    g_clear_pointer(&caps->device_uuid, g_free);
    g_clear_pointer(&caps->driver_uuid, g_free);
    g_clear_pointer(&caps->identity_mode, g_free);
    if (caps->fourccs)
        g_array_unref(caps->fourccs);
    if (caps->implicit_linear_fourccs)
        g_array_unref(caps->implicit_linear_fourccs);
    if (caps->modifiers)
        g_array_unref(caps->modifiers);
    memset(caps, 0, sizeof(*caps));
}

static gboolean
hex_uuid_to_bytes(const gchar* text,
                  guint8       out_uuid[VIVID_DMABUF_DEVICE_UUID_BYTES])
{
    if (!text || !*text || !out_uuid)
        return FALSE;

    gchar compact[(VIVID_DMABUF_DEVICE_UUID_BYTES * 2) + 1] = {0};
    guint cursor = 0;
    for (const gchar* p = text; *p; p++) {
        if (*p == '-')
            continue;
        if (!g_ascii_isxdigit(*p) || cursor >= sizeof(compact) - 1)
            return FALSE;
        compact[cursor++] = *p;
    }
    if (cursor != VIVID_DMABUF_DEVICE_UUID_BYTES * 2)
        return FALSE;

    for (guint i = 0; i < VIVID_DMABUF_DEVICE_UUID_BYTES; i++) {
        gchar byte_text[3] = { compact[i * 2], compact[i * 2 + 1], '\0' };
        gchar* end = NULL;
        const guint64 value = g_ascii_strtoull(byte_text, &end, 16);
        if (!end || *end != '\0' || value > 0xff)
            return FALSE;
        out_uuid[i] = (guint8)value;
    }
    return TRUE;
}

static gboolean
uuid_bytes_known(const guint8 uuid[VIVID_DMABUF_DEVICE_UUID_BYTES])
{
    for (guint i = 0; i < VIVID_DMABUF_DEVICE_UUID_BYTES; i++) {
        if (uuid[i] != 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
dmabuf_identity_fill_render_node(VividDmaBufDeviceIdentity* identity,
                                 const gchar*               render_node)
{
    if (!identity)
        return FALSE;

    identity->drm_render_major = 0;
    identity->drm_render_minor = 0;
    g_strlcpy(identity->render_node,
              render_node ? render_node : "",
              sizeof(identity->render_node));

    if (!render_node || !*render_node)
        return FALSE;

    struct stat st;
    if (stat(render_node, &st) != 0 || !S_ISCHR(st.st_mode))
        return FALSE;

    identity->drm_render_major = (guint32)major(st.st_rdev);
    identity->drm_render_minor = (guint32)minor(st.st_rdev);
    return TRUE;
}

static void
dmabuf_identity_fill_from_gpu(VividDmaBufDeviceIdentity* identity,
                              const VividGpuDevice*      gpu)
{
    if (!identity || !gpu)
        return;

    memcpy(identity->device_uuid,
           gpu->uuid,
           MIN((gsize)sizeof(identity->device_uuid),
               (gsize)sizeof(gpu->uuid)));
    memcpy(identity->driver_uuid,
           gpu->driver_uuid,
           MIN((gsize)sizeof(identity->driver_uuid),
               (gsize)sizeof(gpu->driver_uuid)));
    identity->drm_render_major = gpu->drm_render_major;
    identity->drm_render_minor = gpu->drm_render_minor;
    g_strlcpy(identity->render_node,
              gpu->render_node,
              sizeof(identity->render_node));

    /*
     * VK_EXT_physical_device_drm is authoritative when present, but keeping a
     * stat() fallback makes same-device negotiation robust on stacks that
     * resolve a render node yet leave the DRM properties unset. The fallback
     * records kernel device numbers only; path strings remain diagnostics.
     */
    if ((identity->drm_render_major == 0 && identity->drm_render_minor == 0) &&
        gpu->render_node[0]) {
        (void)dmabuf_identity_fill_render_node(identity, gpu->render_node);
    }
}

static void
consumer_dmabuf_caps_parse_uuid(ConsumerDmaBufCaps* caps,
                                JsonObject*          object,
                                const gchar*         member,
                                gchar**              out_text,
                                guint8               out_uuid[VIVID_DMABUF_DEVICE_UUID_BYTES])
{
    (void)caps;

    const gchar* text = json_object_get_string_default(object, member, "");
    g_clear_pointer(out_text, g_free);
    *out_text = g_strdup(text ? text : "");
    if (text && *text && !hex_uuid_to_bytes(text, out_uuid)) {
        g_warning("VividProducer: consumer caps ignored malformed %s='%s'",
                  member,
                  text);
        memset(out_uuid, 0, VIVID_DMABUF_DEVICE_UUID_BYTES);
    }
}

static gboolean
consumer_dmabuf_caps_parse_sync_caps(ConsumerDmaBufCaps* caps,
                                     JsonObject*          object,
                                     GError**             error)
{
    if (!caps || !object || !json_object_has_member(object, VIVID_JSON_DMABUF_CAPS_SYNC_CAPS))
        return TRUE;

    JsonNode* member = json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_SYNC_CAPS);
    if (!member || !JSON_NODE_HOLDS_ARRAY(member)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an array",
                         VIVID_JSON_DMABUF_CAPS_SYNC_CAPS);
        return FALSE;
    }

    JsonArray* values = json_node_get_array(member);
    const guint length = json_array_get_length(values);
    for (guint i = 0; i < length; i++) {
        JsonNode* node = json_array_get_element(values, i);
        if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] must be a string",
                             VIVID_JSON_DMABUF_CAPS_SYNC_CAPS,
                             i);
            return FALSE;
        }
        const gchar* cap = json_node_get_string(node);
        if (g_strcmp0(cap, "implicit") == 0) {
            caps->peer_caps.sync_caps |= VIVID_DMABUF_SYNC_CAP_IMPLICIT;
        } else if (g_strcmp0(cap, "explicit-sync-fd") == 0) {
            caps->sync_explicit_acquire = TRUE;
        } else if (g_strcmp0(cap, "drm-syncobj-release") == 0 ||
                   g_strcmp0(cap, "release-syncobj") == 0 ||
                   g_strcmp0(cap, "syncobj-binary") == 0) {
            caps->peer_caps.sync_caps |= VIVID_DMABUF_SYNC_CAP_SYNCOBJ_BINARY;
            caps->sync_release_syncobj = TRUE;
        } else if (g_strcmp0(cap, "syncobj-timeline") == 0) {
            caps->peer_caps.sync_caps |= VIVID_DMABUF_SYNC_CAP_SYNCOBJ_TIMELINE;
        } else {
            g_message("VividProducer: consumer caps ignored unknown syncCaps[%u]=%s",
                      i,
                      cap ? cap : "(null)");
        }
    }
    return TRUE;
}

static gboolean
consumer_dmabuf_caps_parse_color_caps(ConsumerDmaBufCaps* caps,
                                      JsonObject*          object,
                                      GError**             error)
{
    if (!caps || !object || !json_object_has_member(object, VIVID_JSON_DMABUF_CAPS_COLOR_CAPS))
        return TRUE;

    JsonNode* member = json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_COLOR_CAPS);
    if (!member || !JSON_NODE_HOLDS_ARRAY(member)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an array",
                         VIVID_JSON_DMABUF_CAPS_COLOR_CAPS);
        return FALSE;
    }

    JsonArray* values = json_node_get_array(member);
    const guint length = json_array_get_length(values);
    for (guint i = 0; i < length; i++) {
        JsonNode* node = json_array_get_element(values, i);
        if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] must be a string",
                             VIVID_JSON_DMABUF_CAPS_COLOR_CAPS,
                             i);
            return FALSE;
        }
        const gchar* cap = json_node_get_string(node);
        if (g_strcmp0(cap, "srgb") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_SRGB;
        } else if (g_strcmp0(cap, "linear") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_LINEAR;
        } else if (g_strcmp0(cap, "bt601") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_BT601;
        } else if (g_strcmp0(cap, "bt709") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_BT709;
        } else if (g_strcmp0(cap, "bt2020") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_BT2020;
        } else if (g_strcmp0(cap, "full-range") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_RANGE_FULL;
        } else if (g_strcmp0(cap, "limited-range") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_RANGE_LIMITED;
        } else if (g_strcmp0(cap, "premultiplied-alpha") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_ALPHA_PREMULTIPLIED;
        } else if (g_strcmp0(cap, "straight-alpha") == 0) {
            caps->peer_caps.color_caps |= VIVID_DMABUF_COLOR_CAP_ALPHA_STRAIGHT;
        } else {
            g_message("VividProducer: consumer caps ignored unknown colorCaps[%u]=%s",
                      i,
                      cap ? cap : "(null)");
        }
    }
    return TRUE;
}

static gboolean
consumer_dmabuf_caps_parse_relay_modes(ConsumerDmaBufCaps* caps,
                                       JsonObject*          object,
                                       GError**             error)
{
    if (!caps || !object || !json_object_has_member(object, VIVID_JSON_DMABUF_CAPS_RELAY_MODES)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must advertise %s",
                         VIVID_JSON_DMABUF_CAPS_RELAY_MODES,
                         DMABUF_RELAY_MODE_SHADOW_COPY);
        return FALSE;
    }

    JsonNode* member = json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_RELAY_MODES);
    if (!member || !JSON_NODE_HOLDS_ARRAY(member)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an array",
                         VIVID_JSON_DMABUF_CAPS_RELAY_MODES);
        return FALSE;
    }

    JsonArray* values = json_node_get_array(member);
    const guint length = json_array_get_length(values);
    for (guint i = 0; i < length; i++) {
        JsonNode* node = json_array_get_element(values, i);
        if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] must be a string",
                             VIVID_JSON_DMABUF_CAPS_RELAY_MODES,
                             i);
            return FALSE;
        }

        const gchar* mode = json_node_get_string(node);
        if (g_strcmp0(mode, DMABUF_RELAY_MODE_SHADOW_COPY) == 0) {
            caps->relay_shadow_copy = TRUE;
            caps->peer_caps.relay_modes |= VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
        } else {
            g_message("VividProducer: consumer caps ignored unknown relayModes[%u]=%s",
                      i,
                      mode ? mode : "(null)");
        }
    }

    if (!caps->relay_shadow_copy) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must contain %s",
                         VIVID_JSON_DMABUF_CAPS_RELAY_MODES,
                         DMABUF_RELAY_MODE_SHADOW_COPY);
        return FALSE;
    }
    return TRUE;
}

static void
consumer_dmabuf_caps_parse_render_node_identity(ConsumerDmaBufCaps* caps)
{
    const gboolean drm_identity_known =
        dmabuf_identity_fill_render_node(&caps->peer_caps.identity,
                                         caps->render_node);

    if (uuid_bytes_known(caps->peer_caps.identity.device_uuid)) {
        caps->identity_mode = g_strdup("uuid");
        return;
    }

    if (drm_identity_known) {
        caps->identity_mode = g_strdup("drm-render-node");
        g_message("VividProducer: consumer backend=%s did not provide "
                  "deviceUuid; using renderNode=%s DRM identity %u:%u for "
                  "DMA-BUF same-device negotiation",
                  caps->backend && *caps->backend ? caps->backend : "(unknown)",
                  caps->render_node,
                  caps->peer_caps.identity.drm_render_major,
                  caps->peer_caps.identity.drm_render_minor);
        return;
    }

    if (caps->render_node && *caps->render_node) {
        caps->identity_mode = g_strdup("render-node-only");
        g_message("VividProducer: consumer backend=%s did not provide "
                  "deviceUuid; renderNode=%s could not be resolved to a DRM "
                  "character-device identity and is diagnostic only. The "
                  "DMA-BUF picker will use the conservative cross-GPU path",
                  caps->backend && *caps->backend ? caps->backend : "(unknown)",
                  caps->render_node);
        return;
    }

    caps->identity_mode = g_strdup("unknown");
}

static gboolean
consumer_dmabuf_caps_parse_extent_caps(ConsumerDmaBufCaps* caps,
                                       JsonObject*          object,
                                       GError**             error)
{
    if (!caps || !object || !json_object_has_member(object, VIVID_JSON_DMABUF_CAPS_EXTENT_MAX))
        return TRUE;

    JsonNode* member = json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_EXTENT_MAX);
    if (!member || !JSON_NODE_HOLDS_OBJECT(member)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an object",
                         VIVID_JSON_DMABUF_CAPS_EXTENT_MAX);
        return FALSE;
    }

    JsonObject* extent = json_node_get_object(member);
    guint64 width = 0;
    guint64 height = 0;
    if ((json_object_has_member(extent, VIVID_JSON_EXTENT_MAX_WIDTH) &&
         (!json_value_to_uint64(json_object_get_member(extent, VIVID_JSON_EXTENT_MAX_WIDTH), &width) ||
          width > G_MAXUINT32)) ||
        (json_object_has_member(extent, VIVID_JSON_EXTENT_MAX_HEIGHT) &&
         (!json_value_to_uint64(json_object_get_member(extent, VIVID_JSON_EXTENT_MAX_HEIGHT), &height) ||
          height > G_MAXUINT32))) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s width/height must be uint32",
                         VIVID_JSON_DMABUF_CAPS_EXTENT_MAX);
        return FALSE;
    }

    caps->peer_caps.extent_max_w = (guint32)width;
    caps->peer_caps.extent_max_h = (guint32)height;
    return TRUE;
}

static gboolean
uint32_array_contains(GArray* array, guint32 value)
{
    if (!array)
        return FALSE;
    for (guint i = 0; i < array->len; i++) {
        if (g_array_index(array, guint32, i) == value)
            return TRUE;
    }
    return FALSE;
}

static void
uint32_array_append_unique(GArray* array, guint32 value)
{
    if (!array || uint32_array_contains(array, value))
        return;
    g_array_append_val(array, value);
}

static gboolean
json_value_to_uint64(JsonNode* node, guint64* out_value)
{
    if (!node || !out_value)
        return FALSE;
    if (JSON_NODE_HOLDS_VALUE(node)) {
        const GType value_type = json_node_get_value_type(node);
        if (value_type == G_TYPE_STRING) {
            const gchar* text = json_node_get_string(node);
            if (!text || !*text || text[0] == '-')
                return FALSE;
            gchar* end = NULL;
            guint64 value = g_ascii_strtoull(text, &end, 0);
            if (!end || *end != '\0')
                return FALSE;
            *out_value = value;
            return TRUE;
        }
        if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_FLOAT) {
            const gdouble double_value = json_node_get_double(node);
            if (!isfinite(double_value) ||
                double_value < 0.0 ||
                floor(double_value) != double_value ||
                double_value > (gdouble)G_MAXUINT64) {
                return FALSE;
            }
            *out_value = (guint64)double_value;
            return TRUE;
        }
        if (value_type != G_TYPE_INT64 &&
            value_type != G_TYPE_INT &&
            value_type != G_TYPE_UINT &&
            value_type != G_TYPE_UINT64) {
            return FALSE;
        }
        const gint64 value = json_node_get_int(node);
        if (value < 0)
            return FALSE;
        *out_value = (guint64)value;
        return TRUE;
    }
    return FALSE;
}

static gboolean
consumer_dmabuf_caps_parse_uint32_array(GArray*      array,
                                        JsonObject*  object,
                                        const gchar* member,
                                        GError**     error)
{
    if (!array || !object || !json_object_has_member(object, member))
        return TRUE;

    JsonNode* array_node = json_object_get_member(object, member);
    if (!array_node || !JSON_NODE_HOLDS_ARRAY(array_node)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an array",
                         member);
        return FALSE;
    }

    JsonArray* values = json_node_get_array(array_node);
    const guint length = json_array_get_length(values);
    for (guint i = 0; i < length; i++) {
        JsonNode* node = json_array_get_element(values, i);
        guint64 value = 0;
        if (!json_value_to_uint64(node, &value) || value > G_MAXUINT32) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] must be uint32",
                             member,
                             i);
            return FALSE;
        }
        uint32_array_append_unique(array, (guint32)value);
    }
    return TRUE;
}

static gboolean
consumer_dmabuf_caps_parse_memory_hints(ConsumerDmaBufCaps* caps,
                                        JsonObject*          object,
                                        GError**             error)
{
    if (!caps || !object || !json_object_has_member(object, VIVID_JSON_DMABUF_CAPS_MEMORY_HINTS))
        return TRUE;

    JsonNode* member = json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_MEMORY_HINTS);
    if (!member || !JSON_NODE_HOLDS_ARRAY(member)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an array",
                         VIVID_JSON_DMABUF_CAPS_MEMORY_HINTS);
        return FALSE;
    }

    JsonArray* hints = json_node_get_array(member);
    const guint length = json_array_get_length(hints);
    for (guint i = 0; i < length; i++) {
        JsonNode* node = json_array_get_element(hints, i);
        if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] must be a string",
                             VIVID_JSON_DMABUF_CAPS_MEMORY_HINTS,
                             i);
            return FALSE;
        }
        const gchar* hint = json_node_get_string(node);
        if (g_strcmp0(hint, "host-visible") == 0) {
            caps->hint_host_visible = TRUE;
            caps->peer_caps.memory_hints |= VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
        } else if (g_strcmp0(hint, "device-local") == 0) {
            caps->hint_device_local = TRUE;
            caps->peer_caps.memory_hints |= VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL;
        } else if (g_strcmp0(hint, "implicit-linear") == 0) {
            caps->hint_implicit_linear = TRUE;
            caps->peer_caps.memory_hints |= VIVID_DMABUF_MEMORY_HINT_LINEAR_ONLY_RESERVED;
        } else {
            g_message("VividProducer: consumer caps ignored unknown memoryHints[%u]=%s",
                      i,
                      hint ? hint : "(null)");
        }
    }
    return TRUE;
}

static gboolean
consumer_dmabuf_caps_parse_modifiers(ConsumerDmaBufCaps* caps,
                                     JsonObject*          object,
                                     GError**             error)
{
    if (!caps || !object || !json_object_has_member(object, VIVID_JSON_DMABUF_CAPS_MODIFIERS))
        return TRUE;

    JsonNode* member = json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_MODIFIERS);
    if (!member || !JSON_NODE_HOLDS_ARRAY(member)) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be an array",
                         VIVID_JSON_DMABUF_CAPS_MODIFIERS);
        return FALSE;
    }

    JsonArray* values = json_node_get_array(member);
    const guint length = json_array_get_length(values);
    for (guint i = 0; i < length; i++) {
        JsonNode* entry_node = json_array_get_element(values, i);
        if (!entry_node || !JSON_NODE_HOLDS_OBJECT(entry_node)) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] must be an object",
                             VIVID_JSON_DMABUF_CAPS_MODIFIERS,
                             i);
            return FALSE;
        }
        JsonObject* entry = json_node_get_object(entry_node);
        guint64 fourcc = 0;
        guint64 modifier = 0;
        guint64 plane_count = 0;
        if (!json_value_to_uint64(json_object_get_member(entry, VIVID_JSON_DMABUF_CAPS_FOURCC), &fourcc) ||
            !json_value_to_uint64(json_object_get_member(entry, VIVID_JSON_DMABUF_CAPS_MODIFIER), &modifier) ||
            !json_value_to_uint64(json_object_get_member(entry, VIVID_JSON_DMABUF_CAPS_PLANE_COUNT), &plane_count) ||
            plane_count == 0 ||
            plane_count > VIVID_DISPLAY_DMABUF_MAX_PLANES ||
            fourcc > G_MAXUINT32) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s[%u] requires uint32 fourcc, uint64 modifier, and planeCount in 1..%u",
                             VIVID_JSON_DMABUF_CAPS_MODIFIERS,
                             i,
                             VIVID_DISPLAY_DMABUF_MAX_PLANES);
            return FALSE;
        }
        ConsumerDmaBufModifier cap = {
            .fourcc = (guint32)fourcc,
            .modifier = modifier,
            .plane_count = (guint32)plane_count,
        };
        g_array_append_val(caps->modifiers, cap);
        uint32_array_append_unique(caps->fourccs, cap.fourcc);
        vivid_dmabuf_peer_caps_add_modifier(&caps->peer_caps,
                                            cap.fourcc,
                                            cap.modifier == DRM_FORMAT_MOD_INVALID
                                                ? DRM_FORMAT_MOD_LINEAR
                                                : cap.modifier,
                                            cap.plane_count);
        if (cap.modifier == DRM_FORMAT_MOD_LINEAR ||
            cap.modifier == DRM_FORMAT_MOD_INVALID)
            uint32_array_append_unique(caps->implicit_linear_fourccs, cap.fourcc);
    }
    return TRUE;
}

static gboolean
consumer_dmabuf_caps_parse(ConsumerDmaBufCaps* caps, JsonObject* root, GError** error)
{
    consumer_dmabuf_caps_clear(caps);
    consumer_dmabuf_caps_init(caps);

    JsonNode* caps_node = root && json_object_has_member(root, VIVID_JSON_CONSUMER_CAPS_DMABUF_CAPS)
        ? json_object_get_member(root, VIVID_JSON_CONSUMER_CAPS_DMABUF_CAPS)
        : NULL;
    if (!caps_node || !JSON_NODE_HOLDS_OBJECT(caps_node)) {
        set_buffer_error(error,
                         "consumer protocol error: missing required object dmabufCaps.version=%u",
                         DMABUF_CAPS_VERSION);
        return FALSE;
    }
    JsonObject* object = json_node_get_object(caps_node);

    guint64 version_value = 0;
    if (!json_value_to_uint64(json_object_get_member(object, VIVID_JSON_DMABUF_CAPS_VERSION),
                              &version_value) ||
        version_value > G_MAXUINT32) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.%s must be uint32 and equal %u",
                         VIVID_JSON_DMABUF_CAPS_VERSION,
                         DMABUF_CAPS_VERSION);
        return FALSE;
    }

    const guint32 version = (guint32)version_value;
    if (version != DMABUF_CAPS_VERSION) {
        set_buffer_error(error,
                         "consumer protocol error: dmabufCaps.version=%u is unsupported; "
                         "this producer requires version=%u",
                         version,
                         DMABUF_CAPS_VERSION);
        return FALSE;
    }

    caps->present = TRUE;
    caps->version = version;
    caps->backend = g_strdup(json_object_get_string_default(object, VIVID_JSON_DMABUF_CAPS_BACKEND, ""));
    caps->probe = g_strdup(json_object_get_string_default(object, VIVID_JSON_DMABUF_CAPS_PROBE, ""));
    caps->render_node = g_strdup(json_object_get_string_default(object,
                                                                VIVID_JSON_DMABUF_CAPS_RENDER_NODE,
                                                                ""));
    caps->vendor = g_strdup(json_object_get_string_default(object, VIVID_JSON_DMABUF_CAPS_VENDOR, ""));
    caps->pci_address = g_strdup(json_object_get_string_default(object, VIVID_JSON_DMABUF_CAPS_PCI_ADDRESS, ""));
    caps->skips_external_only_modifiers =
        json_object_get_boolean_default(object, VIVID_JSON_DMABUF_CAPS_SKIPS_EXTERNAL_ONLY_MODIFIERS, FALSE);

    if (!consumer_dmabuf_caps_parse_uint32_array(caps->fourccs,
                                                 object,
                                                 VIVID_JSON_DMABUF_CAPS_FOURCCS,
                                                 error) ||
        !consumer_dmabuf_caps_parse_uint32_array(caps->implicit_linear_fourccs,
                                                 object,
                                                 VIVID_JSON_DMABUF_CAPS_IMPLICIT_LINEAR_FOURCCS,
                                                 error) ||
        !consumer_dmabuf_caps_parse_modifiers(caps, object, error) ||
        !consumer_dmabuf_caps_parse_memory_hints(caps, object, error) ||
        !consumer_dmabuf_caps_parse_sync_caps(caps, object, error) ||
        !consumer_dmabuf_caps_parse_color_caps(caps, object, error) ||
        !consumer_dmabuf_caps_parse_relay_modes(caps, object, error) ||
        !consumer_dmabuf_caps_parse_extent_caps(caps, object, error)) {
        return FALSE;
    }
    consumer_dmabuf_caps_parse_uuid(caps,
                                    object,
                                    VIVID_JSON_DMABUF_CAPS_DEVICE_UUID,
                                    &caps->device_uuid,
                                    caps->peer_caps.identity.device_uuid);
    consumer_dmabuf_caps_parse_uuid(caps,
                                    object,
                                    VIVID_JSON_DMABUF_CAPS_DRIVER_UUID,
                                    &caps->driver_uuid,
                                    caps->peer_caps.identity.driver_uuid);
    if (caps->peer_caps.color_caps == 0)
        caps->peer_caps.color_caps = VIVID_DMABUF_DEFAULT_COLOR_CAPS;

    /*
     * v2/v3 caps are authoritative. Empty format sets are logged and rejected
     * instead of upgraded into a synthetic LINEAR path: CompatLinear is a
     * negotiated scheme, not an old-client fallback.
     */
    consumer_dmabuf_caps_parse_render_node_identity(caps);

    for (guint i = 0; caps->implicit_linear_fourccs &&
         i < caps->implicit_linear_fourccs->len; i++) {
        const guint32 fourcc =
            g_array_index(caps->implicit_linear_fourccs, guint32, i);
        if (!uint32_array_contains(caps->fourccs, fourcc)) {
            set_buffer_error(error,
                             "consumer protocol error: dmabufCaps.%s contains fourcc=0x%08x missing from dmabufCaps.%s",
                             VIVID_JSON_DMABUF_CAPS_IMPLICIT_LINEAR_FOURCCS,
                             fourcc,
                             VIVID_JSON_DMABUF_CAPS_FOURCCS);
            return FALSE;
        }
        vivid_dmabuf_peer_caps_add_modifier(&caps->peer_caps,
                                            fourcc,
                                            DRM_FORMAT_MOD_LINEAR,
                                            1);
    }

    if (caps->peer_caps.formats.n_modifiers == 0) {
        set_buffer_error(error,
                         "consumer backend=%s sent dmabufCaps.version=%u but no importable "
                         "(fourcc, modifier, planeCount) tuples",
                         caps->backend && *caps->backend ? caps->backend : "(unknown)",
                         version);
        return FALSE;
    }

    if (!caps->sync_explicit_acquire ||
        !caps->sync_release_syncobj ||
        (caps->peer_caps.sync_caps & VIVID_DMABUF_SYNC_CAP_SYNCOBJ_BINARY) == 0) {
        set_buffer_error(error,
                         "consumer backend=%s dmabufCaps.version=%u lacks required "
                         "explicit acquire sync_file and drm_syncobj release support "
                         "syncCaps=0x%x explicitAcquire=%s releaseSyncobj=%s",
                         caps->backend && *caps->backend ? caps->backend : "(unknown)",
                         version,
                         caps->peer_caps.sync_caps,
                         caps->sync_explicit_acquire ? "true" : "false",
                         caps->sync_release_syncobj ? "true" : "false");
        return FALSE;
    }

    return TRUE;
}

static gboolean
consumer_dmabuf_caps_supports_fourcc(const ConsumerDmaBufCaps* caps, guint32 fourcc)
{
    return !caps || !caps->present || uint32_array_contains(caps->fourccs, fourcc);
}

static gboolean
consumer_dmabuf_caps_supports_implicit_linear(const ConsumerDmaBufCaps* caps, guint32 fourcc)
{
    return !caps || !caps->present ||
        uint32_array_contains(caps->implicit_linear_fourccs, fourcc);
}

static gboolean
consumer_dmabuf_caps_supports_modifier(const ConsumerDmaBufCaps* caps,
                                       guint32                   fourcc,
                                       guint64                   modifier)
{
    if (!caps || !caps->present)
        return TRUE;
    if (modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID)
        return consumer_dmabuf_caps_supports_implicit_linear(caps, fourcc);

    for (guint i = 0; caps->modifiers && i < caps->modifiers->len; i++) {
        const ConsumerDmaBufModifier cap =
            g_array_index(caps->modifiers, ConsumerDmaBufModifier, i);
        if (cap.fourcc == fourcc && cap.modifier == modifier)
            return TRUE;
    }
    return FALSE;
}

static gboolean
dmabuf_modifier_is_implicit_linear(guint64 modifier)
{
    return modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;
}

static const gchar*
output_dmabuf_path_name(OutputDmaBufPath path)
{
    switch (path) {
    case OUTPUT_DMABUF_PATH_OPTIMIZED_SAME_DEVICE:
        return "optimized-same-device";
    case OUTPUT_DMABUF_PATH_COMPAT_LINEAR:
        return "compat-linear";
    case OUTPUT_DMABUF_PATH_COMPAT_CPU_READBACK_RESERVED:
        return "compat-cpu-readback-reserved";
    case OUTPUT_DMABUF_PATH_UNNEGOTIATED:
    default:
        return "unnegotiated";
    }
}

static const gchar*
dmabuf_memory_preference_name(VividProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return "host-visible";
    case VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return "device-local";
    case VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return "default";
    }
}

static void
renderer_dmabuf_request_add_fourcc(VividProducerRendererDmaBufRequest* request,
                                   guint32                              fourcc)
{
    if (!request || request->n_allowed_fourccs >= G_N_ELEMENTS(request->allowed_fourccs))
        return;

    for (guint i = 0; i < request->n_allowed_fourccs; i++) {
        if (request->allowed_fourccs[i] == fourcc)
            return;
    }

    request->allowed_fourccs[request->n_allowed_fourccs++] = fourcc;
}

static guint32
renderer_memory_preference_to_dmabuf_hints(VividProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL;
    case VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    case VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL |
            VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    }
}

static void
renderer_caps_to_peer_caps(const VividProducerRendererDmaBufCaps* renderer_caps,
                           const VividGpuDevice*                  resolved_gpu,
                           VividDmaBufPeerCaps*                   out_caps)
{
    vivid_dmabuf_peer_caps_init(out_caps);
    if (!renderer_caps || !out_caps)
        return;

    if (resolved_gpu)
        dmabuf_identity_fill_from_gpu(&out_caps->identity, resolved_gpu);
    out_caps->memory_hints =
        renderer_memory_preference_to_dmabuf_hints(renderer_caps->memory_preference);
    out_caps->sync_caps = VIVID_DMABUF_REQUIRED_SYNC_CAPS;
    out_caps->color_caps = VIVID_DMABUF_DEFAULT_COLOR_CAPS;

    for (guint32 i = 0; i < renderer_caps->n_caps; i++) {
        const VividProducerRendererDmaBufFormatCap* cap = &renderer_caps->caps[i];
        if (cap->plane_count == 0 || cap->plane_count > VIVID_DISPLAY_DMABUF_MAX_PLANES)
            continue;
        vivid_dmabuf_peer_caps_add_modifier(out_caps,
                                            cap->fourcc,
                                            cap->modifier == DRM_FORMAT_MOD_INVALID
                                                ? DRM_FORMAT_MOD_LINEAR
                                                : cap->modifier,
                                            cap->plane_count);
    }
}

static void
dmabuf_log_peer_caps(const gchar*              label,
                     const VividDmaBufPeerCaps* caps)
{
    if (!caps)
        return;

    g_message("VividProducer: %s DMA-BUF caps summary modifiers=%u relay=0x%x "
              "memory-hints=0x%x sync=0x%x color=0x%x drm=%u:%u render-node=%s "
              "blacklist=%u",
              label ? label : "peer",
              caps->formats.n_modifiers,
              caps->relay_modes,
              caps->memory_hints,
              caps->sync_caps,
              caps->color_caps,
              caps->identity.drm_render_major,
              caps->identity.drm_render_minor,
              caps->identity.render_node[0] ? caps->identity.render_node : "(unknown)",
              caps->n_blacklist);
    for (guint32 i = 0; i < caps->formats.n_modifiers; i++) {
        const VividDmaBufModifierCap* cap = &caps->formats.modifiers[i];
        g_message("VividProducer: %s DMA-BUF caps[%u] fourcc=0x%08x "
                  "modifier=0x%016" G_GINT64_MODIFIER "x planes=%u linear=%s",
                  label ? label : "peer",
                  i,
                  cap->fourcc,
                  (guint64)cap->modifier,
                  cap->plane_count,
                  dmabuf_modifier_is_implicit_linear(cap->modifier) ? "true" : "false");
    }
}

static gboolean
output_negotiate_linear_dmabuf(Output*                                output,
                               Producer*                              producer,
                               gboolean                               restrict_fourcc,
                               guint32                                fixed_fourcc,
                               gboolean                               allow_optimized_modifier,
                               VividProducerRendererDmaBufRequest*    request,
                               GError**                               error)
{
    const ConsumerDmaBufCaps* caps = output && output->client
        ? &output->client->dmabuf_caps
        : NULL;
    if (!caps || !caps->present) {
        set_buffer_error(error,
                         "consumer has not sent required dmabufCaps.version=3 before output registration");
        return FALSE;
    }
    VividGpuDevice resolved_gpu = {0};
    if (!vivid_producer_renderer_resolved_gpu(output_renderer(output), &resolved_gpu)) {
        set_buffer_error(error,
                         "render-device '%s' has no resolved GPU identity for DMA-BUF negotiation",
                         producer->config.render_device ? producer->config.render_device : "auto");
        return FALSE;
    }

    VividDmaBufPeerCaps producer_caps;
    VividProducerRendererDmaBufCaps renderer_caps = {0};
    gboolean have_renderer_caps = FALSE;
    if (request) {
        have_renderer_caps =
            vivid_producer_renderer_query_dmabuf_caps(output_renderer(output), &renderer_caps);
        if (!have_renderer_caps) {
            set_buffer_error(error, "renderer did not publish DMA-BUF format caps");
            return FALSE;
        }
        renderer_caps_to_peer_caps(&renderer_caps, &resolved_gpu, &producer_caps);
    } else {
        vivid_dmabuf_peer_caps_init(&producer_caps);
        dmabuf_identity_fill_from_gpu(&producer_caps.identity, &resolved_gpu);
        producer_caps.memory_hints = VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
        producer_caps.sync_caps = VIVID_DMABUF_REQUIRED_SYNC_CAPS;
        producer_caps.color_caps = VIVID_DMABUF_DEFAULT_COLOR_CAPS;
        vivid_dmabuf_peer_caps_add_modifier(&producer_caps,
                                            fixed_fourcc,
                                            DRM_FORMAT_MOD_LINEAR,
                                            1);
    }

    if (restrict_fourcc && request) {
        VividDmaBufPeerCaps restricted_caps;
        restricted_caps = producer_caps;
        restricted_caps.formats.n_modifiers = 0;
        for (guint32 i = 0; i < producer_caps.formats.n_modifiers; i++) {
            const VividDmaBufModifierCap* cap = &producer_caps.formats.modifiers[i];
            if (cap->fourcc == fixed_fourcc) {
                vivid_dmabuf_peer_caps_add_modifier(&restricted_caps,
                                                    cap->fourcc,
                                                    cap->modifier,
                                                    cap->plane_count);
            }
        }
        producer_caps = restricted_caps;
    }

    if (!allow_optimized_modifier) {
        for (guint32 i = 0; i < producer_caps.formats.n_modifiers; i++) {
            const VividDmaBufModifierCap* cap = &producer_caps.formats.modifiers[i];
            if (!dmabuf_modifier_is_implicit_linear(cap->modifier))
                vivid_dmabuf_peer_caps_blacklist_modifier(&producer_caps,
                                                          cap->fourcc,
                                                          cap->modifier);
        }
    }

    for (guint32 i = 0; output && i < output->n_producer_blacklist; i++) {
        const VividDmaBufModifierCap* entry = &output->producer_blacklist[i];
        vivid_dmabuf_peer_caps_blacklist_modifier(&producer_caps,
                                                  entry->fourcc,
                                                  entry->modifier);
    }

    dmabuf_log_peer_caps("producer", &producer_caps);
    dmabuf_log_peer_caps("consumer", &caps->peer_caps);

    VividDmaBufNegotiatedScheme scheme = {0};
    VividDmaBufNegotiateError negotiate_error = VIVID_DMABUF_NEGOTIATE_ERROR_NONE;
    if (!vivid_dmabuf_negotiate_pick(&producer_caps,
                                     &caps->peer_caps,
                                     &scheme,
                                     &negotiate_error)) {
        set_buffer_error(error,
                         "DMA-BUF negotiation failed with consumer backend=%s error=%u",
                         caps->backend && *caps->backend ? caps->backend : "(unknown)",
                         negotiate_error);
        return FALSE;
    }

    output->consumer_same_render_node = scheme.same_device;
    output->dmabuf_path =
        scheme.path == VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE
            ? OUTPUT_DMABUF_PATH_OPTIMIZED_SAME_DEVICE
            : (scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR
                   ? OUTPUT_DMABUF_PATH_COMPAT_LINEAR
                   : OUTPUT_DMABUF_PATH_COMPAT_CPU_READBACK_RESERVED);
    output->memory_preference =
        scheme.memory_source == VIVID_DMABUF_MEMORY_SOURCE_GPU_NATIVE
            ? VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
            : VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    if (request) {
        memset(request, 0, sizeof(*request));
        request->required_modifier = scheme.modifier;
        request->required_plane_count = scheme.plane_count;
        request->require_modifier = TRUE;
        request->memory_preference = output->memory_preference;
        request->debug_label = output_dmabuf_path_name(output->dmabuf_path);
        renderer_dmabuf_request_add_fourcc(request, scheme.fourcc);
    }

    g_autofree gchar* producer_uuid = gpu_uuid_to_hex(resolved_gpu.uuid);
    g_autofree gchar* consumer_uuid =
        caps ? gpu_uuid_to_hex(caps->peer_caps.identity.device_uuid) : g_strdup("");
    g_message("VividProducer: negotiated DMA-BUF output=%u path=%s relay=%s memory=%s "
              "memory-source=%s producer-render-node=%s producer-drm=%u:%u "
              "producer-uuid=%s consumer-render-node=%s consumer-drm=%u:%u "
              "consumer-uuid=%s producer-caps=%u allowed-fourccs=%u "
              "chosen-fourcc=0x%08x chosen-modifier=0x%016"
              G_GINT64_MODIFIER "x chosen-planes=%u same-device=%s "
              "identity=%s sync=0x%x color=0x%x",
              output->output_id,
              output_dmabuf_path_name(output->dmabuf_path),
              vivid_dmabuf_relay_mode_name(scheme.relay_mode),
              dmabuf_memory_preference_name(output->memory_preference),
              vivid_dmabuf_memory_source_name(scheme.memory_source),
              resolved_gpu.render_node[0] ? resolved_gpu.render_node : "(unresolved)",
              resolved_gpu.drm_render_major,
              resolved_gpu.drm_render_minor,
              producer_uuid && *producer_uuid ? producer_uuid : "(unknown)",
              caps && caps->render_node && caps->render_node[0]
                  ? caps->render_node
                  : "(unknown)",
              caps ? caps->peer_caps.identity.drm_render_major : 0,
              caps ? caps->peer_caps.identity.drm_render_minor : 0,
              caps && caps->device_uuid && *caps->device_uuid
                  ? caps->device_uuid
                  : (consumer_uuid && *consumer_uuid ? consumer_uuid : "(unknown)"),
              have_renderer_caps ? renderer_caps.n_caps : 0,
              request ? request->n_allowed_fourccs : (restrict_fourcc ? 1u : 0u),
              scheme.fourcc,
              (guint64)scheme.modifier,
              scheme.plane_count,
              scheme.same_device ? "true" : "false",
              caps && caps->identity_mode && *caps->identity_mode
                  ? caps->identity_mode
                  : "(unknown)",
              caps ? caps->peer_caps.sync_caps : 0,
              caps ? caps->peer_caps.color_caps : 0);
    return TRUE;
}

static gchar*
gpu_uuid_to_hex(const guint8 uuid[VIVID_GPU_DEVICE_UUID_BYTES])
{
    GString* text = g_string_sized_new(VIVID_GPU_DEVICE_UUID_BYTES * 2);
    for (guint i = 0; i < VIVID_GPU_DEVICE_UUID_BYTES; i++)
        g_string_append_printf(text, "%02x", uuid[i]);
    return g_string_free(text, FALSE);
}

static const gchar*
output_memory_source_name(const Output* output)
{
    if (!output)
        return "unknown";
    if (output->dmabuf_path == OUTPUT_DMABUF_PATH_OPTIMIZED_SAME_DEVICE)
        return "gpu-native";
    if (output->dmabuf_path == OUTPUT_DMABUF_PATH_COMPAT_LINEAR &&
        output->memory_preference == VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE)
        return "gpu-linear";
    if (output->dmabuf_path == OUTPUT_DMABUF_PATH_COMPAT_CPU_READBACK_RESERVED)
        return "dmabuf-heap-reserved";
    return output->memory_preference == VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
        ? "gpu-native"
        : "gpu-linear";
}

static JsonObject*
gpu_device_to_json_object(const VividGpuDevice* device)
{
    JsonObject* object = json_object_new();
    g_autofree gchar* uuid = gpu_uuid_to_hex(device->uuid);
    g_autofree gchar* driver_uuid = gpu_uuid_to_hex(device->driver_uuid);

    json_object_set_string_member(object, "render-node", device->render_node);
    json_object_set_string_member(object, "name", device->name);
    json_object_set_string_member(object, "pci-address", device->pci_address);
    json_object_set_string_member(object, "vendor",
                                  vivid_gpu_vendor_name(device->vendor_id));
    json_object_set_int_member(object, "vendor-id", device->vendor_id);
    json_object_set_string_member(object, "uuid", uuid);
    json_object_set_string_member(object, "driver-uuid", driver_uuid);
    json_object_set_boolean_member(object, "is-discrete", device->is_discrete);
    return object;
}

static JsonArray*
build_gpu_devices_json_array(VividProducerRenderer* renderer)
{
    JsonArray* array = json_array_new();
    const VividGpuDeviceList* devices =
        vivid_producer_renderer_gpu_devices(renderer);
    if (!devices)
        return array;

    for (guint i = 0; i < devices->n_devices; i++)
        json_array_add_object_element(array,
                                      gpu_device_to_json_object(&devices->devices[i]));
    return array;
}

static JsonObject*
output_to_json_object(const Output* output, const Output* primary)
{
    JsonObject* object = json_object_new();
    json_object_set_int_member(object, "outputId", output->output_id);
    json_object_set_int_member(object, "consumerOutputId", output->consumer_output_id);
    json_object_set_int_member(object, "monitorIndex", output->monitor_index);
    json_object_set_int_member(object, "x", output->display_x);
    json_object_set_int_member(object, "y", output->display_y);
    json_object_set_int_member(object, "logicalWidth", output->logical_width);
    json_object_set_int_member(object, "logicalHeight", output->logical_height);
    json_object_set_int_member(object, "physicalWidth", output->target_width);
    json_object_set_int_member(object, "physicalHeight", output->target_height);
    json_object_set_int_member(object, "routeWidth", output->width);
    json_object_set_int_member(object, "routeHeight", output->height);
    json_object_set_double_member(object, "scale", output->scale);
    json_object_set_int_member(object, "refreshRateMhz", output->refresh_rate_mhz);
    json_object_set_boolean_member(object, "primary", output == primary);
    json_object_set_boolean_member(object, "active", output->active);
    json_object_set_string_member(object,
                                  "desktop",
                                  output->desktop ? output->desktop : "");
    json_object_set_string_member(object,
                                  "displayKey",
                                  output->display_key ? output->display_key : "");
    json_object_set_string_member(object,
                                  "displayName",
                                  output->display_name ? output->display_name : "");
    return object;
}

static JsonArray*
build_outputs_json_array(Producer* producer)
{
    JsonArray* array = json_array_new();
    Output* primary = producer_primary_output(producer);
    for (guint client_index = 0; producer && client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            json_array_add_object_element(array,
                                          output_to_json_object(output, primary));
        }
    }
    return array;
}

static gchar*
build_control_state_json(Producer* producer)
{
    g_autofree gchar* config_json =
        vivid_producer_config_to_json(&producer->config);
    g_autoptr(JsonParser) parser = json_parser_new();
    GError* error = NULL;
    if (!json_parser_load_from_data(parser, config_json, -1, &error)) {
        g_warning("VividProducer: failed to extend control state JSON: %s",
                  error ? error->message : "unknown parse error");
        g_clear_error(&error);
        return g_strdup(config_json);
    }

    JsonNode* root = json_parser_get_root(parser);
    JsonObject* object = root && JSON_NODE_HOLDS_OBJECT(root)
        ? json_node_get_object(root)
        : NULL;
    if (!object)
        return g_strdup(config_json);

    /*
     * Config is the user's requested render-device value. The resolved fields
     * are runtime facts from the renderer after "auto" or a render node was
     * mapped to a concrete Vulkan physical device. WebUI and the consumer must
     * use these fields instead of re-resolving or guessing independently.
     */
    json_object_set_array_member(object,
                                 "gpu-devices",
                                 build_gpu_devices_json_array(producer->renderer));
    json_object_set_array_member(object,
                                 "outputs",
                                 build_outputs_json_array(producer));

    VividGpuDevice resolved_gpu;
    if (vivid_producer_renderer_resolved_gpu(producer->renderer, &resolved_gpu)) {
        g_autofree gchar* uuid = gpu_uuid_to_hex(resolved_gpu.uuid);
        g_autofree gchar* driver_uuid = gpu_uuid_to_hex(resolved_gpu.driver_uuid);
        json_object_set_string_member(object,
                                      "resolved-render-node",
                                      resolved_gpu.render_node);
        json_object_set_string_member(object,
                                      "resolved-vendor",
                                      vivid_gpu_vendor_name(resolved_gpu.vendor_id));
        json_object_set_int_member(object,
                                   "resolved-vendor-id",
                                   resolved_gpu.vendor_id);
        json_object_set_string_member(object,
                                      "resolved-device-name",
                                      resolved_gpu.name);
        json_object_set_string_member(object,
                                      "resolved-pci-address",
                                      resolved_gpu.pci_address);
        json_object_set_string_member(object,
                                      "resolved-device-uuid",
                                      uuid);
        json_object_set_string_member(object,
                                      "resolved-driver-uuid",
                                      driver_uuid);
    } else {
        json_object_set_string_member(object, "resolved-render-node", "");
        json_object_set_string_member(object, "resolved-vendor", "unknown");
        json_object_set_int_member(object, "resolved-vendor-id", 0);
        json_object_set_string_member(object, "resolved-device-name", "");
        json_object_set_string_member(object, "resolved-pci-address", "");
        json_object_set_string_member(object, "resolved-device-uuid", "");
        json_object_set_string_member(object, "resolved-driver-uuid", "");
    }

    return json_node_to_compact_string(root);
}

static void
write_u16_le(guint8* bytes, guint16 value)
{
    bytes[0] = (guint8)(value & 0xffu);
    bytes[1] = (guint8)((value >> 8) & 0xffu);
}

static void
write_u32_le(guint8* bytes, guint32 value)
{
    bytes[0] = (guint8)(value & 0xffu);
    bytes[1] = (guint8)((value >> 8) & 0xffu);
    bytes[2] = (guint8)((value >> 16) & 0xffu);
    bytes[3] = (guint8)((value >> 24) & 0xffu);
}

static guint32
read_u32_le(const guint8* bytes)
{
    return (guint32)bytes[0] |
        ((guint32)bytes[1] << 8) |
        ((guint32)bytes[2] << 16) |
        ((guint32)bytes[3] << 24);
}

static guint64
read_u64_le(const guint8* bytes)
{
    return (guint64)read_u32_le(bytes) |
        ((guint64)read_u32_le(bytes + 4) << 32);
}

static GQuark
vivid_producer_buffer_error_quark(void)
{
    return g_quark_from_static_string("vivid-producer-buffer-error");
}

static void
set_buffer_error(GError** error, const gchar* format, ...)
{
    if (!error || *error)
        return;

    va_list args;
    va_start(args, format);
    gchar* message = g_strdup_vprintf(format, args);
    va_end(args);

    g_set_error_literal(error,
                        VIVID_PRODUCER_BUFFER_ERROR,
                        VIVID_PRODUCER_BUFFER_ERROR_FAILED,
                        message);
    g_free(message);
}

static gdouble
read_f64_le(const guint8* bytes)
{
    const guint64 raw = read_u64_le(bytes);
    gdouble value = 0.0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void
write_u64_le(guint8* bytes, guint64 value)
{
    write_u32_le(bytes, (guint32)(value & 0xffffffffu));
    write_u32_le(bytes + 4, (guint32)(value >> 32));
}

static guint64
producer_next_config_generation(Producer* producer)
{
    if (!producer)
        return 0;
    if (producer->next_config_generation == G_MAXUINT64)
        producer->next_config_generation = 0;
    return ++producer->next_config_generation;
}

static gchar*
default_socket_path(void)
{
    const gchar* env_socket = g_getenv("VIVID_DISPLAY_SOCKET");
    if (env_socket && *env_socket)
        return g_strdup(env_socket);

    const gchar* runtime_dir = g_get_user_runtime_dir();
    gchar* host_runtime_dir = NULL;

    /*
     * Flatpak sets XDG_RUNTIME_DIR to /run/user/$uid/app/$appid. The GNOME
     * Shell extension runs outside the sandbox and connects to the host runtime
     * dir, so a packaged producer must deliberately publish the socket under
     * /run/user/$uid/vivid instead of inside the app-private runtime dir.
     */
    if (g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)) {
        g_autofree gchar* parent = g_path_get_dirname(runtime_dir);
        g_autofree gchar* parent_name = g_path_get_basename(parent);
        if (g_strcmp0(parent_name, "app") == 0)
            host_runtime_dir = g_path_get_dirname(parent);
    }

    if (!host_runtime_dir)
        host_runtime_dir = g_strdup(runtime_dir);

    gchar* socket_path =
        g_build_filename(host_runtime_dir, "vivid", "display-v1.sock", NULL);
    g_free(host_runtime_dir);
    return socket_path;
}

static gboolean
set_fd_nonblocking(gint fd)
{
    const gint flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return FALSE;
    if ((flags & O_NONBLOCK) != 0)
        return TRUE;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void
client_outbox_entry_free(gpointer data)
{
    ClientOutboxEntry* entry = data;
    if (!entry)
        return;
    if (entry->data)
        g_byte_array_unref(entry->data);
    g_free(entry);
}

static void
client_outbox_clear(Client* client)
{
    if (!client)
        return;

    while (!g_queue_is_empty(&client->outbox)) {
        ClientOutboxEntry* entry = g_queue_pop_head(&client->outbox);
        client_outbox_entry_free(entry);
    }
    client->outbox_pending_bytes = 0;
    client->outbox_offset = 0;
}

static void
client_outbox_update_write_watch(Client* client)
{
    if (!client || client->fd < 0)
        return;

    if (g_queue_is_empty(&client->outbox)) {
        if (client->write_source_id != 0) {
            g_source_remove(client->write_source_id);
            client->write_source_id = 0;
        }
        return;
    }

    if (client->write_source_id != 0)
        return;

    /*
     * Client owns the socket. A GIOChannel with close-on-unref transfers that
     * ownership to the watch and closes the socket whenever the watch is
     * replaced. Rebind bursts commonly enqueue several frames before G_IO_OUT,
     * so replacing such a watch could close client->fd and let Vulkan reuse the
     * descriptor number for a non-socket before the control ACK was sent.
     */
    GSource* source =
        g_unix_fd_source_new(client->fd,
                             G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL);
    g_source_set_callback(source, (GSourceFunc)client_write_ready, client, NULL);
    g_source_set_name(source, "vivid-producer-client-outbox");
    client->write_source_id = g_source_attach(source, NULL);
    g_source_unref(source);
}

static gboolean
client_flush_outbox(Client* client)
{
    if (!client || client->fd < 0)
        return FALSE;

    while (!g_queue_is_empty(&client->outbox)) {
        ClientOutboxEntry* entry = g_queue_peek_head(&client->outbox);
        if (!entry || !entry->data || entry->data->len == 0) {
            g_queue_pop_head(&client->outbox);
            client_outbox_entry_free(entry);
            client->outbox_offset = 0;
            continue;
        }

        const gsize remaining = entry->data->len - client->outbox_offset;
        const ssize_t sent =
            vivid_display_send_bytes_nonblocking(client->fd,
                                                  entry->data->data + client->outbox_offset,
                                                  remaining);
        if (sent < 0) {
            const gint error = (gint)-sent;
            if (error == EAGAIN || error == EWOULDBLOCK)
                return FALSE;
            g_warning("VividProducer: outbox send failed: %s", g_strerror(error));
            return FALSE;
        }
        if (sent == 0)
            return FALSE;

        client->outbox_offset += (gsize)sent;
        if (client->outbox_offset >= entry->data->len) {
            g_queue_pop_head(&client->outbox);
            client->outbox_pending_bytes -= entry->data->len;
            client_outbox_entry_free(entry);
            client->outbox_offset = 0;
        }
    }

    return TRUE;
}

static gboolean
client_prepare_fd_send(Client* client, guint16 opcode)
{
    if (!client || client->fd < 0)
        return FALSE;

    /*
     * Ancillary data cannot be resumed after a partial sendmsg(). Do not let
     * an fd-bearing event overtake an earlier plain event that is still queued
     * because the non-blocking socket was briefly full. Retrying the higher
     * level operation is safer than corrupting the consumer's frame stream.
     */
    if (g_queue_is_empty(&client->outbox))
        return TRUE;

    client_flush_outbox(client);
    if (g_queue_is_empty(&client->outbox))
        return TRUE;

    g_warning("VividProducer: fd-bearing frame opcode=%u blocked behind "
              "queued output; deferring",
              opcode);
    return FALSE;
}

static gboolean
client_write_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    Client* client = user_data;
    (void)fd;
    if (!client)
        return G_SOURCE_REMOVE;

    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
        client->write_source_id = 0;
        client_disconnect(client, "socket write error");
        return G_SOURCE_REMOVE;
    }

    if (client_flush_outbox(client)) {
        client->write_source_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static GByteArray*
client_build_frame_bytes(guint16 opcode, const guint8* body, gsize body_len)
{
    if (body_len > VIVID_DISPLAY_CODEC_MAX_BODY_BYTES)
        return NULL;

    GByteArray* frame = g_byte_array_sized_new(4 + body_len);
    const guint16 wire_len = (guint16)(body_len + 4);
    g_byte_array_append(frame, (const guint8*)&opcode, sizeof(opcode));
    g_byte_array_append(frame, (const guint8*)&wire_len, sizeof(wire_len));
    if (body_len > 0 && body)
        g_byte_array_append(frame, body, body_len);
    return frame;
}

static gboolean
client_queue_frame_bytes(Client* client, GByteArray* frame)
{
    if (!client || !frame || frame->len == 0)
        return FALSE;

    if (client->outbox_pending_bytes + frame->len > CLIENT_OUTBOX_MAX_BYTES) {
        g_warning("VividProducer: client outbox exceeded %u bytes; refusing frame",
                  (guint)CLIENT_OUTBOX_MAX_BYTES);
        g_byte_array_unref(frame);
        return FALSE;
    }

    ClientOutboxEntry* entry = g_new0(ClientOutboxEntry, 1);
    entry->data = frame;
    g_queue_push_tail(&client->outbox, entry);
    client->outbox_pending_bytes += frame->len;

    client_flush_outbox(client);
    client_outbox_update_write_watch(client);

    return TRUE;
}

static gboolean
client_send_frame(Client* client,
                  guint16 opcode,
                  const guint8* body,
                  gsize body_len,
                  const int* fds,
                  gsize n_fds)
{
    if (!client || client->fd < 0)
        return FALSE;

    if (n_fds > 0) {
        if (!client_prepare_fd_send(client, opcode))
            return FALSE;

        const gint result =
            vivid_display_send_frame(client->fd, opcode, body, body_len, fds, n_fds);
        if (result == -EAGAIN || result == -EWOULDBLOCK) {
            g_warning("VividProducer: fd-bearing frame opcode=%u blocked",
                      opcode);
            return FALSE;
        }
        if (result < 0) {
            g_warning("VividProducer: fd-bearing frame opcode=%u failed: %s",
                      opcode,
                      g_strerror(-result));
            return FALSE;
        }
        return TRUE;
    }

    g_autoptr(GByteArray) frame = client_build_frame_bytes(opcode, body, body_len);
    if (!frame)
        return FALSE;

    /* Preserve wire order whenever a previous plain frame is already queued. */
    if (!g_queue_is_empty(&client->outbox))
        return client_queue_frame_bytes(client, g_steal_pointer(&frame));

    /*
     * Try an immediate send before queueing so the common non-blocking path
     * stays hot. Any remainder is queued and drained on G_IO_OUT.
     */
    const ssize_t sent =
        vivid_display_send_bytes_nonblocking(client->fd, frame->data, frame->len);
    if (sent == (ssize_t)frame->len)
        return TRUE;
    if (sent < 0) {
        const gint error = (gint)-sent;
        if (error != EAGAIN && error != EWOULDBLOCK) {
            g_warning("VividProducer: frame opcode=%u send failed: %s",
                      opcode,
                      g_strerror(error));
            return FALSE;
        }
    }

    if (sent > 0) {
        g_byte_array_remove_range(frame, 0, (guint)sent);
        if (frame->len == 0)
            return TRUE;
    }

    return client_queue_frame_bytes(client, g_steal_pointer(&frame));
}

static void
client_disconnect(Client* client, const gchar* reason)
{
    if (!client)
        return;

    if (reason && *reason)
        g_message("VividProducer: disconnecting client: %s", reason);
    client_free(client);
}

static gboolean
send_json_frame(Client* client, guint16 opcode, const gchar* json)
{
    const guint8* body = (const guint8*)(json ? json : "{}");
    const gsize body_len = strlen((const gchar*)body);
    return client_send_frame(client, opcode, body, body_len, NULL, 0);
}

static gboolean
send_control_json(Client* client,
                  guint16 control_opcode,
                  const gchar* json,
                  guint64 request_id)
{
    const gchar* payload = json ? json : "{}";
    const gsize json_len = strlen(payload);
    if (json_len > VIVID_DISPLAY_CODEC_MAX_BODY_BYTES - VIVID_DISPLAY_CONTROL_HEADER_BYTES) {
        g_warning("VividProducer: control payload too large: %" G_GSIZE_FORMAT, json_len);
        return FALSE;
    }

    g_autofree guint8* body = g_malloc0(VIVID_DISPLAY_CONTROL_HEADER_BYTES + json_len);
    vivid_display_control_header_encode(body,
                                         control_opcode,
                                         0,
                                         (guint32)json_len);
    memcpy(body + VIVID_DISPLAY_CONTROL_HEADER_BYTES, payload, json_len);

    return client_send_frame(client,
                              VIVID_DISPLAY_EVT_CONTROL,
                              body,
                              VIVID_DISPLAY_CONTROL_HEADER_BYTES + json_len,
                              NULL,
                              0);
}

static gboolean
send_control_ack(Client* client,
                 guint16 request_opcode,
                 gboolean saved,
                 guint64 request_id)
{
    g_autofree gchar* json =
        g_strdup_printf("{\"ok\":true,\"requestOpcode\":%u,\"requestId\":%"
                      G_GUINT64_FORMAT ",\"configSaved\":%s}",
                      request_opcode,
                      request_id,
                      saved ? "true" : "false");
    return send_control_json(client, VIVID_DISPLAY_CONTROL_ACK, json, request_id);
}

static gboolean
send_control_error(Client* client,
                   guint16 request_opcode,
                   guint code,
                   guint64 request_id,
                   const gchar* message)
{
    g_autofree gchar* escaped =
        g_strescape(message ? message : "invalid control request", NULL);
    g_autofree gchar* json =
        g_strdup_printf("{\"%s\":false,\"%s\":%u,\"%s\":%"
                        G_GUINT64_FORMAT ",\"%s\":%u,\"%s\":\"%s\"}",
                        VIVID_JSON_CONTROL_ENVELOPE_OK,
                        VIVID_JSON_CONTROL_ENVELOPE_REQUEST_OPCODE,
                        request_opcode,
                        VIVID_JSON_CONTROL_ENVELOPE_REQUEST_ID,
                        request_id,
                        VIVID_JSON_CONTROL_ENVELOPE_CODE,
                        code,
                        VIVID_JSON_CONTROL_ENVELOPE_MESSAGE,
                        escaped);
    return send_control_json(client, VIVID_DISPLAY_CONTROL_ERROR, json, request_id);
}

static gboolean
send_display_error(Client* client, guint code, gboolean fatal, const gchar* format, ...)
{
    va_list args;
    va_start(args, format);
    g_autofree gchar* message = g_strdup_vprintf(format, args);
    va_end(args);

    const char* error_name = vivid_display_error_name((int)code);
    g_warning("VividProducer: display error code=%u name=%s domain=%s fatal=%s: %s",
              code,
              error_name ? error_name : "UNKNOWN",
              vivid_display_error_domain((int)code),
              fatal ? "true" : "false",
              message ? message : "(none)");

    g_autofree gchar* escaped =
        g_strescape(message ? message : "display producer error", NULL);
    g_autofree gchar* json =
        g_strdup_printf("{\"%s\":%u,\"%s\":%s,\"%s\":\"%s\"}",
                        VIVID_JSON_EVT_ERROR_CODE,
                        code,
                        VIVID_JSON_EVT_ERROR_FATAL,
                        fatal ? "true" : "false",
                        VIVID_JSON_EVT_ERROR_MESSAGE,
                        escaped);
    return send_json_frame(client, VIVID_DISPLAY_EVT_ERROR, json);
}

static gboolean
client_protocol_error(Client* client, guint code, const gchar* format, ...)
{
    va_list args;
    va_start(args, format);
    g_autofree gchar* message = g_strdup_vprintf(format, args);
    va_end(args);

    send_display_error(client, code, TRUE, "%s", message);
    return FALSE;
}

static gboolean
send_welcome(Client* client, guint32 negotiated_version)
{
    g_autofree gchar* json =
        g_strdup_printf("{\"%s\":\"%s\","
                        "\"%s\":%u,"
                        "\"%s\":%u,"
                        "\"%s\":[\"socket-control-v1\","
                        "\"dmabuf-buffer-transport-v2\","
                        "\"explicit-sync-fd-v1\","
                        "\"dmabuf-bind-failed-v1\","
                        "\"dmabuf-unbind-done-v1\","
                        "\"unbind-v2\","
                        "\"producer-config-json-v1\","
                        "\"window-state-policy-v1\"]}",
                        VIVID_JSON_WELCOME_PROTOCOL,
                        VIVID_DISPLAY_PROTOCOL_NAME,
                        VIVID_JSON_WELCOME_VERSION,
                        VIVID_DISPLAY_PROTOCOL_VERSION,
                        VIVID_JSON_WELCOME_NEGOTIATED_VERSION,
                        negotiated_version,
                        VIVID_JSON_WELCOME_FEATURES);
    return send_json_frame(client, VIVID_DISPLAY_EVT_WELCOME, json);
}

static gboolean
send_output_accepted(Client* client, const Output* output)
{
    g_autofree gchar* json =
        g_strdup_printf("{\"%s\":%u,"
                        "\"%s\":%u,"
                        "\"%s\":%u,"
                        "\"%s\":%s,"
                        "\"%s\":%u,"
                        "\"%s\":%u,"
                        "\"%s\":%u,"
                        "\"%s\":%u,"
                        "\"%s\":%.6f}",
                        VIVID_JSON_OUTPUT_ACCEPTED_CONSUMER_OUTPUT_ID,
                        output->consumer_output_id,
                        VIVID_JSON_OUTPUT_ACCEPTED_OUTPUT_ID,
                        output->output_id,
                        VIVID_JSON_OUTPUT_ACCEPTED_MONITOR_INDEX,
                        output->monitor_index,
                        VIVID_JSON_OUTPUT_ACCEPTED_ACTIVE,
                        output->active ? "true" : "false",
                        VIVID_JSON_OUTPUT_ACCEPTED_LOGICAL_WIDTH,
                        output->logical_width,
                        VIVID_JSON_OUTPUT_ACCEPTED_LOGICAL_HEIGHT,
                        output->logical_height,
                        VIVID_JSON_OUTPUT_ACCEPTED_WIDTH,
                        output->width,
                        VIVID_JSON_OUTPUT_ACCEPTED_HEIGHT,
                        output->height,
                        VIVID_JSON_OUTPUT_ACCEPTED_SCALE,
                        output->scale);

    return send_json_frame(client, VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED, json);
}

static gboolean
producer_try_open_render_node(Producer* producer,
                              const gchar* path,
                              gboolean     warn_on_failure)
{
    const gint fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (warn_on_failure) {
            g_warning("VividProducer: failed to open DRM render node %s: %s",
                      path,
                      g_strerror(errno));
        } else if (errno != ENOENT) {
            g_debug("VividProducer: skipped DRM render node %s: %s",
                    path,
                    g_strerror(errno));
        }
        return FALSE;
    }

    struct gbm_device* device = gbm_create_device(fd);
    if (!device) {
        if (warn_on_failure) {
            g_warning("VividProducer: gbm_create_device failed for %s", path);
        } else {
            g_debug("VividProducer: gbm_create_device failed for %s", path);
        }
        close(fd);
        return FALSE;
    }

    producer->render_fd = fd;
    producer->gbm_device = device;
    producer->render_node_path = g_strdup(path);

    const guint32 preferred_flags = GBM_BO_USE_WRITE | GBM_BO_USE_LINEAR;
    g_message("VividProducer: using DRM render node %s with GBM backend %s; "
              "XRGB8888 CPU-write linear flags supported=%s",
              path,
              gbm_device_get_backend_name(device),
              gbm_device_is_format_supported(device,
                                             DRM_FORMAT_XRGB8888,
                                             preferred_flags)
                  ? "true"
                  : "false");
    return TRUE;
}

static gboolean
producer_open_dmabuf_allocator(Producer* producer)
{
    const gchar* env_render_node = g_getenv("VIVID_DRM_RENDER_NODE");
    if (env_render_node && *env_render_node)
        return producer_try_open_render_node(producer, env_render_node, TRUE);

    for (gint node = DRM_RENDER_NODE_FIRST; node <= DRM_RENDER_NODE_LAST; node++) {
        g_autofree gchar* path = g_strdup_printf("/dev/dri/renderD%d", node);
        if (producer_try_open_render_node(producer, path, FALSE))
            return TRUE;
    }

    g_warning("VividProducer: no usable DRM render node found under /dev/dri/renderD%d..renderD%d; "
              "DMA-BUF production cannot start. Set VIVID_DRM_RENDER_NODE to a usable render node "
              "if the device is exposed somewhere else.",
              DRM_RENDER_NODE_FIRST,
              DRM_RENDER_NODE_LAST);
    return FALSE;
}

static void
producer_close_dmabuf_allocator(Producer* producer)
{
    if (producer->gbm_device) {
        gbm_device_destroy(producer->gbm_device);
        producer->gbm_device = NULL;
    }

    if (producer->render_fd >= 0) {
        close(producer->render_fd);
        producer->render_fd = -1;
    }

    g_clear_pointer(&producer->render_node_path, g_free);
}

static struct gbm_bo*
create_output_bo(Producer* producer,
                 guint32   width,
                 guint32   height,
                 GError**  error)
{
    const uint64_t linear_modifier = DRM_FORMAT_MOD_LINEAR;
    const guint32 write_flags = GBM_BO_USE_WRITE;
    g_autofree gchar* modifier_error = NULL;
    g_autofree gchar* linear_error = NULL;
    g_autofree gchar* write_error = NULL;

    /*
     * Prefer an explicitly linear DMA-BUF because it is the easiest format for
     * every consumer to import and for the current producer renderer to update
     * from the CPU. Do not mix GBM_BO_USE_RENDERING with GBM_BO_USE_WRITE here:
     * NVIDIA GBM advertises that combination as supported on some systems but
     * rejects actual allocations with EINVAL. The producer is not rendering
     * into this BO with GL/Vulkan yet, so CPU-write flags express the real
     * ownership model more accurately.
     */
    errno = 0;
    struct gbm_bo* bo =
        gbm_bo_create_with_modifiers2(producer->gbm_device,
                                      width,
                                      height,
                                      DRM_FORMAT_XRGB8888,
                                      &linear_modifier,
                                      1,
                                      write_flags);
    if (bo)
        return bo;

    modifier_error =
        g_strdup(errno != 0 ? g_strerror(errno) : "driver rejected format/modifier");
    g_message("VividProducer: linear modifier GBM BO allocation failed for %ux%u: %s",
              width,
              height,
              modifier_error);

    const guint32 linear_flags = GBM_BO_USE_WRITE | GBM_BO_USE_LINEAR;
    errno = 0;
    bo = gbm_bo_create(producer->gbm_device,
                       width,
                       height,
                       DRM_FORMAT_XRGB8888,
                       linear_flags);
    if (bo)
        return bo;

    linear_error =
        g_strdup(errno != 0 ? g_strerror(errno) : "driver rejected flags");
    g_message("VividProducer: linear GBM BO allocation failed for %ux%u flags=0x%x: %s",
              width,
              height,
              linear_flags,
              linear_error);

    errno = 0;
    bo = gbm_bo_create(producer->gbm_device,
                       width,
                       height,
                       DRM_FORMAT_XRGB8888,
                       write_flags);
    if (bo)
        return bo;

    write_error =
        g_strdup(errno != 0 ? g_strerror(errno) : "driver rejected flags");
    g_warning("VividProducer: DMA-BUF GBM BO allocation failed for %ux%u flags=0x%x: %s",
              width,
              height,
              write_flags,
              write_error);
    set_buffer_error(error,
                     "GBM BO allocation failed on render node %s for %ux%u: "
                     "with linear modifier failed: %s; "
                     "with GBM_BO_USE_LINEAR|GBM_BO_USE_WRITE failed: %s; "
                     "with GBM_BO_USE_WRITE failed: %s",
                     producer->render_node_path ? producer->render_node_path : "(none)",
                     width,
                     height,
                     modifier_error,
                     linear_error,
                     write_error);
    return NULL;
}

static gboolean
fill_output_buffer(Producer* producer, Output* output, GError** error)
{
    if (!output->bo) {
        set_buffer_error(error, "cannot fill output=%u without a GBM BO", output->output_id);
        return FALSE;
    }

    void* map_data = NULL;
    guint32 map_stride = 0;
    errno = 0;
    guint8* pixels = gbm_bo_map(output->bo,
                                0,
                                0,
                                output->width,
                                output->height,
                                GBM_BO_TRANSFER_WRITE,
                                &map_stride,
                                &map_data);
    if (pixels) {
        if (map_stride < output->width * 4u) {
            g_warning("VividProducer: gbm_bo_map returned invalid stride=%u for output=%u",
                      map_stride,
                      output->output_id);
            gbm_bo_unmap(output->bo, map_data);
            set_buffer_error(error,
                             "gbm_bo_map returned invalid stride=%u for output=%u size=%ux%u",
                             map_stride,
                             output->output_id,
                             output->width,
                             output->height);
            return FALSE;
        }

        vivid_producer_renderer_write_frame(output_renderer(output),
                                             pixels,
                                             map_stride,
                                             output->width,
                                             output->height,
                                             output->sequence);
        gbm_bo_unmap(output->bo, map_data);
        return TRUE;
    }

    g_message("VividProducer: gbm_bo_map failed for output=%u: %s; trying gbm_bo_write",
              output->output_id,
              errno != 0 ? g_strerror(errno) : "driver rejected CPU map");

    const gsize buffer_size = output->buffers[0].size;
    g_autofree guint8* staging = g_malloc0(buffer_size);
    vivid_producer_renderer_write_frame(output_renderer(output),
                                         staging,
                                         output->stride,
                                         output->width,
                                         output->height,
                                         output->sequence);
    errno = 0;
    if (gbm_bo_write(output->bo, staging, buffer_size) == 0)
        return TRUE;

    g_warning("VividProducer: gbm_bo_write failed for output=%u: %s",
              output->output_id,
              errno != 0 ? g_strerror(errno) : "driver rejected write transfer");
    set_buffer_error(error,
                     "gbm_bo_map and gbm_bo_write failed for output=%u: %s",
                     output->output_id,
                     errno != 0 ? g_strerror(errno) : "driver rejected write transfer");
    return FALSE;
}

static void
output_close_plane_fds(Output* output)
{
    for (guint buffer = 0; buffer < G_N_ELEMENTS(output->buffers); buffer++) {
        for (guint plane = 0; plane < VIVID_DISPLAY_DMABUF_MAX_PLANES; plane++) {
            if (output->buffers[buffer].plane_fds[plane] >= 0) {
                close(output->buffers[buffer].plane_fds[plane]);
                output->buffers[buffer].plane_fds[plane] = -1;
            }
        }
    }
}

static gint
drm_call_error(gint result)
{
    if (result == 0)
        return 0;
    if (result < 0) {
        if (errno != 0)
            return errno;
        return -result;
    }
    return result;
}

static gint64
drm_wait_deadline_nsec(guint timeout_msec)
{
    return ((gint64)g_get_monotonic_time() * 1000) +
        ((gint64)timeout_msec * G_TIME_SPAN_MILLISECOND * 1000);
}

static gboolean
producer_create_signaled_sync_file(Producer* producer,
                                   gint*     out_fd)
{
    g_return_val_if_fail(out_fd != NULL, FALSE);
    *out_fd = -1;

    if (!producer || producer->render_fd < 0) {
        g_warning("VividProducer: cannot create acquire sync_file without an open DRM render fd");
        return FALSE;
    }

    guint32 handle = 0;
    errno = 0;
    gint result = drmSyncobjCreate(producer->render_fd,
                                   DRM_SYNCOBJ_CREATE_SIGNALED,
                                   &handle);
    if (result != 0) {
        g_warning("VividProducer: drmSyncobjCreate(SIGNALED) for acquire fence failed: %s",
                  g_strerror(drm_call_error(result)));
        return FALSE;
    }

    errno = 0;
    result = drmSyncobjExportSyncFile(producer->render_fd, handle, out_fd);
    const gint export_error = drm_call_error(result);
    errno = 0;
    const gint destroy_result = drmSyncobjDestroy(producer->render_fd, handle);
    if (destroy_result != 0) {
        g_warning("VividProducer: drmSyncobjDestroy(acquire handle=%u) failed: %s",
                  handle,
                  g_strerror(drm_call_error(destroy_result)));
    }

    if (result != 0 || *out_fd < 0) {
        g_warning("VividProducer: drmSyncobjExportSyncFile(acquire handle=%u) failed: %s",
                  handle,
                  g_strerror(export_error));
        if (*out_fd >= 0) {
            close(*out_fd);
            *out_fd = -1;
        }
        return FALSE;
    }

    return TRUE;
}

static gboolean
producer_create_release_syncobj(Producer* producer,
                                guint32*  out_handle,
                                gint*     out_fd)
{
    g_return_val_if_fail(out_handle != NULL, FALSE);
    g_return_val_if_fail(out_fd != NULL, FALSE);
    *out_handle = 0;
    *out_fd = -1;

    if (!producer || producer->render_fd < 0) {
        g_warning("VividProducer: cannot create release syncobj without an open DRM render fd");
        return FALSE;
    }

    guint32 handle = 0;
    errno = 0;
    gint result = drmSyncobjCreate(producer->render_fd, 0, &handle);
    if (result != 0) {
        g_warning("VividProducer: drmSyncobjCreate(release) failed: %s",
                  g_strerror(drm_call_error(result)));
        return FALSE;
    }

    errno = 0;
    result = drmSyncobjHandleToFD(producer->render_fd, handle, out_fd);
    if (result != 0 || *out_fd < 0) {
        const gint error = drm_call_error(result);
        g_warning("VividProducer: drmSyncobjHandleToFD(release handle=%u) failed: %s",
                  handle,
                  g_strerror(error));
        errno = 0;
        (void)drmSyncobjDestroy(producer->render_fd, handle);
        if (*out_fd >= 0) {
            close(*out_fd);
            *out_fd = -1;
        }
        return FALSE;
    }

    *out_handle = handle;
    return TRUE;
}

static gboolean
producer_release_coordinator_is_stopping(Producer* producer)
{
    gboolean stopping = TRUE;
    if (!producer)
        return TRUE;

    g_mutex_lock(&producer->release_lock);
    stopping = producer->release_thread_stopping;
    g_mutex_unlock(&producer->release_lock);
    return stopping;
}

static void
producer_release_destroy_syncobj(Producer*    producer,
                                 guint32      handle,
                                 const gchar* label)
{
    if (!producer || producer->render_fd < 0 || handle == 0)
        return;

    errno = 0;
    const gint result = drmSyncobjDestroy(producer->render_fd, handle);
    if (result != 0) {
        g_warning("VividProducer: drmSyncobjDestroy(%s handle=%u) failed: %s",
                  label ? label : "syncobj",
                  handle,
                  g_strerror(drm_call_error(result)));
    }
}

static gboolean
producer_release_signal_syncobj(Producer*    producer,
                                guint32      handle,
                                const gchar* reason)
{
    if (!producer || producer->render_fd < 0 || handle == 0)
        return FALSE;

    errno = 0;
    const gint result = drmSyncobjSignal(producer->render_fd, &handle, 1);
    if (result != 0) {
        g_warning("VividProducer: drmSyncobjSignal(release handle=%u reason=%s) "
                  "failed: %s",
                  handle,
                  reason ? reason : "reaper",
                  g_strerror(drm_call_error(result)));
        return FALSE;
    }
    return TRUE;
}

static gboolean
producer_release_transfer_to_timeline(Producer*    producer,
                                      guint32      binary_handle,
                                      guint64      release_point,
                                      const gchar* reason)
{
    if (!producer ||
        producer->render_fd < 0 ||
        producer->release_timeline_handle == 0 ||
        binary_handle == 0 ||
        release_point == 0)
        return FALSE;

    errno = 0;
    const gint result =
        drmSyncobjTransfer(producer->render_fd,
                           producer->release_timeline_handle,
                           release_point,
                           binary_handle,
                           0,
                           0);
    if (result != 0) {
        g_warning("VividProducer: drmSyncobjTransfer(binary handle=%u -> "
                  "release timeline point=%" G_GUINT64_FORMAT " reason=%s) "
                  "failed: %s",
                  binary_handle,
                  release_point,
                  reason ? reason : "reaper",
                  g_strerror(drm_call_error(result)));
        return FALSE;
    }
    return TRUE;
}

static ReleaseGroup*
release_group_new(Producer*    producer,
                  RenderRoute* route,
                  guint32      buffer_index,
                  guint64      sequence,
                  guint        expected,
                  guint64      aggregate_point)
{
    ReleaseGroup* group = g_new0(ReleaseGroup, 1);
    group->producer = producer;
    group->route_id = route ? route->route_id : 0;
    group->buffer_index = buffer_index;
    group->sequence = sequence;
    group->aggregate_point = aggregate_point;
    group->expected = MAX(expected, 1u);
    group->ref_count = 1;
    g_mutex_init(&group->lock);
    return group;
}

static ReleaseGroup*
release_group_ref(ReleaseGroup* group)
{
    if (!group)
        return NULL;
    g_mutex_lock(&group->lock);
    group->ref_count++;
    g_mutex_unlock(&group->lock);
    return group;
}

static void
release_group_unref(ReleaseGroup* group)
{
    if (!group)
        return;

    gboolean free_group = FALSE;
    g_mutex_lock(&group->lock);
    group->ref_count--;
    free_group = group->ref_count == 0;
    g_mutex_unlock(&group->lock);

    if (free_group) {
        g_mutex_clear(&group->lock);
        g_free(group);
    }
}

static void
release_group_complete(ReleaseGroup* group, const gchar* reason)
{
    if (!group || !group->producer)
        return;

    gboolean should_signal = FALSE;
    guint completed = 0;
    g_mutex_lock(&group->lock);
    if (group->completed < group->expected)
        group->completed++;
    completed = group->completed;
    if (!group->signaled && group->completed >= group->expected) {
        group->signaled = TRUE;
        should_signal = TRUE;
    }
    g_mutex_unlock(&group->lock);

    if (!should_signal)
        return;

    Producer* producer = group->producer;
    guint32 handle = producer->release_timeline_handle;
    guint64 point = group->aggregate_point;
    errno = 0;
    const gint result =
        drmSyncobjTimelineSignal(producer->render_fd, &handle, &point, 1);
    if (result != 0) {
        g_warning("VividProducer: release group signal route=%u buffer=%u "
                  "sequence=%" G_GUINT64_FORMAT " aggregate_point=%"
                  G_GUINT64_FORMAT " completed=%u/%u reason=%s failed: %s",
                  group->route_id,
                  group->buffer_index,
                  group->sequence,
                  group->aggregate_point,
                  completed,
                  group->expected,
                  reason ? reason : "consumer-release",
                  g_strerror(drm_call_error(result)));
        return;
    }
}

static void
producer_release_reaper_process(Producer* producer, ReleaseReaperRecord* record)
{
    if (!producer || !record || record->binary_handle == 0)
        return;

    guint32 handle = record->binary_handle;
    const gboolean stopping = producer_release_coordinator_is_stopping(producer);

    if (!stopping) {
        const gint64 wait_start_usec = g_get_monotonic_time();
        const gint64 queued_age_msec = record->created_usec > 0
            ? (wait_start_usec - record->created_usec) / 1000
            : -1;
        guint32 first_signaled = 0;
        errno = 0;
        const gint wait_result =
            drmSyncobjWait(producer->render_fd,
                           &handle,
                           1,
                           drm_wait_deadline_nsec(RELEASE_REAPER_TIMEOUT_MSEC),
                           DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                               DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                           &first_signaled);
        if (wait_result != 0) {
            const gint error = drm_call_error(wait_result);
            const gint64 now_usec = g_get_monotonic_time();
            g_warning("VividProducer: release reaper wait output=%u generation=%"
                      G_GUINT64_FORMAT " buffer=%u sequence=%" G_GUINT64_FORMAT
                      " release_point=%" G_GUINT64_FORMAT " handle=%u timed "
                      "out/error after %ums queued-age=%" G_GINT64_FORMAT
                      "ms total-age=%" G_GINT64_FORMAT "ms: %s; "
                      "force-signaling to avoid producer-side release deadlock",
                      record->output_id,
                      record->generation,
                      record->buffer_index,
                      record->sequence,
                      record->release_point,
                      handle,
                      RELEASE_REAPER_TIMEOUT_MSEC,
                      queued_age_msec,
                      record->created_usec > 0
                          ? (now_usec - record->created_usec) / 1000
                          : -1,
                      g_strerror(error));
            producer_release_signal_syncobj(producer, handle, "reaper-timeout");
        }
    } else {
        producer_release_signal_syncobj(producer, handle, "reaper-stop");
    }

    if (record->release_group) {
        release_group_complete(record->release_group,
                               stopping ? "reaper-stop" : "consumer-release");
    } else {
        producer_release_transfer_to_timeline(producer,
                                              handle,
                                              record->release_point,
                                              stopping ? "reaper-stop" : "consumer-release");
    }
    producer_release_destroy_syncobj(producer, handle, "release-binary");
    record->binary_handle = 0;
}

static gpointer
producer_release_reaper_thread(gpointer user_data)
{
    Producer* producer = user_data;

    for (;;) {
        ReleaseReaperRecord* record =
            g_async_queue_pop(producer->release_queue);
        if (!record)
            continue;

        if (record->shutdown) {
            g_free(record);
            break;
        }

        producer_release_reaper_process(producer, record);
        release_group_unref(record->release_group);
        g_free(record);
    }

    return NULL;
}

static gboolean
producer_release_coordinator_start(Producer* producer)
{
    if (!producer || producer->render_fd < 0) {
        g_warning("VividProducer: cannot start release coordinator without an open DRM render fd");
        return FALSE;
    }

    if (producer->release_timeline_handle != 0)
        return TRUE;

    guint32 handle = 0;
    errno = 0;
    const gint result = drmSyncobjCreate(producer->render_fd, 0, &handle);
    if (result != 0) {
        g_warning("VividProducer: drmSyncobjCreate(release timeline) failed: %s",
                  g_strerror(drm_call_error(result)));
        return FALSE;
    }

    producer->release_timeline_handle = handle;
    producer->next_release_point = 0;
    producer->release_points_renderer_generation = 0;
    memset(producer->buffer_release_points,
           0,
           sizeof(producer->buffer_release_points));

    g_mutex_lock(&producer->release_lock);
    producer->release_thread_stopping = FALSE;
    g_mutex_unlock(&producer->release_lock);

    producer->release_queue = g_async_queue_new();
    producer->release_thread =
        g_thread_new("vivid-release-reaper",
                     producer_release_reaper_thread,
                     producer);
    g_message("VividProducer: release coordinator started timeline handle=%u "
              "reaper-timeout=%ums gate-timeout=%ums",
              producer->release_timeline_handle,
              RELEASE_REAPER_TIMEOUT_MSEC,
              RELEASE_GATE_WAIT_TIMEOUT_MSEC);
    return TRUE;
}

static void
producer_release_coordinator_stop(Producer* producer)
{
    if (!producer)
        return;

    g_mutex_lock(&producer->release_lock);
    producer->release_thread_stopping = TRUE;
    g_mutex_unlock(&producer->release_lock);

    if (producer->release_queue && producer->release_thread) {
        ReleaseReaperRecord* shutdown = g_new0(ReleaseReaperRecord, 1);
        shutdown->shutdown = TRUE;
        g_async_queue_push(producer->release_queue, shutdown);
        g_thread_join(producer->release_thread);
        producer->release_thread = NULL;
    }

    if (producer->release_queue) {
        ReleaseReaperRecord* record = NULL;
        while ((record = g_async_queue_try_pop(producer->release_queue)) != NULL) {
            if (record->binary_handle != 0)
                producer_release_destroy_syncobj(producer,
                                                 record->binary_handle,
                                                 "release-binary-drain");
            release_group_unref(record->release_group);
            g_free(record);
        }
        g_async_queue_unref(producer->release_queue);
        producer->release_queue = NULL;
    }

    if (producer->release_timeline_handle != 0) {
        producer_release_destroy_syncobj(producer,
                                         producer->release_timeline_handle,
                                         "release-timeline");
        producer->release_timeline_handle = 0;
    }

    producer->next_release_point = 0;
    producer->release_points_renderer_generation = 0;
    memset(producer->buffer_release_points,
           0,
           sizeof(producer->buffer_release_points));
}

static void
render_route_release_reset_points(RenderRoute* route,
                                  guint64      renderer_generation)
{
    if (!route || !route->release_gate_context)
        return;

    RenderRouteReleaseGateContext* context = route->release_gate_context;
    g_mutex_lock(&context->lock);
    if (context->release_points_renderer_generation != renderer_generation) {
        context->release_points_renderer_generation = renderer_generation;
        memset(context->buffer_release_points,
               0,
               sizeof(context->buffer_release_points));
        g_message("VividProducer: reset release gate points route=%u renderer generation=%"
                  G_GUINT64_FORMAT,
                  route->route_id,
                  renderer_generation);
    }
    g_mutex_unlock(&context->lock);
}

static guint64
producer_release_coordinator_allocate_point(Producer* producer)
{
    if (!producer)
        return 0;

    g_mutex_lock(&producer->release_lock);
    producer->next_release_point++;
    const guint64 point = producer->next_release_point;
    g_mutex_unlock(&producer->release_lock);
    return point;
}

static void
render_route_release_note_buffer_point(RenderRoute* route,
                                       guint32      buffer_index,
                                       guint64      release_point)
{
    if (!route ||
        !route->release_gate_context ||
        buffer_index >= G_N_ELEMENTS(route->release_gate_context->buffer_release_points) ||
        release_point == 0)
        return;

    RenderRouteReleaseGateContext* context = route->release_gate_context;
    g_mutex_lock(&context->lock);
    if (context->active && context->buffer_release_points[buffer_index] < release_point)
        context->buffer_release_points[buffer_index] = release_point;
    g_mutex_unlock(&context->lock);
}

static gboolean
producer_release_coordinator_enqueue(Producer*             producer,
                                     ReleaseReaperRecord*  record)
{
    if (!producer || !record)
        return FALSE;

    if (!producer->release_queue ||
        producer->release_timeline_handle == 0 ||
        producer_release_coordinator_is_stopping(producer)) {
        return FALSE;
    }

    g_async_queue_push(producer->release_queue, record);
    return TRUE;
}

static gboolean
producer_release_timeline_wait_point(Producer* producer,
                                     guint32   buffer_index,
                                     guint64   release_point,
                                     guint32   timeout_ms)
{
    if (!producer || release_point == 0)
        return TRUE;

    if (producer->release_timeline_handle == 0 || producer->render_fd < 0) {
        g_warning("VividProducer: release gate cannot wait buffer=%u point=%"
                  G_GUINT64_FORMAT " without a release timeline",
                  buffer_index,
                  release_point);
        return FALSE;
    }

    guint32 handle = producer->release_timeline_handle;
    guint64 point = release_point;
    guint32 first_signaled = 0;
    errno = 0;
    const gint wait_result =
        drmSyncobjTimelineWait(producer->render_fd,
                               &handle,
                               &point,
                               1,
                               drm_wait_deadline_nsec(timeout_ms),
                               DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                               &first_signaled);
    if (wait_result == 0)
        return TRUE;

    const gint error = drm_call_error(wait_result);
    g_warning("VividProducer: release gate wait buffer=%u release_point=%"
              G_GUINT64_FORMAT " timed out/error after %ums: %s",
              buffer_index,
              release_point,
              timeout_ms,
              g_strerror(error));
    return FALSE;
}

static gboolean
render_route_release_gate_wait(gpointer user_data,
                               guint32  buffer_index,
                               guint32  timeout_ms)
{
    RenderRouteReleaseGateContext* context = user_data;
    if (!context)
        return TRUE;

    if (buffer_index >= G_N_ELEMENTS(context->buffer_release_points)) {
        g_warning("VividProducer: release gate route=%u received out-of-range buffer=%u",
                  context->route_id,
                  buffer_index);
        return FALSE;
    }

    g_mutex_lock(&context->lock);
    if (!context->active) {
        g_mutex_unlock(&context->lock);
        return TRUE;
    }
    Producer* producer = context->producer;
    const guint64 point = context->buffer_release_points[buffer_index];
    g_mutex_unlock(&context->lock);

    const gboolean waited = producer_release_timeline_wait_point(producer,
                                                                  buffer_index,
                                                                  point,
                                                                  timeout_ms);

    return waited;
}

static void
output_init_plane_fds(Output* output)
{
    for (guint buffer = 0; buffer < G_N_ELEMENTS(output->buffers); buffer++) {
        for (guint plane = 0; plane < VIVID_DISPLAY_DMABUF_MAX_PLANES; plane++)
            output->buffers[buffer].plane_fds[plane] = -1;
    }
}

static void
output_release_buffers(Output* output)
{
    output_close_plane_fds(output);
    if (output->bo) {
        gbm_bo_destroy(output->bo);
        output->bo = NULL;
    }
    output->n_buffers = 0;
    output->stride = 0;
    output->modifier = 0;
    output->fourcc = 0;
    output->premultiplied = FALSE;
    output->dmabuf_path = OUTPUT_DMABUF_PATH_UNNEGOTIATED;
    output->memory_preference = VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT;
    output->consumer_same_render_node = FALSE;
    memset(output->buffers, 0, sizeof(output->buffers));
    output_init_plane_fds(output);
}

static gboolean
output_blacklist_producer_modifier(Output* output,
                                   guint32 fourcc,
                                   guint64 modifier,
                                   guint32 plane_count)
{
    if (!output)
        return FALSE;

    if (modifier == DRM_FORMAT_MOD_INVALID)
        modifier = DRM_FORMAT_MOD_LINEAR;
    for (guint32 i = 0; i < output->n_producer_blacklist; i++) {
        const VividDmaBufModifierCap* entry = &output->producer_blacklist[i];
        if (entry->fourcc == fourcc && entry->modifier == modifier)
            return FALSE;
    }

    if (output->n_producer_blacklist >= G_N_ELEMENTS(output->producer_blacklist))
        return FALSE;

    output->producer_blacklist[output->n_producer_blacklist++] =
        (VividDmaBufModifierCap) {
            .fourcc = fourcc,
            .modifier = modifier,
            .plane_count = plane_count,
        };
    return TRUE;
}

static VividProducerRendererDmaBufPrepareStatus
output_prepare_renderer_buffers(Producer* producer, Output* output, GError** error)
{
    VividProducerRendererBufferSet set = {0};
    VividProducerRendererDmaBufRequest request = {0};
    VividGpuDevice resolved_gpu;
    if (!vivid_producer_renderer_resolved_gpu(output_renderer(output), &resolved_gpu)) {
        /*
         * Renderer-owned DMA-BUFs are only valid when the producer can also
         * publish the exact GPU identity through BIND_BUFFERS. The renderer module
         * must not become an independent resolver that renders successfully while
         * the protocol state still says "unknown"; that would break the
         * end-to-end same-render-node invariant the consumer relies on.
         */
        g_warning("VividProducer: refusing renderer DMA-BUF output because "
                  "render-device='%s' has no resolved GPU render-node=(unresolved) "
                  "vendor=unknown device=(unknown)",
                  producer->config.render_device ? producer->config.render_device : "auto");
        set_buffer_error(error,
                         "render-device '%s' has no resolved GPU render node for renderer DMA-BUF output",
                         producer->config.render_device ? producer->config.render_device : "auto");
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    if (!vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(output))) {
        set_buffer_error(error, "current renderer does not export DMA-BUF buffers");
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    for (guint retry = 0; retry <= VIVID_DMABUF_NEGOTIATION_MAX_BLACKLIST; retry++) {
        if (!output_negotiate_linear_dmabuf(output, producer, FALSE, 0, TRUE, &request, error))
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;

        VividProducerRendererDmaBufPrepareStatus prepare_status =
            vivid_producer_renderer_prepare_dmabuf_buffers_ex(output_renderer(output),
                                                               output->width,
                                                               output->height,
                                                               output_render_scale(output),
                                                               &request,
                                                               &set);
        if (prepare_status == VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK)
            break;

        vivid_producer_renderer_buffer_set_clear(&set);
        if (prepare_status == VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY) {
            set_buffer_error(error,
                             "renderer-owned DMA-BUF preparation failed for output=%u "
                             "size=%ux%u status=%u path=%s modifier=0x%016"
                             G_GINT64_MODIFIER "x",
                             output->output_id,
                             output->width,
                             output->height,
                             prepare_status,
                             output_dmabuf_path_name(output->dmabuf_path),
                             (guint64)request.required_modifier);
            return prepare_status;
        }
        if (prepare_status != VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED) {
            set_buffer_error(error,
                             "renderer-owned DMA-BUF preparation failed for output=%u "
                             "size=%ux%u status=%u path=%s modifier=0x%016"
                             G_GINT64_MODIFIER "x",
                             output->output_id,
                             output->width,
                             output->height,
                             prepare_status,
                             output_dmabuf_path_name(output->dmabuf_path),
                             (guint64)request.required_modifier);
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        }

        const gboolean inserted =
            request.n_allowed_fourccs > 0 &&
            output_blacklist_producer_modifier(output,
                                               request.allowed_fourccs[0],
                                               request.required_modifier,
                                               request.required_plane_count);
        g_message("VividProducer: renderer BIND_FAILED output=%u path=%s "
                  "fourcc=0x%08x modifier=0x%016" G_GINT64_MODIFIER
                  "x planes=%u inserted-blacklist=%s retry=%u",
                  output->output_id,
                  output_dmabuf_path_name(output->dmabuf_path),
                  request.n_allowed_fourccs > 0 ? request.allowed_fourccs[0] : 0,
                  (guint64)request.required_modifier,
                  request.required_plane_count,
                  inserted ? "true" : "false",
                  retry);
        if (!inserted)
            break;
    }

    if (set.n_buffers == 0) {
        set_buffer_error(error,
                         "renderer-owned DMA-BUF preparation has no usable scheme for output=%u "
                         "after %u producer-side blacklist entries",
                         output->output_id,
                         output->n_producer_blacklist);
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    render_route_release_reset_points(
        output->route,
        vivid_producer_renderer_generation(output_renderer(output)));

    output->memory_kind = OUTPUT_MEMORY_RENDERER_DMABUF;
    output->fourcc = set.fourcc;
    output->modifier = set.modifier;
    output->premultiplied = set.premultiplied;
    output->n_buffers = MIN(set.n_buffers, (guint32)G_N_ELEMENTS(output->buffers));

    for (guint buffer = 0; buffer < output->n_buffers; buffer++) {
        OutputBuffer* dst = &output->buffers[buffer];
        const VividProducerRendererBuffer* src = &set.buffers[buffer];
        dst->index = src->index;
        dst->size = src->size;
        dst->n_planes = MIN(src->n_planes, (guint32)VIVID_DISPLAY_DMABUF_MAX_PLANES);
        for (guint plane = 0; plane < dst->n_planes; plane++) {
            dst->plane_fds[plane] = src->planes[plane].fd;
            dst->plane_strides[plane] = src->planes[plane].stride;
            dst->plane_offsets[plane] = src->planes[plane].offset;
            set.buffers[buffer].planes[plane].fd = -1;
        }
    }

    vivid_producer_renderer_buffer_set_clear(&set);
    if (output->n_buffers == 0) {
        set_buffer_error(error,
                         "renderer-owned DMA-BUF preparation returned zero buffers for output=%u",
                         output->output_id);
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK;
}

static gboolean
output_prepare_gbm_buffer(Producer* producer, Output* output, GError** error)
{
    VividGpuDevice resolved_gpu;
    if (!vivid_producer_renderer_resolved_gpu(output_renderer(output), &resolved_gpu)) {
        g_warning("VividProducer: refusing GBM diagnostic output because "
                  "render-device='%s' did not resolve to a Vulkan render node",
                  producer->config.render_device ? producer->config.render_device : "auto");
        set_buffer_error(error,
                         "render-device '%s' did not resolve to a Vulkan render node",
                         producer->config.render_device ? producer->config.render_device : "auto");
        return FALSE;
    }

    /*
     * The GBM path is only a local diagnostic renderer. It is allowed to export
     * DMA-BUFs when its allocator already belongs to the same DRM render node
     * as the resolved producer GPU, but it must not become a silent cross-GPU
     * fallback after the user selects a different card.
     */
    if (g_strcmp0(producer->render_node_path, resolved_gpu.render_node) != 0) {
        g_warning("VividProducer: refusing GBM diagnostic output on %s because "
                  "render-device='%s' resolved to %s (%s)",
                  producer->render_node_path ? producer->render_node_path : "(none)",
                  producer->config.render_device ? producer->config.render_device : "auto",
                  resolved_gpu.render_node,
                  resolved_gpu.name);
        set_buffer_error(error,
                         "GBM diagnostic allocator is on %s but render-device '%s' "
                         "resolved to %s (%s)",
                         producer->render_node_path ? producer->render_node_path : "(none)",
                         producer->config.render_device ? producer->config.render_device : "auto",
                         resolved_gpu.render_node,
                         resolved_gpu.name);
        return FALSE;
    }

    if (!producer->gbm_device) {
        g_warning("VividProducer: cannot create output without a GBM DMA-BUF allocator");
        set_buffer_error(error,
                         "cannot create GBM fallback output because no GBM DMA-BUF allocator is available");
        return FALSE;
    }

    if (!output_negotiate_linear_dmabuf(output,
                                        producer,
                                        TRUE,
                                        DRM_FORMAT_XRGB8888,
                                        FALSE,
                                        NULL,
                                        error)) {
        return FALSE;
    }

    output->bo = create_output_bo(producer, output->width, output->height, error);
    if (!output->bo)
        return FALSE;

    output->stride = gbm_bo_get_stride(output->bo);
    if (output->stride < output->width * 4u) {
        g_warning("VividProducer: GBM BO stride=%u is too small for output=%u size=%ux%u",
                  output->stride,
                  output->output_id,
                  output->width,
                  output->height);
        set_buffer_error(error,
                         "GBM BO stride=%u is too small for output=%u size=%ux%u",
                         output->stride,
                         output->output_id,
                         output->width,
                         output->height);
        return FALSE;
    }

    OutputBuffer* buffer = &output->buffers[0];
    buffer->index = 0;
    buffer->size = (gsize)output->stride * output->height;
    output->fourcc = DRM_FORMAT_XRGB8888;
    output->modifier = gbm_bo_get_modifier(output->bo);
    output->premultiplied = FALSE;
    output->memory_kind = OUTPUT_MEMORY_GBM;

    const gint plane_count = gbm_bo_get_plane_count(output->bo);
    buffer->n_planes = plane_count > 0 ? (guint32)plane_count : 1u;
    if (buffer->n_planes == 0 || buffer->n_planes > VIVID_DISPLAY_DMABUF_MAX_PLANES) {
        g_warning("VividProducer: unsupported GBM plane count=%u for output=%u",
                  buffer->n_planes,
                  output->output_id);
        set_buffer_error(error,
                         "unsupported GBM plane count=%u for output=%u",
                         buffer->n_planes,
                         output->output_id);
        return FALSE;
    }

    for (guint i = 0; i < buffer->n_planes; i++) {
        buffer->plane_fds[i] = buffer->n_planes == 1
            ? gbm_bo_get_fd(output->bo)
            : gbm_bo_get_fd_for_plane(output->bo, (int)i);
        if (buffer->plane_fds[i] < 0) {
            g_warning("VividProducer: failed to export DMA-BUF fd for output=%u plane=%u: %s",
                      output->output_id,
                      i,
                      g_strerror(errno));
            set_buffer_error(error,
                             "failed to export DMA-BUF fd for output=%u plane=%u: %s",
                             output->output_id,
                             i,
                             g_strerror(errno));
            return FALSE;
        }

        buffer->plane_strides[i] = buffer->n_planes == 1
            ? gbm_bo_get_stride(output->bo)
            : gbm_bo_get_stride_for_plane(output->bo, (int)i);
        buffer->plane_offsets[i] = buffer->n_planes == 1
            ? 0u
            : gbm_bo_get_offset(output->bo, (int)i);
    }

    output->n_buffers = 1;
    return fill_output_buffer(producer, output, error);
}

static gboolean
output_validate_consumer_caps(Output* output, GError** error)
{
    const ConsumerDmaBufCaps* caps = output && output->client
        ? &output->client->dmabuf_caps
        : NULL;
    if (!caps || !caps->present)
        return TRUE;

    if (!vivid_dmabuf_peer_caps_accepts_extent(&caps->peer_caps,
                                               output->width,
                                               output->height)) {
        /*
         * extentMax is a hard import limit from the consumer. Rejecting before
         * bind avoids exporting a buffer pool the display backend already told
         * us it cannot create textures/images for, which is especially
         * important for cross-GPU CompatLinear paths where retries are costly.
         */
        set_buffer_error(error,
                         "consumer backend=%s extentMax=%ux%u cannot import output buffer=%ux%u",
                         caps->backend && *caps->backend ? caps->backend : "(unknown)",
                         caps->peer_caps.extent_max_w,
                         caps->peer_caps.extent_max_h,
                         output->width,
                         output->height);
        return FALSE;
    }

    if (!consumer_dmabuf_caps_supports_fourcc(caps, output->fourcc)) {
        set_buffer_error(error,
                         "consumer backend=%s does not advertise fourcc=0x%08x",
                         caps->backend && *caps->backend ? caps->backend : "(unknown)",
                         output->fourcc);
        return FALSE;
    }

    if (output->dmabuf_path == OUTPUT_DMABUF_PATH_COMPAT_LINEAR &&
        !output->consumer_same_render_node) {
        /*
         * Waywallen's cross-device CompatLinear path is a topology decision,
         * not a normal modifier intersection. The picker only requires a shared
         * fourcc and tells the allocation side to produce LINEAR/GPU_LINEAR
         * memory. Keep the same boundary here: verify our exported pool is
         * actually linear, then let the consumer relay import it instead of
         * rejecting early because LINEAR was not explicitly listed in the
         * modifier caps. Same-device LINEAR fallback still goes through the
         * strict modifier check below because it came from a real intersection.
         */
        if (!dmabuf_modifier_is_implicit_linear(output->modifier)) {
            set_buffer_error(error,
                             "compat-linear output=%u produced non-linear modifier=0x%016"
                             G_GINT64_MODIFIER "x",
                             output->output_id,
                             (guint64)output->modifier);
            return FALSE;
        }
        return TRUE;
    }

    if (!consumer_dmabuf_caps_supports_modifier(caps, output->fourcc, output->modifier)) {
        set_buffer_error(error,
                         "consumer backend=%s does not advertise fourcc=0x%08x modifier=0x%016"
                         G_GINT64_MODIFIER "x",
                         caps->backend && *caps->backend ? caps->backend : "(unknown)",
                         output->fourcc,
                         (guint64)output->modifier);
        return FALSE;
    }

    return TRUE;
}

static gboolean
output_rebuild_buffers(Producer* producer,
                       Output*   output,
                       gboolean  advance_generation,
                       OutputRebindPrepareState* prepare_state,
                       GError**  error)
{
    if (prepare_state)
        *prepare_state = OUTPUT_REBIND_PREPARE_FAILED;

    output_release_buffers(output);

    /*
     * Real renderers own their DMA-BUF buffer set and only send buffer indices
     * through FRAME_READY. The producer-owned GBM path is strictly diagnostic;
     * scene/video/web must not silently fall back to CPU-written GBM, because
     * that hides the renderer export failure and reintroduces the slow path.
     */
    gboolean ok = FALSE;
    if (vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(output))) {
        GError* renderer_error = NULL;
        const VividProducerRendererDmaBufPrepareStatus renderer_status =
            output_prepare_renderer_buffers(producer, output, &renderer_error);
        ok = renderer_status == VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK;
        if (prepare_state &&
            renderer_status == VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY)
            *prepare_state = OUTPUT_REBIND_PREPARE_NOT_READY;
        if (!ok) {
            g_warning("VividProducer: renderer DMA-BUF preparation failed for output=%u: %s",
                      output->output_id,
                      renderer_error ? renderer_error->message : "unknown renderer error");
            set_buffer_error(error,
                             "renderer DMA-BUF failed (%s)",
                             renderer_error ? renderer_error->message : "unknown renderer error");
            g_clear_error(&renderer_error);
        }
    } else {
        ok = output_prepare_gbm_buffer(producer, output, error);
    }

    if (!ok) {
        output_release_buffers(output);
        return FALSE;
    }
    if (!output_validate_consumer_caps(output, error)) {
        output_release_buffers(output);
        return FALSE;
    }

    if (advance_generation)
        output->generation++;
    if (prepare_state)
        *prepare_state = OUTPUT_REBIND_PREPARE_READY;
    return TRUE;
}

static Output*
output_new(Client*   client,
           guint32   consumer_output_id,
           guint32   monitor_index,
           gint32    display_x,
           gint32    display_y,
           guint32   logical_width,
           guint32   logical_height,
           guint32   width,
           guint32   height,
           gdouble   scale,
           guint32   refresh_rate_mhz,
           const gchar* desktop,
           const gchar* display_key,
           const gchar* display_name)
{
    Producer* producer = client->producer;
    Output* output = g_new0(Output, 1);
    output_init_plane_fds(output);

    output->client = client;
    output->consumer_output_id = consumer_output_id;
    output->monitor_index = monitor_index;
    output->output_id = producer->next_output_id++;
    output->display_x = display_x;
    output->display_y = display_y;
    output->logical_width = CLAMP(logical_width, 1u, OUTPUT_MAX_DIMENSION);
    output->logical_height = CLAMP(logical_height, 1u, OUTPUT_MAX_DIMENSION);
    output->target_width = CLAMP(width, 1u, OUTPUT_MAX_DIMENSION);
    output->target_height = CLAMP(height, 1u, OUTPUT_MAX_DIMENSION);
    output->width = output->target_width;
    output->height = output->target_height;
    output->scale = MAX(1.0, scale);
    output->refresh_rate_mhz = refresh_rate_mhz;
    output->desktop = g_strdup(desktop ? desktop : "");
    output->display_key = g_strdup(display_key ? display_key : "");
    output->display_name = g_strdup(display_name ? display_name : "");
    output->source_rect = (VividRenderRect) {
        .x = 0.0,
        .y = 0.0,
        .width = output->width,
        .height = output->height,
    };
    output->dest_rect = (VividRenderRect) {
        .x = 0.0,
        .y = 0.0,
        .width = output->target_width,
        .height = output->target_height,
    };
    output->generation = 1;
    output->sequence = 0;
    vivid_auto_replay_state_init(&output->auto_replay);
    return output;
}

static void
output_free(Output* output)
{
    if (!output)
        return;
    vivid_auto_replay_state_clear(&output->auto_replay);
    output_release_buffers(output);
    g_free(output->desktop);
    g_free(output->display_key);
    g_free(output->display_name);
    g_free(output);
}

static Output*
producer_first_output(Producer* producer)
{
    return producer_first_output_by_id(producer);
}

static Output*
producer_find_consumer_output(Producer* producer, guint32 consumer_output_id)
{
    if (!producer || consumer_output_id == 0)
        return NULL;

    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (output->consumer_output_id == consumer_output_id)
                return output;
        }
    }

    return NULL;
}

static Output*
producer_find_output_by_id(Producer* producer, guint32 output_id)
{
    if (!producer || output_id == 0)
        return NULL;

    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (output && output->output_id == output_id)
                return output;
        }
    }

    return NULL;
}

static void
producer_set_renderer_rebind_barrier(Producer* producer, const Output* output)
{
    if (!producer || !output)
        return;

    producer->renderer_rebind_barrier_output_id = output->output_id;
}

static void
producer_clear_renderer_rebind_barrier(Producer* producer, const Output* output)
{
    if (!producer)
        return;
    if (!output || producer->renderer_rebind_barrier_output_id == output->output_id)
        producer->renderer_rebind_barrier_output_id = 0;
}

static gboolean
producer_renderer_rebind_barrier_blocks(Producer* producer, const Output* output)
{
    if (!producer || producer->renderer_rebind_barrier_output_id == 0)
        return FALSE;

    Output* barrier =
        producer_find_output_by_id(producer, producer->renderer_rebind_barrier_output_id);
    if (!barrier || !barrier->needs_renderer_rebind) {
        producer->renderer_rebind_barrier_output_id = 0;
        return FALSE;
    }
    if (barrier == output)
        return FALSE;

    /*
     * Vulkan ICD scanning is process-wide but wallpaper-scene-renderer starts
     * its Vulkan device on a private looper thread. When one output has already
     * asked a renderer to initialize and reported NOT_READY, do not let later
     * outputs probe their own Vulkan caps in parallel. The barrier makes the
     * frame tick retry the waiting output first; once it binds or fails with a
     * non-transient error, other outputs can continue their normal rebind path.
     */
    return TRUE;
}

static guint
producer_registered_output_count(Producer* producer)
{
    guint count = 0;
    for (guint client_index = 0; producer && client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        count += client ? client->outputs->len : 0;
    }
    return count;
}

static guint
producer_declared_output_count(Producer* producer)
{
    guint count = 0;
    for (guint client_index = 0; producer && client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        if (client && client->declared_output_count > 0)
            count += client->declared_output_count;
    }
    return count;
}

static gboolean
producer_clone_route_waiting_for_primary(Producer* producer)
{
    if (!producer || !producer_uses_clone_routes(producer))
        return FALSE;

    const gchar* primary_key = producer->config.primary_display_key;
    if (!primary_key || !*primary_key ||
        producer_output_for_display_key(producer, primary_key) != NULL) {
        return FALSE;
    }

    const guint registered = producer_registered_output_count(producer);
    const guint declared = producer_declared_output_count(producer);
    if (declared == 0)
        return registered == 0;

    /*
     * A stored primary display key is a persistent routing preference. Wait for
     * it only while the active consumer has not yet registered every declared
     * output. Once all declared outputs are present and the key is still
     * missing, clone mode falls back to the lowest output_id instead of
     * leaving the renderer unconfigured forever.
     */
    return registered < declared;
}

static Output*
producer_primary_output(Producer* producer)
{
    if (!producer)
        return NULL;

    const gchar* primary_key = producer->config.primary_display_key;
    if (primary_key && *primary_key) {
        Output* primary = producer_output_for_display_key(producer, primary_key);
        if (primary)
            return primary;
    }

    return producer_first_output_by_id(producer);
}

static void
producer_relayout_outputs(Producer* producer, gboolean mark_rebind)
{
    if (!producer)
        return;

    if (producer_uses_clone_routes(producer)) {
        if (producer_clone_route_waiting_for_primary(producer)) {
            /*
             * Clone mode has a single renderer-visible viewport. If the user
             * selected a primary consumer output and a secondary monitor happens
             * to register first, materializing the clone route here would let
             * that secondary size create the web/CEF page. Some wallpapers cache
             * window and canvas dimensions at load time, so wait until the
             * configured primary appears before exposing any renderer contract.
             */
            g_debug("VividProducer: clone route waiting for configured "
                    "primary-display-key=%s registered-outputs=%u declared-outputs=%u",
                    producer->config.primary_display_key &&
                        *producer->config.primary_display_key
                        ? producer->config.primary_display_key
                        : "(auto)",
                    producer_registered_output_count(producer),
                    producer_declared_output_count(producer));
            return;
        }

        Output* primary = producer_primary_output(producer);
        if (!primary)
            return;

        const gboolean clone_active = producer_output_has_wallpaper(producer, primary);
        if (!clone_active) {
            for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
                Client* client = g_ptr_array_index(producer->clients, client_index);
                for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
                    Output* output = g_ptr_array_index(client->outputs, output_index);
                    if (!output)
                        continue;
                    output->active = FALSE;
                    route_link_detach_output(&producer->route_table, output);
                    output->needs_renderer_rebind = FALSE;
                    output->needs_renderer_first_frame = FALSE;
                }
            }
            producer_prune_unused_routes(producer);
            return;
        }

        RenderRoute* route = producer_get_or_create_clone_route(producer);
        g_autoptr(GArray) sizes = g_array_new(FALSE, FALSE, sizeof(VividRenderSize));
        for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
            Client* client = g_ptr_array_index(producer->clients, client_index);
            for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
                Output* output = g_ptr_array_index(client->outputs, output_index);
                if (!output)
                    continue;
                output->active = TRUE;
                const VividRenderSize size = {
                    .width = output->target_width,
                    .height = output->target_height,
                };
                g_array_append_val(sizes, size);
            }
        }

        const VividRenderSize route_size =
            vivid_render_layout_clone_route_size((VividRenderSize) {
                                                    .width = primary->target_width,
                                                    .height = primary->target_height,
                                                 },
                                                 (const VividRenderSize*)sizes->data,
                                                 sizes->len,
                                                 OUTPUT_MAX_DIMENSION);
        const gboolean route_changed =
            route->width != route_size.width ||
            route->height != route_size.height ||
            route->scale != primary->scale;
        route->width = route_size.width;
        route->height = route_size.height;
        route->scale = primary->scale;

        for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
            Client* client = g_ptr_array_index(producer->clients, client_index);
            for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
                Output* output = g_ptr_array_index(client->outputs, output_index);
                RouteLink* link = route_link_attach(&producer->route_table, output, route);
                const gboolean output_changed =
                    route_link_apply_clone_layout(link,
                                                  output,
                                                  route->width,
                                                  route->height);
                producer_sync_link_clear_rgba(link, route);
                if (mark_rebind && (route_changed || output_changed)) {
                    output->needs_renderer_rebind = TRUE;
                    output->next_renderer_retry_time_usec = 0;
                }
            }
        }

        g_debug("VividProducer: clone route=%u primary-consumer=%u size=%ux%u "
                "outputs=%u changed=%s",
                route->route_id,
                primary->consumer_output_id,
                route->width,
                route->height,
                sizes->len,
                route_changed ? "true" : "false");
        producer_prune_unused_routes(producer);
        producer_sync_route_frame_sources(producer);
        return;
    }

    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (!producer_output_should_be_active(producer, output)) {
                if (output->active || output->route) {
                    g_message("VividProducer: independent output inactive "
                              "output=%u consumer-output=%u display-key=%s "
                              "old-route=%u has-wallpaper=%s",
                              output->output_id,
                              output->consumer_output_id,
                              output->display_key && *output->display_key
                                  ? output->display_key
                                  : "(none)",
                              output->route ? output->route->route_id : 0,
                              producer_output_has_wallpaper(producer, output)
                                  ? "true"
                                  : "false");
                }
                output->active = FALSE;
                route_link_detach_output(&producer->route_table, output);
                output->needs_renderer_rebind = FALSE;
                output->needs_renderer_first_frame = FALSE;
                continue;
            }
            output->active = TRUE;
            RenderRoute* route = producer_route_for_output(producer, output);
            const gboolean route_changed =
                route->width != output->target_width ||
                route->height != output->target_height ||
                route->scale != output->scale ||
                route->consumer_output_id != output->consumer_output_id;
            route->width = output->target_width;
            route->height = output->target_height;
            route->scale = output->scale;
            RouteLink* link = route_link_for_output(&producer->route_table, output);
            const OutputLayoutParams layout =
                producer_output_layout_params(producer, output);
            const gboolean output_changed = link
                ? route_link_apply_layout(link,
                                          output,
                                          route->width,
                                          route->height,
                                          layout.fillmode,
                                          layout.location,
                                          layout.rotation)
                : FALSE;
            if (link)
                producer_sync_link_clear_rgba(link, route);
            if (link && output->register_transform != 0) {
                const VividDisplayPrefs* prefs =
                    vivid_producer_config_display_prefs(&producer->config,
                                                        output->display_key);
                if (!prefs || prefs->rotation == VIVID_DISPLAY_PREFS_UNSET)
                    link->transform = output->register_transform;
            }
            if (mark_rebind && (route_changed || output_changed)) {
                output->needs_renderer_rebind = TRUE;
                output->next_renderer_retry_time_usec = 0;
                g_message("VividProducer: independent output relayout marked rebind "
                          "output=%u consumer-output=%u display-key=%s route=%u "
                          "route-changed=%s output-changed=%s target=%ux%u route=%ux%u "
                          "scale=%.3f",
                          output->output_id,
                          output->consumer_output_id,
                          output->display_key && *output->display_key
                              ? output->display_key
                              : "(none)",
                          route->route_id,
                          route_changed ? "true" : "false",
                          output_changed ? "true" : "false",
                          output->target_width,
                          output->target_height,
                          route->width,
                          route->height,
                          route->scale);
            }
        }
    }
    producer_prune_unused_routes(producer);
    producer_sync_route_frame_sources(producer);
}

static const gchar*
output_fourcc_format_name(guint32 fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
        return "xrgb8888";
    case DRM_FORMAT_ARGB8888:
        return "argb8888";
    case DRM_FORMAT_XBGR8888:
        return "xbgr8888";
    case DRM_FORMAT_ABGR8888:
        return "abgr8888";
    default:
        return "unknown";
    }
}

static gchar*
build_bind_buffers_json(Producer* producer, const Output* output)
{
    (void)producer;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_OUTPUT_ID);
    json_builder_add_int_value(builder, output->output_id);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_GENERATION);
    json_builder_add_int_value(builder, (gint64)output->generation);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_MEMORY_TYPE);
    json_builder_add_string_value(builder, "dmabuf");
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_FORMAT);
    json_builder_add_string_value(builder, output_fourcc_format_name(output->fourcc));
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_WIDTH);
    json_builder_add_int_value(builder, output->width);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_HEIGHT);
    json_builder_add_int_value(builder, output->height);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_LOGICAL_WIDTH);
    json_builder_add_int_value(builder, output->logical_width);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_LOGICAL_HEIGHT);
    json_builder_add_int_value(builder, output->logical_height);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_SCALE);
    json_builder_add_double_value(builder, output->scale);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_REFRESH_RATE_MHZ);
    json_builder_add_int_value(builder, output->refresh_rate_mhz);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_FOURCC);
    json_builder_add_int_value(builder, output->fourcc);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_MODIFIER);
    g_autofree gchar* modifier_text = g_strdup_printf("%" G_GUINT64_FORMAT,
                                                      output->modifier);
    json_builder_add_string_value(builder, modifier_text);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PLANES_PER_BUFFER);
    json_builder_add_int_value(builder,
                               output->n_buffers > 0 ? output->buffers[0].n_planes : 0);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PREMULTIPLIED);
    json_builder_add_boolean_value(builder, output->premultiplied);
    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_SYNC_MODE);
    json_builder_add_string_value(builder, "explicit-sync-fd+drm-syncobj-release");

    /*
     * GPU metadata is part of the DMA-BUF semantic contract. Fields are omitted
     * when the render device cannot be resolved; consumers must treat absence as
     * "not parsed" rather than a hard requirement.
     */
    VividGpuDevice resolved_gpu;
    if (vivid_producer_renderer_resolved_gpu(output_renderer(output), &resolved_gpu)) {
        g_autofree gchar* producer_uuid = gpu_uuid_to_hex(resolved_gpu.uuid);
        g_autofree gchar* producer_driver_uuid =
            gpu_uuid_to_hex(resolved_gpu.driver_uuid);
        /*
         * BIND_BUFFERS has a display-release render-node contract in addition
         * to the structured producer GPU metadata. The native GNOME paintable
         * uses renderNode to open the producer DRM device and signal the
         * per-frame release syncobj; producerRenderNode remains the explicit
         * metadata field consumed by helper/UI code.
         */
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_RENDER_NODE);
        json_builder_add_string_value(builder, resolved_gpu.render_node);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_RENDER_NODE);
        json_builder_add_string_value(builder, resolved_gpu.render_node);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_VENDOR);
        json_builder_add_string_value(builder,
                                      vivid_gpu_vendor_name(resolved_gpu.vendor_id));
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_PCI_ADDRESS);
        json_builder_add_string_value(builder, resolved_gpu.pci_address);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_DEVICE_UUID);
        json_builder_add_string_value(builder, producer_uuid);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_DRIVER_UUID);
        json_builder_add_string_value(builder, producer_driver_uuid);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_DRM_RENDER_MAJOR);
        json_builder_add_int_value(builder, resolved_gpu.drm_render_major);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRODUCER_DRM_RENDER_MINOR);
        json_builder_add_int_value(builder, resolved_gpu.drm_render_minor);
    }

    const ConsumerDmaBufCaps* caps = output->client ? &output->client->dmabuf_caps : NULL;
    if (caps && caps->present) {
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_BACKEND);
        json_builder_add_string_value(builder,
                                      caps->backend && *caps->backend
                                          ? caps->backend
                                          : "unknown");
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_RENDER_NODE);
        json_builder_add_string_value(builder,
                                      caps->render_node && *caps->render_node
                                          ? caps->render_node
                                          : "");
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_DEVICE_UUID);
        json_builder_add_string_value(builder,
                                      caps->device_uuid && *caps->device_uuid
                                          ? caps->device_uuid
                                          : "");
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_DRIVER_UUID);
        json_builder_add_string_value(builder,
                                      caps->driver_uuid && *caps->driver_uuid
                                          ? caps->driver_uuid
                                          : "");
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_DRM_RENDER_MAJOR);
        json_builder_add_int_value(builder, caps->peer_caps.identity.drm_render_major);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_DRM_RENDER_MINOR);
        json_builder_add_int_value(builder, caps->peer_caps.identity.drm_render_minor);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_NEGOTIATED_PATH);
        json_builder_add_string_value(builder, output_dmabuf_path_name(output->dmabuf_path));
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PRESENTATION_PATH);
        json_builder_add_string_value(builder, "shadow-copy");
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_MEMORY_HINT);
        json_builder_add_string_value(builder,
                                      dmabuf_memory_preference_name(output->memory_preference));
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_MEMORY_SOURCE);
        json_builder_add_string_value(builder, output_memory_source_name(output));
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_CONSUMER_IDENTITY_MODE);
        json_builder_add_string_value(builder,
                                      caps->identity_mode && *caps->identity_mode
                                          ? caps->identity_mode
                                          : "unknown");
    }

    json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_BUFFERS);
    json_builder_begin_array(builder);
    guint fd_index = 0;
    for (guint buffer_index = 0; buffer_index < output->n_buffers; buffer_index++) {
        const OutputBuffer* buffer = &output->buffers[buffer_index];
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_BUFFER_INDEX);
        json_builder_add_int_value(builder, buffer->index);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_BUFFER_SIZE);
        json_builder_add_int_value(builder, (gint64)buffer->size);
        json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_BUFFER_PLANES);
        json_builder_begin_array(builder);
        for (guint plane = 0; plane < buffer->n_planes; plane++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PLANE_FD_INDEX);
            json_builder_add_int_value(builder, fd_index++);
            json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PLANE_STRIDE);
            json_builder_add_int_value(builder, buffer->plane_strides[plane]);
            json_builder_set_member_name(builder, VIVID_JSON_BIND_BUFFERS_PLANE_OFFSET);
            json_builder_add_int_value(builder, buffer->plane_offsets[plane]);
            json_builder_end_object(builder);
        }
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    return json_node_to_compact_string(root);
}

static gboolean
send_bind_buffers(Client* client, const Output* output)
{
    if (output->n_buffers == 0)
        return FALSE;

    VividGpuDevice resolved_gpu;
    if (!vivid_producer_renderer_resolved_gpu(output_renderer(output), &resolved_gpu)) {
        g_warning("VividProducer: refusing to send BIND_BUFFERS for output=%u "
                  "because render-device='%s' has no resolved GPU render-node=(unresolved) "
                  "vendor=unknown device=(unknown)",
                  output->output_id,
                  client->producer->config.render_device
                      ? client->producer->config.render_device
                      : "auto");
        return FALSE;
    }

    g_autofree gchar* json = build_bind_buffers_json(client->producer, output);
    const guint8* body = (const guint8*)json;
    gint fds[VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE] = {0};
    guint n_fds = 0;

    for (guint buffer = 0; buffer < output->n_buffers; buffer++) {
        for (guint plane = 0; plane < output->buffers[buffer].n_planes; plane++) {
            if (n_fds >= G_N_ELEMENTS(fds)) {
                g_warning("VividProducer: too many DMA-BUF fds for output=%u",
                          output->output_id);
                return FALSE;
            }
            fds[n_fds++] = output->buffers[buffer].plane_fds[plane];
        }
    }

    /*
     * The descriptor is sent once when the buffer generation is bound. The
     * producer keeps its own fds open and later FRAME_READY messages only
     * select a buffer index. No pixel data is copied through the socket.
     */
    const gboolean sent = client_send_frame(client,
                                             VIVID_DISPLAY_EVT_BIND_BUFFERS,
                                             body,
                                             strlen(json),
                                             fds,
                                             n_fds);
    return sent;
}

static gchar*
build_set_config_json(Producer* producer, const Output* output)
{
    const RouteLink* link = route_link_for_output(&producer->route_table, output);
    const VividRenderRect source = link ? link->src_rect : output->source_rect;
    const VividRenderRect dest = link ? link->dst_rect : output->dest_rect;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const guint64 config_generation = producer_next_config_generation(producer);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_OUTPUT_ID);
    json_builder_add_int_value(builder, output->output_id);
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_GENERATION);
    json_builder_add_int_value(builder, (gint64)output->generation);
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_CONFIG_GENERATION);
    json_builder_add_int_value(builder, (gint64)config_generation);
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_SOURCE);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_X);
    json_builder_add_double_value(builder, source.x);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_Y);
    json_builder_add_double_value(builder, source.y);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_WIDTH);
    json_builder_add_double_value(builder, source.width);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_HEIGHT);
    json_builder_add_double_value(builder, source.height);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_DESTINATION);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_X);
    json_builder_add_double_value(builder, dest.x);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_Y);
    json_builder_add_double_value(builder, dest.y);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_WIDTH);
    json_builder_add_double_value(builder, dest.width);
    json_builder_set_member_name(builder, VIVID_JSON_RECT_HEIGHT);
    json_builder_add_double_value(builder, dest.height);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_TRANSFORM);
    json_builder_add_string_value(builder,
                                  vivid_render_layout_transform_string(
                                      link ? link->transform : 0));
    json_builder_set_member_name(builder, VIVID_JSON_SET_CONFIG_CLEAR_COLOR);
    json_builder_begin_array(builder);
    json_builder_add_double_value(builder, link ? link->clear_rgba[0] : 0.0);
    json_builder_add_double_value(builder, link ? link->clear_rgba[1] : 0.0);
    json_builder_add_double_value(builder, link ? link->clear_rgba[2] : 0.0);
    json_builder_add_double_value(builder, link ? link->clear_rgba[3] : 1.0);
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    return json_node_to_compact_string(root);
}

static gboolean
send_set_config(Client* client, const Output* output)
{
    if (!client || !output)
        return FALSE;

    g_autofree gchar* json = build_set_config_json(client->producer, output);
    const gboolean sent = client_send_frame(client,
                                             VIVID_DISPLAY_EVT_SET_CONFIG,
                                             (const guint8*)json,
                                             strlen(json),
                                             NULL,
                                             0);
    return sent;
}

static gboolean
send_bind_with_config(Client* client, const Output* output)
{
    /*
     * waywallen's display contract treats BIND_BUFFERS as entering
     * PENDING_CONFIG: the consumer must not accept FRAME_READY for the new
     * buffer_generation until it has seen a matching SET_CONFIG. Keep this pair
     * as one producer-side operation so every initial bind and rebind follows the
     * same ordering before the frame clock can publish a buffer index.
     */
    if (!send_bind_buffers(client, output))
        return FALSE;
    if (!send_set_config(client, output))
        return FALSE;
    return TRUE;
}

static gboolean
send_unbind(Client* client, const Output* output, guint64 generation)
{
    if (!client || !output)
        return FALSE;

    guint8 body[VIVID_DISPLAY_UNBIND_BODY_BYTES];
    vivid_display_unbind_body_write(body, output->output_id, generation);

    const gboolean sent =
        client_send_frame(client,
                          VIVID_DISPLAY_EVT_UNBIND,
                          body,
                          sizeof(body),
                          NULL,
                          0);
    if (sent) {
        const gboolean inserted =
            vivid_unbind_ack_tracker_register(&client->unbind_acks,
                                              output->output_id,
                                              generation,
                                              g_get_monotonic_time());
        g_message("VividProducer: UNBIND sent output=%u generation=%"
                  G_GUINT64_FORMAT " pending-ack=%s remaining=%u",
                  output->output_id,
                  generation,
                  inserted ? "new" : "refreshed",
                  vivid_unbind_ack_tracker_count(&client->unbind_acks));
    }
    return sent;
}

static ClientDeferredFrame*
client_deferred_frame_new(guint16 opcode, const guint8* body, gsize body_len)
{
    ClientDeferredFrame* frame = g_new0(ClientDeferredFrame, 1);
    frame->opcode = opcode;
    frame->body_len = body_len;
    if (body_len > 0 && body) {
        frame->body = g_malloc(body_len);
        memcpy(frame->body, body, body_len);
    }
    return frame;
}

static void
client_deferred_frame_free(ClientDeferredFrame* frame)
{
    if (!frame)
        return;
    g_free(frame->body);
    g_free(frame);
}

static void
client_queue_deferred_frame(Client* client, guint16 opcode, const guint8* body, gsize body_len)
{
    if (!client)
        return;
    g_queue_push_tail(&client->deferred_frames,
                      client_deferred_frame_new(opcode, body, body_len));
}

static gboolean
client_process_deferred_frames(Client* client)
{
    if (!client || client->processing_deferred_frames)
        return TRUE;

    client->processing_deferred_frames = TRUE;
    while (!g_queue_is_empty(&client->deferred_frames)) {
        ClientDeferredFrame* frame = g_queue_pop_head(&client->deferred_frames);
        const gboolean keep = handle_frame(client,
                                           frame->opcode,
                                           frame->body,
                                           frame->body_len);
        client_deferred_frame_free(frame);
        if (!keep) {
            client->processing_deferred_frames = FALSE;
            return FALSE;
        }
    }
    client->processing_deferred_frames = FALSE;
    return TRUE;
}

static void
client_clear_deferred_frames(Client* client)
{
    if (!client)
        return;

    while (!g_queue_is_empty(&client->deferred_frames))
        client_deferred_frame_free(g_queue_pop_head(&client->deferred_frames));
}

static gboolean
client_wait_unbind_ack(Client* client,
                       guint32 output_id,
                       guint64 generation,
                       guint   timeout_msec)
{
    if (!client ||
        !vivid_unbind_ack_tracker_contains(&client->unbind_acks,
                                           output_id,
                                           generation)) {
        return TRUE;
    }

    /*
     * UNBIND_DONE is the only frame that can prove the old consumer-side import
     * state has been torn down. During this short barrier window we still read
     * the socket so the ack is observed immediately, but defer all other
     * requests until the new generation has been committed. That preserves
     * generation-scoped BIND_FAILED semantics: a retry for the just-sent
     * generation must not be handled while the output object still carries the
     * old generation number.
     */
    const gint64 deadline_usec =
        g_get_monotonic_time() + ((gint64)timeout_msec * 1000);
    while (vivid_unbind_ack_tracker_contains(&client->unbind_acks,
                                             output_id,
                                             generation)) {
        const gint64 now_usec = g_get_monotonic_time();
        if (now_usec >= deadline_usec)
            break;

        const gint64 remaining_usec = deadline_usec - now_usec;
        struct pollfd pfd = {
            .fd = client->fd,
            .events = POLLIN | POLLHUP | POLLERR | POLLNVAL,
            .revents = 0,
        };
        const gint wait_msec = (gint)MAX((gint64)1,
                                        MIN((gint64)G_MAXINT,
                                            (remaining_usec + 999) / 1000));
        const gint poll_result = poll(&pfd, 1, wait_msec);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            g_warning("VividProducer: UNBIND_DONE wait poll failed output=%u "
                      "generation=%" G_GUINT64_FORMAT ": %s",
                      output_id,
                      generation,
                      g_strerror(errno));
            break;
        }
        if (poll_result == 0)
            break;
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            g_warning("VividProducer: UNBIND_DONE wait socket closed/error output=%u "
                      "generation=%" G_GUINT64_FORMAT " events=0x%x",
                      output_id,
                      generation,
                      pfd.revents);
            break;
        }
        if ((pfd.revents & POLLIN) == 0)
            continue;

        for (;;) {
            const gint result =
                vivid_display_recv_frame_nonblocking(client->fd, &client->recv_state);
            if (result == VIVID_DISPLAY_CODEC_FRAME_NEED_IO)
                break;

            if (result == VIVID_DISPLAY_CODEC_FRAME_DONE) {
                if (client->recv_state.opcode == VIVID_DISPLAY_REQ_UNBIND_DONE) {
                    handle_frame(client,
                                 client->recv_state.opcode,
                                 client->recv_state.body,
                                 client->recv_state.body_len);
                } else {
                    client_queue_deferred_frame(client,
                                                client->recv_state.opcode,
                                                client->recv_state.body,
                                                client->recv_state.body_len);
                }
                vivid_display_recv_state_clear(&client->recv_state);
                continue;
            }

            g_warning("VividProducer: UNBIND_DONE wait protocol error output=%u "
                      "generation=%" G_GUINT64_FORMAT ": %s",
                      output_id,
                      generation,
                      g_strerror(result < 0 ? -result : result));
            return FALSE;
        }
    }

    if (!vivid_unbind_ack_tracker_contains(&client->unbind_acks,
                                           output_id,
                                           generation)) {
        g_message("VividProducer: UNBIND_DONE barrier passed output=%u generation=%"
                  G_GUINT64_FORMAT " remaining=%u",
                  output_id,
                  generation,
                  vivid_unbind_ack_tracker_count(&client->unbind_acks));
        return TRUE;
    }

    const gint64 now_usec = g_get_monotonic_time();
    const gint64 age_usec =
        vivid_unbind_ack_tracker_age_usec(&client->unbind_acks,
                                          output_id,
                                          generation,
                                          now_usec);
    g_warning("VividProducer: UNBIND_DONE barrier timed out outputId=%u "
              "generation=%" G_GUINT64_FORMAT " age=%" G_GINT64_FORMAT
              "ms remaining=%u; continuing teardown",
              output_id,
              generation,
              age_usec >= 0 ? age_usec / 1000 : -1,
              vivid_unbind_ack_tracker_count(&client->unbind_acks));
    vivid_unbind_ack_tracker_forget(&client->unbind_acks, output_id, generation);
    return TRUE;
}

static gboolean
output_has_buffer_index(const Output* output, guint32 buffer_index)
{
    for (guint i = 0; output && i < output->n_buffers; i++) {
        if (output->buffers[i].index == buffer_index)
            return TRUE;
    }
    return FALSE;
}

static OutputBuffer*
output_find_buffer(Output* output, guint32 buffer_index)
{
    for (guint i = 0; output && i < output->n_buffers; i++) {
        if (output->buffers[i].index == buffer_index)
            return &output->buffers[i];
    }
    return NULL;
}

static const gchar*
output_memory_kind_name(OutputMemoryKind kind)
{
    return kind == OUTPUT_MEMORY_RENDERER_DMABUF ? "renderer" : "gbm";
}

static gboolean
send_frame_ready_event(Client* client,
                       Output* output,
                       guint32 buffer_index,
                       guint64 sequence,
                       guint64 target_time_usec,
                       gint    acquire_sync_fd,
                       ReleaseGroup* release_group)
{
    OutputBuffer* buffer = output_find_buffer(output, buffer_index);
    if (!buffer) {
        g_warning("VividProducer: refusing FRAME_READY for output=%u generation=%"
                  G_GUINT64_FORMAT " unknown buffer=%u",
                  output->output_id,
                  output->generation,
                  buffer_index);
        if (acquire_sync_fd >= 0)
            close(acquire_sync_fd);
        if (release_group)
            release_group_complete(release_group, "unknown-buffer");
        return FALSE;
    }

    if (!client_prepare_fd_send(client, VIVID_DISPLAY_EVT_FRAME_READY)) {
        if (acquire_sync_fd >= 0)
            close(acquire_sync_fd);
        if (release_group)
            release_group_complete(release_group, "socket-ordering-blocked");
        return FALSE;
    }

    gint acquire_fd = acquire_sync_fd;
    if (acquire_fd < 0) {
        if (!producer_create_signaled_sync_file(client->producer, &acquire_fd)) {
            if (release_group)
                release_group_complete(release_group, "acquire-create-failed");
            return FALSE;
        }
    }

    guint32 release_handle = 0;
    gint release_fd = -1;
    if (!producer_create_release_syncobj(client->producer, &release_handle, &release_fd)) {
        close(acquire_fd);
        if (release_group)
            release_group_complete(release_group, "release-create-failed");
        return FALSE;
    }
    const guint64 release_point = release_group
        ? release_group->aggregate_point
        : producer_release_coordinator_allocate_point(client->producer);
    if (release_point == 0) {
        close(acquire_fd);
        close(release_fd);
        producer_release_destroy_syncobj(client->producer,
                                         release_handle,
                                         "release-binary");
        if (release_group)
            release_group_complete(release_group, "release-point-failed");
        return FALSE;
    }

    guint8 body[VIVID_DISPLAY_FRAME_READY_BODY_BYTES];
    vivid_display_frame_ready_body_write(body,
                                          output->output_id,
                                          output->generation,
                                          buffer_index,
                                          sequence,
                                          target_time_usec,
                                          0);

    const gint fds[VIVID_DISPLAY_FRAME_READY_FD_COUNT] = { acquire_fd, release_fd };
    const gint result = vivid_display_send_frame(client->fd,
                                                  VIVID_DISPLAY_EVT_FRAME_READY,
                                                  body,
                                                  sizeof(body),
                                                  fds,
                                                  G_N_ELEMENTS(fds));
    close(acquire_fd);
    close(release_fd);

    if (result < 0) {
        g_warning("VividProducer: FRAME_READY send failed: %s", g_strerror(-result));
        producer_release_destroy_syncobj(client->producer,
                                         release_handle,
                                         "release-binary-send-failed");
        if (release_group)
            release_group_complete(release_group, "send-failed");
        return FALSE;
    }

    /*
     * Keep the display wire protocol unchanged: FRAME_READY still carries
     * [acquire sync_file, per-frame binary release syncobj]. The binary handle
     * stays producer-side only long enough for the reaper to observe the
     * consumer signal, then the reaper TRANSFERs binary@0 onto the
     * producer-owned release timeline at release_point. Renderer modules wait
     * that timeline point before reusing the DMA-BUF slot, matching
     * waywallen's pool/reaper split without exposing timeline details on the
     * Vivid display protocol.
     */
    buffer->release_point = release_point;
    buffer->release_sequence = sequence;
    buffer->release_created_usec = g_get_monotonic_time();
    render_route_release_note_buffer_point(output->route,
                                           buffer_index,
                                           release_point);

    ReleaseReaperRecord* record = g_new0(ReleaseReaperRecord, 1);
    record->binary_handle = release_handle;
    record->output_id = output->output_id;
    record->generation = output->generation;
    record->buffer_index = buffer_index;
    record->sequence = sequence;
    record->release_point = release_point;
    record->created_usec = buffer->release_created_usec;
    record->release_group = release_group_ref(release_group);
    if (!producer_release_coordinator_enqueue(client->producer, record)) {
        producer_release_signal_syncobj(client->producer,
                                        release_handle,
                                        "enqueue-failed");
        if (release_group) {
            release_group_complete(release_group, "enqueue-failed");
        } else {
            producer_release_transfer_to_timeline(client->producer,
                                                  release_handle,
                                                  release_point,
                                                  "enqueue-failed");
        }
        producer_release_destroy_syncobj(client->producer,
                                         release_handle,
                                         "release-binary-enqueue-failed");
        release_group_unref(record->release_group);
        g_free(record);
    }
    return result == 0;
}

static void
output_init_rebind_candidate(const Output* current, Output* candidate)
{
    memset(candidate, 0, sizeof(*candidate));
    output_init_plane_fds(candidate);
    candidate->client = current->client;
    candidate->route = current->route;
    candidate->active = current->active;
    candidate->consumer_output_id = current->consumer_output_id;
    candidate->monitor_index = current->monitor_index;
    candidate->output_id = current->output_id;
    candidate->display_x = current->display_x;
    candidate->display_y = current->display_y;
    candidate->logical_width = current->logical_width;
    candidate->logical_height = current->logical_height;
    candidate->target_width = current->target_width;
    candidate->target_height = current->target_height;
    candidate->width = current->width;
    candidate->height = current->height;
    candidate->scale = current->scale;
    candidate->refresh_rate_mhz = current->refresh_rate_mhz;
    candidate->desktop = current->desktop;
    candidate->display_key = current->display_key;
    candidate->display_name = current->display_name;
    candidate->source_rect = current->source_rect;
    candidate->dest_rect = current->dest_rect;
    candidate->generation = current->generation + 1;
    candidate->sequence = current->sequence + 1;
    candidate->n_producer_blacklist = current->n_producer_blacklist;
    memcpy(candidate->producer_blacklist,
           current->producer_blacklist,
           sizeof(candidate->producer_blacklist));
}

static gboolean
client_prepare_rebind_candidate(Client*                     client,
                                Output*                     output,
                                Output*                     candidate,
                                VividProducerRendererFrame* first_frame,
                                OutputRebindPrepareState*   prepare_state,
                                GError**                    error)
{
    if (prepare_state)
        *prepare_state = OUTPUT_REBIND_PREPARE_FAILED;

    output_init_rebind_candidate(output, candidate);
    candidate->renderer_generation =
        vivid_producer_renderer_generation(output_renderer(candidate));

    if (vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(candidate))) {
        const VividProducerRendererDmaBufPrepareStatus renderer_status =
            output_prepare_renderer_buffers(client->producer, candidate, error);
        if (renderer_status != VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK) {
            if (prepare_state &&
                renderer_status == VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY)
                *prepare_state = OUTPUT_REBIND_PREPARE_NOT_READY;
            return FALSE;
        }
        if (!output_validate_consumer_caps(candidate, error))
            return FALSE;

        /*
         * Waywallen treats the renderer's BindBuffers event as sufficient to
         * bind a cold display immediately. The router publishes Bind +
         * SetConfig at that point; FrameReady remains an independent later
         * renderer event. Do the same when this output has no prior pool.
         *
         * Requiring a bootstrap frame here creates a self-resetting loop for
         * asynchronous scene swapchains: every retry rebuilds the exported
         * pool, requests a frame, polls before the render thread can publish
         * it, then discards the candidate and repeats. run15 recorded this 90
         * times without ever sending BIND_BUFFERS to KDE.
         */
        if (output->n_buffers == 0) {
            candidate->sequence = output->sequence;
            output->renderer_first_frame_miss_count = 0;
            if (prepare_state)
                *prepare_state = OUTPUT_REBIND_PREPARE_READY;
            return TRUE;
        }

        /*
         * Renderer-owned DMA-BUFs are only safe to expose after the renderer has
         * produced a frame for the same exported buffer set. Binding a fresh
         * swapchain before its first frame lets consumers sample uninitialized or
         * still-transitioning GPU memory, which shows up as persistent corruption
         * after project switches. Keep the old generation on screen until a real
         * first frame can be sent immediately after BIND_BUFFERS.
         *
         * Paused playback makes that safety rule easy to deadlock: the render
         * loop may have a valid exported ring but no dirty frame, while the
         * producer refuses to publish BIND_BUFFERS until that first frame exists.
         * Ask the active renderer for a single bootstrap frame before polling
         * next_frame(); next_frame() still remains the only authority that a real
         * frame was produced for this exact buffer contract.
         */
        vivid_producer_renderer_request_dmabuf_frame(output_renderer(candidate),
                                                     "rebind-first-frame");
        if (!vivid_producer_renderer_next_dmabuf_frame(output_renderer(candidate),
                                                        first_frame)) {
            output->renderer_first_frame_miss_count++;
            if (prepare_state)
                *prepare_state = OUTPUT_REBIND_PREPARE_NOT_READY;
            set_buffer_error(error,
                             "renderer-owned DMA-BUF first frame is not ready for output=%u "
                             "misses=%u user-playing=%s policy-stopped=%s policy-paused=%s "
                             "renderer-generation=%" G_GUINT64_FORMAT
                             " output-renderer-generation=%" G_GUINT64_FORMAT,
                             output->output_id,
                             output->renderer_first_frame_miss_count,
                             client->producer->user_playing ? "true" : "false",
                             client->producer->policy_stopped ? "true" : "false",
                             client->producer->policy_paused ? "true" : "false",
                             candidate->renderer_generation,
                             output->renderer_generation);
            return FALSE;
        }

        if (!output_has_buffer_index(candidate, first_frame->buffer_index)) {
            set_buffer_error(error,
                             "renderer first frame buffer=%u is not part of output=%u generation=%"
                             G_GUINT64_FORMAT,
                             first_frame->buffer_index,
                             output->output_id,
                             candidate->generation);
            return FALSE;
        }

        candidate->sequence = first_frame->sequence;
        output->renderer_first_frame_miss_count = 0;
        if (prepare_state)
            *prepare_state = OUTPUT_REBIND_PREPARE_READY;
        return TRUE;
    }

    if (!output_prepare_gbm_buffer(client->producer, candidate, error))
        return FALSE;
    if (!output_validate_consumer_caps(candidate, error))
        return FALSE;

    first_frame->buffer_index = 0;
    first_frame->source_frame_id = 0;
    first_frame->sequence = candidate->sequence;
    first_frame->target_time_usec = (guint64)g_get_monotonic_time();
    first_frame->acquire_sync_fd = -1;
    if (prepare_state)
        *prepare_state = OUTPUT_REBIND_PREPARE_READY;
    return TRUE;
}

static OutputRebindResult
client_rebind_output(Client* client, Output* output)
{
    Output candidate = {0};
    /*
     * File descriptors are the one field where a zeroed C struct is not a
     * safe sentinel: fd 0 is a valid, unrelated descriptor (and in a
     * running producer it can be PulseAudio's shared-memory memfd).  The
     * renderer-rebind preparation helper deliberately leaves the frame
     * untouched on the cold renderer-owned path, so initialize the acquire
     * fence to the documented "no fence" value before calling it.
     */
    VividProducerRendererFrame first_frame = { .acquire_sync_fd = -1 };
    OutputRebindPrepareState prepare_state = OUTPUT_REBIND_PREPARE_FAILED;
    GError* error = NULL;
    if (!client_prepare_rebind_candidate(client,
                                         output,
                                         &candidate,
                                         &first_frame,
                                         &prepare_state,
                                         &error)) {
        const guint64 now = (guint64)g_get_monotonic_time();
        output->needs_renderer_rebind = TRUE;
        output->next_renderer_retry_time_usec = now + RENDERER_DMABUF_RETRY_INTERVAL_USEC;
        if (prepare_state == OUTPUT_REBIND_PREPARE_NOT_READY)
            producer_set_renderer_rebind_barrier(client->producer, output);
        else
            producer_clear_renderer_rebind_barrier(client->producer, output);
        g_message("VividProducer: output=%u renderer generation=%" G_GUINT64_FORMAT
                  " is not ready to switch yet: %s",
                  output->output_id,
                  vivid_producer_renderer_generation(output_renderer(output)),
                  error ? error->message : "unknown rebind preparation error");
        if (first_frame.acquire_sync_fd >= 0) {
            close(first_frame.acquire_sync_fd);
            first_frame.acquire_sync_fd = -1;
        }
        output_release_buffers(&candidate);
        g_clear_error(&error);
        return prepare_state == OUTPUT_REBIND_PREPARE_NOT_READY
            ? OUTPUT_REBIND_RESULT_NOT_READY
            : OUTPUT_REBIND_RESULT_FAILED;
    }
    producer_clear_renderer_rebind_barrier(client->producer, output);

    const guint64 old_generation = output->generation;
    g_message("VividProducer: prepared output=%u renderer generation=%" G_GUINT64_FORMAT
              " old generation=%" G_GUINT64_FORMAT
              " new generation=%" G_GUINT64_FORMAT
              " memory=%s buffers=%u first-buffer=%u first-sequence=%"
              G_GUINT64_FORMAT " first-target=%" G_GUINT64_FORMAT,
              output->output_id,
              candidate.renderer_generation,
              old_generation,
              candidate.generation,
              output_memory_kind_name(candidate.memory_kind),
              candidate.n_buffers,
              first_frame.buffer_index,
              first_frame.sequence,
              first_frame.target_time_usec);

    if (!send_bind_with_config(client, &candidate)) {
        output->needs_renderer_rebind = TRUE;
        if (first_frame.acquire_sync_fd >= 0) {
            close(first_frame.acquire_sync_fd);
            first_frame.acquire_sync_fd = -1;
        }
        output_release_buffers(&candidate);
        return OUTPUT_REBIND_RESULT_FAILED;
    }

    /*
     * Keep the cold scene-start ordering aligned with waywallen's router. Its
     * initial dynamic-renderer BindBuffers event is enough to publish
     * BindBuffers + SetConfig; FrameReady is a separate later renderer event.
     *
     * Vivid previously sent all three messages back-to-back from this one
     * frame-clock callback. On Plasma's threaded render loop the KDE consumer
     * could dispatch SET_CONFIG and FRAME_READY before the posted scene-graph
     * update was serviced. SET_CONFIG had already marked the QQuickItem
     * content dirty, so FRAME_READY's QQuickItem::update() was coalesced and
     * did not create a second wake-up. The item remained in the window dirty
     * list while its render thread slept until plasmashell was restarted.
     *
     * Only the no-previous-pool case is split here. Warm rebinds retain the
     * existing atomic switch: their first new frame is sent before UNBIND so
     * the old visible generation remains available throughout the handoff.
     * The next frame-clock tick requests and forwards the first frame for the
     * committed generation. No frame is polled synchronously for a cold bind.
     */
    const gboolean defer_cold_first_frame =
        output->n_buffers == 0 &&
        candidate.memory_kind == OUTPUT_MEMORY_RENDERER_DMABUF;
    if (defer_cold_first_frame) {
        if (first_frame.acquire_sync_fd >= 0) {
            close(first_frame.acquire_sync_fd);
            first_frame.acquire_sync_fd = -1;
        }

        output_release_buffers(output);
        *output = candidate;
        output->needs_renderer_rebind = FALSE;
        output->needs_renderer_first_frame = TRUE;
        output->renderer_first_frame_miss_count = 0;
        output->next_renderer_retry_time_usec = 0;

        g_message("VividProducer: committed cold pool before first FRAME_READY "
                  "output=%u generation=%" G_GUINT64_FORMAT,
                  output->output_id,
                  output->generation);
        return client_process_deferred_frames(client)
            ? OUTPUT_REBIND_RESULT_READY
            : OUTPUT_REBIND_RESULT_FAILED;
    }

    const gint first_frame_acquire_fd = first_frame.acquire_sync_fd;
    first_frame.acquire_sync_fd = -1;
    if (!send_frame_ready_event(client,
                                &candidate,
                                first_frame.buffer_index,
                                first_frame.sequence,
                                first_frame.target_time_usec,
                                first_frame_acquire_fd,
                                NULL)) {
        output->needs_renderer_rebind = TRUE;
        if (send_unbind(client, &candidate, candidate.generation))
            client_wait_unbind_ack(client,
                                   candidate.output_id,
                                   candidate.generation,
                                   UNBIND_ACK_TIMEOUT_MSEC);
        output_release_buffers(&candidate);
        client_process_deferred_frames(client);
        return OUTPUT_REBIND_RESULT_FAILED;
    }

    /*
     * Commit order matters. Consumers should receive and display the first frame
     * for the new generation before the old generation is retired. Sending UNBIND
     * first can clear the desktop or expose uninitialized GPU memory during a
     * project switch; sending it after the first new FRAME_READY makes the switch
     * atomic from the user's point of view.
     */
    if (output->n_buffers > 0 && send_unbind(client, output, old_generation)) {
        client_wait_unbind_ack(client,
                               output->output_id,
                               old_generation,
                               UNBIND_ACK_TIMEOUT_MSEC);
    }
    output_release_buffers(output);
    *output = candidate;
    output->needs_renderer_rebind = FALSE;
    output->next_renderer_retry_time_usec = 0;
    return client_process_deferred_frames(client)
        ? OUTPUT_REBIND_RESULT_READY
        : OUTPUT_REBIND_RESULT_FAILED;
}

static gboolean
client_try_upgrade_output_to_renderer_dmabuf(Client* client, Output* output)
{
    if (!vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(output)))
        return FALSE;
    if (output->memory_kind == OUTPUT_MEMORY_RENDERER_DMABUF)
        return FALSE;
    if (producer_renderer_rebind_barrier_blocks(client->producer, output))
        return FALSE;

    const guint64 now = (guint64)g_get_monotonic_time();
    if (output->next_renderer_retry_time_usec > now)
        return FALSE;
    return client_rebind_output(client, output) == OUTPUT_REBIND_RESULT_READY;
}

static gboolean
output_needs_renderer_rebind(const Output* output, guint64 renderer_generation)
{
    return output &&
        (output->needs_renderer_rebind ||
         output->renderer_generation != renderer_generation);
}

static gboolean
output_can_keep_current_renderer_frame(const Output* output,
                                       guint64       renderer_generation)
{
    return output &&
        output->active &&
        output->memory_kind == OUTPUT_MEMORY_RENDERER_DMABUF &&
        output->n_buffers > 0 &&
        !output->needs_renderer_first_frame &&
        output->renderer_generation == renderer_generation;
}

static gboolean
output_needs_paused_renderer_progress(const Producer* producer,
                                      const Output*   output,
                                      guint64       renderer_generation)
{
    if (!producer || !output)
        return FALSE;
    if (!vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(output)))
        return FALSE;

    return output_needs_renderer_rebind(output, renderer_generation) ||
        (output->needs_renderer_first_frame &&
         output->memory_kind == OUTPUT_MEMORY_RENDERER_DMABUF);
}

static gboolean
output_route_policy_paused(const Output* output)
{
    return output && output->route && output->route->policy_paused;
}

static void
producer_rebind_all_outputs(Producer* producer)
{
    if (!producer)
        return;

    if (producer_clone_route_waiting_for_primary(producer)) {
        g_debug("VividProducer: delaying output rebinds until configured "
                "clone primary-display-key=%s registers",
                producer->config.primary_display_key &&
                    *producer->config.primary_display_key
                    ? producer->config.primary_display_key
                    : "(auto)");
        for (guint client_index = 0; producer && client_index < producer->clients->len; client_index++) {
            Client* client = g_ptr_array_index(producer->clients, client_index);
            for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
                Output* output = g_ptr_array_index(client->outputs, output_index);
                output->needs_renderer_rebind = TRUE;
                output->next_renderer_retry_time_usec = 0;
            }
        }
        return;
    }

    producer_clear_renderer_rebind_barrier(producer, NULL);
    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (!output->active) {
                if (output->n_buffers > 0) {
                    send_unbind(client, output, output->generation);
                    output_release_buffers(output);
                }
                continue;
            }
            /*
             * Applying a config update touches every independent render route,
             * but unchanged routes keep the same renderer generation. Do not
             * force those outputs through BIND_BUFFERS again: a different
             * monitor may be waiting for its new renderer-owned DMA-BUF first
             * frame, and a needless rebind would put otherwise healthy outputs
             * behind that transient barrier.
             */
            const guint64 renderer_generation =
                vivid_producer_renderer_generation(output_renderer(output));
            if (output_needs_renderer_rebind(output, renderer_generation)) {
                output->needs_renderer_rebind = TRUE;
                output->next_renderer_retry_time_usec = 0;
            }
        }
    }

    producer_sync_route_frame_sources(producer);

    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len; output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (!output->active)
                continue;
            const guint64 renderer_generation =
                vivid_producer_renderer_generation(output_renderer(output));
            if (!output_needs_renderer_rebind(output, renderer_generation))
                continue;
            const OutputRebindResult result = client_rebind_output(client, output);
            if (result == OUTPUT_REBIND_RESULT_NOT_READY) {
                g_message("VividProducer: pausing batch output rebinds behind "
                          "output=%u until its renderer finishes the current "
                          "DMA-BUF initialization step",
                          output->output_id);
                return;
            }
        }
    }
}

static void
producer_disconnect_display_clients(Producer* producer, Client* exempt)
{
    if (!producer)
        return;

    for (gint client_index = (gint)producer->clients->len - 1; client_index >= 0; client_index--) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        if (!client || client == exempt)
            continue;

        /*
         * Runtime GPU switching is a process-boundary problem for the GTK
         * display helper: its EGL/Vulkan environment is fixed when gjs starts.
         * When the producer's resolved render node changes, any existing display
         * client is guaranteed to have the old helper environment. Closing that
         * socket forces the GNOME service to query the new producer state and
         * spawn a fresh helper before it sees new-GPU BIND_BUFFERS. Controller
         * clients stay connected so the WebUI still receives its ACK.
         */
        if (!client_role_includes_consumer(client) && client->outputs->len == 0)
            continue;

        client_free(client);
    }
}

static gboolean
send_frame_ready(Client* client, Output* output)
{
    guint32 buffer_index = 0;
    guint64 sequence = 0;
    guint64 target_time_usec = (guint64)g_get_monotonic_time();

    const guint64 renderer_generation =
        vivid_producer_renderer_generation(output_renderer(output));
    if (output_needs_renderer_rebind(output, renderer_generation)) {
        const guint64 now = (guint64)g_get_monotonic_time();
        const gboolean blocked_by_other_output =
            producer_renderer_rebind_barrier_blocks(client->producer, output);
        if (blocked_by_other_output) {
            if (!output_can_keep_current_renderer_frame(output, renderer_generation))
                return TRUE;
        } else {
            if (output->next_renderer_retry_time_usec == 0 ||
                output->next_renderer_retry_time_usec <= now) {
                g_message("VividProducer: attempting output rebind output=%u "
                          "output-generation=%" G_GUINT64_FORMAT
                          " output-renderer-generation=%" G_GUINT64_FORMAT
                          " renderer-generation=%" G_GUINT64_FORMAT
                          " needs-rebind=%s",
                          output->output_id,
                          output->generation,
                          output->renderer_generation,
                          renderer_generation,
                          output->needs_renderer_rebind ? "true" : "false");
                client_rebind_output(client, output);
            }
            return TRUE;
        }
    }

    if (output->memory_kind == OUTPUT_MEMORY_RENDERER_DMABUF) {
        /* A renderer may return a ready frame without an acquire fence. */
        VividProducerRendererFrame frame = { .acquire_sync_fd = -1 };
        if (output->needs_renderer_first_frame) {
            vivid_producer_renderer_request_dmabuf_frame(output_renderer(output),
                                                         "initial-first-frame");
        }
        if (!vivid_producer_renderer_next_dmabuf_frame(output_renderer(output), &frame)) {
            if (output->needs_renderer_first_frame) {
                output->renderer_first_frame_miss_count++;
                if (output->renderer_first_frame_miss_count == 1 ||
                    output->renderer_first_frame_miss_count % 60 == 0) {
                    g_message("VividProducer: initial renderer-owned DMA-BUF "
                              "first frame is not ready for output=%u misses=%u "
                              "user-playing=%s policy-stopped=%s policy-paused=%s "
                              "generation=%" G_GUINT64_FORMAT
                              " renderer-generation=%" G_GUINT64_FORMAT,
                              output->output_id,
                              output->renderer_first_frame_miss_count,
                              client->producer->user_playing ? "true" : "false",
                              client->producer->policy_stopped ? "true" : "false",
                              client->producer->policy_paused ? "true" : "false",
                              output->generation,
                              output->renderer_generation);
                }
            }
            return TRUE;
        }
        buffer_index = frame.buffer_index;
        sequence = frame.sequence;
        target_time_usec = frame.target_time_usec;
        output->sequence = sequence;
        if (output->needs_renderer_first_frame) {
            g_message("VividProducer: initial renderer-owned DMA-BUF first frame "
                      "ready output=%u generation=%" G_GUINT64_FORMAT
                      " buffer=%u sequence=%" G_GUINT64_FORMAT,
                      output->output_id,
                      output->generation,
                      buffer_index,
                      sequence);
            output->needs_renderer_first_frame = FALSE;
            output->renderer_first_frame_miss_count = 0;
        }
        const gboolean sent = send_frame_ready_event(client,
                                                     output,
                                                     buffer_index,
                                                     sequence,
                                                     target_time_usec,
                                                     frame.acquire_sync_fd,
                                                     NULL);
        return sent;
    } else {
        if (!output->bo)
            return FALSE;

        if (client_try_upgrade_output_to_renderer_dmabuf(client, output))
            return TRUE;

        output->sequence++;
        GError* error = NULL;
        if (!fill_output_buffer(client->producer, output, &error)) {
            g_warning("VividProducer: failed to refresh output=%u: %s",
                      output->output_id,
                      error ? error->message : "unknown frame refresh error");
            g_clear_error(&error);
            return FALSE;
        }
        buffer_index = 0;
        sequence = output->sequence;
    }

    const gboolean sent = send_frame_ready_event(client,
                                                 output,
                                                 buffer_index,
                                                 sequence,
                                                 target_time_usec,
                                                 -1,
                                                 NULL);
    return sent;
}

static GPtrArray*
route_collect_outputs(RenderRoute* route, Producer* producer)
{
    GPtrArray* outputs = g_ptr_array_new();
    if (!producer || !producer->route_table.links || !route)
        return outputs;

    for (guint i = 0; i < producer->route_table.links->len; i++) {
        RouteLink* link = g_ptr_array_index(producer->route_table.links, i);
        if (!link || link->route != route || !link->enabled || !link->output)
            continue;
        g_ptr_array_add(outputs, link->output);
    }

    return outputs;
}

static gboolean
route_send_frame(RenderRoute* route)
{
    if (!route || !route->producer)
        return FALSE;

    Producer* producer = route->producer;
    g_autoptr(GPtrArray) outputs = route_collect_outputs(route, producer);
    if (outputs->len == 0)
        return FALSE;

    if (outputs->len == 1 && !route->clone_route) {
        Output* output = g_ptr_array_index(outputs, 0);
        return send_frame_ready(output->client, output);
    }

    if (!vivid_producer_renderer_prefers_dmabuf_buffers(route->renderer))
        return FALSE;

    const guint64 renderer_generation =
        vivid_producer_renderer_generation(route->renderer);
    for (guint i = 0; i < outputs->len; i++) {
        Output* output = g_ptr_array_index(outputs, i);
        if (output_needs_renderer_rebind(output, renderer_generation)) {
            const guint64 now = (guint64)g_get_monotonic_time();
            if (output->next_renderer_retry_time_usec == 0 ||
                output->next_renderer_retry_time_usec <= now) {
                g_message("VividProducer: attempting route output rebind route=%u "
                          "output=%u renderer-generation=%" G_GUINT64_FORMAT,
                          route->route_id,
                          output->output_id,
                          renderer_generation);
                client_rebind_output(output->client, output);
            }
            return TRUE;
        }
    }

    gboolean needs_first_frame = FALSE;
    for (guint i = 0; i < outputs->len; i++) {
        Output* output = g_ptr_array_index(outputs, i);
        if (output->memory_kind != OUTPUT_MEMORY_RENDERER_DMABUF || output->n_buffers == 0)
            return FALSE;
        needs_first_frame = needs_first_frame || output->needs_renderer_first_frame;
    }

    if (needs_first_frame)
        vivid_producer_renderer_request_dmabuf_frame(route->renderer, "route-initial-first-frame");

    /* A renderer may return a ready frame without an acquire fence. */
    VividProducerRendererFrame frame = { .acquire_sync_fd = -1 };
    if (!vivid_producer_renderer_next_dmabuf_frame(route->renderer, &frame)) {
        if (needs_first_frame) {
            for (guint i = 0; i < outputs->len; i++) {
                Output* output = g_ptr_array_index(outputs, i);
                output->renderer_first_frame_miss_count++;
            }
        }
        /*
         * A missing renderer frame is a transient producer-side condition, not
         * a reason to destroy the route frame clock. Video pipelines can resume
         * from PAUSED before appsink has a fresh sample, and scene swapchains can
         * have an empty poll at the configured frame cadence. Returning FALSE
         * here removes the GSource while route->frame_source_id still looks
         * active, so later config changes cannot restart playback for that route.
         */
        return TRUE;
    }

    for (guint i = 0; i < outputs->len; i++) {
        Output* output = g_ptr_array_index(outputs, i);
        if (!output_has_buffer_index(output, frame.buffer_index)) {
            if (frame.acquire_sync_fd >= 0)
                close(frame.acquire_sync_fd);
            return FALSE;
        }
    }

    const guint64 aggregate_point =
        producer_release_coordinator_allocate_point(producer);
    if (aggregate_point == 0) {
        if (frame.acquire_sync_fd >= 0)
            close(frame.acquire_sync_fd);
        return FALSE;
    }

    ReleaseGroup* group = release_group_new(producer,
                                            route,
                                            frame.buffer_index,
                                            frame.sequence,
                                            outputs->len,
                                            aggregate_point);
    render_route_release_note_buffer_point(route,
                                           frame.buffer_index,
                                           aggregate_point);

    gboolean ok = TRUE;
    for (guint i = 0; i < outputs->len; i++) {
        Output* output = g_ptr_array_index(outputs, i);
        gint acquire_fd = -1;
        if (frame.acquire_sync_fd >= 0) {
            acquire_fd = dup(frame.acquire_sync_fd);
            if (acquire_fd < 0) {
                release_group_complete(group, "acquire-dup-failed");
                ok = FALSE;
                continue;
            }
        }

        output->sequence = frame.sequence;
        if (output->needs_renderer_first_frame) {
            output->needs_renderer_first_frame = FALSE;
            output->renderer_first_frame_miss_count = 0;
        }
        if (!send_frame_ready_event(output->client,
                                    output,
                                    frame.buffer_index,
                                    frame.sequence,
                                    frame.target_time_usec,
                                    acquire_fd,
                                    group)) {
            ok = FALSE;
        }
    }

    if (frame.acquire_sync_fd >= 0)
        close(frame.acquire_sync_fd);
    release_group_unref(group);
    return ok;
}

static guint
producer_frame_rate(const Producer* producer)
{
    return MAX(1u, producer ? producer->fps : DEFAULT_FPS);
}

static guint64
producer_frame_interval_usec(const Producer* producer)
{
    const guint fps = producer_frame_rate(producer);
    return (G_USEC_PER_SEC + fps - 1u) / fps;
}

static gint64
frame_clock_next_due_usec(const FrameClockSource* clock)
{
    /*
     * Use a cumulative ceiling division instead of a millisecond timeout.
     * A 60 FPS scene has a 16666.666us period; the old floor(1000 / fps)
     * timeout ran at 16ms, effectively polling the scene swapchain at 62.5Hz
     * and periodically finding no completed frame. Cumulative scheduling keeps
     * the average rate at the configured FPS while alternating the unavoidable
     * integer microsecond remainder without long-term drift.
     */
    const guint64 numerator =
        clock->tick_index * (guint64)G_USEC_PER_SEC + (guint64)clock->fps - 1u;
    return clock->start_time_usec + (gint64)(numerator / (guint64)clock->fps);
}

static gint
frame_clock_timeout_ms(const FrameClockSource* clock, gint64 now_usec)
{
    if (clock->next_due_usec <= now_usec)
        return 0;

    const gint64 remaining_usec = clock->next_due_usec - now_usec;
    return (gint)MIN((gint64)G_MAXINT, (remaining_usec + 999) / 1000);
}

static gboolean
frame_clock_prepare(GSource* source, gint* timeout_ms)
{
    FrameClockSource* clock = (FrameClockSource*)source;
    const gint64 now_usec = g_get_monotonic_time();
    if (timeout_ms)
        *timeout_ms = frame_clock_timeout_ms(clock, now_usec);
    return clock->next_due_usec <= now_usec;
}

static gboolean
frame_clock_check(GSource* source)
{
    FrameClockSource* clock = (FrameClockSource*)source;
    return clock->next_due_usec <= g_get_monotonic_time();
}

static gboolean
frame_clock_dispatch(GSource* source, GSourceFunc callback, gpointer user_data)
{
    FrameClockSource* clock = (FrameClockSource*)source;
    if (!callback)
        return G_SOURCE_REMOVE;

    const gboolean keep_source = callback(user_data);
    if (!keep_source)
        return G_SOURCE_REMOVE;

    /*
     * If the main loop was delayed, skip over missed presentation slots instead
     * of firing several FRAME_READY messages back-to-back. The consumer wants
     * the newest desktop frame cadence, not a burst of stale buffer selections.
     */
    const gint64 now_usec = g_get_monotonic_time();
    do {
        clock->tick_index++;
        clock->next_due_usec = frame_clock_next_due_usec(clock);
    } while (clock->next_due_usec <= now_usec);

    return G_SOURCE_CONTINUE;
}

static GSourceFuncs frame_clock_source_funcs = {
    frame_clock_prepare,
    frame_clock_check,
    frame_clock_dispatch,
    NULL,
};

static void
route_stop_frame_source(RenderRoute* route)
{
    if (!route || route->frame_source_id == 0)
        return;
    g_source_remove(route->frame_source_id);
    route->frame_source_id = 0;
}

void
producer_route_cancel_orphan_reap(RenderRoute* route)
{
    if (!route || route->orphan_reap_source_id == 0)
        return;
    g_source_remove(route->orphan_reap_source_id);
    route->orphan_reap_source_id = 0;
}

static void
producer_destroy_orphan_route(RenderRoute* route)
{
    if (!route || !route->producer)
        return;

    Producer* producer = route->producer;

    if (producer->route_table.links) {
        for (gint i = (gint)producer->route_table.links->len - 1; i >= 0; i--) {
            RouteLink* link = g_ptr_array_index(producer->route_table.links, (guint)i);
            if (!link || link->route != route)
                continue;
            if (link->output)
                link->output->route = NULL;
            g_ptr_array_remove_index(producer->route_table.links, (guint)i);
        }
    }

    /*
     * producer->routes uses render_route_free as its GDestroyNotify. Tear down
     * the route exactly once by removing it from the table; do not free the
     * renderer or route struct here or render_route_free will double-free.
     */
    for (guint i = 0; producer->routes && i < producer->routes->len; i++) {
        if (g_ptr_array_index(producer->routes, i) == route) {
            g_ptr_array_remove_index(producer->routes, i);
            return;
        }
    }

    g_warning("VividProducer: orphan route=%u missing from route table",
              route->route_id);
    render_route_free(route);
}

static gboolean
route_orphan_reap_timeout(gpointer user_data)
{
    RenderRoute* route = user_data;
    if (!route)
        return G_SOURCE_REMOVE;

    route->orphan_reap_source_id = 0;
    if (route_link_count_for_route(&route->producer->route_table, route) > 0)
        return G_SOURCE_REMOVE;

    g_message("VividProducer: reaping orphan render route=%u clone=%s",
              route->route_id,
              route->clone_route ? "true" : "false");
    producer_destroy_orphan_route(route);
    return G_SOURCE_REMOVE;
}

static void
route_schedule_orphan_reap(RenderRoute* route)
{
    if (!route || route->orphan_reap_source_id != 0)
        return;

    route->orphan_reap_source_id =
        g_timeout_add(VIVID_ROUTE_ORPHAN_REAP_TIMEOUT_MSEC,
                      route_orphan_reap_timeout,
                      route);
}

static gboolean route_frame_tick(gpointer user_data);

static guint
route_add_frame_source(RenderRoute* route)
{
    if (!route || !route->producer)
        return 0;

    const guint fps = producer_frame_rate(route->producer);
    GSource* source = g_source_new(&frame_clock_source_funcs, sizeof(FrameClockSource));
    FrameClockSource* clock = (FrameClockSource*)source;
    clock->fps = fps;
    clock->tick_index = 1;
    clock->start_time_usec = g_get_monotonic_time();
    clock->next_due_usec = frame_clock_next_due_usec(clock);

    g_source_set_priority(source, G_PRIORITY_HIGH);
    g_source_set_callback(source, route_frame_tick, route, NULL);
    g_source_set_name(source, "vivid-producer-route-frame-clock");
    const guint source_id = g_source_attach(source, NULL);
    g_source_unref(source);
    return source_id;
}

static void
route_ensure_frame_source(RenderRoute* route)
{
    if (!route || route->frame_source_id != 0)
        return;
    if (route_link_count_for_route(&route->producer->route_table, route) == 0)
        return;
    route->frame_source_id = route_add_frame_source(route);
}

static void
producer_sync_route_frame_sources(Producer* producer)
{
    if (!producer || !producer->routes)
        return;

    for (guint i = 0; i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (route_link_count_for_route(&producer->route_table, route) > 0)
            route_ensure_frame_source(route);
        else
            route_stop_frame_source(route);
    }
}

static gboolean
route_frame_tick(gpointer user_data)
{
    RenderRoute* route = user_data;
    Producer* producer = route ? route->producer : NULL;
    if (!producer)
        return G_SOURCE_REMOVE;

    if (producer_clone_route_waiting_for_primary(producer) && route->clone_route)
        return G_SOURCE_CONTINUE;

    g_autoptr(GPtrArray) outputs = route_collect_outputs(route, producer);
    if (outputs->len == 0)
        return G_SOURCE_CONTINUE;

    if (!producer->user_playing || producer->policy_stopped || producer->policy_paused) {
        if (!vivid_producer_renderer_prefers_dmabuf_buffers(route->renderer))
            return G_SOURCE_CONTINUE;

        const guint64 renderer_generation =
            vivid_producer_renderer_generation(route->renderer);
        gboolean needs_progress = FALSE;
        for (guint i = 0; i < outputs->len; i++) {
            Output* output = g_ptr_array_index(outputs, i);
            if (!output->active)
                continue;
            if (output_needs_paused_renderer_progress(producer, output, renderer_generation))
                needs_progress = TRUE;
        }
        if (needs_progress && !route_send_frame(route))
            return G_SOURCE_REMOVE;
        return G_SOURCE_CONTINUE;
    }

    if (route->clone_route) {
        return route_send_frame(route) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }

    for (guint i = 0; i < outputs->len; i++) {
        Output* output = g_ptr_array_index(outputs, i);
        if (!output->active)
            continue;
        if (output_route_policy_paused(output)) {
            const guint64 renderer_generation =
                vivid_producer_renderer_generation(output_renderer(output));
            if (output_needs_paused_renderer_progress(producer,
                                                      output,
                                                      renderer_generation) &&
                !route_send_frame(route))
                return G_SOURCE_REMOVE;
            continue;
        }
        if (!route_send_frame(route))
            return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
producer_restart_frame_sources(Producer* producer)
{
    producer_sync_route_frame_sources(producer);
}

static void
producer_set_frame_rate(Producer* producer, guint fps, const gchar* reason)
{
    if (!producer)
        return;

    const guint next_fps = CLAMP(fps, 1u, 240u);
    if (producer->fps == next_fps)
        return;

    const guint old_fps = producer->fps;
    producer->fps = next_fps;
    g_message("VividProducer: frame-ready rate %u -> %u fps via %s",
              old_fps,
              producer->fps,
              reason ? reason : "config");
    producer_restart_frame_sources(producer);
}

static void
producer_sync_frame_rate_from_config(Producer* producer, const gchar* reason)
{
    if (!producer)
        return;
    producer_set_frame_rate(producer, (guint)producer->config.scene_fps, reason);
}

static guint32
json_object_get_uint_default(JsonObject* object,
                             const char* member,
                             guint32     fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    return (guint32)MAX(0, json_object_get_int_member(object, member));
}

static gint32
json_object_get_int_default(JsonObject* object,
                            const char* member,
                            gint32      fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    return (gint32)json_object_get_int_member(object, member);
}

static gdouble
json_object_get_double_default(JsonObject* object,
                               const char* member,
                               gdouble     fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    return json_object_get_double_member(object, member);
}

static JsonObject*
parse_json_object(const guint8* body, gsize body_len, JsonParser** out_parser)
{
    JsonParser* parser = json_parser_new();
    GError* error = NULL;
    if (!json_parser_load_from_data(parser, (const gchar*)body, (gssize)body_len, &error)) {
        g_warning("VividProducer: invalid json payload: %s", error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("VividProducer: json payload root is not an object");
        g_object_unref(parser);
        return NULL;
    }

    *out_parser = parser;
    return json_node_get_object(root);
}

static JsonObject*
json_object_get_object_member_or_null(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;
    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;
    return json_node_get_object(node);
}

static JsonArray*
json_object_get_array_member_or_null(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;
    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;
    return json_node_get_array(node);
}

static VividOutputIndexEntry*
client_build_output_index_entries(Client* client, guint* n_entries_out)
{
    if (!client || !client->outputs || client->outputs->len == 0) {
        if (n_entries_out)
            *n_entries_out = 0;
        return NULL;
    }

    const guint n_entries = client->outputs->len;
    VividOutputIndexEntry* entries = g_new0(VividOutputIndexEntry, n_entries);
    for (guint i = 0; i < n_entries; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        entries[i].monitor_index = output ? output->monitor_index : 0;
        entries[i].output_id = output ? output->output_id : 0;
        entries[i].display_key = output ? output->display_key : NULL;
    }

    if (n_entries_out)
        *n_entries_out = n_entries;
    return entries;
}

static gboolean
json_object_get_boolean_default(JsonObject* object,
                                const gchar* member,
                                gboolean     fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    return json_object_get_boolean_member(object, member);
}

static void
client_reset_window_state_flags(Client* client)
{
    if (!client || !client->outputs)
        return;

    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (!output)
            continue;

        output->window_state_flags = 0;
        vivid_auto_policy_contribution_clear(&output->policy);
    }
}

static const gchar*
json_object_get_string_default(JsonObject* object,
                               const gchar* member,
                               const gchar* fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;

    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return fallback;

    const gchar* value = json_node_get_string(node);
    return value ? value : fallback;
}

static void
client_apply_window_facts_to_outputs(Client* client, const VividWindowFacts* facts)
{
    if (!client || !facts || !facts->outputs)
        return;

    for (guint i = 0; i < facts->outputs->len; i++) {
        const VividOutputWindowFacts* entry =
            &g_array_index(facts->outputs, VividOutputWindowFacts, i);
        Output* output = producer_find_output_by_id(client->producer, entry->output_id);
        if (!output || output->client != client)
            continue;

        output->window_state_flags = entry->flags;
    }
}

static void
client_cache_window_facts_json(Client* client, JsonObject* facts_json)
{
    if (!client)
        return;

    if (client->cached_window_facts_json)
        json_object_unref(client->cached_window_facts_json);
    client->cached_window_facts_json =
        facts_json ? json_object_ref(facts_json) : NULL;
}

static void
client_clear_cached_window_facts(Client* client)
{
    if (!client)
        return;

    if (client->cached_window_facts_json) {
        json_object_unref(client->cached_window_facts_json);
        client->cached_window_facts_json = NULL;
    }
}

static VividAutoAction
output_effective_auto_replay_action(const Output* output)
{
    if (!output)
        return VIVID_AUTO_ACTION_NONE;

    return output->auto_replay.resume_source_id
        ? output->auto_replay.raw
        : output->auto_replay.requested;
}

static gdouble
json_node_get_sample_value(JsonNode* node)
{
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return 0.0;

    const GType value_type = json_node_get_value_type(node);
    if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_INT64)
        return CLAMP(json_node_get_double(node), 0.0, 1.0);
    if (value_type == G_TYPE_BOOLEAN)
        return json_node_get_boolean(node) ? 1.0 : 0.0;

    return 0.0;
}

static gboolean
render_route_set_policy_paused(RenderRoute* route, gboolean paused)
{
    if (!route || route->policy_paused == paused)
        return FALSE;

    route->policy_paused = paused;
    return TRUE;
}

typedef struct
{
    Producer* producer;
    guint32   output_id;
    guint32   gen;
} AutoReplayResumeRequest;

static void producer_synthesize_policy_from_outputs(Producer* producer);
static void producer_commit_policy_state(Producer* producer,
                                         gboolean    old_mute,
                                         gboolean    old_pause,
                                         gboolean    old_stop,
                                         gboolean    route_policy_changed);

static const gchar*
vivid_auto_action_name(VividAutoAction action)
{
    switch (action) {
    case VIVID_AUTO_ACTION_MUTE:
        return "mute";
    case VIVID_AUTO_ACTION_PAUSE:
        return "pause";
    case VIVID_AUTO_ACTION_STOP:
        return "stop";
    case VIVID_AUTO_ACTION_NONE:
    default:
        return "none";
    }
}

static void
producer_log_window_state_trace(Producer* producer,
                                Client*    reporting_client,
                                const VividWindowFacts* facts)
{
    static gchar* last_fingerprint = NULL;
    g_autoptr(GString) fingerprint = g_string_new(NULL);
    g_autoptr(GString) detail = g_string_new("VividProducer: window-state trace");

    if (facts)
        vivid_window_facts_describe(facts, detail);

    for (guint client_index = 0; client_index < producer->clients->len; client_index++) {
        Client* client = g_ptr_array_index(producer->clients, client_index);
        for (guint output_index = 0; client && output_index < client->outputs->len;
             output_index++) {
            Output* output = g_ptr_array_index(client->outputs, output_index);
            if (!output)
                continue;

            g_string_append_printf(fingerprint,
                                   "o%u:m%u/f=0x%x/r=%d/q=%d;",
                                   output->output_id,
                                   output->monitor_index,
                                   output->window_state_flags,
                                   output->auto_replay.raw,
                                   output->auto_replay.requested);
            g_string_append_printf(detail,
                                   " output=%u monitor=%u key=%s flags=0x%x "
                                   "raw=%s requested=%s",
                                   output->output_id,
                                   output->monitor_index,
                                   output->display_key && *output->display_key
                                       ? output->display_key
                                       : "(none)",
                                   output->window_state_flags,
                                   vivid_auto_action_name(output->auto_replay.raw),
                                   vivid_auto_action_name(output->auto_replay.requested));
        }
    }

    g_string_append_printf(fingerprint,
                           "pol=%d%d%d",
                           producer->policy_muted,
                           producer->policy_paused,
                           producer->policy_stopped);

    for (guint i = 0; producer->routes && i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (!route)
            continue;

        g_string_append_printf(fingerprint,
                               "rt%u:%d/%d;",
                               route->route_id,
                               route->synthesized_action,
                               route->policy_paused);
        g_string_append_printf(detail,
                               " route=%u clone=%s action=%s route-paused=%s",
                               route->route_id,
                               route->clone_route ? "true" : "false",
                               vivid_auto_action_name(route->synthesized_action),
                               route->policy_paused ? "true" : "false");
    }

    g_string_append_printf(detail,
                           " policy-mute=%s policy-pause=%s policy-stop=%s",
                           producer->policy_muted ? "true" : "false",
                           producer->policy_paused ? "true" : "false",
                           producer->policy_stopped ? "true" : "false");

    (void)reporting_client;

    if (!last_fingerprint ||
        g_strcmp0(last_fingerprint, fingerprint->str) != 0) {
        g_message("%s", detail->str);
        g_free(last_fingerprint);
        last_fingerprint = g_strdup(fingerprint->str);
    }
}

static gboolean
auto_replay_resume_timeout(gpointer user_data)
{
    AutoReplayResumeRequest* request = user_data;
    Producer* producer = request ? request->producer : NULL;
    Output* output = producer_find_output_by_id(producer, request ? request->output_id : 0);

    if (output && output->auto_replay.gen == request->gen) {
        output->auto_replay.requested = output->auto_replay.raw;
        output->auto_replay.stop_applied =
            output->auto_replay.requested >= VIVID_AUTO_ACTION_STOP;
        producer_synthesize_policy_from_outputs(producer);
    }

    if (output)
        output->auto_replay.resume_source_id = 0;
    g_free(request);
    return G_SOURCE_REMOVE;
}

static void
output_update_auto_replay(Producer* producer, Output* output, VividAutoAction raw_action)
{
    VividAutoReplayState* state = &output->auto_replay;

    if (output->window_state_flags == state->last_flags && raw_action == state->raw)
        return;

    const VividAutoAction previous_effective = state->requested;
    const gboolean was_holding = previous_effective >= VIVID_AUTO_ACTION_PAUSE;
    const gboolean now_holding = raw_action >= VIVID_AUTO_ACTION_PAUSE;

    state->last_flags = output->window_state_flags;
    state->raw = raw_action;

    if (state->resume_source_id) {
        g_source_remove(state->resume_source_id);
        state->resume_source_id = 0;
    }

    if (now_holding) {
        state->requested = raw_action;
        state->stop_applied = raw_action >= VIVID_AUTO_ACTION_STOP;
        return;
    }

    if (was_holding && !now_holding) {
        if (previous_effective >= VIVID_AUTO_ACTION_STOP) {
            state->gen++;
            AutoReplayResumeRequest* request = g_new0(AutoReplayResumeRequest, 1);
            request->producer = producer;
            request->output_id = output->output_id;
            request->gen = state->gen;
            state->resume_source_id =
                g_timeout_add(VIVID_AUTO_POLICY_RESUME_DELAY_MSEC,
                              auto_replay_resume_timeout,
                              request);
            return;
        }

        state->requested = raw_action;
        state->stop_applied = FALSE;
        return;
    }

    state->requested = raw_action;
    state->stop_applied = FALSE;
}

static void
producer_synthesize_policy_from_outputs(Producer* producer)
{
    gboolean mute = FALSE;
    gboolean pause = FALSE;
    gboolean stop = FALSE;
    gboolean route_policy_changed = FALSE;
    const gboolean old_mute = producer->policy_muted;
    const gboolean old_pause = producer->policy_paused;
    const gboolean old_stop = producer->policy_stopped;

    for (guint i = 0; producer->routes && i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (!route)
            continue;

        route->synthesized_action = VIVID_AUTO_ACTION_NONE;
        route_policy_changed =
            render_route_set_policy_paused(route, FALSE) || route_policy_changed;
    }

    if (producer->route_table.links) {
        for (guint link_index = 0; link_index < producer->route_table.links->len; link_index++) {
            RouteLink* link = g_ptr_array_index(producer->route_table.links, link_index);
            Output* output = link ? link->output : NULL;
            if (!link || !link->enabled || !output)
                continue;

            const VividAutoAction effective_action =
                output_effective_auto_replay_action(output);

            if (output->policy.global_mute ||
                effective_action == VIVID_AUTO_ACTION_MUTE) {
                mute = TRUE;
            }
            if (output->policy.global_stop ||
                effective_action >= VIVID_AUTO_ACTION_STOP) {
                stop = TRUE;
            }
            if (output->policy.global_pause)
                pause = TRUE;
        }
    }

    for (guint route_index = 0; producer->routes && route_index < producer->routes->len;
         route_index++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, route_index);
        if (!route)
            continue;

        gboolean route_paused = pause;
        VividAutoAction route_action = VIVID_AUTO_ACTION_NONE;
        guint route_output_count = 0;

        if (producer->route_table.links) {
            for (guint link_index = 0; link_index < producer->route_table.links->len;
                 link_index++) {
                RouteLink* link =
                    g_ptr_array_index(producer->route_table.links, link_index);
                Output* output = link ? link->output : NULL;
                if (!link || !link->enabled || !output || link->route != route)
                    continue;

                const VividAutoAction effective_action =
                    output_effective_auto_replay_action(output);
                route_output_count++;
                if (route->clone_route) {
                    route_action = route_output_count == 1
                        ? effective_action
                        : vivid_auto_action_max(route_action, effective_action);
                } else {
                    route_action = effective_action;
                }

                if (!route_paused && output->policy.route_pause) {
                    if (route->clone_route) {
                        pause = TRUE;
                        route_paused = TRUE;
                    } else {
                        route_paused = TRUE;
                    }
                }
            }
        }

        route->synthesized_action = route_action;
        route_policy_changed =
            render_route_set_policy_paused(route, route_paused) || route_policy_changed;
    }

    producer->policy_muted = mute;
    producer->policy_paused = pause;
    producer->policy_stopped = stop;
    producer_commit_policy_state(producer,
                                 old_mute,
                                 old_pause,
                                 old_stop,
                                 route_policy_changed);
}

static void
producer_eval_window_state_for_client(Client* client, JsonObject* payload)
{
    if (!client || !client->producer || !payload)
        return;

    Producer* producer = client->producer;
    guint n_entries = 0;
    g_autofree VividOutputIndexEntry* entries =
        client_build_output_index_entries(client, &n_entries);
    if (!entries || n_entries == 0)
        return;

    g_autoptr(VividWindowFacts) facts =
        vivid_window_facts_parse(payload, entries, n_entries);
    if (!facts)
        return;

    client_cache_window_facts_json(client, payload);
    client_reset_window_state_flags(client);
    client_apply_window_facts_to_outputs(client, facts);

    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (!output)
            continue;

        VividOutputWindowFacts output_facts = {
            .output_id = output->output_id,
            .flags = output->window_state_flags,
        };

        VividAutoPolicy effective = {0};
        vivid_producer_config_effective_auto_policy(&producer->config,
                                                    output->display_key,
                                                    &effective);
        output->policy = vivid_auto_policy_evaluate(&effective,
                                                    producer->config.application_rules,
                                                    &output_facts,
                                                    &facts->session);
        output_update_auto_replay(producer,
                                  output,
                                  output->policy.action);
    }

    producer_synthesize_policy_from_outputs(producer);
    producer_log_window_state_trace(producer, client, facts);
}

static void
producer_replay_cached_window_facts_for_client(Client* client)
{
    if (!client || !client->cached_window_facts_json)
        return;

    producer_eval_window_state_for_client(client, client->cached_window_facts_json);
}

static void
producer_commit_policy_state(Producer* producer,
                             gboolean    old_mute,
                             gboolean    old_pause,
                             gboolean    old_stop,
                             gboolean    route_policy_changed)
{
    const gboolean mute = producer->policy_muted;
    const gboolean pause = producer->policy_paused;
    const gboolean stop = producer->policy_stopped;

    if (old_mute == mute && old_pause == pause && old_stop == stop && !route_policy_changed)
        return;

    g_message("VividProducer: policy state mute=%s pause=%s stop=%s",
              mute ? "true" : "false",
              pause ? "true" : "false",
              stop ? "true" : "false");
    if (old_mute != mute)
        producer_apply_config_to_renderers(producer);
    producer_apply_playback_policy(producer);
    if ((old_pause && !pause) || (old_stop && !stop))
        producer_rebind_all_outputs(producer);
}

static void
producer_apply_playback_policy(Producer* producer)
{
    if (!producer->renderer)
        return;

    /*
     * Policy signals are facts from the consumer; user_playing is explicit
     * controller intent. Keeping this merge point in the producer gives every
     * future consumer the same pause behavior without duplicating policy code.
     */
    RenderRoute* clone_route = producer_find_clone_route(producer);
    const gboolean clone_renderer_idle =
        !producer_uses_clone_routes(producer) ||
        !clone_route ||
        !render_route_is_used(producer, clone_route);

    vivid_producer_renderer_set_playback_stopped(producer->renderer,
                                                 producer->policy_stopped);
    vivid_producer_renderer_set_playback_paused(
        producer->renderer,
        clone_renderer_idle || producer->policy_paused ||
        producer->policy_stopped || !producer->user_playing);
    for (guint i = 0; producer->routes && i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (!route || !route->owns_renderer || !route->renderer)
            continue;
        vivid_producer_renderer_set_playback_stopped(route->renderer,
                                                     producer->policy_stopped);
        vivid_producer_renderer_set_playback_paused(
            route->renderer,
            !render_route_is_used(producer, route) ||
            producer->policy_paused || route->policy_paused ||
            producer->policy_stopped || !producer->user_playing);
    }
}

static void
producer_apply_config_to_global_renderer(Producer* producer)
{
    if (!producer || !producer->renderer)
        return;

    /*
     * The root renderer owns GPU discovery, device resolution, state snapshots,
     * and the primary wallpaper. Keep that scene configured while independent
     * routes are active; detached per-output routes are reaped by
     * producer_prune_unused_routes instead of retaining their Vulkan pools.
     */
    {
        VividProducerConfig root_config = producer->config;
        g_autofree gchar* per_output_project_path = NULL;
        g_autofree gchar* per_output_user_properties = NULL;
        Output* primary = producer_primary_output(producer);
        const gchar* primary_display_key = primary && primary->display_key
            ? primary->display_key
            : (producer->config.primary_display_key
                   ? producer->config.primary_display_key
                   : "");
        const JsonObject* per_output =
            vivid_producer_config_display_project_view(&producer->config,
                                                       primary ? primary->display_key : NULL);
        if (per_output) {
            const gchar* project_path =
                json_object_get_string_alias(per_output,
                                             "project-path",
                                             "projectPath",
                                             NULL);
            if (project_path)
                per_output_project_path = g_strdup(project_path);

            if (project_path && primary && primary->display_key) {
                per_output_user_properties =
                    vivid_producer_config_stored_user_properties_json(
                        &producer->config,
                        primary->display_key,
                        project_path);
            }
            if (!per_output_user_properties)
                per_output_user_properties = g_strdup("{}");

            root_config.content_fit =
                json_object_get_int_alias(per_output,
                                          "content-fit",
                                          "contentFit",
                                          root_config.content_fit,
                                          1,
                                          3);
            root_config.mute =
                json_object_get_boolean_default(
                    per_output,
                    "mute",
                    json_object_get_boolean_default(per_output,
                                                    "muted",
                                                    root_config.mute));
        }
        root_config.mute = root_config.mute || producer->policy_muted;
        vivid_producer_renderer_apply_config(producer->renderer,
                                             &root_config,
                                             per_output_project_path,
                                             per_output_user_properties);
        g_message("VividProducer: root renderer config primary-display-key=%s project=%s",
                  primary_display_key && *primary_display_key
                      ? primary_display_key
                      : "(auto)",
                  per_output_project_path && *per_output_project_path
                      ? per_output_project_path
                      : "(none)");
        RenderRoute* clone_route = producer_find_clone_route(producer);
        if (clone_route && producer->route_table.links) {
            for (guint link_index = 0; link_index < producer->route_table.links->len; link_index++) {
                RouteLink* link = g_ptr_array_index(producer->route_table.links, link_index);
                if (link && link->route == clone_route)
                    producer_sync_link_clear_rgba(link, clone_route);
            }
            producer_resync_route_set_configs(producer, clone_route);
        }
    }
}

static void
producer_render_route_apply_config(RenderRoute* route)
{
    if (!route || !route->renderer || !route->producer)
        return;

    /*
     * Clone mode renders the primary monitor's wallpaper image once; secondary
     * monitors only cover-crop that image through SET_CONFIG. Independent routes
     * look up wallpaper bindings by the monitor connector key while still
     * inheriting global audio, GPU, content-fit, and frame-rate settings.
     */
    VividProducerConfig route_config = route->producer->config;
    g_autofree gchar* per_output_project_path = NULL;
    g_autofree gchar* per_output_user_properties = NULL;
    if (!route->clone_route && route->consumer_output_id != 0) {
        Output* route_output =
            producer_find_consumer_output(route->producer, route->consumer_output_id);
        const JsonObject* per_output =
            vivid_producer_config_display_project_view(&route->producer->config,
                                                       route_output
                                                           ? route_output->display_key
                                                           : NULL);
        if (per_output) {
            const gchar* project_path =
                json_object_get_string_alias(per_output,
                                             "project-path",
                                             "projectPath",
                                             NULL);
            if (project_path)
                per_output_project_path = g_strdup(project_path);

            if (project_path && route_output && route_output->display_key) {
                per_output_user_properties =
                    vivid_producer_config_stored_user_properties_json(
                        &route->producer->config,
                        route_output->display_key,
                        project_path);
            }
            if (!per_output_user_properties)
                per_output_user_properties = g_strdup("{}");

            route_config.content_fit =
                json_object_get_int_alias(per_output,
                                          "content-fit",
                                          "contentFit",
                                          route_config.content_fit,
                                          1,
                                          3);
            /*
             * Independent render routes inherit global audio by default, but a
             * display chooser can store a per-output mute flag beside that
             * output's wallpaper selection. Keep the override local to this
             * route so muting one monitor does not leak into clone rendering or
             * other independent monitors.
             */
            route_config.mute =
                json_object_get_boolean_default(
                    per_output,
                    "mute",
                    json_object_get_boolean_default(per_output,
                                                    "muted",
                                                    route_config.mute));
        }
    }

    route_config.mute = route_config.mute || route->producer->policy_muted;
    vivid_producer_renderer_apply_config(route->renderer,
                                         &route_config,
                                         per_output_project_path,
                                         per_output_user_properties);

    if (route->producer->route_table.links) {
        for (guint link_index = 0; link_index < route->producer->route_table.links->len; link_index++) {
            RouteLink* link = g_ptr_array_index(route->producer->route_table.links, link_index);
            if (link && link->route == route)
                producer_sync_link_clear_rgba(link, route);
        }
    }
    producer_resync_route_set_configs(route->producer, route);
}

static void
producer_apply_config_to_routes(Producer* producer)
{
    if (!producer || !producer->routes)
        return;

    for (guint i = 0; i < producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(producer->routes, i);
        if (!route || !route->owns_renderer || !route->renderer)
            continue;
        producer_render_route_apply_config(route);
        vivid_producer_renderer_set_playback_stopped(route->renderer,
                                                     producer->policy_stopped);
        vivid_producer_renderer_set_playback_paused(
            route->renderer,
            producer->policy_paused || route->policy_paused ||
            producer->policy_stopped || !producer->user_playing);
    }
}

static void
producer_apply_config_to_renderers(Producer* producer)
{
    producer_apply_config_to_global_renderer(producer);
    producer_apply_config_to_routes(producer);
}

static gboolean
handle_window_state(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return TRUE;

    const gchar* schema = json_object_get_string_default(object, VIVID_JSON_WINDOW_STATE_SCHEMA, "");
    if (g_strcmp0(schema, VIVID_DISPLAY_WINDOW_STATE_SCHEMA_V2) != 0) {
        send_display_error(client,
                           VIVID_DISPLAY_ERR_WINDOW_STATE_INVALID,
                           FALSE,
                           "window-state schema must be %s",
                           VIVID_DISPLAY_WINDOW_STATE_SCHEMA_V2);
        g_object_unref(parser);
        return TRUE;
    }

    if (!json_object_has_member(object, VIVID_JSON_WINDOW_STATE_DISPLAYS)) {
        send_display_error(client,
                           VIVID_DISPLAY_ERR_WINDOW_STATE_INVALID,
                           FALSE,
                           "window-state payload missing displays section");
        g_object_unref(parser);
        return TRUE;
    }

    producer_eval_window_state_for_client(client, object);

    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_media_state(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return TRUE;

    JsonNode* root = json_parser_get_root(parser);
    g_autofree gchar* compact_json = json_node_to_compact_string(root);
    vivid_producer_renderer_set_media_state_json(client->producer->renderer,
                                                  compact_json);
    for (guint i = 0; client->producer->routes && i < client->producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(client->producer->routes, i);
        if (!route || !route->owns_renderer || !route->renderer)
            continue;
        vivid_producer_renderer_set_media_state_json(route->renderer, compact_json);
    }

    client->producer->media_state_received++;
    const gchar* title = json_object_get_string_default(object, "title", "");
    const gchar* artist = json_object_get_string_default(object, "artist", "");
    const gchar* thumbnail_path =
        json_object_get_string_default(object, "thumbnailPath", "");
    const gboolean has_thumbnail =
        json_object_get_boolean_default(object, "hasThumbnail", FALSE);
    g_message("VividProducer: media state received #%" G_GUINT64_FORMAT
              " title='%s' artist='%s' has-thumbnail=%s thumbnail-path=%s bytes=%"
              G_GSIZE_FORMAT,
              client->producer->media_state_received,
              title,
              artist,
              has_thumbnail ? "true" : "false",
              has_thumbnail && thumbnail_path && *thumbnail_path ? thumbnail_path : "(none)",
              body_len);

    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_audio_samples_bin(Client* client, const guint8* body, gsize body_len)
{
    if (client->negotiated_version < VIVID_DISPLAY_PROTOCOL_MIN_SUPPORTED + 1)
        return TRUE;

    if (body_len < 12)
        return TRUE;

    const guint32 count = vivid_display_codec_read_u32(body);
    if (count != VIVID_DISPLAY_AUDIO_SAMPLES_BIN_MAX_COUNT)
        return TRUE;

    const gsize expected = vivid_display_audio_samples_bin_body_bytes(count);
    if (body_len != expected)
        return TRUE;

    g_autofree float* samples = g_new(float, count);
    uint64_t time_usec = 0;
    uint32_t decoded_count = 0;
    if (vivid_display_audio_samples_bin_body_read(body,
                                                   body_len,
                                                   &decoded_count,
                                                   &time_usec,
                                                   samples,
                                                   count) < 0)
        return TRUE;

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ad"));
    for (guint32 i = 0; i < decoded_count; i++) {
        gfloat sample = samples[i];
        if (!isfinite(sample) || sample <= 0.f)
            sample = 0.f;
        g_variant_builder_add(&builder, "d", (gdouble)sample);
    }

    GVariant* audio_samples = g_variant_ref_sink(g_variant_builder_end(&builder));
    vivid_producer_renderer_set_audio_samples(client->producer->renderer, audio_samples);
    for (guint i = 0; client->producer->routes && i < client->producer->routes->len; i++) {
        RenderRoute* route = g_ptr_array_index(client->producer->routes, i);
        if (!route || !route->owns_renderer || !route->renderer)
            continue;
        vivid_producer_renderer_set_audio_samples(route->renderer, audio_samples);
    }
    g_variant_unref(audio_samples);

    return TRUE;
}

static guint64
control_request_id_from_payload(JsonObject* payload)
{
    guint64 request_id = 0;
    if (!payload)
        return 0;
    if (json_value_to_uint64(json_object_get_member(payload, "requestId"), &request_id))
        return request_id;
    return 0;
}

static gboolean
handle_control(Client* client, const guint8* body, gsize body_len)
{
    if (body_len < VIVID_DISPLAY_CONTROL_HEADER_BYTES) {
        send_control_error(client,
                           0,
                           VIVID_DISPLAY_ERR_CONTROL_MALFORMED,
                           0,
                           "control frame body too small");
        return TRUE;
    }

    VividDisplayControlHeader header = {0};
    const gint decode_result =
        vivid_display_control_header_decode(body, body_len, &header);
    if (decode_result < 0) {
        send_control_error(client,
                           0,
                           VIVID_DISPLAY_ERR_CONTROL_MALFORMED,
                           0,
                           "control header decode failed");
        return TRUE;
    }

    if ((gsize)header.json_length != body_len - VIVID_DISPLAY_CONTROL_HEADER_BYTES) {
        send_control_error(client,
                           header.opcode,
                           VIVID_DISPLAY_ERR_CONTROL_MALFORMED,
                           0,
                           "control json length mismatch");
        return TRUE;
    }

    JsonParser* parser = NULL;
    JsonObject* payload = parse_json_object(body + VIVID_DISPLAY_CONTROL_HEADER_BYTES,
                                            header.json_length,
                                            &parser);
    if (!payload) {
        send_control_error(client,
                           header.opcode,
                           VIVID_DISPLAY_ERR_CONTROL_MALFORMED,
                           0,
                           "control payload is not valid JSON");
        return TRUE;
    }

    const guint64 request_id = control_request_id_from_payload(payload);

    if (header.opcode == VIVID_DISPLAY_CONTROL_GET_STATE) {
        g_autofree gchar* state = build_control_state_json(client->producer);
        g_autofree gchar* json =
            g_strdup_printf("{\"requestId\":%" G_GUINT64_FORMAT ",%s",
                            request_id,
                            state && state[0] == '{' ? state + 1 : state ? state : "");
        gboolean ok = send_control_json(client,
                                        VIVID_DISPLAY_CONTROL_STATE_SNAPSHOT,
                                        json,
                                        request_id);
        g_object_unref(parser);
        return ok;
    }

    g_autofree gchar* previous_render_device =
        g_strdup(producer_effective_render_device(&client->producer->config));
    g_autofree gchar* previous_multi_display_mode =
        g_strdup(client->producer->config.multi_display_mode
                     ? client->producer->config.multi_display_mode
                     : "clone");
    const g_autofree gchar* previous_primary_display_key =
        g_strdup(client->producer->config.primary_display_key
                     ? client->producer->config.primary_display_key
                     : "");
    const gint previous_content_fit = client->producer->config.content_fit;

    if (header.opcode == VIVID_DISPLAY_CONTROL_SET_PLAYING) {
        client->producer->user_playing =
            json_object_get_boolean_default(payload, "playing", client->producer->user_playing);
        producer_apply_playback_policy(client->producer);
        g_message("VividProducer: playback user state playing=%s",
                  client->producer->user_playing ? "true" : "false");
        send_control_ack(client, header.opcode, FALSE, request_id);
        g_object_unref(parser);
        return TRUE;
    }

    GHashTable* previous_display_prefs =
        vivid_producer_config_snapshot_display_prefs(&client->producer->config);

    GError* apply_error = NULL;
    const gboolean changed =
        vivid_producer_config_apply_control(&client->producer->config,
                                             header.opcode,
                                             payload,
                                             &apply_error);
    if (!changed) {
        send_control_error(client,
                           header.opcode,
                           VIVID_DISPLAY_ERR_CONTROL_INVALID,
                           request_id,
                           apply_error
                               ? apply_error->message
                               : "unsupported or invalid control request");
        vivid_producer_config_free_display_prefs_snapshot(previous_display_prefs);
        g_clear_error(&apply_error);
        g_object_unref(parser);
        return TRUE;
    }
    g_clear_error(&apply_error);

    vivid_producer_config_schedule_save(&client->producer->config);
    producer_relayout_outputs(client->producer, TRUE);
    producer_apply_config_to_renderers(client->producer);
    producer_sync_frame_rate_from_config(client->producer, "control");
    producer_apply_playback_policy(client->producer);

    const gboolean render_device_changed =
        g_strcmp0(previous_render_device,
                  producer_effective_render_device(&client->producer->config)) != 0;
    const gboolean state_requires_rebind =
        header.opcode == VIVID_DISPLAY_CONTROL_SET_STATE &&
        control_set_state_requires_output_rebind(client->producer,
                                                 payload,
                                                 previous_display_prefs,
                                                 previous_multi_display_mode,
                                                 previous_primary_display_key,
                                                 previous_content_fit);
    if (state_requires_rebind && !render_device_changed)
        producer_rebind_all_outputs(client->producer);

    g_message("VividProducer: control opcode=%u changed=%s save=scheduled",
              header.opcode,
              changed ? "true" : "false");

    send_control_ack(client, header.opcode, TRUE, request_id);
    if (render_device_changed) {
        producer_disconnect_display_clients(client->producer, client);
    }
    vivid_producer_config_free_display_prefs_snapshot(previous_display_prefs);
    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_register_output(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return TRUE;

    const guint32 monitor_index =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_MONITOR_INDEX, 0);
    const guint32 consumer_output_id =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_CONSUMER_OUTPUT_ID, monitor_index + 1);
    const gint32 display_x =
        json_object_get_int_default(object, VIVID_JSON_REGISTER_OUTPUT_X, 0);
    const gint32 display_y =
        json_object_get_int_default(object, VIVID_JSON_REGISTER_OUTPUT_Y, 0);
    const guint32 width =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_WIDTH, 1280);
    const guint32 height =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_HEIGHT, 720);
    const gdouble scale =
        MAX(1.0, json_object_get_double_default(object, VIVID_JSON_REGISTER_OUTPUT_SCALE, 1.0));
    guint32 physical_width =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_PHYSICAL_WIDTH, 0);
    guint32 physical_height =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_PHYSICAL_HEIGHT, 0);
    if (physical_width == 0)
        physical_width = (guint32)MAX(1.0, floor((gdouble)width * scale + 0.5));
    if (physical_height == 0)
        physical_height = (guint32)MAX(1.0, floor((gdouble)height * scale + 0.5));
    const guint32 refresh_rate_mhz =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_REFRESH_RATE_MHZ, 0);
    const gchar* desktop =
        json_object_get_string_default(object, VIVID_JSON_REGISTER_OUTPUT_DESKTOP, "");
    const gchar* display_key =
        json_object_get_string_default(object, VIVID_JSON_REGISTER_OUTPUT_DISPLAY_KEY, "");
    const gchar* display_name =
        json_object_get_string_default(object, VIVID_JSON_REGISTER_OUTPUT_DISPLAY_NAME, "");
    const guint32 layout_output_count =
        json_object_get_uint_default(object, VIVID_JSON_REGISTER_OUTPUT_LAYOUT_OUTPUT_COUNT, 0);
    if (layout_output_count > 0)
        client->declared_output_count = layout_output_count;

    if (!client->dmabuf_caps.present) {
        g_warning("VividProducer: refusing REGISTER_OUTPUT before dmabufCaps.version=3");
        send_display_error(client,
                           VIVID_DISPLAY_ERR_CAPS_REQUIRED,
                           FALSE,
                           "protocol error: send valid dmabufCaps.version=3 before REGISTER_OUTPUT");
        g_object_unref(parser);
        return TRUE;
    }

    Output* output = output_new(client,
                                consumer_output_id,
                                monitor_index,
                                display_x,
                                display_y,
                                width,
                                height,
                                physical_width,
                                physical_height,
                                scale,
                                refresh_rate_mhz,
                                desktop,
                                display_key,
                                display_name);
    output->register_transform =
        vivid_render_layout_transform_from_string(
            json_object_get_string_default(object, VIVID_JSON_REGISTER_OUTPUT_TRANSFORM, "normal"));
    g_ptr_array_add(client->outputs, output);
    producer_relayout_outputs(client->producer, TRUE);
    producer_apply_config_to_renderers(client->producer);
    producer_apply_playback_policy(client->producer);

    GError* error = NULL;
    const gboolean waiting_for_clone_primary =
        producer_clone_route_waiting_for_primary(client->producer) && !output->route;
    if (!output->active) {
        output->memory_kind = OUTPUT_MEMORY_RENDERER_DMABUF;
        output->needs_renderer_rebind = FALSE;
        output->needs_renderer_first_frame = FALSE;
        output->renderer_generation = 0;
        g_message("VividProducer: registered inactive output=%u "
                  "consumer-output=%u display-key=%s",
                  output->output_id,
                  output->consumer_output_id,
                  output->display_key && *output->display_key ? output->display_key : "(none)");
    } else if (waiting_for_clone_primary) {
        output->memory_kind = OUTPUT_MEMORY_RENDERER_DMABUF;
        output->needs_renderer_rebind = TRUE;
        output->next_renderer_retry_time_usec = 0;
        output->renderer_generation = 0;
        g_message("VividProducer: registered pending clone output=%u "
                  "consumer-output=%u while waiting for primary-display-key=%s",
                  output->output_id,
                  output->consumer_output_id,
                  client->producer->config.primary_display_key &&
                      *client->producer->config.primary_display_key
                      ? client->producer->config.primary_display_key
                      : "(auto)");
    } else {
        output->renderer_generation = vivid_producer_renderer_generation(output_renderer(output));
        OutputRebindPrepareState prepare_state = OUTPUT_REBIND_PREPARE_FAILED;
        if (vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(output)) &&
            producer_renderer_rebind_barrier_blocks(client->producer, output)) {
            g_message("VividProducer: registered pending renderer DMA-BUF output=%u "
                      "behind output=%u rebind barrier logical=%ux%u target=%ux%u "
                      "route=%ux%u scale=%.3f",
                      output->output_id,
                      client->producer->renderer_rebind_barrier_output_id,
                      output->logical_width,
                      output->logical_height,
                      output->target_width,
                      output->target_height,
                      output->width,
                      output->height,
                      output->scale);
            output->memory_kind = OUTPUT_MEMORY_RENDERER_DMABUF;
            output->needs_renderer_rebind = TRUE;
            output->next_renderer_retry_time_usec = 0;
        } else if (!output_rebuild_buffers(client->producer,
                                           output,
                                           FALSE,
                                           &prepare_state,
                                           &error)) {
            if (vivid_producer_renderer_prefers_dmabuf_buffers(output_renderer(output))) {
                if (prepare_state == OUTPUT_REBIND_PREPARE_NOT_READY)
                    producer_set_renderer_rebind_barrier(client->producer, output);
                else
                    producer_clear_renderer_rebind_barrier(client->producer, output);
                g_message("VividProducer: registered pending renderer DMA-BUF output=%u "
                          "logical=%ux%u target=%ux%u route=%ux%u scale=%.3f refresh=%u: %s",
                          output->output_id,
                          output->logical_width,
                          output->logical_height,
                          output->target_width,
                          output->target_height,
                          output->width,
                          output->height,
                          output->scale,
                          output->refresh_rate_mhz,
                          error ? error->message : "renderer buffers are not ready yet");
                g_clear_error(&error);
                output->memory_kind = OUTPUT_MEMORY_RENDERER_DMABUF;
                output->needs_renderer_rebind = TRUE;
                output->next_renderer_retry_time_usec = 0;
                output->renderer_generation = vivid_producer_renderer_generation(output_renderer(output));
            } else {
                g_warning("VividProducer: failed to allocate DMA-BUF output buffer: %s",
                          error ? error->message : "unknown buffer allocation error");
                g_ptr_array_remove(client->outputs, output);
                producer_relayout_outputs(client->producer, TRUE);
                g_autofree gchar* message =
                    g_strdup_printf("failed to allocate DMA-BUF output buffer: %s",
                                    error ? error->message : "unknown buffer allocation error");
                send_display_error(client,
                                   VIVID_DISPLAY_ERR_BUFFER_ALLOC_FAILED,
                                   FALSE,
                                   "%s",
                                   message);
                g_clear_error(&error);
                g_object_unref(parser);
                return TRUE;
            }
        } else {
            producer_clear_renderer_rebind_barrier(client->producer, output);
            output->needs_renderer_rebind = FALSE;
            if (output->memory_kind == OUTPUT_MEMORY_RENDERER_DMABUF)
                output->needs_renderer_first_frame = TRUE;
        }
    }

    g_message("VividProducer: registered monitor=%u consumer-output=%u output=%u "
              "logical=%ux%u target=%ux%u route=%ux%u scale=%.3f refresh=%u "
              "pos=%d,%d desktop=%s display-key=%s display=%s",
              monitor_index,
              consumer_output_id,
              output->output_id,
              output->logical_width,
              output->logical_height,
              output->target_width,
              output->target_height,
              output->width,
              output->height,
              output->scale,
              output->refresh_rate_mhz,
              output->display_x,
              output->display_y,
              output->desktop && *output->desktop ? output->desktop : "(unknown)",
              output->display_key && *output->display_key ? output->display_key : "(none)",
              output->display_name && *output->display_name ? output->display_name : "(unnamed)");

    send_output_accepted(client, output);
    if (!output->active) {
        g_object_unref(parser);
        return TRUE;
    }
    if (output->n_buffers > 0 && !send_bind_with_config(client, output)) {
        g_object_unref(parser);
        return FALSE;
    }

    producer_sync_route_frame_sources(client->producer);

    producer_replay_cached_window_facts_for_client(client);

    g_object_unref(parser);
    return TRUE;
}

static Output*
client_find_output(Client* client, guint32 output_id, guint32 consumer_output_id)
{
    for (guint i = 0; client && i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (output_id != 0 && output->output_id == output_id)
            return output;
        if (output_id == 0 &&
            consumer_output_id != 0 &&
            output->consumer_output_id == consumer_output_id)
            return output;
    }
    return NULL;
}

static gboolean
output_update_from_json(Output* output, JsonObject* object)
{
    if (!output || !object)
        return FALSE;

    const guint32 old_consumer_output_id = output->consumer_output_id;
    const guint32 old_monitor_index = output->monitor_index;
    const gint32 old_display_x = output->display_x;
    const gint32 old_display_y = output->display_y;
    const guint32 old_logical_width = output->logical_width;
    const guint32 old_logical_height = output->logical_height;
    const guint32 old_target_width = output->target_width;
    const guint32 old_target_height = output->target_height;
    const gdouble old_scale = output->scale;
    const guint32 old_refresh_rate_mhz = output->refresh_rate_mhz;
    g_autofree gchar* old_desktop = g_strdup(output->desktop ? output->desktop : "");
    g_autofree gchar* old_display_key =
        g_strdup(output->display_key ? output->display_key : "");
    g_autofree gchar* old_display_name =
        g_strdup(output->display_name ? output->display_name : "");

    output->monitor_index =
        json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_MONITOR_INDEX, output->monitor_index);
    if (json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_CONSUMER_OUTPUT_ID)) {
        output->consumer_output_id =
            json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_CONSUMER_OUTPUT_ID, output->consumer_output_id);
    }
    output->display_x =
        json_object_get_int_default(object, VIVID_JSON_UPDATE_OUTPUT_X, output->display_x);
    output->display_y =
        json_object_get_int_default(object, VIVID_JSON_UPDATE_OUTPUT_Y, output->display_y);
    output->logical_width =
        CLAMP(json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_WIDTH, output->logical_width),
              1u,
              OUTPUT_MAX_DIMENSION);
    output->logical_height =
        CLAMP(json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_HEIGHT, output->logical_height),
              1u,
              OUTPUT_MAX_DIMENSION);
    output->scale =
        MAX(1.0, json_object_get_double_default(object, VIVID_JSON_UPDATE_OUTPUT_SCALE, output->scale));

    guint32 physical_width =
        json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_PHYSICAL_WIDTH, 0);
    guint32 physical_height =
        json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_PHYSICAL_HEIGHT, 0);
    if (physical_width == 0 &&
        (json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_WIDTH) ||
         json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_SCALE))) {
        physical_width =
            (guint32)MAX(1.0, floor((gdouble)output->logical_width * output->scale + 0.5));
    }
    if (physical_height == 0 &&
        (json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_HEIGHT) ||
         json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_SCALE))) {
        physical_height =
            (guint32)MAX(1.0, floor((gdouble)output->logical_height * output->scale + 0.5));
    }
    if (physical_width != 0)
        output->target_width = CLAMP(physical_width, 1u, OUTPUT_MAX_DIMENSION);
    if (physical_height != 0)
        output->target_height = CLAMP(physical_height, 1u, OUTPUT_MAX_DIMENSION);

    output->refresh_rate_mhz =
        json_object_get_uint_default(object,
                                     VIVID_JSON_UPDATE_OUTPUT_REFRESH_RATE_MHZ,
                                     output->refresh_rate_mhz);
    if (json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_DESKTOP)) {
        g_free(output->desktop);
        output->desktop =
            g_strdup(json_object_get_string_default(object, VIVID_JSON_UPDATE_OUTPUT_DESKTOP, ""));
    }
    if (json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_DISPLAY_KEY)) {
        g_free(output->display_key);
        output->display_key =
            g_strdup(json_object_get_string_default(object, VIVID_JSON_UPDATE_OUTPUT_DISPLAY_KEY, ""));
    }
    if (json_object_has_member(object, VIVID_JSON_UPDATE_OUTPUT_DISPLAY_NAME)) {
        g_free(output->display_name);
        output->display_name =
            g_strdup(json_object_get_string_default(object, VIVID_JSON_UPDATE_OUTPUT_DISPLAY_NAME, ""));
    }

    return old_consumer_output_id != output->consumer_output_id ||
        old_monitor_index != output->monitor_index ||
        old_display_x != output->display_x ||
        old_display_y != output->display_y ||
        old_logical_width != output->logical_width ||
        old_logical_height != output->logical_height ||
        old_target_width != output->target_width ||
        old_target_height != output->target_height ||
        old_scale != output->scale ||
        old_refresh_rate_mhz != output->refresh_rate_mhz ||
        g_strcmp0(old_desktop, output->desktop) != 0 ||
        g_strcmp0(old_display_key, output->display_key) != 0 ||
        g_strcmp0(old_display_name, output->display_name) != 0;
}

static gboolean
handle_update_output(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object) {
        send_display_error(client,
                           VIVID_DISPLAY_ERR_UPDATE_OUTPUT_INVALID,
                           FALSE,
                           "UPDATE_OUTPUT payload is not valid JSON");
        return TRUE;
    }

    const guint32 output_id =
        json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_OUTPUT_ID, 0);
    const guint32 update_monitor_index =
        json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_MONITOR_INDEX, G_MAXUINT32);
    const guint32 update_consumer_output_id =
        json_object_get_uint_default(object, VIVID_JSON_UPDATE_OUTPUT_CONSUMER_OUTPUT_ID, 0);
    const guint32 consumer_output_id =
        update_consumer_output_id != 0
            ? update_consumer_output_id
            : (update_monitor_index != G_MAXUINT32 ? update_monitor_index + 1 : 0);
    Output* output = client_find_output(client, output_id, consumer_output_id);
    if (!output) {
        send_display_error(client,
                           VIVID_DISPLAY_ERR_UPDATE_OUTPUT_INVALID,
                           FALSE,
                           "UPDATE_OUTPUT target not found output=%u consumer-output=%u",
                           output_id,
                           consumer_output_id);
        g_object_unref(parser);
        return TRUE;
    }

    const gboolean changed = output_update_from_json(output, object);
    producer_relayout_outputs(client->producer, changed);
    if (changed) {
        producer_apply_config_to_renderers(client->producer);
        producer_apply_playback_policy(client->producer);
        output->needs_renderer_rebind = TRUE;
        output->next_renderer_retry_time_usec = 0;
    }

    g_message("VividProducer: updated output=%u consumer-output=%u changed=%s "
              "active=%s route-id=%u needs-rebind=%s monitor=%u "
              "logical=%ux%u target=%ux%u route=%ux%u scale=%.3f refresh=%u "
              "pos=%d,%d desktop=%s display-key=%s display=%s",
              output->output_id,
              output->consumer_output_id,
              changed ? "true" : "false",
              output->active ? "true" : "false",
              output->route ? output->route->route_id : 0,
              output->needs_renderer_rebind ? "true" : "false",
              output->monitor_index,
              output->logical_width,
              output->logical_height,
              output->target_width,
              output->target_height,
              output->width,
              output->height,
              output->scale,
              output->refresh_rate_mhz,
              output->display_x,
              output->display_y,
              output->desktop && *output->desktop ? output->desktop : "(unknown)",
              output->display_key && *output->display_key ? output->display_key : "(none)",
              output->display_name && *output->display_name
                  ? output->display_name
                  : "(unnamed)");

    producer_replay_cached_window_facts_for_client(client);

    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_pointer_event(Client* client, guint16 opcode, const guint8* body, gsize body_len)
{
    guint32 event_output_id = body_len >= 4 ? vivid_display_codec_read_u32(body) : 0;
    gdouble pointer_x = 0.0;
    gdouble pointer_y = 0.0;
    guint32 button = 0;
    guint32 button_state = 0;
    gdouble axis_dx = 0.0;
    gdouble axis_dy = 0.0;
    uint64_t ignored_time = 0;

    if (opcode == VIVID_DISPLAY_REQ_POINTER_MOTION &&
        body_len == VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES &&
        vivid_display_pointer_motion_body_read(body,
                                                body_len,
                                                &event_output_id,
                                                &pointer_x,
                                                &pointer_y,
                                                &ignored_time) < 0) {
        return TRUE;
    }

    if (opcode == VIVID_DISPLAY_REQ_POINTER_BUTTON &&
        body_len == VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES &&
        vivid_display_pointer_button_body_read(body,
                                                body_len,
                                                &event_output_id,
                                                &pointer_x,
                                                &pointer_y,
                                                &button,
                                                &button_state,
                                                &ignored_time) < 0) {
        return TRUE;
    }

    if (opcode == VIVID_DISPLAY_REQ_POINTER_AXIS &&
        body_len == VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES) {
        guint32 axis_source = 0;
        if (vivid_display_pointer_axis_body_read(body,
                                                  body_len,
                                                  &event_output_id,
                                                  &pointer_x,
                                                  &pointer_y,
                                                  &axis_dx,
                                                  &axis_dy,
                                                  &axis_source,
                                                  &ignored_time) < 0)
            return TRUE;
    }

    Output* event_output = NULL;
    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (output->output_id == event_output_id) {
            event_output = output;
            break;
        }
    }
    if (event_output && !event_output->active)
        return TRUE;
    VividProducerRenderer* renderer =
        event_output ? output_renderer(event_output) : client->producer->renderer;
    if (!renderer)
        return TRUE;

    if (event_output) {
        const RouteLink* link =
            route_link_for_output(&client->producer->route_table, event_output);
        const VividRenderRect source = link ? link->src_rect : event_output->source_rect;
        const VividRenderRect dest = link ? link->dst_rect : event_output->dest_rect;
        const guint32 transform = link ? link->transform : 0;
        vivid_render_layout_display_point_to_texture(pointer_x,
                                                     pointer_y,
                                                     transform,
                                                     &source,
                                                     &dest,
                                                     &pointer_x,
                                                     &pointer_y);
    }

    if (opcode == VIVID_DISPLAY_REQ_POINTER_MOTION &&
        body_len == VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES) {
        vivid_producer_renderer_pointer_motion(renderer, pointer_x, pointer_y);
        return TRUE;
    }

    if (opcode == VIVID_DISPLAY_REQ_POINTER_BUTTON &&
        body_len == VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES) {
        vivid_producer_renderer_pointer_motion(renderer, pointer_x, pointer_y);
        vivid_producer_renderer_pointer_button(renderer,
                                                button,
                                                button_state == VIVID_DISPLAY_BUTTON_PRESSED);
        g_debug("VividProducer: pointer button output=%u button=%u state=%u",
                event_output_id,
                button,
                button_state);
        return TRUE;
    }

    if (opcode == VIVID_DISPLAY_REQ_POINTER_AXIS &&
        body_len == VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES) {
        vivid_producer_renderer_pointer_motion(renderer, pointer_x, pointer_y);
        vivid_producer_renderer_pointer_axis(renderer, axis_dx, axis_dy);
        return TRUE;
    }

    g_warning("VividProducer: invalid pointer event opcode=%u body-len=%" G_GSIZE_FORMAT,
              opcode,
              body_len);
    return TRUE;
}

static gboolean
handle_hello(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return client_protocol_error(client,
                                     VIVID_DISPLAY_ERR_HELLO_REQUIRED,
                                     "HELLO payload must be a JSON object");

    const gchar* protocol =
        json_object_get_string_default(object, VIVID_JSON_HELLO_PROTOCOL, NULL);
    if (g_strcmp0(protocol, VIVID_DISPLAY_PROTOCOL_NAME) != 0) {
        g_object_unref(parser);
        return client_protocol_error(client,
                                     VIVID_DISPLAY_ERR_PROTO_NAME_MISMATCH,
                                     "HELLO protocol mismatch got='%s' expected='%s'",
                                     protocol ? protocol : "(missing)",
                                     VIVID_DISPLAY_PROTOCOL_NAME);
    }

    guint64 version = 0;
    if (!json_value_to_uint64(json_object_get_member(object, VIVID_JSON_HELLO_VERSION), &version) ||
        version < VIVID_DISPLAY_PROTOCOL_MIN_SUPPORTED ||
        version > VIVID_DISPLAY_PROTOCOL_VERSION) {
        g_object_unref(parser);
        send_display_error(client,
                           VIVID_DISPLAY_ERR_VERSION_UNSUPPORTED,
                           TRUE,
                           "HELLO version unsupported got=%" G_GUINT64_FORMAT
                           " supported=%u..%u",
                           version,
                           VIVID_DISPLAY_PROTOCOL_MIN_SUPPORTED,
                           VIVID_DISPLAY_PROTOCOL_VERSION);
        return FALSE;
    }

    const gchar* role_text =
        json_object_get_string_default(object, VIVID_JSON_HELLO_ROLE, NULL);
    client->role = client_role_from_text(role_text);
    if (client->role == 0) {
        g_object_unref(parser);
        return client_protocol_error(client,
                                     VIVID_DISPLAY_ERR_HELLO_REQUIRED,
                                     "HELLO role is missing or unsupported: %s",
                                     role_text ? role_text : "(missing)");
    }

    client->negotiated_version =
        (guint32)MIN(version, (guint64)VIVID_DISPLAY_PROTOCOL_VERSION);
    const gchar* client_name =
        json_object_get_string_default(object, VIVID_JSON_HELLO_CLIENT_NAME, "(unknown)");
    client->protocol_state = CLIENT_PROTOCOL_READY;
    g_message("VividProducer: client hello name=%s role=%s protocol=%s version=%"
              G_GUINT64_FORMAT " negotiated=%u",
              client_name,
              role_text,
              protocol,
              version,
              client->negotiated_version);
    g_object_unref(parser);
    return send_welcome(client, client->negotiated_version);
}

static gboolean
handle_consumer_caps(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return TRUE;

    GError* parse_error = NULL;
    if (!consumer_dmabuf_caps_parse(&client->dmabuf_caps, object, &parse_error)) {
        g_warning("VividProducer: rejected consumer caps: %s",
                  parse_error ? parse_error->message : "unknown caps parse error");
        consumer_dmabuf_caps_clear(&client->dmabuf_caps);
        consumer_dmabuf_caps_init(&client->dmabuf_caps);
        send_display_error(client,
                           VIVID_DISPLAY_ERR_CAPS_INVALID,
                           FALSE,
                           "%s",
                           parse_error ? parse_error->message
                                       : "invalid consumer DMA-BUF caps");
        g_clear_error(&parse_error);
        g_object_unref(parser);
        return TRUE;
    }
    const ConsumerDmaBufCaps* caps = &client->dmabuf_caps;
    g_message("VividProducer: consumer caps backend=%s probe=%s render-node=%s "
              "drm-render=%u:%u extentMax=%ux%u fourccs=%u modifiers=%u "
              "implicit-linear=%u hints=[host-visible:%s device-local:%s "
              "implicit-linear:%s] skips-external-only=%s",
              caps->backend && *caps->backend ? caps->backend : "(unknown)",
              caps->probe && *caps->probe ? caps->probe : "(unknown)",
              caps->render_node && *caps->render_node ? caps->render_node : "(unknown)",
              caps->peer_caps.identity.drm_render_major,
              caps->peer_caps.identity.drm_render_minor,
              caps->peer_caps.extent_max_w,
              caps->peer_caps.extent_max_h,
              caps->fourccs ? caps->fourccs->len : 0,
              caps->modifiers ? caps->modifiers->len : 0,
              caps->implicit_linear_fourccs ? caps->implicit_linear_fourccs->len : 0,
              caps->hint_host_visible ? "true" : "false",
              caps->hint_device_local ? "true" : "false",
              caps->hint_implicit_linear ? "true" : "false",
              caps->skips_external_only_modifiers ? "true" : "false");

    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        output->needs_renderer_rebind = TRUE;
        output->next_renderer_retry_time_usec = 0;
    }
    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (producer_renderer_rebind_barrier_blocks(client->producer, output))
            break;
        const OutputRebindResult result = client_rebind_output(client, output);
        if (result == OUTPUT_REBIND_RESULT_NOT_READY)
            break;
    }

    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_bind_failed(Client* client, const guint8* body, gsize body_len)
{
    if (!client->dmabuf_caps.present) {
        g_warning("VividProducer: ignoring BIND_FAILED before valid dmabufCaps.version=%u",
                  DMABUF_CAPS_VERSION);
        send_display_error(client,
                           VIVID_DISPLAY_ERR_CAPS_REQUIRED,
                           FALSE,
                           "protocol error: BIND_FAILED requires prior dmabufCaps.version=3");
        return TRUE;
    }

    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return TRUE;

    guint64 raw_fourcc = 0;
    guint64 modifier = DRM_FORMAT_MOD_LINEAR;
    guint64 generation = 0;
    const gboolean have_fourcc =
        json_value_to_uint64(json_object_get_member(object, VIVID_JSON_BIND_FAILED_FOURCC), &raw_fourcc);
    const gboolean have_modifier =
        json_value_to_uint64(json_object_get_member(object, VIVID_JSON_BIND_FAILED_MODIFIER), &modifier);
    const gboolean have_generation =
        json_value_to_uint64(json_object_get_member(object, VIVID_JSON_BIND_FAILED_GENERATION), &generation);
    if (modifier == DRM_FORMAT_MOD_INVALID)
        modifier = DRM_FORMAT_MOD_LINEAR;

    const guint32 output_id = json_object_get_uint_default(object, VIVID_JSON_BIND_FAILED_OUTPUT_ID, 0);
    const guint32 reason = json_object_get_uint_default(object, VIVID_JSON_BIND_FAILED_REASON, 0);
    const gchar* message = json_object_get_string_default(object, VIVID_JSON_BIND_FAILED_MESSAGE, "");

    if (!have_fourcc ||
        !have_modifier ||
        raw_fourcc == 0 ||
        raw_fourcc > G_MAXUINT32) {
        g_warning("VividProducer: invalid BIND_FAILED payload fourcc=%" G_GUINT64_FORMAT
                  " modifier=0x%016" G_GINT64_MODIFIER "x output=%u generation=%"
                  G_GUINT64_FORMAT " message=%s",
                  raw_fourcc,
                  (guint64)modifier,
                  output_id,
                  generation,
                  message && *message ? message : "(none)");
        g_object_unref(parser);
        send_display_error(client,
                           VIVID_DISPLAY_ERR_BIND_FAILED_INVALID,
                           FALSE,
                           "invalid BIND_FAILED payload");
        return TRUE;
    }

    /*
     * BIND_FAILED is a generation-scoped import verdict. Consumers can still be
     * processing an old BIND_BUFFERS message while the producer has already
     * switched to a newer generation; treating that stale verdict as a live
     * blacklist entry would poison the next negotiation and can create an
     * unnecessary retry loop.
     */
    gboolean targets_live_output = FALSE;
    gboolean saw_matching_output = FALSE;
    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (output_id != 0 && output->output_id != output_id)
            continue;
        saw_matching_output = TRUE;
        if (!have_generation || generation == output->generation) {
            targets_live_output = TRUE;
            break;
        }
    }

    if (!targets_live_output) {
        g_message("VividProducer: ignoring stale BIND_FAILED output=%u generation=%"
                  G_GUINT64_FORMAT " fourcc=0x%08x modifier=0x%016"
                  G_GINT64_MODIFIER
                  "x reason=%u matching-output=%s message=%s",
                  output_id,
                  generation,
                  (guint32)raw_fourcc,
                  (guint64)modifier,
                  reason,
                  saw_matching_output ? "true" : "false",
                  message && *message ? message : "(none)");
        g_object_unref(parser);
        return TRUE;
    }

    const guint32 fourcc = (guint32)raw_fourcc;
    const gboolean inserted =
        vivid_dmabuf_peer_caps_blacklist_modifier(&client->dmabuf_caps.peer_caps,
                                                  fourcc,
                                                  modifier);
    g_message("VividProducer: consumer BIND_FAILED output=%u generation=%"
              G_GUINT64_FORMAT " fourcc=0x%08x "
              "modifier=0x%016" G_GINT64_MODIFIER "x reason=%u [%s] inserted-blacklist=%s "
              "message=%s",
              output_id,
              generation,
              fourcc,
              (guint64)modifier,
              reason,
              vivid_display_bind_failed_reason_name(reason)
                  ? vivid_display_bind_failed_reason_name(reason)
                  : "UNKNOWN",
              inserted ? "true" : "false",
              message && *message ? message : "(none)");

    for (guint i = 0; i < client->outputs->len; i++) {
        Output* output = g_ptr_array_index(client->outputs, i);
        if (output_id != 0 && output->output_id != output_id)
            continue;
        if (have_generation && output->generation != generation)
            continue;
        output->needs_renderer_rebind = TRUE;
        output->next_renderer_retry_time_usec = 0;
        if (producer_renderer_rebind_barrier_blocks(client->producer, output))
            break;
        const OutputRebindResult result = client_rebind_output(client, output);
        if (result == OUTPUT_REBIND_RESULT_FAILED) {
            g_autofree gchar* error =
                g_strdup_printf("BIND_FAILED retry has no usable DMA-BUF scheme for output=%u "
                                "after blacklisting fourcc=0x%08x modifier=0x%016"
                                G_GINT64_MODIFIER "x",
                                output->output_id,
                                fourcc,
                                (guint64)modifier);
            send_display_error(client,
                               VIVID_DISPLAY_ERR_NO_DMABUF_SCHEME,
                               FALSE,
                               "%s",
                               error);
        } else if (result == OUTPUT_REBIND_RESULT_NOT_READY) {
            break;
        }
    }

    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_unbind_done(Client* client, const guint8* body, gsize body_len)
{
    JsonParser* parser = NULL;
    JsonObject* object = parse_json_object(body, body_len, &parser);
    if (!object)
        return TRUE;

    guint64 generation = 0;
    json_value_to_uint64(json_object_get_member(object, VIVID_JSON_UNBIND_DONE_GENERATION), &generation);
    const guint32 output_id = json_object_get_uint_default(object, VIVID_JSON_UNBIND_DONE_OUTPUT_ID, 0);
    const VividUnbindAckResult ack_result =
        vivid_unbind_ack_tracker_ack(&client->unbind_acks, output_id, generation);
    if (ack_result == VIVID_UNBIND_ACK_RESULT_MATCHED) {
        g_message("VividProducer: consumer UNBIND_DONE matched output=%u generation=%"
                  G_GUINT64_FORMAT " remaining=%u",
                  output_id,
                  generation,
                  vivid_unbind_ack_tracker_count(&client->unbind_acks));
    } else {
        g_message("VividProducer: consumer UNBIND_DONE stale/unknown output=%u "
                  "generation=%" G_GUINT64_FORMAT " pending=%u",
                  output_id,
                  generation,
                  vivid_unbind_ack_tracker_count(&client->unbind_acks));
    }

    g_object_unref(parser);
    return TRUE;
}

static gboolean
handle_frame(Client* client, guint16 opcode, const guint8* body, gsize body_len)
{
    if (client->protocol_state == CLIENT_PROTOCOL_EXPECT_HELLO &&
        opcode != VIVID_DISPLAY_REQ_HELLO) {
        return client_protocol_error(client,
                                     VIVID_DISPLAY_ERR_HELLO_REQUIRED,
                                     "first request must be HELLO, got opcode=%u",
                                     opcode);
    }
    if (client->protocol_state == CLIENT_PROTOCOL_READY &&
        opcode == VIVID_DISPLAY_REQ_HELLO) {
        return client_protocol_error(client,
                                     VIVID_DISPLAY_ERR_DUPLICATE_HELLO,
                                     "duplicate HELLO is not allowed");
    }

    switch (opcode) {
    case VIVID_DISPLAY_REQ_HELLO:
        return handle_hello(client, body, body_len);
    case VIVID_DISPLAY_REQ_REGISTER_OUTPUT:
        return handle_register_output(client, body, body_len);
    case VIVID_DISPLAY_REQ_UPDATE_OUTPUT:
        return handle_update_output(client, body, body_len);
    case VIVID_DISPLAY_REQ_CONSUMER_CAPS:
        return handle_consumer_caps(client, body, body_len);
    case VIVID_DISPLAY_REQ_WINDOW_STATE:
        return handle_window_state(client, body, body_len);
    case VIVID_DISPLAY_REQ_MEDIA_STATE:
        return handle_media_state(client, body, body_len);
    case VIVID_DISPLAY_REQ_AUDIO_SAMPLES:
        return TRUE;
    case VIVID_DISPLAY_REQ_AUDIO_SAMPLES_BIN:
        return handle_audio_samples_bin(client, body, body_len);
    case VIVID_DISPLAY_REQ_CONTROL:
        return handle_control(client, body, body_len);
    case VIVID_DISPLAY_REQ_BIND_FAILED:
        return handle_bind_failed(client, body, body_len);
    case VIVID_DISPLAY_REQ_UNBIND_DONE:
        return handle_unbind_done(client, body, body_len);
    case VIVID_DISPLAY_REQ_POINTER_MOTION:
    case VIVID_DISPLAY_REQ_POINTER_BUTTON:
    case VIVID_DISPLAY_REQ_POINTER_AXIS:
        return handle_pointer_event(client, opcode, body, body_len);
    case VIVID_DISPLAY_REQ_BYE:
        return FALSE;
    default:
        g_debug("VividProducer: ignored request opcode=%u", opcode);
        return TRUE;
    }
}

static gboolean
client_fd_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    Client* client = user_data;

    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0)
        goto closed;

    for (;;) {
        const gint result = vivid_display_recv_frame_nonblocking(fd, &client->recv_state);
        if (result == VIVID_DISPLAY_CODEC_FRAME_NEED_IO)
            return G_SOURCE_CONTINUE;

        if (result == VIVID_DISPLAY_CODEC_FRAME_DONE) {
            const gboolean keep =
                handle_frame(client,
                             client->recv_state.opcode,
                             client->recv_state.body,
                             client->recv_state.body_len);
            vivid_display_recv_state_clear(&client->recv_state);
            if (!keep)
                goto closed;
            continue;
        }

        g_warning("VividProducer: client protocol error: %s",
                  g_strerror(result < 0 ? -result : result));
        goto closed;
    }

closed:
    client->source_id = 0;
    client_free(client);
    return G_SOURCE_REMOVE;
}

static Client*
client_new(Producer* producer, gint fd)
{
    Client* client = g_new0(Client, 1);
    client->producer = producer;
    client->fd = fd;
    client->outputs = g_ptr_array_new_with_free_func((GDestroyNotify)output_free);
    consumer_dmabuf_caps_init(&client->dmabuf_caps);
    vivid_unbind_ack_tracker_init(&client->unbind_acks);
    g_queue_init(&client->deferred_frames);
    g_queue_init(&client->outbox);
    client->negotiated_version = VIVID_DISPLAY_PROTOCOL_VERSION;
    vivid_display_recv_state_init(&client->recv_state);
    set_fd_nonblocking(fd);

    /*
     * Pointer input reaches the scene renderer through this same client socket.
     * The display frame clock is intentionally high priority so FRAME_READY
     * cadence stays stable, but leaving the socket reader at GLib's default
     * priority lets steady frame ticks repeatedly run before pending pointer
     * packets. Under fast mouse movement that turns parallax into a queue of
     * stale positions. Keep socket reads at the frame-clock priority so input
     * and presentation progress together instead of one starving the other.
     */
    GSource* source = g_unix_fd_source_new(fd,
                                           G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL);
    g_source_set_priority(source, CLIENT_SOCKET_SOURCE_PRIORITY);
    g_source_set_callback(source, (GSourceFunc)client_fd_ready, client, NULL);
    g_source_set_name(source, "vivid-producer-client-socket");
    client->source_id = g_source_attach(source, NULL);
    g_source_unref(source);
    g_ptr_array_add(producer->clients, client);
    return client;
}

static void
client_free(Client* client)
{
    if (!client)
        return;

    Producer* producer = client->producer;
    if (producer) {
        for (guint i = 0; i < producer->clients->len; i++) {
            if (g_ptr_array_index(producer->clients, i) == client) {
                g_ptr_array_remove_index_fast(producer->clients, i);
                break;
            }
        }
    }

    if (client->source_id != 0)
        g_source_remove(client->source_id);
    if (client->write_source_id != 0)
        g_source_remove(client->write_source_id);

    client_outbox_clear(client);
    vivid_display_recv_state_clear(&client->recv_state);
    consumer_dmabuf_caps_clear(&client->dmabuf_caps);
    vivid_unbind_ack_tracker_clear(&client->unbind_acks);
    client_clear_deferred_frames(client);
    client_clear_cached_window_facts(client);
    if (producer && producer->route_table.links) {
        for (guint i = 0; i < client->outputs->len; i++) {
            Output* output = g_ptr_array_index(client->outputs, i);
            route_link_detach_output(&producer->route_table, output);
        }
    }
    g_ptr_array_unref(client->outputs);
    if (producer) {
        producer_prune_unused_routes(producer);
        producer_sync_route_frame_sources(producer);
    }
    if (client->fd >= 0)
        close(client->fd);

    g_message("VividProducer: client disconnected");
    g_free(client);
}

static gboolean
producer_accept_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    Producer* producer = user_data;

    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0)
        return G_SOURCE_REMOVE;

    for (;;) {
        const gint client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return G_SOURCE_CONTINUE;
            if (errno == EINTR)
                continue;
            g_warning("VividProducer: accept failed: %s", g_strerror(errno));
            return G_SOURCE_CONTINUE;
        }

        client_new(producer, client_fd);
        g_message("VividProducer: client connected");
    }
}

static gboolean
producer_start(Producer* producer)
{
    g_autofree gchar* socket_dir = g_path_get_dirname(producer->socket_path);
    if (g_mkdir_with_parents(socket_dir, 0700) < 0) {
        g_warning("VividProducer: failed to create socket dir %s: %s",
                  socket_dir,
                  g_strerror(errno));
        return FALSE;
    }

    g_clear_pointer(&producer->lock_path, g_free);
    producer->lock_path = g_strdup_printf("%s.lock", producer->socket_path);
    producer->lock_fd = open(producer->lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (producer->lock_fd < 0) {
        g_warning("VividProducer: failed to open singleton lock %s: %s",
                  producer->lock_path,
                  g_strerror(errno));
        return FALSE;
    }
    if (flock(producer->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        const gint lock_error = errno;
        g_warning("VividProducer: another producer already owns %s: %s",
                  producer->socket_path,
                  g_strerror(lock_error));
        close(producer->lock_fd);
        producer->lock_fd = -1;
        return FALSE;
    }

    unlink(producer->socket_path);

    producer->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (producer->listen_fd < 0) {
        g_warning("VividProducer: socket failed: %s", g_strerror(errno));
        return FALSE;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(producer->socket_path) >= sizeof(address.sun_path)) {
        g_warning("VividProducer: socket path is too long: %s", producer->socket_path);
        return FALSE;
    }
    g_strlcpy(address.sun_path, producer->socket_path, sizeof(address.sun_path));

    if (bind(producer->listen_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        g_warning("VividProducer: bind %s failed: %s",
                  producer->socket_path,
                  g_strerror(errno));
        return FALSE;
    }
    producer->socket_bound = TRUE;

    if (listen(producer->listen_fd, 16) < 0) {
        g_warning("VividProducer: listen failed: %s", g_strerror(errno));
        return FALSE;
    }

    producer->accept_source_id =
        g_unix_fd_add(producer->listen_fd,
                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                      producer_accept_ready,
                      producer);

    g_message("VividProducer: listening at %s fps=%u interval=%" G_GUINT64_FORMAT "us",
              producer->socket_path,
              producer->fps,
              producer_frame_interval_usec(producer));
    return TRUE;
}

static void
producer_stop(Producer* producer)
{
    while (producer->clients->len > 0)
        client_free(g_ptr_array_index(producer->clients, 0));

    if (producer->accept_source_id != 0) {
        g_source_remove(producer->accept_source_id);
        producer->accept_source_id = 0;
    }

    if (producer->listen_fd >= 0) {
        close(producer->listen_fd);
        producer->listen_fd = -1;
    }

    if (producer->socket_bound && producer->socket_path) {
        unlink(producer->socket_path);
        producer->socket_bound = FALSE;
    }

    if (producer->lock_fd >= 0) {
        flock(producer->lock_fd, LOCK_UN);
        close(producer->lock_fd);
        producer->lock_fd = -1;
    }
    g_clear_pointer(&producer->lock_path, g_free);

    vivid_producer_config_flush_save(&producer->config);
}

static gboolean
quit_on_signal(gpointer user_data)
{
    Producer* producer = user_data;
    g_main_loop_quit(producer->loop);
    return G_SOURCE_REMOVE;
}

int
main(int argc, char** argv)
{
    /*
     * Renderer modules run in-process and may use libraries whose internal
     * wakeup channels are ordinary Unix pipes. PulseAudio's mainloop can
     * briefly write to such a pipe while another thread is tearing it down;
     * EPIPE is recoverable, but the default SIGPIPE disposition would kill
     * the entire producer and disconnect every display. Socket sends still
     * use MSG_NOSIGNAL so their normal error handling remains unchanged.
     */
    struct sigaction ignore_sigpipe = { .sa_handler = SIG_IGN };
    sigemptyset(&ignore_sigpipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_sigpipe, NULL) < 0) {
        g_printerr("VividProducer: failed to ignore SIGPIPE: %s\n", g_strerror(errno));
        return 1;
    }

    Producer producer = {
        .lock_fd = -1,
        .listen_fd = -1,
        .render_fd = -1,
        .fps = DEFAULT_FPS,
        .next_output_id = 1,
        .user_playing = TRUE,
    };
    producer.socket_path = default_socket_path();
    producer.clients = g_ptr_array_new();
    producer.release_gate_contexts =
        g_ptr_array_new_with_free_func(render_route_release_gate_context_free);
    producer.routes = g_ptr_array_new_with_free_func((GDestroyNotify)render_route_free);
    vivid_route_table_init(&producer.route_table);
    g_mutex_init(&producer.release_lock);
    gchar* config_path = NULL;
    gint cli_fps = 0;

    GOptionEntry entries[] = {
        {"socket", 0, 0, G_OPTION_ARG_FILENAME, &producer.socket_path,
         "Unix socket path", "PATH"},
        {"config", 0, 0, G_OPTION_ARG_FILENAME, &config_path,
         "Producer JSON config path", "PATH"},
        {"fps", 0, 0, G_OPTION_ARG_INT, &cli_fps,
         "Frame-ready notification rate", "FPS"},
        {NULL},
    };

    GError* error = NULL;
    GOptionContext* context = g_option_context_new("- Vivid display producer");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("VividProducer: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        vivid_route_table_clear(&producer.route_table);
        g_ptr_array_unref(producer.routes);
        g_ptr_array_unref(producer.clients);
        g_ptr_array_unref(producer.release_gate_contexts);
        g_mutex_clear(&producer.release_lock);
        g_free(producer.socket_path);
        return 1;
    }
    g_option_context_free(context);

    producer.fps = DEFAULT_FPS;
    vivid_producer_config_init(&producer.config, config_path);
    g_free(config_path);
    vivid_producer_config_load(&producer.config);
    vivid_producer_config_save(&producer.config);
    if (cli_fps > 0)
        producer_set_frame_rate(&producer, (guint)cli_fps, "cli");
    else
        producer_sync_frame_rate_from_config(&producer, "config");
    g_message("VividProducer: config path %s", producer.config.config_path);

    producer.renderer = vivid_producer_renderer_new();
    producer_apply_config_to_global_renderer(&producer);
    producer_apply_playback_policy(&producer);

    producer.loop = g_main_loop_new(NULL, FALSE);

    if (!producer_open_dmabuf_allocator(&producer)) {
        vivid_route_table_clear(&producer.route_table);
        g_ptr_array_unref(producer.routes);
        vivid_producer_renderer_free(producer.renderer);
        g_ptr_array_unref(producer.release_gate_contexts);
        g_main_loop_unref(producer.loop);
        g_ptr_array_unref(producer.clients);
        g_mutex_clear(&producer.release_lock);
        vivid_producer_config_clear(&producer.config);
        g_free(producer.socket_path);
        return 1;
    }

    if (!producer_release_coordinator_start(&producer)) {
        vivid_route_table_clear(&producer.route_table);
        g_ptr_array_unref(producer.routes);
        producer_close_dmabuf_allocator(&producer);
        vivid_producer_renderer_free(producer.renderer);
        g_ptr_array_unref(producer.release_gate_contexts);
        g_main_loop_unref(producer.loop);
        g_ptr_array_unref(producer.clients);
        g_mutex_clear(&producer.release_lock);
        vivid_producer_config_clear(&producer.config);
        g_free(producer.socket_path);
        return 1;
    }

    if (!producer_start(&producer)) {
        producer_stop(&producer);
        vivid_route_table_clear(&producer.route_table);
        g_ptr_array_unref(producer.routes);
        producer_release_coordinator_stop(&producer);
        producer_close_dmabuf_allocator(&producer);
        vivid_producer_renderer_free(producer.renderer);
        g_ptr_array_unref(producer.release_gate_contexts);
        g_main_loop_unref(producer.loop);
        g_ptr_array_unref(producer.clients);
        g_mutex_clear(&producer.release_lock);
        vivid_producer_config_clear(&producer.config);
        g_free(producer.socket_path);
        return 1;
    }

    g_unix_signal_add(SIGINT, quit_on_signal, &producer);
    g_unix_signal_add(SIGTERM, quit_on_signal, &producer);

    g_main_loop_run(producer.loop);

    producer_stop(&producer);
    vivid_route_table_clear(&producer.route_table);
    g_clear_pointer(&producer.routes, g_ptr_array_unref);
    producer_release_coordinator_stop(&producer);
    producer_close_dmabuf_allocator(&producer);
    vivid_producer_renderer_free(producer.renderer);
    g_clear_pointer(&producer.release_gate_contexts, g_ptr_array_unref);
    g_main_loop_unref(producer.loop);
    g_clear_pointer(&producer.clients, g_ptr_array_unref);
    g_mutex_clear(&producer.release_lock);
    vivid_producer_config_clear(&producer.config);
    g_free(producer.socket_path);
    return 0;
}
