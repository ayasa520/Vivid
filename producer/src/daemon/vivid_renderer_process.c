#include "vivid_renderer_process.h"

#include <gio/gio.h>
#include <glib-unix.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct
{
    guint16 opcode;
    guint64 request_id;
    guint8* payload;
    size_t payload_length;
} VividRendererOutboxItem;

typedef struct
{
    VividRendererProcessFrame frame;
} VividRendererQueuedFrame;

struct _VividRendererProcess
{
    const VividRendererDescriptor* descriptor;
    gchar* route_id;
    gchar* identity_hash;
    guint64 instance_id;
    pid_t pid;
    gint pidfd;
    pid_t process_group;
    gint socket_fd;
    gint log_fd;
    gboolean reaped;
    gboolean top_reaped;
    gint wait_status;
    gboolean was_active;
    gboolean termination_sent;
    gboolean kill_sent;

    VividRendererProcessState state;
    gint64 state_entered_usec;
    guint64 next_request_id;
    guint64 init_request_id;
    guint64 negotiate_request_id;
    guint64 quiesce_request_id;
    guint64 shutdown_request_id;
    gboolean hello_seen;
    gboolean ready_seen;
    gboolean format_caps_seen;
    gboolean release_timeline_seen;
    gboolean bind_buffers_seen;
    gboolean first_frame_seen;
    guint8 expected_device_uuid[VIVID_RENDERER_UUID_BYTES];
    gchar* expected_render_node;
    guint8 actual_device_uuid[VIVID_RENDERER_UUID_BYTES];
    guint8 actual_driver_uuid[VIVID_RENDERER_UUID_BYTES];
    guint32 actual_drm_render_major;
    guint32 actual_drm_render_minor;
    gchar* actual_render_node;
    VividRendererProcessFormatCaps format_caps;
    VividRendererProcessPool pool;
    gint timeline_fd;
    gint timeline_drm_fd;
    guint32 timeline_handle;
    GQueue frames;
    guint64 last_frame_sequence;
    guint64 last_release_point;
    GMutex timeline_lock;
    gchar* last_error;

    GMainContext* context;
    GSource* socket_source;
    GSource* write_source;
    GSource* pidfd_source;
    GSource* log_source;
    GSource* deadline_source;
    GSource* group_reap_source;
    gint64 group_reap_log_deadline_usec;
    GQueue outbox;
    GString* partial_log_line;
    VividRendererLifecyclePolicy policy;
    VividRendererProcessObserver observer;
    gpointer observer_data;
};

static void process_fail(VividRendererProcess* process, const gchar* reason);
static gboolean process_socket_ready(gint fd,
                                     GIOCondition condition,
                                     gpointer user_data);
static gboolean process_socket_writable(gint fd,
                                        GIOCondition condition,
                                        gpointer user_data);
static gboolean process_pidfd_ready(gint fd,
                                    GIOCondition condition,
                                    gpointer user_data);
static gboolean process_log_ready(gint fd,
                                  GIOCondition condition,
                                  gpointer user_data);
static gboolean process_group_reap_ready(gpointer user_data);

void
vivid_renderer_lifecycle_policy_init(VividRendererLifecyclePolicy* policy)
{
    if (!policy)
        return;
    *policy = (VividRendererLifecyclePolicy) {
        .hello_timeout_ms = VIVID_RENDERER_LIFECYCLE_HELLO_TIMEOUT_MS,
        .init_timeout_ms = VIVID_RENDERER_LIFECYCLE_INIT_TIMEOUT_MS,
        .quiesce_timeout_ms = VIVID_RENDERER_LIFECYCLE_QUIESCE_TIMEOUT_MS,
        .shutdown_timeout_ms = VIVID_RENDERER_LIFECYCLE_SHUTDOWN_TIMEOUT_MS,
        .term_timeout_ms = VIVID_RENDERER_LIFECYCLE_TERM_TIMEOUT_MS,
    };
}

const gchar*
vivid_renderer_process_state_name(VividRendererProcessState state)
{
    switch (state) {
    case VIVID_RENDERER_PROCESS_EMPTY: return "EMPTY";
    case VIVID_RENDERER_PROCESS_SPAWNING: return "SPAWNING";
    case VIVID_RENDERER_PROCESS_WAIT_HELLO: return "WAIT_HELLO";
    case VIVID_RENDERER_PROCESS_INITIALIZING: return "INITIALIZING";
    case VIVID_RENDERER_PROCESS_NEGOTIATING: return "NEGOTIATING";
    case VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME: return "WAIT_FIRST_FRAME";
    case VIVID_RENDERER_PROCESS_ACTIVE: return "ACTIVE";
    case VIVID_RENDERER_PROCESS_QUIESCING: return "QUIESCING";
    case VIVID_RENDERER_PROCESS_UNBINDING: return "UNBINDING";
    case VIVID_RENDERER_PROCESS_SHUTTING_DOWN: return "SHUTTING_DOWN";
    case VIVID_RENDERER_PROCESS_FAILED: return "FAILED";
    case VIVID_RENDERER_PROCESS_CRASHED: return "CRASHED";
    case VIVID_RENDERER_PROCESS_EXITED: return "EXITED";
    default: return "UNKNOWN";
    }
}

static void
outbox_item_free(VividRendererOutboxItem* item)
{
    if (!item)
        return;
    g_free(item->payload);
    g_free(item);
}

static void
destroy_source(GSource** source)
{
    if (!source || !*source)
        return;
    g_source_destroy(*source);
    g_source_unref(*source);
    *source = NULL;
}

static GSource*
attach_unix_source(VividRendererProcess* process,
                   gint fd,
                   GIOCondition condition,
                   GUnixFDSourceFunc callback)
{
    GSource* source = g_unix_fd_source_new(fd, condition);
    g_source_set_callback(source, G_SOURCE_FUNC(callback), process, NULL);
    g_source_attach(source, process->context);
    return source;
}

static void
cancel_deadline(VividRendererProcess* process)
{
    destroy_source(&process->deadline_source);
}

static void
signal_process_group(VividRendererProcess* process,
                     gint signal_number,
                     const gchar* reason)
{
    if (!process || process->process_group <= 0)
        return;
    if (kill(-process->process_group, signal_number) == 0) {
        g_warning("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
                  " renderer=%s pid=%d pgid=%d signal=%d reason=%s",
                  process->route_id,
                  process->instance_id,
                  vivid_renderer_descriptor_id(process->descriptor),
                  process->pid,
                  process->process_group,
                  signal_number,
                  reason);
    } else if (errno != ESRCH) {
        g_warning("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
                  " pid=%d failed to signal pgid=%d signal=%d: %s",
                  process->route_id,
                  process->instance_id,
                  process->pid,
                  process->process_group,
                  signal_number,
                  g_strerror(errno));
    }
}

static gboolean
process_deadline_expired(gpointer user_data)
{
    VividRendererProcess* process = user_data;
    GSource* expired = process->deadline_source;
    process->deadline_source = NULL;
    if (expired)
        g_source_unref(expired);

    if (process->reaped)
        return G_SOURCE_REMOVE;
    if (process->termination_sent) {
        if (!process->kill_sent) {
            process->kill_sent = TRUE;
            signal_process_group(process, SIGKILL, "SIGTERM deadline expired");
        }
        return G_SOURCE_REMOVE;
    }

    g_autofree gchar* reason =
        g_strdup_printf("lifecycle deadline expired in %s",
                        vivid_renderer_process_state_name(process->state));
    process_fail(process, reason);
    return G_SOURCE_REMOVE;
}

static void
schedule_deadline(VividRendererProcess* process, guint timeout_ms)
{
    cancel_deadline(process);
    if (timeout_ms == 0 || process->reaped)
        return;
    process->deadline_source = g_timeout_source_new(timeout_ms);
    g_source_set_callback(process->deadline_source,
                          process_deadline_expired,
                          process,
                          NULL);
    g_source_attach(process->deadline_source, process->context);
}

static void
schedule_state_deadline(VividRendererProcess* process)
{
    switch (process->state) {
    case VIVID_RENDERER_PROCESS_WAIT_HELLO:
        schedule_deadline(process, process->policy.hello_timeout_ms);
        break;
    case VIVID_RENDERER_PROCESS_INITIALIZING:
    case VIVID_RENDERER_PROCESS_NEGOTIATING:
    case VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME:
        schedule_deadline(process, process->policy.init_timeout_ms);
        break;
    case VIVID_RENDERER_PROCESS_QUIESCING:
        schedule_deadline(process, process->policy.quiesce_timeout_ms);
        break;
    case VIVID_RENDERER_PROCESS_SHUTTING_DOWN:
        schedule_deadline(process, process->policy.shutdown_timeout_ms);
        break;
    default:
        cancel_deadline(process);
        break;
    }
}

static void
process_transition(VividRendererProcess* process,
                   VividRendererProcessState new_state)
{
    if (!process || process->state == new_state)
        return;
    const VividRendererProcessState old_state = process->state;
    const gint64 now_usec = g_get_monotonic_time();
    const gdouble elapsed_ms = process->state_entered_usec == 0
        ? 0.0
        : (gdouble)(now_usec - process->state_entered_usec) / 1000.0;
    process->state = new_state;
    process->state_entered_usec = now_usec;
    if (new_state == VIVID_RENDERER_PROCESS_ACTIVE)
        process->was_active = TRUE;
    g_message("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
              " renderer=%s kind=%u pid=%d identity=%s transition=%s->%s"
              " previous-state-ms=%.3f",
              process->route_id,
              process->instance_id,
              vivid_renderer_descriptor_id(process->descriptor),
              vivid_renderer_descriptor_kind(process->descriptor),
              process->pid,
              process->identity_hash,
              vivid_renderer_process_state_name(old_state),
              vivid_renderer_process_state_name(new_state),
              elapsed_ms);
    schedule_state_deadline(process);
    if (process->observer.state_changed) {
        process->observer.state_changed(process,
                                        old_state,
                                        new_state,
                                        process->observer_data);
    }
}

static void
close_ipc_transaction(VividRendererProcess* process)
{
    destroy_source(&process->socket_source);
    destroy_source(&process->write_source);
    if (process->socket_fd >= 0) {
        close(process->socket_fd);
        process->socket_fd = -1;
    }
    while (!g_queue_is_empty(&process->outbox))
        outbox_item_free(g_queue_pop_head(&process->outbox));
}

static void
begin_termination(VividRendererProcess* process, const gchar* reason)
{
    if (!process || process->reaped || process->termination_sent)
        return;
    process->termination_sent = TRUE;
    signal_process_group(process, SIGTERM, reason);
    schedule_deadline(process, process->policy.term_timeout_ms);
}

static void
process_fail(VividRendererProcess* process, const gchar* reason)
{
    if (!process || process->reaped ||
        process->state == VIVID_RENDERER_PROCESS_EXITED) {
        return;
    }
    /* Duplicate first because worker ERROR handling may pass last_error itself. */
    gchar* owned_reason = g_strdup(reason ? reason : "renderer process failed");
    g_free(process->last_error);
    process->last_error = owned_reason;
    const VividRendererProcessState failure_state = process->was_active
        ? VIVID_RENDERER_PROCESS_CRASHED
        : VIVID_RENDERER_PROCESS_FAILED;
    process_transition(process, failure_state);
    close_ipc_transaction(process);
    begin_termination(process, process->last_error);
}

static gboolean
hello_is_valid(VividRendererProcess* process, const VividRendererPacket* packet)
{
    if (packet->header.opcode != VIVID_RENDERER_MSG_HELLO ||
        process->hello_seen ||
        process->state != VIVID_RENDERER_PROCESS_WAIT_HELLO) {
        return FALSE;
    }
    const guint32 spawn_version = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_HELLO_SPAWN_VERSION_OFFSET);
    const guint32 kind = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_HELLO_RENDERER_KIND_OFFSET);
    const guint32 reported_pid = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_HELLO_PID_OFFSET);
    const guint32 id_length = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_HELLO_RENDERER_ID_LENGTH_OFFSET);
    g_autofree gchar* renderer_id =
        g_strndup((const gchar*)packet->payload + VIVID_RENDERER_HELLO_FIXED_BYTES,
                  id_length);
    return spawn_version == vivid_renderer_descriptor_spawn_version(process->descriptor) &&
        kind == (guint32)vivid_renderer_descriptor_kind(process->descriptor) &&
        (guint64)reported_pid == (guint64)process->pid &&
        g_strcmp0(renderer_id,
                  vivid_renderer_descriptor_id(process->descriptor)) == 0;
}

static void
store_worker_error(VividRendererProcess* process,
                   const VividRendererPacket* packet)
{
    const gboolean bind_failed =
        packet->header.opcode == VIVID_RENDERER_MSG_BIND_FAILED;
    const guint32 error_code = vivid_renderer_wire_read_u32(
        packet->payload + (bind_failed
                               ? VIVID_RENDERER_BIND_FAILED_ERROR_CODE_OFFSET
                               : VIVID_RENDERER_ERROR_ERROR_CODE_OFFSET));
    const guint32 stage = vivid_renderer_wire_read_u32(
        packet->payload + (bind_failed
                               ? VIVID_RENDERER_BIND_FAILED_STAGE_OFFSET
                               : VIVID_RENDERER_ERROR_STAGE_OFFSET));
    const guint32 length = vivid_renderer_wire_read_u32(
        packet->payload + (bind_failed
                               ? VIVID_RENDERER_BIND_FAILED_DIAGNOSTIC_LENGTH_OFFSET
                               : VIVID_RENDERER_ERROR_DIAGNOSTIC_LENGTH_OFFSET));
    g_autofree gchar* diagnostic =
        g_strndup((const gchar*)packet->payload +
                      (bind_failed
                           ? VIVID_RENDERER_BIND_FAILED_FIXED_BYTES
                           : VIVID_RENDERER_ERROR_FIXED_BYTES),
                  length);
    g_free(process->last_error);
    process->last_error = g_strdup_printf("worker error code=%u stage=%u: %s",
                                          error_code,
                                          stage,
                                          diagnostic);
}

static gboolean
store_ready(VividRendererProcess* process, const VividRendererPacket* packet)
{
    const guint32 render_node_length = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_READY_RENDER_NODE_LENGTH_OFFSET);
    g_autofree gchar* render_node = g_strndup(
        (const gchar*)packet->payload + VIVID_RENDERER_READY_FIXED_BYTES,
        render_node_length);
    if (!process->expected_render_node ||
        g_strcmp0(render_node, process->expected_render_node) != 0 ||
        memcmp(packet->payload + VIVID_RENDERER_READY_DEVICE_UUID_OFFSET,
               process->expected_device_uuid,
               VIVID_RENDERER_UUID_BYTES) != 0) {
        return FALSE;
    }
    memcpy(process->actual_device_uuid,
           packet->payload + VIVID_RENDERER_READY_DEVICE_UUID_OFFSET,
           VIVID_RENDERER_UUID_BYTES);
    memcpy(process->actual_driver_uuid,
           packet->payload + VIVID_RENDERER_READY_DRIVER_UUID_OFFSET,
           VIVID_RENDERER_UUID_BYTES);
    process->actual_drm_render_major = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_READY_DRM_RENDER_MAJOR_OFFSET);
    process->actual_drm_render_minor = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_READY_DRM_RENDER_MINOR_OFFSET);
    g_free(process->actual_render_node);
    process->actual_render_node = g_steal_pointer(&render_node);
    return process->actual_drm_render_major != 0 ||
        process->actual_drm_render_minor != 0;
}

static gboolean
store_format_caps(VividRendererProcess* process,
                  const VividRendererPacket* packet)
{
    VividRendererProcessFormatCaps caps = {
        .memory_hints = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_MEMORY_HINTS_OFFSET),
        .sync_caps = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_SYNC_CAPS_OFFSET),
        .color_caps = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_COLOR_CAPS_OFFSET),
        .relay_modes = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_RELAY_MODES_OFFSET),
        .extent_max_width = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_EXTENT_MAX_WIDTH_OFFSET),
        .extent_max_height = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_EXTENT_MAX_HEIGHT_OFFSET),
        .pool_size_min = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_POOL_SIZE_MIN_OFFSET),
        .pool_size_max = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_POOL_SIZE_MAX_OFFSET),
        .n_caps = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FORMAT_CAPS_CAPABILITY_COUNT_OFFSET),
    };
    if (caps.n_caps == 0 || caps.n_caps > VIVID_RENDERER_MAX_FORMAT_CAPS ||
        caps.pool_size_min == 0 || caps.pool_size_min > caps.pool_size_max ||
        caps.pool_size_max > VIVID_RENDERER_MAX_POOL_BUFFERS ||
        (caps.sync_caps & VIVID_RENDERER_SYNC_CAP_SYNCOBJ_TIMELINE) == 0) {
        return FALSE;
    }
    for (guint32 i = 0; i < caps.n_caps; i++) {
        const guint8* record = packet->payload +
            VIVID_RENDERER_FORMAT_CAPS_FIXED_BYTES +
            (gsize)i * VIVID_RENDERER_FORMAT_CAPABILITY_BYTES;
        caps.caps[i] = (VividRendererProcessFormatCap) {
            .fourcc = vivid_renderer_wire_read_u32(
                record + VIVID_RENDERER_FORMAT_CAPABILITY_FOURCC_OFFSET),
            .modifier = vivid_renderer_wire_read_u64(
                record + VIVID_RENDERER_FORMAT_CAPABILITY_MODIFIER_OFFSET),
            .plane_count = vivid_renderer_wire_read_u32(
                record + VIVID_RENDERER_FORMAT_CAPABILITY_PLANE_COUNT_OFFSET),
        };
        if (caps.caps[i].plane_count == 0 ||
            caps.caps[i].plane_count > VIVID_RENDERER_MAX_PLANES) {
            return FALSE;
        }
    }
    process->format_caps = caps;
    return TRUE;
}

static gboolean
store_release_timeline(VividRendererProcess* process,
                       VividRendererPacket* packet)
{
    if (!process->actual_render_node || process->timeline_handle != 0)
        return FALSE;
    gint timeline_fd = vivid_renderer_packet_steal_fd(packet, 0);
    if (timeline_fd < 0)
        return FALSE;
    const gint drm_fd = open(process->actual_render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        close(timeline_fd);
        return FALSE;
    }
    guint32 handle = 0;
    if (drmSyncobjFDToHandle(drm_fd, timeline_fd, &handle) != 0 || handle == 0) {
        close(timeline_fd);
        close(drm_fd);
        return FALSE;
    }
    process->timeline_fd = timeline_fd;
    process->timeline_drm_fd = drm_fd;
    process->timeline_handle = handle;
    return TRUE;
}

static void
process_pool_init(VividRendererProcessPool* pool)
{
    memset(pool, 0, sizeof(*pool));
    for (guint32 buffer = 0; buffer < VIVID_RENDERER_MAX_POOL_BUFFERS; buffer++) {
        for (guint32 plane = 0; plane < VIVID_RENDERER_MAX_PLANES; plane++)
            pool->buffers[buffer].planes[plane].fd = -1;
    }
}

void
vivid_renderer_process_pool_clear(VividRendererProcessPool* pool)
{
    if (!pool)
        return;
    const guint32 n_buffers = MIN(pool->n_buffers,
                                  (guint32)VIVID_RENDERER_MAX_POOL_BUFFERS);
    for (guint32 buffer = 0; buffer < n_buffers; buffer++) {
        const guint32 n_planes = MIN(pool->buffers[buffer].n_planes,
                                     (guint32)VIVID_RENDERER_MAX_PLANES);
        for (guint32 plane = 0; plane < n_planes; plane++) {
            if (pool->buffers[buffer].planes[plane].fd >= 0)
                close(pool->buffers[buffer].planes[plane].fd);
        }
    }
    process_pool_init(pool);
}

static gboolean
store_pool(VividRendererProcess* process, VividRendererPacket* packet)
{
    if (process->pool.generation != 0)
        return FALSE;
    VividRendererProcessPool pool;
    process_pool_init(&pool);
    pool.generation = vivid_renderer_wire_read_u64(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_POOL_GENERATION_OFFSET);
    pool.fourcc = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_FOURCC_OFFSET);
    pool.width = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_WIDTH_OFFSET);
    pool.height = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_HEIGHT_OFFSET);
    pool.n_buffers = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_BUFFER_COUNT_OFFSET);
    const guint32 plane_count = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_PLANE_COUNT_OFFSET);
    pool.memory_source = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_MEMORY_SOURCE_OFFSET);
    pool.flags = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_FLAGS_OFFSET);
    pool.modifier = vivid_renderer_wire_read_u64(
        packet->payload + VIVID_RENDERER_BIND_BUFFERS_MODIFIER_OFFSET);
    if (pool.generation == 0 || pool.width == 0 || pool.height == 0 ||
        pool.n_buffers == 0 || pool.n_buffers > VIVID_RENDERER_MAX_POOL_BUFFERS ||
        plane_count == 0 || plane_count > VIVID_RENDERER_MAX_PLANES ||
        pool.memory_source > VIVID_RENDERER_MEMORY_SOURCE_DMABUF_HEAP_RESERVED) {
        return FALSE;
    }
    size_t fd_index = 0;
    for (guint32 buffer = 0; buffer < pool.n_buffers; buffer++) {
        pool.buffers[buffer].index = buffer;
        pool.buffers[buffer].n_planes = plane_count;
        for (guint32 plane = 0; plane < plane_count; plane++, fd_index++) {
            const guint8* record = packet->payload +
                VIVID_RENDERER_BIND_BUFFERS_FIXED_BYTES +
                fd_index * VIVID_RENDERER_BUFFER_PLANE_BYTES;
            pool.buffers[buffer].planes[plane] = (VividRendererProcessPlane) {
                .fd = vivid_renderer_packet_steal_fd(packet, fd_index),
                .stride = vivid_renderer_wire_read_u32(
                    record + VIVID_RENDERER_BUFFER_PLANE_STRIDE_OFFSET),
                .offset = vivid_renderer_wire_read_u32(
                    record + VIVID_RENDERER_BUFFER_PLANE_OFFSET_OFFSET),
                .size = vivid_renderer_wire_read_u64(
                    record + VIVID_RENDERER_BUFFER_PLANE_SIZE_OFFSET),
            };
            if (pool.buffers[buffer].planes[plane].fd < 0 ||
                pool.buffers[buffer].planes[plane].stride == 0 ||
                pool.buffers[buffer].planes[plane].size == 0) {
                vivid_renderer_process_pool_clear(&pool);
                return FALSE;
            }
        }
    }
    process->pool = pool;
    return TRUE;
}

void
vivid_renderer_process_frame_clear(VividRendererProcessFrame* frame)
{
    if (!frame)
        return;
    if (frame->acquire_sync_fd >= 0)
        close(frame->acquire_sync_fd);
    memset(frame, 0, sizeof(*frame));
    frame->acquire_sync_fd = -1;
}

static void
queued_frame_free(VividRendererQueuedFrame* queued)
{
    if (!queued)
        return;
    vivid_renderer_process_frame_clear(&queued->frame);
    g_free(queued);
}

static gboolean
store_frame(VividRendererProcess* process, VividRendererPacket* packet)
{
    /*
     * A correct worker cannot have more unpublished frames than negotiated
     * pool slots because each slot remains timeline-owned until the consumer
     * releases it. Enforce that invariant before stealing the acquire fence so
     * a malformed or compromised worker cannot turn the daemon queue into
     * unbounded memory and FD growth.
     */
    if (process->pool.n_buffers == 0 ||
        g_queue_get_length(&process->frames) >= process->pool.n_buffers) {
        return FALSE;
    }

    VividRendererQueuedFrame* queued = g_new0(VividRendererQueuedFrame, 1);
    queued->frame = (VividRendererProcessFrame) {
        .pool_generation = vivid_renderer_wire_read_u64(
            packet->payload + VIVID_RENDERER_FRAME_READY_POOL_GENERATION_OFFSET),
        .buffer_index = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FRAME_READY_BUFFER_INDEX_OFFSET),
        .flags = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_FRAME_READY_FLAGS_OFFSET),
        .sequence = vivid_renderer_wire_read_u64(
            packet->payload + VIVID_RENDERER_FRAME_READY_SEQUENCE_OFFSET),
        .target_time_usec = vivid_renderer_wire_read_u64(
            packet->payload + VIVID_RENDERER_FRAME_READY_TARGET_TIME_USEC_OFFSET),
        .release_point = vivid_renderer_wire_read_u64(
            packet->payload + VIVID_RENDERER_FRAME_READY_RELEASE_POINT_OFFSET),
        .acquire_sync_fd = vivid_renderer_packet_steal_fd(packet, 0),
    };
    if (queued->frame.pool_generation != process->pool.generation ||
        queued->frame.buffer_index >= process->pool.n_buffers ||
        queued->frame.sequence == 0 || queued->frame.release_point == 0 ||
        queued->frame.acquire_sync_fd < 0 ||
        queued->frame.sequence <= process->last_frame_sequence ||
        queued->frame.release_point <= process->last_release_point) {
        queued_frame_free(queued);
        return FALSE;
    }
    process->last_frame_sequence = queued->frame.sequence;
    process->last_release_point = queued->frame.release_point;
    g_queue_push_tail(&process->frames, queued);
    return TRUE;
}

static gboolean
process_accept_packet(VividRendererProcess* process, VividRendererPacket* packet)
{
    if (!process->hello_seen) {
        if (!hello_is_valid(process, packet))
            return FALSE;
        process->hello_seen = TRUE;
        process_transition(process, VIVID_RENDERER_PROCESS_INITIALIZING);
        return TRUE;
    }

    switch (packet->header.opcode) {
    case VIVID_RENDERER_MSG_HELLO:
        return FALSE;
    case VIVID_RENDERER_MSG_READY:
        if (process->state != VIVID_RENDERER_PROCESS_INITIALIZING ||
            process->ready_seen ||
            packet->header.request_id != process->init_request_id) {
            return FALSE;
        }
        if (!store_ready(process, packet))
            return FALSE;
        process->ready_seen = TRUE;
        break;
    case VIVID_RENDERER_MSG_FORMAT_CAPS:
        if (process->state != VIVID_RENDERER_PROCESS_INITIALIZING ||
            process->format_caps_seen ||
            packet->header.request_id != process->init_request_id) {
            return FALSE;
        }
        if (!store_format_caps(process, packet))
            return FALSE;
        process->format_caps_seen = TRUE;
        break;
    case VIVID_RENDERER_MSG_RELEASE_TIMELINE:
        if (process->state != VIVID_RENDERER_PROCESS_INITIALIZING ||
            process->release_timeline_seen ||
            packet->header.request_id != process->init_request_id) {
            return FALSE;
        }
        if (!store_release_timeline(process, packet))
            return FALSE;
        process->release_timeline_seen = TRUE;
        break;
    case VIVID_RENDERER_MSG_BIND_BUFFERS:
        if ((process->state != VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME &&
             process->state != VIVID_RENDERER_PROCESS_ACTIVE) ||
            packet->header.request_id != process->negotiate_request_id) {
            return FALSE;
        }
        if (!store_pool(process, packet))
            return FALSE;
        process->bind_buffers_seen = TRUE;
        break;
    case VIVID_RENDERER_MSG_FRAME_READY:
        if ((process->state != VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME &&
             process->state != VIVID_RENDERER_PROCESS_ACTIVE) ||
            !process->bind_buffers_seen) {
            return FALSE;
        }
        if (!store_frame(process, packet))
            return FALSE;
        if (!process->first_frame_seen) {
            process->first_frame_seen = TRUE;
            process_transition(process, VIVID_RENDERER_PROCESS_ACTIVE);
        }
        break;
    case VIVID_RENDERER_MSG_STATE_CHANGED:
        if (process->state < VIVID_RENDERER_PROCESS_INITIALIZING ||
            process->state >= VIVID_RENDERER_PROCESS_SHUTTING_DOWN) {
            return FALSE;
        }
        break;
    case VIVID_RENDERER_MSG_BIND_FAILED:
        if (packet->header.request_id != process->negotiate_request_id)
            return FALSE;
        store_worker_error(process, packet);
        process_fail(process, process->last_error);
        break;
    case VIVID_RENDERER_MSG_ERROR:
        store_worker_error(process, packet);
        process_fail(process, process->last_error);
        break;
    case VIVID_RENDERER_MSG_QUIESCED:
        if (process->state != VIVID_RENDERER_PROCESS_QUIESCING ||
            packet->header.request_id != process->quiesce_request_id) {
            return FALSE;
        }
        process_transition(process, VIVID_RENDERER_PROCESS_UNBINDING);
        break;
    default:
        return FALSE;
    }

    if (process->state == VIVID_RENDERER_PROCESS_INITIALIZING &&
        process->ready_seen && process->format_caps_seen &&
        process->release_timeline_seen) {
        process_transition(process, VIVID_RENDERER_PROCESS_NEGOTIATING);
    }
    return TRUE;
}

static gboolean
process_socket_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    VividRendererProcess* process = user_data;
    if (fd != process->socket_fd)
        return G_SOURCE_REMOVE;

    for (;;) {
        VividRendererPacket packet;
        vivid_renderer_packet_init(&packet);
        const int result = vivid_renderer_transport_receive(
            process->socket_fd,
            process->instance_id,
            VIVID_RENDERER_DIRECTION_WORKER_TO_DAEMON,
            &packet);
        if (result == VIVID_RENDERER_TRANSPORT_WOULD_BLOCK) {
            vivid_renderer_packet_clear(&packet);
            break;
        }
        if (result == VIVID_RENDERER_TRANSPORT_EOF) {
            vivid_renderer_packet_clear(&packet);
            if (process->state == VIVID_RENDERER_PROCESS_SHUTTING_DOWN) {
                g_message("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
                          " renderer=%s pid=%d IPC closed after SHUTDOWN; awaiting pidfd",
                          process->route_id,
                          process->instance_id,
                          vivid_renderer_descriptor_id(process->descriptor),
                          process->pid);
                close_ipc_transaction(process);
                return G_SOURCE_REMOVE;
            }
            process_fail(process, "renderer IPC reached EOF");
            return G_SOURCE_REMOVE;
        }
        if (result < 0) {
            vivid_renderer_packet_clear(&packet);
            g_autofree gchar* reason =
                g_strdup_printf("renderer IPC protocol transaction failed: %s",
                                g_strerror(-result));
            process_fail(process, reason);
            return G_SOURCE_REMOVE;
        }
        if (!process_accept_packet(process, &packet)) {
            const guint16 opcode = packet.header.opcode;
            vivid_renderer_packet_clear(&packet);
            g_autofree gchar* reason =
                g_strdup_printf("renderer IPC message 0x%04x violates lifecycle state %s",
                                opcode,
                                vivid_renderer_process_state_name(process->state));
            process_fail(process, reason);
            return G_SOURCE_REMOVE;
        }
        if (process->observer.packet_received) {
            process->observer.packet_received(process,
                                              &packet,
                                              process->observer_data);
        }
        vivid_renderer_packet_clear(&packet);
        if (process->socket_fd < 0)
            return G_SOURCE_REMOVE;
    }

    if ((condition & (G_IO_ERR | G_IO_NVAL)) != 0) {
        process_fail(process, "renderer IPC source reported an error");
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
flush_outbox(VividRendererProcess* process)
{
    while (!g_queue_is_empty(&process->outbox)) {
        VividRendererOutboxItem* item = g_queue_peek_head(&process->outbox);
        const int result = vivid_renderer_transport_send_borrowed(
            process->socket_fd,
            process->instance_id,
            VIVID_RENDERER_DIRECTION_DAEMON_TO_WORKER,
            item->opcode,
            item->request_id,
            item->payload,
            item->payload_length,
            NULL,
            0);
        if (result == VIVID_RENDERER_TRANSPORT_WOULD_BLOCK)
            return TRUE;
        if (result < 0) {
            g_autofree gchar* reason =
                g_strdup_printf("renderer IPC send failed: %s", g_strerror(-result));
            process_fail(process, reason);
            return FALSE;
        }
        g_queue_pop_head(&process->outbox);
        outbox_item_free(item);
    }
    return TRUE;
}

static gboolean
process_socket_writable(gint fd, GIOCondition condition, gpointer user_data)
{
    VividRendererProcess* process = user_data;
    if (fd != process->socket_fd ||
        (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0 ||
        !flush_outbox(process)) {
        if (process->socket_fd >= 0 &&
            (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
            process_fail(process, "renderer IPC outbox source closed");
        }
        return G_SOURCE_REMOVE;
    }
    if (g_queue_is_empty(&process->outbox)) {
        GSource* source = process->write_source;
        process->write_source = NULL;
        if (source)
            g_source_unref(source);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
queue_command(VividRendererProcess* process,
              guint16 opcode,
              guint64 request_id,
              const void* payload,
              size_t payload_length,
              GError** error)
{
    if (!process || process->socket_fd < 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "renderer IPC is closed");
        return FALSE;
    }
    if (!vivid_renderer_fd_count_valid(opcode,
                                       (const guint8*)payload,
                                       payload_length,
                                       0)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "renderer command payload violates the generated protocol layout");
        return FALSE;
    }
    VividRendererOutboxItem* item = g_new0(VividRendererOutboxItem, 1);
    item->opcode = opcode;
    item->request_id = request_id;
    item->payload_length = payload_length;
    item->payload = payload_length == 0 ? NULL : g_memdup2(payload, payload_length);
    g_queue_push_tail(&process->outbox, item);

    if (!flush_outbox(process)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "%s",
                    process->last_error ? process->last_error : "renderer IPC send failed");
        return FALSE;
    }
    if (!g_queue_is_empty(&process->outbox) && !process->write_source) {
        process->write_source = attach_unix_source(process,
                                                   process->socket_fd,
                                                   G_IO_OUT | G_IO_ERR |
                                                       G_IO_HUP | G_IO_NVAL,
                                                   process_socket_writable);
    }
    return TRUE;
}

static guint64
next_request_id(VividRendererProcess* process)
{
    guint64 request_id = process->next_request_id++;
    if (request_id == 0u)
        request_id = process->next_request_id++;
    return request_id;
}

static gboolean
send_required_request(VividRendererProcess* process,
                      guint16 opcode,
                      const void* payload,
                      size_t payload_length,
                      guint64* out_request_id,
                      GError** error)
{
    const guint64 request_id = next_request_id(process);
    if (!queue_command(process,
                       opcode,
                       request_id,
                       payload,
                       payload_length,
                       error)) {
        return FALSE;
    }
    if (out_request_id)
        *out_request_id = request_id;
    return TRUE;
}

static void
emit_log_line(VividRendererProcess* process, const gchar* line)
{
    g_message("VividRendererWorker: route=%s instance=%" G_GUINT64_FORMAT
              " renderer=%s pid=%d phase=%s %s",
              process->route_id,
              process->instance_id,
              vivid_renderer_descriptor_id(process->descriptor),
              process->pid,
              vivid_renderer_process_state_name(process->state),
              line);
    if (process->observer.log_line)
        process->observer.log_line(process, line, process->observer_data);
}

static void
consume_log_bytes(VividRendererProcess* process,
                  const guint8* bytes,
                  gsize length)
{
    for (gsize i = 0; i < length; i++) {
        if (bytes[i] == '\n') {
            emit_log_line(process, process->partial_log_line->str);
            g_string_truncate(process->partial_log_line, 0);
        } else if (bytes[i] != '\r') {
            g_string_append_c(process->partial_log_line, (gchar)bytes[i]);
        }
    }
}

static gboolean
drain_log_pipe(VividRendererProcess* process)
{
    guint8 buffer[4096];
    for (;;) {
        const ssize_t count = read(process->log_fd, buffer, sizeof(buffer));
        if (count > 0) {
            consume_log_bytes(process, buffer, (gsize)count);
            continue;
        }
        if (count == 0)
            return FALSE;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return TRUE;
        g_warning("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
                  " pid=%d log pipe read failed: %s",
                  process->route_id,
                  process->instance_id,
                  process->pid,
                  g_strerror(errno));
        return FALSE;
    }
}

static gboolean
process_log_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    VividRendererProcess* process = user_data;
    if (fd != process->log_fd)
        return G_SOURCE_REMOVE;
    const gboolean keep = drain_log_pipe(process);
    if (keep && (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) == 0)
        return G_SOURCE_CONTINUE;
    if (process->partial_log_line->len != 0) {
        emit_log_line(process, process->partial_log_line->str);
        g_string_truncate(process->partial_log_line, 0);
    }
    GSource* source = process->log_source;
    process->log_source = NULL;
    if (source)
        g_source_unref(source);
    close(process->log_fd);
    process->log_fd = -1;
    return G_SOURCE_REMOVE;
}

static gboolean
process_group_exists(const VividRendererProcess* process)
{
    if (!process || process->process_group <= 0)
        return FALSE;
    if (kill(-process->process_group, 0) == 0)
        return TRUE;
    return errno == EPERM;
}

static void
process_close_runtime_resources(VividRendererProcess* process)
{
    while (!g_queue_is_empty(&process->frames))
        queued_frame_free(g_queue_pop_head(&process->frames));
    vivid_renderer_process_pool_clear(&process->pool);
    g_mutex_lock(&process->timeline_lock);
    if (process->timeline_handle != 0 && process->timeline_drm_fd >= 0)
        (void)drmSyncobjDestroy(process->timeline_drm_fd, process->timeline_handle);
    process->timeline_handle = 0;
    if (process->timeline_fd >= 0)
        close(process->timeline_fd);
    if (process->timeline_drm_fd >= 0)
        close(process->timeline_drm_fd);
    process->timeline_fd = -1;
    process->timeline_drm_fd = -1;
    g_mutex_unlock(&process->timeline_lock);
}

static void
process_finish_reap(VividRendererProcess* process)
{
    if (!process || process->reaped)
        return;
    process->reaped = TRUE;
    process_close_runtime_resources(process);
    process_transition(process, VIVID_RENDERER_PROCESS_EXITED);
    g_message("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
              " renderer=%s pid=%d process-group-reap-complete status=0x%x",
              process->route_id,
              process->instance_id,
              vivid_renderer_descriptor_id(process->descriptor),
              process->pid,
              process->wait_status);
    if (process->observer.reaped) {
        process->observer.reaped(process,
                                 process->wait_status,
                                 process->observer_data);
    }
}

static void
process_reap_group_members(VividRendererProcess* process)
{
    gint status = 0;
    pid_t waited;
    do {
        waited = waitpid(-process->process_group, &status, WNOHANG);
        if (waited > 0) {
            g_message("VividRendererProcess: route=%s instance=%" G_GUINT64_FORMAT
                      " reaped helper pid=%d status=0x%x pgid=%d",
                      process->route_id,
                      process->instance_id,
                      waited,
                      status,
                      process->process_group);
        }
    } while (waited > 0 || (waited < 0 && errno == EINTR));
}

static gboolean
process_group_reap_ready(gpointer user_data)
{
    VividRendererProcess* process = user_data;
    process_reap_group_members(process);
    if (!process_group_exists(process)) {
        GSource* source = process->group_reap_source;
        process->group_reap_source = NULL;
        if (source)
            g_source_unref(source);
        process_finish_reap(process);
        return G_SOURCE_REMOVE;
    }
    const gint64 now = g_get_monotonic_time();
    if (now >= process->group_reap_log_deadline_usec) {
        signal_process_group(process,
                             SIGKILL,
                             "renderer helper group still alive after reap interval");
        process->group_reap_log_deadline_usec = now +
            (gint64)process->policy.term_timeout_ms * 1000;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
process_pidfd_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    VividRendererProcess* process = user_data;
    (void)condition;
    if (fd != process->pidfd)
        return G_SOURCE_REMOVE;
    gint status = 0;
    pid_t waited;
    do {
        waited = waitpid(process->pid, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == 0)
        return G_SOURCE_CONTINUE;
    if (waited < 0) {
        g_autofree gchar* reason =
            g_strdup_printf("waitpid failed for renderer pid=%d: %s",
                            process->pid,
                            g_strerror(errno));
        process_fail(process, reason);
        return G_SOURCE_CONTINUE;
    }

    process->top_reaped = TRUE;
    process->wait_status = status;
    cancel_deadline(process);
    close_ipc_transaction(process);
    if (process->log_fd >= 0) {
        drain_log_pipe(process);
        if (process->partial_log_line->len != 0) {
            emit_log_line(process, process->partial_log_line->str);
            g_string_truncate(process->partial_log_line, 0);
        }
        destroy_source(&process->log_source);
        close(process->log_fd);
        process->log_fd = -1;
    }
    if (process->state != VIVID_RENDERER_PROCESS_SHUTTING_DOWN &&
        process->state != VIVID_RENDERER_PROCESS_FAILED &&
        process->state != VIVID_RENDERER_PROCESS_CRASHED) {
        g_autofree gchar* reason =
            g_strdup_printf("renderer exited unexpectedly status=0x%x", status);
        g_free(process->last_error);
        process->last_error = g_steal_pointer(&reason);
        process_transition(process,
                           process->was_active
                               ? VIVID_RENDERER_PROCESS_CRASHED
                               : VIVID_RENDERER_PROCESS_FAILED);
    }

    GSource* source = process->pidfd_source;
    process->pidfd_source = NULL;
    if (source)
        g_source_unref(source);
    close(process->pidfd);
    process->pidfd = -1;

    process_reap_group_members(process);
    if (!process_group_exists(process)) {
        process_finish_reap(process);
        return G_SOURCE_REMOVE;
    }

    /*
     * A top-level exit is not the transaction boundary for Web: Chromium may
     * still have adopted renderer/GPU/utility helpers in the same group. Kill
     * the complete group and keep reaping subreaper children before reporting
     * EXITED, so stop-before-spawn cannot overlap helper generations.
     */
    signal_process_group(process,
                         SIGKILL,
                         "top-level worker reaped with live process group");
    process->group_reap_log_deadline_usec = g_get_monotonic_time() +
        (gint64)process->policy.term_timeout_ms * 1000;
    process->group_reap_source = g_timeout_source_new(
        VIVID_RENDERER_BACKEND_READY_POLL_INTERVAL_MS);
    g_source_set_callback(process->group_reap_source,
                          process_group_reap_ready,
                          process,
                          NULL);
    g_source_attach(process->group_reap_source, process->context);
    return G_SOURCE_REMOVE;
}

VividRendererProcess*
vivid_renderer_process_new_spawned(
    const VividRendererDescriptor* descriptor,
    const gchar* route_id,
    const gchar* identity_hash,
    guint64 instance_id,
    pid_t pid,
    gint pidfd,
    pid_t process_group,
    gint socket_fd,
    gint log_fd,
    GMainContext* context,
    const VividRendererLifecyclePolicy* policy,
    const VividRendererProcessObserver* observer,
    gpointer observer_data)
{
    g_return_val_if_fail(descriptor != NULL, NULL);
    g_return_val_if_fail(route_id != NULL, NULL);
    g_return_val_if_fail(identity_hash != NULL, NULL);
    g_return_val_if_fail(instance_id != 0u, NULL);
    g_return_val_if_fail(pid > 0 && pidfd >= 0 && process_group > 0, NULL);
    g_return_val_if_fail(socket_fd >= 0 && log_fd >= 0, NULL);

    VividRendererProcess* process = g_new0(VividRendererProcess, 1);
    process->descriptor = descriptor;
    process->route_id = g_strdup(route_id);
    process->identity_hash = g_strdup(identity_hash);
    process->instance_id = instance_id;
    process->pid = pid;
    process->pidfd = pidfd;
    process->process_group = process_group;
    process->socket_fd = socket_fd;
    process->log_fd = log_fd;
    process->timeline_fd = -1;
    process->timeline_drm_fd = -1;
    process->wait_status = -1;
    process->next_request_id = 1;
    process->context = g_main_context_ref(context ? context : g_main_context_default());
    vivid_renderer_lifecycle_policy_init(&process->policy);
    if (policy)
        process->policy = *policy;
    if (observer)
        process->observer = *observer;
    process->observer_data = observer_data;
    g_queue_init(&process->outbox);
    g_queue_init(&process->frames);
    g_mutex_init(&process->timeline_lock);
    process_pool_init(&process->pool);
    process->partial_log_line = g_string_new(NULL);
    process->state = VIVID_RENDERER_PROCESS_EMPTY;

    process_transition(process, VIVID_RENDERER_PROCESS_SPAWNING);
    process->socket_source = attach_unix_source(process,
                                                process->socket_fd,
                                                G_IO_IN | G_IO_ERR |
                                                    G_IO_HUP | G_IO_NVAL,
                                                process_socket_ready);
    process->pidfd_source = attach_unix_source(process,
                                               process->pidfd,
                                               G_IO_IN | G_IO_ERR |
                                                   G_IO_HUP | G_IO_NVAL,
                                               process_pidfd_ready);
    process->log_source = attach_unix_source(process,
                                             process->log_fd,
                                             G_IO_IN | G_IO_ERR |
                                                 G_IO_HUP | G_IO_NVAL,
                                             process_log_ready);
    process_transition(process, VIVID_RENDERER_PROCESS_WAIT_HELLO);
    return process;
}

void
vivid_renderer_process_free(VividRendererProcess* process)
{
    if (!process)
        return;
    if (!process->top_reaped) {
        signal_process_group(process, SIGKILL, "process object destroyed before reap");
        gint status = 0;
        while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {
        }
        process->top_reaped = TRUE;
        process->wait_status = status;
    }
    if (!process->reaped) {
        signal_process_group(process, SIGKILL, "process group destroyed before reap");
        gint status = 0;
        while (waitpid(-process->process_group, &status, 0) > 0 || errno == EINTR) {
            errno = 0;
        }
        process->reaped = TRUE;
    }
    cancel_deadline(process);
    destroy_source(&process->group_reap_source);
    close_ipc_transaction(process);
    destroy_source(&process->pidfd_source);
    destroy_source(&process->log_source);
    if (process->pidfd >= 0)
        close(process->pidfd);
    if (process->log_fd >= 0)
        close(process->log_fd);
    while (!g_queue_is_empty(&process->outbox))
        outbox_item_free(g_queue_pop_head(&process->outbox));
    process_close_runtime_resources(process);
    g_mutex_clear(&process->timeline_lock);
    if (process->partial_log_line)
        g_string_free(process->partial_log_line, TRUE);
    g_clear_pointer(&process->context, g_main_context_unref);
    g_free(process->last_error);
    g_free(process->actual_render_node);
    g_free(process->expected_render_node);
    g_free(process->identity_hash);
    g_free(process->route_id);
    g_free(process);
}

gboolean
vivid_renderer_process_send_init(VividRendererProcess* process,
                                 const void* payload,
                                 size_t payload_length,
                                 guint64* out_request_id,
                                 GError** error)
{
    if (!process || process->state != VIVID_RENDERER_PROCESS_INITIALIZING ||
        process->init_request_id != 0u || !payload) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PENDING,
                            "renderer process is not waiting for INIT");
        return FALSE;
    }
    const guint8* bytes = payload;
    const guint32 renderer_id_length = vivid_renderer_wire_read_u32(
        bytes + VIVID_RENDERER_INIT_RENDERER_ID_LENGTH_OFFSET);
    const guint32 project_path_length = vivid_renderer_wire_read_u32(
        bytes + VIVID_RENDERER_INIT_PROJECT_PATH_LENGTH_OFFSET);
    const guint32 render_node_length = vivid_renderer_wire_read_u32(
        bytes + VIVID_RENDERER_INIT_RENDER_NODE_LENGTH_OFFSET);
    if (!vivid_renderer_payload_length_valid(VIVID_RENDERER_MSG_INIT,
                                             bytes,
                                             payload_length) ||
        render_node_length == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "renderer INIT payload does not contain a valid GPU identity");
        return FALSE;
    }
    memcpy(process->expected_device_uuid,
           bytes + VIVID_RENDERER_INIT_EXPECTED_DEVICE_UUID_OFFSET,
           VIVID_RENDERER_UUID_BYTES);
    const guint8* render_node = bytes + VIVID_RENDERER_INIT_FIXED_BYTES +
        renderer_id_length + project_path_length;
    g_free(process->expected_render_node);
    process->expected_render_node =
        g_strndup((const gchar*)render_node, render_node_length);
    if (!send_required_request(process,
                               VIVID_RENDERER_MSG_INIT,
                               payload,
                               payload_length,
                               &process->init_request_id,
                               error)) {
        return FALSE;
    }
    if (out_request_id)
        *out_request_id = process->init_request_id;
    return TRUE;
}

gboolean
vivid_renderer_process_send_negotiate_buffers(
    VividRendererProcess* process,
    const void* payload,
    size_t payload_length,
    guint64* out_request_id,
    GError** error)
{
    if (!process || process->state != VIVID_RENDERER_PROCESS_NEGOTIATING) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PENDING,
                            "renderer process is not ready for buffer negotiation");
        return FALSE;
    }
    if (!send_required_request(process,
                               VIVID_RENDERER_MSG_NEGOTIATE_BUFFERS,
                               payload,
                               payload_length,
                               &process->negotiate_request_id,
                               error)) {
        return FALSE;
    }
    process->bind_buffers_seen = FALSE;
    process->first_frame_seen = FALSE;
    process_transition(process, VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME);
    if (out_request_id)
        *out_request_id = process->negotiate_request_id;
    return TRUE;
}

gboolean
vivid_renderer_process_send_runtime(VividRendererProcess* process,
                                    guint16 opcode,
                                    const void* payload,
                                    size_t payload_length,
                                    guint64* out_request_id,
                                    GError** error)
{
    const VividRendererMessageDescriptor* descriptor =
        vivid_renderer_message_descriptor(opcode);
    if (!process || !descriptor ||
        descriptor->direction != VIVID_RENDERER_DIRECTION_DAEMON_TO_WORKER ||
        opcode == VIVID_RENDERER_MSG_INIT ||
        opcode == VIVID_RENDERER_MSG_NEGOTIATE_BUFFERS ||
        opcode == VIVID_RENDERER_MSG_QUIESCE ||
        opcode == VIVID_RENDERER_MSG_SHUTDOWN ||
        (process->state != VIVID_RENDERER_PROCESS_NEGOTIATING &&
         process->state != VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME &&
         process->state != VIVID_RENDERER_PROCESS_ACTIVE)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "renderer runtime command is invalid in the current state");
        return FALSE;
    }
    const guint64 request_id =
        descriptor->request_id_policy == VIVID_RENDERER_REQUEST_ID_REQUIRED
        ? next_request_id(process)
        : 0u;
    if (!queue_command(process,
                       opcode,
                       request_id,
                       payload,
                       payload_length,
                       error)) {
        return FALSE;
    }
    if (out_request_id)
        *out_request_id = request_id;
    return TRUE;
}

gboolean
vivid_renderer_process_request_quiesce(VividRendererProcess* process,
                                       GError** error)
{
    if (!process ||
        (process->state != VIVID_RENDERER_PROCESS_NEGOTIATING &&
         process->state != VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME &&
         process->state != VIVID_RENDERER_PROCESS_ACTIVE)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PENDING,
                            "renderer process cannot quiesce in the current state");
        return FALSE;
    }
    if (!send_required_request(process,
                               VIVID_RENDERER_MSG_QUIESCE,
                               NULL,
                               0,
                               &process->quiesce_request_id,
                               error)) {
        return FALSE;
    }
    process_transition(process, VIVID_RENDERER_PROCESS_QUIESCING);
    return TRUE;
}

void
vivid_renderer_process_mark_unbinding(VividRendererProcess* process)
{
    if (process && process->state == VIVID_RENDERER_PROCESS_QUIESCING)
        process_transition(process, VIVID_RENDERER_PROCESS_UNBINDING);
}

gboolean
vivid_renderer_process_request_shutdown(VividRendererProcess* process,
                                        GError** error)
{
    if (!process || process->state != VIVID_RENDERER_PROCESS_UNBINDING) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PENDING,
                            "renderer process cannot shut down before unbinding");
        return FALSE;
    }
    if (!send_required_request(process,
                               VIVID_RENDERER_MSG_SHUTDOWN,
                               NULL,
                               0,
                               &process->shutdown_request_id,
                               error)) {
        return FALSE;
    }
    process_transition(process, VIVID_RENDERER_PROCESS_SHUTTING_DOWN);
    return TRUE;
}

void
vivid_renderer_process_terminate(VividRendererProcess* process,
                                 const gchar* reason)
{
    process_fail(process, reason ? reason : "renderer process terminated");
}

VividRendererProcessState
vivid_renderer_process_state(const VividRendererProcess* process)
{
    return process ? process->state : VIVID_RENDERER_PROCESS_EMPTY;
}

const VividRendererDescriptor*
vivid_renderer_process_descriptor(const VividRendererProcess* process)
{
    return process ? process->descriptor : NULL;
}

const gchar*
vivid_renderer_process_route_id(const VividRendererProcess* process)
{
    return process ? process->route_id : NULL;
}

const gchar*
vivid_renderer_process_identity_hash(const VividRendererProcess* process)
{
    return process ? process->identity_hash : NULL;
}

guint64
vivid_renderer_process_instance_id(const VividRendererProcess* process)
{
    return process ? process->instance_id : 0;
}

pid_t
vivid_renderer_process_pid(const VividRendererProcess* process)
{
    return process ? process->pid : -1;
}

pid_t
vivid_renderer_process_process_group(const VividRendererProcess* process)
{
    return process ? process->process_group : -1;
}

gint
vivid_renderer_process_pidfd(const VividRendererProcess* process)
{
    return process ? process->pidfd : -1;
}

gboolean
vivid_renderer_process_is_reaped(const VividRendererProcess* process)
{
    return process && process->reaped;
}

gint
vivid_renderer_process_wait_status(const VividRendererProcess* process)
{
    return process ? process->wait_status : -1;
}

const gchar*
vivid_renderer_process_last_error(const VividRendererProcess* process)
{
    return process ? process->last_error : NULL;
}

gboolean
vivid_renderer_process_actual_gpu(const VividRendererProcess* process,
                                  guint8 device_uuid[VIVID_RENDERER_UUID_BYTES],
                                  guint8 driver_uuid[VIVID_RENDERER_UUID_BYTES],
                                  guint32* drm_render_major,
                                  guint32* drm_render_minor,
                                  const gchar** render_node)
{
    if (!process || !process->ready_seen || !process->actual_render_node)
        return FALSE;
    if (device_uuid)
        memcpy(device_uuid, process->actual_device_uuid, VIVID_RENDERER_UUID_BYTES);
    if (driver_uuid)
        memcpy(driver_uuid, process->actual_driver_uuid, VIVID_RENDERER_UUID_BYTES);
    if (drm_render_major)
        *drm_render_major = process->actual_drm_render_major;
    if (drm_render_minor)
        *drm_render_minor = process->actual_drm_render_minor;
    if (render_node)
        *render_node = process->actual_render_node;
    return TRUE;
}

const VividRendererProcessFormatCaps*
vivid_renderer_process_format_caps(const VividRendererProcess* process)
{
    return process && process->format_caps_seen ? &process->format_caps : NULL;
}

gboolean
vivid_renderer_process_copy_pool(const VividRendererProcess* process,
                                 VividRendererProcessPool* out_pool,
                                 GError** error)
{
    if (!process || !out_pool || process->pool.generation == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PENDING,
                            "renderer has not published a DMA-BUF pool");
        return FALSE;
    }
    process_pool_init(out_pool);
    out_pool->generation = process->pool.generation;
    out_pool->fourcc = process->pool.fourcc;
    out_pool->width = process->pool.width;
    out_pool->height = process->pool.height;
    out_pool->memory_source = process->pool.memory_source;
    out_pool->flags = process->pool.flags;
    out_pool->modifier = process->pool.modifier;
    out_pool->n_buffers = process->pool.n_buffers;
    for (guint32 buffer = 0; buffer < process->pool.n_buffers; buffer++) {
        out_pool->buffers[buffer].index = process->pool.buffers[buffer].index;
        out_pool->buffers[buffer].n_planes = process->pool.buffers[buffer].n_planes;
        for (guint32 plane = 0;
             plane < process->pool.buffers[buffer].n_planes;
             plane++) {
            const VividRendererProcessPlane* source =
                &process->pool.buffers[buffer].planes[plane];
            VividRendererProcessPlane* target =
                &out_pool->buffers[buffer].planes[plane];
            const gint duplicated = fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
            if (duplicated < 0) {
                const gint saved_errno = errno;
                vivid_renderer_process_pool_clear(out_pool);
                g_set_error(error,
                            G_IO_ERROR,
                            g_io_error_from_errno(saved_errno),
                            "failed to duplicate renderer DMA-BUF: %s",
                            g_strerror(saved_errno));
                return FALSE;
            }
            *target = *source;
            target->fd = duplicated;
        }
    }
    return TRUE;
}

gboolean
vivid_renderer_process_take_frame(VividRendererProcess* process,
                                  VividRendererProcessFrame* out_frame)
{
    if (!process || !out_frame)
        return FALSE;
    VividRendererQueuedFrame* queued = g_queue_pop_head(&process->frames);
    if (!queued)
        return FALSE;
    *out_frame = queued->frame;
    queued->frame.acquire_sync_fd = -1;
    g_free(queued);
    return TRUE;
}

guint
vivid_renderer_process_pending_frame_count(const VividRendererProcess* process)
{
    return process ? process->frames.length : 0;
}

guint64
vivid_renderer_process_last_release_point(const VividRendererProcess* process)
{
    return process ? process->last_release_point : 0;
}

gboolean
vivid_renderer_process_signal_release_point(VividRendererProcess* process,
                                            guint64 release_point,
                                            GError** error)
{
    if (!process || release_point == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "renderer release point must be non-zero");
        return FALSE;
    }
    g_mutex_lock(&process->timeline_lock);
    guint32 handle = process->timeline_handle;
    const gint drm_fd = process->timeline_drm_fd;
    errno = 0;
    const gint result = handle != 0 && drm_fd >= 0
        ? drmSyncobjTimelineSignal(drm_fd, &handle, &release_point, 1)
        : -1;
    const gint saved_errno = errno != 0 ? errno : EINVAL;
    g_mutex_unlock(&process->timeline_lock);
    if (result == 0)
        return TRUE;
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "failed to signal renderer release point=%" G_GUINT64_FORMAT ": %s",
                release_point,
                g_strerror(saved_errno));
    return FALSE;
}
