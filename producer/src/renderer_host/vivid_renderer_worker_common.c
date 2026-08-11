#include "vivid_renderer_worker_common.h"

#include <gio/gio.h>
#include <xf86drm.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static gchar*
byte_view_dup(const VividRendererByteView* view)
{
    return view ? g_strndup((const gchar*)view->data, view->length) : NULL;
}

static gboolean
resolve_expected_gpu(const VividRendererHostInit* init,
                     VividGpuDevice* out_gpu,
                     gchar** out_render_node,
                     GError** error)
{
    g_autofree gchar* render_node = byte_view_dup(&init->render_node);
    VividGpuDeviceList devices;
    if (!vivid_gpu_devices_enumerate(&devices) ||
        !vivid_gpu_devices_resolve(&devices, render_node, out_gpu)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "worker cannot resolve expected render node '%s'",
                    render_node);
        return FALSE;
    }
    if (memcmp(out_gpu->uuid,
               init->expected_device_uuid,
               VIVID_RENDERER_UUID_BYTES) != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "worker GPU UUID does not match INIT render node '%s'",
                    render_node);
        return FALSE;
    }
    *out_render_node = g_steal_pointer(&render_node);
    return TRUE;
}

gboolean
vivid_renderer_worker_common_init(VividRendererWorkerCommon* common,
                                  VividRendererHost* host,
                                  const VividRendererHostInit* init,
                                  GError** error)
{
    if (!common || !host || !init) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "worker common initialization requires host and INIT data");
        return FALSE;
    }
    memset(common, 0, sizeof(*common));
    common->drm_fd = -1;
    common->host = host;
    g_mutex_init(&common->release_lock);
    if (!resolve_expected_gpu(init,
                              &common->gpu,
                              &common->render_node,
                              error)) {
        vivid_renderer_worker_common_clear(common);
        return FALSE;
    }

    common->project_path = byte_view_dup(&init->project_path);
    common->settings_json = byte_view_dup(&init->settings_json);
    common->properties_json = byte_view_dup(&init->properties_json);
    common->drm_fd = open(common->render_node, O_RDWR | O_CLOEXEC);
    if (common->drm_fd < 0) {
        const gint saved_errno = errno;
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_errno),
                    "worker cannot open render node '%s': %s",
                    common->render_node,
                    g_strerror(saved_errno));
        vivid_renderer_worker_common_clear(common);
        return FALSE;
    }
    if (!vivid_renderer_release_timeline_init(&common->release_timeline,
                                              common->drm_fd,
                                              error)) {
        vivid_renderer_worker_common_clear(common);
        return FALSE;
    }
    common->release_timeline_initialized = TRUE;
    g_message("VividRendererWorkerCommon: renderer=%s instance=%" G_GUINT64_FORMAT
              " render-node=%s drm=%u:%u project=%s",
              vivid_renderer_host_renderer_id(host),
              vivid_renderer_host_instance_id(host),
              common->render_node,
              common->gpu.drm_render_major,
              common->gpu.drm_render_minor,
              common->project_path);
    return TRUE;
}

static gboolean
publish_ready(VividRendererWorkerCommon* common,
              guint64 request_id,
              GError** error)
{
    const gsize render_node_length = strlen(common->render_node);
    const gsize payload_length = VIVID_RENDERER_READY_FIXED_BYTES +
        render_node_length;
    g_autofree guint8* payload = g_malloc0(payload_length);
    memcpy(payload + VIVID_RENDERER_READY_DEVICE_UUID_OFFSET,
           common->gpu.uuid,
           VIVID_RENDERER_UUID_BYTES);
    memcpy(payload + VIVID_RENDERER_READY_DRIVER_UUID_OFFSET,
           common->gpu.driver_uuid,
           VIVID_RENDERER_UUID_BYTES);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_READY_DRM_RENDER_MAJOR_OFFSET,
                                  common->gpu.drm_render_major);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_READY_DRM_RENDER_MINOR_OFFSET,
                                  common->gpu.drm_render_minor);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_READY_RENDER_NODE_LENGTH_OFFSET,
                                  (guint32)render_node_length);
    memcpy(payload + VIVID_RENDERER_READY_FIXED_BYTES,
           common->render_node,
           render_node_length);
    return vivid_renderer_host_publish_borrowed(common->host,
                                                VIVID_RENDERER_MSG_READY,
                                                request_id,
                                                payload,
                                                payload_length,
                                                NULL,
                                                0,
                                                error);
}

static gboolean
publish_format_caps(VividRendererWorkerCommon* common,
                    guint64 request_id,
                    const VividRendererWorkerFormatCap* caps,
                    guint32 n_caps,
                    guint32 memory_hints,
                    guint32 color_caps,
                    guint32 relay_modes,
                    guint32 extent_max_width,
                    guint32 extent_max_height,
                    guint32 pool_size_min,
                    guint32 pool_size_max,
                    GError** error)
{
    if (!caps || n_caps == 0 || n_caps > VIVID_RENDERER_MAX_FORMAT_CAPS) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "worker must publish at least one bounded format capability");
        return FALSE;
    }
    const gsize payload_length = VIVID_RENDERER_FORMAT_CAPS_FIXED_BYTES +
        (gsize)n_caps * VIVID_RENDERER_FORMAT_CAPABILITY_BYTES;
    g_autofree guint8* payload = g_malloc0(payload_length);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_MEMORY_HINTS_OFFSET,
                                  memory_hints);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_SYNC_CAPS_OFFSET,
                                  VIVID_RENDERER_SYNC_CAP_SYNCOBJ_BINARY |
                                      VIVID_RENDERER_SYNC_CAP_SYNCOBJ_TIMELINE);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_COLOR_CAPS_OFFSET,
                                  color_caps);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_RELAY_MODES_OFFSET,
                                  relay_modes);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_EXTENT_MAX_WIDTH_OFFSET,
                                  extent_max_width);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_EXTENT_MAX_HEIGHT_OFFSET,
                                  extent_max_height);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_POOL_SIZE_MIN_OFFSET,
                                  pool_size_min);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_POOL_SIZE_MAX_OFFSET,
                                  pool_size_max);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FORMAT_CAPS_CAPABILITY_COUNT_OFFSET,
                                  n_caps);
    for (guint32 i = 0; i < n_caps; i++) {
        guint8* record = payload + VIVID_RENDERER_FORMAT_CAPS_FIXED_BYTES +
            (gsize)i * VIVID_RENDERER_FORMAT_CAPABILITY_BYTES;
        vivid_renderer_wire_write_u32(
            record + VIVID_RENDERER_FORMAT_CAPABILITY_FOURCC_OFFSET,
            caps[i].fourcc);
        vivid_renderer_wire_write_u32(
            record + VIVID_RENDERER_FORMAT_CAPABILITY_PLANE_COUNT_OFFSET,
            caps[i].plane_count);
        vivid_renderer_wire_write_u64(
            record + VIVID_RENDERER_FORMAT_CAPABILITY_MODIFIER_OFFSET,
            caps[i].modifier);
    }
    return vivid_renderer_host_publish_borrowed(common->host,
                                                VIVID_RENDERER_MSG_FORMAT_CAPS,
                                                request_id,
                                                payload,
                                                payload_length,
                                                NULL,
                                                0,
                                                error);
}

gboolean
vivid_renderer_worker_common_publish_handshake(
    VividRendererWorkerCommon* common,
    guint64 request_id,
    const VividRendererWorkerFormatCap* caps,
    guint32 n_caps,
    guint32 memory_hints,
    guint32 color_caps,
    guint32 relay_modes,
    guint32 extent_max_width,
    guint32 extent_max_height,
    guint32 pool_size_min,
    guint32 pool_size_max,
    GError** error)
{
    if (!common || !common->release_timeline_initialized ||
        !publish_ready(common, request_id, error) ||
        !publish_format_caps(common,
                             request_id,
                             caps,
                             n_caps,
                             memory_hints,
                             color_caps,
                             relay_modes,
                             extent_max_width,
                             extent_max_height,
                             pool_size_min,
                             pool_size_max,
                             error)) {
        return FALSE;
    }
    gint timeline_fd = -1;
    if (!vivid_renderer_release_timeline_take_export_fd(&common->release_timeline,
                                                        &timeline_fd,
                                                        error)) {
        return FALSE;
    }
    return vivid_renderer_host_publish_moved(common->host,
                                             VIVID_RENDERER_MSG_RELEASE_TIMELINE,
                                             request_id,
                                             NULL,
                                             0,
                                             &timeline_fd,
                                             1,
                                             error);
}

gboolean
vivid_renderer_worker_common_parse_negotiation(
    const VividRendererPacket* packet,
    VividRendererWorkerNegotiation* negotiation,
    GError** error)
{
    if (!packet || !negotiation ||
        packet->header.opcode != VIVID_RENDERER_MSG_NEGOTIATE_BUFFERS) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "worker expected NEGOTIATE_BUFFERS");
        return FALSE;
    }
    *negotiation = (VividRendererWorkerNegotiation) {
        .fourcc = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_FOURCC_OFFSET),
        .width = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_WIDTH_OFFSET),
        .height = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_HEIGHT_OFFSET),
        .plane_count = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_PLANE_COUNT_OFFSET),
        .pool_size = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_POOL_SIZE_OFFSET),
        .memory_source = (VividRendererMemorySource)vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_MEMORY_SOURCE_OFFSET),
        .sync_mode = vivid_renderer_wire_read_u32(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_SYNC_MODE_OFFSET),
        .modifier = vivid_renderer_wire_read_u64(
            packet->payload + VIVID_RENDERER_NEGOTIATE_BUFFERS_MODIFIER_OFFSET),
    };
    if (negotiation->width == 0 || negotiation->height == 0 ||
        negotiation->plane_count == 0 ||
        negotiation->plane_count > VIVID_RENDERER_MAX_PLANES ||
        negotiation->pool_size == 0 ||
        negotiation->pool_size > VIVID_RENDERER_MAX_POOL_BUFFERS ||
        negotiation->memory_source > VIVID_RENDERER_MEMORY_SOURCE_DMABUF_HEAP_RESERVED ||
        (negotiation->sync_mode & VIVID_RENDERER_SYNC_CAP_SYNCOBJ_TIMELINE) == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "NEGOTIATE_BUFFERS contains an unsupported bounded scheme");
        return FALSE;
    }
    return TRUE;
}

gboolean
vivid_renderer_worker_common_publish_pool(
    VividRendererWorkerCommon* common,
    guint64 request_id,
    const VividRendererWorkerNegotiation* negotiation,
    const VividRendererWorkerPool* pool,
    GError** error)
{
    if (!common || !negotiation || !pool || pool->n_buffers == 0 ||
        pool->n_buffers > VIVID_RENDERER_MAX_POOL_BUFFERS ||
        pool->n_buffers != negotiation->pool_size ||
        pool->width != negotiation->width || pool->height != negotiation->height ||
        pool->fourcc != negotiation->fourcc || pool->modifier != negotiation->modifier) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "backend pool does not match the negotiated scheme");
        return FALSE;
    }
    const guint32 plane_count = pool->buffers[0].n_planes;
    if (plane_count == 0 || plane_count != negotiation->plane_count)
        return FALSE;
    const gsize flat_count = (gsize)pool->n_buffers * plane_count;
    if (flat_count > VIVID_RENDERER_MAX_FDS_PER_MESSAGE)
        return FALSE;

    const gsize payload_length = VIVID_RENDERER_BIND_BUFFERS_FIXED_BYTES +
        flat_count * VIVID_RENDERER_BUFFER_PLANE_BYTES;
    g_autofree guint8* payload = g_malloc0(payload_length);
    const guint64 generation = ++common->pool_generation;
    vivid_renderer_wire_write_u64(payload +
                                      VIVID_RENDERER_BIND_BUFFERS_POOL_GENERATION_OFFSET,
                                  generation);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_BIND_BUFFERS_FOURCC_OFFSET,
                                  pool->fourcc);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_BIND_BUFFERS_WIDTH_OFFSET,
                                  pool->width);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_BIND_BUFFERS_HEIGHT_OFFSET,
                                  pool->height);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_BIND_BUFFERS_BUFFER_COUNT_OFFSET,
                                  pool->n_buffers);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_BIND_BUFFERS_PLANE_COUNT_OFFSET,
                                  plane_count);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_BIND_BUFFERS_MEMORY_SOURCE_OFFSET,
                                  negotiation->memory_source);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_BIND_BUFFERS_FLAGS_OFFSET,
                                  pool->premultiplied
                                      ? VIVID_RENDERER_BUFFER_FLAG_PREMULTIPLIED
                                      : 0u);
    vivid_renderer_wire_write_u64(payload + VIVID_RENDERER_BIND_BUFFERS_MODIFIER_OFFSET,
                                  pool->modifier);

    gint fds[VIVID_RENDERER_MAX_FDS_PER_MESSAGE];
    size_t fd_count = 0;
    for (guint32 buffer_i = 0; buffer_i < pool->n_buffers; buffer_i++) {
        const VividRendererWorkerBuffer* buffer = &pool->buffers[buffer_i];
        if (buffer->index != buffer_i || buffer->n_planes != plane_count)
            return FALSE;
        for (guint32 plane_i = 0; plane_i < plane_count; plane_i++) {
            const VividRendererWorkerPlane* plane = &buffer->planes[plane_i];
            if (plane->fd < 0 || plane->stride == 0 || plane->size == 0)
                return FALSE;
            guint8* record = payload + VIVID_RENDERER_BIND_BUFFERS_FIXED_BYTES +
                fd_count * VIVID_RENDERER_BUFFER_PLANE_BYTES;
            vivid_renderer_wire_write_u32(
                record + VIVID_RENDERER_BUFFER_PLANE_STRIDE_OFFSET,
                plane->stride);
            vivid_renderer_wire_write_u32(
                record + VIVID_RENDERER_BUFFER_PLANE_OFFSET_OFFSET,
                plane->offset);
            vivid_renderer_wire_write_u64(
                record + VIVID_RENDERER_BUFFER_PLANE_SIZE_OFFSET,
                plane->size);
            fds[fd_count++] = plane->fd;
        }
    }
    if (!vivid_renderer_host_publish_borrowed(common->host,
                                              VIVID_RENDERER_MSG_BIND_BUFFERS,
                                              request_id,
                                              payload,
                                              payload_length,
                                              fds,
                                              fd_count,
                                              error)) {
        return FALSE;
    }
    g_mutex_lock(&common->release_lock);
    common->pool_buffer_count = pool->n_buffers;
    memset(common->buffer_release_points,
           0,
           sizeof(common->buffer_release_points));
    g_mutex_unlock(&common->release_lock);
    g_message("VividRendererWorkerCommon: renderer=%s instance=%" G_GUINT64_FORMAT
              " pool-generation=%" G_GUINT64_FORMAT " buffers=%u planes=%u"
              " fourcc=0x%08x modifier=0x%016" G_GINT64_MODIFIER "x fds=%zu",
              vivid_renderer_host_renderer_id(common->host),
              vivid_renderer_host_instance_id(common->host),
              generation,
              pool->n_buffers,
              plane_count,
              pool->fourcc,
              pool->modifier,
              fd_count);
    return TRUE;
}

static gboolean
create_signaled_sync_file(VividRendererWorkerCommon* common,
                          gint* out_fd,
                          GError** error)
{
    guint32 handle = 0;
    errno = 0;
    gint result = drmSyncobjCreate(common->drm_fd,
                                   DRM_SYNCOBJ_CREATE_SIGNALED,
                                   &handle);
    if (result != 0) {
        const gint saved_error = errno != 0 ? errno : -result;
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_error),
                    "failed to create explicit signaled acquire fence: %s",
                    g_strerror(saved_error));
        return FALSE;
    }
    errno = 0;
    result = drmSyncobjExportSyncFile(common->drm_fd, handle, out_fd);
    const gint saved_error = result == 0 ? 0 : (errno != 0 ? errno : -result);
    (void)drmSyncobjDestroy(common->drm_fd, handle);
    if (result != 0 || *out_fd < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_error),
                    "failed to export explicit signaled acquire fence: %s",
                    g_strerror(saved_error));
        return FALSE;
    }
    return TRUE;
}

gboolean
vivid_renderer_worker_common_publish_frame(
    VividRendererWorkerCommon* common,
    guint32 buffer_index,
    guint64 target_time_usec,
    gint* acquire_sync_fd,
    GError** error)
{
    if (!common || !acquire_sync_fd || common->pool_generation == 0 ||
        buffer_index >= common->pool_buffer_count) {
        if (acquire_sync_fd)
            vivid_renderer_transport_close_fds(acquire_sync_fd, 1);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "worker frame does not reference the active pool");
        return FALSE;
    }
    if (*acquire_sync_fd < 0 &&
        !create_signaled_sync_file(common, acquire_sync_fd, error)) {
        return FALSE;
    }
    const guint64 release_point =
        vivid_renderer_release_timeline_allocate_point(&common->release_timeline);
    if (release_point == 0) {
        vivid_renderer_transport_close_fds(acquire_sync_fd, 1);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "worker release timeline exhausted its point space");
        return FALSE;
    }
    const guint64 sequence = ++common->next_frame_sequence;
    guint8 payload[VIVID_RENDERER_FRAME_READY_FIXED_BYTES] = {0};
    vivid_renderer_wire_write_u64(payload +
                                      VIVID_RENDERER_FRAME_READY_POOL_GENERATION_OFFSET,
                                  common->pool_generation);
    vivid_renderer_wire_write_u32(payload +
                                      VIVID_RENDERER_FRAME_READY_BUFFER_INDEX_OFFSET,
                                  buffer_index);
    vivid_renderer_wire_write_u32(payload + VIVID_RENDERER_FRAME_READY_FLAGS_OFFSET,
                                  0);
    vivid_renderer_wire_write_u64(payload + VIVID_RENDERER_FRAME_READY_SEQUENCE_OFFSET,
                                  sequence);
    vivid_renderer_wire_write_u64(payload +
                                      VIVID_RENDERER_FRAME_READY_TARGET_TIME_USEC_OFFSET,
                                  target_time_usec);
    vivid_renderer_wire_write_u64(payload +
                                      VIVID_RENDERER_FRAME_READY_RELEASE_POINT_OFFSET,
                                  release_point);

    if (!vivid_renderer_host_publish_moved(common->host,
                                           VIVID_RENDERER_MSG_FRAME_READY,
                                           0,
                                           payload,
                                           sizeof(payload),
                                           acquire_sync_fd,
                                           1,
                                           error)) {
        g_autoptr(GError) signal_error = NULL;
        if (!vivid_renderer_release_timeline_signal(&common->release_timeline,
                                                    release_point,
                                                    &signal_error)) {
            g_warning("VividRendererWorkerCommon: failed to complete rejected"
                      " release-point=%" G_GUINT64_FORMAT ": %s",
                      release_point,
                      signal_error->message);
        }
        return FALSE;
    }
    g_mutex_lock(&common->release_lock);
    common->buffer_release_points[buffer_index] = release_point;
    g_mutex_unlock(&common->release_lock);
    return TRUE;
}

static gboolean
wait_release(gpointer user_data, guint32 buffer_index, guint32 timeout_ms)
{
    VividRendererWorkerCommon* common = user_data;
    if (!common || buffer_index >= VIVID_RENDERER_MAX_POOL_BUFFERS)
        return FALSE;
    g_mutex_lock(&common->release_lock);
    const guint64 release_point = common->buffer_release_points[buffer_index];
    g_mutex_unlock(&common->release_lock);
    g_autoptr(GError) error = NULL;
    if (vivid_renderer_release_timeline_wait(&common->release_timeline,
                                             release_point,
                                             timeout_ms,
                                             &error)) {
        return TRUE;
    }
    g_warning("VividRendererWorkerCommon: renderer=%s instance=%" G_GUINT64_FORMAT
              " buffer=%u release-point=%" G_GUINT64_FORMAT " wait failed: %s",
              vivid_renderer_host_renderer_id(common->host),
              vivid_renderer_host_instance_id(common->host),
              buffer_index,
              release_point,
              error->message);
    return FALSE;
}

VividRendererReleaseGate
vivid_renderer_worker_common_release_gate(VividRendererWorkerCommon* common)
{
    return (VividRendererReleaseGate) {
        .abi_version = VIVID_RENDERER_RELEASE_GATE_ABI_VERSION,
        .user_data = common,
        .wait_release = wait_release,
    };
}

void
vivid_renderer_worker_common_clear(VividRendererWorkerCommon* common)
{
    if (!common)
        return;
    if (common->release_timeline_initialized) {
        vivid_renderer_release_timeline_clear(&common->release_timeline);
        common->release_timeline_initialized = FALSE;
    }
    if (common->drm_fd >= 0) {
        close(common->drm_fd);
        common->drm_fd = -1;
    }
    g_free(common->properties_json);
    g_free(common->settings_json);
    g_free(common->render_node);
    g_free(common->project_path);
    common->properties_json = NULL;
    common->settings_json = NULL;
    common->render_node = NULL;
    common->project_path = NULL;
    common->host = NULL;
    g_mutex_clear(&common->release_lock);
}
