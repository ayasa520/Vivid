/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Public C ABI of the CEF-based "web" wallpaper backend. The shape mirrors
 * vivid_scene_producer.h so the core renderer can treat scene and web as
 * peer dlopen'ed producers; web additionally consumes pointer axis events
 * (scroll) and needs an explicit process-wide CEF shutdown hook because CEF
 * can only be initialized once per process and never re-initialized.
 */
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

#define VIVID_WEB_PRODUCER_MAX_PLANES 4u
#define VIVID_WEB_PRODUCER_MAX_BUFFERS 3u
#define VIVID_WEB_PRODUCER_DMABUF_MAX_CAPS 64u

typedef struct _VividWebProducer VividWebProducer;

typedef struct
{
    gint    fd;
    guint32 stride;
    guint32 offset;
} VividWebProducerPlane;

typedef struct
{
    guint32 index;
    guint64 size;
    guint32 n_planes;
    VividWebProducerPlane planes[VIVID_WEB_PRODUCER_MAX_PLANES];
} VividWebProducerBuffer;

typedef struct
{
    guint32 width;
    guint32 height;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 n_buffers;
    VividWebProducerBuffer buffers[VIVID_WEB_PRODUCER_MAX_BUFFERS];
} VividWebProducerBufferSet;

typedef enum
{
    VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEFAULT,
    VIVID_WEB_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL,
} VividWebProducerDmaBufMemoryPreference;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
    gboolean require_modifier;
    VividWebProducerDmaBufMemoryPreference memory_preference;
} VividWebProducerDmaBufRequest;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividWebProducerDmaBufFormatCap;

typedef struct
{
    guint32 n_caps;
    VividWebProducerDmaBufFormatCap caps[VIVID_WEB_PRODUCER_DMABUF_MAX_CAPS];
    VividWebProducerDmaBufMemoryPreference memory_preference;
} VividWebProducerDmaBufCaps;

typedef struct
{
    guint32 buffer_index;
    gint32  source_frame_id;
    guint64 sequence;
    guint64 target_time_usec;
    gint    acquire_sync_fd;
} VividWebProducerFrame;

VividWebProducer* vivid_web_producer_new(void);
void               vivid_web_producer_free(VividWebProducer* self);

/*
 * project_dir may be a Wallpaper Engine web project directory (project.json
 * with type "web" / an .html entry) or a direct path to an .html file.
 */
gboolean vivid_web_producer_configure(VividWebProducer* self,
                                       const gchar*       project_dir,
                                       const gchar*       user_properties_json,
                                       gboolean           muted,
                                       gdouble            volume,
                                       gint               fill_mode,
                                       gint               fps,
                                       const gchar*       render_device);

void vivid_web_producer_set_playing(VividWebProducer* self, gboolean playing);
void vivid_web_producer_set_pointer_motion(VividWebProducer* self,
                                            gdouble            x,
                                            gdouble            y);
void vivid_web_producer_set_pointer_button(VividWebProducer* self,
                                            guint32            button,
                                            gboolean           pressed);
void vivid_web_producer_set_pointer_axis(VividWebProducer* self,
                                          gdouble            delta_x,
                                          gdouble            delta_y);
void vivid_web_producer_set_media_state_json(VividWebProducer* self,
                                              const gchar*       media_state_json);
void vivid_web_producer_set_audio_samples(VividWebProducer* self,
                                           GVariant*          audio_samples);
void vivid_web_producer_set_release_gate(VividWebProducer*          self,
                                          const VividRendererReleaseGate* gate);

gboolean vivid_web_producer_prepare_buffers(VividWebProducer*          self,
                                             guint32                     width,
                                             guint32                     height,
                                             gdouble                     render_scale,
                                             VividWebProducerBufferSet* out_set);
gboolean vivid_web_producer_query_dmabuf_caps(
    VividWebProducer*             self,
    VividWebProducerDmaBufCaps*   out_caps);
gboolean vivid_web_producer_prepare_buffers_with_request(
    VividWebProducer*                    self,
    guint32                              width,
    guint32                              height,
    gdouble                              render_scale,
    const VividWebProducerDmaBufRequest* request,
    VividWebProducerBufferSet*           out_set);
gboolean vivid_web_producer_next_frame(VividWebProducer*      self,
                                        VividWebProducerFrame* out_frame);
void vivid_web_producer_buffer_set_clear(VividWebProducerBufferSet* set);

/*
 * CefShutdown for the whole process. Must only be called once, after every
 * VividWebProducer instance has been freed, on the same thread that first
 * triggered CEF initialization (the GLib main thread in the producer).
 */
void vivid_web_producer_global_shutdown(void);

G_END_DECLS
