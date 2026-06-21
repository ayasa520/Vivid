/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_scene_producer.h"
#include "vivid_gpu_devices.h"
#include "vivid_producer_frame_route.hpp"

#include <drm/drm_fourcc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include "vivid_scene_project.hpp"

#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Vulkan/include/Vulkan/VulkanExSwapchain.hpp"

using vivid::scene::SceneProject;
using vivid::scene::build_scene_audio_samples_from_variant;
using vivid::scene::build_scene_media_state_from_json;
using vivid::scene::configure_scene_wallpaper;
using vivid::scene::ensure_scene_wallpaper;
using vivid::scene::load_scene_project_with_overrides;
using vivid::scene::sync_scene_audio_samples;
using vivid::scene::sync_scene_media_state;
using vivid::scene::sync_scene_user_properties;
using vivid::scene::to_wallpaper_fill_mode;
using vivid::producer::DmabufBufferSetView;
using vivid::producer::ProducerFrameRoute;

namespace
{

constexpr const char* CACHE_DIR_NAME = "vivid_scene_producer";
constexpr guint32     SCENE_DMABUF_FOURCC = DRM_FORMAT_ABGR8888;
constexpr guint32     SCENE_RELEASE_GATE_TIMEOUT_MSEC = 600u;
constexpr gint64      SCENE_MISSED_FRAME_LOG_INTERVAL_USEC =
    2 * G_TIME_SPAN_SECOND;

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

const char* bool_to_string(bool value) { return value ? "true" : "false"; }

struct SceneDmaBufRequest {
    guint32 fourcc { SCENE_DMABUF_FOURCC };
    guint64 modifier { DRM_FORMAT_MOD_LINEAR };
    bool require_modifier { true };
    VividSceneProducerDmaBufMemoryPreference memory_preference {
        VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE
    };
};

bool scene_caps_contains_modifier(const VividSceneProducerDmaBufCaps* caps,
                                  guint32                              fourcc,
                                  guint64                              modifier)
{
    for (guint32 i = 0; i < caps->n_caps; i++) {
        if (caps->caps[i].fourcc == fourcc && caps->caps[i].modifier == modifier)
            return true;
    }
    return false;
}

void scene_caps_append(VividSceneProducerDmaBufCaps* caps,
                       guint32                        fourcc,
                       guint64                        modifier,
                       guint32                        plane_count)
{
    if (!caps || caps->n_caps >= VIVID_SCENE_PRODUCER_DMABUF_MAX_CAPS ||
        scene_caps_contains_modifier(caps, fourcc, modifier))
        return;

    caps->caps[caps->n_caps++] = {
        .fourcc = fourcc,
        .modifier = modifier,
        .plane_count = plane_count,
    };
}

} // namespace

struct _VividSceneProducer
{
    std::unique_ptr<wallpaper::SceneWallpaper> scene;
    SceneProject project;
    std::string project_dir;
    std::string user_properties_json;
    std::string media_state_json;
    std::string render_device { "auto" };
    /*
     * Member storage on purpose: RenderInitInfo.uuid is a std::span and
     * SceneWallpaper::initVulkan() posts a copy of the init info to the
     * renderer thread, so the bytes it points at must outlive this call.
     */
    std::array<guint8, VIVID_GPU_DEVICE_UUID_BYTES> resolved_gpu_uuid {};
    std::shared_ptr<wallpaper::WPSceneScriptMediaState> media_state {
        std::make_shared<wallpaper::WPSceneScriptMediaState>()
    };
    std::shared_ptr<std::vector<float>> audio_samples {
        std::make_shared<std::vector<float>>()
    };

    bool scene_ready { false };
    bool render_ready { false };
    bool playing { true };
    bool muted { false };
    double volume { 1.0 };
    int fill_mode { 1 };
    int fps { 30 };
    guint32 width { 0 };
    guint32 height { 0 };
    double render_scale { 1.0 };
    SceneDmaBufRequest dmabuf_request {};
    VividSceneProducerDmaBufPrepareStatus last_dmabuf_prepare_status {
        VIVID_SCENE_PRODUCER_DMABUF_PREPARE_NOT_READY
    };
    ProducerFrameRoute frame_route { "VividSceneProducer" };
    VividRendererReleaseGate release_gate {};
    bool release_gate_valid { false };
    bool logged_waiting_for_frame { false };
    guint64 missed_frame_count { 0 };
    gint64 last_missed_frame_summary_usec { 0 };
    bool logged_waiting_for_swapchain { false };
    bool logged_swapchain_type_mismatch { false };
    bool logged_empty_swapchain_handles { false };
    bool logged_linear_only_caps { false };
};

namespace
{

void reset_scene_runtime(VividSceneProducer* self) {
    if (!self)
        return;

    self->scene.reset();
    self->scene_ready = false;
    self->render_ready = false;
    self->width = 0;
    self->height = 0;
    self->render_scale = 1.0;
    self->dmabuf_request = SceneDmaBufRequest {};
    self->last_dmabuf_prepare_status =
        VIVID_SCENE_PRODUCER_DMABUF_PREPARE_NOT_READY;
    self->frame_route.reset();
    self->logged_waiting_for_frame = false;
    self->missed_frame_count = 0;
    self->last_missed_frame_summary_usec = 0;
    self->logged_waiting_for_swapchain = false;
    self->logged_swapchain_type_mismatch = false;
    self->logged_empty_swapchain_handles = false;
    self->logged_linear_only_caps = false;
}

SceneDmaBufRequest normalize_scene_dmabuf_request(
    const VividSceneProducerDmaBufRequest* request)
{
    SceneDmaBufRequest normalized {};
    if (!request)
        return normalized;

    normalized.fourcc = request->fourcc ? request->fourcc : SCENE_DMABUF_FOURCC;
    normalized.modifier = request->require_modifier
        ? request->modifier
        : DRM_FORMAT_MOD_LINEAR;
    normalized.require_modifier = request->require_modifier;
    normalized.memory_preference = request->memory_preference;
    return normalized;
}

bool scene_dmabuf_requests_equal(const SceneDmaBufRequest& a,
                                 const SceneDmaBufRequest& b)
{
    return a.fourcc == b.fourcc &&
        a.modifier == b.modifier &&
        a.require_modifier == b.require_modifier &&
        a.memory_preference == b.memory_preference;
}

bool scene_size_contract_changed(const VividSceneProducer* self,
                                 guint32                   width,
                                 guint32                   height,
                                 double                    render_scale)
{
    return self->width != width ||
        self->height != height ||
        std::abs(self->render_scale - render_scale) > 0.0001;
}

wallpaper::ExternalFrameMemoryPreference
to_wallpaper_memory_preference(VividSceneProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return wallpaper::ExternalFrameMemoryPreference::DeviceLocal;
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return wallpaper::ExternalFrameMemoryPreference::HostVisible;
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return wallpaper::ExternalFrameMemoryPreference::Default;
    }
}

const char*
scene_memory_preference_name(VividSceneProducerDmaBufMemoryPreference preference)
{
    switch (preference) {
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL:
        return "device-local";
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE:
        return "host-visible";
    case VIVID_SCENE_PRODUCER_DMABUF_MEMORY_DEFAULT:
    default:
        return "default";
    }
}

void apply_scene_runtime_properties(wallpaper::SceneWallpaper& scene,
                                    double                     volume,
                                    gboolean                   muted,
                                    int                        fill_mode,
                                    int                        fps) {
    scene.setPropertyFloat(wallpaper::PROPERTY_VOLUME, static_cast<float>(volume));
    scene.setPropertyBool(wallpaper::PROPERTY_MUTED, muted);
    scene.setPropertyInt32(
        wallpaper::PROPERTY_FILLMODE,
        static_cast<int32_t>(to_wallpaper_fill_mode(fill_mode)));
    scene.setPropertyInt32(wallpaper::PROPERTY_FPS, fps);
}

void apply_scene_script_runtime_objects(VividSceneProducer* self) {
    if (!self || !self->scene)
        return;

    /*
     * SceneWallpaper owns project/user properties, while media state and audio
     * samples are live desktop facts pushed by the consumer. Keep this bridge
     * explicit so scene recreation, project switches, and runtime media updates
     * all go through the same property-object synchronization path.
     */
    sync_scene_media_state(*self->scene, self->media_state);
    sync_scene_audio_samples(*self->scene, self->audio_samples);
}

bool scene_wait_release_gate(const VividRendererReleaseGate& gate, guint32 buffer_index)
{
    if (gate.abi_version != VIVID_RENDERER_RELEASE_GATE_ABI_VERSION ||
        !gate.wait_release) {
        return true;
    }

    if (gate.wait_release(gate.user_data,
                          buffer_index,
                          SCENE_RELEASE_GATE_TIMEOUT_MSEC)) {
        return true;
    }

    g_warning("VividSceneProducer: release gate timed out for offscreen "
              "slot=%u timeout-ms=%u; skipping scene frame",
              buffer_index,
              SCENE_RELEASE_GATE_TIMEOUT_MSEC);
    return false;
}

void scene_apply_release_gate_callback(VividSceneProducer* self)
{
    if (!self || !self->scene)
        return;

    if (!self->release_gate_valid) {
        self->scene->setOffscreenFrameReleaseCallback({});
        return;
    }

    const VividRendererReleaseGate gate = self->release_gate;
    self->scene->setOffscreenFrameReleaseCallback([gate](std::uint32_t buffer_index) {
        return scene_wait_release_gate(gate, buffer_index);
    });
}

void scene_note_missing_frame(VividSceneProducer* self)
{
    if (!self)
        return;

    const gint64 now = g_get_monotonic_time();
    self->missed_frame_count++;

    if (!self->logged_waiting_for_frame) {
        g_message("VividSceneProducer: swapchain has no frame available yet");
        self->logged_waiting_for_frame = true;
        self->missed_frame_count = 0;
        self->last_missed_frame_summary_usec = now;
        return;
    }

    if (self->last_missed_frame_summary_usec == 0)
        self->last_missed_frame_summary_usec = now;

    if (now - self->last_missed_frame_summary_usec >=
        SCENE_MISSED_FRAME_LOG_INTERVAL_USEC) {
        const double elapsed_sec =
            (now - self->last_missed_frame_summary_usec) / 1000000.0;
        g_message("VividSceneProducer: still waiting for swapchain frames "
                  "missed=%" G_GUINT64_FORMAT " interval=%.2fs",
                  self->missed_frame_count,
                  elapsed_sec);
        self->missed_frame_count = 0;
        self->last_missed_frame_summary_usec = now;
    }
}

} // namespace

VividSceneProducer*
vivid_scene_producer_new(void)
{
    return new VividSceneProducer();
}

void
vivid_scene_producer_free(VividSceneProducer* self)
{
    delete self;
}

gboolean
vivid_scene_producer_configure(VividSceneProducer* self,
                                const gchar*         project_dir,
                                const gchar*         user_properties_json,
                                gboolean             muted,
                                gdouble              volume,
                                gint                 fill_mode,
                                gint                 fps,
                                const gchar*         render_device)
{
    g_return_val_if_fail(self != nullptr, FALSE);

    SceneProject project;
    if (!load_scene_project_with_overrides(project_dir,
                                           user_properties_json,
                                           project,
                                           "producer")) {
        g_warning("VividSceneProducer: rejected project reload because project load failed: %s",
                  project_dir ? project_dir : "(null)");
        return FALSE;
    }

    const bool project_changed =
        self->project.scene_path != project.scene_path ||
        self->project.assets_path != project.assets_path ||
        self->project_dir != (project_dir ? project_dir : "");
    const gboolean next_muted = !!muted;
    const double next_volume = std::clamp(volume, 0.0, 1.0);
    const int next_fill_mode = std::clamp(fill_mode, 1, 3);
    const int next_fps = std::clamp(fps, 5, 240);
    const std::string next_render_device =
        render_device && *render_device ? render_device : "auto";
    const bool user_properties_changed =
        self->user_properties_json != (user_properties_json ? user_properties_json : "");
    const bool runtime_properties_changed =
        self->muted != next_muted ||
        std::abs(self->volume - next_volume) > 0.0001 ||
        self->fill_mode != next_fill_mode ||
        self->fps != next_fps;
    const bool render_device_changed = self->render_device != next_render_device;

    self->project = std::move(project);
    self->project_dir = project_dir ? project_dir : "";
    self->user_properties_json = user_properties_json ? user_properties_json : "";
    self->muted = next_muted;
    self->volume = next_volume;
    self->fill_mode = next_fill_mode;
    self->fps = next_fps;
    self->render_device = next_render_device;

    g_message("VividSceneProducer: configure project=%s project-changed=%s user-properties-changed=%s runtime-properties-changed=%s gpu-changed=%s muted=%s volume=%.3f fill-mode=%d fps=%d render-device=%s",
              self->project_dir.c_str(),
              bool_to_string(project_changed),
              bool_to_string(user_properties_changed),
              bool_to_string(runtime_properties_changed),
              bool_to_string(render_device_changed),
              bool_to_string(self->muted),
              self->volume,
              self->fill_mode,
              self->fps,
              self->render_device.c_str());

    if (project_changed || render_device_changed) {
        /*
         * Producer mode exports the scene renderer's swapchain directly as DMA-BUFs.
         * Project changes can leave the old swapchain tied to the outgoing assets,
         * while render-device changes must rebuild Vulkan and VideoTextureCache on
         * the newly selected physical device. In both cases keep the public buffer
         * contract empty until prepare_buffers() creates a fresh renderer generation.
         */
        reset_scene_runtime(self);
    }

    if (!ensure_scene_wallpaper(self->scene,
                                CACHE_DIR_NAME,
                                "producer",
                                self->project))
        return FALSE;
    scene_apply_release_gate_callback(self);

    if (project_changed || !self->scene_ready) {
        configure_scene_wallpaper(*self->scene,
                                  self->project,
                                  self->volume,
                                  self->muted,
                                  self->fill_mode,
                                  self->fps);
        self->scene_ready = true;
    } else {
        if (user_properties_changed)
            sync_scene_user_properties(*self->scene, self->project);
        if (runtime_properties_changed)
            apply_scene_runtime_properties(*self->scene,
                                           self->volume,
                                           self->muted,
                                           self->fill_mode,
                                           self->fps);
    }

    apply_scene_script_runtime_objects(self);

    if (self->playing)
        self->scene->play();
    else
        self->scene->pause();

    return TRUE;
}

void
vivid_scene_producer_set_playing(VividSceneProducer* self, gboolean playing)
{
    g_return_if_fail(self != nullptr);

    self->playing = !!playing;
    if (!self->scene)
        return;

    if (self->playing)
        self->scene->play();
    else
        self->scene->pause();
}

void
vivid_scene_producer_request_frame(VividSceneProducer* self, const gchar* reason)
{
    g_return_if_fail(self != nullptr);

    if (!self->scene)
        return;

    /*
     * Producer-owned DMA-BUF handoff needs one real rendered slot before
     * BIND_BUFFERS is safe. When policy pause has stopped the scene timer,
     * posting a single draw mirrors waywallen's paused-negotiation fix without
     * changing the user's playback state or restarting the periodic timer.
     */
    g_message("VividSceneProducer: request one DMA-BUF frame playing=%s reason=%s",
              self->playing ? "true" : "false",
              reason && *reason ? reason : "(none)");
    self->scene->requestFrame();
}

void
vivid_scene_producer_set_pointer_motion(VividSceneProducer* self,
                                         gdouble              x,
                                         gdouble              y)
{
    g_return_if_fail(self != nullptr);

    if (!self->scene || self->width == 0 || self->height == 0)
        return;

    const double nx = std::clamp(x / static_cast<double>(self->width), 0.0, 1.0);
    const double ny = std::clamp(y / static_cast<double>(self->height), 0.0, 1.0);
    self->scene->mouseInput(nx, ny);
}

void
vivid_scene_producer_set_pointer_button(VividSceneProducer* self,
                                         guint32              button,
                                         gboolean             pressed)
{
    g_return_if_fail(self != nullptr);

    if (!self->scene)
        return;

    if (button == 1)
        self->scene->mouseLeftButton(!!pressed);
}

void
vivid_scene_producer_set_media_state_json(VividSceneProducer* self,
                                           const gchar*         media_state_json)
{
    g_return_if_fail(self != nullptr);

    self->media_state_json = media_state_json ? media_state_json : "";
    self->media_state =
        build_scene_media_state_from_json(self->media_state_json.c_str(), "producer-runtime");

    if (self->scene)
        sync_scene_media_state(*self->scene, self->media_state);

    g_message("VividSceneProducer: media state applied title='%s' artist='%s' "
              "has-thumbnail=%s thumbnail=%dx%d",
              self->media_state ? self->media_state->title.c_str() : "",
              self->media_state ? self->media_state->artist.c_str() : "",
              self->media_state && self->media_state->has_thumbnail ? "true" : "false",
              self->media_state ? self->media_state->thumbnail_width : 0,
              self->media_state ? self->media_state->thumbnail_height : 0);
}

void
vivid_scene_producer_set_audio_samples(VividSceneProducer* self,
                                        GVariant*            audio_samples)
{
    g_return_if_fail(self != nullptr);

    self->audio_samples =
        build_scene_audio_samples_from_variant(audio_samples, "producer-runtime");

    if (self->scene)
        sync_scene_audio_samples(*self->scene, self->audio_samples);
}

void
vivid_scene_producer_set_release_gate(VividSceneProducer*          self,
                                      const VividRendererReleaseGate* gate)
{
    g_return_if_fail(self != nullptr);

    if (gate && gate->abi_version == VIVID_RENDERER_RELEASE_GATE_ABI_VERSION &&
        gate->wait_release) {
        self->release_gate = *gate;
        self->release_gate_valid = true;
    } else {
        self->release_gate = {};
        self->release_gate_valid = false;
    }
    scene_apply_release_gate_callback(self);
}

gboolean
vivid_scene_producer_query_dmabuf_caps(VividSceneProducer*           self,
                                       VividSceneProducerDmaBufCaps* out_caps)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_caps != nullptr, FALSE);

    memset(out_caps, 0, sizeof(*out_caps));
    scene_caps_append(out_caps, SCENE_DMABUF_FOURCC, DRM_FORMAT_MOD_LINEAR, 1);
    out_caps->memory_preference = VIVID_SCENE_PRODUCER_DMABUF_MEMORY_HOST_VISIBLE;
    if (!self->logged_linear_only_caps) {
        /*
         * Do not create a temporary Vulkan instance here. SceneWallpaper starts
         * its renderer on an internal looper thread, and querying Vulkan
         * modifiers from the producer's negotiation path can race that
         * renderer initialization inside the Vulkan loader/ICD. waywallen gets
         * modifier caps from an already-owned pool/backend; until
         * wallpaper-scene-renderer exposes the same kind of in-device caps
         * query, scene advertises the safe LINEAR export contract only.
         */
        g_message("VividSceneProducer: advertising LINEAR-only DMA-BUF caps for scene "
                  "renderer; modifier caps require an in-renderer Vulkan caps hook");
        self->logged_linear_only_caps = true;
    }
    return TRUE;
}

gboolean
vivid_scene_producer_prepare_buffers_with_request(
    VividSceneProducer*                    self,
    guint32                                width,
    guint32                                height,
    gdouble                                render_scale,
    const VividSceneProducerDmaBufRequest* request,
    VividSceneProducerBufferSet*           out_set)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_set != nullptr, FALSE);

    vivid::producer::init_dmabuf_buffer_set(*out_set);
    self->last_dmabuf_prepare_status =
        VIVID_SCENE_PRODUCER_DMABUF_PREPARE_NOT_READY;

    if (!self->scene_ready || !self->scene) {
        g_warning("VividSceneProducer: cannot prepare buffers before a scene project is configured");
        return FALSE;
    }

    width = std::clamp(width, 1u, 8192u);
    height = std::clamp(height, 1u, 8192u);
    render_scale = std::max(1.0, static_cast<double>(render_scale));
    const SceneDmaBufRequest dmabuf_request = normalize_scene_dmabuf_request(request);
    if (dmabuf_request.fourcc != SCENE_DMABUF_FOURCC) {
        g_warning("VividSceneProducer: unsupported requested DMA-BUF fourcc=0x%08x "
                  "scene-fourcc=0x%08x",
                  dmabuf_request.fourcc,
                  SCENE_DMABUF_FOURCC);
        self->last_dmabuf_prepare_status =
            VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
        return FALSE;
    }

    const bool size_contract_changed =
        self->render_ready &&
        scene_size_contract_changed(self, width, height, render_scale);
    const bool dmabuf_contract_changed =
        self->render_ready &&
        !scene_dmabuf_requests_equal(self->dmabuf_request, dmabuf_request);

    if (size_contract_changed) {
        /*
         * Size and render-scale still define the scene's framebuffer and text
         * rasterization contract.  Keep those as scene rebuild triggers, while
         * modifier/memory-only retries are handled by the route swapchain below
         * so consumer import failures do not reload the wallpaper project.
         */
        g_message("VividSceneProducer: scene size contract changed %ux%u scale=%.3f "
                  "modifier=0x%016" G_GINT64_MODIFIER "x memory=%s -> %ux%u "
                  "scale=%.3f modifier=0x%016" G_GINT64_MODIFIER
                  "x memory=%s; recreating scene",
                  self->width,
                  self->height,
                  self->render_scale,
                  static_cast<guint64>(self->dmabuf_request.modifier),
                  scene_memory_preference_name(self->dmabuf_request.memory_preference),
                  width,
                  height,
                  render_scale,
                  static_cast<guint64>(dmabuf_request.modifier),
                  scene_memory_preference_name(dmabuf_request.memory_preference));
        self->scene.reset();
        self->scene_ready = false;
        self->render_ready = false;
        self->logged_waiting_for_frame = false;
        self->logged_waiting_for_swapchain = false;
        self->logged_swapchain_type_mismatch = false;
        self->logged_empty_swapchain_handles = false;
        if (!ensure_scene_wallpaper(self->scene,
                                    CACHE_DIR_NAME,
                                    "producer-resize",
                                    self->project)) {
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }
        scene_apply_release_gate_callback(self);
        configure_scene_wallpaper(*self->scene,
                                  self->project,
                                  self->volume,
                                  self->muted,
                                  self->fill_mode,
                                  self->fps);
        self->scene_ready = true;
        apply_scene_script_runtime_objects(self);
    }

    if (!self->render_ready) {
        VividGpuDevice gpu_device {};
        if (!vivid_gpu_device_resolve(self->render_device.c_str(), &gpu_device)) {
            g_warning("VividSceneProducer: no usable Vulkan GPU for render-device='%s'",
                      self->render_device.c_str());
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }
        memcpy(self->resolved_gpu_uuid.data(),
               gpu_device.uuid,
               VIVID_GPU_DEVICE_UUID_BYTES);
        const VividGpuDecoderRoute decoder_route =
            vivid_gpu_decoder_route_for_vendor(gpu_device.vendor_id);

        wallpaper::RenderInitInfo info {};
        info.offscreen = true;
        info.export_mode = wallpaper::ExternalFrameExportMode::DMA_BUF;
        /*
         * The deviceUUID pins the exact physical device; the submodule's
         * preference heuristic never runs when a uuid is provided. The decoder
         * route only steers videotexture decoding inside the scene.
         */
        info.uuid = std::span<const std::uint8_t>(self->resolved_gpu_uuid.data(),
                                                  self->resolved_gpu_uuid.size());
        info.device_preference = wallpaper::VulkanDevicePreference::Default;
        info.video_texture_decoder_route =
            decoder_route == VIVID_GPU_DECODER_ROUTE_NVIDIA
                ? wallpaper::VideoTextureDecoderRoute::Nvidia
                : wallpaper::VideoTextureDecoderRoute::Va;
        info.render_node = gpu_device.render_node;
        const bool explicit_non_linear_modifier =
            dmabuf_request.require_modifier &&
            dmabuf_request.modifier != DRM_FORMAT_MOD_LINEAR &&
            dmabuf_request.modifier != DRM_FORMAT_MOD_INVALID;
        info.offscreen_tiling = explicit_non_linear_modifier
            ? wallpaper::TexTiling::OPTIMAL
            : wallpaper::TexTiling::LINEAR;
        info.export_drm_fourcc = dmabuf_request.fourcc;
        info.export_drm_modifiers = explicit_non_linear_modifier
            ? std::vector<uint64_t> { dmabuf_request.modifier }
            : std::vector<uint64_t> {};
        info.export_memory_preference =
            to_wallpaper_memory_preference(dmabuf_request.memory_preference);
        info.width = static_cast<uint16_t>(width);
        info.height = static_cast<uint16_t>(height);
        info.render_scale = render_scale;
        info.ex_swapchain_factory =
            [width,
             height,
             tiling = info.offscreen_tiling,
             export_mode = info.export_mode,
             fourcc = info.export_drm_fourcc,
             modifiers = info.export_drm_modifiers,
             memory_preference = info.export_memory_preference](const wallpaper::RenderInitInfo::ExSwapchainHandles& handles)
                -> std::unique_ptr<wallpaper::vulkan::VulkanExSwapchain> {
            if (!handles.renderer_device) {
                g_warning("VividSceneProducer: common route factory got no renderer device");
                return nullptr;
            }

            /*
             * This is the Vivid equivalent of waywallen's ex_swapchain_factory
             * hook. The scene renderer and the direct-video decoder are peers:
             * both publish into a producer route, while the route owns the
             * exported slots seen by the display protocol. Today the local route
             * still creates renderer-compatible DMA-BUF images through the
             * renderer Device; the raw Vulkan handles are logged and kept in the
             * factory contract so a bridge/pool implementation can replace this
             * local allocator without changing scene producer ownership.
             */
            g_message("VividSceneProducer: creating common scene export route "
                      "device=%p physical=%p queue=%p family=%u",
                      static_cast<void*>(handles.device),
                      static_cast<void*>(handles.physical_device),
                      static_cast<void*>(handles.graphics_queue),
                      handles.graphics_queue_family);
            auto swapchain = wallpaper::vulkan::CreateExSwapchain(
                *handles.renderer_device,
                width,
                height,
                tiling == wallpaper::TexTiling::OPTIMAL
                    ? VK_IMAGE_TILING_OPTIMAL
                    : VK_IMAGE_TILING_LINEAR,
                export_mode,
                fourcc,
                modifiers,
                memory_preference);
            return swapchain;
        };
        info.redraw_callback = []() {
            /*
             * The display protocol has its own frame tick. The callback only
             * tells toolkit paintables to invalidate; producer mode polls the
             * offscreen swapchain directly, so no GLib source is needed here.
             */
        };

        g_message("VividSceneProducer: initVulkan offscreen DMA-BUF %ux%u render-scale=%.3f fourcc=0x%x device=%s (%s) decoder-route=%s",
                  width,
                  height,
                  render_scale,
                  SCENE_DMABUF_FOURCC,
                  gpu_device.name,
                  gpu_device.render_node[0] ? gpu_device.render_node : "unknown-node",
                  vivid_gpu_decoder_route_name(decoder_route));
        self->scene->initVulkan(info);
        self->width = width;
        self->height = height;
        self->render_scale = render_scale;
        self->dmabuf_request = dmabuf_request;
        self->render_ready = true;
        self->logged_waiting_for_swapchain = false;
        self->logged_swapchain_type_mismatch = false;
        self->logged_empty_swapchain_handles = false;
        if (self->playing)
            self->scene->play();
        else
            self->scene->pause();
    }

    auto* base_swapchain = self->scene->exSwapchain();
    if (!base_swapchain) {
        if (!self->logged_waiting_for_swapchain) {
            /*
             * SceneWallpaper::initVulkan() posts initialization to the renderer
             * thread. A producer output can ask for BIND_BUFFERS before that
             * message has created the offscreen swapchain; the producer should
             * keep retrying instead of binding a diagnostic GBM buffer.
             */
            g_message("VividSceneProducer: waiting for Vulkan DMA-BUF swapchain "
                      "after initVulkan request size=%ux%u render-scale=%.3f",
                      self->width,
                      self->height,
                      self->render_scale);
            self->logged_waiting_for_swapchain = true;
        }
        return FALSE;
    }

    auto* swapchain = dynamic_cast<wallpaper::vulkan::VulkanExSwapchain*>(base_swapchain);
    if (!swapchain) {
        if (!self->logged_swapchain_type_mismatch) {
            g_warning("VividSceneProducer: scene swapchain is not a Vulkan "
                      "DMA-BUF swapchain type=%s width=%u height=%u",
                      typeid(*base_swapchain).name(),
                      base_swapchain->width(),
                      base_swapchain->height());
            self->logged_swapchain_type_mismatch = true;
        }
        self->last_dmabuf_prepare_status =
            VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
        return FALSE;
    }

    if (dmabuf_contract_changed && !size_contract_changed) {
        const bool explicit_non_linear_modifier =
            dmabuf_request.require_modifier &&
            dmabuf_request.modifier != DRM_FORMAT_MOD_LINEAR &&
            dmabuf_request.modifier != DRM_FORMAT_MOD_INVALID;
        const wallpaper::TexTiling tiling = explicit_non_linear_modifier
            ? wallpaper::TexTiling::OPTIMAL
            : wallpaper::TexTiling::LINEAR;
        const std::vector<uint64_t> modifiers = explicit_non_linear_modifier
            ? std::vector<uint64_t> { dmabuf_request.modifier }
            : std::vector<uint64_t> {};

        /*
         * This is the waywallen-style long-lived scene route: consumer
         * BIND_FAILED changes the export slot contract, not the wallpaper scene.
         * The call is synchronous but executes allocation on SceneWallpaper's
         * render thread, so TextureCache and the Vulkan device remain
         * renderer-thread-owned while prepare_buffers still reports allocation
         * failure through the existing producer-side blacklist path.
         */
        g_message("VividSceneProducer: DMA-BUF route contract changed "
                  "modifier=0x%016" G_GINT64_MODIFIER "x memory=%s -> "
                  "modifier=0x%016" G_GINT64_MODIFIER "x memory=%s; "
                  "reconfiguring export route without recreating scene",
                  static_cast<guint64>(self->dmabuf_request.modifier),
                  scene_memory_preference_name(self->dmabuf_request.memory_preference),
                  static_cast<guint64>(dmabuf_request.modifier),
                  scene_memory_preference_name(dmabuf_request.memory_preference));
        if (!self->scene->reconfigureOffscreenExport(
                width,
                height,
                tiling,
                wallpaper::ExternalFrameExportMode::DMA_BUF,
                dmabuf_request.fourcc,
                modifiers,
                to_wallpaper_memory_preference(dmabuf_request.memory_preference))) {
            g_warning("VividSceneProducer: failed to reconfigure scene DMA-BUF "
                      "route modifier=0x%016" G_GINT64_MODIFIER "x memory=%s",
                      static_cast<guint64>(dmabuf_request.modifier),
                      scene_memory_preference_name(dmabuf_request.memory_preference));
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }
        self->dmabuf_request = dmabuf_request;
        self->logged_waiting_for_frame = false;
        self->logged_waiting_for_swapchain = false;
        self->logged_empty_swapchain_handles = false;
    }

    auto handles = swapchain->handlesSnapshot();
    if (handles.empty()) {
        if (!self->logged_empty_swapchain_handles) {
            g_warning("VividSceneProducer: Vulkan DMA-BUF swapchain has no handles");
            self->logged_empty_swapchain_handles = true;
        }
        self->last_dmabuf_prepare_status =
            VIVID_SCENE_PRODUCER_DMABUF_PREPARE_NOT_READY;
        return FALSE;
    }

    DmabufBufferSetView route_set;
    route_set.width = width;
    route_set.height = height;
    route_set.fourcc = SCENE_DMABUF_FOURCC;
    route_set.modifier = wallpaper::ExHandle::INVALID_DRM_MODIFIER;
    route_set.premultiplied = FALSE;

    for (const auto& frame : handles) {
        if (!frame.isDmabuf() || frame.n_planes == 0 ||
            frame.n_planes > vivid::producer::kFrameRouteMaxPlanes) {
            g_warning("VividSceneProducer: exported frame id=%d is not a "
                      "supported DMA-BUF frame type=%d is-dmabuf=%s size=%dx%d "
                      "fourcc=0x%x modifier=0x%016" G_GINT64_MODIFIER
                      "x n-planes=%u primary-fd=%d",
                      frame.id(),
                      static_cast<int>(frame.handle_type),
                      bool_to_string(frame.isDmabuf()),
                      frame.width,
                      frame.height,
                      frame.drm_fourcc,
                      static_cast<guint64>(frame.drm_modifier),
                      frame.n_planes,
                      frame.primaryFd());
            vivid_scene_producer_buffer_set_clear(out_set);
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }

        if (route_set.n_buffers >= vivid::producer::kFrameRouteMaxBuffers) {
            g_warning("VividSceneProducer: too many swapchain buffers limit=%u",
                      vivid::producer::kFrameRouteMaxBuffers);
            vivid_scene_producer_buffer_set_clear(out_set);
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }

        const guint64 frame_modifier =
            frame.drm_modifier == wallpaper::ExHandle::INVALID_DRM_MODIFIER
                ? DRM_FORMAT_MOD_LINEAR
                : frame.drm_modifier;
        if (dmabuf_request.require_modifier && frame_modifier != dmabuf_request.modifier) {
            g_warning("VividSceneProducer: exported scene DMA-BUF modifier mismatch "
                      "frame-id=%d expected=0x%016" G_GINT64_MODIFIER
                      "x actual=0x%016" G_GINT64_MODIFIER "x",
                      frame.id(),
                      static_cast<guint64>(dmabuf_request.modifier),
                      static_cast<guint64>(frame_modifier));
            vivid_scene_producer_buffer_set_clear(out_set);
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }
        if (!dmabuf_request.require_modifier && frame_modifier != DRM_FORMAT_MOD_LINEAR) {
            g_warning("VividSceneProducer: refusing non-linear scene DMA-BUF "
                      "frame-id=%d modifier=0x%016" G_GINT64_MODIFIER "x",
                      frame.id(),
                      static_cast<guint64>(frame_modifier));
            vivid_scene_producer_buffer_set_clear(out_set);
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }

        if (route_set.modifier == wallpaper::ExHandle::INVALID_DRM_MODIFIER)
            route_set.modifier = frame_modifier;
        if (route_set.modifier != frame_modifier) {
            g_warning("VividSceneProducer: mixed modifiers are not supported "
                      "in one buffer generation first=0x%016" G_GINT64_MODIFIER
                      "x frame=0x%016" G_GINT64_MODIFIER "x frame-id=%d",
                      static_cast<guint64>(route_set.modifier),
                      static_cast<guint64>(frame_modifier),
                      frame.id());
            vivid_scene_producer_buffer_set_clear(out_set);
            self->last_dmabuf_prepare_status =
                VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
            return FALSE;
        }

        auto& buffer = route_set.buffers[route_set.n_buffers++];
        buffer.index = static_cast<guint32>(frame.id());
        buffer.size = static_cast<guint64>(frame.size);
        buffer.n_planes = frame.n_planes;
        for (guint plane = 0; plane < frame.n_planes; plane++) {
            const int source_fd = frame.planes[plane].fd >= 0
                ? frame.planes[plane].fd
                : frame.primaryFd();
            buffer.planes[plane].fd = source_fd;
            buffer.planes[plane].stride = frame.planes[plane].stride;
            buffer.planes[plane].offset = frame.planes[plane].offset;
        }

        route_set.premultiplied = frame.premultiplied;
    }

    if (route_set.modifier == wallpaper::ExHandle::INVALID_DRM_MODIFIER)
        route_set.modifier = DRM_FORMAT_MOD_LINEAR;

    if (!self->frame_route.publish_buffer_set(route_set, *out_set)) {
        self->last_dmabuf_prepare_status =
            VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED;
        return FALSE;
    }

    g_message("VividSceneProducer: prepared DMA-BUF buffer set %ux%u render-scale=%.3f "
              "buffers=%u modifier=0x%016" G_GINT64_MODIFIER "x",
              out_set->width,
              out_set->height,
              self->render_scale,
              out_set->n_buffers,
              static_cast<guint64>(out_set->modifier));
    if (out_set->n_buffers == 0)
        return FALSE;

    self->last_dmabuf_prepare_status =
        VIVID_SCENE_PRODUCER_DMABUF_PREPARE_OK;
    return TRUE;
}

gboolean
vivid_scene_producer_prepare_buffers(VividSceneProducer*          self,
                                      guint32                       width,
                                      guint32                       height,
                                      gdouble                       render_scale,
                                      VividSceneProducerBufferSet* out_set)
{
    return vivid_scene_producer_prepare_buffers_with_request(self,
                                                            width,
                                                            height,
                                                            render_scale,
                                                            nullptr,
                                                            out_set);
}

VividSceneProducerDmaBufPrepareStatus
vivid_scene_producer_get_last_dmabuf_prepare_status(VividSceneProducer* self)
{
    g_return_val_if_fail(self != nullptr,
                         VIVID_SCENE_PRODUCER_DMABUF_PREPARE_UNSUPPORTED);
    return self->last_dmabuf_prepare_status;
}

gboolean
vivid_scene_producer_next_frame(VividSceneProducer*      self,
                                 VividSceneProducerFrame* out_frame)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_frame != nullptr, FALSE);

    memset(out_frame, 0, sizeof(*out_frame));

    if (!self->render_ready || !self->scene || !self->scene->exSwapchain())
        return FALSE;

    auto* frame = self->scene->exSwapchain()->eatFrame();
    if (!frame) {
        scene_note_missing_frame(self);
        return FALSE;
    }

    self->frame_route.write_ready_frame(static_cast<guint32>(frame->id()),
                                        frame->id(),
                                        *out_frame);
    return TRUE;
}

void
vivid_scene_producer_buffer_set_clear(VividSceneProducerBufferSet* set)
{
    if (!set)
        return;

    vivid::producer::clear_dmabuf_buffer_set(*set);
}
