/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_producer_renderer.h"

#include "../renderers/scene/vivid_scene_producer.h"
#include "../renderers/video/vivid_video_producer.h"
#include "../renderers/web/vivid_web_producer.h"

#include <dlfcn.h>
#include <gio/gio.h>
#include <gst/gst.h>

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

typedef enum
{
    VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC,
    VIVID_PRODUCER_RENDERER_MODE_VIDEO,
    VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING,
    VIVID_PRODUCER_RENDERER_MODE_WEB,
} VividProducerRendererMode;

typedef struct
{
    gint    fd;
    guint32 stride;
    guint32 offset;
} VividProducerRendererRoutePlane;

typedef struct
{
    guint32 index;
    guint64 size;
    guint32 n_planes;
    VividProducerRendererRoutePlane planes[VIVID_PRODUCER_RENDERER_MAX_PLANES];
} VividProducerRendererRouteBuffer;

typedef struct
{
    guint32 width;
    guint32 height;
    guint32 fourcc;
    guint64 modifier;
    gboolean premultiplied;
    guint32 n_buffers;
    VividProducerRendererRouteBuffer buffers[VIVID_PRODUCER_RENDERER_MAX_BUFFERS];
} VividProducerRendererRouteBufferSet;

typedef struct
{
    guint32 buffer_index;
    gint32  source_frame_id;
    guint64 sequence;
    guint64 target_time_usec;
    gint    acquire_sync_fd;
} VividProducerRendererRouteFrame;

typedef struct
{
    void* module;
    VividVideoProducer* instance;

    VividVideoProducer* (*new_func)(void);
    void (*free_func)(VividVideoProducer* self);
    gboolean (*configure_func)(VividVideoProducer* self,
                               const gchar*         video_path,
                               gboolean             muted,
                               gdouble              volume,
                               gint                 fill_mode,
                               gint                 fps,
                               const gchar*         render_device);
    void (*set_audio_state_func)(VividVideoProducer* self,
                                 gboolean             muted,
                                 gdouble              volume);
    void (*set_playing_func)(VividVideoProducer* self, gboolean playing);
    gboolean (*prepare_buffers_func)(VividVideoProducer*          self,
                                     guint32                       width,
                                     guint32                       height,
                                     gdouble                       render_scale,
                                     VividVideoProducerBufferSet* out_set);
    gboolean (*query_dmabuf_caps_func)(VividVideoProducer*           self,
                                       VividVideoProducerDmaBufCaps* out_caps);
    gboolean (*prepare_buffers_with_request_func)(
        VividVideoProducer*                    self,
        guint32                                width,
        guint32                                height,
        gdouble                                render_scale,
        const VividVideoProducerDmaBufRequest* request,
        VividVideoProducerBufferSet*           out_set);
    void (*set_release_gate_func)(VividVideoProducer*           self,
                                  const VividRendererReleaseGate* gate);
    void (*request_frame_func)(VividVideoProducer* self,
                               const gchar*         reason);
    gboolean (*next_frame_func)(VividVideoProducer*      self,
                                VividVideoProducerFrame* out_frame);
    void (*buffer_set_clear_func)(VividVideoProducerBufferSet* set);
} VividProducerVideoRenderer;

typedef struct
{
    void* module;
    VividSceneProducer* instance;

    VividSceneProducer* (*new_func)(void);
    void (*free_func)(VividSceneProducer* self);
    gboolean (*configure_func)(VividSceneProducer* self,
                               const gchar*         project_dir,
                               const gchar*         user_properties_json,
                               gboolean             muted,
                               gdouble              volume,
                               gint                 fill_mode,
                               gint                 fps,
                               const gchar*         render_device);
    void (*set_playing_func)(VividSceneProducer* self, gboolean playing);
    void (*set_pointer_motion_func)(VividSceneProducer* self, gdouble x, gdouble y);
    void (*set_pointer_button_func)(VividSceneProducer* self,
                                    guint32              button,
                                    gboolean             pressed);
    void (*set_media_state_json_func)(VividSceneProducer* self,
                                      const gchar*         media_state_json);
    void (*set_audio_samples_func)(VividSceneProducer* self,
                                   GVariant*            audio_samples);
    gboolean (*prepare_buffers_func)(VividSceneProducer*          self,
                                     guint32                       width,
                                     guint32                       height,
                                     gdouble                       render_scale,
                                     VividSceneProducerBufferSet* out_set);
    gboolean (*query_dmabuf_caps_func)(VividSceneProducer*           self,
                                       VividSceneProducerDmaBufCaps* out_caps);
    gboolean (*prepare_buffers_with_request_func)(
        VividSceneProducer*                    self,
        guint32                                width,
        guint32                                height,
        gdouble                                render_scale,
        const VividSceneProducerDmaBufRequest* request,
        VividSceneProducerBufferSet*           out_set);
    VividSceneProducerDmaBufPrepareStatus (*get_last_dmabuf_prepare_status_func)(
        VividSceneProducer* self);
    void (*set_release_gate_func)(VividSceneProducer*           self,
                                  const VividRendererReleaseGate* gate);
    void (*request_frame_func)(VividSceneProducer* self,
                               const gchar*         reason);
    gboolean (*next_frame_func)(VividSceneProducer*      self,
                                VividSceneProducerFrame* out_frame);
    void (*buffer_set_clear_func)(VividSceneProducerBufferSet* set);
} VividProducerSceneRenderer;

typedef struct
{
    void* module;
    VividWebProducer* instance;

    VividWebProducer* (*new_func)(void);
    void (*free_func)(VividWebProducer* self);
    gboolean (*configure_func)(VividWebProducer* self,
                               const gchar*       project_dir,
                               const gchar*       user_properties_json,
                               gboolean           muted,
                               gdouble            volume,
                               gint               fill_mode,
                               gint               fps,
                               const gchar*       render_device);
    void (*set_playing_func)(VividWebProducer* self, gboolean playing);
    void (*set_pointer_motion_func)(VividWebProducer* self, gdouble x, gdouble y);
    void (*set_pointer_button_func)(VividWebProducer* self,
                                    guint32            button,
                                    gboolean           pressed);
    void (*set_pointer_axis_func)(VividWebProducer* self,
                                  gdouble            delta_x,
                                  gdouble            delta_y);
    void (*set_media_state_json_func)(VividWebProducer* self,
                                      const gchar*       media_state_json);
    void (*set_audio_samples_func)(VividWebProducer* self,
                                   GVariant*          audio_samples);
    gboolean (*prepare_buffers_func)(VividWebProducer*          self,
                                     guint32                     width,
                                     guint32                     height,
                                     gdouble                     render_scale,
                                     VividWebProducerBufferSet* out_set);
    gboolean (*query_dmabuf_caps_func)(VividWebProducer*           self,
                                       VividWebProducerDmaBufCaps* out_caps);
    gboolean (*prepare_buffers_with_request_func)(
        VividWebProducer*                    self,
        guint32                              width,
        guint32                              height,
        gdouble                              render_scale,
        const VividWebProducerDmaBufRequest* request,
        VividWebProducerBufferSet*           out_set);
    void (*set_release_gate_func)(VividWebProducer*             self,
                                  const VividRendererReleaseGate* gate);
    void (*request_frame_func)(VividWebProducer* self,
                               const gchar*       reason);
    gboolean (*next_frame_func)(VividWebProducer*      self,
                                VividWebProducerFrame* out_frame);
    void (*buffer_set_clear_func)(VividWebProducerBufferSet* set);
    void (*global_shutdown_func)(void);
} VividProducerWebRenderer;

struct _VividProducerRenderer
{
    VividProducerRendererMode mode;
    VividProducerVideoRenderer video;
    VividProducerSceneRenderer scene;
    VividProducerWebRenderer web;
    GMutex latest_lock;

    gchar*   project_path;
    gchar*   user_properties_json;
    gchar*   media_state_json;
    GVariant* audio_samples;
    gchar*   render_device;
    gboolean muted;
    gint     volume;
    gint     content_fit;
    gint     scene_fps;
    gboolean playback_paused;
    guint64  generation;
    guint64  last_request_frame_log_time_usec;
    guint32  request_frame_log_suppressed;

    /*
     * Device facts are enumerated once and re-resolved whenever the configured
     * render-device value changes. Backends receive the resolved render node
     * (never the raw "auto"), so scene and video can only land on this card.
     */
    VividGpuDeviceList gpu_devices;
    VividGpuDevice     resolved_gpu;
    gboolean            resolved_gpu_valid;

    VividRendererReleaseGate release_gate;
    gboolean               release_gate_valid;
};

static gboolean read_project_json(const gchar* project_dir,
                                  JsonParser** out_parser,
                                  JsonObject** out_object);

static const gchar*
default_scene_media_state_json(void)
{
    return "{\"title\":\"\",\"artist\":\"\",\"albumTitle\":\"\",\"albumArtist\":\"\","
           "\"subTitle\":\"\",\"genres\":\"\",\"contentType\":\"\","
           "\"hasThumbnail\":false,\"playbackState\":0,"
           "\"primaryColor\":[0,0,0],\"secondaryColor\":[1,1,1],"
           "\"tertiaryColor\":[1,1,1],\"textColor\":[1,1,1],"
           "\"highContrastColor\":[1,1,1],\"thumbnailPath\":\"\"}";
}

static GVariant*
new_empty_audio_samples_variant(void)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ad"));
    return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void
log_gstreamer_environment_once(void)
{
    static gsize logged = 0;
    if (!g_once_init_enter(&logged))
        return;

    /*
     * Keep this diagnostic process-local. External gst-inspect runs can see a
     * different registry, plugin path, or feature-rank override than the
     * producer process. Direct video and scene video textures explicitly select
     * GPU decoder factories.
     */
    guint major = 0;
    guint minor = 0;
    guint micro = 0;
    guint nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    g_message("VividProducer: GStreamer runtime version=%u.%u.%u nano=%u",
              major,
              minor,
              micro,
              nano);

    static const gchar* const env_names[] = {
        "GST_PLUGIN_PATH",
        "GST_PLUGIN_SYSTEM_PATH",
        "GST_PLUGIN_SYSTEM_PATH_1_0",
        "GST_REGISTRY",
        "GST_REGISTRY_1_0",
        "GST_PLUGIN_FEATURE_RANK",
        "LD_LIBRARY_PATH",
    };
    for (guint i = 0; i < G_N_ELEMENTS(env_names); i++) {
        const gchar* value = g_getenv(env_names[i]);
        g_message("VividProducer: GStreamer env %s=%s",
                  env_names[i],
                  value && *value ? value : "(unset)");
    }

    static const gchar* const factory_names[] = {
        "playbin",
        "uridecodebin",
        "qtdemux",
        "h264parse",
        "mpeg4videoparse",
        "avdec_h264",
        "avdec_mpeg4",
        "openh264dec",
        "nvh264dec",
        "nvh264sldec",
        "nvmpeg4videodec",
        "vah264dec",
        "vapostproc",
        "vulkanh264dec",
        "avdec_aac",
        "glupload",
        "glcolorconvert",
        "gldownload",
        "appsink",
    };
    for (guint i = 0; i < G_N_ELEMENTS(factory_names); i++) {
        GstElementFactory* factory = gst_element_factory_find(factory_names[i]);
        if (!factory) {
            g_message("VividProducer: GStreamer factory %s=missing",
                      factory_names[i]);
            continue;
        }

        const guint rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
        const gchar* plugin_name =
            gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory));
        GstPlugin* plugin =
            gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
        g_message("VividProducer: GStreamer factory %s=available rank=%u "
                  "plugin=%s version=%s file=%s",
                  factory_names[i],
                  rank,
                  plugin_name ? plugin_name : "(unknown)",
                  plugin ? gst_plugin_get_version(plugin) : "(unknown)",
                  plugin ? gst_plugin_get_filename(plugin) : "(unknown)");
        g_clear_object(&plugin);
        gst_object_unref(factory);
    }

    g_once_init_leave(&logged, 1);
}

static void
fill_diagnostic_pattern(guint8* pixels,
                        guint32 stride,
                        guint32 width,
                        guint32 height,
                        guint64 sequence)
{
    for (guint32 y = 0; y < height; y++) {
        guint8* row = pixels + (gsize)y * stride;
        for (guint32 x = 0; x < width; x++) {
            guint8* pixel = row + (gsize)x * 4u;
            const guint8 r = (guint8)((x + sequence * 3u) & 0xffu);
            const guint8 g = (guint8)((y + sequence * 5u) & 0xffu);
            const guint8 b = (guint8)(((x ^ y) + sequence * 7u) & 0xffu);

            /*
             * The producer exports DRM_FORMAT_XRGB8888 DMA-BUFs. On
             * little-endian machines this is stored as B, G, R, X in memory,
             * and only the buffer fd/metadata crosses the socket boundary.
             */
            pixel[0] = b;
            pixel[1] = g;
            pixel[2] = r;
            pixel[3] = 0xffu;
        }
    }
}

static const gchar*
renderer_mode_label(VividProducerRendererMode mode)
{
    switch (mode) {
    case VIVID_PRODUCER_RENDERER_MODE_VIDEO:
        return "video";
    case VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING:
        return "scene";
    case VIVID_PRODUCER_RENDERER_MODE_WEB:
        return "web";
    case VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC:
    default:
        return "diagnostic";
    }
}

static void
renderer_resolve_gpu_device(VividProducerRenderer* renderer)
{
    renderer->resolved_gpu_valid =
        vivid_gpu_devices_resolve(&renderer->gpu_devices,
                                   renderer->render_device,
                                   &renderer->resolved_gpu);
    if (renderer->resolved_gpu_valid) {
        g_message("VividProducer: render-device '%s' resolved to node=%s name=%s "
                  "vendor=%s decoder-route=%s",
                  renderer->render_device ? renderer->render_device : "auto",
                  renderer->resolved_gpu.render_node[0]
                      ? renderer->resolved_gpu.render_node
                      : "(unknown)",
                  renderer->resolved_gpu.name,
                  vivid_gpu_vendor_name(renderer->resolved_gpu.vendor_id),
                  vivid_gpu_decoder_route_name(
                      vivid_gpu_decoder_route_for_vendor(renderer->resolved_gpu.vendor_id)));
    } else {
        g_warning("VividProducer: render-device '%s' resolved to no usable Vulkan GPU",
                  renderer->render_device ? renderer->render_device : "auto");
    }
}

/*
 * Backends never see "auto": they get the resolved render node so the scene
 * and video producers cannot re-run device preference logic independently.
 */
static const gchar*
renderer_backend_render_device(VividProducerRenderer* renderer)
{
    if (renderer->resolved_gpu_valid && renderer->resolved_gpu.render_node[0])
        return renderer->resolved_gpu.render_node;
    return "(unresolved)";
}

static gboolean
renderer_has_resolved_gpu(VividProducerRenderer* renderer,
                          const gchar*            backend_name)
{
    /*
     * The core renderer owns the single GPU decision. Scene/video adapters are
     * deliberately passed the resolved render node, not the user's raw value, so
     * they cannot independently retry "auto" and accidentally produce DMA-BUFs
     * without matching protocol metadata. If the shared resolver has no concrete
     * DRM render node, the backend must stay stopped until a later configuration
     * change resolves one.
     */
    if (renderer->resolved_gpu_valid && renderer->resolved_gpu.render_node[0])
        return TRUE;

    g_warning("VividProducer: refusing to start %s renderer because render-device "
              "'%s' has no resolved GPU render-node=(unresolved) vendor=unknown "
              "device=(unknown)",
              backend_name ? backend_name : "renderer",
              renderer->render_device ? renderer->render_device : "auto");
    return FALSE;
}

static const VividRendererReleaseGate*
renderer_active_release_gate(VividProducerRenderer* renderer)
{
    return renderer && renderer->release_gate_valid ? &renderer->release_gate : NULL;
}

static void
vivid_producer_video_apply_release_gate(VividProducerRenderer* renderer)
{
    if (renderer->video.instance && renderer->video.set_release_gate_func) {
        renderer->video.set_release_gate_func(renderer->video.instance,
                                              renderer_active_release_gate(renderer));
    }
}

static void
vivid_producer_scene_apply_release_gate(VividProducerRenderer* renderer)
{
    if (renderer->scene.instance && renderer->scene.set_release_gate_func) {
        renderer->scene.set_release_gate_func(renderer->scene.instance,
                                              renderer_active_release_gate(renderer));
    }
}

static void
vivid_producer_web_apply_release_gate(VividProducerRenderer* renderer)
{
    if (renderer->web.instance && renderer->web.set_release_gate_func) {
        renderer->web.set_release_gate_func(renderer->web.instance,
                                            renderer_active_release_gate(renderer));
    }
}

static void
vivid_producer_video_stop(VividProducerRenderer* renderer)
{
    VividProducerVideoRenderer* video = &renderer->video;

    if (video->instance && video->free_func) {
        if (video->set_release_gate_func)
            video->set_release_gate_func(video->instance, NULL);
        video->free_func(video->instance);
        video->instance = NULL;
    }

    renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
}

static gpointer
load_video_symbol(void* module, const gchar* name)
{
    dlerror();
    gpointer symbol = dlsym(module, name);
    const gchar* error = dlerror();
    if (error) {
        g_warning("VividProducer: failed to resolve %s from VividVideo: %s",
                  name,
                  error);
        return NULL;
    }
    return symbol;
}

static gpointer
load_optional_video_symbol(void* module, const gchar* name)
{
    dlerror();
    gpointer symbol = dlsym(module, name);
    const gchar* error = dlerror();
    if (error) {
        g_debug("VividProducer: optional video symbol %s is unavailable: %s",
                name,
                error);
        return NULL;
    }
    return symbol;
}

static gboolean
vivid_producer_video_ensure_instance(VividProducerRenderer* renderer)
{
    VividProducerVideoRenderer* video = &renderer->video;

    if (video->instance)
        return TRUE;

    if (!video->new_func) {
        g_warning("VividProducer: video producer adapter is loaded without a constructor");
        return FALSE;
    }

    video->instance = video->new_func();
    if (!video->instance) {
        g_warning("VividProducer: failed to create video producer adapter");
        return FALSE;
    }

    g_message("VividProducer: created video producer adapter instance");
    vivid_producer_video_apply_release_gate(renderer);
    return TRUE;
}

static gboolean
vivid_producer_video_load(VividProducerRenderer* renderer)
{
    VividProducerVideoRenderer* video = &renderer->video;
    if (video->module)
        return vivid_producer_video_ensure_instance(renderer);

    const gchar* env_path = g_getenv("VIVID_VIDEO_LIBRARY");
    if (env_path && *env_path) {
        video->module = dlopen(env_path, RTLD_NOW | RTLD_LOCAL);
        if (video->module)
            g_message("VividProducer: loaded video producer adapter from %s", env_path);
        else
            g_debug("VividProducer: failed to load %s: %s", env_path, dlerror());
    }

    const gchar* candidates[] = {
        "libVividVideo.so",
        "/app/lib/libVividVideo.so",
        NULL,
    };

    for (guint i = 0; !video->module && candidates[i]; i++) {
        video->module = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (video->module) {
            g_message("VividProducer: loaded video producer adapter from %s",
                      candidates[i]);
            break;
        }
        g_debug("VividProducer: failed to load %s: %s", candidates[i], dlerror());
    }

    if (!video->module) {
        g_warning("VividProducer: video project selected but libVividVideo.so could not be loaded");
        return FALSE;
    }

#define LOAD_VIDEO_SYMBOL(field, symbol_name) \
    G_STMT_START { \
        video->field = load_video_symbol(video->module, symbol_name); \
        if (!video->field) \
            return FALSE; \
    } G_STMT_END

    LOAD_VIDEO_SYMBOL(new_func, "vivid_video_producer_new");
    LOAD_VIDEO_SYMBOL(free_func, "vivid_video_producer_free");
    LOAD_VIDEO_SYMBOL(configure_func, "vivid_video_producer_configure");
    LOAD_VIDEO_SYMBOL(set_audio_state_func, "vivid_video_producer_set_audio_state");
    LOAD_VIDEO_SYMBOL(set_playing_func, "vivid_video_producer_set_playing");
    LOAD_VIDEO_SYMBOL(prepare_buffers_func, "vivid_video_producer_prepare_buffers");
    video->query_dmabuf_caps_func = (gboolean (*)(
        VividVideoProducer*,
        VividVideoProducerDmaBufCaps*))load_optional_video_symbol(
            video->module,
            "vivid_video_producer_query_dmabuf_caps");
    video->prepare_buffers_with_request_func = (gboolean (*)(
        VividVideoProducer*,
        guint32,
        guint32,
        gdouble,
        const VividVideoProducerDmaBufRequest*,
        VividVideoProducerBufferSet*))load_optional_video_symbol(
            video->module,
            "vivid_video_producer_prepare_buffers_with_request");
    video->set_release_gate_func = (void (*)(
        VividVideoProducer*,
        const VividRendererReleaseGate*))load_optional_video_symbol(
            video->module,
            "vivid_video_producer_set_release_gate");
    LOAD_VIDEO_SYMBOL(request_frame_func, "vivid_video_producer_request_frame");
    LOAD_VIDEO_SYMBOL(next_frame_func, "vivid_video_producer_next_frame");
    LOAD_VIDEO_SYMBOL(buffer_set_clear_func, "vivid_video_producer_buffer_set_clear");

#undef LOAD_VIDEO_SYMBOL

    return vivid_producer_video_ensure_instance(renderer);
}

static void
vivid_producer_video_unload(VividProducerRenderer* renderer)
{
    VividProducerVideoRenderer* video = &renderer->video;
    if (video->instance && video->free_func) {
        video->free_func(video->instance);
        video->instance = NULL;
    }
    if (video->module) {
        dlclose(video->module);
        memset(video, 0, sizeof(*video));
    }
}

static gpointer
load_scene_symbol(void* module, const gchar* name)
{
    dlerror();
    gpointer symbol = dlsym(module, name);
    const gchar* error = dlerror();
    if (error) {
        g_warning("VividProducer: failed to resolve %s from VividScene: %s",
                  name,
                  error);
        return NULL;
    }
    return symbol;
}

static gpointer
load_optional_scene_symbol(void* module, const gchar* name)
{
    dlerror();
    gpointer symbol = dlsym(module, name);
    const gchar* error = dlerror();
    if (error) {
        g_debug("VividProducer: optional scene symbol %s is unavailable: %s",
                name,
                error);
        return NULL;
    }
    return symbol;
}

static gboolean
vivid_producer_scene_ensure_instance(VividProducerRenderer* renderer)
{
    VividProducerSceneRenderer* scene = &renderer->scene;

    if (scene->instance)
        return TRUE;

    if (!scene->new_func) {
        g_warning("VividProducer: scene producer adapter is loaded without a constructor");
        return FALSE;
    }

    /*
     * The scene module is intentionally kept loaded across wallpaper switches, but
     * the producer instance is destroyed whenever the active project stops. A
     * later scene project therefore must recreate the instance even though the
     * library and symbol table are already cached; otherwise configure() receives
     * NULL and GLib reports a critical assertion from inside libVividScene.
     */
    scene->instance = scene->new_func();
    if (!scene->instance) {
        g_warning("VividProducer: failed to create scene producer adapter");
        return FALSE;
    }

    g_message("VividProducer: created scene producer adapter instance");
    vivid_producer_scene_apply_release_gate(renderer);
    return TRUE;
}

static gboolean
vivid_producer_scene_load(VividProducerRenderer* renderer)
{
    VividProducerSceneRenderer* scene = &renderer->scene;
    if (scene->module)
        return vivid_producer_scene_ensure_instance(renderer);

    const gchar* env_path = g_getenv("VIVID_SCENE_LIBRARY");
    if (env_path && *env_path) {
        scene->module = dlopen(env_path, RTLD_NOW | RTLD_LOCAL);
        if (scene->module)
            g_message("VividProducer: loaded scene producer adapter from %s", env_path);
        else
            g_debug("VividProducer: failed to load %s: %s", env_path, dlerror());
    }

    const gchar* candidates[] = {
        "libVividScene.so",
        "/app/lib/libVividScene.so",
        NULL,
    };

    for (guint i = 0; !scene->module && candidates[i]; i++) {
        scene->module = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (scene->module) {
            g_message("VividProducer: loaded scene producer adapter from %s",
                      candidates[i]);
            break;
        }
        g_debug("VividProducer: failed to load %s: %s", candidates[i], dlerror());
    }

    if (!scene->module) {
        g_warning("VividProducer: scene project selected but libVividScene.so could not be loaded");
        return FALSE;
    }

#define LOAD_SCENE_SYMBOL(field, symbol_name) \
    G_STMT_START { \
        scene->field = load_scene_symbol(scene->module, symbol_name); \
        if (!scene->field) \
            return FALSE; \
    } G_STMT_END

    LOAD_SCENE_SYMBOL(new_func, "vivid_scene_producer_new");
    LOAD_SCENE_SYMBOL(free_func, "vivid_scene_producer_free");
    LOAD_SCENE_SYMBOL(configure_func, "vivid_scene_producer_configure");
    LOAD_SCENE_SYMBOL(set_playing_func, "vivid_scene_producer_set_playing");
    LOAD_SCENE_SYMBOL(set_pointer_motion_func, "vivid_scene_producer_set_pointer_motion");
    LOAD_SCENE_SYMBOL(set_pointer_button_func, "vivid_scene_producer_set_pointer_button");
    LOAD_SCENE_SYMBOL(set_media_state_json_func, "vivid_scene_producer_set_media_state_json");
    LOAD_SCENE_SYMBOL(set_audio_samples_func, "vivid_scene_producer_set_audio_samples");
    LOAD_SCENE_SYMBOL(prepare_buffers_func, "vivid_scene_producer_prepare_buffers");
    scene->query_dmabuf_caps_func = (gboolean (*)(
        VividSceneProducer*,
        VividSceneProducerDmaBufCaps*))load_optional_scene_symbol(
            scene->module,
            "vivid_scene_producer_query_dmabuf_caps");
    scene->prepare_buffers_with_request_func = (gboolean (*)(
        VividSceneProducer*,
        guint32,
        guint32,
        gdouble,
        const VividSceneProducerDmaBufRequest*,
        VividSceneProducerBufferSet*))load_optional_scene_symbol(
            scene->module,
            "vivid_scene_producer_prepare_buffers_with_request");
    scene->get_last_dmabuf_prepare_status_func =
        (VividSceneProducerDmaBufPrepareStatus (*)(VividSceneProducer*))
            load_optional_scene_symbol(
                scene->module,
                "vivid_scene_producer_get_last_dmabuf_prepare_status");
    scene->set_release_gate_func = (void (*)(
        VividSceneProducer*,
        const VividRendererReleaseGate*))load_optional_scene_symbol(
            scene->module,
            "vivid_scene_producer_set_release_gate");
    LOAD_SCENE_SYMBOL(request_frame_func, "vivid_scene_producer_request_frame");
    LOAD_SCENE_SYMBOL(next_frame_func, "vivid_scene_producer_next_frame");
    LOAD_SCENE_SYMBOL(buffer_set_clear_func, "vivid_scene_producer_buffer_set_clear");

#undef LOAD_SCENE_SYMBOL

    return vivid_producer_scene_ensure_instance(renderer);
}

static void
vivid_producer_scene_stop(VividProducerRenderer* renderer)
{
    VividProducerSceneRenderer* scene = &renderer->scene;
    if (scene->instance && scene->free_func) {
        if (scene->set_release_gate_func)
            scene->set_release_gate_func(scene->instance, NULL);
        scene->free_func(scene->instance);
        scene->instance = NULL;
    }
}

static void
vivid_producer_scene_unload(VividProducerRenderer* renderer)
{
    VividProducerSceneRenderer* scene = &renderer->scene;
    vivid_producer_scene_stop(renderer);
    if (scene->module) {
        dlclose(scene->module);
        memset(scene, 0, sizeof(*scene));
    }
}

static gpointer
load_web_symbol(void* module, const gchar* name)
{
    dlerror();
    gpointer symbol = dlsym(module, name);
    const gchar* error = dlerror();
    if (error) {
        g_warning("VividProducer: failed to resolve %s from VividWeb: %s",
                  name,
                  error);
        return NULL;
    }
    return symbol;
}

static gpointer
load_optional_web_symbol(void* module, const gchar* name)
{
    dlerror();
    gpointer symbol = dlsym(module, name);
    const gchar* error = dlerror();
    if (error) {
        g_debug("VividProducer: optional web symbol %s is unavailable: %s",
                name,
                error);
        return NULL;
    }
    return symbol;
}

static gboolean
vivid_producer_web_ensure_instance(VividProducerRenderer* renderer)
{
    VividProducerWebRenderer* web = &renderer->web;

    if (web->instance)
        return TRUE;

    if (!web->new_func) {
        g_warning("VividProducer: web producer adapter is loaded without a constructor");
        return FALSE;
    }

    web->instance = web->new_func();
    if (!web->instance) {
        g_warning("VividProducer: failed to create web producer adapter");
        return FALSE;
    }

    g_message("VividProducer: created web producer adapter instance");
    vivid_producer_web_apply_release_gate(renderer);
    return TRUE;
}

static gboolean
vivid_producer_web_load(VividProducerRenderer* renderer)
{
    VividProducerWebRenderer* web = &renderer->web;
    if (web->module)
        return vivid_producer_web_ensure_instance(renderer);

    const gchar* env_path = g_getenv("VIVID_WEB_LIBRARY");
    if (env_path && *env_path) {
        web->module = dlopen(env_path, RTLD_NOW | RTLD_LOCAL);
        if (web->module)
            g_message("VividProducer: loaded web producer adapter from %s", env_path);
        else
            g_debug("VividProducer: failed to load %s: %s", env_path, dlerror());
    }

    const gchar* candidates[] = {
        "libVividWeb.so",
        "/app/lib/libVividWeb.so",
        NULL,
    };

    for (guint i = 0; !web->module && candidates[i]; i++) {
        web->module = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (web->module) {
            g_message("VividProducer: loaded web producer adapter from %s",
                      candidates[i]);
            break;
        }
        g_debug("VividProducer: failed to load %s: %s", candidates[i], dlerror());
    }

    if (!web->module) {
        g_warning("VividProducer: web project selected but libVividWeb.so could not be loaded");
        return FALSE;
    }

#define LOAD_WEB_SYMBOL(field, symbol_name) \
    G_STMT_START { \
        web->field = load_web_symbol(web->module, symbol_name); \
        if (!web->field) \
            return FALSE; \
    } G_STMT_END

    LOAD_WEB_SYMBOL(new_func, "vivid_web_producer_new");
    LOAD_WEB_SYMBOL(free_func, "vivid_web_producer_free");
    LOAD_WEB_SYMBOL(configure_func, "vivid_web_producer_configure");
    LOAD_WEB_SYMBOL(set_playing_func, "vivid_web_producer_set_playing");
    LOAD_WEB_SYMBOL(set_pointer_motion_func, "vivid_web_producer_set_pointer_motion");
    LOAD_WEB_SYMBOL(set_pointer_button_func, "vivid_web_producer_set_pointer_button");
    LOAD_WEB_SYMBOL(set_pointer_axis_func, "vivid_web_producer_set_pointer_axis");
    LOAD_WEB_SYMBOL(set_media_state_json_func, "vivid_web_producer_set_media_state_json");
    LOAD_WEB_SYMBOL(set_audio_samples_func, "vivid_web_producer_set_audio_samples");
    LOAD_WEB_SYMBOL(prepare_buffers_func, "vivid_web_producer_prepare_buffers");
    web->query_dmabuf_caps_func = (gboolean (*)(
        VividWebProducer*,
        VividWebProducerDmaBufCaps*))load_optional_web_symbol(
            web->module,
            "vivid_web_producer_query_dmabuf_caps");
    web->prepare_buffers_with_request_func = (gboolean (*)(
        VividWebProducer*,
        guint32,
        guint32,
        gdouble,
        const VividWebProducerDmaBufRequest*,
        VividWebProducerBufferSet*))load_optional_web_symbol(
            web->module,
            "vivid_web_producer_prepare_buffers_with_request");
    web->set_release_gate_func = (void (*)(
        VividWebProducer*,
        const VividRendererReleaseGate*))load_optional_web_symbol(
            web->module,
            "vivid_web_producer_set_release_gate");
    LOAD_WEB_SYMBOL(request_frame_func, "vivid_web_producer_request_frame");
    LOAD_WEB_SYMBOL(next_frame_func, "vivid_web_producer_next_frame");
    LOAD_WEB_SYMBOL(buffer_set_clear_func, "vivid_web_producer_buffer_set_clear");
    LOAD_WEB_SYMBOL(global_shutdown_func, "vivid_web_producer_global_shutdown");

#undef LOAD_WEB_SYMBOL

    return vivid_producer_web_ensure_instance(renderer);
}

static void
vivid_producer_web_stop(VividProducerRenderer* renderer)
{
    VividProducerWebRenderer* web = &renderer->web;
    if (web->instance && web->free_func) {
        if (web->set_release_gate_func)
            web->set_release_gate_func(web->instance, NULL);
        web->free_func(web->instance);
        web->instance = NULL;
    }
}

static void
vivid_producer_web_unload(VividProducerRenderer* renderer)
{
    VividProducerWebRenderer* web = &renderer->web;
    vivid_producer_web_stop(renderer);
    if (web->module) {
        /*
         * CEF can be initialized exactly once per process and never again
         * after CefShutdown; the web module therefore stays loaded across
         * wallpaper switches and this unload only runs on renderer teardown.
         */
        if (web->global_shutdown_func)
            web->global_shutdown_func();
        dlclose(web->module);
        memset(web, 0, sizeof(*web));
    }
}

static void
vivid_producer_web_apply_media_state(VividProducerRenderer* renderer)
{
    g_return_if_fail(renderer != NULL);

    if (!renderer->web.instance)
        return;

    if (renderer->web.set_media_state_json_func) {
        renderer->web.set_media_state_json_func(
            renderer->web.instance,
            renderer->media_state_json ? renderer->media_state_json : default_scene_media_state_json());
    }
}

static void
vivid_producer_web_apply_audio_samples(VividProducerRenderer* renderer)
{
    g_return_if_fail(renderer != NULL);

    if (!renderer->web.instance)
        return;

    if (renderer->web.set_audio_samples_func) {
        renderer->web.set_audio_samples_func(
            renderer->web.instance,
            renderer->audio_samples ? renderer->audio_samples : NULL);
    }
}

static void
vivid_producer_web_apply_runtime_media(VividProducerRenderer* renderer)
{
    vivid_producer_web_apply_media_state(renderer);
    vivid_producer_web_apply_audio_samples(renderer);
}

static gboolean
vivid_producer_web_start(VividProducerRenderer* renderer,
                          const gchar*            project_path)
{
    if (!renderer_has_resolved_gpu(renderer, "web"))
        return FALSE;

    if (!vivid_producer_web_load(renderer))
        return FALSE;

    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);
    if (!renderer->web.configure_func(renderer->web.instance,
                                      project_path,
                                      renderer->user_properties_json,
                                      renderer->muted,
                                      volume,
                                      CLAMP(renderer->content_fit, 0, 3),
                                      renderer->scene_fps,
                                      renderer_backend_render_device(renderer))) {
        g_warning("VividProducer: web renderer failed to configure project=%s",
                  project_path ? project_path : "(null)");
        vivid_producer_web_stop(renderer);
        return FALSE;
    }

    renderer->mode = VIVID_PRODUCER_RENDERER_MODE_WEB;
    vivid_producer_web_apply_runtime_media(renderer);
    renderer->web.set_playing_func(renderer->web.instance,
                                   !renderer->playback_paused);
    g_message("VividProducer: web renderer configured project=%s",
              project_path ? project_path : "(null)");
    return TRUE;
}

static gboolean
vivid_producer_web_refresh_config(VividProducerRenderer* renderer)
{
    if (!renderer_has_resolved_gpu(renderer, "web")) {
        vivid_producer_web_stop(renderer);
        renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
        return FALSE;
    }

    if (!renderer->web.instance || !renderer->web.configure_func)
        return FALSE;

    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);
    const gboolean ok = renderer->web.configure_func(renderer->web.instance,
                                                     renderer->project_path,
                                                     renderer->user_properties_json,
                                                     renderer->muted,
                                                     volume,
                                                     CLAMP(renderer->content_fit, 0, 3),
                                                     renderer->scene_fps,
                                                     renderer_backend_render_device(renderer));
    if (!ok) {
        g_warning("VividProducer: web renderer failed runtime reconfigure "
                  "project=%s render-device=%s",
                  renderer->project_path ? renderer->project_path : "(null)",
                  renderer_backend_render_device(renderer));
        vivid_producer_web_stop(renderer);
        renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
        return FALSE;
    }

    vivid_producer_web_apply_runtime_media(renderer);
    renderer->web.set_playing_func(renderer->web.instance,
                                   !renderer->playback_paused);
    g_message("VividProducer: web renderer reconfigured project=%s "
              "render-device=%s fps=%d",
              renderer->project_path ? renderer->project_path : "(null)",
              renderer_backend_render_device(renderer),
              renderer->scene_fps);
    return TRUE;
}

static void
vivid_producer_scene_apply_media_state(VividProducerRenderer* renderer)
{
    g_return_if_fail(renderer != NULL);

    if (!renderer->scene.instance)
        return;

    if (renderer->scene.set_media_state_json_func) {
        renderer->scene.set_media_state_json_func(
            renderer->scene.instance,
            renderer->media_state_json ? renderer->media_state_json : default_scene_media_state_json());
    }
}

static void
vivid_producer_scene_apply_audio_samples(VividProducerRenderer* renderer)
{
    g_return_if_fail(renderer != NULL);

    if (!renderer->scene.instance)
        return;

    if (renderer->scene.set_audio_samples_func) {
        renderer->scene.set_audio_samples_func(
            renderer->scene.instance,
            renderer->audio_samples ? renderer->audio_samples : NULL);
    }
}

static void
vivid_producer_scene_apply_runtime_media(VividProducerRenderer* renderer)
{
    /*
     * Media state and audio samples are live consumer facts, not persisted
     * project configuration. The renderer keeps the latest snapshot so every
     * newly-created scene adapter receives the same runtime objects immediately
     * after configure(), including project switches and scene renderer rebuilds.
     */
    vivid_producer_scene_apply_media_state(renderer);
    vivid_producer_scene_apply_audio_samples(renderer);
}

static gboolean
vivid_producer_scene_start(VividProducerRenderer* renderer,
                            const gchar*            project_path)
{
    if (!renderer_has_resolved_gpu(renderer, "scene"))
        return FALSE;

    if (!vivid_producer_scene_load(renderer))
        return FALSE;

    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);
    if (!renderer->scene.configure_func(renderer->scene.instance,
                                        project_path,
                                        renderer->user_properties_json,
                                        renderer->muted,
                                        volume,
                                        CLAMP(renderer->content_fit, 0, 3),
                                        renderer->scene_fps,
                                        renderer_backend_render_device(renderer))) {
        g_warning("VividProducer: scene renderer failed to configure project=%s",
                  project_path ? project_path : "(null)");
        return FALSE;
    }

    renderer->mode = VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING;
    vivid_producer_scene_apply_runtime_media(renderer);
    renderer->scene.set_playing_func(renderer->scene.instance,
                                     !renderer->playback_paused);
    g_message("VividProducer: scene renderer configured project=%s",
              project_path ? project_path : "(null)");
    return TRUE;
}

static void
vivid_producer_video_apply_audio_state(VividProducerRenderer* renderer)
{
    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO &&
        renderer->video.instance &&
        renderer->video.set_audio_state_func) {
        renderer->video.set_audio_state_func(renderer->video.instance,
                                             renderer->muted,
                                             volume);
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING &&
        renderer->scene.instance &&
        renderer->scene.configure_func) {
        renderer->scene.configure_func(renderer->scene.instance,
                                       renderer->project_path,
                                       renderer->user_properties_json,
                                       renderer->muted,
                                       volume,
                                       CLAMP(renderer->content_fit, 0, 3),
                                       renderer->scene_fps,
                                       renderer_backend_render_device(renderer));
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
        renderer->web.instance &&
        renderer->web.configure_func) {
        renderer->web.configure_func(renderer->web.instance,
                                     renderer->project_path,
                                     renderer->user_properties_json,
                                     renderer->muted,
                                     volume,
                                     CLAMP(renderer->content_fit, 0, 3),
                                     renderer->scene_fps,
                                     renderer_backend_render_device(renderer));
    }
}

static void
vivid_producer_video_apply_playback_state(VividProducerRenderer* renderer)
{
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING &&
        renderer->scene.instance &&
        renderer->scene.set_playing_func) {
        renderer->scene.set_playing_func(renderer->scene.instance,
                                         !renderer->playback_paused);
        return;
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
        renderer->web.instance &&
        renderer->web.set_playing_func) {
        renderer->web.set_playing_func(renderer->web.instance,
                                       !renderer->playback_paused);
        return;
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO &&
        renderer->video.instance &&
        renderer->video.set_playing_func) {
        renderer->video.set_playing_func(renderer->video.instance,
                                         !renderer->playback_paused);
    }
}

typedef enum
{
    VIVID_PRODUCER_PROJECT_NONE,
    VIVID_PRODUCER_PROJECT_VIDEO,
    VIVID_PRODUCER_PROJECT_SCENE,
    VIVID_PRODUCER_PROJECT_WEB,
} VividProducerProjectKind;

typedef struct
{
    VividProducerProjectKind kind;
    gchar* video_path;
} VividProducerProjectTarget;

static const gchar*
project_target_kind_label(VividProducerProjectKind kind)
{
    switch (kind) {
    case VIVID_PRODUCER_PROJECT_VIDEO:
        return "video";
    case VIVID_PRODUCER_PROJECT_SCENE:
        return "scene";
    case VIVID_PRODUCER_PROJECT_WEB:
        return "web";
    case VIVID_PRODUCER_PROJECT_NONE:
    default:
        return "none";
    }
}

static void
project_target_clear(VividProducerProjectTarget* target)
{
    if (!target)
        return;
    g_clear_pointer(&target->video_path, g_free);
    target->kind = VIVID_PRODUCER_PROJECT_NONE;
}

static gboolean
string_has_ascii_suffix(const gchar* value, const gchar* suffix)
{
    if (!value || !suffix)
        return FALSE;

    g_autofree gchar* lower_value = g_ascii_strdown(value, -1);
    g_autofree gchar* lower_suffix = g_ascii_strdown(suffix, -1);
    return g_str_has_suffix(lower_value, lower_suffix);
}

static gboolean
path_looks_like_scene_entry(const gchar* path)
{
    return string_has_ascii_suffix(path, ".pkg") ||
        string_has_ascii_suffix(path, ".json");
}

static gboolean
path_looks_like_web_entry(const gchar* path)
{
    return string_has_ascii_suffix(path, ".html") ||
        string_has_ascii_suffix(path, ".htm");
}

static const gchar*
json_string_member_or_null(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member) ||
        json_object_get_null_member(object, member))
        return NULL;
    return json_object_get_string_member(object, member);
}


static gboolean
read_project_json(const gchar* project_dir, JsonParser** out_parser, JsonObject** out_object)
{
    g_autofree gchar* project_json_path =
        g_build_filename(project_dir, "project.json", NULL);
    if (!g_file_test(project_json_path, G_FILE_TEST_IS_REGULAR))
        return FALSE;

    JsonParser* parser = json_parser_new();
    GError* error = NULL;
    if (!json_parser_load_from_file(parser, project_json_path, &error)) {
        g_warning("VividProducer: failed to parse project.json at %s: %s",
                  project_json_path,
                  error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("VividProducer: project.json root is not an object at %s",
                  project_json_path);
        g_object_unref(parser);
        return FALSE;
    }

    *out_parser = parser;
    *out_object = json_node_get_object(root);
    return TRUE;
}

static gchar*
resolve_project_entry_path(const gchar* project_dir, const gchar* entry)
{
    if (!entry || !*entry)
        return NULL;

    gchar* path = g_path_is_absolute(entry)
        ? g_strdup(entry)
        : g_build_filename(project_dir, entry, NULL);

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(path);
        return NULL;
    }

    return path;
}

static void
resolve_project_target(const gchar* project_path, VividProducerProjectTarget* out_target)
{
    out_target->kind = VIVID_PRODUCER_PROJECT_NONE;

    if (!project_path || !*project_path)
        return;

    if (g_file_test(project_path, G_FILE_TEST_IS_REGULAR)) {
        if (path_looks_like_scene_entry(project_path)) {
            out_target->kind = VIVID_PRODUCER_PROJECT_SCENE;
            return;
        }

        if (path_looks_like_web_entry(project_path)) {
            out_target->kind = VIVID_PRODUCER_PROJECT_WEB;
            return;
        }

        out_target->kind = VIVID_PRODUCER_PROJECT_VIDEO;
        out_target->video_path = g_strdup(project_path);
        return;
    }

    if (!g_file_test(project_path, G_FILE_TEST_IS_DIR))
        return;

    JsonParser* parser = NULL;
    JsonObject* object = NULL;
    if (read_project_json(project_path, &parser, &object)) {
        const gchar* type = json_string_member_or_null(object, "type");
        const gchar* entry = json_string_member_or_null(object, "file");
        g_autofree gchar* entry_path = resolve_project_entry_path(project_path, entry);

        if (type && g_ascii_strcasecmp(type, "video") == 0) {
            if (entry_path) {
                out_target->kind = VIVID_PRODUCER_PROJECT_VIDEO;
                out_target->video_path = g_steal_pointer(&entry_path);
            }
        } else if (type && g_ascii_strcasecmp(type, "web") == 0) {
            out_target->kind = VIVID_PRODUCER_PROJECT_WEB;
        } else if ((type && g_ascii_strcasecmp(type, "scene") == 0) ||
                   (!type && entry_path && path_looks_like_scene_entry(entry_path))) {
            out_target->kind = VIVID_PRODUCER_PROJECT_SCENE;
        } else if (!type && entry_path && path_looks_like_web_entry(entry_path)) {
            /* Same legacy detection as the WebUI's resolve_legacy_project_type. */
            out_target->kind = VIVID_PRODUCER_PROJECT_WEB;
        }

        g_object_unref(parser);
        if (out_target->kind != VIVID_PRODUCER_PROJECT_NONE)
            return;
    }

    g_autofree gchar* scene_pkg = g_build_filename(project_path, "scene.pkg", NULL);
    if (g_file_test(scene_pkg, G_FILE_TEST_IS_REGULAR))
        out_target->kind = VIVID_PRODUCER_PROJECT_SCENE;
}

static gboolean
vivid_producer_video_start(VividProducerRenderer* renderer,
                            const gchar*           project_path)
{
    if (!renderer_has_resolved_gpu(renderer, "video"))
        return FALSE;

    if (!project_path || !*project_path) {
        g_message("VividProducer: no project-path configured; using diagnostic producer output");
        return FALSE;
    }

    if (!g_file_test(project_path, G_FILE_TEST_IS_REGULAR)) {
        g_message("VividProducer: project-path is not a regular video file yet: %s; using diagnostic output",
                  project_path);
        return FALSE;
    }

    if (!vivid_producer_video_load(renderer))
        return FALSE;

    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);
    if (!renderer->video.configure_func(renderer->video.instance,
                                        project_path,
                                        renderer->muted,
                                        volume,
                                        CLAMP(renderer->content_fit, 0, 3),
                                        renderer->scene_fps,
                                        renderer_backend_render_device(renderer))) {
        g_warning("VividProducer: video renderer failed to configure project=%s",
                  project_path ? project_path : "(null)");
        return FALSE;
    }

    renderer->mode = VIVID_PRODUCER_RENDERER_MODE_VIDEO;
    vivid_producer_video_apply_audio_state(renderer);
    vivid_producer_video_apply_playback_state(renderer);
    g_message("VividProducer: video renderer configured project=%s",
              project_path ? project_path : "(null)");
    return TRUE;
}

static gboolean
vivid_producer_video_refresh_config(VividProducerRenderer* renderer)
{
    if (!renderer_has_resolved_gpu(renderer, "video")) {
        vivid_producer_video_stop(renderer);
        return FALSE;
    }

    if (!renderer->video.instance || !renderer->video.configure_func)
        return FALSE;

    VividProducerProjectTarget target = {0};
    resolve_project_target(renderer->project_path, &target);
    if (target.kind != VIVID_PRODUCER_PROJECT_VIDEO || !target.video_path) {
        g_warning("VividProducer: active video renderer cannot resolve "
                  "project-path=%s for runtime reconfigure",
                  renderer->project_path ? renderer->project_path : "(null)");
        project_target_clear(&target);
        return FALSE;
    }

    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);
    const gboolean ok = renderer->video.configure_func(renderer->video.instance,
                                                       target.video_path,
                                                       renderer->muted,
                                                       volume,
                                                       CLAMP(renderer->content_fit, 0, 3),
                                                       renderer->scene_fps,
                                                       renderer_backend_render_device(renderer));
    if (!ok) {
        g_warning("VividProducer: video renderer failed runtime "
                  "reconfigure project=%s render-device=%s",
                  target.video_path,
                  renderer_backend_render_device(renderer));
        project_target_clear(&target);
        vivid_producer_video_stop(renderer);
        return FALSE;
    }

    renderer->video.set_playing_func(renderer->video.instance,
                                     !renderer->playback_paused);
    g_message("VividProducer: video renderer reconfigured project=%s "
              "render-device=%s content-fit=%d fps=%d",
              target.video_path,
              renderer_backend_render_device(renderer),
              renderer->content_fit,
              renderer->scene_fps);
    project_target_clear(&target);
    return TRUE;
}

static gboolean
vivid_producer_scene_refresh_config(VividProducerRenderer* renderer)
{
    if (!renderer_has_resolved_gpu(renderer, "scene")) {
        vivid_producer_scene_stop(renderer);
        renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
        return FALSE;
    }

    if (!renderer->scene.instance || !renderer->scene.configure_func)
        return FALSE;

    const gdouble volume = CLAMP((gdouble)renderer->volume / 100.0, 0.0, 1.0);
    const gboolean ok = renderer->scene.configure_func(renderer->scene.instance,
                                                       renderer->project_path,
                                                       renderer->user_properties_json,
                                                       renderer->muted,
                                                       volume,
                                                       CLAMP(renderer->content_fit, 0, 3),
                                                       renderer->scene_fps,
                                                       renderer_backend_render_device(renderer));
    if (!ok) {
        g_warning("VividProducer: scene renderer failed runtime reconfigure "
                  "project=%s render-device=%s",
                  renderer->project_path ? renderer->project_path : "(null)",
                  renderer_backend_render_device(renderer));
        vivid_producer_scene_stop(renderer);
        renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
        return FALSE;
    }

    vivid_producer_scene_apply_runtime_media(renderer);
    renderer->scene.set_playing_func(renderer->scene.instance,
                                     !renderer->playback_paused);
    g_message("VividProducer: scene renderer reconfigured project=%s "
              "render-device=%s content-fit=%d fps=%d",
              renderer->project_path ? renderer->project_path : "(null)",
              renderer_backend_render_device(renderer),
              renderer->content_fit,
              renderer->scene_fps);
    return TRUE;
}

VividProducerRenderer*
vivid_producer_renderer_new(void)
{
    gst_init(NULL, NULL);
    log_gstreamer_environment_once();

    VividProducerRenderer* renderer = g_new0(VividProducerRenderer, 1);
    g_mutex_init(&renderer->latest_lock);
    renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
    renderer->volume = 50;
    renderer->content_fit = 1;
    renderer->scene_fps = 30;
    renderer->media_state_json = g_strdup(default_scene_media_state_json());
    renderer->audio_samples = new_empty_audio_samples_variant();
    renderer->render_device = g_strdup("auto");
    renderer->generation = 1;
    if (!vivid_gpu_devices_enumerate(&renderer->gpu_devices))
        g_warning("VividProducer: GPU device enumeration failed; device selection is unavailable");
    renderer_resolve_gpu_device(renderer);
    return renderer;
}

void
vivid_producer_renderer_free(VividProducerRenderer* renderer)
{
    if (!renderer)
        return;

    vivid_producer_video_stop(renderer);
    vivid_producer_video_unload(renderer);
    vivid_producer_scene_unload(renderer);
    vivid_producer_web_unload(renderer);
    g_clear_pointer(&renderer->project_path, g_free);
    g_clear_pointer(&renderer->user_properties_json, g_free);
    g_clear_pointer(&renderer->media_state_json, g_free);
    g_clear_pointer(&renderer->audio_samples, g_variant_unref);
    g_clear_pointer(&renderer->render_device, g_free);
    g_mutex_clear(&renderer->latest_lock);
    g_free(renderer);
}

void
vivid_producer_renderer_apply_config(VividProducerRenderer*     renderer,
                                      const VividProducerConfig* config)
{
    g_return_if_fail(renderer != NULL);
    g_return_if_fail(config != NULL);

    const gboolean next_muted = config->mute;
    const gint next_volume = CLAMP(config->volume, 0, 100);
    const gint next_content_fit = CLAMP(config->content_fit, 0, 3);
    const gint next_scene_fps = CLAMP(config->scene_fps, 5, 240);
    const gchar* next_user_properties =
        config->user_properties ? config->user_properties : "{}";
    const gchar* next_render_device =
        config->render_device && *config->render_device ? config->render_device : "auto";
    const gboolean project_changed =
        g_strcmp0(renderer->project_path, config->project_path) != 0;
    const gboolean render_device_changed =
        g_strcmp0(renderer->render_device, next_render_device) != 0;
    const gboolean video_config_changed =
        renderer->muted != next_muted ||
        renderer->volume != next_volume ||
        renderer->content_fit != next_content_fit ||
        renderer->scene_fps != next_scene_fps ||
        render_device_changed;
    const gboolean user_properties_changed =
        g_strcmp0(renderer->user_properties_json, next_user_properties) != 0;
    const gboolean scene_config_changed =
        video_config_changed ||
        user_properties_changed;

    renderer->muted = next_muted;
    renderer->volume = next_volume;
    renderer->content_fit = next_content_fit;
    renderer->scene_fps = next_scene_fps;
    g_free(renderer->user_properties_json);
    renderer->user_properties_json = g_strdup(next_user_properties);
    g_free(renderer->render_device);
    renderer->render_device = g_strdup(next_render_device);
    if (render_device_changed)
        renderer_resolve_gpu_device(renderer);

    if (project_changed) {
        g_autofree gchar* old_project_path =
            g_strdup(renderer->project_path ? renderer->project_path : "");

        renderer->generation++;
        g_free(renderer->project_path);
        renderer->project_path = g_strdup(config->project_path ? config->project_path : "");

        VividProducerProjectTarget target = {0};
        resolve_project_target(renderer->project_path, &target);
        g_message("VividProducer: project changed generation=%" G_GUINT64_FORMAT
                  " previous-renderer=%s next-kind=%s old-project=%s "
                  "new-project=%s",
                  renderer->generation,
                  renderer_mode_label(renderer->mode),
                  project_target_kind_label(target.kind),
                  old_project_path,
                  renderer->project_path);

        vivid_producer_video_stop(renderer);
        vivid_producer_scene_stop(renderer);
        vivid_producer_web_stop(renderer);
        switch (target.kind) {
        case VIVID_PRODUCER_PROJECT_VIDEO:
            vivid_producer_video_start(renderer, target.video_path);
            break;
        case VIVID_PRODUCER_PROJECT_SCENE:
            vivid_producer_scene_stop(renderer);
            if (!vivid_producer_scene_start(renderer, renderer->project_path))
                renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
            break;
        case VIVID_PRODUCER_PROJECT_WEB:
            if (!vivid_producer_web_start(renderer, renderer->project_path))
                renderer->mode = VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC;
            break;
        case VIVID_PRODUCER_PROJECT_NONE:
        default:
            if (renderer->project_path && *renderer->project_path) {
                g_message("VividProducer: project-path is not a supported producer target yet: %s; using diagnostic output",
                          renderer->project_path);
            } else {
                g_message("VividProducer: no project-path configured; using diagnostic producer output");
            }
            break;
        }
        project_target_clear(&target);
    } else {
        if (render_device_changed &&
            (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO ||
             renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING ||
             renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB)) {
            /*
             * render-device selects the producer-owned Vulkan/decoder backend.
             * Changing it can replace the exported DMA-BUF ring or its format,
             * so publish a new renderer generation and let producer.c rebind
             * outputs only after the refreshed renderer has a first frame.
             */
            renderer->generation++;
            g_message("VividProducer: render-device changed for %s renderer; "
                      "generation=%" G_GUINT64_FORMAT " next=%s",
                      renderer_mode_label(renderer->mode),
                      renderer->generation,
                      renderer_backend_render_device(renderer));
        }

        if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO &&
            video_config_changed) {
            vivid_producer_video_refresh_config(renderer);
            return;
        }

        if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING &&
            scene_config_changed) {
            vivid_producer_scene_refresh_config(renderer);
            return;
        }

        if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
            scene_config_changed) {
            vivid_producer_web_refresh_config(renderer);
            return;
        }

        vivid_producer_video_apply_audio_state(renderer);
        vivid_producer_video_apply_playback_state(renderer);
    }
}

guint64
vivid_producer_renderer_generation(VividProducerRenderer* renderer)
{
    g_return_val_if_fail(renderer != NULL, 0);
    return renderer->generation;
}

const VividGpuDeviceList*
vivid_producer_renderer_gpu_devices(VividProducerRenderer* renderer)
{
    g_return_val_if_fail(renderer != NULL, NULL);
    return &renderer->gpu_devices;
}

gboolean
vivid_producer_renderer_resolved_gpu(VividProducerRenderer* renderer,
                                      VividGpuDevice*        out_device)
{
    g_return_val_if_fail(renderer != NULL, FALSE);
    g_return_val_if_fail(out_device != NULL, FALSE);

    if (!renderer->resolved_gpu_valid)
        return FALSE;

    *out_device = renderer->resolved_gpu;
    return TRUE;
}

void
vivid_producer_renderer_set_playback_paused(VividProducerRenderer* renderer,
                                             gboolean                paused)
{
    g_return_if_fail(renderer != NULL);

    paused = !!paused;
    if (renderer->playback_paused == paused)
        return;

    renderer->playback_paused = paused;
    vivid_producer_video_apply_playback_state(renderer);
}

void
vivid_producer_renderer_set_media_state_json(VividProducerRenderer* renderer,
                                              const gchar*            media_state_json)
{
    g_return_if_fail(renderer != NULL);

    g_free(renderer->media_state_json);
    renderer->media_state_json =
        g_strdup(media_state_json && *media_state_json
                     ? media_state_json
                     : default_scene_media_state_json());

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING)
        vivid_producer_scene_apply_media_state(renderer);
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB)
        vivid_producer_web_apply_media_state(renderer);
}

void
vivid_producer_renderer_set_audio_samples(VividProducerRenderer* renderer,
                                           GVariant*               audio_samples)
{
    g_return_if_fail(renderer != NULL);

    if (audio_samples && !g_variant_is_of_type(audio_samples, G_VARIANT_TYPE("ad"))) {
        g_warning("VividProducer: rejected audio samples variant type=%s",
                  g_variant_get_type_string(audio_samples));
        return;
    }

    g_clear_pointer(&renderer->audio_samples, g_variant_unref);
    renderer->audio_samples = audio_samples
        ? g_variant_ref_sink(audio_samples)
        : new_empty_audio_samples_variant();

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING)
        vivid_producer_scene_apply_audio_samples(renderer);
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB)
        vivid_producer_web_apply_audio_samples(renderer);
}

gboolean
vivid_producer_renderer_write_frame(VividProducerRenderer* renderer,
                                     guint8*                 pixels,
                                     guint32                 stride,
                                     guint32                 width,
                                     guint32                 height,
                                     guint64                 sequence)
{
    g_return_val_if_fail(renderer != NULL, FALSE);
    g_return_val_if_fail(pixels != NULL, FALSE);

    fill_diagnostic_pattern(pixels, stride, width, height, sequence);
    return TRUE;
}

gboolean
vivid_producer_renderer_prefers_dmabuf_buffers(VividProducerRenderer* renderer)
{
    g_return_val_if_fail(renderer != NULL, FALSE);
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO)
        return renderer->video.instance != NULL;
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING)
        return renderer->scene.instance != NULL;
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB)
        return renderer->web.instance != NULL;
    return FALSE;
}

static void
renderer_dmabuf_caps_append(VividProducerRendererDmaBufCaps* caps,
                            guint32                           fourcc,
                            guint64                           modifier,
                            guint32                           plane_count)
{
    if (!caps || caps->n_caps >= VIVID_PRODUCER_RENDERER_DMABUF_MAX_CAPS)
        return;
    for (guint32 i = 0; i < caps->n_caps; i++) {
        if (caps->caps[i].fourcc == fourcc &&
            caps->caps[i].modifier == modifier &&
            caps->caps[i].plane_count == plane_count) {
            return;
        }
    }
    caps->caps[caps->n_caps++] = (VividProducerRendererDmaBufFormatCap) {
        .fourcc = fourcc,
        .modifier = modifier,
        .plane_count = plane_count,
    };
}

static VividProducerDmaBufMemoryPreference
renderer_memory_preference_from_scene(VividSceneProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT;
    }
}

static VividProducerDmaBufMemoryPreference
renderer_memory_preference_from_video(VividVideoProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    case VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    case VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT;
    }
}

static VividProducerDmaBufMemoryPreference
renderer_memory_preference_from_web(VividWebProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    case VIVID_WEB_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    case VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT;
    }
}

gboolean
vivid_producer_renderer_query_dmabuf_caps(
    VividProducerRenderer*           renderer,
    VividProducerRendererDmaBufCaps* out_caps)
{
    g_return_val_if_fail(renderer != NULL, FALSE);
    g_return_val_if_fail(out_caps != NULL, FALSE);

    memset(out_caps, 0, sizeof(*out_caps));
    out_caps->memory_preference = VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO &&
        renderer->video.instance) {
        if (renderer->video.query_dmabuf_caps_func) {
            VividVideoProducerDmaBufCaps video_caps = {0};
            if (!renderer->video.query_dmabuf_caps_func(renderer->video.instance, &video_caps))
                return FALSE;
            for (guint32 i = 0; i < video_caps.n_caps; i++) {
                renderer_dmabuf_caps_append(out_caps,
                                            video_caps.caps[i].fourcc,
                                            video_caps.caps[i].modifier,
                                            video_caps.caps[i].plane_count);
            }
            out_caps->memory_preference =
                renderer_memory_preference_from_video(video_caps.memory_preference);
            return out_caps->n_caps > 0;
        }
        renderer_dmabuf_caps_append(out_caps, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
        renderer_dmabuf_caps_append(out_caps, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, 1);
        return TRUE;
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
        renderer->web.instance) {
        if (renderer->web.query_dmabuf_caps_func) {
            VividWebProducerDmaBufCaps web_caps = {0};
            if (!renderer->web.query_dmabuf_caps_func(renderer->web.instance, &web_caps))
                return FALSE;
            for (guint32 i = 0; i < web_caps.n_caps; i++) {
                renderer_dmabuf_caps_append(out_caps,
                                            web_caps.caps[i].fourcc,
                                            web_caps.caps[i].modifier,
                                            web_caps.caps[i].plane_count);
            }
            out_caps->memory_preference =
                renderer_memory_preference_from_web(web_caps.memory_preference);
            return out_caps->n_caps > 0;
        }
        renderer_dmabuf_caps_append(out_caps, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 1);
        return TRUE;
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING &&
        renderer->scene.instance) {
        /*
         * Scene modifier caps are cached during the producer's single Vulkan
         * GPU enumeration, matching waywallen's "query the owned backend/pool,
         * then negotiate from immutable tuples" shape. Do not call into the
         * scene module here: wallpaper-scene-renderer initializes Vulkan on an
         * internal looper thread, and creating/probing a second temporary
         * VkInstance from this negotiation path can race the Vulkan loader.
         */
        if (renderer->resolved_gpu_valid && renderer->resolved_gpu.scene_dmabuf_n_caps > 0) {
            for (guint32 i = 0; i < renderer->resolved_gpu.scene_dmabuf_n_caps; i++) {
                renderer_dmabuf_caps_append(
                    out_caps,
                    renderer->resolved_gpu.scene_dmabuf_caps[i].fourcc,
                    renderer->resolved_gpu.scene_dmabuf_caps[i].modifier,
                    renderer->resolved_gpu.scene_dmabuf_caps[i].plane_count);
            }
            out_caps->memory_preference = out_caps->n_caps > 1
                ? VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
                : VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
            return TRUE;
        }
        renderer_dmabuf_caps_append(out_caps, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
        return TRUE;
    }

    return FALSE;
}

static void
producer_renderer_init_buffer_set(VividProducerRendererBufferSet* set)
{
    memset(set, 0, sizeof(*set));
    for (guint buffer = 0; buffer < G_N_ELEMENTS(set->buffers); buffer++) {
        for (guint plane = 0; plane < VIVID_PRODUCER_RENDERER_MAX_PLANES; plane++)
            set->buffers[buffer].planes[plane].fd = -1;
    }
}

static void
renderer_route_buffer_set_init(VividProducerRendererRouteBufferSet* set)
{
    memset(set, 0, sizeof(*set));
    for (guint buffer = 0; buffer < G_N_ELEMENTS(set->buffers); buffer++) {
        for (guint plane = 0; plane < VIVID_PRODUCER_RENDERER_MAX_PLANES; plane++)
            set->buffers[buffer].planes[plane].fd = -1;
    }
}

static void
renderer_route_buffer_set_clear(VividProducerRendererRouteBufferSet* set)
{
    for (guint buffer = 0; buffer < set->n_buffers; buffer++) {
        VividProducerRendererRouteBuffer* entry = &set->buffers[buffer];
        for (guint plane = 0; plane < entry->n_planes; plane++) {
            if (entry->planes[plane].fd >= 0) {
                close(entry->planes[plane].fd);
                entry->planes[plane].fd = -1;
            }
        }
    }
    renderer_route_buffer_set_init(set);
}

static gboolean
renderer_route_buffer_set_take_to_renderer(VividProducerRendererRouteBufferSet* route_set,
                                           VividProducerRendererBufferSet*       out_set,
                                           const gchar*                          producer_label)
{
    producer_renderer_init_buffer_set(out_set);
    out_set->width = route_set->width;
    out_set->height = route_set->height;
    out_set->fourcc = route_set->fourcc;
    out_set->modifier = route_set->modifier;
    out_set->premultiplied = route_set->premultiplied;

    if (route_set->n_buffers == 0) {
        g_warning("VividProducer: %s renderer route published no DMA-BUF buffers",
                  producer_label);
        return FALSE;
    }
    if (route_set->n_buffers > G_N_ELEMENTS(out_set->buffers)) {
        g_warning("VividProducer: %s renderer route published too many buffers "
                  "count=%u capacity=%zu",
                  producer_label,
                  route_set->n_buffers,
                  G_N_ELEMENTS(out_set->buffers));
        return FALSE;
    }
    out_set->n_buffers = route_set->n_buffers;

    /*
     * Scene, video, and web renderer modules are peer clients of this route.
     * Their public C structs intentionally stay source-specific for ABI
     * stability, but after each adapter has moved fds into RendererRouteBufferSet
     * the display binding path below is identical for both producers.
     */
    for (guint buffer = 0; buffer < out_set->n_buffers; buffer++) {
        VividProducerRendererRouteBuffer* src = &route_set->buffers[buffer];
        VividProducerRendererBuffer* dst = &out_set->buffers[buffer];
        if (src->n_planes == 0) {
            g_warning("VividProducer: %s renderer route buffer=%u has no DMA-BUF planes",
                      producer_label,
                      src->index);
            vivid_producer_renderer_buffer_set_clear(out_set);
            return FALSE;
        }
        if (src->n_planes > VIVID_PRODUCER_RENDERER_MAX_PLANES) {
            g_warning("VividProducer: %s renderer route buffer=%u published too many planes "
                      "count=%u capacity=%u",
                      producer_label,
                      src->index,
                      src->n_planes,
                      VIVID_PRODUCER_RENDERER_MAX_PLANES);
            vivid_producer_renderer_buffer_set_clear(out_set);
            return FALSE;
        }

        dst->index = src->index;
        dst->size = src->size;
        dst->n_planes = src->n_planes;
        for (guint plane = 0; plane < dst->n_planes; plane++) {
            if (src->planes[plane].fd < 0) {
                g_warning("VividProducer: %s renderer route buffer=%u plane=%u "
                          "has no DMA-BUF fd",
                          producer_label,
                          src->index,
                          plane);
                vivid_producer_renderer_buffer_set_clear(out_set);
                return FALSE;
            }

            dst->planes[plane].fd = src->planes[plane].fd;
            dst->planes[plane].stride = src->planes[plane].stride;
            dst->planes[plane].offset = src->planes[plane].offset;
            src->planes[plane].fd = -1;
        }
    }

    return TRUE;
}

static void
renderer_route_frame_to_renderer(const VividProducerRendererRouteFrame* route_frame,
                                 VividProducerRendererFrame*            out_frame)
{
    out_frame->buffer_index = route_frame->buffer_index;
    out_frame->source_frame_id = route_frame->source_frame_id;
    out_frame->sequence = route_frame->sequence;
    out_frame->target_time_usec = route_frame->target_time_usec;
    out_frame->acquire_sync_fd = route_frame->acquire_sync_fd;
}

static gboolean
dmabuf_request_allows_fourcc(const VividProducerRendererDmaBufRequest* request,
                             guint32                                    fourcc)
{
    if (!request || request->n_allowed_fourccs == 0)
        return TRUE;

    for (guint i = 0; i < request->n_allowed_fourccs; i++) {
        if (request->allowed_fourccs[i] == fourcc)
            return TRUE;
    }
    return FALSE;
}

static gboolean
dmabuf_request_modifier_matches(const VividProducerRendererDmaBufRequest* request,
                                guint64                                    modifier)
{
    if (!request || !request->require_modifier)
        return TRUE;

    /*
     * LINEAR is the current renderer route contract. Treat INVALID as an
     * implicit LINEAR layout for validation because some producer-side import
     * APIs report "no explicit modifier" for buffers that must still travel on
     * the implicit LINEAR path at the EGL consumer.
     */
    if (request->required_modifier == DRM_FORMAT_MOD_LINEAR)
        return modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;

    return modifier == request->required_modifier;
}

static gboolean
dmabuf_request_accepts_buffer_set(const VividProducerRendererDmaBufRequest* request,
                                  const VividProducerRendererBufferSet*     set,
                                  const gchar*                              producer_label)
{
    if (!request)
        return TRUE;

    if (!dmabuf_request_allows_fourcc(request, set->fourcc)) {
        g_warning("VividProducer: %s renderer route produced fourcc=0x%08x but "
                  "DMA-BUF request=%s does not allow it",
                  producer_label,
                  set->fourcc,
                  request->debug_label ? request->debug_label : "(unnamed)");
        return FALSE;
    }

    if (!dmabuf_request_modifier_matches(request, set->modifier)) {
        g_warning("VividProducer: %s renderer route produced modifier=0x%016"
                  G_GINT64_MODIFIER "x but DMA-BUF request=%s requires "
                  "modifier=0x%016" G_GINT64_MODIFIER "x",
                  producer_label,
                  (guint64)set->modifier,
                  request->debug_label ? request->debug_label : "(unnamed)",
                  (guint64)request->required_modifier);
        return FALSE;
    }

    if (request->require_modifier && request->required_plane_count > 0) {
        for (guint buffer = 0; buffer < set->n_buffers; buffer++) {
            const VividProducerRendererBuffer* entry = &set->buffers[buffer];
            if (entry->n_planes != request->required_plane_count) {
                g_warning("VividProducer: %s renderer route produced buffer=%u "
                          "planes=%u but DMA-BUF request=%s requires planes=%u",
                          producer_label,
                          entry->index,
                          entry->n_planes,
                          request->debug_label ? request->debug_label : "(unnamed)",
                          request->required_plane_count);
                return FALSE;
            }
        }
    }

    return TRUE;
}

static gboolean
renderer_route_buffer_set_take_video(VividVideoProducerBufferSet*         source,
                                     VividProducerRendererRouteBufferSet* route_set)
{
    renderer_route_buffer_set_init(route_set);
    route_set->width = source->width;
    route_set->height = source->height;
    route_set->fourcc = source->fourcc;
    route_set->modifier = source->modifier;
    route_set->premultiplied = source->premultiplied;
    if (source->n_buffers > G_N_ELEMENTS(route_set->buffers))
        return FALSE;
    route_set->n_buffers = source->n_buffers;

    for (guint buffer = 0; buffer < route_set->n_buffers; buffer++) {
        VividVideoProducerBuffer* src = &source->buffers[buffer];
        VividProducerRendererRouteBuffer* dst = &route_set->buffers[buffer];
        if (src->n_planes > VIVID_PRODUCER_RENDERER_MAX_PLANES)
            return FALSE;

        dst->index = src->index;
        dst->size = src->size;
        dst->n_planes = src->n_planes;
        for (guint plane = 0; plane < dst->n_planes; plane++) {
            dst->planes[plane].fd = src->planes[plane].fd;
            dst->planes[plane].stride = src->planes[plane].stride;
            dst->planes[plane].offset = src->planes[plane].offset;
            src->planes[plane].fd = -1;
        }
    }

    return TRUE;
}

static gboolean
renderer_route_buffer_set_take_scene(VividSceneProducerBufferSet*         source,
                                     VividProducerRendererRouteBufferSet* route_set)
{
    renderer_route_buffer_set_init(route_set);
    route_set->width = source->width;
    route_set->height = source->height;
    route_set->fourcc = source->fourcc;
    route_set->modifier = source->modifier;
    route_set->premultiplied = source->premultiplied;
    if (source->n_buffers > G_N_ELEMENTS(route_set->buffers))
        return FALSE;
    route_set->n_buffers = source->n_buffers;

    for (guint buffer = 0; buffer < route_set->n_buffers; buffer++) {
        VividSceneProducerBuffer* src = &source->buffers[buffer];
        VividProducerRendererRouteBuffer* dst = &route_set->buffers[buffer];
        if (src->n_planes > VIVID_PRODUCER_RENDERER_MAX_PLANES)
            return FALSE;

        dst->index = src->index;
        dst->size = src->size;
        dst->n_planes = src->n_planes;
        for (guint plane = 0; plane < dst->n_planes; plane++) {
            dst->planes[plane].fd = src->planes[plane].fd;
            dst->planes[plane].stride = src->planes[plane].stride;
            dst->planes[plane].offset = src->planes[plane].offset;
            src->planes[plane].fd = -1;
        }
    }

    return TRUE;
}

static gboolean
renderer_route_buffer_set_take_web(VividWebProducerBufferSet*           source,
                                   VividProducerRendererRouteBufferSet* route_set)
{
    renderer_route_buffer_set_init(route_set);
    route_set->width = source->width;
    route_set->height = source->height;
    route_set->fourcc = source->fourcc;
    route_set->modifier = source->modifier;
    route_set->premultiplied = source->premultiplied;
    if (source->n_buffers > G_N_ELEMENTS(route_set->buffers))
        return FALSE;
    route_set->n_buffers = source->n_buffers;

    for (guint buffer = 0; buffer < route_set->n_buffers; buffer++) {
        VividWebProducerBuffer* src = &source->buffers[buffer];
        VividProducerRendererRouteBuffer* dst = &route_set->buffers[buffer];
        if (src->n_planes > VIVID_PRODUCER_RENDERER_MAX_PLANES)
            return FALSE;

        dst->index = src->index;
        dst->size = src->size;
        dst->n_planes = src->n_planes;
        for (guint plane = 0; plane < dst->n_planes; plane++) {
            dst->planes[plane].fd = src->planes[plane].fd;
            dst->planes[plane].stride = src->planes[plane].stride;
            dst->planes[plane].offset = src->planes[plane].offset;
            src->planes[plane].fd = -1;
        }
    }

    return TRUE;
}

static VividSceneProducerDmaBufMemoryPreference
scene_memory_preference_from_renderer(VividProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    case VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    case VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEFAULT;
    }
}

static VividVideoProducerDmaBufMemoryPreference
video_memory_preference_from_renderer(VividProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    case VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    case VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEFAULT;
    }
}

static VividWebProducerDmaBufMemoryPreference
web_memory_preference_from_renderer(VividProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    case VIVID_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return VIVID_WEB_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    case VIVID_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEFAULT;
    }
}

static gboolean
renderer_request_allows_fourcc(const VividProducerRendererDmaBufRequest* request,
                               guint32                                    fourcc)
{
    if (!request || request->n_allowed_fourccs == 0)
        return TRUE;
    for (guint i = 0; i < request->n_allowed_fourccs; i++) {
        if (request->allowed_fourccs[i] == fourcc)
            return TRUE;
    }
    return FALSE;
}

static VividVideoProducerDmaBufRequest
video_dmabuf_request_from_renderer(const VividProducerRendererDmaBufRequest* request)
{
    VividVideoProducerDmaBufRequest video_request = {
        .fourcc = 0,
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .plane_count = 1,
        .require_modifier = TRUE,
        .memory_preference = VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    };

    if (!request)
        return video_request;

    video_request.modifier = request->required_modifier;
    video_request.plane_count = request->required_plane_count > 0
        ? request->required_plane_count
        : 1;
    video_request.require_modifier = request->require_modifier;
    video_request.memory_preference =
        video_memory_preference_from_renderer(request->memory_preference);
    return video_request;
}

static VividWebProducerDmaBufRequest
web_dmabuf_request_from_renderer(const VividProducerRendererDmaBufRequest* request)
{
    VividWebProducerDmaBufRequest web_request = {
        .fourcc = DRM_FORMAT_XRGB8888,
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .plane_count = 1,
        .require_modifier = TRUE,
        .memory_preference = VIVID_WEB_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    };

    if (!request)
        return web_request;

    web_request.fourcc = renderer_request_allows_fourcc(request, DRM_FORMAT_XRGB8888)
        ? DRM_FORMAT_XRGB8888
        : (request->n_allowed_fourccs > 0 ? request->allowed_fourccs[0] : DRM_FORMAT_XRGB8888);
    web_request.modifier = request->required_modifier;
    web_request.plane_count = request->required_plane_count > 0
        ? request->required_plane_count
        : 1;
    web_request.require_modifier = request->require_modifier;
    web_request.memory_preference =
        web_memory_preference_from_renderer(request->memory_preference);
    return web_request;
}

static VividSceneProducerDmaBufRequest
scene_dmabuf_request_from_renderer(const VividProducerRendererDmaBufRequest* request)
{
    VividSceneProducerDmaBufRequest scene_request = {
        .fourcc = DRM_FORMAT_ABGR8888,
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .plane_count = 1,
        .require_modifier = TRUE,
        .memory_preference = VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE,
    };

    if (!request)
        return scene_request;

    if (request->n_allowed_fourccs > 0) {
        scene_request.fourcc = request->allowed_fourccs[0];
        for (guint i = 0; i < request->n_allowed_fourccs; i++) {
            if (request->allowed_fourccs[i] == DRM_FORMAT_ABGR8888) {
                scene_request.fourcc = DRM_FORMAT_ABGR8888;
                break;
            }
        }
    }
    scene_request.modifier = request->required_modifier;
    scene_request.plane_count = request->required_plane_count > 0
        ? request->required_plane_count
        : 1;
    scene_request.require_modifier = request->require_modifier;
    scene_request.memory_preference =
        scene_memory_preference_from_renderer(request->memory_preference);
    return scene_request;
}

static VividProducerRendererDmaBufPrepareStatus
renderer_scene_last_prepare_status(VividProducerRenderer* renderer)
{
    if (!renderer ||
        !renderer->scene.instance ||
        !renderer->scene.get_last_dmabuf_prepare_status_func) {
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    switch (renderer->scene.get_last_dmabuf_prepare_status_func(renderer->scene.instance)) {
    case VIVID_SCENE_PRODUCER_DMABUF_PREPARE_OK:
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK;
    case VIVID_SCENE_PRODUCER_DMABUF_PREPARE_NOT_READY:
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_NOT_READY;
    case VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED:
    default:
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }
}

VividProducerRendererDmaBufPrepareStatus
vivid_producer_renderer_prepare_dmabuf_buffers_ex(
    VividProducerRenderer*          renderer,
    guint32                          width,
    guint32                          height,
    gdouble                          render_scale,
    const VividProducerRendererDmaBufRequest* request,
    VividProducerRendererBufferSet* out_set)
{
    g_return_val_if_fail(renderer != NULL,
                         VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED);
    g_return_val_if_fail(out_set != NULL,
                         VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED);

    producer_renderer_init_buffer_set(out_set);
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO) {
        if (!renderer->video.prepare_buffers_func)
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;

        VividVideoProducerBufferSet video_set = {0};
        gboolean video_ready = FALSE;
        if (renderer->video.prepare_buffers_with_request_func) {
            VividVideoProducerDmaBufRequest video_request =
                video_dmabuf_request_from_renderer(request);
            video_ready = renderer->video.prepare_buffers_with_request_func(
                renderer->video.instance,
                width,
                height,
                render_scale,
                &video_request,
                &video_set);
        } else {
            video_ready = renderer->video.prepare_buffers_func(renderer->video.instance,
                                                               width,
                                                               height,
                                                               render_scale,
                                                               &video_set);
        }
        if (!video_ready) {
            g_message("VividProducer: video renderer DMA-BUF buffer set is not "
                      "ready width=%u height=%u scale=%.3f generation=%"
                      G_GUINT64_FORMAT,
                      width,
                      height,
                      render_scale,
                      renderer->generation);
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        }

        VividProducerRendererRouteBufferSet route_set;
        if (!renderer_route_buffer_set_take_video(&video_set, &route_set)) {
            g_warning("VividProducer: video renderer route buffer set is invalid");
            if (renderer->video.buffer_set_clear_func)
                renderer->video.buffer_set_clear_func(&video_set);
            renderer_route_buffer_set_clear(&route_set);
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        }
        if (renderer->video.buffer_set_clear_func)
            renderer->video.buffer_set_clear_func(&video_set);

        gboolean ok = renderer_route_buffer_set_take_to_renderer(&route_set, out_set, "video");
        if (ok && !dmabuf_request_accepts_buffer_set(request, out_set, "video")) {
            vivid_producer_renderer_buffer_set_clear(out_set);
            ok = FALSE;
        }
        renderer_route_buffer_set_clear(&route_set);
        return ok
            ? VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK
            : VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB) {
        if (!renderer->web.prepare_buffers_func)
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;

        VividWebProducerBufferSet web_set = {0};
        gboolean web_ready = FALSE;
        if (renderer->web.prepare_buffers_with_request_func) {
            VividWebProducerDmaBufRequest web_request =
                web_dmabuf_request_from_renderer(request);
            web_ready = renderer->web.prepare_buffers_with_request_func(
                renderer->web.instance,
                width,
                height,
                render_scale,
                &web_request,
                &web_set);
        } else {
            web_ready = renderer->web.prepare_buffers_func(renderer->web.instance,
                                                           width,
                                                           height,
                                                           render_scale,
                                                           &web_set);
        }
        if (!web_ready) {
            g_message("VividProducer: web renderer DMA-BUF buffer set is not "
                      "ready width=%u height=%u scale=%.3f generation=%"
                      G_GUINT64_FORMAT,
                      width,
                      height,
                      render_scale,
                      renderer->generation);
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        }

        VividProducerRendererRouteBufferSet route_set;
        if (!renderer_route_buffer_set_take_web(&web_set, &route_set)) {
            g_warning("VividProducer: web renderer route buffer set is invalid");
            if (renderer->web.buffer_set_clear_func)
                renderer->web.buffer_set_clear_func(&web_set);
            renderer_route_buffer_set_clear(&route_set);
            return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
        }
        if (renderer->web.buffer_set_clear_func)
            renderer->web.buffer_set_clear_func(&web_set);

        gboolean ok = renderer_route_buffer_set_take_to_renderer(&route_set, out_set, "web");
        if (ok && !dmabuf_request_accepts_buffer_set(request, out_set, "web")) {
            vivid_producer_renderer_buffer_set_clear(out_set);
            ok = FALSE;
        }
        renderer_route_buffer_set_clear(&route_set);
        return ok
            ? VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK
            : VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }

    if (renderer->mode != VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING)
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;

    if (!renderer->scene.prepare_buffers_func)
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;

    VividSceneProducerBufferSet scene_set = {0};
    gboolean scene_ready = FALSE;
    if (renderer->scene.prepare_buffers_with_request_func) {
        VividSceneProducerDmaBufRequest scene_request =
            scene_dmabuf_request_from_renderer(request);
        scene_ready = renderer->scene.prepare_buffers_with_request_func(
            renderer->scene.instance,
            width,
            height,
            render_scale,
            &scene_request,
            &scene_set);
    } else {
        scene_ready = renderer->scene.prepare_buffers_func(renderer->scene.instance,
                                                           width,
                                                           height,
                                                           render_scale,
                                                           &scene_set);
    }
    if (!scene_ready) {
        g_message("VividProducer: scene renderer DMA-BUF buffer set is not "
                  "ready width=%u height=%u scale=%.3f generation=%"
                  G_GUINT64_FORMAT,
                  width,
                  height,
                  render_scale,
                  renderer->generation);
        return renderer_scene_last_prepare_status(renderer);
    }

    VividProducerRendererRouteBufferSet route_set;
    if (!renderer_route_buffer_set_take_scene(&scene_set, &route_set)) {
        g_warning("VividProducer: scene renderer route buffer set is invalid");
        if (renderer->scene.buffer_set_clear_func)
            renderer->scene.buffer_set_clear_func(&scene_set);
        renderer_route_buffer_set_clear(&route_set);
        return VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
    }
    if (renderer->scene.buffer_set_clear_func)
        renderer->scene.buffer_set_clear_func(&scene_set);

    gboolean ok = renderer_route_buffer_set_take_to_renderer(&route_set, out_set, "scene");
    if (ok && !dmabuf_request_accepts_buffer_set(request, out_set, "scene")) {
        vivid_producer_renderer_buffer_set_clear(out_set);
        ok = FALSE;
    }
    renderer_route_buffer_set_clear(&route_set);
    return ok
        ? VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK
        : VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_UNSUPPORTED;
}

gboolean
vivid_producer_renderer_prepare_dmabuf_buffers(
    VividProducerRenderer*          renderer,
    guint32                          width,
    guint32                          height,
    gdouble                          render_scale,
    const VividProducerRendererDmaBufRequest* request,
    VividProducerRendererBufferSet* out_set)
{
    return vivid_producer_renderer_prepare_dmabuf_buffers_ex(renderer,
                                                             width,
                                                             height,
                                                             render_scale,
                                                             request,
                                                             out_set) ==
        VIVID_PRODUCER_RENDERER_DMABUF_PREPARE_OK;
}

gboolean
vivid_producer_renderer_next_dmabuf_frame(VividProducerRenderer*      renderer,
                                           VividProducerRendererFrame* out_frame)
{
    g_return_val_if_fail(renderer != NULL, FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    memset(out_frame, 0, sizeof(*out_frame));
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_VIDEO) {
        if (!renderer->video.next_frame_func)
            return FALSE;

        VividVideoProducerFrame video_frame = {0};
        if (!renderer->video.next_frame_func(renderer->video.instance, &video_frame))
            return FALSE;

        const VividProducerRendererRouteFrame route_frame = {
            .buffer_index = video_frame.buffer_index,
            .source_frame_id = video_frame.source_frame_id,
            .sequence = video_frame.sequence,
            .target_time_usec = video_frame.target_time_usec,
            .acquire_sync_fd = video_frame.acquire_sync_fd,
        };
        renderer_route_frame_to_renderer(&route_frame, out_frame);
        return TRUE;
    }

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB) {
        if (!renderer->web.next_frame_func)
            return FALSE;

        VividWebProducerFrame web_frame = {0};
        if (!renderer->web.next_frame_func(renderer->web.instance, &web_frame))
            return FALSE;

        const VividProducerRendererRouteFrame route_frame = {
            .buffer_index = web_frame.buffer_index,
            .source_frame_id = web_frame.source_frame_id,
            .sequence = web_frame.sequence,
            .target_time_usec = web_frame.target_time_usec,
            .acquire_sync_fd = web_frame.acquire_sync_fd,
        };
        renderer_route_frame_to_renderer(&route_frame, out_frame);
        return TRUE;
    }

    if (renderer->mode != VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING)
        return FALSE;

    if (!renderer->scene.next_frame_func)
        return FALSE;

    VividSceneProducerFrame scene_frame = {0};
    if (!renderer->scene.next_frame_func(renderer->scene.instance, &scene_frame))
        return FALSE;

    const VividProducerRendererRouteFrame route_frame = {
        .buffer_index = scene_frame.buffer_index,
        .source_frame_id = scene_frame.source_frame_id,
        .sequence = scene_frame.sequence,
        .target_time_usec = scene_frame.target_time_usec,
        .acquire_sync_fd = scene_frame.acquire_sync_fd,
    };
    renderer_route_frame_to_renderer(&route_frame, out_frame);
    return TRUE;
}

static void
renderer_log_request_frame(VividProducerRenderer* renderer,
                           const gchar*           reason,
                           gboolean               dispatched)
{
    const guint64 now = (guint64)g_get_monotonic_time();
    if (renderer->last_request_frame_log_time_usec != 0 &&
        now - renderer->last_request_frame_log_time_usec < G_USEC_PER_SEC) {
        renderer->request_frame_log_suppressed++;
        return;
    }

    g_message("VividProducer: request renderer DMA-BUF frame mode=%s "
              "generation=%" G_GUINT64_FORMAT " playback-paused=%s "
              "dispatched=%s reason=%s suppressed=%u",
              renderer_mode_label(renderer->mode),
              renderer->generation,
              renderer->playback_paused ? "true" : "false",
              dispatched ? "true" : "false",
              reason && *reason ? reason : "(none)",
              renderer->request_frame_log_suppressed);
    renderer->last_request_frame_log_time_usec = now;
    renderer->request_frame_log_suppressed = 0;
}

gboolean
vivid_producer_renderer_request_dmabuf_frame(VividProducerRenderer* renderer,
                                             const gchar*            reason)
{
    g_return_val_if_fail(renderer != NULL, FALSE);

    gboolean dispatched = FALSE;
    switch (renderer->mode) {
    case VIVID_PRODUCER_RENDERER_MODE_VIDEO:
        if (renderer->video.instance && renderer->video.request_frame_func) {
            renderer->video.request_frame_func(renderer->video.instance, reason);
            dispatched = TRUE;
        }
        break;
    case VIVID_PRODUCER_RENDERER_MODE_WEB:
        if (renderer->web.instance && renderer->web.request_frame_func) {
            renderer->web.request_frame_func(renderer->web.instance, reason);
            dispatched = TRUE;
        }
        break;
    case VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING:
        if (renderer->scene.instance && renderer->scene.request_frame_func) {
            renderer->scene.request_frame_func(renderer->scene.instance, reason);
            dispatched = TRUE;
        }
        break;
    case VIVID_PRODUCER_RENDERER_MODE_DIAGNOSTIC:
    default:
        break;
    }

    renderer_log_request_frame(renderer, reason, dispatched);
    return dispatched;
}

void
vivid_producer_renderer_buffer_set_clear(VividProducerRendererBufferSet* set)
{
    if (!set)
        return;

    for (guint buffer = 0; buffer < set->n_buffers; buffer++) {
        VividProducerRendererBuffer* entry = &set->buffers[buffer];
        for (guint plane = 0; plane < entry->n_planes; plane++) {
            if (entry->planes[plane].fd >= 0) {
                close(entry->planes[plane].fd);
                entry->planes[plane].fd = -1;
            }
        }
    }
    producer_renderer_init_buffer_set(set);
}

void
vivid_producer_renderer_set_release_gate(VividProducerRenderer*         renderer,
                                          const VividRendererReleaseGate* gate)
{
    g_return_if_fail(renderer != NULL);

    if (gate &&
        gate->abi_version == VIVID_RENDERER_RELEASE_GATE_ABI_VERSION &&
        gate->wait_release) {
        renderer->release_gate = *gate;
        renderer->release_gate_valid = TRUE;
    } else {
        memset(&renderer->release_gate, 0, sizeof(renderer->release_gate));
        renderer->release_gate_valid = FALSE;
    }

    vivid_producer_video_apply_release_gate(renderer);
    vivid_producer_scene_apply_release_gate(renderer);
    vivid_producer_web_apply_release_gate(renderer);
}

void
vivid_producer_renderer_pointer_motion(VividProducerRenderer* renderer,
                                        gdouble                 x,
                                        gdouble                 y)
{
    g_return_if_fail(renderer != NULL);

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
        renderer->web.instance &&
        renderer->web.set_pointer_motion_func) {
        renderer->web.set_pointer_motion_func(renderer->web.instance, x, y);
        return;
    }

    if (renderer->mode != VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING ||
        !renderer->scene.instance ||
        !renderer->scene.set_pointer_motion_func)
        return;

    renderer->scene.set_pointer_motion_func(renderer->scene.instance, x, y);
}

void
vivid_producer_renderer_pointer_button(VividProducerRenderer* renderer,
                                        guint32                 button,
                                        gboolean                pressed)
{
    g_return_if_fail(renderer != NULL);

    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
        renderer->web.instance &&
        renderer->web.set_pointer_button_func) {
        renderer->web.set_pointer_button_func(renderer->web.instance, button, pressed);
        return;
    }

    if (renderer->mode != VIVID_PRODUCER_RENDERER_MODE_SCENE_PENDING ||
        !renderer->scene.instance ||
        !renderer->scene.set_pointer_button_func)
        return;

    renderer->scene.set_pointer_button_func(renderer->scene.instance, button, pressed);
}

void
vivid_producer_renderer_pointer_axis(VividProducerRenderer* renderer,
                                      gdouble                 delta_x,
                                      gdouble                 delta_y)
{
    g_return_if_fail(renderer != NULL);

    /* The web backend is the only scroll consumer; scene ignores wheel input. */
    if (renderer->mode == VIVID_PRODUCER_RENDERER_MODE_WEB &&
        renderer->web.instance &&
        renderer->web.set_pointer_axis_func) {
        renderer->web.set_pointer_axis_func(renderer->web.instance, delta_x, delta_y);
        return;
    }

    (void)delta_x;
    (void)delta_y;
}
