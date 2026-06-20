/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * CEF-based "web" wallpaper backend.
 *
 * One Vulkan-created export ring (3 linear XRGB8888 DMA-BUF images on the
 * resolved GPU) is the only DMA-BUF contract this backend publishes. Frames
 * arrive through OnAcceleratedPaint: CEF exports a dmabuf, we import it with
 * Vulkan and copy into the exported ring slot on the GPU. Shared-texture
 * failures are reported as errors and must not be silently converted into
 * software output.
 *
 * Threading: the C ABI entry points run on the producer's GLib main thread;
 * CEF callbacks run on CEF's internal UI/audio threads
 * (multi_threaded_message_loop). VividWebClient::producer_lock_ serializes
 * callback access to the producer, and producer state lives behind
 * self->lock. Lock order is always client.producer_lock_ -> self->lock.
 */
/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */
#include "vivid_web_producer.h"
#include "vivid_gpu_devices.h"
#include "vivid_producer_frame_route.hpp"
#include "vivid_web_vulkan_route.hpp"

#include <dlfcn.h>

#include <gio/gio.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <json-glib/json-glib.h>

#include <drm/drm_fourcc.h>

#include <include/base/cef_bind.h>
#include <include/base/cef_callback.h>
#include <include/cef_app.h>
#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <include/cef_task_manager.h>
#include <include/wrapper/cef_closure_task.h>
#include <include/wrapper/cef_helpers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using vivid::producer::DmabufBufferSetView;
using vivid::producer::ProducerFrameRoute;

namespace
{

constexpr guint32 WEB_RING_FOURCC = DRM_FORMAT_XRGB8888;
constexpr guint WEB_RING_BUFFERS = VIVID_WEB_PRODUCER_MAX_BUFFERS;
constexpr guint32 WEB_RELEASE_GATE_TIMEOUT_MSEC = 600u;
constexpr int DEFAULT_VIEW_WIDTH = 1280;
constexpr int DEFAULT_VIEW_HEIGHT = 720;
constexpr double CHROMIUM_ZOOM_BASE = 1.2;
constexpr const char* ENV_DRI_PRIME = "DRI_PRIME";
constexpr const char* ENV_NV_PRIME_RENDER_OFFLOAD = "__NV_PRIME_RENDER_OFFLOAD";
constexpr const char* ENV_EGL_VENDOR_LIBRARY_FILENAMES =
    "__EGL_VENDOR_LIBRARY_FILENAMES";
constexpr const char* ENV_EGL_VENDOR_LIBRARY_DIRS = "__EGL_VENDOR_LIBRARY_DIRS";
constexpr const char* ENV_GBM_SHIM_REWRITE = "VIVID_GBM_SHIM_REWRITE";
constexpr const char* ENV_GBM_SHIM_FORCE_RENDER_NODE = "VIVID_GBM_SHIM_FORCE_RENDER_NODE";
constexpr const char* CEF_SWITCH_PROCESS_TYPE = "type";
constexpr const char* CEF_SWITCH_NO_ZYGOTE = "no-zygote";
constexpr const char* CEF_SWITCH_DISABLE_GPU_PROCESS_CRASH_LIMIT =
    "disable-gpu-process-crash-limit";
constexpr const char* CEF_SWITCH_DISABLE_DOMAIN_BLOCKING_FOR_3D_APIS =
    "disable-domain-blocking-for-3d-apis";
constexpr const char* CEF_SWITCH_RENDER_NODE_OVERRIDE = "render-node-override";
constexpr const char* CEF_SWITCH_VIVID_RENDER_NODE = "vivid-render-node";
constexpr const char* CEF_SWITCH_VIVID_DRI_PRIME = "vivid-dri-prime";
constexpr const char* CEF_SWITCH_VIVID_EGL_VENDOR = "vivid-egl-vendor";
constexpr const char* CEF_FEATURE_SPARE_RENDERER_FOR_SITE_PER_PROCESS =
    "SpareRendererForSitePerProcess";

enum class WebViewportMode
{
    LogicalDip,
    AcceleratedPhysical,
};

struct WebViewport
{
    int physical_width { 0 };
    int physical_height { 0 };
    double scale { 1.0 };
    int dip_width { DEFAULT_VIEW_WIDTH };
    int dip_height { DEFAULT_VIEW_HEIGHT };
    int view_width { DEFAULT_VIEW_WIDTH };
    int view_height { DEFAULT_VIEW_HEIGHT };
    double screen_scale { 1.0 };
    double zoom_level { 0.0 };
    WebViewportMode mode { WebViewportMode::LogicalDip };
    bool configured { false };
};

struct CefGpuProcessPolicy
{
    VividGpuDevice gpu {};
    std::string dri_prime;
    std::string egl_vendor_library_filename;
    bool use_nvidia_prime { false };
    bool use_gbm_shim_rewrite { false };
    bool valid { false };
    guint64 serial { 0 };
};

GMutex g_gpu_policy_lock;
guint64 g_gpu_policy_serial = 0;
CefGpuProcessPolicy g_gpu_policy;

CefGpuProcessPolicy
gpu_process_policy_snapshot()
{
    g_mutex_lock(&g_gpu_policy_lock);
    const CefGpuProcessPolicy policy = g_gpu_policy;
    g_mutex_unlock(&g_gpu_policy_lock);
    return policy;
}

double
zoom_level_for_page_zoom(double page_zoom)
{
    if (page_zoom <= 0.0)
        return 0.0;
    return log(page_zoom) / log(CHROMIUM_ZOOM_BASE);
}

WebViewport
resolve_viewport(int width_px, int height_px, double scale, bool shared_textures)
{
    WebViewport viewport;
    viewport.physical_width = width_px;
    viewport.physical_height = height_px;
    viewport.scale = scale > 0.0 ? scale : 1.0;
    viewport.configured = width_px > 0 && height_px > 0;
    if (viewport.configured) {
        viewport.dip_width = MAX(1, (int)lround((double)width_px / viewport.scale));
        viewport.dip_height = MAX(1, (int)lround((double)height_px / viewport.scale));
    }
    viewport.mode = shared_textures
        ? WebViewportMode::AcceleratedPhysical
        : WebViewportMode::LogicalDip;

    if (viewport.mode == WebViewportMode::AcceleratedPhysical && viewport.configured) {
        /*
         * Linux CEF shared textures are allocated in GetViewRect units, not in
         * GetViewRect * device_scale_factor units. A DIP-sized view therefore
         * gives us a 1600x1000 dmabuf on a 3200x2000/200% desktop and the
         * downstream Vulkan copy has to upscale it. Keep the accelerated view in
         * physical pixels, then use page zoom to preserve Wallpaper Engine's
         * logical viewport contract: innerWidth stays 1600, DPR becomes 2, and
         * the shared texture is still 3200x2000.
         */
        viewport.view_width = viewport.physical_width;
        viewport.view_height = viewport.physical_height;
        viewport.screen_scale = 1.0;
        viewport.zoom_level = zoom_level_for_page_zoom(viewport.scale);
    } else {
        viewport.view_width = viewport.dip_width;
        viewport.view_height = viewport.dip_height;
        viewport.screen_scale = viewport.scale;
        viewport.zoom_level = 0.0;
    }
    return viewport;
}

int
physical_to_cef_view_coordinate(double physical_coordinate, const WebViewport& viewport)
{
    const double view_coordinate =
        viewport.mode == WebViewportMode::AcceleratedPhysical
            ? physical_coordinate
            : physical_coordinate / viewport.scale;
    return (int)lround(view_coordinate);
}

const char*
bool_to_string(bool value)
{
    return value ? "true" : "false";
}

bool
web_export_requests_equal(const VividWebVulkanExportRequest& a,
                          const VividWebVulkanExportRequest& b)
{
    return a.fourcc == b.fourcc &&
        a.modifier == b.modifier &&
        a.plane_count == b.plane_count &&
        a.require_modifier == b.require_modifier &&
        a.memory == b.memory;
}

VividWebVulkanExportMemory
web_export_memory_from_request(VividWebProducerDmaBufMemoryPreference preference)
{
    return preference == VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
        ? VividWebVulkanExportMemory::DeviceLocal
        : VividWebVulkanExportMemory::HostVisible;
}

VividWebVulkanExportRequest
web_export_request_from_abi(const VividWebProducerDmaBufRequest* request)
{
    VividWebVulkanExportRequest export_request;
    export_request.fourcc = WEB_RING_FOURCC;
    export_request.modifier = DRM_FORMAT_MOD_LINEAR;
    export_request.plane_count = 1;
    export_request.require_modifier = true;
    export_request.memory = VividWebVulkanExportMemory::HostVisible;
    if (request) {
        export_request.fourcc = request->fourcc != 0 ? request->fourcc : WEB_RING_FOURCC;
        export_request.modifier = request->require_modifier
            ? request->modifier
            : DRM_FORMAT_MOD_LINEAR;
        export_request.plane_count = request->plane_count > 0 ? request->plane_count : 1;
        export_request.require_modifier = request->require_modifier;
        export_request.memory = web_export_memory_from_request(request->memory_preference);
    }
    return export_request;
}

const char*
env_value_for_log(const char* name)
{
    const gchar* value = g_getenv(name);
    return value && *value ? value : "(unset)";
}

std::string
dri_prime_value_for_pci_address(const gchar* pci_address)
{
    if (!pci_address || !*pci_address)
        return {};

    gchar* tag = g_strdup_printf("pci-%s", pci_address);
    for (gchar* c = tag; *c; c++) {
        if (*c == ':' || *c == '.')
            *c = '_';
    }

    std::string result = tag;
    g_free(tag);
    return result;
}

void
set_process_environment_value(const char* name, const char* value)
{
    if (value && *value)
        g_setenv(name, value, TRUE);
    else
        g_unsetenv(name);
}

void
apply_gpu_process_policy_to_environment_unlocked(const CefGpuProcessPolicy& policy)
{
    g_return_if_fail(policy.valid);

    set_process_environment_value(ENV_DRI_PRIME, policy.dri_prime.c_str());
    set_process_environment_value(ENV_NV_PRIME_RENDER_OFFLOAD,
                                  policy.use_nvidia_prime ? "1" : nullptr);
    set_process_environment_value(ENV_EGL_VENDOR_LIBRARY_FILENAMES,
                                  policy.egl_vendor_library_filename.c_str());

    if (policy.use_gbm_shim_rewrite) {
        set_process_environment_value(ENV_GBM_SHIM_FORCE_RENDER_NODE,
                                      policy.gpu.render_node);
        set_process_environment_value(ENV_GBM_SHIM_REWRITE, "1");
    } else {
        set_process_environment_value(ENV_GBM_SHIM_FORCE_RENDER_NODE, nullptr);
        set_process_environment_value(ENV_GBM_SHIM_REWRITE, nullptr);
    }
}

void
add_unique_path(std::vector<std::string>& paths, const std::string& path)
{
    if (path.empty())
        return;

    gchar* canonical = g_canonicalize_filename(path.c_str(), NULL);
    std::string normalized = canonical ? canonical : path;
    g_free(canonical);

    if (std::find(paths.begin(), paths.end(), normalized) == paths.end())
        paths.push_back(normalized);
}

void
add_colon_separated_paths(std::vector<std::string>& paths, const gchar* value)
{
    if (!value || !*value)
        return;

    gchar** split = g_strsplit(value, G_SEARCHPATH_SEPARATOR_S, -1);
    for (guint i = 0; split[i]; i++)
        add_unique_path(paths, split[i]);
    g_strfreev(split);
}

void
add_glvnd_data_dirs(std::vector<std::string>& dirs)
{
    const gchar* user_data_dir = g_get_user_data_dir();
    if (user_data_dir && *user_data_dir) {
        gchar* path = g_build_filename(user_data_dir, "glvnd", "egl_vendor.d", NULL);
        add_unique_path(dirs, path);
        g_free(path);
    }

    const gchar* const* system_data_dirs = g_get_system_data_dirs();
    for (guint i = 0; system_data_dirs && system_data_dirs[i]; i++) {
        gchar* path =
            g_build_filename(system_data_dirs[i], "glvnd", "egl_vendor.d", NULL);
        add_unique_path(dirs, path);
        g_free(path);
    }
}

void
add_existing_dir(std::vector<std::string>& dirs, const gchar* path)
{
    if (path && *path && g_file_test(path, G_FILE_TEST_IS_DIR))
        add_unique_path(dirs, path);
}

void
add_gl_root_egl_vendor_dirs(std::vector<std::string>& dirs, const std::string& gl_root)
{
    if (!g_file_test(gl_root.c_str(), G_FILE_TEST_IS_DIR))
        return;

    gchar* merged_glvnd = g_build_filename(gl_root.c_str(), "glvnd", "egl_vendor.d", NULL);
    add_existing_dir(dirs, merged_glvnd);
    g_free(merged_glvnd);

    gchar* default_glvnd =
        g_build_filename(gl_root.c_str(), "default", "share", "glvnd", "egl_vendor.d", NULL);
    add_existing_dir(dirs, default_glvnd);
    g_free(default_glvnd);

    GError* error = NULL;
    GDir* dir = g_dir_open(gl_root.c_str(), 0, &error);
    if (!dir) {
        g_clear_error(&error);
        return;
    }

    const gchar* name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.')
            continue;
        gchar* path =
            g_build_filename(gl_root.c_str(), name, "glvnd", "egl_vendor.d", NULL);
        add_existing_dir(dirs, path);
        g_free(path);
    }
    g_dir_close(dir);
}

void
add_flatpak_gl_extension_dirs(std::vector<std::string>& dirs)
{
    const std::array<const char*, 6> library_roots {
        "/app/lib", "/app/lib64", "/usr/lib", "/usr/lib64", "/lib", "/lib64",
    };

    /*
     * Flatpak mounts GL extensions below a lib root, usually under an
     * architecture triplet (for example .../x86_64-linux-gnu/GL). Probe that
     * shape directly so discovery is architecture-independent without walking
     * the entire library tree.
     */
    for (const char* root : library_roots) {
        add_gl_root_egl_vendor_dirs(dirs, std::string(root) + "/GL");

        GError* error = NULL;
        GDir* dir = g_dir_open(root, 0, &error);
        if (!dir) {
            g_clear_error(&error);
            continue;
        }

        const gchar* name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (name[0] == '.')
                continue;
            gchar* gl_root = g_build_filename(root, name, "GL", NULL);
            add_gl_root_egl_vendor_dirs(dirs, gl_root);
            g_free(gl_root);
        }
        g_dir_close(dir);
    }
}

std::vector<std::string>
discover_egl_vendor_dirs(void)
{
    std::vector<std::string> dirs;

    /* Prefer GLVND's own directory override, then probe the installed layouts. */
    add_colon_separated_paths(dirs, g_getenv(ENV_EGL_VENDOR_LIBRARY_DIRS));
    add_glvnd_data_dirs(dirs);
    add_flatpak_gl_extension_dirs(dirs);

    return dirs;
}

std::string
lowercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool
cef_feature_is_chromium_vulkan(const std::string& feature)
{
    static const std::array<const char*, 3> chromium_vulkan_features {
        "Vulkan",
        "DefaultANGLEVulkan",
        "VulkanFromANGLE",
    };

    return std::find(chromium_vulkan_features.begin(),
                     chromium_vulkan_features.end(),
                     feature) != chromium_vulkan_features.end();
}

std::string
join_feature_list(const std::vector<std::string>& features)
{
    std::string result;
    for (const std::string& feature : features) {
        if (!result.empty())
            result += ",";
        result += feature;
    }
    return result;
}

std::string
cef_feature_list_without_chromium_vulkan(const gchar* value, bool& removed_vulkan)
{
    removed_vulkan = false;
    if (!value || !*value)
        return {};

    std::vector<std::string> kept;
    gchar** features = g_strsplit(value, ",", -1);
    for (guint i = 0; features[i]; i++) {
        const std::string feature = features[i];
        if (cef_feature_is_chromium_vulkan(feature)) {
            removed_vulkan = true;
            continue;
        }
        if (!feature.empty())
            kept.push_back(feature);
    }
    g_strfreev(features);

    return join_feature_list(kept);
}

std::string
cef_feature_list_with_chromium_vulkan_disabled(const gchar* value)
{
    std::vector<std::string> features;
    gchar** split = g_strsplit(value ? value : "", ",", -1);
    for (guint i = 0; split[i]; i++) {
        const std::string feature = split[i];
        if (!feature.empty() &&
            std::find(features.begin(), features.end(), feature) == features.end()) {
            features.push_back(feature);
        }
    }
    g_strfreev(split);

    static const std::array<const char*, 3> required_disabled_features {
        "Vulkan",
        "DefaultANGLEVulkan",
        "VulkanFromANGLE",
    };
    for (const char* feature : required_disabled_features) {
        if (std::find(features.begin(), features.end(), feature) == features.end())
            features.push_back(feature);
    }

    return join_feature_list(features);
}

std::string
cef_feature_list_with_feature(const gchar* value, const char* required_feature)
{
    std::vector<std::string> features;
    gchar** split = g_strsplit(value ? value : "", ",", -1);
    for (guint i = 0; split[i]; i++) {
        const std::string feature = split[i];
        if (!feature.empty() &&
            std::find(features.begin(), features.end(), feature) == features.end()) {
            features.push_back(feature);
        }
    }
    g_strfreev(split);

    if (required_feature && *required_feature &&
        std::find(features.begin(), features.end(), required_feature) == features.end()) {
        features.push_back(required_feature);
    }

    return join_feature_list(features);
}

bool
egl_vendor_json_matches(const std::string& path, bool use_nvidia)
{
    JsonParser* parser = json_parser_new();
    GError* error = NULL;
    const gboolean loaded = json_parser_load_from_file(parser, path.c_str(), &error);
    if (!loaded) {
        g_clear_error(&error);
        g_object_unref(parser);
        return false;
    }

    bool matches = false;
    JsonNode* root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject* object = json_node_get_object(root);
        JsonObject* icd = json_object_has_member(object, "ICD") ?
            json_object_get_object_member(object, "ICD") : NULL;
        const gchar* library_path =
            icd && json_object_has_member(icd, "library_path") ?
                json_object_get_string_member(icd, "library_path") : NULL;
        const std::string library = lowercase_ascii(library_path ? library_path : "");
        matches = use_nvidia ?
            library.find("nvidia") != std::string::npos :
            library.find("mesa") != std::string::npos;
    }

    g_object_unref(parser);
    return matches;
}

std::string
egl_vendor_library_filename_for_gpu(bool use_nvidia)
{
    /*
     * GLVND selects the EGL implementation from vendor JSON files, not by
     * dlopening libEGL_nvidia/libEGL_mesa directly. Always choosing one side
     * makes the CEF/ANGLE EGL vendor policy explicit for both NVIDIA and Mesa,
     * while discovery keeps the path portable across Flatpak and direct-run.
     */
    const std::vector<std::string> dirs = discover_egl_vendor_dirs();
    for (const std::string& dir : dirs) {
        GError* error = NULL;
        GDir* gdir = g_dir_open(dir.c_str(), 0, &error);
        if (!gdir) {
            g_clear_error(&error);
            continue;
        }

        const gchar* name = NULL;
        while ((name = g_dir_read_name(gdir)) != NULL) {
            if (!g_str_has_suffix(name, ".json"))
                continue;
            gchar* path = g_build_filename(dir.c_str(), name, NULL);
            const std::string candidate = path;
            g_free(path);
            if (egl_vendor_json_matches(candidate, use_nvidia)) {
                g_dir_close(gdir);
                return candidate;
            }
        }
        g_dir_close(gdir);
    }

    return {};
}

void
log_gpu_process_environment(const char* reason, const VividGpuDevice& gpu)
{
    g_message("VividWebProducer: %s gpu=%s vendor=%s render-node=%s pci=%s "
              "env %s=%s %s=%s %s=%s",
              reason,
              gpu.name[0] ? gpu.name : "(unknown)",
              vivid_gpu_vendor_name(gpu.vendor_id),
              gpu.render_node[0] ? gpu.render_node : "(unknown)",
              gpu.pci_address[0] ? gpu.pci_address : "(unknown)",
              ENV_DRI_PRIME,
              env_value_for_log(ENV_DRI_PRIME),
              ENV_NV_PRIME_RENDER_OFFLOAD,
              env_value_for_log(ENV_NV_PRIME_RENDER_OFFLOAD),
              ENV_EGL_VENDOR_LIBRARY_FILENAMES,
              env_value_for_log(ENV_EGL_VENDOR_LIBRARY_FILENAMES));
}

void
log_gpu_process_policy(const char* reason,
                       const CefGpuProcessPolicy& policy,
                       const char* chromium_process_type)
{
    g_message("VividWebProducer: %s serial=%" G_GUINT64_FORMAT
              " child-type=%s gpu=%s vendor=%s render-node=%s pci=%s "
              "policy %s=%s %s=%s %s=%s %s=%s %s=%s",
              reason,
              policy.serial,
              chromium_process_type && *chromium_process_type
                  ? chromium_process_type
                  : "(browser)",
              policy.gpu.name[0] ? policy.gpu.name : "(unknown)",
              vivid_gpu_vendor_name(policy.gpu.vendor_id),
              policy.gpu.render_node[0] ? policy.gpu.render_node : "(unknown)",
              policy.gpu.pci_address[0] ? policy.gpu.pci_address : "(unknown)",
              ENV_DRI_PRIME,
              policy.dri_prime.empty() ? "(unset)" : policy.dri_prime.c_str(),
              ENV_NV_PRIME_RENDER_OFFLOAD,
              policy.use_nvidia_prime ? "1" : "(unset)",
              ENV_EGL_VENDOR_LIBRARY_FILENAMES,
              policy.egl_vendor_library_filename.empty()
                  ? "(unset)"
                  : policy.egl_vendor_library_filename.c_str(),
              ENV_GBM_SHIM_FORCE_RENDER_NODE,
              policy.use_gbm_shim_rewrite ? policy.gpu.render_node : "(unset)",
              ENV_GBM_SHIM_REWRITE,
              policy.use_gbm_shim_rewrite ? "1" : "(unset)");
}

void
append_child_gpu_policy_switches(CefRefPtr<CefCommandLine> command_line,
                                 const CefGpuProcessPolicy& policy)
{
    if (!command_line)
        return;

    /*
     * CEF reuses and extends command lines across helper restarts. After a
     * runtime GPU switch the incoming child command line can still contain the
     * previous Chromium --render-node-override before Vivid appends the new
     * policy. The helper's CefCommandLine view reports only one value, but the
     * real argv still contains both switches and Chromium may honor the first
     * one. Always clear the policy surface before writing the current device so
     * the exec argv has a single authoritative render node.
     */
    command_line->RemoveSwitch(CEF_SWITCH_RENDER_NODE_OVERRIDE);
    command_line->RemoveSwitch(CEF_SWITCH_VIVID_RENDER_NODE);
    command_line->RemoveSwitch(CEF_SWITCH_VIVID_DRI_PRIME);
    command_line->RemoveSwitch(CEF_SWITCH_VIVID_EGL_VENDOR);

    if (policy.gpu.render_node[0]) {
        /*
         * Chromium's own Linux GPU selection code recognizes this switch and
         * logs "Forcibly using value of --render-node-override" when it wins.
         * Keep it alongside the PRIME/GLVND environment so helper selection is
         * explicit even when Chromium ignores or caches part of the inherited
         * environment across a runtime GPU-process restart.
         */
        command_line->AppendSwitchWithValue(CEF_SWITCH_RENDER_NODE_OVERRIDE,
                                            policy.gpu.render_node);
    }
    /*
     * These switches are intentionally Vivid-private diagnostics. Chromium
     * ignores them, while vivid-web-helper logs the inherited environment.
     * Keeping the requested render node on the child command line makes it
     * obvious in logs whether a helper was launched for the current GPU policy
     * or is a leftover process from the previous browser generation.
     */
    if (policy.gpu.render_node[0])
        command_line->AppendSwitchWithValue(CEF_SWITCH_VIVID_RENDER_NODE,
                                            policy.gpu.render_node);
    if (!policy.dri_prime.empty())
        command_line->AppendSwitchWithValue(CEF_SWITCH_VIVID_DRI_PRIME,
                                            policy.dri_prime);
    if (!policy.egl_vendor_library_filename.empty())
        command_line->AppendSwitchWithValue(CEF_SWITCH_VIVID_EGL_VENDOR,
                                            policy.egl_vendor_library_filename);
}

void
apply_gpu_process_policy_for_child_launch(CefRefPtr<CefCommandLine> command_line)
{
    g_mutex_lock(&g_gpu_policy_lock);
    const CefGpuProcessPolicy policy = g_gpu_policy;
    if (policy.valid)
        apply_gpu_process_policy_to_environment_unlocked(policy);
    g_mutex_unlock(&g_gpu_policy_lock);

    if (!policy.valid) {
        g_warning("VividWebProducer: launching CEF child without a resolved "
                  "GPU policy; helper environment was not changed");
        return;
    }

    append_child_gpu_policy_switches(command_line, policy);
    const std::string process_type =
        command_line && command_line->HasSwitch(CEF_SWITCH_PROCESS_TYPE)
            ? command_line->GetSwitchValue(CEF_SWITCH_PROCESS_TYPE).ToString()
            : std::string();
    log_gpu_process_policy("applied CEF child GPU policy",
                           policy,
                           process_type.c_str());
}

/* ---------------------------------------------------------------- CEF glue */

enum class CefGlobalState
{
    NotInitialized,
    Initialized,
    Failed,
    ShutDown,
};

GMutex g_cef_lock;
CefGlobalState g_cef_state = CefGlobalState::NotInitialized;

void
warn_if_web_force_software_requested(void)
{
    const gchar* env = g_getenv("VIVID_WEB_FORCE_SOFTWARE");
    const bool requested = env && *env && g_strcmp0(env, "0") != 0;
    static std::atomic_bool warned { false };
    if (requested && !warned.exchange(true)) {
        g_warning("VividWebProducer: ignoring VIVID_WEB_FORCE_SOFTWARE=%s; "
                  "web output requires CEF DMA-BUF shared textures and will "
                  "not use CPU-copy fallback",
                  env);
    }
}

const char*
cef_task_type_name(cef_task_type_t type)
{
    switch (type) {
    case CEF_TASK_TYPE_BROWSER: return "browser";
    case CEF_TASK_TYPE_GPU: return "gpu";
    case CEF_TASK_TYPE_ZYGOTE: return "zygote";
    case CEF_TASK_TYPE_UTILITY: return "utility";
    case CEF_TASK_TYPE_RENDERER: return "renderer";
    case CEF_TASK_TYPE_EXTENSION: return "extension";
    case CEF_TASK_TYPE_GUEST: return "guest";
#if CEF_API_ADDED(14000)
    case CEF_TASK_TYPE_PLUGIN_DEPRECATED: return "plugin";
#else
    case CEF_TASK_TYPE_PLUGIN: return "plugin";
#endif
    case CEF_TASK_TYPE_SANDBOX_HELPER: return "sandbox-helper";
    case CEF_TASK_TYPE_DEDICATED_WORKER: return "dedicated-worker";
    case CEF_TASK_TYPE_SHARED_WORKER: return "shared-worker";
    case CEF_TASK_TYPE_SERVICE_WORKER: return "service-worker";
    case CEF_TASK_TYPE_UNKNOWN:
    default:
        return "unknown";
    }
}

std::string
resolve_cef_dir(void)
{
    const gchar* env = g_getenv("VIVID_WEB_CEF_DIR");
    if (env && *env)
        return env;

    /*
     * libVividWeb.so is laid out next to libcef.so, the helper executable
     * and the CEF resources (the build copies them into one directory), so
     * the module's own location is the natural default.
     */
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&resolve_cef_dir), &info) && info.dli_fname) {
        gchar* dir = g_path_get_dirname(info.dli_fname);
        std::string result = dir ? dir : ".";
        g_free(dir);
        return result;
    }
    return ".";
}

std::string
cef_switch_value_for_log(CefRefPtr<CefCommandLine> command_line, const char* name)
{
    if (!command_line || !command_line->HasSwitch(name))
        return "(unset)";
    const std::string value = command_line->GetSwitchValue(name).ToString();
    return value.empty() ? "(empty)" : value;
}

void
force_cef_switch_with_value(CefRefPtr<CefCommandLine> command_line,
                            const char*               name,
                            const char*               value)
{
    command_line->RemoveSwitch(name);
    command_line->AppendSwitchWithValue(name, value);
}

void
force_cef_switch(CefRefPtr<CefCommandLine> command_line, const char* name)
{
    command_line->RemoveSwitch(name);
    command_line->AppendSwitch(name);
}

void
log_cef_graphics_command_line(const char*               reason,
                              const char*               chromium_process_type,
                              CefRefPtr<CefCommandLine> command_line)
{
    g_message("VividWebProducer: %s child-type=%s use-angle=%s "
              "ozone-platform=%s use-vulkan=%s disable-vulkan-surface=%s "
              "enable-features=%s disable-features=%s",
              reason,
              chromium_process_type && *chromium_process_type
                  ? chromium_process_type
                  : "(browser)",
              cef_switch_value_for_log(command_line, "use-angle").c_str(),
              cef_switch_value_for_log(command_line, "ozone-platform").c_str(),
              cef_switch_value_for_log(command_line, "use-vulkan").c_str(),
              command_line && command_line->HasSwitch("disable-vulkan-surface")
                  ? "true"
                  : "false",
              cef_switch_value_for_log(command_line, "enable-features").c_str(),
              cef_switch_value_for_log(command_line, "disable-features").c_str());
}

void
apply_cef_shared_texture_graphics_policy(CefRefPtr<CefCommandLine> command_line,
                                         const char*               reason,
                                         const char*               chromium_process_type,
                                         bool                      browser_process)
{
    if (!command_line)
        return;

    warn_if_web_force_software_requested();

    /*
     * Keep Chromium's page compositor on the Linux shared-texture path that CEF
     * expects for OSR: ANGLE/EGL plus Wayland ozone. Chromium's own Vulkan
     * compositor/shared-image path is a different producer of GBM images and is
     * the path that fails NVIDIA Wayland SkSurface creation, while Vivid's
     * later Vulkan DMA-BUF import/blit remains enabled.
     */
    force_cef_switch_with_value(command_line, "use-angle", "gl-egl");
    force_cef_switch_with_value(command_line, "ozone-platform", "wayland");

    const CefGpuProcessPolicy policy = gpu_process_policy_snapshot();
    const bool disable_chromium_vulkan =
        !browser_process &&
        policy.valid &&
        policy.gpu.vendor_id == VIVID_GPU_VENDOR_ID_NVIDIA;

    if (disable_chromium_vulkan) {
        force_cef_switch_with_value(command_line, "use-vulkan", "disabled");
        force_cef_switch(command_line, "disable-vulkan-surface");
    } else {
        /*
         * The Vulkan compositor/shared-image workaround is NVIDIA-specific.
         * The browser process is long-lived across runtime GPU switches, and
         * Intel/Mesa children need Chromium's adapter discovery left available.
         * Remove only the Vulkan switches/features that Vivid
         * owns here; the child GPU environment still selects the render node.
         */
        command_line->RemoveSwitch("use-vulkan");
        command_line->RemoveSwitch("disable-vulkan-surface");
    }

    bool removed_vulkan = false;
    if (disable_chromium_vulkan) {
        const std::string enabled_features =
            cef_feature_list_without_chromium_vulkan(
                command_line->GetSwitchValue("enable-features").ToString().c_str(),
                removed_vulkan);
        if (enabled_features.empty())
            command_line->RemoveSwitch("enable-features");
        else
            force_cef_switch_with_value(command_line,
                                        "enable-features",
                                        enabled_features.c_str());

        std::string disabled_features =
            cef_feature_list_with_chromium_vulkan_disabled(
                command_line->GetSwitchValue("disable-features").ToString().c_str());
        disabled_features =
            cef_feature_list_with_feature(disabled_features.c_str(),
                                          CEF_FEATURE_SPARE_RENDERER_FOR_SITE_PER_PROCESS);
        force_cef_switch_with_value(command_line,
                                    "disable-features",
                                    disabled_features.c_str());
    } else {
        std::string disabled_features =
            cef_feature_list_without_chromium_vulkan(
                command_line->GetSwitchValue("disable-features").ToString().c_str(),
                removed_vulkan);
        disabled_features =
            cef_feature_list_with_feature(disabled_features.c_str(),
                                          CEF_FEATURE_SPARE_RENDERER_FOR_SITE_PER_PROCESS);
        if (disabled_features.empty())
            command_line->RemoveSwitch("disable-features");
        else
            force_cef_switch_with_value(command_line,
                                        "disable-features",
                                        disabled_features.c_str());
    }

    if (removed_vulkan) {
        g_warning("VividWebProducer: removed Chromium Vulkan features from %s "
                  "for %s",
                  disable_chromium_vulkan ? "enable-features" : "disable-features",
                  disable_chromium_vulkan
                      ? "NVIDIA/Wayland shared textures"
                      : "non-NVIDIA child graphics policy");
    }
    log_cef_graphics_command_line(reason, chromium_process_type, command_line);
}

class VividWebApp : public CefApp, public CefBrowserProcessHandler
{
public:
    VividWebApp() = default;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    /*
     * Chromium's process singleton forwards a second launch using the same
     * root_cache_path to this (first) process; the Chrome-bootstrap default
     * would open a visible browser window on the desktop. A wallpaper
     * producer must never grow windows, so the relaunch is swallowed.
     */
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                     const CefString& current_directory) override
    {
        g_warning("VividWebProducer: another process tried to start CEF with the same "
                  "cache directory; ignoring the relaunch request (no window opened)");
        return true;
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override
    {
        command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        command_line->AppendSwitch("enable-media-stream");
        command_line->AppendSwitch("allow-file-access-from-files");
        command_line->AppendSwitchWithValue("enable-logging", "stderr");
        /* https://github.com/GoogleChrome/puppeteer/issues/1834 */
        command_line->AppendSwitch("disable-dev-shm-usage");
        /*
         * GPU switching is a runtime operation in Vivid. Chromium's Linux
         * zygote is intentionally long-lived and would keep the environment it
         * inherited from the first web browser generation, so direct child
         * launches are the simpler invariant here: every renderer/GPU helper is
         * born from the browser process after OnBeforeChildProcessLaunch applies
         * the current resolved render-node policy.
         */
        if (!command_line->HasSwitch(CEF_SWITCH_NO_ZYGOTE))
            command_line->AppendSwitch(CEF_SWITCH_NO_ZYGOTE);
        /*
         * A runtime render-device switch deliberately terminates Chromium's GPU
         * task so the replacement child starts with the new PRIME/GLVND/render
         * node policy. Chromium otherwise counts those controlled exits as GPU
         * crashes and can disable GPU-backed page APIs after several switches,
         * which makes wallpapers report that WebGL/WebGPU is unavailable.
         */
        if (!command_line->HasSwitch(CEF_SWITCH_DISABLE_GPU_PROCESS_CRASH_LIMIT))
            command_line->AppendSwitch(CEF_SWITCH_DISABLE_GPU_PROCESS_CRASH_LIMIT);
        /*
         * The GPU reset also belongs to the wallpaper's file:// origin from
         * Chromium's point of view. Without this switch repeated runtime GPU
         * changes can trip Chromium's per-domain 3D API protection and leave the
         * next page generation with WebGL/WebGPU blocked even though the new GPU
         * process and Vivid's accelerated OSR path are healthy.
         */
        if (!command_line->HasSwitch(CEF_SWITCH_DISABLE_DOMAIN_BLOCKING_FOR_3D_APIS))
            command_line->AppendSwitch(CEF_SWITCH_DISABLE_DOMAIN_BLOCKING_FOR_3D_APIS);

        const gchar* extra_flags = g_getenv("VIVID_WEB_CEF_EXTRA_FLAGS");
        if (extra_flags && *extra_flags) {
            gchar** flags = g_strsplit(extra_flags, ",", -1);
            for (guint i = 0; flags[i]; i++) {
                gchar** pair = g_strsplit(flags[i], "=", 2);
                if (pair[0] && *pair[0]) {
                    if (pair[1]) {
                        const bool is_enable_features =
                            g_strcmp0(pair[0], "enable-features") == 0;
                        bool removed_chromium_vulkan = false;
                        std::string value = pair[1];

                        /*
                         * Linux OSR shared-texture mode is the ANGLE EGL path
                         * described by upstream CEF. Enabling Chromium's Vulkan
                         * feature here is a different compositor/shared-image
                         * path; on NVIDIA it produced importable DMA-BUFs whose
                         * contents were all zero even after forcing same-node GBM
                         * allocation. Keep this guard strict and logged so a bad
                         * debug flag cannot masquerade as a renderer failure.
                         */
                        if (is_enable_features) {
                            value = cef_feature_list_without_chromium_vulkan(
                                pair[1],
                                removed_chromium_vulkan);
                            if (removed_chromium_vulkan) {
                                g_warning("VividWebProducer: removed Chromium "
                                          "Vulkan features from extra CEF flags "
                                          "because Linux shared textures use "
                                          "ANGLE gl-egl");
                            }
                        }

                        if (!value.empty()) {
                            g_message("VividWebProducer: extra CEF switch %s=%s",
                                      pair[0],
                                      value.c_str());
                            command_line->AppendSwitchWithValue(pair[0], value);
                        } else if (!removed_chromium_vulkan) {
                            g_message("VividWebProducer: extra CEF switch %s=",
                                      pair[0]);
                            command_line->AppendSwitchWithValue(pair[0], "");
                        }
                    } else {
                        g_message("VividWebProducer: extra CEF switch %s", pair[0]);
                        command_line->AppendSwitch(pair[0]);
                    }
                }
                g_strfreev(pair);
            }
            g_strfreev(flags);
        }

        const std::string chromium_process_type = process_type.ToString();
        apply_cef_shared_texture_graphics_policy(command_line,
                                                 "CEF browser command line",
                                                 chromium_process_type.c_str(),
                                                 true);
    }

    void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override
    {
        apply_gpu_process_policy_for_child_launch(command_line);
        const std::string process_type =
            command_line && command_line->HasSwitch(CEF_SWITCH_PROCESS_TYPE)
                ? command_line->GetSwitchValue(CEF_SWITCH_PROCESS_TYPE).ToString()
                : std::string();
        apply_cef_shared_texture_graphics_policy(command_line,
                                                 "CEF child command line",
                                                 process_type.c_str(),
                                                 false);
    }

private:
    IMPLEMENT_REFCOUNTING(VividWebApp);
    DISALLOW_COPY_AND_ASSIGN(VividWebApp);
};

bool
ensure_cef_initialized(void)
{
    g_mutex_lock(&g_cef_lock);
    if (g_cef_state == CefGlobalState::Initialized) {
        g_mutex_unlock(&g_cef_lock);
        return true;
    }
    if (g_cef_state != CefGlobalState::NotInitialized) {
        g_mutex_unlock(&g_cef_lock);
        g_warning("VividWebProducer: CEF is unavailable in this process (state=%d)",
                  static_cast<int>(g_cef_state));
        return false;
    }

    const std::string cef_dir = resolve_cef_dir();
    std::string helper_path;
    if (const gchar* env = g_getenv("VIVID_WEB_HELPER_PATH"); env && *env) {
        helper_path = env;
    } else {
        gchar* joined = g_build_filename(cef_dir.c_str(), "vivid-web-helper", NULL);
        helper_path = joined;
        g_free(joined);
    }

    if (!g_file_test(helper_path.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
        g_mutex_unlock(&g_cef_lock);
        g_warning("VividWebProducer: CEF helper executable not found at %s "
                  "(set VIVID_WEB_CEF_DIR or VIVID_WEB_HELPER_PATH)",
                  helper_path.c_str());
        return false;
    }

    gchar* cache_dir =
        g_build_filename(g_get_user_cache_dir(), "vivid_web_producer", NULL);
    g_mkdir_with_parents(cache_dir, 0700);

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    settings.background_color = CefColorSetARGB(0xff, 0xff, 0xff, 0xff);
    CefString(&settings.browser_subprocess_path).FromString(helper_path);
    CefString(&settings.resources_dir_path).FromString(cef_dir);
    {
        gchar* locales = g_build_filename(cef_dir.c_str(), "locales", NULL);
        CefString(&settings.locales_dir_path).FromASCII(locales);
        g_free(locales);
    }
    CefString(&settings.root_cache_path).FromString(cache_dir);
    CefString(&settings.cache_path).FromString(cache_dir);
    /*
     * CEF documents an empty log_file as "write debug.log next to the main
     * executable". Point it at stderr's device instead so Chromium diagnostics
     * follow the producer's console stream and no runtime log file is created.
     */
    CefString(&settings.log_file).FromString("/dev/stderr");
    g_free(cache_dir);

    /*
     * The producer aligns CEF helper processes with the resolved render device
     * through the GPU policy updated by apply_gpu_process_environment(). That
     * policy is also re-applied from OnBeforeChildProcessLaunch so runtime GPU
     * switches affect the next browser/helper generation without reinitializing
     * process-global CEF.
     */
    warn_if_web_force_software_requested();

    g_message("VividWebProducer: initializing CEF dir=%s helper=%s "
              "shared-textures=required",
              cef_dir.c_str(),
              helper_path.c_str());

    CefMainArgs args(0, nullptr);
    CefRefPtr<VividWebApp> app = new VividWebApp();
    if (!CefInitialize(args, settings, app, nullptr)) {
        g_cef_state = CefGlobalState::Failed;
        g_mutex_unlock(&g_cef_lock);
        g_warning("VividWebProducer: CefInitialize failed; web wallpapers are disabled");
        return false;
    }

    g_cef_state = CefGlobalState::Initialized;
    g_mutex_unlock(&g_cef_lock);
    g_message("VividWebProducer: CEF initialized");
    return true;
}

/*
 * Best-effort hint so Chromium's GPU process picks the same card the ring
 * lives on. CEF itself is process-global, but its helper processes are launched
 * later through OnBeforeChildProcessLaunch; keep this policy current so a web
 * browser rebuild after a render-device change starts children with the new
 * render-node environment.
 */
void
apply_gpu_process_environment(const VividGpuDevice& gpu)
{
    const bool use_nvidia_prime = gpu.vendor_id == VIVID_GPU_VENDOR_ID_NVIDIA;
    const std::string dri_prime = dri_prime_value_for_pci_address(gpu.pci_address);
    const std::string egl_vendor_library_filename =
        egl_vendor_library_filename_for_gpu(use_nvidia_prime);

    CefGpuProcessPolicy policy;
    policy.gpu = gpu;
    policy.dri_prime = dri_prime;
    policy.egl_vendor_library_filename = egl_vendor_library_filename;
    policy.use_nvidia_prime = use_nvidia_prime;
    policy.use_gbm_shim_rewrite = use_nvidia_prime;
    policy.valid = true;

    if (egl_vendor_library_filename.empty()) {
        g_warning("VividWebProducer: no %s EGL vendor JSON found; leaving %s unset",
                  use_nvidia_prime ? "NVIDIA" : "Mesa",
                  ENV_EGL_VENDOR_LIBRARY_FILENAMES);
    }

    /*
     * CEF forks/execs vivid-web-helper for GPU/renderer children. The process
     * environment is still the only PRIME/GLVND contract those children inherit,
     * so overwrite inherited launcher values every time the resolved GPU
     * changes. OnBeforeChildProcessLaunch repeats the same application at the
     * exact child-launch boundary.
     */
    g_mutex_lock(&g_gpu_policy_lock);
    policy.serial = ++g_gpu_policy_serial;
    g_gpu_policy = policy;
    apply_gpu_process_policy_to_environment_unlocked(policy);
    g_mutex_unlock(&g_gpu_policy_lock);

    log_gpu_process_environment("applied CEF GPU process environment", gpu);
}

/* ----------------------------------------------------------- project entry */

std::string
resolve_web_project_url(const gchar* project_path)
{
    if (!project_path || !*project_path)
        return {};

    std::string entry_path;
    if (g_file_test(project_path, G_FILE_TEST_IS_REGULAR)) {
        entry_path = project_path;
    } else if (g_file_test(project_path, G_FILE_TEST_IS_DIR)) {
        std::string entry = "index.html";
        gchar* manifest_path = g_build_filename(project_path, "project.json", NULL);
        if (g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR)) {
            JsonParser* parser = json_parser_new();
            GError* error = NULL;
            if (json_parser_load_from_file(parser, manifest_path, &error)) {
                JsonNode* root = json_parser_get_root(parser);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject* object = json_node_get_object(root);
                    if (json_object_has_member(object, "file") &&
                        !json_object_get_null_member(object, "file")) {
                        const gchar* file = json_object_get_string_member(object, "file");
                        if (file && *file)
                            entry = file;
                    }
                }
            } else {
                g_warning("VividWebProducer: failed to parse %s: %s",
                          manifest_path,
                          error ? error->message : "unknown error");
                g_clear_error(&error);
            }
            g_object_unref(parser);
        }
        g_free(manifest_path);

        gchar* joined = g_build_filename(project_path, entry.c_str(), NULL);
        entry_path = joined;
        g_free(joined);
    }

    if (entry_path.empty() || !g_file_test(entry_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
        g_warning("VividWebProducer: web project entry not found for %s", project_path);
        return {};
    }

    std::string url;
    if (g_path_is_absolute(entry_path.c_str())) {
        gchar* uri = g_filename_to_uri(entry_path.c_str(), NULL, NULL);
        if (uri) {
            url = uri;
            g_free(uri);
        }
    } else {
        gchar* cwd = g_get_current_dir();
        gchar* absolute = g_build_filename(cwd, entry_path.c_str(), NULL);
        gchar* uri = g_filename_to_uri(absolute, NULL, NULL);
        if (uri) {
            url = uri;
            g_free(uri);
        }
        g_free(absolute);
        g_free(cwd);
    }
    return url;
}

/* ------------------------------------------------------------- audio sink */

/*
 * Browser audio is intercepted by CefAudioHandler (which detaches it from the
 * system output) and re-emitted through a local GStreamer chain so volume and
 * mute follow the producer configuration exactly like the video backend.
 */
struct WebAudioPipeline
{
    GMutex lock;
    GstElement* pipeline { nullptr };
    GstElement* appsrc { nullptr };
    GstElement* volume_element { nullptr };
    int channels { 0 };
    int sample_rate { 0 };
    double volume { 1.0 };
    bool muted { false };
    std::vector<float> interleave_scratch;

    WebAudioPipeline() { g_mutex_init(&lock); }
    ~WebAudioPipeline()
    {
        stop();
        g_mutex_clear(&lock);
    }

    static double cubic_volume(double value)
    {
        return std::pow(std::clamp(value, 0.0, 1.0), 3.0);
    }

    void start(int next_channels, int next_rate)
    {
        g_mutex_lock(&lock);
        stop_locked();

        GstElement* next_pipeline = gst_pipeline_new("vivid-web-audio");
        GstElement* src = gst_element_factory_make("appsrc", "vivid-web-audio-src");
        GstElement* convert = gst_element_factory_make("audioconvert", NULL);
        GstElement* resample = gst_element_factory_make("audioresample", NULL);
        GstElement* vol = gst_element_factory_make("volume", NULL);
        GstElement* sink = gst_element_factory_make("autoaudiosink", NULL);
        if (!next_pipeline || !src || !convert || !resample || !vol || !sink) {
            g_warning("VividWebProducer: failed to create browser audio pipeline elements");
            g_clear_object(&next_pipeline);
            if (src) gst_object_unref(src);
            if (convert) gst_object_unref(convert);
            if (resample) gst_object_unref(resample);
            if (vol) gst_object_unref(vol);
            if (sink) gst_object_unref(sink);
            g_mutex_unlock(&lock);
            return;
        }

        GstCaps* caps = gst_caps_new_simple("audio/x-raw",
                                            "format", G_TYPE_STRING, "F32LE",
                                            "layout", G_TYPE_STRING, "interleaved",
                                            "rate", G_TYPE_INT, next_rate,
                                            "channels", G_TYPE_INT, next_channels,
                                            NULL);
        g_object_set(src,
                     "caps", caps,
                     "is-live", TRUE,
                     "do-timestamp", TRUE,
                     "format", GST_FORMAT_TIME,
                     "block", FALSE,
                     "max-bytes", (guint64)(next_channels * next_rate * 4 / 2),
                     NULL);
        gst_caps_unref(caps);

        g_object_set(vol,
                     "volume", cubic_volume(volume),
                     "mute", muted ? TRUE : FALSE,
                     NULL);
        g_object_set(sink, "sync", FALSE, NULL);

        gst_bin_add_many(GST_BIN(next_pipeline), src, convert, resample, vol, sink, NULL);
        if (!gst_element_link_many(src, convert, resample, vol, sink, NULL)) {
            g_warning("VividWebProducer: failed to link browser audio pipeline");
            gst_object_unref(next_pipeline);
            g_mutex_unlock(&lock);
            return;
        }

        pipeline = next_pipeline;
        appsrc = src;
        volume_element = vol;
        channels = next_channels;
        sample_rate = next_rate;
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        g_message("VividWebProducer: browser audio started rate=%d channels=%d",
                  next_rate,
                  next_channels);
        g_mutex_unlock(&lock);
    }

    void push(const float** data, int frames)
    {
        g_mutex_lock(&lock);
        if (!appsrc || channels <= 0 || frames <= 0 || !data) {
            g_mutex_unlock(&lock);
            return;
        }

        const gsize sample_count = (gsize)frames * (gsize)channels;
        interleave_scratch.resize(sample_count);
        for (int channel = 0; channel < channels; channel++) {
            const float* source = data[channel];
            if (!source)
                continue;
            for (int frame = 0; frame < frames; frame++)
                interleave_scratch[(gsize)frame * channels + channel] = source[frame];
        }

        GstBuffer* buffer = gst_buffer_new_allocate(NULL, sample_count * sizeof(float), NULL);
        gst_buffer_fill(buffer, 0, interleave_scratch.data(), sample_count * sizeof(float));
        GST_BUFFER_DURATION(buffer) =
            gst_util_uint64_scale((guint64)frames, GST_SECOND, (guint64)sample_rate);
        const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (flow != GST_FLOW_OK)
            g_debug("VividWebProducer: browser audio push returned %d", (int)flow);
        g_mutex_unlock(&lock);
    }

    void stop()
    {
        g_mutex_lock(&lock);
        stop_locked();
        g_mutex_unlock(&lock);
    }

    void set_volume(double next_volume, bool next_muted)
    {
        g_mutex_lock(&lock);
        volume = std::clamp(next_volume, 0.0, 1.0);
        muted = next_muted;
        if (volume_element) {
            g_object_set(volume_element,
                         "volume", cubic_volume(volume),
                         "mute", muted ? TRUE : FALSE,
                         NULL);
        }
        g_mutex_unlock(&lock);
    }

private:
    void stop_locked()
    {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
            appsrc = nullptr;
            volume_element = nullptr;
        }
        channels = 0;
        sample_rate = 0;
    }
};

/* ------------------------------------------------------ Vulkan export ring */

struct WebRingSlot
{
    VividWebVulkanImage vk_image;
};

struct WebFrameRing
{
    std::array<WebRingSlot, WEB_RING_BUFFERS> slots {};
    guint32 width { 0 };
    guint32 height { 0 };
    VividWebVulkanExportRequest export_request {};
    guint32 presented_index { 0 };
    guint32 ready_index { 1 };
    guint32 in_progress_index { 2 };
    bool dirty { false };
    bool vulkan_ready { false };
    VividWebVulkanRoute vulkan;

    bool valid() const { return slots[0].vk_image && slots[0].vk_image.fd >= 0; }

    void destroy()
    {
        for (auto& slot : slots)
            slot.vk_image.reset();
        vulkan.abandon_for_process_lifetime();
        vulkan_ready = false;
        width = 0;
        height = 0;
        export_request = {};
        presented_index = 0;
        ready_index = 1;
        in_progress_index = 2;
        dirty = false;
    }

    bool create(const VividGpuDevice& gpu,
                guint32 next_width,
                guint32 next_height,
                const VividWebVulkanExportRequest& next_request)
    {
        destroy();

        if (!gpu.render_node[0]) {
            g_warning("VividWebProducer: resolved GPU has no DRM render node");
            return false;
        }

        vulkan_ready = vulkan.ensure(gpu);
        if (!vulkan_ready)
            return false;

        for (guint i = 0; i < WEB_RING_BUFFERS; i++) {
            WebRingSlot& slot = slots[i];
            auto image = vulkan.create_export_image(next_width,
                                                    next_height,
                                                    VK_FORMAT_B8G8R8A8_UNORM,
                                                    next_request);
            if (!image.has_value()) {
                g_warning("VividWebProducer: failed to create Vulkan export ring "
                          "image %ux%u index=%u",
                          next_width,
                          next_height,
                          i);
                destroy();
                return false;
            }
            slot.vk_image = std::move(image.value());
        }

        width = next_width;
        height = next_height;
        export_request = next_request;
        presented_index = 0;
        ready_index = 1;
        in_progress_index = 2;
        dirty = false;

        g_message("VividWebProducer: prepared Vulkan export ring %ux%u fourcc=0x%08x "
                  "buffers=%u node=%s stride=%u modifier=0x%016" G_GINT64_MODIFIER
                  "x memory=%s",
                  width,
                  height,
                  WEB_RING_FOURCC,
                  WEB_RING_BUFFERS,
                  gpu.render_node,
                  slots[0].vk_image.stride,
                  static_cast<guint64>(slots[0].vk_image.modifier),
                  next_request.memory == VividWebVulkanExportMemory::DeviceLocal
                      ? "device-local"
                      : "host-visible");
        return true;
    }

    void mark_frame_ready()
    {
        std::swap(in_progress_index, ready_index);
        dirty = true;
    }

    bool eat_frame(guint32* out_index)
    {
        if (!dirty)
            return false;
        std::swap(presented_index, ready_index);
        dirty = false;
        *out_index = presented_index;
        return true;
    }
};

} // namespace

/* ------------------------------------------------------------- CEF client */

struct _VividWebProducer;

namespace
{

class VividWebClient : public CefClient,
                        public CefLifeSpanHandler,
                        public CefRenderHandler,
                        public CefAudioHandler,
                        public CefLoadHandler,
                        public CefDisplayHandler,
                        public CefRequestHandler
{
public:
    explicit VividWebClient(_VividWebProducer* producer)
        : producer_(producer)
    {
        g_mutex_init(&producer_lock_);
        g_mutex_init(&state_lock_);
        g_cond_init(&state_cond_);
    }

    ~VividWebClient() override
    {
        g_mutex_clear(&producer_lock_);
        g_mutex_clear(&state_lock_);
        g_cond_clear(&state_cond_);
    }

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    /* ---- viewport facts mirrored from the producer (any thread) ---- */
    void set_viewport(int width_px, int height_px, double scale, bool shared_textures)
    {
        g_mutex_lock(&state_lock_);
        viewport_width_px_ = width_px;
        viewport_height_px_ = height_px;
        viewport_scale_ = scale > 0.0 ? scale : 1.0;
        shared_textures_ = shared_textures;
        g_mutex_unlock(&state_lock_);
    }

    /* ---- lifecycle (called from the producer thread) ---- */
    void detach_producer()
    {
        g_mutex_lock(&producer_lock_);
        producer_ = nullptr;
        g_mutex_unlock(&producer_lock_);
    }

    bool wait_browser_created()
    {
        g_mutex_lock(&state_lock_);
        const gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
        while (!browser_created_ && !browser_failed_) {
            if (!g_cond_wait_until(&state_cond_, &state_lock_, deadline))
                break;
        }
        const bool created = browser_created_;
        g_mutex_unlock(&state_lock_);
        return created;
    }

    bool has_browser_created()
    {
        g_mutex_lock(&state_lock_);
        const bool created = browser_created_ && browser_ != nullptr && !browser_closed_;
        g_mutex_unlock(&state_lock_);
        return created;
    }

    void wait_browser_closed()
    {
        g_mutex_lock(&state_lock_);
        const gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        while (browser_created_ && !browser_closed_) {
            if (!g_cond_wait_until(&state_cond_, &state_lock_, deadline)) {
                g_warning("VividWebProducer: timed out waiting for the CEF browser to close");
                break;
            }
        }
        g_mutex_unlock(&state_lock_);
    }

    bool kill_gpu_process_for_device_switch(const char* reason)
    {
        g_mutex_lock(&state_lock_);
        const guint64 serial = ++gpu_kill_request_serial_;
        g_mutex_unlock(&state_lock_);

        CefRefPtr<VividWebClient> self_ref = this;
        CefPostTask(TID_UI,
                    base::BindOnce(&VividWebClient::DoKillGpuProcess,
                                   self_ref,
                                   std::string(reason ? reason : "unknown"),
                                   serial));

        g_mutex_lock(&state_lock_);
        const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (gpu_kill_completed_serial_ < serial) {
            if (!g_cond_wait_until(&state_cond_, &state_lock_, deadline)) {
                g_warning("VividWebProducer: timed out waiting for CEF GPU "
                          "process kill request serial=%" G_GUINT64_FORMAT,
                          serial);
                g_mutex_unlock(&state_lock_);
                return false;
            }
        }
        const bool ok = gpu_kill_last_result_;
        g_mutex_unlock(&state_lock_);
        return ok;
    }

    /* ---- UI-thread operations (invoked via CefPostTask) ---- */
    void DoKillGpuProcess(std::string reason, guint64 serial)
    {
        CEF_REQUIRE_UI_THREAD();

        bool killed_gpu_task = false;
        bool saw_gpu_task = false;

        CefRefPtr<CefTaskManager> manager = CefTaskManager::GetTaskManager();
        if (!manager) {
            g_warning("VividWebProducer: CEF task manager is unavailable while "
                      "killing GPU process for %s",
                      reason.c_str());
            complete_gpu_kill_request(serial, false);
            return;
        }

        CefTaskManager::TaskIdList task_ids;
        if (!manager->GetTaskIdsList(task_ids)) {
            g_warning("VividWebProducer: failed to list CEF tasks while killing "
                      "GPU process for %s",
                      reason.c_str());
            complete_gpu_kill_request(serial, false);
            return;
        }

        /*
         * Chromium keeps the GPU process global to the CEF runtime, so closing
         * the wallpaper browser is not enough for a runtime GPU switch. Kill the
         * GPU tasks visible in this single TaskManager snapshot after updating
         * Vivid's child-launch policy; Chromium will create the replacement GPU
         * helper with the new render-node environment the next time compositing
         * needs it.
         *
         * Do not poll TaskManager and kill every GPU task that appears after
         * this point. TaskManager exposes only the generic CEF_TASK_TYPE_GPU
         * process type, not the render-node policy used by that process. During
         * a valid switch Chromium can respawn the new GPU helper immediately,
         * so "a GPU task still exists" is not evidence that the old task
         * survived. Re-killing those replacement tasks makes Chromium mark the
         * GPU process unusable and abort the browser process.
         */
        for (const int64_t task_id : task_ids) {
            CefTaskInfo info;
            if (!manager->GetTaskInfo(task_id, info)) {
                g_warning("VividWebProducer: failed to read CEF task info id=%" G_GINT64_FORMAT,
                          (gint64)task_id);
                continue;
            }

            const std::string title = CefString(&info.title).ToString();
            g_message("VividWebProducer: CEF task id=%" G_GINT64_FORMAT
                      " type=%s killable=%s title=%s",
                      (gint64)task_id,
                      cef_task_type_name(info.type),
                      bool_to_string(info.is_killable != 0),
                      title.empty() ? "(empty)" : title.c_str());

            if (info.type != CEF_TASK_TYPE_GPU)
                continue;

            saw_gpu_task = true;
            if (!info.is_killable) {
                g_warning("VividWebProducer: CEF GPU task id=%" G_GINT64_FORMAT
                          " is not killable for %s",
                          (gint64)task_id,
                          reason.c_str());
                continue;
            }

            const bool ok = manager->KillTask(task_id);
            killed_gpu_task = killed_gpu_task || ok;
            g_message("VividWebProducer: requested CEF GPU task kill id=%"
                      G_GINT64_FORMAT " reason=%s ok=%s",
                      (gint64)task_id,
                      reason.c_str(),
                      bool_to_string(ok));
        }

        if (!saw_gpu_task) {
            g_message("VividWebProducer: no live CEF GPU task found while "
                      "switching GPU for %s",
                      reason.c_str());
            complete_gpu_kill_request(serial, true);
            return;
        }

        complete_gpu_kill_request(serial, killed_gpu_task);
    }

    void DoCreateBrowser(std::string url, int fps, bool shared_textures)
    {
        CEF_REQUIRE_UI_THREAD();
        g_mutex_lock(&state_lock_);
        shared_textures_ = shared_textures;
        g_mutex_unlock(&state_lock_);
        const WebViewport viewport = viewport_snapshot();

        CefWindowInfo window_info;
        window_info.SetAsWindowless(0);
        window_info.shared_texture_enabled = shared_textures ? 1 : 0;

        CefBrowserSettings browser_settings;
        browser_settings.windowless_frame_rate = std::clamp(fps, 1, 240);
        browser_settings.background_color = CefColorSetARGB(0xff, 0xff, 0xff, 0xff);

        /*
         * Create a blank OSR browser first so the viewport mode and zoom level are
         * available before any real wallpaper navigation starts. Page zoom is
         * URL-scoped in Chromium, so it is applied again from OnLoadStart after
         * the file:// navigation commits but before the frame begins loading
         * contents. Passing the project URL directly to CreateBrowserSync lets
         * early page code run at the raw physical view size and only later receive
         * the page zoom, which makes high-DPI wallpapers initialize at the wrong
         * scale for one or more frames.
         */
        CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(window_info,
                                                                          this,
                                                                          "about:blank",
                                                                          browser_settings,
                                                                          nullptr,
                                                                          nullptr);
        g_mutex_lock(&state_lock_);
        browser_ = browser;
        browser_created_ = browser != nullptr;
        browser_failed_ = browser == nullptr;
        g_cond_broadcast(&state_cond_);
        g_mutex_unlock(&state_lock_);

        if (browser) {
            /*
             * The audio handler owns browser sound from here on; mute the
             * direct OS output so nothing is heard twice.
             */
            browser->GetHost()->SetAudioMuted(true);
            ApplyViewportToBrowser(browser, true);
            if (browser->GetMainFrame())
                browser->GetMainFrame()->LoadURL(url);
            g_message("VividWebProducer: CEF browser created url=%s fps=%d "
                      "shared-textures=%s initial-viewport=%dx%d dip=%dx%d "
                      "view=%dx%d scale=%.3f configured=%s",
                      url.c_str(),
                      fps,
                      bool_to_string(shared_textures),
                      viewport.physical_width,
                      viewport.physical_height,
                      viewport.dip_width,
                      viewport.dip_height,
                      viewport.view_width,
                      viewport.view_height,
                      viewport.scale,
                      bool_to_string(viewport.configured));
        } else {
            g_warning("VividWebProducer: CreateBrowserSync failed for %s", url.c_str());
        }
    }

    void DoLoadUrl(std::string url)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (browser && browser->GetMainFrame()) {
            ApplyViewportToBrowser(browser, false);
            browser->GetMainFrame()->LoadURL(url);
        }
    }

    void DoSetFrameRate(int fps)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (browser)
            browser->GetHost()->SetWindowlessFrameRate(std::clamp(fps, 1, 240));
    }

    void DoNotifyViewportChanged()
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        ApplyViewportToBrowser(browser, true);
    }

    void DoExecuteJs(std::string script)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        CefRefPtr<CefFrame> frame = browser->GetMainFrame();
        if (frame)
            frame->ExecuteJavaScript(script, frame->GetURL(), 0);
    }

    /*
     * Ported from gstcefsrc gst_cef_src_apply_browser_suspended_now(): pause
     * page media and Vivid-tracked AudioContexts, freeze the web lifecycle,
     * and hide the browser; resume reverses everything and forces a repaint.
     */
    void DoSetSuspended(bool suspended)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        one_shot_restore_suspended_ = false;

        CefRefPtr<CefFrame> frame = browser->GetMainFrame();
        if (frame) {
            const std::string script = suspended
                ? R"JS(
                    (function() {
                      var mediaElements = document.querySelectorAll('audio, video');
                      for (var i = 0; i < mediaElements.length; i++) {
                        if (mediaElements[i].pause) mediaElements[i].pause();
                      }
                      var contexts = window.__vividAudioContexts || [];
                      for (var j = 0; j < contexts.length; j++) {
                        if (contexts[j].suspend) contexts[j].suspend().catch(function() {});
                      }
                      try {
                        window.dispatchEvent(new CustomEvent('vivid-playback-change', {
                          detail: {playing: false},
                        }));
                      } catch (e) {}
                      if (window.__vividSetPaused) window.__vividSetPaused(true);
                    })();
                  )JS"
                : R"JS(
                    (function() {
                      var mediaElements = document.querySelectorAll('audio, video');
                      for (var i = 0; i < mediaElements.length; i++) {
                        if (mediaElements[i].play) {
                          var p = mediaElements[i].play();
                          if (p && p.catch) p.catch(function() {});
                        }
                      }
                      var contexts = window.__vividAudioContexts || [];
                      for (var j = 0; j < contexts.length; j++) {
                        if (contexts[j].resume) contexts[j].resume().catch(function() {});
                      }
                      try {
                        window.dispatchEvent(new CustomEvent('vivid-playback-change', {
                          detail: {playing: true},
                        }));
                      } catch (e) {}
                      if (window.__vividSetPaused) window.__vividSetPaused(false);
                    })();
                  )JS";
            frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        }

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (!host)
            return;
        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("state", suspended ? "frozen" : "active");
        host->ExecuteDevToolsMethod(0, "Page.setWebLifecycleState", params);
        host->WasHidden(suspended);
        if (!suspended)
            ApplyViewportToBrowser(browser, true);
    }

    void DoRequestFrame(bool restore_suspended, std::string reason)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (!host)
            return;

        /*
         * A suspended CEF browser is both hidden and lifecycle-frozen, so it can
         * refuse to paint even after the producer has a new DMA-BUF export ring.
         * For first-frame handoff we temporarily thaw only the browser host and
         * request a repaint; page media stays paused because we deliberately do
         * not run the resume branch from DoSetSuspended(false). The next paint
         * callback restores the hidden/frozen state.
         */
        if (restore_suspended) {
            one_shot_restore_suspended_ = true;
            CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
            params->SetString("state", "active");
            host->ExecuteDevToolsMethod(0, "Page.setWebLifecycleState", params);
            host->WasHidden(false);
        } else {
            one_shot_restore_suspended_ = false;
        }

        ApplyViewportToBrowser(browser, true);
        host->Invalidate(PET_VIEW);
        g_message("VividWebProducer: request one DMA-BUF frame suspended=%s reason=%s",
                  restore_suspended ? "true" : "false",
                  reason.empty() ? "(none)" : reason.c_str());
    }

    void DoMouseMove(int x_view, int y_view, guint32 button_mask)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        CefMouseEvent event;
        event.x = x_view;
        event.y = y_view;
        event.modifiers = mouse_modifiers(button_mask);
        browser->GetHost()->SendMouseMoveEvent(event, false);
    }

    void DoMouseButton(int x_view, int y_view, guint32 button, bool pressed, guint32 button_mask)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        CefMouseEvent event;
        event.x = x_view;
        event.y = y_view;
        event.modifiers = mouse_modifiers(button_mask);
        browser->GetHost()->SetFocus(true);
        browser->GetHost()->SendMouseClickEvent(event,
                                                cef_button_type(button),
                                                !pressed,
                                                1);
    }

    void DoMouseWheel(int x_view, int y_view, int delta_x, int delta_y, guint32 button_mask)
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (!browser)
            return;
        CefMouseEvent event;
        event.x = x_view;
        event.y = y_view;
        event.modifiers = mouse_modifiers(button_mask);
        browser->GetHost()->SendMouseWheelEvent(event, delta_x, delta_y);
    }

    void DoClose()
    {
        CEF_REQUIRE_UI_THREAD();
        CefRefPtr<CefBrowser> browser = current_browser();
        if (browser)
            browser->GetHost()->CloseBrowser(true);
    }

    /* ---- CefLifeSpanHandler ---- */
    /* Wallpapers never open windows; cancel every popup/new-window request. */
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int popup_id,
                       const CefString& target_url,
                       const CefString& target_frame_name,
                       cef_window_open_disposition_t target_disposition,
                       bool user_gesture,
                       const CefPopupFeatures& popup_features,
                       CefWindowInfo& window_info,
                       CefRefPtr<CefClient>& client,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extra_info,
                       bool* no_javascript_access) override
    {
        g_message("VividWebProducer: blocked popup to %s",
                  target_url.ToString().c_str());
        return true;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
    {
        CEF_REQUIRE_UI_THREAD();
        g_mutex_lock(&state_lock_);
        browser_ = nullptr;
        browser_closed_ = true;
        g_cond_broadcast(&state_cond_);
        g_mutex_unlock(&state_lock_);
    }

    /* ---- CefRenderHandler ---- */
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType type,
                 const RectList& dirty_rects,
                 const void* buffer,
                 int width,
                 int height) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                            PaintElementType type,
                            const RectList& dirty_rects,
                            const CefAcceleratedPaintInfo& info) override;

    /* ---- CefAudioHandler ---- */
    void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                              const CefAudioParameters& params,
                              int channels) override;
    void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser,
                             const float** data,
                             int frames,
                             int64_t pts) override;
    void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override;
    void OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString& message) override
    {
        g_warning("VividWebProducer: browser audio stream error: %s",
                  message.ToString().c_str());
    }

    /* ---- CefLoadHandler ---- */
    void OnLoadStart(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     TransitionType transition_type) override;
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                              bool is_loading,
                              bool can_go_back,
                              bool can_go_forward) override;

    /* ---- CefDisplayHandler ---- */
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override
    {
        const std::string text = message.ToString();
        const std::string origin = source.ToString();
        if (level >= LOGSEVERITY_ERROR)
            g_warning("VividWebProducer(js): %s:%d %s", origin.c_str(), line, text.c_str());
        else
            g_message("VividWebProducer(js): %s:%d %s", origin.c_str(), line, text.c_str());
        return false;
    }

    /* ---- CefRequestHandler ---- */
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                   TerminationStatus status,
                                   int error_code,
                                   const CefString& error_string) override
    {
        CEF_REQUIRE_UI_THREAD();
        g_warning("VividWebProducer: browser render process terminated status=%d "
                  "error=%d (%s); reloading",
                  (int)status,
                  error_code,
                  error_string.ToString().c_str());
        browser->Reload();
    }

private:
    void RestoreOneShotSuspendedIfNeeded(CefRefPtr<CefBrowser> browser)
    {
        CEF_REQUIRE_UI_THREAD();
        if (!one_shot_restore_suspended_)
            return;
        one_shot_restore_suspended_ = false;

        if (!browser)
            return;
        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (!host)
            return;

        CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
        params->SetString("state", "frozen");
        host->ExecuteDevToolsMethod(0, "Page.setWebLifecycleState", params);
        host->WasHidden(true);
        g_message("VividWebProducer: restored suspended browser after one-shot paint");
    }

    void complete_gpu_kill_request(guint64 serial, bool ok)
    {
        g_mutex_lock(&state_lock_);
        if (serial > gpu_kill_completed_serial_) {
            gpu_kill_completed_serial_ = serial;
            gpu_kill_last_result_ = ok;
        }
        g_cond_broadcast(&state_cond_);
        g_mutex_unlock(&state_lock_);
    }

    static guint32 button_mask_bit(guint32 button)
    {
        return button >= 1 && button <= 3 ? (1u << (button - 1)) : 0u;
    }

    static int mouse_modifiers(guint32 button_mask)
    {
        int modifiers = 0;
        if (button_mask & button_mask_bit(1))
            modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
        if (button_mask & button_mask_bit(2))
            modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
        if (button_mask & button_mask_bit(3))
            modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
        return modifiers;
    }

    static cef_mouse_button_type_t cef_button_type(guint32 button)
    {
        switch (button) {
        case 2: return MBT_MIDDLE;
        case 3: return MBT_RIGHT;
        case 1:
        default: return MBT_LEFT;
        }
    }

    CefRefPtr<CefBrowser> current_browser()
    {
        g_mutex_lock(&state_lock_);
        CefRefPtr<CefBrowser> browser = browser_;
        g_mutex_unlock(&state_lock_);
        return browser;
    }

    WebViewport viewport_snapshot()
    {
        g_mutex_lock(&state_lock_);
        const WebViewport viewport =
            resolve_viewport(viewport_width_px_,
                             viewport_height_px_,
                             viewport_scale_,
                             shared_textures_);
        g_mutex_unlock(&state_lock_);
        return viewport;
    }

    void ApplyViewportToBrowser(CefRefPtr<CefBrowser> browser, bool notify_resize)
    {
        if (!browser)
            return;
        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (!host)
            return;

        const WebViewport viewport = viewport_snapshot();
        host->SetZoomLevel(viewport.zoom_level);
        if (notify_resize) {
            host->NotifyScreenInfoChanged();
            host->WasResized();
            host->Invalidate(PET_VIEW);
        }
    }

    /*
     * producer_lock_ guards every callback's access to producer_, so
     * detach_producer() can guarantee no callback still dereferences the
     * producer after vivid_web_producer_free() proceeds to delete it.
     */
    template<typename Functor>
    void with_producer(Functor&& functor)
    {
        g_mutex_lock(&producer_lock_);
        if (producer_)
            functor(producer_);
        g_mutex_unlock(&producer_lock_);
    }

    GMutex producer_lock_;
    _VividWebProducer* producer_ { nullptr };

    GMutex state_lock_;
    GCond state_cond_;
    CefRefPtr<CefBrowser> browser_;
    bool browser_created_ { false };
    bool browser_failed_ { false };
    bool browser_closed_ { false };
    int viewport_width_px_ { 0 };
    int viewport_height_px_ { 0 };
    double viewport_scale_ { 1.0 };
    bool shared_textures_ { false };
    guint64 gpu_kill_request_serial_ { 0 };
    guint64 gpu_kill_completed_serial_ { 0 };
    bool gpu_kill_last_result_ { true };
    bool one_shot_restore_suspended_ { false };

    IMPLEMENT_REFCOUNTING(VividWebClient);
    DISALLOW_COPY_AND_ASSIGN(VividWebClient);
};

} // namespace

/* --------------------------------------------------------------- producer */

struct _VividWebProducer
{
    GMutex lock;

    std::string project_path;
    std::string entry_url;
    std::string user_properties_json { "{}" };
    std::string media_state_json;
    std::string render_device { "auto" };
    bool muted { false };
    double volume { 1.0 };
    int fps { 30 };
    bool playing { true };

    VividGpuDevice resolved_gpu {};
    bool resolved_gpu_valid { false };

    guint32 width { 0 };
    guint32 height { 0 };
    double render_scale { 1.0 };

    WebFrameRing ring;
    ProducerFrameRoute frame_route { "VividWebProducer" };
    VividRendererReleaseGate release_gate {};
    bool release_gate_valid { false };

    CefRefPtr<VividWebClient> client;
    bool shared_textures { false };

    guint32 pointer_button_mask { 0 };
    int pointer_x_view { 0 };
    int pointer_y_view { 0 };

    WebAudioPipeline audio;

    enum class FramePath
    {
        Unknown,
        Accelerated,
        Software,
    };
    FramePath frame_path { FramePath::Unknown };
    guint accel_failure_count { 0 };
    bool logged_size_mismatch { false };
    bool logged_unsupported_format { false };

    _VividWebProducer() { g_mutex_init(&lock); }
    ~_VividWebProducer()
    {
        ring.destroy();
        g_mutex_clear(&lock);
    }
};

namespace
{

void
reset_frame_diagnostics(_VividWebProducer* self)
{
    self->frame_path = _VividWebProducer::FramePath::Unknown;
    self->accel_failure_count = 0;
    self->logged_size_mismatch = false;
    self->logged_unsupported_format = false;
}

void
reset_render_contract_locked(_VividWebProducer* self)
{
    self->ring.destroy();
    self->frame_route.reset();
    self->width = 0;
    self->height = 0;
    self->render_scale = 1.0;
    reset_frame_diagnostics(self);
}

void
post_client_task(const CefRefPtr<VividWebClient>& client, base::OnceClosure task)
{
    if (client)
        CefPostTask(TID_UI, std::move(task));
}

void
close_client_browser(const CefRefPtr<VividWebClient>& client, const char* reason)
{
    if (!client)
        return;

    /*
     * Detach first so CEF callbacks cannot write into the producer while the
     * GPU switch tears down the old browser-bound GBM/Vulkan contract. Waiting
     * for OnBeforeClose keeps the next browser generation from racing with a
     * still-live renderer/GPU helper that inherited the previous environment.
     */
    g_message("VividWebProducer: closing CEF browser for %s", reason);
    client->detach_producer();
    post_client_task(client,
                     base::BindOnce(&VividWebClient::DoClose, client));
    client->wait_browser_closed();
}

std::string
json_number(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(buffer, sizeof(buffer), value);
    return buffer;
}

std::string
build_general_properties_script(guint32 width_px, guint32 height_px, double scale, int fps)
{
    const int dip_width = MAX(1, (int)lround((double)width_px / scale));
    const int dip_height = MAX(1, (int)lround((double)height_px / scale));
    std::string payload = "{";
    payload += "\"width\":" + std::to_string(dip_width);
    payload += ",\"height\":" + std::to_string(dip_height);
    payload += ",\"screenWidth\":" + std::to_string(dip_width);
    payload += ",\"screenHeight\":" + std::to_string(dip_height);
    payload += ",\"renderWidth\":" + std::to_string(width_px);
    payload += ",\"renderHeight\":" + std::to_string(height_px);
    payload += ",\"scale\":" + json_number(scale);
    payload += ",\"scaleFactor\":" + json_number(scale);
    payload += ",\"devicePixelRatio\":" + json_number(scale);
    payload += ",\"fps\":" + std::to_string(fps);
    payload += "}";
    return "window.__vividApplyGeneralProperties && "
           "window.__vividApplyGeneralProperties(" + payload + ");";
}

std::string
build_user_properties_script(const std::string& user_properties_json)
{
    const std::string& payload =
        user_properties_json.empty() ? std::string("{}") : user_properties_json;
    return "window.__vividApplyUserProperties && "
           "window.__vividApplyUserProperties(" + payload + ");";
}

std::string
build_media_state_script(const std::string& media_state_json)
{
    if (media_state_json.empty())
        return {};
    return "window.__vividApplyMediaState && "
           "window.__vividApplyMediaState(" + media_state_json + ");";
}

/* CEF UI thread; pushes the full page state after each top-level load. */
void
push_initial_page_state(_VividWebProducer* self, const CefRefPtr<VividWebClient>& client)
{
    g_mutex_lock(&self->lock);
    std::string user_script = build_user_properties_script(self->user_properties_json);
    std::string general_script;
    if (self->width > 0 && self->height > 0) {
        general_script = build_general_properties_script(self->width,
                                                         self->height,
                                                         self->render_scale,
                                                         self->fps);
    }
    std::string media_script = build_media_state_script(self->media_state_json);
    const bool suspended = !self->playing;
    g_mutex_unlock(&self->lock);

    client->DoExecuteJs(std::move(user_script));
    if (!general_script.empty())
        client->DoExecuteJs(std::move(general_script));
    if (!media_script.empty())
        client->DoExecuteJs(std::move(media_script));
    if (suspended)
        client->DoSetSuspended(true);
}

VkFormat
vk_format_for_cef_color_type(cef_color_type_t format)
{
    switch (format) {
    case CEF_COLOR_TYPE_RGBA_8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case CEF_COLOR_TYPE_BGRA_8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

/* Producer-side frame sinks, called with client.producer_lock_ held. */

bool
web_wait_release_gate(const VividRendererReleaseGate& gate, guint32 buffer_index)
{
    if (gate.abi_version != VIVID_RENDERER_RELEASE_GATE_ABI_VERSION ||
        !gate.wait_release) {
        return true;
    }

    if (gate.wait_release(gate.user_data,
                          buffer_index,
                          WEB_RELEASE_GATE_TIMEOUT_MSEC)) {
        return true;
    }

    g_warning("VividWebProducer: release gate timed out for accelerated "
              "slot=%u timeout-ms=%u; dropping CEF frame",
              buffer_index,
              WEB_RELEASE_GATE_TIMEOUT_MSEC);
    return false;
}

void
producer_handle_accelerated_paint(_VividWebProducer* self,
                                  const CefAcceleratedPaintInfo& info)
{
    g_mutex_lock(&self->lock);

    if (!self->ring.valid()) {
        g_mutex_unlock(&self->lock);
        return;
    }

    const VkFormat source_format = vk_format_for_cef_color_type(info.format);
    if (source_format == VK_FORMAT_UNDEFINED) {
        if (!self->logged_unsupported_format) {
            g_warning("VividWebProducer: unsupported CEF shared texture format=%d",
                      (int)info.format);
            self->logged_unsupported_format = true;
        }
        g_mutex_unlock(&self->lock);
        return;
    }

    if (info.plane_count < 1 || info.planes[0].fd < 0) {
        g_mutex_unlock(&self->lock);
        return;
    }

    const guint32 coded_width = info.extra.coded_size.width > 0
        ? (guint32)info.extra.coded_size.width
        : self->ring.width;
    const guint32 coded_height = info.extra.coded_size.height > 0
        ? (guint32)info.extra.coded_size.height
        : self->ring.height;

    gint32 src_x = 0;
    gint32 src_y = 0;
    guint32 src_width = coded_width;
    guint32 src_height = coded_height;
    if (info.extra.visible_rect.width > 0 && info.extra.visible_rect.height > 0) {
        src_x = info.extra.visible_rect.x;
        src_y = info.extra.visible_rect.y;
        src_width = (guint32)info.extra.visible_rect.width;
        src_height = (guint32)info.extra.visible_rect.height;
    }

    const guint32 upload_slot_index = self->ring.in_progress_index;
    const bool release_gate_valid = self->release_gate_valid;
    const VividRendererReleaseGate release_gate = self->release_gate;

    if (!self->ring.vulkan_ready) {
        self->accel_failure_count++;
        if (self->accel_failure_count == 1 || self->accel_failure_count % 300 == 0) {
            g_warning("VividWebProducer: received an accelerated CEF frame without "
                      "a usable Vulkan shared-texture route; refusing to publish "
                      "frame (failure #%u)",
                      self->accel_failure_count);
        }
        g_mutex_unlock(&self->lock);
        return;
    }

    if (release_gate_valid) {
        g_mutex_unlock(&self->lock);
        if (!web_wait_release_gate(release_gate, upload_slot_index))
            return;
        g_mutex_lock(&self->lock);
        /*
         * The release wait can take up to 600ms. Re-check the ring contract
         * after reacquiring the producer mutex so a resize/device switch that
         * happened during the wait cannot make us import into a stale slot.
         */
        if (!self->ring.valid() || !self->ring.vulkan_ready ||
            self->ring.in_progress_index != upload_slot_index) {
            g_mutex_unlock(&self->lock);
            return;
        }
    }

    WebRingSlot& slot = self->ring.slots[upload_slot_index];

    auto imported = self->ring.vulkan.import_dmabuf_image(
        coded_width,
        coded_height,
        source_format,
        info.modifier,
        info.planes[0].fd,
        info.planes[0].size,
        (guint32)info.planes[0].offset,
        info.planes[0].stride,
        /* as_blit_target = */ false);
    if (!imported.has_value() ||
        !self->ring.vulkan.blit_image(imported.value(),
                                      src_x,
                                      src_y,
                                      src_width,
                                      src_height,
                                      slot.vk_image)) {
        self->accel_failure_count++;
        if (self->accel_failure_count == 1 || self->accel_failure_count % 300 == 0) {
            g_warning("VividWebProducer: strict shared-texture Vulkan copy failed; "
                      "refusing to publish frame (failure #%u) coded=%ux%u "
                      "modifier=0x%016" G_GINT64_MODIFIER "x vulkan-ready=%s",
                      self->accel_failure_count,
                      coded_width,
                      coded_height,
                      (guint64)info.modifier,
                      bool_to_string(self->ring.vulkan_ready));
        }
        g_mutex_unlock(&self->lock);
        return;
    }

    self->ring.mark_frame_ready();
    if (self->frame_path != _VividWebProducer::FramePath::Accelerated) {
        self->frame_path = _VividWebProducer::FramePath::Accelerated;
        g_message("VividWebProducer: accelerated frame path active "
                  "(CEF dmabuf -> Vulkan blit -> export ring) "
                  "coded=%ux%u visible=%ux%u",
                  coded_width,
                  coded_height,
                  src_width,
                  src_height);
    }
    g_mutex_unlock(&self->lock);
}

void
producer_handle_software_paint(_VividWebProducer* self,
                               const void* buffer,
                               int paint_width,
                               int paint_height)
{
    if (!buffer || paint_width <= 0 || paint_height <= 0)
        return;

    g_mutex_lock(&self->lock);
    if (self->frame_path != _VividWebProducer::FramePath::Software) {
        self->frame_path = _VividWebProducer::FramePath::Software;
        g_warning("VividWebProducer: refusing CEF OnPaint software frame "
                  "because the web backend requires DMA-BUF shared textures "
                  "without CPU copy size=%dx%d",
                  paint_width,
                  paint_height);
    }
    g_mutex_unlock(&self->lock);
}

} // namespace

/* ---- VividWebClient callbacks that need the full producer definition ---- */

void
VividWebClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
    const WebViewport viewport = viewport_snapshot();
    rect = CefRect(0, 0, viewport.view_width, viewport.view_height);
}

bool
VividWebClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info)
{
    const WebViewport viewport = viewport_snapshot();
    screen_info.device_scale_factor = (float)viewport.screen_scale;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, viewport.view_width, viewport.view_height);
    screen_info.available_rect = screen_info.rect;
    return true;
}

void
VividWebClient::OnPaint(CefRefPtr<CefBrowser> browser,
                         PaintElementType type,
                         const RectList& dirty_rects,
                         const void* buffer,
                         int width,
                         int height)
{
    if (type != PET_VIEW)
        return;
    with_producer([&](_VividWebProducer* producer) {
        producer_handle_software_paint(producer, buffer, width, height);
    });
    RestoreOneShotSuspendedIfNeeded(browser);
}

void
VividWebClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                                    PaintElementType type,
                                    const RectList& dirty_rects,
                                    const CefAcceleratedPaintInfo& info)
{
    if (type != PET_VIEW)
        return;
    with_producer([&](_VividWebProducer* producer) {
        producer_handle_accelerated_paint(producer, info);
    });
    RestoreOneShotSuspendedIfNeeded(browser);
}

void
VividWebClient::OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                                      const CefAudioParameters& params,
                                      int channels)
{
    with_producer([&](_VividWebProducer* producer) {
        producer->audio.start(channels, params.sample_rate);
    });
}

void
VividWebClient::OnAudioStreamPacket(CefRefPtr<CefBrowser> browser,
                                     const float** data,
                                     int frames,
                                     int64_t pts)
{
    with_producer([&](_VividWebProducer* producer) {
        producer->audio.push(data, frames);
    });
}

void
VividWebClient::OnAudioStreamStopped(CefRefPtr<CefBrowser> browser)
{
    with_producer([&](_VividWebProducer* producer) {
        producer->audio.stop();
    });
}

void
VividWebClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             TransitionType transition_type)
{
    if (!frame || !frame->IsMain())
        return;
    const std::string url = frame->GetURL().ToString();
    if (url.empty() || url == "about:blank")
        return;
    ApplyViewportToBrowser(browser, false);
}

void
VividWebClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                      bool is_loading,
                                      bool can_go_back,
                                      bool can_go_forward)
{
    if (is_loading)
        return;
    CefRefPtr<CefFrame> frame = browser ? browser->GetMainFrame() : nullptr;
    const std::string url = frame ? frame->GetURL().ToString() : std::string();
    if (url.empty() || url == "about:blank")
        return;
    CefRefPtr<VividWebClient> self_ref = this;
    with_producer([&](_VividWebProducer* producer) {
        g_message("VividWebProducer: page load finished url=%s; pushing initial wallpaper state",
                  url.c_str());
        push_initial_page_state(producer, self_ref);
    });
}

/* ------------------------------------------------------------------ C ABI */

VividWebProducer*
vivid_web_producer_new(void)
{
    gst_init(NULL, NULL);
    return new _VividWebProducer();
}

void
vivid_web_producer_free(VividWebProducer* self)
{
    if (!self)
        return;

    if (self->client) {
        close_client_browser(self->client, "producer free");
        self->client = nullptr;
    }

    self->audio.stop();
    delete self;
}

gboolean
vivid_web_producer_configure(VividWebProducer* self,
                              const gchar*       project_dir,
                              const gchar*       user_properties_json,
                              gboolean           muted,
                              gdouble            volume,
                              gint               fill_mode,
                              gint               fps,
                              const gchar*       render_device)
{
    g_return_val_if_fail(self != NULL, FALSE);
    (void)fill_mode; /* web pages always fill their own viewport */

    const std::string url = resolve_web_project_url(project_dir);
    if (url.empty()) {
        g_warning("VividWebProducer: rejected web project because no entry page "
                  "was found: %s",
                  project_dir ? project_dir : "(null)");
        return FALSE;
    }

    const std::string next_render_device =
        render_device && *render_device ? render_device : "auto";

    VividGpuDevice gpu {};
    const bool gpu_valid = vivid_gpu_device_resolve(next_render_device.c_str(), &gpu);
    if (gpu_valid)
        apply_gpu_process_environment(gpu);

    if (!ensure_cef_initialized())
        return FALSE;

    g_mutex_lock(&self->lock);
    const bool url_changed = self->entry_url != url;
    const bool user_properties_changed =
        self->user_properties_json != (user_properties_json ? user_properties_json : "{}");
    const bool fps_changed = self->fps != CLAMP(fps, 5, 240);
    const bool render_device_changed = self->render_device != next_render_device;

    self->project_path = project_dir ? project_dir : "";
    self->entry_url = url;
    self->user_properties_json =
        user_properties_json && *user_properties_json ? user_properties_json : "{}";
    self->muted = !!muted;
    self->volume = CLAMP(volume, 0.0, 1.0);
    self->fps = CLAMP(fps, 5, 240);
    self->render_device = next_render_device;
    if (gpu_valid) {
        self->resolved_gpu = gpu;
        self->resolved_gpu_valid = true;
    } else {
        self->resolved_gpu_valid = false;
    }

    CefRefPtr<VividWebClient> client_to_close;
    if (render_device_changed) {
        /*
         * A render-device change moves both sides of the web DMA-BUF contract:
         * Vivid's exported Vulkan ring and Chromium's source shared texture.
         * CEF cannot hot-migrate an existing browser/GPU process to a different
         * DRM render node, so the old browser is detached and closed outside
         * this mutex. prepare_buffers() will then publish a fresh ring and
         * create a new browser whose helper processes inherit the current GPU
         * policy through OnBeforeChildProcessLaunch.
         */
        client_to_close = self->client;
        self->client = nullptr;
        self->shared_textures = false;
    }

    const std::string user_props_json_copy = self->user_properties_json;
    const int next_fps = self->fps;
    const double next_volume_for_log = self->volume;
    const bool next_muted_for_audio = self->muted;
    g_mutex_unlock(&self->lock);

    if (client_to_close) {
        close_client_browser(client_to_close, "render-device change");
        self->audio.stop();
        const bool gpu_killed =
            client_to_close->kill_gpu_process_for_device_switch("render-device change");
        if (!gpu_killed) {
            g_warning("VividWebProducer: CEF GPU process kill did not report "
                      "success during render-device change; continuing so logs "
                      "show whether Chromium relaunches it on the new GPU");
        }
    }

    if (render_device_changed) {
        g_mutex_lock(&self->lock);
        reset_render_contract_locked(self);
        g_mutex_unlock(&self->lock);
        g_message("VividWebProducer: render-device changed to %s; closed old "
                  "CEF browser and reset web DMA-BUF contract",
                  next_render_device.c_str());
    }

    g_mutex_lock(&self->lock);
    const bool have_client = self->client != nullptr;
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);

    g_message("VividWebProducer: configure project=%s url=%s url-changed=%s "
              "user-properties-changed=%s render-device-changed=%s muted=%s "
              "volume=%.3f fps=%d render-device=%s gpu=%s",
              project_dir ? project_dir : "(null)",
              url.c_str(),
              bool_to_string(url_changed),
              bool_to_string(user_properties_changed),
              bool_to_string(render_device_changed),
              bool_to_string(!!muted),
              next_volume_for_log,
              next_fps,
              next_render_device.c_str(),
              gpu_valid ? gpu.name : "(unresolved)");

    if (!have_client) {
        client = new VividWebClient(self);
        g_mutex_lock(&self->lock);
        self->client = client;
        self->shared_textures = true;
        g_mutex_unlock(&self->lock);

        /*
         * Do not create the browser here. The first configure() call happens
         * before any consumer output has registered its physical size and
         * scale, so creating a browser now makes CEF render one default
         * 1280x720 frame and then resize. prepare_buffers() owns the actual
         * DMA-BUF contract and is therefore the first point where a correct
         * initial viewport can be guaranteed.
         */
        g_message("VividWebProducer: deferred CEF browser creation until "
                  "the first output viewport is known");
    }

    if (client && client->has_browser_created()) {
        if (url_changed) {
            post_client_task(client,
                             base::BindOnce(&VividWebClient::DoLoadUrl, client, url));
        } else if (user_properties_changed) {
            post_client_task(client,
                             base::BindOnce(&VividWebClient::DoExecuteJs,
                                            client,
                                            build_user_properties_script(user_props_json_copy)));
        }
        if (fps_changed) {
            post_client_task(client,
                             base::BindOnce(&VividWebClient::DoSetFrameRate,
                                            client,
                                            next_fps));
        }
    }

    self->audio.set_volume(next_volume_for_log, next_muted_for_audio);
    return TRUE;
}

void
vivid_web_producer_set_playing(VividWebProducer* self, gboolean playing)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    const bool changed = self->playing != !!playing;
    self->playing = !!playing;
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);

    if (changed && client) {
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoSetSuspended, client, !playing));
    }
}

void
vivid_web_producer_request_frame(VividWebProducer* self, const gchar* reason)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    CefRefPtr<VividWebClient> client = self->client;
    const bool restore_suspended = !self->playing;
    std::string reason_copy = reason && *reason ? reason : "";
    g_mutex_unlock(&self->lock);

    if (!client)
        return;

    post_client_task(client,
                     base::BindOnce(&VividWebClient::DoRequestFrame,
                                    client,
                                    restore_suspended,
                                    std::move(reason_copy)));
}

void
vivid_web_producer_set_pointer_motion(VividWebProducer* self, gdouble x, gdouble y)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    const WebViewport viewport =
        resolve_viewport((int)self->width,
                         (int)self->height,
                         self->render_scale,
                         self->shared_textures);
    /*
     * The display protocol reports physical output pixels. CEF mouse events
     * must use the same coordinate space as GetViewRect: DIP coordinates for
     * the software/logical path, physical coordinates for the accelerated view
     * that is page-zoomed back to logical CSS pixels.
     */
    self->pointer_x_view = physical_to_cef_view_coordinate(x, viewport);
    self->pointer_y_view = physical_to_cef_view_coordinate(y, viewport);
    const int x_view = self->pointer_x_view;
    const int y_view = self->pointer_y_view;
    const guint32 mask = self->pointer_button_mask;
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);

    if (client) {
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoMouseMove,
                                        client,
                                        x_view,
                                        y_view,
                                        mask));
    }
}

void
vivid_web_producer_set_pointer_button(VividWebProducer* self,
                                       guint32            button,
                                       gboolean           pressed)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    const guint32 bit = button >= 1 && button <= 3 ? (1u << (button - 1)) : 0u;
    if (pressed)
        self->pointer_button_mask |= bit;
    else
        self->pointer_button_mask &= ~bit;
    const int x_view = self->pointer_x_view;
    const int y_view = self->pointer_y_view;
    const guint32 mask = self->pointer_button_mask;
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);

    if (client) {
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoMouseButton,
                                        client,
                                        x_view,
                                        y_view,
                                        button,
                                        (bool)!!pressed,
                                        mask));
    }
}

void
vivid_web_producer_set_pointer_axis(VividWebProducer* self,
                                     gdouble            delta_x,
                                     gdouble            delta_y)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    const int x_view = self->pointer_x_view;
    const int y_view = self->pointer_y_view;
    const guint32 mask = self->pointer_button_mask;
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);

    if (client) {
        /*
         * Protocol axis deltas follow the desktop convention (positive =
         * scroll down/right, like DOM WheelEvent.delta*), while Chromium's
         * WebMouseWheelEvent uses positive = scroll up/left; negate so page
         * content scrolls the way the physical wheel moved.
         */
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoMouseWheel,
                                        client,
                                        x_view,
                                        y_view,
                                        -(int)lround(delta_x),
                                        -(int)lround(delta_y),
                                        mask));
    }
}

void
vivid_web_producer_set_media_state_json(VividWebProducer* self,
                                         const gchar*       media_state_json)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    self->media_state_json = media_state_json ? media_state_json : "";
    std::string script = build_media_state_script(self->media_state_json);
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);

    if (client && !script.empty()) {
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoExecuteJs,
                                        client,
                                        std::move(script)));
    }
}

void
vivid_web_producer_set_audio_samples(VividWebProducer* self, GVariant* audio_samples)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    CefRefPtr<VividWebClient> client = self->client;
    g_mutex_unlock(&self->lock);
    if (!client)
        return;

    std::string payload = "[";
    if (audio_samples && g_variant_is_of_type(audio_samples, G_VARIANT_TYPE("ad"))) {
        gsize count = 0;
        const gdouble* values =
            (const gdouble*)g_variant_get_fixed_array(audio_samples, &count, sizeof(gdouble));
        for (gsize i = 0; i < count; i++) {
            if (i > 0)
                payload += ",";
            payload += json_number(values[i]);
        }
    }
    payload += "]";

    post_client_task(client,
                     base::BindOnce(&VividWebClient::DoExecuteJs,
                                    client,
                                    "window.__vividApplyAudioFrame && "
                                    "window.__vividApplyAudioFrame(" + payload + ");"));
}

void
vivid_web_producer_set_release_gate(VividWebProducer*          self,
                                    const VividRendererReleaseGate* gate)
{
    g_return_if_fail(self != NULL);

    g_mutex_lock(&self->lock);
    if (gate && gate->abi_version == VIVID_RENDERER_RELEASE_GATE_ABI_VERSION &&
        gate->wait_release) {
        self->release_gate = *gate;
        self->release_gate_valid = true;
    } else {
        self->release_gate = {};
        self->release_gate_valid = false;
    }
    g_mutex_unlock(&self->lock);
}

static gboolean
vivid_web_producer_prepare_buffers_internal(
    VividWebProducer*                       self,
    guint32                                 width,
    guint32                                 height,
    gdouble                                 render_scale,
    const VividWebVulkanExportRequest&      export_request,
    VividWebProducerBufferSet*              out_set)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(out_set != NULL, FALSE);

    vivid::producer::init_dmabuf_buffer_set(*out_set);

    width = CLAMP(width, 1u, 8192u);
    height = CLAMP(height, 1u, 8192u);
    render_scale = MAX(1.0, render_scale);

    g_mutex_lock(&self->lock);

    if (!self->client) {
        g_mutex_unlock(&self->lock);
        g_warning("VividWebProducer: cannot prepare buffers before a web project is configured");
        return FALSE;
    }

    const bool contract_changed = !self->ring.valid() ||
        self->ring.width != width ||
        self->ring.height != height ||
        !web_export_requests_equal(self->ring.export_request, export_request) ||
        std::abs(self->render_scale - render_scale) > 0.0001;

    if (contract_changed) {
        if (!self->resolved_gpu_valid) {
            VividGpuDevice gpu {};
            if (!vivid_gpu_device_resolve(self->render_device.c_str(), &gpu)) {
                g_mutex_unlock(&self->lock);
                g_warning("VividWebProducer: no usable Vulkan GPU for render-device='%s'",
                          self->render_device.c_str());
                return FALSE;
            }
            self->resolved_gpu = gpu;
            self->resolved_gpu_valid = true;
        }

        if (!self->ring.create(self->resolved_gpu, width, height, export_request)) {
            g_mutex_unlock(&self->lock);
            return FALSE;
        }
        self->width = width;
        self->height = height;
        self->render_scale = render_scale;
        reset_frame_diagnostics(self);
        self->frame_route.reset();
    }

    DmabufBufferSetView route_set;
    route_set.width = self->ring.width;
    route_set.height = self->ring.height;
    route_set.fourcc = WEB_RING_FOURCC;
    route_set.modifier = self->ring.slots[0].vk_image.modifier;
    route_set.premultiplied = FALSE;
    route_set.n_buffers = WEB_RING_BUFFERS;
    for (guint i = 0; i < WEB_RING_BUFFERS; i++) {
        const VividWebVulkanImage& slot_image = self->ring.slots[i].vk_image;
        const guint64 slot_modifier = slot_image.modifier;
        if (slot_modifier != route_set.modifier) {
            g_mutex_unlock(&self->lock);
            g_warning("VividWebProducer: refusing mixed Vulkan export ring modifiers "
                      "slot0=0x%016" G_GINT64_MODIFIER "x slot%u=0x%016"
                      G_GINT64_MODIFIER "x",
                      static_cast<guint64>(route_set.modifier),
                      i,
                      static_cast<guint64>(slot_modifier));
            return FALSE;
        }

        auto& buffer = route_set.buffers[i];
        buffer.index = i;
        buffer.size = slot_image.size;
        buffer.n_planes = slot_image.n_planes;
        if (buffer.n_planes == 0 ||
            buffer.n_planes > vivid::producer::kFrameRouteMaxPlanes) {
            g_mutex_unlock(&self->lock);
            g_warning("VividWebProducer: export ring slot=%u has invalid plane count=%u",
                      i,
                      buffer.n_planes);
            return FALSE;
        }
        for (guint plane = 0; plane < buffer.n_planes; plane++) {
            buffer.planes[plane].fd = slot_image.plane_fds[plane];
            buffer.planes[plane].stride = slot_image.plane_strides[plane];
            buffer.planes[plane].offset = slot_image.plane_offsets[plane];
        }
    }

    const bool published = self->frame_route.publish_buffer_set(route_set, *out_set);
    CefRefPtr<VividWebClient> client = self->client;
    std::string general_script;
    bool notify_viewport = false;
    bool create_browser = false;
    std::string create_url;
    int create_fps = self->fps;
    bool create_shared_textures = self->shared_textures;
    bool create_suspended = !self->playing;
    guint32 create_width = width;
    guint32 create_height = height;
    double create_scale = render_scale;
    if (published) {
        client->set_viewport((int)width,
                             (int)height,
                             render_scale,
                             self->shared_textures);
    }
    if (published && contract_changed) {
        general_script =
            build_general_properties_script(width, height, render_scale, self->fps);
    }
    if (published && client) {
        const bool browser_created = client->has_browser_created();
        notify_viewport = contract_changed && browser_created;
        create_browser = !browser_created;
        if (create_browser) {
            create_url = self->entry_url;
            create_fps = self->fps;
            create_shared_textures = self->shared_textures;
            create_suspended = !self->playing;
            create_width = width;
            create_height = height;
            create_scale = render_scale;
        }
    }
    g_mutex_unlock(&self->lock);

    if (!published)
        return FALSE;

    if (create_browser) {
        g_message("VividWebProducer: creating deferred CEF browser after "
                  "viewport is ready physical=%ux%u scale=%.3f url=%s",
                  create_width,
                  create_height,
                  create_scale,
                  create_url.c_str());
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoCreateBrowser,
                                        client,
                                        create_url,
                                        create_fps,
                                        create_shared_textures));
        if (!client->wait_browser_created()) {
            g_warning("VividWebProducer: browser creation failed for %s",
                      create_url.c_str());
            client->detach_producer();
            g_mutex_lock(&self->lock);
            if (self->client.get() == client.get())
                self->client = nullptr;
            self->ring.destroy();
            reset_frame_diagnostics(self);
            self->frame_route.reset();
            g_mutex_unlock(&self->lock);
            vivid::producer::clear_dmabuf_buffer_set(*out_set);
            return FALSE;
        }
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoSetSuspended,
                                        client,
                                        create_suspended));
    }

    if (!general_script.empty()) {
        if (notify_viewport) {
            post_client_task(client,
                             base::BindOnce(&VividWebClient::DoNotifyViewportChanged,
                                            client));
        }
        post_client_task(client,
                         base::BindOnce(&VividWebClient::DoExecuteJs,
                                        client,
                                        std::move(general_script)));
    }

    g_message("VividWebProducer: prepared DMA-BUF buffer set %ux%u scale=%.3f buffers=%u",
              out_set->width,
              out_set->height,
              render_scale,
              out_set->n_buffers);
    return out_set->n_buffers > 0;
}

gboolean
vivid_web_producer_prepare_buffers(VividWebProducer*          self,
                                    guint32                     width,
                                    guint32                     height,
                                    gdouble                     render_scale,
                                    VividWebProducerBufferSet* out_set)
{
    const VividWebVulkanExportRequest export_request = web_export_request_from_abi(nullptr);
    return vivid_web_producer_prepare_buffers_internal(self,
                                                       width,
                                                       height,
                                                       render_scale,
                                                       export_request,
                                                       out_set);
}

gboolean
vivid_web_producer_query_dmabuf_caps(VividWebProducer*           self,
                                     VividWebProducerDmaBufCaps* out_caps)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_caps != nullptr, FALSE);

    memset(out_caps, 0, sizeof(*out_caps));
    VividGpuDevice gpu {};
    if (!self->resolved_gpu_valid) {
        if (!vivid_gpu_device_resolve(self->render_device.c_str(), &gpu)) {
            g_warning("VividWebProducer: cannot query DMA-BUF caps for unresolved "
                      "render-device='%s'",
                      self->render_device.c_str());
            return FALSE;
        }
        self->resolved_gpu = gpu;
        self->resolved_gpu_valid = true;
    } else {
        gpu = self->resolved_gpu;
    }

    const auto caps = VividWebVulkanRoute::query_export_caps(
        gpu,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    for (const auto& cap : caps) {
        if (out_caps->n_caps >= VIVID_WEB_PRODUCER_DMABUF_MAX_CAPS)
            break;
        out_caps->caps[out_caps->n_caps++] = {
            .fourcc = cap.fourcc,
            .modifier = cap.modifier,
            .plane_count = cap.plane_count,
        };
    }
    if (out_caps->n_caps == 0) {
        out_caps->caps[out_caps->n_caps++] = {
            .fourcc = WEB_RING_FOURCC,
            .modifier = DRM_FORMAT_MOD_LINEAR,
            .plane_count = 1,
        };
    }
    out_caps->memory_preference = VIVID_WEB_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    return TRUE;
}

gboolean
vivid_web_producer_prepare_buffers_with_request(
    VividWebProducer*                    self,
    guint32                              width,
    guint32                              height,
    gdouble                              render_scale,
    const VividWebProducerDmaBufRequest* request,
    VividWebProducerBufferSet*           out_set)
{
    if (request && request->fourcc != 0 && request->fourcc != WEB_RING_FOURCC) {
        g_warning("VividWebProducer: requested fourcc=0x%08x but web route "
                  "currently exports fourcc=0x%08x",
                  request->fourcc,
                  WEB_RING_FOURCC);
        return FALSE;
    }
    const VividWebVulkanExportRequest export_request = web_export_request_from_abi(request);
    return vivid_web_producer_prepare_buffers_internal(self,
                                                       width,
                                                       height,
                                                       render_scale,
                                                       export_request,
                                                       out_set);
}

gboolean
vivid_web_producer_next_frame(VividWebProducer* self, VividWebProducerFrame* out_frame)
{
    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    memset(out_frame, 0, sizeof(*out_frame));

    g_mutex_lock(&self->lock);
    guint32 index = 0;
    const bool has_frame = self->ring.valid() && self->ring.eat_frame(&index);
    if (has_frame)
        self->frame_route.write_ready_frame(index, (gint32)index, *out_frame);
    g_mutex_unlock(&self->lock);

    return has_frame;
}

void
vivid_web_producer_buffer_set_clear(VividWebProducerBufferSet* set)
{
    if (!set)
        return;
    vivid::producer::clear_dmabuf_buffer_set(*set);
}

void
vivid_web_producer_global_shutdown(void)
{
    g_mutex_lock(&g_cef_lock);
    const bool initialized = g_cef_state == CefGlobalState::Initialized;
    if (initialized)
        g_cef_state = CefGlobalState::ShutDown;
    g_mutex_unlock(&g_cef_lock);

    if (initialized) {
        g_message("VividWebProducer: shutting down CEF");
        CefShutdown();
    }
}
