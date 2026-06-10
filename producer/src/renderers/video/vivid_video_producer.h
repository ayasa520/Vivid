/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

#include "../../renderer_api/vivid_renderer_release_gate.h"

#include <glib.h>

G_BEGIN_DECLS

#define VIVID_VIDEO_PRODUCER_MAX_PLANES 4u
#define VIVID_VIDEO_PRODUCER_MAX_BUFFERS 3u
#define VIVID_VIDEO_PRODUCER_DMABUF_MAX_CAPS 64u

typedef struct _VividVideoProducer VividVideoProducer;

typedef struct
{
    gint    fd;
    guint32 stride;
    guint32 offset;
} VividVideoProducerPlane;

typedef struct
{
    guint32 index;
    guint64 size;
    guint32 n_planes;
    VividVideoProducerPlane planes[VIVID_VIDEO_PRODUCER_MAX_PLANES];
} VividVideoProducerBuffer;

typedef struct
{
    guint32 width;
    guint32 height;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 n_buffers;
    VividVideoProducerBuffer buffers[VIVID_VIDEO_PRODUCER_MAX_BUFFERS];
} VividVideoProducerBufferSet;

typedef enum
{
    VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEFAULT,
    VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL,
} VividVideoProducerDmaBufMemoryPreference;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
    gboolean require_modifier;
    VividVideoProducerDmaBufMemoryPreference memory_preference;
} VividVideoProducerDmaBufRequest;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividVideoProducerDmaBufFormatCap;

typedef struct
{
    guint32 n_caps;
    VividVideoProducerDmaBufFormatCap caps[VIVID_VIDEO_PRODUCER_DMABUF_MAX_CAPS];
    VividVideoProducerDmaBufMemoryPreference memory_preference;
} VividVideoProducerDmaBufCaps;

typedef struct
{
    guint32 buffer_index;
    gint32  source_frame_id;
    guint64 sequence;
    guint64 target_time_usec;
    gint    acquire_sync_fd;
} VividVideoProducerFrame;

VividVideoProducer* vivid_video_producer_new(void);
void                 vivid_video_producer_free(VividVideoProducer* self);

gboolean vivid_video_producer_configure(VividVideoProducer* self,
                                         const gchar*         video_path,
                                         gboolean             muted,
                                         gdouble              volume,
                                         gint                 fill_mode,
                                         gint                 fps,
                                         const gchar*         render_device);

void vivid_video_producer_set_audio_state(VividVideoProducer* self,
                                           gboolean             muted,
                                           gdouble              volume);
void vivid_video_producer_set_playing(VividVideoProducer* self, gboolean playing);
void vivid_video_producer_set_release_gate(VividVideoProducer*          self,
                                            const VividRendererReleaseGate* gate);

gboolean vivid_video_producer_prepare_buffers(VividVideoProducer*          self,
                                               guint32                       width,
                                               guint32                       height,
                                               gdouble                       render_scale,
                                               VividVideoProducerBufferSet* out_set);
gboolean vivid_video_producer_query_dmabuf_caps(
    VividVideoProducer*             self,
    VividVideoProducerDmaBufCaps*   out_caps);
gboolean vivid_video_producer_prepare_buffers_with_request(
    VividVideoProducer*                    self,
    guint32                                width,
    guint32                                height,
    gdouble                                render_scale,
    const VividVideoProducerDmaBufRequest* request,
    VividVideoProducerBufferSet*           out_set);
gboolean vivid_video_producer_next_frame(VividVideoProducer*      self,
                                          VividVideoProducerFrame* out_frame);
void vivid_video_producer_buffer_set_clear(VividVideoProducerBufferSet* set);

G_END_DECLS
