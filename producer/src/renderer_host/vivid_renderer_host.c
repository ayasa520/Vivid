#define _GNU_SOURCE

#include "vivid_renderer_host.h"
#include "vivid_renderer_process_group.h"

#include <gio/gio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <unistd.h>

typedef enum
{
    VIVID_RENDERER_HOST_BOOT,
    VIVID_RENDERER_HOST_WAIT_INIT,
    VIVID_RENDERER_HOST_BACKEND_INIT,
    VIVID_RENDERER_HOST_WAIT_NEGOTIATION,
    VIVID_RENDERER_HOST_RUNNING,
    VIVID_RENDERER_HOST_QUIESCING,
    VIVID_RENDERER_HOST_QUIESCED,
    VIVID_RENDERER_HOST_SHUTDOWN,
    VIVID_RENDERER_HOST_EXIT,
} VividRendererHostPhase;

typedef struct
{
    guint16 opcode;
    guint64 request_id;
    guint8* payload;
    size_t payload_length;
    gint fds[VIVID_RENDERER_MAX_FDS_PER_MESSAGE];
    size_t n_fds;
} VividRendererHostEvent;

struct _VividRendererHost
{
    gint ipc_fd;
    gint wake_fd;
    gchar* renderer_id;
    guint64 instance_id;
    const VividRendererHostBackend* backend;
    gpointer backend_data;
    VividRendererHostPhase phase;
    GAsyncQueue* events;
    VividRendererHostEvent* pending_send;
    GMutex publish_lock;
    gboolean accepting_backend_events;
    gboolean init_callback_complete;
    gboolean ready_sent;
    gboolean format_caps_sent;
    gboolean release_timeline_sent;
    gboolean quiesce_reply_pending;
    gboolean stop_after_flush;
    gint exit_code;
    guint64 init_request_id;
    guint64 negotiate_request_id;
    guint64 quiesce_request_id;
};

static const gchar*
host_phase_name(VividRendererHostPhase phase)
{
    switch (phase) {
    case VIVID_RENDERER_HOST_BOOT: return "BOOT";
    case VIVID_RENDERER_HOST_WAIT_INIT: return "WAIT_INIT";
    case VIVID_RENDERER_HOST_BACKEND_INIT: return "BACKEND_INIT";
    case VIVID_RENDERER_HOST_WAIT_NEGOTIATION: return "WAIT_NEGOTIATION";
    case VIVID_RENDERER_HOST_RUNNING: return "RUNNING";
    case VIVID_RENDERER_HOST_QUIESCING: return "QUIESCING";
    case VIVID_RENDERER_HOST_QUIESCED: return "QUIESCED";
    case VIVID_RENDERER_HOST_SHUTDOWN: return "SHUTDOWN";
    case VIVID_RENDERER_HOST_EXIT: return "EXIT";
    default: return "UNKNOWN";
    }
}

static void
host_transition(VividRendererHost* host, VividRendererHostPhase phase)
{
    g_mutex_lock(&host->publish_lock);
    const VividRendererHostPhase old_phase = host->phase;
    if (old_phase == phase) {
        g_mutex_unlock(&host->publish_lock);
        return;
    }
    host->phase = phase;
    g_mutex_unlock(&host->publish_lock);
    g_message("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
              " pid=%d lifecycle=%s->%s",
              host->renderer_id,
              host->instance_id,
              getpid(),
              host_phase_name(old_phase),
              host_phase_name(phase));
}

static void
host_event_free(VividRendererHostEvent* event)
{
    if (!event)
        return;
    vivid_renderer_transport_close_fds(event->fds, event->n_fds);
    g_free(event->payload);
    g_free(event);
}

static void
wake_host(VividRendererHost* host)
{
    const guint64 value = 1;
    if (write(host->wake_fd, &value, sizeof(value)) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
        g_warning("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
                  " event wake failed: %s",
                  host->renderer_id,
                  host->instance_id,
                  g_strerror(errno));
    }
}

static void
enqueue_internal_event(VividRendererHost* host, VividRendererHostEvent* event)
{
    g_async_queue_push(host->events, event);
    wake_host(host);
}

static gboolean
validate_event_for_current_transaction(VividRendererHost* host,
                                       guint16 opcode,
                                       guint64 request_id)
{
    switch (opcode) {
    case VIVID_RENDERER_MSG_READY:
    case VIVID_RENDERER_MSG_FORMAT_CAPS:
    case VIVID_RENDERER_MSG_RELEASE_TIMELINE:
        return host->phase == VIVID_RENDERER_HOST_BACKEND_INIT &&
            request_id == host->init_request_id;
    case VIVID_RENDERER_MSG_BIND_BUFFERS:
    case VIVID_RENDERER_MSG_BIND_FAILED:
        return (host->phase == VIVID_RENDERER_HOST_RUNNING ||
                host->phase == VIVID_RENDERER_HOST_WAIT_NEGOTIATION) &&
            request_id == host->negotiate_request_id;
    case VIVID_RENDERER_MSG_FRAME_READY:
    case VIVID_RENDERER_MSG_STATE_CHANGED:
        return host->phase == VIVID_RENDERER_HOST_BACKEND_INIT ||
            host->phase == VIVID_RENDERER_HOST_WAIT_NEGOTIATION ||
            host->phase == VIVID_RENDERER_HOST_RUNNING;
    case VIVID_RENDERER_MSG_ERROR:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
create_event(VividRendererHost* host,
             guint16 opcode,
             guint64 request_id,
             const void* payload,
             size_t payload_length,
             const gint* fds,
             size_t n_fds,
             VividRendererHostEvent** out_event,
             GError** error)
{
    if (!host || !out_event ||
        !vivid_renderer_fd_count_valid(opcode,
                                       (const guint8*)payload,
                                       payload_length,
                                       n_fds) ||
        !validate_event_for_current_transaction(host, opcode, request_id)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "worker event violates the generated protocol or host transaction");
        return FALSE;
    }

    VividRendererHostEvent* event = g_new0(VividRendererHostEvent, 1);
    event->opcode = opcode;
    event->request_id = request_id;
    event->payload_length = payload_length;
    event->payload = payload_length == 0 ? NULL : g_memdup2(payload, payload_length);
    event->n_fds = n_fds;
    for (size_t i = 0; i < VIVID_RENDERER_MAX_FDS_PER_MESSAGE; i++)
        event->fds[i] = -1;
    for (size_t i = 0; i < n_fds; i++) {
        event->fds[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
        if (event->fds[i] < 0) {
            const gint saved_errno = errno;
            host_event_free(event);
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(saved_errno),
                        "failed to duplicate worker event FD: %s",
                        g_strerror(saved_errno));
            return FALSE;
        }
    }
    *out_event = event;
    return TRUE;
}

gboolean
vivid_renderer_host_publish_borrowed(VividRendererHost* host,
                                     guint16 opcode,
                                     guint64 request_id,
                                     const void* payload,
                                     size_t payload_length,
                                     const gint* fds,
                                     size_t n_fds,
                                     GError** error)
{
    if (!host || (n_fds != 0 && !fds)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "worker event requires a host and valid FD array");
        return FALSE;
    }

    g_mutex_lock(&host->publish_lock);
    if (!host->accepting_backend_events) {
        g_mutex_unlock(&host->publish_lock);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "worker host is no longer accepting backend events");
        return FALSE;
    }
    VividRendererHostEvent* event = NULL;
    const gboolean created = create_event(host,
                                          opcode,
                                          request_id,
                                          payload,
                                          payload_length,
                                          fds,
                                          n_fds,
                                          &event,
                                          error);
    if (created)
        enqueue_internal_event(host, event);
    g_mutex_unlock(&host->publish_lock);
    return created;
}

gboolean
vivid_renderer_host_publish_moved(VividRendererHost* host,
                                  guint16 opcode,
                                  guint64 request_id,
                                  const void* payload,
                                  size_t payload_length,
                                  gint* fds,
                                  size_t n_fds,
                                  GError** error)
{
    const gboolean published = vivid_renderer_host_publish_borrowed(host,
                                                                    opcode,
                                                                    request_id,
                                                                    payload,
                                                                    payload_length,
                                                                    fds,
                                                                    n_fds,
                                                                    error);
    vivid_renderer_transport_close_fds(fds, n_fds);
    return published;
}

static void
note_sent_event(VividRendererHost* host, const VividRendererHostEvent* event)
{
    switch (event->opcode) {
    case VIVID_RENDERER_MSG_READY:
        host->ready_sent = TRUE;
        break;
    case VIVID_RENDERER_MSG_FORMAT_CAPS:
        host->format_caps_sent = TRUE;
        break;
    case VIVID_RENDERER_MSG_RELEASE_TIMELINE:
        host->release_timeline_sent = TRUE;
        break;
    case VIVID_RENDERER_MSG_QUIESCED:
        host_transition(host, VIVID_RENDERER_HOST_QUIESCED);
        break;
    default:
        break;
    }
}

static gint
flush_events(VividRendererHost* host)
{
    for (;;) {
        if (!host->pending_send)
            host->pending_send = g_async_queue_try_pop(host->events);
        if (!host->pending_send)
            return VIVID_RENDERER_TRANSPORT_OK;
        VividRendererHostEvent* event = host->pending_send;
        const gint result = vivid_renderer_transport_send_borrowed(
            host->ipc_fd,
            host->instance_id,
            VIVID_RENDERER_DIRECTION_WORKER_TO_DAEMON,
            event->opcode,
            event->request_id,
            event->payload,
            event->payload_length,
            event->fds,
            event->n_fds);
        if (result == VIVID_RENDERER_TRANSPORT_WOULD_BLOCK)
            return result;
        if (result < 0)
            return result;
        note_sent_event(host, event);
        host->pending_send = NULL;
        host_event_free(event);
    }
}

static gboolean
event_queue_empty(VividRendererHost* host)
{
    return !host->pending_send && g_async_queue_length(host->events) <= 0;
}

static gboolean
parse_u64(const gchar* value, guint64* out_value)
{
    if (!value || !*value)
        return FALSE;
    errno = 0;
    gchar* end = NULL;
    const guint64 parsed = g_ascii_strtoull(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0u)
        return FALSE;
    *out_value = parsed;
    return TRUE;
}

static gboolean
parse_int_fd(const gchar* value, gint* out_fd)
{
    guint64 parsed = 0;
    if (!parse_u64(value, &parsed) || parsed > G_MAXINT)
        return FALSE;
    *out_fd = (gint)parsed;
    return TRUE;
}

static gboolean
parse_host_arguments(gint argc,
                     gchar** argv,
                     gint* out_ipc_fd,
                     gchar** out_renderer_id,
                     guint64* out_instance_id)
{
    const gchar* ipc_prefix = "--renderer-ipc-fd=";
    const gchar* id_prefix = "--renderer-id=";
    const gchar* instance_prefix = "--renderer-instance-id=";
    for (gint i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], ipc_prefix) && *out_ipc_fd < 0) {
            if (!parse_int_fd(argv[i] + strlen(ipc_prefix), out_ipc_fd))
                return FALSE;
        } else if (g_str_has_prefix(argv[i], id_prefix) && !*out_renderer_id) {
            const gchar* value = argv[i] + strlen(id_prefix);
            if (!*value)
                return FALSE;
            *out_renderer_id = g_strdup(value);
        } else if (g_str_has_prefix(argv[i], instance_prefix) &&
                   *out_instance_id == 0u) {
            if (!parse_u64(argv[i] + strlen(instance_prefix), out_instance_id))
                return FALSE;
        } else {
            return FALSE;
        }
    }
    return *out_ipc_fd >= 0 && *out_renderer_id && *out_instance_id != 0u;
}

gboolean
vivid_renderer_host_parse_init(const VividRendererPacket* packet,
                               VividRendererHostInit* init)
{
    if (!packet || !init || packet->header.opcode != VIVID_RENDERER_MSG_INIT ||
        !vivid_renderer_payload_length_valid(packet->header.opcode,
                                             packet->payload,
                                             packet->payload_length)) {
        return FALSE;
    }
    memset(init, 0, sizeof(*init));
    init->renderer_kind = (VividRendererKind)vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_INIT_RENDERER_KIND_OFFSET);
    init->width = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_INIT_WIDTH_OFFSET);
    init->height = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_INIT_HEIGHT_OFFSET);
    init->scale = vivid_renderer_wire_read_f32(
        packet->payload + VIVID_RENDERER_INIT_SCALE_OFFSET);
    init->fps = vivid_renderer_wire_read_f32(
        packet->payload + VIVID_RENDERER_INIT_FPS_OFFSET);
    init->playback_flags = vivid_renderer_wire_read_u32(
        packet->payload + VIVID_RENDERER_INIT_PLAYBACK_FLAGS_OFFSET);
    init->volume = vivid_renderer_wire_read_f32(
        packet->payload + VIVID_RENDERER_INIT_VOLUME_OFFSET);
    memcpy(init->expected_device_uuid,
           packet->payload + VIVID_RENDERER_INIT_EXPECTED_DEVICE_UUID_OFFSET,
           sizeof(init->expected_device_uuid));

    const guint32 lengths[] = {
        vivid_renderer_wire_read_u32(packet->payload +
                                     VIVID_RENDERER_INIT_RENDERER_ID_LENGTH_OFFSET),
        vivid_renderer_wire_read_u32(packet->payload +
                                     VIVID_RENDERER_INIT_PROJECT_PATH_LENGTH_OFFSET),
        vivid_renderer_wire_read_u32(packet->payload +
                                     VIVID_RENDERER_INIT_RENDER_NODE_LENGTH_OFFSET),
        vivid_renderer_wire_read_u32(packet->payload +
                                     VIVID_RENDERER_INIT_SETTINGS_JSON_LENGTH_OFFSET),
        vivid_renderer_wire_read_u32(packet->payload +
                                     VIVID_RENDERER_INIT_PROPERTIES_JSON_LENGTH_OFFSET),
    };
    VividRendererByteView* views[] = {
        &init->renderer_id,
        &init->project_path,
        &init->render_node,
        &init->settings_json,
        &init->properties_json,
    };
    const guint8* cursor = packet->payload + VIVID_RENDERER_INIT_FIXED_BYTES;
    for (guint i = 0; i < G_N_ELEMENTS(views); i++) {
        views[i]->data = cursor;
        views[i]->length = lengths[i];
        cursor += lengths[i];
    }
    return cursor == packet->payload + packet->payload_length;
}

static gboolean
enqueue_error(VividRendererHost* host,
              guint64 request_id,
              VividRendererErrorCode error_code,
              VividRendererLifecycleStage stage,
              const gchar* diagnostic)
{
    g_mutex_lock(&host->publish_lock);
    host->accepting_backend_events = FALSE;
    g_mutex_unlock(&host->publish_lock);
    const gchar* text = diagnostic && *diagnostic ? diagnostic : "renderer host error";
    const gsize text_length = MIN(strlen(text), VIVID_RENDERER_MAX_DIAGNOSTIC_BYTES);
    const gsize payload_length = VIVID_RENDERER_ERROR_FIXED_BYTES + text_length;
    g_autofree guint8* payload = g_malloc0(payload_length);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_ERROR_ERROR_CODE_OFFSET,
                                  error_code);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_ERROR_STAGE_OFFSET,
                                  stage);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_ERROR_DIAGNOSTIC_LENGTH_OFFSET,
                                  (guint32)text_length);
    memcpy(payload + VIVID_RENDERER_ERROR_FIXED_BYTES, text, text_length);

    VividRendererHostEvent* event = NULL;
    if (!create_event(host,
                      VIVID_RENDERER_MSG_ERROR,
                      request_id,
                      payload,
                      payload_length,
                      NULL,
                      0,
                      &event,
                      NULL)) {
        return FALSE;
    }
    enqueue_internal_event(host, event);
    host->stop_after_flush = TRUE;
    host->exit_code = 1;
    return TRUE;
}

static gboolean
send_hello(VividRendererHost* host)
{
    const gsize id_length = strlen(host->renderer_id);
    const gsize payload_length = VIVID_RENDERER_HELLO_FIXED_BYTES + id_length;
    g_autofree guint8* payload = g_malloc0(payload_length);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_HELLO_SPAWN_VERSION_OFFSET,
                                  VIVID_RENDERER_SPAWN_VERSION);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_HELLO_RENDERER_KIND_OFFSET,
                                  host->backend->kind);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_HELLO_PID_OFFSET,
                                  (guint32)getpid());
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_HELLO_RENDERER_ID_LENGTH_OFFSET,
                                  (guint32)id_length);
    memcpy(payload + VIVID_RENDERER_HELLO_FIXED_BYTES,
           host->renderer_id,
           id_length);
    return vivid_renderer_transport_send_borrowed(
               host->ipc_fd,
               host->instance_id,
               VIVID_RENDERER_DIRECTION_WORKER_TO_DAEMON,
               VIVID_RENDERER_MSG_HELLO,
               0,
               payload,
               payload_length,
               NULL,
               0) == VIVID_RENDERER_TRANSPORT_OK;
}

static gboolean
handle_init(VividRendererHost* host, const VividRendererPacket* packet)
{
    if (host->phase != VIVID_RENDERER_HOST_WAIT_INIT)
        return FALSE;
    VividRendererHostInit init;
    if (!vivid_renderer_host_parse_init(packet, &init) ||
        init.renderer_kind != host->backend->kind ||
        init.renderer_id.length != strlen(host->renderer_id) ||
        memcmp(init.renderer_id.data,
               host->renderer_id,
               init.renderer_id.length) != 0) {
        enqueue_error(host,
                      packet->header.request_id,
                      VIVID_RENDERER_ERROR_IDENTITY_MISMATCH,
                      VIVID_RENDERER_STAGE_HANDSHAKE,
                      "INIT renderer identity does not match the spawned executable");
        return TRUE;
    }

    g_mutex_lock(&host->publish_lock);
    host->init_request_id = packet->header.request_id;
    g_mutex_unlock(&host->publish_lock);
    host_transition(host, VIVID_RENDERER_HOST_BACKEND_INIT);
    g_autoptr(GError) error = NULL;
    if (!host->backend->initialize ||
        !host->backend->initialize(host,
                                   &init,
                                   packet->header.request_id,
                                   host->backend_data,
                                   &error)) {
        enqueue_error(host,
                      packet->header.request_id,
                      VIVID_RENDERER_ERROR_BACKEND_INIT,
                      VIVID_RENDERER_STAGE_BACKEND_INIT,
                      error ? error->message : "renderer backend initialization failed");
        return TRUE;
    }
    host->init_callback_complete = TRUE;
    return TRUE;
}

static gboolean
handle_negotiate(VividRendererHost* host, const VividRendererPacket* packet)
{
    if (host->phase != VIVID_RENDERER_HOST_WAIT_NEGOTIATION &&
        host->phase != VIVID_RENDERER_HOST_RUNNING) {
        return FALSE;
    }
    g_mutex_lock(&host->publish_lock);
    host->negotiate_request_id = packet->header.request_id;
    g_mutex_unlock(&host->publish_lock);
    host_transition(host, VIVID_RENDERER_HOST_RUNNING);
    g_autoptr(GError) error = NULL;
    if (!host->backend->negotiate_buffers ||
        !host->backend->negotiate_buffers(host,
                                          packet,
                                          host->backend_data,
                                          &error)) {
        enqueue_error(host,
                      packet->header.request_id,
                      VIVID_RENDERER_ERROR_BUFFER_ALLOCATION,
                      VIVID_RENDERER_STAGE_NEGOTIATION,
                      error ? error->message : "renderer buffer negotiation failed");
    }
    return TRUE;
}

static gboolean
handle_quiesce(VividRendererHost* host, const VividRendererPacket* packet)
{
    if (host->phase != VIVID_RENDERER_HOST_WAIT_NEGOTIATION &&
        host->phase != VIVID_RENDERER_HOST_RUNNING) {
        return FALSE;
    }
    host_transition(host, VIVID_RENDERER_HOST_QUIESCING);
    g_mutex_lock(&host->publish_lock);
    host->accepting_backend_events = FALSE;
    g_mutex_unlock(&host->publish_lock);

    g_autoptr(GError) error = NULL;
    if (!host->backend->quiesce ||
        !host->backend->quiesce(host, host->backend_data, &error)) {
        enqueue_error(host,
                      packet->header.request_id,
                      VIVID_RENDERER_ERROR_QUIESCE,
                      VIVID_RENDERER_STAGE_QUIESCE,
                      error ? error->message : "renderer backend failed to quiesce");
        return TRUE;
    }
    host->quiesce_request_id = packet->header.request_id;
    host->quiesce_reply_pending = TRUE;
    return TRUE;
}

static gboolean
handle_shutdown(VividRendererHost* host, const VividRendererPacket* packet)
{
    (void)packet;
    if (host->phase != VIVID_RENDERER_HOST_QUIESCED)
        return FALSE;
    host_transition(host, VIVID_RENDERER_HOST_SHUTDOWN);
    if (host->backend->shutdown)
        host->backend->shutdown(host, host->backend_data);
    host_transition(host, VIVID_RENDERER_HOST_EXIT);
    host->exit_code = 0;
    return TRUE;
}

static gboolean
handle_runtime(VividRendererHost* host, const VividRendererPacket* packet)
{
    if (host->phase != VIVID_RENDERER_HOST_WAIT_NEGOTIATION &&
        host->phase != VIVID_RENDERER_HOST_RUNNING) {
        return FALSE;
    }
    g_autoptr(GError) error = NULL;
    if (!host->backend->runtime_command ||
        !host->backend->runtime_command(host,
                                        packet,
                                        host->backend_data,
                                        &error)) {
        enqueue_error(host,
                      packet->header.request_id,
                      VIVID_RENDERER_ERROR_PROTOCOL,
                      VIVID_RENDERER_STAGE_RUNNING,
                      error ? error->message : "renderer rejected runtime command");
    }
    return TRUE;
}

static gboolean
handle_command(VividRendererHost* host, const VividRendererPacket* packet)
{
    switch (packet->header.opcode) {
    case VIVID_RENDERER_MSG_INIT:
        return handle_init(host, packet);
    case VIVID_RENDERER_MSG_NEGOTIATE_BUFFERS:
        return handle_negotiate(host, packet);
    case VIVID_RENDERER_MSG_QUIESCE:
        return handle_quiesce(host, packet);
    case VIVID_RENDERER_MSG_SHUTDOWN:
        return handle_shutdown(host, packet);
    case VIVID_RENDERER_MSG_SET_RUNTIME:
    case VIVID_RENDERER_MSG_SET_PLAYBACK:
    case VIVID_RENDERER_MSG_SET_MEDIA_STATE:
    case VIVID_RENDERER_MSG_SET_AUDIO_SAMPLES:
    case VIVID_RENDERER_MSG_POINTER_MOTION:
    case VIVID_RENDERER_MSG_POINTER_BUTTON:
    case VIVID_RENDERER_MSG_POINTER_AXIS:
        return handle_runtime(host, packet);
    default:
        return FALSE;
    }
}

static gboolean
drain_commands(VividRendererHost* host)
{
    for (;;) {
        VividRendererPacket packet;
        vivid_renderer_packet_init(&packet);
        const gint result = vivid_renderer_transport_receive(
            host->ipc_fd,
            host->instance_id,
            VIVID_RENDERER_DIRECTION_DAEMON_TO_WORKER,
            &packet);
        if (result == VIVID_RENDERER_TRANSPORT_WOULD_BLOCK) {
            vivid_renderer_packet_clear(&packet);
            return TRUE;
        }
        if (result == VIVID_RENDERER_TRANSPORT_EOF) {
            vivid_renderer_packet_clear(&packet);
            return FALSE;
        }
        if (result < 0) {
            g_warning("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
                      " phase=%s receive failed: %s",
                      host->renderer_id,
                      host->instance_id,
                      host_phase_name(host->phase),
                      g_strerror(-result));
            vivid_renderer_packet_clear(&packet);
            return FALSE;
        }
        const guint16 opcode = packet.header.opcode;
        if (!handle_command(host, &packet)) {
            enqueue_error(host,
                          packet.header.request_id,
                          VIVID_RENDERER_ERROR_PROTOCOL,
                          VIVID_RENDERER_STAGE_HANDSHAKE,
                          "daemon command violates the worker lifecycle state");
            vivid_renderer_packet_clear(&packet);
            g_warning("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
                      " rejected opcode=0x%04x phase=%s",
                      host->renderer_id,
                      host->instance_id,
                      opcode,
                      host_phase_name(host->phase));
            return TRUE;
        }
        vivid_renderer_packet_clear(&packet);
        if (host->phase == VIVID_RENDERER_HOST_EXIT)
            return TRUE;
    }
}

static void
drain_wake_fd(VividRendererHost* host)
{
    guint64 value = 0;
    while (read(host->wake_fd, &value, sizeof(value)) > 0) {
    }
}

static gboolean
enqueue_quiesced_if_ready(VividRendererHost* host)
{
    if (!host->quiesce_reply_pending || !event_queue_empty(host))
        return TRUE;
    VividRendererHostEvent* event = g_new0(VividRendererHostEvent, 1);
    event->opcode = VIVID_RENDERER_MSG_QUIESCED;
    event->request_id = host->quiesce_request_id;
    for (size_t i = 0; i < VIVID_RENDERER_MAX_FDS_PER_MESSAGE; i++)
        event->fds[i] = -1;
    if (!vivid_renderer_fd_count_valid(event->opcode, NULL, 0, 0)) {
        host_event_free(event);
        return FALSE;
    }
    host->quiesce_reply_pending = FALSE;
    enqueue_internal_event(host, event);
    return TRUE;
}

static gboolean
finish_init_if_ready(VividRendererHost* host)
{
    if (host->phase != VIVID_RENDERER_HOST_BACKEND_INIT ||
        !host->init_callback_complete || !event_queue_empty(host)) {
        return TRUE;
    }
    if (!host->ready_sent || !host->format_caps_sent ||
        !host->release_timeline_sent) {
        enqueue_error(host,
                      host->init_request_id,
                      VIVID_RENDERER_ERROR_BACKEND_INIT,
                      VIVID_RENDERER_STAGE_BACKEND_INIT,
                      "backend returned from initialize without READY, FORMAT_CAPS, and RELEASE_TIMELINE");
        return TRUE;
    }
    host_transition(host, VIVID_RENDERER_HOST_WAIT_NEGOTIATION);
    return TRUE;
}

static gint
run_host_loop(VividRendererHost* host)
{
    while (host->phase != VIVID_RENDERER_HOST_EXIT) {
        const gint flush_result = flush_events(host);
        if (flush_result < 0) {
            g_warning("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
                      " send failed: %s",
                      host->renderer_id,
                      host->instance_id,
                      g_strerror(-flush_result));
            return 1;
        }
        if (host->stop_after_flush && event_queue_empty(host))
            return host->exit_code;
        if (!finish_init_if_ready(host) || !enqueue_quiesced_if_ready(host))
            return 1;

        struct pollfd poll_fds[2] = {
            {
                .fd = host->ipc_fd,
                .events = POLLIN | (event_queue_empty(host) ? 0 : POLLOUT),
            },
            {
                .fd = host->wake_fd,
                .events = POLLIN,
            },
        };
        gint poll_result;
        do {
            poll_result = poll(poll_fds, G_N_ELEMENTS(poll_fds), -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            g_warning("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
                      " poll failed: %s",
                      host->renderer_id,
                      host->instance_id,
                      g_strerror(errno));
            return 1;
        }
        if ((poll_fds[1].revents & POLLIN) != 0)
            drain_wake_fd(host);
        if ((poll_fds[0].revents & POLLIN) != 0 && !drain_commands(host))
            return 1;
        if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return host->phase == VIVID_RENDERER_HOST_EXIT ? host->exit_code : 1;
    }
    return host->exit_code;
}

static gboolean
prepare_worker_process(gint ipc_fd)
{
    const pid_t original_parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) < 0)
        return FALSE;
    if (original_parent == 1 || getppid() != original_parent) {
        raise(SIGTERM);
        return FALSE;
    }
    if (setpgid(0, 0) < 0 || getpgrp() != getpid())
        return FALSE;
    g_autofree gchar* process_group = g_strdup_printf("%d", getpgrp());
    if (!g_setenv(VIVID_RENDERER_PROCESS_GROUP_ENV, process_group, TRUE))
        return FALSE;
    const gint fd_flags = fcntl(ipc_fd, F_GETFD);
    if (fd_flags < 0 || fcntl(ipc_fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        return FALSE;
    struct sigaction ignore_sigpipe = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore_sigpipe.sa_mask);
    return sigaction(SIGPIPE, &ignore_sigpipe, NULL) == 0;
}

gint
vivid_renderer_host_run(gint argc,
                        gchar** argv,
                        const VividRendererHostBackend* backend,
                        gpointer backend_data)
{
    if (!backend || !backend->renderer_id || !*backend->renderer_id ||
        backend->kind < VIVID_RENDERER_KIND_SCENE ||
        backend->kind > VIVID_RENDERER_KIND_VIDEO) {
        g_printerr("VividRendererHost: invalid compiled backend descriptor\n");
        return 2;
    }

    gint ipc_fd = -1;
    g_autofree gchar* renderer_id = NULL;
    guint64 instance_id = 0;
    if (!parse_host_arguments(argc,
                              argv,
                              &ipc_fd,
                              &renderer_id,
                              &instance_id) ||
        g_strcmp0(renderer_id, backend->renderer_id) != 0) {
        g_printerr("VividRendererHost: invalid spawn arguments\n");
        return 2;
    }
    if (!prepare_worker_process(ipc_fd)) {
        g_printerr("VividRendererHost: failed to establish worker lifecycle: %s\n",
                   g_strerror(errno));
        return 2;
    }

    VividRendererHost host;
    memset(&host, 0, sizeof(host));
    host.ipc_fd = ipc_fd;
    host.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    host.renderer_id = g_strdup(renderer_id);
    host.instance_id = instance_id;
    host.backend = backend;
    host.backend_data = backend_data;
    host.phase = VIVID_RENDERER_HOST_BOOT;
    host.events = g_async_queue_new();
    host.accepting_backend_events = TRUE;
    host.exit_code = 1;
    g_mutex_init(&host.publish_lock);
    if (host.wake_fd < 0) {
        g_printerr("VividRendererHost: eventfd failed: %s\n", g_strerror(errno));
        g_async_queue_unref(host.events);
        g_mutex_clear(&host.publish_lock);
        g_free(host.renderer_id);
        return 2;
    }

    g_message("VividRendererHost: renderer=%s instance=%" G_GUINT64_FORMAT
              " pid=%d pgid=%d ipc-fd=%d",
              host.renderer_id,
              host.instance_id,
              getpid(),
              getpgrp(),
              host.ipc_fd);
    if (!send_hello(&host)) {
        g_printerr("VividRendererHost: failed to send HELLO\n");
        host.stop_after_flush = TRUE;
    } else {
        host_transition(&host, VIVID_RENDERER_HOST_WAIT_INIT);
    }
    const gint exit_code = host.stop_after_flush ? 1 : run_host_loop(&host);

    g_mutex_lock(&host.publish_lock);
    host.accepting_backend_events = FALSE;
    g_mutex_unlock(&host.publish_lock);
    host_event_free(host.pending_send);
    VividRendererHostEvent* queued = NULL;
    while ((queued = g_async_queue_try_pop(host.events)) != NULL)
        host_event_free(queued);
    g_async_queue_unref(host.events);
    close(host.wake_fd);
    close(host.ipc_fd);
    g_mutex_clear(&host.publish_lock);
    g_free(host.renderer_id);
    return exit_code;
}

const gchar*
vivid_renderer_host_renderer_id(const VividRendererHost* host)
{
    return host ? host->renderer_id : NULL;
}

guint64
vivid_renderer_host_instance_id(const VividRendererHost* host)
{
    return host ? host->instance_id : 0;
}
