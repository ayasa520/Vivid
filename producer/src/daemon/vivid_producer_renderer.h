/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_PRODUCER_RENDERER_H
#define VIVID_PRODUCER_RENDERER_H

#include "vivid_producer_config.h"
#include "../graphics/vivid_gpu_devices.h"
#include "../renderer_api/vivid_renderer_release_gate.h"

#include <glib.h>

#define VIVID_PRODUCER_RENDERER_MAX_PLANES 4u
#define VIVID_PRODUCER_RENDERER_MAX_BUFFERS 8u
#define VIVID_PRODUCER_RENDERER_DMABUF_MAX_FOURCCS 8u
#define VIVID_PRODUCER_RENDERER_DMABUF_MAX_CAPS 64u

typedef struct _VividProducerRenderer VividProducerRenderer;

typedef enum
{
    VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT,
    VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL,
} VividProducerDmaBufMemoryPreference;

/*
 * Per-bind DMA-BUF allocation contract selected by the core producer after it
 * has seen the display side import capabilities. Scene/video/web renderer modules
 * still keep their stable prepare_buffers ABI; the core renderer adapter uses
 * this request to reject a published buffer set that would violate the current
 * consumer negotiation. This keeps consumer-specific policy out of individual
 * renderers while leaving a single extension point for future same-GPU
 * non-LINEAR modifier requests.
 */
typedef struct
{
    guint32 allowed_fourccs[VIVID_PRODUCER_RENDERER_DMABUF_MAX_FOURCCS];
    guint32 n_allowed_fourccs;
    guint64 required_modifier;
    guint32 required_plane_count;
    gboolean require_modifier;
    VividProducerDmaBufMemoryPreference memory_preference;
    const gchar* debug_label;
} VividProducerRendererDmaBufRequest;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividProducerRendererDmaBufFormatCap;

typedef struct
{
    guint32 n_caps;
    VividProducerRendererDmaBufFormatCap caps[VIVID_PRODUCER_RENDERER_DMABUF_MAX_CAPS];
    VividProducerDmaBufMemoryPreference memory_preference;
} VividProducerRendererDmaBufCaps;

typedef struct
{
    gint    fd;
    guint32 stride;
    guint32 offset;
} VividProducerRendererPlane;

typedef struct
{
    guint32 index;
    guint64 size;
    guint32 n_planes;
    VividProducerRendererPlane planes[VIVID_PRODUCER_RENDERER_MAX_PLANES];
} VividProducerRendererBuffer;

typedef struct
{
    guint32 width;
    guint32 height;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 n_buffers;
    VividProducerRendererBuffer buffers[VIVID_PRODUCER_RENDERER_MAX_BUFFERS];
} VividProducerRendererBufferSet;

typedef struct
{
    guint32 buffer_index;
    gint32  source_frame_id;
    guint64 sequence;
    guint64 target_time_usec;
    /*
     * Per-frame acquire fence exported as a dma_fence sync_file. The core
     * takes ownership and closes it after FRAME_READY send. -1 is only valid
     * when the renderer module has already waited for the GPU work to finish
     * before returning the frame.
     */
    gint    acquire_sync_fd;
} VividProducerRendererFrame;

typedef enum
{
    VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK = 0,
    VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY = 1,
    VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED = 2,
} VividProducerRendererDmaBufPrepareStatus;

VividProducerRenderer* vivid_producer_renderer_new(void);
void                    vivid_producer_renderer_free(VividProducerRenderer* renderer);

void vivid_producer_renderer_apply_config(VividProducerRenderer*     renderer,
                                           const VividProducerConfig* config);
void vivid_producer_renderer_set_playback_paused(VividProducerRenderer* renderer,
                                                  gboolean                paused);
void vivid_producer_renderer_set_media_state_json(VividProducerRenderer* renderer,
                                                   const gchar*            media_state_json);
void vivid_producer_renderer_set_audio_samples(VividProducerRenderer* renderer,
                                                GVariant*               audio_samples);
guint64 vivid_producer_renderer_generation(VividProducerRenderer* renderer);

/*
 * GPU device facts for the control plane: the cached enumeration result and
 * the device the configured render-device value actually resolved to ("auto"
 * already landed on a concrete card). The renderer owns the single resolve
 * point so core state snapshots, BIND_BUFFERS metadata, and all renderer
 * modules describe the same physical device.
 */
const VividGpuDeviceList* vivid_producer_renderer_gpu_devices(
    VividProducerRenderer* renderer);
gboolean vivid_producer_renderer_resolved_gpu(VividProducerRenderer* renderer,
                                               VividGpuDevice*        out_device);

gboolean vivid_producer_renderer_write_frame(VividProducerRenderer* renderer,
                                              guint8*                 pixels,
                                              guint32                 stride,
                                              guint32                 width,
                                              guint32                 height,
                                              guint64                 sequence);

gboolean vivid_producer_renderer_prefers_dmabuf_buffers(VividProducerRenderer* renderer);
gboolean vivid_producer_renderer_query_dmabuf_caps(
    VividProducerRenderer*             renderer,
    VividProducerRendererDmaBufCaps*   out_caps);
gboolean vivid_producer_renderer_prepare_dmabuf_buffers(
    VividProducerRenderer*          renderer,
    guint32                          width,
    guint32                          height,
    gdouble                          render_scale,
    const VividProducerRendererDmaBufRequest* request,
    VividProducerRendererBufferSet* out_set);
VividProducerRendererDmaBufPrepareStatus
vivid_producer_renderer_prepare_dmabuf_buffers_ex(
    VividProducerRenderer*          renderer,
    guint32                          width,
    guint32                          height,
    gdouble                          render_scale,
    const VividProducerRendererDmaBufRequest* request,
    VividProducerRendererBufferSet* out_set);
gboolean vivid_producer_renderer_next_dmabuf_frame(VividProducerRenderer*      renderer,
                                                    VividProducerRendererFrame* out_frame);
void vivid_producer_renderer_buffer_set_clear(VividProducerRendererBufferSet* set);
void vivid_producer_renderer_set_release_gate(VividProducerRenderer*         renderer,
                                               const VividRendererReleaseGate* gate);

void vivid_producer_renderer_pointer_motion(VividProducerRenderer* renderer,
                                             gdouble                 x,
                                             gdouble                 y);
void vivid_producer_renderer_pointer_button(VividProducerRenderer* renderer,
                                             guint32                 button,
                                             gboolean                pressed);
void vivid_producer_renderer_pointer_axis(VividProducerRenderer* renderer,
                                           gdouble                 delta_x,
                                           gdouble                 delta_y);

#endif
