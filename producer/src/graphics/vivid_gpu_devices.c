/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_gpu_devices.h"

#include <drm/drm_fourcc.h>
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#define VIVID_GPU_SCENE_DMABUF_FOURCC DRM_FORMAT_ABGR8888
#define VIVID_GPU_SCENE_DMABUF_VK_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define VIVID_GPU_SCENE_REQUIRED_DMABUF_FEATURES \
    ((VkFormatFeatureFlags2)(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
                             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
                             VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
#define VIVID_GPU_SCENE_DMABUF_USAGE \
    ((VkImageUsageFlags)(VK_IMAGE_USAGE_SAMPLED_BIT | \
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | \
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT))

static gboolean
render_device_value_is_auto(const gchar* value)
{
    return !value || !*value || g_ascii_strcasecmp(value, "auto") == 0;
}

static void
fill_pci_address_for_render_node(const gchar* render_node,
                                 gchar*       out_address,
                                 gsize        out_size)
{
    out_address[0] = '\0';
    if (!render_node || !*render_node)
        return;

    const gchar* basename = strrchr(render_node, '/');
    basename = basename ? basename + 1 : render_node;

    gchar sysfs_path[PATH_MAX];
    g_snprintf(sysfs_path,
               sizeof(sysfs_path),
               "/sys/class/drm/%s/device",
               basename);

    gchar resolved[PATH_MAX];
    if (!realpath(sysfs_path, resolved))
        return;

    const gchar* address = strrchr(resolved, '/');
    address = address ? address + 1 : resolved;
    if (*address)
        g_strlcpy(out_address, address, out_size);
}

static gboolean
bracket_text_looks_like_pci_id(const gchar* start, const gchar* end)
{
    if (!start || !end || end <= start)
        return FALSE;

    gboolean saw_colon = FALSE;
    for (const gchar* p = start; p < end; p++) {
        if (*p == ':') {
            saw_colon = TRUE;
            continue;
        }
        if (!g_ascii_isxdigit(*p))
            return FALSE;
    }
    return saw_colon;
}

static void
fill_lspci_name_for_pci_address(const gchar* pci_address,
                                gchar*       out_name,
                                gsize        out_size)
{
    if (!pci_address || !*pci_address || out_size == 0)
        return;

    gchar* argv[] = {
        (gchar*)"lspci",
        (gchar*)"-nn",
        (gchar*)"-s",
        (gchar*)pci_address,
        NULL,
    };
    g_autofree gchar* stdout_text = NULL;
    g_autofree gchar* stderr_text = NULL;
    gint exit_status = 0;
    GError* error = NULL;
    if (!g_spawn_sync(NULL,
                      argv,
                      NULL,
                      G_SPAWN_SEARCH_PATH,
                      NULL,
                      NULL,
                      &stdout_text,
                      &stderr_text,
                      &exit_status,
                      &error)) {
        g_debug("VividGpuDevices: lspci lookup failed for pci=%s: %s",
                pci_address,
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    if (exit_status != 0 || !stdout_text || !*stdout_text) {
        g_debug("VividGpuDevices: lspci lookup returned no GPU name for pci=%s stderr=%s",
                pci_address,
                stderr_text && *stderr_text ? stderr_text : "(none)");
        return;
    }

    /*
     * Match the user-facing format from the WebUI discovery command:
     *
     *   /dev/dri/renderD128 : NVIDIA Corporation AD107M [GeForce ...]
     *
     * lspci includes both the PCI class ("[0300]") and the vendor/device ID
     * ("[10de:28e0]"). Keep the descriptive middle section, including product
     * brackets such as "[GeForce ...]", but trim the trailing numeric PCI ID.
     */
    gchar** lines = g_strsplit(stdout_text, "\n", 0);
    for (guint i = 0; lines && lines[i]; i++) {
        if (!strstr(lines[i], "[03"))
            continue;

        gchar* class_end = strstr(lines[i], "]:");
        if (!class_end)
            continue;

        g_autofree gchar* name = g_strdup(class_end + 2);
        g_strstrip(name);
        gchar* last_open = strrchr(name, '[');
        gchar* last_close = strrchr(name, ']');
        if (last_open && last_close && last_close > last_open &&
            bracket_text_looks_like_pci_id(last_open + 1, last_close)) {
            *last_open = '\0';
            g_strstrip(name);
        }

        if (*name) {
            g_strlcpy(out_name, name, out_size);
            break;
        }
    }
    g_strfreev(lines);
}

static void
find_render_node_for_drm_ids(gint64 render_major,
                             gint64 render_minor,
                             gchar* out_node,
                             gsize  out_size)
{
    out_node[0] = '\0';

    DIR* dir = opendir("/dev/dri");
    if (!dir)
        return;

    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "renderD", 7) != 0)
            continue;

        gchar path[VIVID_GPU_DEVICE_RENDER_NODE_MAX];
        g_snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
            continue;

        if ((gint64)major(st.st_rdev) == render_major &&
            (gint64)minor(st.st_rdev) == render_minor) {
            g_strlcpy(out_node, path, out_size);
            break;
        }
    }
    closedir(dir);
}

static gint
compare_gpu_devices_by_render_node(gconstpointer left, gconstpointer right)
{
    const VividGpuDevice* lhs = left;
    const VividGpuDevice* rhs = right;
    return g_strcmp0(lhs->render_node, rhs->render_node);
}

static gboolean
device_has_extension(VkPhysicalDevice gpu, const char* extension_name)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL) != VK_SUCCESS ||
        count == 0)
        return FALSE;

    VkExtensionProperties* extensions = g_new0(VkExtensionProperties, count);
    gboolean found = FALSE;
    if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, extensions) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(extensions[i].extensionName, extension_name) == 0) {
                found = TRUE;
                break;
            }
        }
    }
    g_free(extensions);
    return found;
}

static gboolean
dmabuf_caps_contains(const VividGpuDmaBufFormatCap* caps,
                     guint32                         n_caps,
                     guint32                         fourcc,
                     guint64                         modifier,
                     guint32                         plane_count)
{
    for (guint32 i = 0; i < n_caps; i++) {
        if (caps[i].fourcc == fourcc &&
            caps[i].modifier == modifier &&
            caps[i].plane_count == plane_count) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
dmabuf_caps_append(VividGpuDmaBufFormatCap* caps,
                   guint32*                  n_caps,
                   guint32                   fourcc,
                   guint64                   modifier,
                   guint32                   plane_count)
{
    if (!caps || !n_caps || *n_caps >= VIVID_GPU_DEVICE_DMABUF_CAPS_MAX ||
        plane_count == 0 ||
        dmabuf_caps_contains(caps, *n_caps, fourcc, modifier, plane_count)) {
        return;
    }

    caps[*n_caps].fourcc = fourcc;
    caps[*n_caps].modifier = modifier;
    caps[*n_caps].plane_count = plane_count;
    *n_caps += 1;
}

static gboolean
physical_device_has_scene_dmabuf_export_extensions(VkPhysicalDevice gpu)
{
    return device_has_extension(gpu, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
        device_has_extension(gpu, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
        device_has_extension(gpu, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

static gboolean
physical_device_has_scene_linear_dmabuf_export_extensions(VkPhysicalDevice gpu)
{
    return device_has_extension(gpu, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
        device_has_extension(gpu, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
}

static gboolean
scene_dmabuf_export_tuple_supported(VkPhysicalDevice gpu,
                                    VkImageTiling    tiling,
                                    guint64          modifier)
{
    VkPhysicalDeviceExternalImageFormatInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external_info,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const void* image_info_pnext = tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
        ? (const void*)&modifier_info
        : (const void*)&external_info;
    VkPhysicalDeviceImageFormatInfo2 image_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = image_info_pnext,
        .format = VIVID_GPU_SCENE_DMABUF_VK_FORMAT,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = tiling,
        .usage = VIVID_GPU_SCENE_DMABUF_USAGE,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 image_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    const VkResult result =
        vkGetPhysicalDeviceImageFormatProperties2(gpu, &image_info, &image_properties);
    if (result != VK_SUCCESS) {
        g_debug("VividGpuDevices: scene DMA-BUF export query failed tiling=%s "
                "modifier=0x%016" G_GINT64_MODIFIER "x result=%d",
                tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ? "modifier" : "linear",
                modifier,
                (int)result);
        return FALSE;
    }

    const VkExternalMemoryProperties* external =
        &external_properties.externalMemoryProperties;
    if ((external->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0 ||
        (external->compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0) {
        g_debug("VividGpuDevices: scene DMA-BUF tuple is not exportable tiling=%s "
                "modifier=0x%016" G_GINT64_MODIFIER "x external-features=0x%x "
                "compatible-handles=0x%x",
                tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ? "modifier" : "linear",
                modifier,
                external->externalMemoryFeatures,
                external->compatibleHandleTypes);
        return FALSE;
    }
    return TRUE;
}

static void
fill_scene_dmabuf_caps_from_physical_device(VkPhysicalDevice gpu,
                                            VividGpuDevice*  out_device)
{
    if (!out_device)
        return;

    /*
     * This mirrors waywallen's pool-owned capability model: modifier facts are
     * collected while the producer is already doing its single Vulkan GPU
     * enumeration, then later negotiation only intersects cached tuples. The
     * negotiation path must not create a second temporary VkInstance while
     * wallpaper-scene-renderer's render thread is initializing Vulkan; NVIDIA's
     * ICD/loader stack has crashed in that exact concurrent loader scan.
     */
    if (physical_device_has_scene_linear_dmabuf_export_extensions(gpu) &&
        scene_dmabuf_export_tuple_supported(gpu, VK_IMAGE_TILING_LINEAR, 0)) {
        dmabuf_caps_append(out_device->scene_dmabuf_caps,
                           &out_device->scene_dmabuf_n_caps,
                           VIVID_GPU_SCENE_DMABUF_FOURCC,
                           DRM_FORMAT_MOD_LINEAR,
                           1);
    } else {
        g_debug("VividGpuDevices: scene linear DMA-BUF export tuple is unavailable");
    }

    if (!physical_device_has_scene_dmabuf_export_extensions(gpu))
        return;

    VkDrmFormatModifierPropertiesList2EXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT,
    };
    VkFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifier_list,
    };
    vkGetPhysicalDeviceFormatProperties2(gpu,
                                         VIVID_GPU_SCENE_DMABUF_VK_FORMAT,
                                         &format_properties);
    if (modifier_list.drmFormatModifierCount == 0)
        return;

    VkDrmFormatModifierProperties2EXT* modifiers =
        g_new0(VkDrmFormatModifierProperties2EXT,
               modifier_list.drmFormatModifierCount);
    modifier_list.pDrmFormatModifierProperties = modifiers;
    vkGetPhysicalDeviceFormatProperties2(gpu,
                                         VIVID_GPU_SCENE_DMABUF_VK_FORMAT,
                                         &format_properties);

    for (uint32_t i = 0; i < modifier_list.drmFormatModifierCount; i++) {
        const VkDrmFormatModifierProperties2EXT* modifier = &modifiers[i];
        if (modifier->drmFormatModifier == DRM_FORMAT_MOD_LINEAR ||
            modifier->drmFormatModifier == DRM_FORMAT_MOD_INVALID ||
            modifier->drmFormatModifierPlaneCount != 1) {
            continue;
        }
        if ((modifier->drmFormatModifierTilingFeatures &
             VIVID_GPU_SCENE_REQUIRED_DMABUF_FEATURES) !=
            VIVID_GPU_SCENE_REQUIRED_DMABUF_FEATURES) {
            continue;
        }
        if (!scene_dmabuf_export_tuple_supported(gpu,
                                                 VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                                 modifier->drmFormatModifier)) {
            g_debug("VividGpuDevices: skipping scene modifier=0x%016"
                    G_GINT64_MODIFIER "x because DMA-BUF export is unavailable",
                    modifier->drmFormatModifier);
            continue;
        }

        dmabuf_caps_append(out_device->scene_dmabuf_caps,
                           &out_device->scene_dmabuf_n_caps,
                           VIVID_GPU_SCENE_DMABUF_FOURCC,
                           modifier->drmFormatModifier,
                           modifier->drmFormatModifierPlaneCount);
    }

    g_free(modifiers);
}

static gboolean
uuid_is_zero(const guint8* uuid)
{
    static const guint8 zeros[VIVID_GPU_DEVICE_UUID_BYTES] = {0};
    return memcmp(uuid, zeros, VIVID_GPU_DEVICE_UUID_BYTES) == 0;
}

static gboolean
uuid_equal(const guint8* left, const guint8* right)
{
    return memcmp(left, right, VIVID_GPU_DEVICE_UUID_BYTES) == 0;
}

static gboolean
gpu_devices_share_driver(const VividGpuDevice* left,
                         const VividGpuDevice* right)
{
    return !uuid_is_zero(left->uuid) &&
        uuid_equal(left->uuid, right->uuid) &&
        uuid_equal(left->driver_uuid, right->driver_uuid);
}

/*
 * Prefer the ICD that actually owns DMA-BUF export and the vendor decoder
 * (NVDEC / VA). NVK is a second NVIDIA ICD for the same render node and
 * must not win over the proprietary driver.
 */
static int
gpu_driver_rank(guint32 driver_id)
{
    switch (driver_id) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:
    case VK_DRIVER_ID_MESA_RADV:
        return 3;
#ifdef VK_DRIVER_ID_MESA_NVK
    case VK_DRIVER_ID_MESA_NVK:
        return 1;
#endif
    default:
        return 2;
    }
}

static gint
gpu_devices_find_duplicate(const VividGpuDeviceList* list,
                           const VividGpuDevice*     device)
{
    gint uuid_match = -1;
    gint node_match = -1;

    for (guint i = 0; i < list->n_devices; i++) {
        const VividGpuDevice* existing = &list->devices[i];
        if (!uuid_is_zero(device->uuid) && uuid_equal(existing->uuid, device->uuid))
            uuid_match = (gint)i;
        if (device->render_node[0] &&
            g_strcmp0(existing->render_node, device->render_node) == 0) {
            node_match = (gint)i;
        }
    }

    /*
     * deviceUUID is the Vulkan physical-device identity. Use it first so a
     * loader that lists one ICD twice (same UUID, same driver) collapses
     * even if render-node lookup somehow diverged.
     */
    if (uuid_match >= 0)
        return uuid_match;
    return node_match;
}

static void
gpu_device_merge_caps(VividGpuDevice* dest, const VividGpuDevice* src)
{
    for (guint32 i = 0; i < src->scene_dmabuf_n_caps; i++) {
        dmabuf_caps_append(dest->scene_dmabuf_caps,
                           &dest->scene_dmabuf_n_caps,
                           src->scene_dmabuf_caps[i].fourcc,
                           src->scene_dmabuf_caps[i].modifier,
                           src->scene_dmabuf_caps[i].plane_count);
    }
}

static gboolean
gpu_device_should_replace(const VividGpuDevice* existing,
                          const VividGpuDevice* candidate)
{
    /*
     * Same physical device from the same driver is a repeated report. Keep
     * the first device identity while vivid_gpu_devices_add() unions only the
     * capability tuples reported by that exact driver.
     */
    if (!uuid_is_zero(existing->uuid) &&
        uuid_equal(existing->uuid, candidate->uuid) &&
        uuid_equal(existing->driver_uuid, candidate->driver_uuid)) {
        return FALSE;
    }

    const int existing_rank = gpu_driver_rank(existing->vulkan_driver_id);
    const int candidate_rank = gpu_driver_rank(candidate->vulkan_driver_id);
    if (candidate_rank != existing_rank)
        return candidate_rank > existing_rank;

    return candidate->scene_dmabuf_n_caps > existing->scene_dmabuf_n_caps;
}

static void
gpu_devices_add(VividGpuDeviceList* list, const VividGpuDevice* device)
{
    g_return_if_fail(list != NULL);
    g_return_if_fail(device != NULL);

    const gint existing_index = gpu_devices_find_duplicate(list, device);
    if (existing_index >= 0) {
        VividGpuDevice* existing = &list->devices[existing_index];
        const gboolean replace = gpu_device_should_replace(existing, device);
        if (gpu_devices_share_driver(existing, device)) {
            VividGpuDevice merged = replace ? *device : *existing;
            merged.scene_dmabuf_n_caps = 0;
            memset(merged.scene_dmabuf_caps, 0, sizeof(merged.scene_dmabuf_caps));
            gpu_device_merge_caps(&merged, existing);
            gpu_device_merge_caps(&merged, device);
            *existing = merged;
        } else if (replace) {
            /*
             * One DRM render node can be exposed by multiple Vulkan ICDs.
             * Capabilities belong to the selected ICD and must not be unioned:
             * advertising a modifier reported only by the discarded ICD would
             * make the renderer negotiate an image contract it cannot create.
             */
            *existing = *device;
        }
        return;
    }

    if (list->n_devices >= VIVID_GPU_DEVICES_MAX) {
        g_warning("VividGpuDevices: ignoring extra GPU '%s' node=%s; list is full",
                  device->name,
                  device->render_node);
        return;
    }

    list->devices[list->n_devices++] = *device;
}

static void
log_gpu_device(guint index, const VividGpuDevice* device)
{
    g_message("VividGpuDevices: device[%u] node=%s pci=%s name=%s "
              "vendor=%s (0x%04x) discrete=%s scene-dmabuf-caps=%u",
              index,
              device->render_node,
              device->pci_address[0] ? device->pci_address : "(unknown)",
              device->name,
              vivid_gpu_vendor_name(device->vendor_id),
              device->vendor_id,
              device->is_discrete ? "true" : "false",
              device->scene_dmabuf_n_caps);
    for (guint32 cap_index = 0; cap_index < device->scene_dmabuf_n_caps; cap_index++) {
        const VividGpuDmaBufFormatCap* cap = &device->scene_dmabuf_caps[cap_index];
        g_debug("VividGpuDevices: device[%u] dmabuf-cap[%u] fourcc=0x%08x "
                "modifier=0x%016" G_GINT64_MODIFIER "x planes=%u",
                index,
                cap_index,
                cap->fourcc,
                cap->modifier,
                cap->plane_count);
    }
}

gboolean
vivid_gpu_devices_enumerate(VividGpuDeviceList* out_list)
{
    g_return_val_if_fail(out_list != NULL, FALSE);

    memset(out_list, 0, sizeof(*out_list));

    const VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vivid GPU enumeration",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "VividProducer",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        /* 1.1 makes vkGetPhysicalDeviceProperties2 core. */
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        g_warning("VividGpuDevices: failed to create Vulkan instance result=%d",
                  (int)result);
        return FALSE;
    }

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        g_warning("VividGpuDevices: no Vulkan physical devices result=%d", (int)result);
        vkDestroyInstance(instance, NULL);
        return FALSE;
    }

    VkPhysicalDevice* gpus = g_new0(VkPhysicalDevice, count);
    result = vkEnumeratePhysicalDevices(instance, &count, gpus);
    if (result != VK_SUCCESS) {
        g_warning("VividGpuDevices: failed to enumerate Vulkan devices result=%d",
                  (int)result);
        g_free(gpus);
        vkDestroyInstance(instance, NULL);
        return FALSE;
    }

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceDriverProperties driver_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        };
        VkPhysicalDeviceDrmPropertiesEXT drm_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
        };
        VkPhysicalDeviceIDProperties id_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 props2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id_props,
        };

        const gboolean has_driver_props =
            device_has_extension(gpus[i], VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
        const gboolean has_drm_props =
            device_has_extension(gpus[i], "VK_EXT_physical_device_drm");
        if (has_driver_props) {
            id_props.pNext = &driver_props;
        }
        if (has_drm_props && has_driver_props) {
            driver_props.pNext = &drm_props;
        } else if (has_drm_props) {
            id_props.pNext = &drm_props;
        }

        vkGetPhysicalDeviceProperties2(gpus[i], &props2);

        /*
         * Software rasterizers (llvmpipe) cannot back the DMA-BUF producer path
         * and would only confuse the device picker exposed to the user.
         */
        if (props2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;

        VividGpuDevice device = {0};
        g_strlcpy(device.name, props2.properties.deviceName, sizeof(device.name));
        device.vendor_id = props2.properties.vendorID;
        device.is_discrete =
            props2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        device.vulkan_driver_id = has_driver_props
            ? (guint32)driver_props.driverID
            : 0;
        memcpy(device.uuid, id_props.deviceUUID, VIVID_GPU_DEVICE_UUID_BYTES);
        memcpy(device.driver_uuid,
               id_props.driverUUID,
               VIVID_GPU_DEVICE_UUID_BYTES);

        if (has_drm_props && drm_props.hasRender) {
            device.drm_render_major = drm_props.renderMajor;
            device.drm_render_minor = drm_props.renderMinor;
            find_render_node_for_drm_ids(drm_props.renderMajor,
                                         drm_props.renderMinor,
                                         device.render_node,
                                         sizeof(device.render_node));
            fill_pci_address_for_render_node(device.render_node,
                                             device.pci_address,
                                             sizeof(device.pci_address));
            fill_lspci_name_for_pci_address(device.pci_address,
                                            device.name,
                                            sizeof(device.name));
        }

        /*
         * The selected GPU must be expressible as a DRM render node because the
         * consumer/helper side aligns itself from that protocol field. A Vulkan
         * device without a confirmed render node cannot satisfy the end-to-end
         * same-card invariant, so keep it out of WebUI and auto resolution.
         */
        if (!device.render_node[0]) {
            g_warning("VividGpuDevices: skipping Vulkan GPU '%s' vendor=%s "
                      "(0x%04x) because no DRM render node could be confirmed",
                      device.name,
                      vivid_gpu_vendor_name(device.vendor_id),
                      device.vendor_id);
            continue;
        }

        fill_scene_dmabuf_caps_from_physical_device(gpus[i], &device);
        gpu_devices_add(out_list, &device);
    }

    g_free(gpus);
    vkDestroyInstance(instance, NULL);

    qsort(out_list->devices,
          out_list->n_devices,
          sizeof(out_list->devices[0]),
          compare_gpu_devices_by_render_node);

    for (guint i = 0; i < out_list->n_devices; i++)
        log_gpu_device(i, &out_list->devices[i]);

    if (out_list->n_devices == 0) {
        g_warning("VividGpuDevices: Vulkan reported devices but none is usable");
        return FALSE;
    }
    return TRUE;
}

gboolean
vivid_gpu_devices_resolve(const VividGpuDeviceList* list,
                           const gchar*               render_device_value,
                           VividGpuDevice*           out_device)
{
    g_return_val_if_fail(out_device != NULL, FALSE);

    if (!list || list->n_devices == 0)
        return FALSE;

    if (!render_device_value_is_auto(render_device_value)) {
        for (guint i = 0; i < list->n_devices; i++) {
            if (list->devices[i].render_node[0] &&
                g_strcmp0(list->devices[i].render_node, render_device_value) == 0) {
                *out_device = list->devices[i];
                return TRUE;
            }
        }
        g_warning("VividGpuDevices: configured render-device '%s' matches no "
                  "enumerated Vulkan render device; refusing to choose a "
                  "different GPU",
                  render_device_value);
        return FALSE;
    }

    for (guint i = 0; i < list->n_devices; i++) {
        if (list->devices[i].is_discrete) {
            *out_device = list->devices[i];
            return TRUE;
        }
    }

    *out_device = list->devices[0];
    return TRUE;
}

gboolean
vivid_gpu_device_resolve(const gchar*     render_device_value,
                          VividGpuDevice* out_device)
{
    VividGpuDeviceList list;
    if (!vivid_gpu_devices_enumerate(&list))
        return FALSE;
    return vivid_gpu_devices_resolve(&list, render_device_value, out_device);
}

VividGpuDecoderRoute
vivid_gpu_decoder_route_for_vendor(guint32 vendor_id)
{
    if (vendor_id == 0)
        return VIVID_GPU_DECODER_ROUTE_NONE;
    if (vendor_id == VIVID_GPU_VENDOR_ID_NVIDIA)
        return VIVID_GPU_DECODER_ROUTE_NVIDIA;
    /*
     * Everything that is not NVIDIA decodes through VA-API: Intel and AMD are
     * the supported cases, and other Mesa-driven hardware has no better route.
     */
    return VIVID_GPU_DECODER_ROUTE_VA;
}

const gchar*
vivid_gpu_decoder_route_name(VividGpuDecoderRoute route)
{
    switch (route) {
    case VIVID_GPU_DECODER_ROUTE_NVIDIA:
        return "nvidia";
    case VIVID_GPU_DECODER_ROUTE_VA:
        return "va";
    case VIVID_GPU_DECODER_ROUTE_NONE:
    default:
        return "none";
    }
}

const gchar*
vivid_gpu_vendor_name(guint32 vendor_id)
{
    switch (vendor_id) {
    case VIVID_GPU_VENDOR_ID_NVIDIA:
        return "nvidia";
    case VIVID_GPU_VENDOR_ID_INTEL:
        return "intel";
    case VIVID_GPU_VENDOR_ID_AMD:
        return "amd";
    default:
        return "unknown";
    }
}
