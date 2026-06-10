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

#define VIVID_SCENE_PRODUCER_MAX_PLANES 4u
#define VIVID_SCENE_PRODUCER_MAX_BUFFERS 3u
#define VIVID_SCENE_PRODUCER_DMABUF_MAX_CAPS 64u

typedef struct _VividSceneProducer VividSceneProducer;

typedef struct
{
    gint    fd;
    guint32 stride;
    guint32 offset;
} VividSceneProducerPlane;

typedef struct
{
    guint32 index;
    guint64 size;
    guint32 n_planes;
    VividSceneProducerPlane planes[VIVID_SCENE_PRODUCER_MAX_PLANES];
} VividSceneProducerBuffer;

typedef struct
{
    guint32 width;
    guint32 height;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 n_buffers;
    VividSceneProducerBuffer buffers[VIVID_SCENE_PRODUCER_MAX_BUFFERS];
} VividSceneProducerBufferSet;

typedef enum
{
    VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEFAULT,
    VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL,
} VividSceneProducerDmaBufMemoryPreference;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
    gboolean require_modifier;
    VividSceneProducerDmaBufMemoryPreference memory_preference;
} VividSceneProducerDmaBufRequest;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividSceneProducerDmaBufFormatCap;

typedef struct
{
    guint32 n_caps;
    VividSceneProducerDmaBufFormatCap caps[VIVID_SCENE_PRODUCER_DMABUF_MAX_CAPS];
    VividSceneProducerDmaBufMemoryPreference memory_preference;
} VividSceneProducerDmaBufCaps;

typedef enum
{
    VIVID_SCENE_PRODUCER_DMABUF_PREPARE_OK = 0,
    VIVID_SCENE_PRODUCER_DMABUF_PREPARE_NOT_READY = 1,
    VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED = 2,
} VividSceneProducerDmaBufPrepareStatus;

typedef struct
{
    guint32 buffer_index;
    gint32  source_frame_id;
    guint64 sequence;
    guint64 target_time_usec;
    gint    acquire_sync_fd;
} VividSceneProducerFrame;

VividSceneProducer* vivid_scene_producer_new(void);
void                 vivid_scene_producer_free(VividSceneProducer* self);

gboolean vivid_scene_producer_configure(VividSceneProducer* self,
                                         const gchar*         project_dir,
                                         const gchar*         user_properties_json,
                                         gboolean             muted,
                                         gdouble              volume,
                                         gint                 fill_mode,
                                         gint                 fps,
                                         const gchar*         render_device);

void vivid_scene_producer_set_playing(VividSceneProducer* self, gboolean playing);
void vivid_scene_producer_set_pointer_motion(VividSceneProducer* self,
                                              gdouble              x,
                                              gdouble              y);
void vivid_scene_producer_set_pointer_button(VividSceneProducer* self,
                                              guint32              button,
                                              gboolean             pressed);
void vivid_scene_producer_set_media_state_json(VividSceneProducer* self,
                                                const gchar*         media_state_json);
void vivid_scene_producer_set_audio_samples(VividSceneProducer* self,
                                             GVariant*            audio_samples);
void vivid_scene_producer_set_release_gate(VividSceneProducer*          self,
                                            const VividRendererReleaseGate* gate);

gboolean vivid_scene_producer_query_dmabuf_caps(
    VividSceneProducer*             self,
    VividSceneProducerDmaBufCaps*   out_caps);

gboolean vivid_scene_producer_prepare_buffers(VividSceneProducer*          self,
                                               guint32                       width,
                                               guint32                       height,
                                               gdouble                       render_scale,
                                               VividSceneProducerBufferSet* out_set);
gboolean vivid_scene_producer_prepare_buffers_with_request(
    VividSceneProducer*                    self,
    guint32                                width,
    guint32                                height,
    gdouble                                render_scale,
    const VividSceneProducerDmaBufRequest* request,
    VividSceneProducerBufferSet*           out_set);
VividSceneProducerDmaBufPrepareStatus
vivid_scene_producer_get_last_dmabuf_prepare_status(VividSceneProducer* self);
gboolean vivid_scene_producer_next_frame(VividSceneProducer*      self,
                                          VividSceneProducerFrame* out_frame);
void vivid_scene_producer_buffer_set_clear(VividSceneProducerBufferSet* set);

G_END_DECLS
