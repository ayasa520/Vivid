#ifndef VIVID_RENDERER_PROCESS_H
#define VIVID_RENDERER_PROCESS_H

#include <glib.h>
#include <sys/types.h>

#include "vivid_renderer_registry.h"
#include "vivid_renderer_transport.h"

G_BEGIN_DECLS

typedef struct _VividRendererProcess VividRendererProcess;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividRendererProcessFormatCap;

typedef struct
{
    guint32 n_caps;
    VividRendererProcessFormatCap caps[VIVID_RENDERER_MAX_FORMAT_CAPS];
    guint32 memory_hints;
    guint32 sync_caps;
    guint32 color_caps;
    guint32 relay_modes;
    guint32 extent_max_width;
    guint32 extent_max_height;
    guint32 pool_size_min;
    guint32 pool_size_max;
} VividRendererProcessFormatCaps;

typedef struct
{
    gint fd;
    guint32 stride;
    guint32 offset;
    guint64 size;
} VividRendererProcessPlane;

typedef struct
{
    guint32 index;
    guint32 n_planes;
    VividRendererProcessPlane planes[VIVID_RENDERER_MAX_PLANES];
} VividRendererProcessBuffer;

typedef struct
{
    guint64 generation;
    guint32 fourcc;
    guint32 width;
    guint32 height;
    guint32 memory_source;
    guint32 flags;
    guint64 modifier;
    guint32 n_buffers;
    VividRendererProcessBuffer buffers[VIVID_RENDERER_MAX_POOL_BUFFERS];
} VividRendererProcessPool;

typedef struct
{
    guint64 pool_generation;
    guint32 buffer_index;
    guint32 flags;
    guint64 sequence;
    guint64 target_time_usec;
    guint64 release_point;
    gint acquire_sync_fd;
} VividRendererProcessFrame;

typedef enum
{
    VIVID_RENDERER_PROCESS_EMPTY,
    VIVID_RENDERER_PROCESS_SPAWNING,
    VIVID_RENDERER_PROCESS_WAIT_HELLO,
    VIVID_RENDERER_PROCESS_INITIALIZING,
    VIVID_RENDERER_PROCESS_NEGOTIATING,
    VIVID_RENDERER_PROCESS_WAIT_FIRST_FRAME,
    VIVID_RENDERER_PROCESS_ACTIVE,
    VIVID_RENDERER_PROCESS_QUIESCING,
    VIVID_RENDERER_PROCESS_UNBINDING,
    VIVID_RENDERER_PROCESS_SHUTTING_DOWN,
    VIVID_RENDERER_PROCESS_FAILED,
    VIVID_RENDERER_PROCESS_CRASHED,
    VIVID_RENDERER_PROCESS_EXITED,
} VividRendererProcessState;

typedef struct
{
    guint hello_timeout_ms;
    guint init_timeout_ms;
    guint quiesce_timeout_ms;
    guint shutdown_timeout_ms;
    guint term_timeout_ms;
} VividRendererLifecyclePolicy;

typedef struct
{
    void (*state_changed)(VividRendererProcess* process,
                          VividRendererProcessState old_state,
                          VividRendererProcessState new_state,
                          gpointer user_data);
    void (*packet_received)(VividRendererProcess* process,
                            VividRendererPacket* packet,
                            gpointer user_data);
    void (*log_line)(VividRendererProcess* process,
                     const gchar* line,
                     gpointer user_data);
    void (*reaped)(VividRendererProcess* process,
                   gint wait_status,
                   gpointer user_data);
} VividRendererProcessObserver;

void vivid_renderer_lifecycle_policy_init(VividRendererLifecyclePolicy* policy);

const gchar* vivid_renderer_process_state_name(VividRendererProcessState state);
VividRendererProcessState vivid_renderer_process_state(
    const VividRendererProcess* process);
const VividRendererDescriptor* vivid_renderer_process_descriptor(
    const VividRendererProcess* process);
const gchar* vivid_renderer_process_route_id(const VividRendererProcess* process);
const gchar* vivid_renderer_process_identity_hash(
    const VividRendererProcess* process);
guint64 vivid_renderer_process_instance_id(const VividRendererProcess* process);
pid_t vivid_renderer_process_pid(const VividRendererProcess* process);
pid_t vivid_renderer_process_process_group(const VividRendererProcess* process);
gint vivid_renderer_process_pidfd(const VividRendererProcess* process);
gboolean vivid_renderer_process_is_reaped(const VividRendererProcess* process);
gint vivid_renderer_process_wait_status(const VividRendererProcess* process);
const gchar* vivid_renderer_process_last_error(const VividRendererProcess* process);
gboolean vivid_renderer_process_actual_gpu(const VividRendererProcess* process,
                                           guint8 device_uuid[VIVID_RENDERER_UUID_BYTES],
                                           guint8 driver_uuid[VIVID_RENDERER_UUID_BYTES],
                                           guint32* drm_render_major,
                                           guint32* drm_render_minor,
                                           const gchar** render_node);
const VividRendererProcessFormatCaps* vivid_renderer_process_format_caps(
    const VividRendererProcess* process);
gboolean vivid_renderer_process_copy_pool(const VividRendererProcess* process,
                                          VividRendererProcessPool* out_pool,
                                          GError** error);
void vivid_renderer_process_pool_clear(VividRendererProcessPool* pool);
gboolean vivid_renderer_process_take_frame(VividRendererProcess* process,
                                           VividRendererProcessFrame* out_frame);
guint vivid_renderer_process_pending_frame_count(
    const VividRendererProcess* process);
guint64 vivid_renderer_process_last_release_point(
    const VividRendererProcess* process);
void vivid_renderer_process_frame_clear(VividRendererProcessFrame* frame);
gboolean vivid_renderer_process_signal_release_point(VividRendererProcess* process,
                                                     guint64 release_point,
                                                     GError** error);

gboolean vivid_renderer_process_send_init(VividRendererProcess* process,
                                           const void* payload,
                                           size_t payload_length,
                                           guint64* out_request_id,
                                           GError** error);
gboolean vivid_renderer_process_send_negotiate_buffers(
    VividRendererProcess* process,
    const void* payload,
    size_t payload_length,
    guint64* out_request_id,
    GError** error);
gboolean vivid_renderer_process_send_runtime(VividRendererProcess* process,
                                              guint16 opcode,
                                              const void* payload,
                                              size_t payload_length,
                                              guint64* out_request_id,
                                              GError** error);
gboolean vivid_renderer_process_request_quiesce(VividRendererProcess* process,
                                                 GError** error);
void vivid_renderer_process_mark_unbinding(VividRendererProcess* process);
gboolean vivid_renderer_process_request_shutdown(VividRendererProcess* process,
                                                  GError** error);
void vivid_renderer_process_terminate(VividRendererProcess* process,
                                      const gchar* reason);

/* Internal constructor used only by VividRendererManager after posix_spawn. */
VividRendererProcess* vivid_renderer_process_new_spawned(
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
    gpointer observer_data);
void vivid_renderer_process_free(VividRendererProcess* process);

G_END_DECLS

#endif
