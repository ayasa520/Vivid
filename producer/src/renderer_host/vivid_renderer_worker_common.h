#ifndef VIVID_RENDERER_WORKER_COMMON_H
#define VIVID_RENDERER_WORKER_COMMON_H

#include <glib.h>

#include "vivid_gpu_devices.h"
#include "vivid_renderer_host.h"
#include "vivid_renderer_release_gate.h"
#include "vivid_renderer_release_timeline.h"

G_BEGIN_DECLS

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividRendererWorkerFormatCap;

typedef struct
{
    guint32 fourcc;
    guint32 width;
    guint32 height;
    guint32 plane_count;
    guint32 pool_size;
    VividRendererMemorySource memory_source;
    guint32 sync_mode;
    guint64 modifier;
} VividRendererWorkerNegotiation;

typedef struct
{
    gint fd;
    guint32 stride;
    guint32 offset;
    guint64 size;
} VividRendererWorkerPlane;

typedef struct
{
    guint32 index;
    guint32 n_planes;
    VividRendererWorkerPlane planes[VIVID_RENDERER_MAX_PLANES];
} VividRendererWorkerBuffer;

typedef struct
{
    guint32 width;
    guint32 height;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 n_buffers;
    VividRendererWorkerBuffer buffers[VIVID_RENDERER_MAX_POOL_BUFFERS];
} VividRendererWorkerPool;

typedef struct
{
    VividRendererHost* host;
    VividGpuDevice gpu;
    gchar* project_path;
    gchar* render_node;
    gchar* settings_json;
    gchar* properties_json;
    gint drm_fd;
    VividRendererReleaseTimeline release_timeline;
    gboolean release_timeline_initialized;
    guint64 pool_generation;
    guint32 pool_buffer_count;
    guint64 next_frame_sequence;
    guint64 buffer_release_points[VIVID_RENDERER_MAX_POOL_BUFFERS];
    GMutex release_lock;
} VividRendererWorkerCommon;

gboolean vivid_renderer_worker_common_init(VividRendererWorkerCommon* common,
                                            VividRendererHost* host,
                                            const VividRendererHostInit* init,
                                            GError** error);

gboolean vivid_renderer_worker_common_publish_handshake(
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
    GError** error);

gboolean vivid_renderer_worker_common_parse_negotiation(
    const VividRendererPacket* packet,
    VividRendererWorkerNegotiation* negotiation,
    GError** error);

gboolean vivid_renderer_worker_common_publish_pool(
    VividRendererWorkerCommon* common,
    guint64 request_id,
    const VividRendererWorkerNegotiation* negotiation,
    const VividRendererWorkerPool* pool,
    GError** error);

/* acquire_sync_fd is consumed and set to -1 on every path. */
gboolean vivid_renderer_worker_common_publish_frame(
    VividRendererWorkerCommon* common,
    guint32 buffer_index,
    guint64 target_time_usec,
    gint* acquire_sync_fd,
    GError** error);

VividRendererReleaseGate vivid_renderer_worker_common_release_gate(
    VividRendererWorkerCommon* common);

void vivid_renderer_worker_common_clear(VividRendererWorkerCommon* common);

G_END_DECLS

#endif
