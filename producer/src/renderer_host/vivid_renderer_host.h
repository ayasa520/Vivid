#ifndef VIVID_RENDERER_HOST_H
#define VIVID_RENDERER_HOST_H

#include <glib.h>

#include "vivid_renderer_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _VividRendererHost VividRendererHost;

typedef struct
{
    const guint8* data;
    guint32 length;
} VividRendererByteView;

typedef struct
{
    VividRendererKind renderer_kind;
    guint32 width;
    guint32 height;
    gfloat scale;
    gfloat fps;
    guint32 playback_flags;
    gfloat volume;
    guint8 expected_device_uuid[VIVID_RENDERER_UUID_BYTES];
    VividRendererByteView renderer_id;
    VividRendererByteView project_path;
    VividRendererByteView render_node;
    VividRendererByteView settings_json;
    VividRendererByteView properties_json;
} VividRendererHostInit;

typedef struct
{
    const gchar* renderer_id;
    VividRendererKind kind;

    /*
     * initialize runs on the host IPC thread. Before it returns success, the
     * backend must enqueue exactly one READY, FORMAT_CAPS, and RELEASE_TIMELINE
     * event with request_id. No backend thread may call sendmsg directly.
     */
    gboolean (*initialize)(VividRendererHost* host,
                           const VividRendererHostInit* init,
                           guint64 request_id,
                           gpointer backend_data,
                           GError** error);
    gboolean (*negotiate_buffers)(VividRendererHost* host,
                                  const VividRendererPacket* packet,
                                  gpointer backend_data,
                                  GError** error);
    gboolean (*runtime_command)(VividRendererHost* host,
                                const VividRendererPacket* packet,
                                gpointer backend_data,
                                GError** error);
    gboolean (*quiesce)(VividRendererHost* host,
                        gpointer backend_data,
                        GError** error);
    void (*shutdown)(VividRendererHost* host, gpointer backend_data);
} VividRendererHostBackend;

gboolean vivid_renderer_host_parse_init(const VividRendererPacket* packet,
                                         VividRendererHostInit* init);

/*
 * Queue a worker event while retaining every caller FD. The queue duplicates
 * descriptors with CLOEXEC and becomes the sole owner of those duplicates.
 */
gboolean vivid_renderer_host_publish_borrowed(VividRendererHost* host,
                                               guint16 opcode,
                                               guint64 request_id,
                                               const void* payload,
                                               size_t payload_length,
                                               const gint* fds,
                                               size_t n_fds,
                                               GError** error);

/*
 * Queue a worker event and consume every caller FD. Entries are closed and set
 * to -1 on all paths, including validation and a quiesced host.
 */
gboolean vivid_renderer_host_publish_moved(VividRendererHost* host,
                                            guint16 opcode,
                                            guint64 request_id,
                                            const void* payload,
                                            size_t payload_length,
                                            gint* fds,
                                            size_t n_fds,
                                            GError** error);

const gchar* vivid_renderer_host_renderer_id(const VividRendererHost* host);
guint64 vivid_renderer_host_instance_id(const VividRendererHost* host);

/* Parse host-only argv, supervise the IPC loop, and return the process exit code. */
gint vivid_renderer_host_run(gint argc,
                             gchar** argv,
                             const VividRendererHostBackend* backend,
                             gpointer backend_data);

#ifdef __cplusplus
}
#endif

#endif
