/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_web_vulkan_route.hpp"

#include <glib.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include <drm/drm_fourcc.h>

namespace
{

constexpr std::array kRequiredDeviceExtensions {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

const char*
vk_result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
        return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
    default: return "VK_RESULT_UNKNOWN";
    }
}

const char*
bool_to_string(bool value)
{
    return value ? "true" : "false";
}

template<typename Property>
bool
has_extension(const std::vector<Property>& properties, const char* name)
{
    return std::any_of(properties.begin(), properties.end(), [name](const Property& property) {
        return strcmp(property.extensionName, name) == 0;
    });
}

std::vector<VkExtensionProperties>
enumerate_device_extensions(VkPhysicalDevice gpu)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkExtensionProperties> result(count);
    if (count > 0 &&
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, result.data()) != VK_SUCCESS)
        return {};
    return result;
}

std::optional<uint32_t>
find_memory_type(const VkPhysicalDeviceMemoryProperties& memory_properties,
                 uint32_t memory_type_bits)
{
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((memory_type_bits & (1u << i)) != 0)
            return i;
    }
    return std::nullopt;
}

std::optional<uint32_t>
find_memory_type_exact(const VkPhysicalDeviceMemoryProperties& memory_properties,
                       uint32_t memory_type_bits,
                       VkMemoryPropertyFlags required_flags,
                       VkMemoryPropertyFlags forbidden_flags)
{
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((memory_type_bits & (1u << i)) == 0)
            continue;
        const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & required_flags) == required_flags &&
            (forbidden_flags == 0 || (flags & forbidden_flags) == 0))
            return i;
    }
    return std::nullopt;
}

const char*
vk_external_memory_feature_names(VkExternalMemoryFeatureFlags features)
{
    static thread_local gchar* text = nullptr;
    g_free(text);
    text = nullptr;

    GString* builder = g_string_new(nullptr);
    if (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
        g_string_append(builder, "DEDICATED_ONLY|");
    if (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
        g_string_append(builder, "EXPORTABLE|");
    if (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
        g_string_append(builder, "IMPORTABLE|");
    if (builder->len == 0)
        g_string_append(builder, "0");
    else
        g_string_truncate(builder, builder->len - 1);

    text = g_string_free(builder, FALSE);
    return text;
}

bool
validate_dmabuf_image_capabilities(VkPhysicalDevice physical_device,
                                   uint32_t         width,
                                   uint32_t         height,
                                   VkFormat         format,
                                   uint64_t         modifier,
                                   VkImageUsageFlags usage,
                                   bool             as_blit_target)
{
    if (physical_device == VK_NULL_HANDLE) {
        g_warning("VividWebProducer: refusing Vulkan DMA-BUF import without a "
                  "selected physical device");
        return false;
    }

    /*
     * This is a strict contract check, not optional debug decoration. NVIDIA can
     * accept VkImage creation for an imported DRM-modifier image even when the
     * exact format/modifier/usage tuple is not a valid DMA-BUF import source for
     * transfer operations. Continuing after that produces black or stale pixels,
     * which hides the real shared-texture failure.
     */
    VkPhysicalDeviceExternalImageFormatInfo external_info {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = nullptr,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external_info,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkPhysicalDeviceImageFormatInfo2 image_info {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifier_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .flags = 0,
    };
    VkExternalImageFormatProperties external_properties {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        .pNext = nullptr,
    };
    VkImageFormatProperties2 image_properties {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };

    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        physical_device,
        &image_info,
        &image_properties);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan dmabuf capability query failed "
                  "%ux%u format=%u modifier=0x%016" G_GINT64_MODIFIER "x "
                  "usage=0x%x role=%s result=%s",
                  width,
                  height,
                  (uint32_t)format,
                  (guint64)modifier,
                  usage,
                  as_blit_target ? "target" : "source",
                  vk_result_name(result));
        return false;
    }

    const VkExternalMemoryProperties& external =
        external_properties.externalMemoryProperties;
    if ((external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0 ||
        (external.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0) {
        g_warning("VividWebProducer: refusing non-importable Vulkan DMA-BUF "
                  "%ux%u format=%u modifier=0x%016" G_GINT64_MODIFIER "x "
                  "usage=0x%x role=%s external-features=%s compatible-handles=0x%x",
                  width,
                  height,
                  (uint32_t)format,
                  (guint64)modifier,
                  usage,
                  as_blit_target ? "target" : "source",
                  vk_external_memory_feature_names(external.externalMemoryFeatures),
                  external.compatibleHandleTypes);
        return false;
    }

    return true;
}

bool
validate_linear_dmabuf_export(VkPhysicalDevice physical_device,
                              VkFormat         format,
                              VkImageUsageFlags usage,
                              bool&            requires_dedicated)
{
    VkPhysicalDeviceExternalImageFormatInfo external_info {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = nullptr,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 image_info {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = usage,
        .flags = 0,
    };
    VkExternalImageFormatProperties external_properties {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        .pNext = nullptr,
    };
    VkImageFormatProperties2 image_properties {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };

    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        physical_device,
        &image_info,
        &image_properties);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan linear DMA-BUF export query failed "
                  "format=%u usage=0x%x result=%s",
                  (uint32_t)format,
                  usage,
                  vk_result_name(result));
        return false;
    }

    const VkExternalMemoryFeatureFlags features =
        external_properties.externalMemoryProperties.externalMemoryFeatures;
    if ((features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
        g_warning("VividWebProducer: refusing non-exportable linear DMA-BUF "
                  "format=%u usage=0x%x external-features=%s",
                  (uint32_t)format,
                  usage,
                  vk_external_memory_feature_names(features));
        return false;
    }

    requires_dedicated =
        (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    return true;
}

std::optional<uint32_t>
find_graphics_queue_family(VkPhysicalDevice gpu)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    if (count > 0)
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());

    for (uint32_t i = 0; i < count; i++) {
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            return i;
    }
    return std::nullopt;
}

VkFormat
fourcc_to_vk_format(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

std::optional<VkPhysicalDevice>
find_physical_device_by_uuid(VkInstance instance,
                             const VividGpuDevice& gpu_device,
                             VkPhysicalDeviceProperties* out_properties)
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0)
        return std::nullopt;

    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (result != VK_SUCCESS)
        return std::nullopt;

    for (VkPhysicalDevice gpu : devices) {
        VkPhysicalDeviceIDProperties id_properties {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            .pNext = nullptr,
        };
        VkPhysicalDeviceProperties2 properties2 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id_properties,
        };
        vkGetPhysicalDeviceProperties2(gpu, &properties2);
        if (memcmp(id_properties.deviceUUID,
                   gpu_device.uuid,
                   VIVID_GPU_DEVICE_UUID_BYTES) == 0) {
            if (out_properties)
                *out_properties = properties2.properties;
            return gpu;
        }
    }

    return std::nullopt;
}

bool
create_probe_instance(VkInstance& instance, const char* app_name)
{
    VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = app_name,
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "VividProducer",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };
    return vkCreateInstance(&instance_info, nullptr, &instance) == VK_SUCCESS;
}

} // namespace

VividWebVulkanImage::VividWebVulkanImage(VividWebVulkanImage&& other) noexcept
{
    *this = std::move(other);
}

VividWebVulkanImage&
VividWebVulkanImage::operator=(VividWebVulkanImage&& other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    device = std::exchange(other.device, VK_NULL_HANDLE);
    image = std::exchange(other.image, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    format = std::exchange(other.format, VK_FORMAT_UNDEFINED);
    width = std::exchange(other.width, 0);
    height = std::exchange(other.height, 0);
    stride = std::exchange(other.stride, 0);
    offset = std::exchange(other.offset, 0);
    size = std::exchange(other.size, 0);
    modifier = std::exchange(other.modifier, 0);
    fd = std::exchange(other.fd, -1);
    n_planes = std::exchange(other.n_planes, 0);
    plane_fds = other.plane_fds;
    plane_strides = other.plane_strides;
    plane_offsets = other.plane_offsets;
    other.plane_fds.fill(-1);
    other.plane_strides = {};
    other.plane_offsets = {};
    initialized = std::exchange(other.initialized, false);
    return *this;
}

VividWebVulkanImage::~VividWebVulkanImage()
{
    reset();
}

void
VividWebVulkanImage::reset()
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    for (uint32_t plane = 1; plane < n_planes && plane < plane_fds.size(); plane++) {
        if (plane_fds[plane] >= 0)
            close(plane_fds[plane]);
    }
    if (device != VK_NULL_HANDLE) {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }
    device = VK_NULL_HANDLE;
    format = VK_FORMAT_UNDEFINED;
    width = 0;
    height = 0;
    stride = 0;
    offset = 0;
    size = 0;
    modifier = 0;
    n_planes = 0;
    plane_fds.fill(-1);
    plane_strides = {};
    plane_offsets = {};
    initialized = false;
}

VividWebVulkanRoute::~VividWebVulkanRoute()
{
    reset();
}

void
VividWebVulkanRoute::reset()
{
    if (device != VK_NULL_HANDLE)
        (void)vkDeviceWaitIdle(device);
    if (command_pool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
        command_buffer = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    physical_device = VK_NULL_HANDLE;
    graphics_queue = VK_NULL_HANDLE;
    graphics_queue_family = 0;
    get_memory_fd = nullptr;
    get_image_drm_format_modifier_properties = nullptr;
    export_requires_dedicated = false;
    export_forbids_device_local_memory = false;
    memory_properties = {};
    device_name.clear();
}

void
VividWebVulkanRoute::abandon_for_process_lifetime()
{
    /*
     * Runtime GPU switching happens while CEF keeps browser, launcher, and GPU
     * recovery threads alive. On NVIDIA the Vulkan loader may dlclose the ICD
     * stack from vkDestroyInstance(); in the same process Chromium can
     * concurrently fork/exec a replacement helper. That combination has been
     * observed to crash inside libnvidia-glcore during the switch. The web
     * backend already destroys the imported dmabuf images before retiring a
     * route, so the remaining instance/device/command-pool objects are small
     * process-lifetime driver allocations. Intentionally drop Vivid's handles
     * here and let the OS reclaim them at process exit instead of running the
     * unsafe loader teardown path during an interactive device change.
     */
    instance = VK_NULL_HANDLE;
    physical_device = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    graphics_queue = VK_NULL_HANDLE;
    graphics_queue_family = 0;
    command_pool = VK_NULL_HANDLE;
    command_buffer = VK_NULL_HANDLE;
    get_memory_fd = nullptr;
    get_image_drm_format_modifier_properties = nullptr;
    export_requires_dedicated = false;
    export_forbids_device_local_memory = false;
    memory_properties = {};
    device_name.clear();
}

bool
VividWebVulkanRoute::ensure(const VividGpuDevice& gpu_device)
{
    reset();

    VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "Vivid web producer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "VividWebProducer",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };
    VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to create Vulkan instance result=%s",
                  vk_result_name(result));
        return false;
    }

    VkPhysicalDeviceProperties selected_properties {};
    const auto selected_device =
        find_physical_device_by_uuid(instance, gpu_device, &selected_properties);
    if (!selected_device.has_value()) {
        g_warning("VividWebProducer: resolved GPU %s (%s) was not found by Vulkan deviceUUID",
                  gpu_device.name,
                  gpu_device.render_node[0] ? gpu_device.render_node : "unknown-node");
        reset();
        return false;
    }
    VkPhysicalDevice selected = selected_device.value();

    const auto queue_family = find_graphics_queue_family(selected);
    if (!queue_family.has_value()) {
        g_warning("VividWebProducer: selected Vulkan GPU %s has no graphics queue",
                  selected_properties.deviceName);
        reset();
        return false;
    }

    const auto extensions = enumerate_device_extensions(selected);
    for (const char* extension : kRequiredDeviceExtensions) {
        if (!has_extension(extensions, extension)) {
            g_message("VividWebProducer: GPU %s is missing %s; accelerated CEF "
                      "frames are unavailable",
                      selected_properties.deviceName,
                      extension);
            reset();
            return false;
        }
    }

    bool requires_dedicated = false;
    if (!validate_linear_dmabuf_export(selected,
                                       VK_FORMAT_B8G8R8A8_UNORM,
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       requires_dedicated)) {
        reset();
        return false;
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = queue_family.value(),
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    std::vector<const char*> enabled_extensions(kRequiredDeviceExtensions.begin(),
                                                kRequiredDeviceExtensions.end());
    if (has_extension(extensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
        enabled_extensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    if (has_extension(extensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
        enabled_extensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    VkDeviceCreateInfo device_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
        .pEnabledFeatures = nullptr,
    };
    result = vkCreateDevice(selected, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to create Vulkan device gpu=%s result=%s",
                  selected_properties.deviceName,
                  vk_result_name(result));
        reset();
        return false;
    }

    physical_device = selected;
    graphics_queue_family = queue_family.value();
    device_name = selected_properties.deviceName;
    export_requires_dedicated = requires_dedicated;
    export_forbids_device_local_memory =
        selected_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    vkGetPhysicalDeviceMemoryProperties(selected, &memory_properties);
    vkGetDeviceQueue(device, graphics_queue_family, 0, &graphics_queue);
    get_memory_fd =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device,
                                                                    "vkGetMemoryFdKHR"));
    get_image_drm_format_modifier_properties =
        reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (!graphics_queue || !get_memory_fd || !get_image_drm_format_modifier_properties) {
        g_warning("VividWebProducer: Vulkan device gpu=%s missing queue/getMemoryFd/"
                  "getImageDrmFormatModifierProperties",
                  device_name.c_str());
        reset();
        return false;
    }

    VkCommandPoolCreateInfo pool_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = graphics_queue_family,
    };
    result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to create Vulkan command pool result=%s",
                  vk_result_name(result));
        reset();
        return false;
    }

    VkCommandBufferAllocateInfo command_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = vkAllocateCommandBuffers(device, &command_info, &command_buffer);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to allocate Vulkan command buffer result=%s",
                  vk_result_name(result));
        reset();
        return false;
    }

    g_message("VividWebProducer: Vulkan blit/export route ready on gpu=%s node=%s "
              "dedicated-export=%s forbid-device-local-export=%s",
              device_name.c_str(),
              gpu_device.render_node[0] ? gpu_device.render_node : "(unknown)",
              bool_to_string(export_requires_dedicated),
              bool_to_string(export_forbids_device_local_memory));
    return true;
}

std::vector<VividWebVulkanFormatCap>
VividWebVulkanRoute::query_export_caps(const VividGpuDevice& gpu_device,
                                       VkImageUsageFlags     usage)
{
    std::vector<VividWebVulkanFormatCap> caps;
    if (!gpu_device.render_node[0])
        return caps;

    VkInstance probe_instance = VK_NULL_HANDLE;
    if (!create_probe_instance(probe_instance, "Vivid web producer caps"))
        return caps;

    VkPhysicalDeviceProperties properties {};
    const auto selected_device =
        find_physical_device_by_uuid(probe_instance, gpu_device, &properties);
    if (!selected_device.has_value()) {
        vkDestroyInstance(probe_instance, nullptr);
        return caps;
    }

    VkPhysicalDevice selected = selected_device.value();
    const auto extensions = enumerate_device_extensions(selected);
    for (const char* extension : kRequiredDeviceExtensions) {
        if (!has_extension(extensions, extension)) {
            vkDestroyInstance(probe_instance, nullptr);
            return caps;
        }
    }

    const VkFormat vk_format = fourcc_to_vk_format(DRM_FORMAT_XRGB8888);
    const VkFormatFeatureFlags want_features =
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;

    VkDrmFormatModifierPropertiesListEXT modifier_list {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        .pNext = nullptr,
        .drmFormatModifierCount = 0,
        .pDrmFormatModifierProperties = nullptr,
    };
    VkFormatProperties2 format_properties {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifier_list,
        .formatProperties = {},
    };
    vkGetPhysicalDeviceFormatProperties2(selected, vk_format, &format_properties);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(
        modifier_list.drmFormatModifierCount);
    if (!modifiers.empty()) {
        modifier_list.pDrmFormatModifierProperties = modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(selected, vk_format, &format_properties);
    }

    /*
     * Mirror waywallen's producer-side cap probe: advertise the exact
     * (fourcc, modifier, plane_count) tuples reported by Vulkan after filtering
     * for the usages this export ring needs. The allocator later reads back each
     * plane layout and duplicates the dma-buf fd for multi-plane modifier
     * metadata.
     */
    for (const auto& modifier : modifiers) {
        if (modifier.drmFormatModifier == DRM_FORMAT_MOD_LINEAR ||
            modifier.drmFormatModifierPlaneCount == 0 ||
            modifier.drmFormatModifierPlaneCount > VividWebVulkanImage::kMaxPlanes ||
            (modifier.drmFormatModifierTilingFeatures & want_features) != want_features) {
            continue;
        }
        caps.push_back(VividWebVulkanFormatCap {
            .fourcc = DRM_FORMAT_XRGB8888,
            .modifier = modifier.drmFormatModifier,
            .plane_count = modifier.drmFormatModifierPlaneCount,
        });
    }

    bool requires_dedicated = false;
    if (validate_linear_dmabuf_export(selected, vk_format, usage, requires_dedicated)) {
        caps.push_back(VividWebVulkanFormatCap {
            .fourcc = DRM_FORMAT_XRGB8888,
            .modifier = DRM_FORMAT_MOD_LINEAR,
            .plane_count = 1,
        });
    }

    g_message("VividWebProducer: probed Vulkan export caps gpu=%s node=%s caps=%zu",
              properties.deviceName,
              gpu_device.render_node,
              caps.size());
    vkDestroyInstance(probe_instance, nullptr);
    return caps;
}

std::optional<VividWebVulkanImage>
VividWebVulkanRoute::create_export_image(uint32_t width,
                                          uint32_t height,
                                          VkFormat format,
                                          const VividWebVulkanExportRequest& request)
{
    if (device == VK_NULL_HANDLE || get_memory_fd == nullptr || width == 0 || height == 0)
        return std::nullopt;

    const bool modifier_path = request.require_modifier &&
        request.modifier != DRM_FORMAT_MOD_LINEAR &&
        request.modifier != DRM_FORMAT_MOD_INVALID;
    VividWebVulkanImage image;
    image.device = device;
    image.format = format;
    image.width = width;
    image.height = height;
    image.modifier = modifier_path ? request.modifier : DRM_FORMAT_MOD_LINEAR;

    uint64_t requested_modifier = image.modifier;
    VkImageDrmFormatModifierListCreateInfoEXT modifier_list {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &requested_modifier,
    };
    VkExternalMemoryImageCreateInfo external_image {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = modifier_path ? static_cast<void*>(&modifier_list) : nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = VkExtent3D { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = modifier_path
            ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
            : VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(device, &image_info, nullptr, &image.image);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to create export image %ux%u result=%s",
                  width,
                  height,
                  vk_result_name(result));
        return std::nullopt;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    const bool prefer_device_local =
        modifier_path || request.memory == VividWebVulkanExportMemory::DeviceLocal;
    const VkMemoryPropertyFlags required_memory_flags = prefer_device_local
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkMemoryPropertyFlags forbidden_memory_flags =
        (!prefer_device_local && export_forbids_device_local_memory)
            ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            : 0;
    const auto memory_type =
        find_memory_type_exact(memory_properties,
                               requirements.memoryTypeBits,
                               required_memory_flags,
                               forbidden_memory_flags);
    if (!memory_type.has_value()) {
        g_warning("VividWebProducer: no suitable export memory type for web ring "
                  "image %ux%u modifier=0x%016" G_GINT64_MODIFIER "x "
                  "memoryTypeBits=0x%x required=0x%x forbidden=0x%x gpu=%s",
                  width,
                  height,
                  static_cast<guint64>(image.modifier),
                  requirements.memoryTypeBits,
                  required_memory_flags,
                  forbidden_memory_flags,
                  device_name.c_str());
        return std::nullopt;
    }

    VkMemoryDedicatedAllocateInfo dedicated_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = image.image,
        .buffer = VK_NULL_HANDLE,
    };
    VkExportMemoryAllocateInfo export_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = export_requires_dedicated
            ? static_cast<void*>(&dedicated_info)
            : nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo allocate_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type.value(),
    };
    result = vkAllocateMemory(device, &allocate_info, nullptr, &image.memory);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to allocate export image memory result=%s",
                  vk_result_name(result));
        return std::nullopt;
    }

    result = vkBindImageMemory(device, image.image, image.memory, 0);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to bind export image memory result=%s",
                  vk_result_name(result));
        return std::nullopt;
    }

    VkMemoryGetFdInfoKHR fd_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = image.memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    result = get_memory_fd(device, &fd_info, &image.fd);
    if (result != VK_SUCCESS || image.fd < 0) {
        g_warning("VividWebProducer: failed to export image DMA-BUF fd result=%s fd=%d",
                  vk_result_name(result),
                  image.fd);
        return std::nullopt;
    }

    if (modifier_path && get_image_drm_format_modifier_properties) {
        VkImageDrmFormatModifierPropertiesEXT modifier_properties {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
            .pNext = nullptr,
            .drmFormatModifier = 0,
        };
        const VkResult modifier_result =
            get_image_drm_format_modifier_properties(device,
                                                     image.image,
                                                     &modifier_properties);
        if (modifier_result != VK_SUCCESS ||
            modifier_properties.drmFormatModifier != request.modifier) {
            g_warning("VividWebProducer: export image modifier mismatch requested=0x%016"
                      G_GINT64_MODIFIER "x actual=0x%016" G_GINT64_MODIFIER
                      "x result=%s",
                      static_cast<guint64>(request.modifier),
                      static_cast<guint64>(modifier_properties.drmFormatModifier),
                      vk_result_name(modifier_result));
            return std::nullopt;
        }
        image.modifier = modifier_properties.drmFormatModifier;
    }

    const std::array<VkImageAspectFlagBits, VividWebVulkanImage::kMaxPlanes> plane_aspects {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
    };
    const uint32_t plane_count = modifier_path ? request.plane_count : 1u;
    if (plane_count == 0 || plane_count > VividWebVulkanImage::kMaxPlanes) {
        g_warning("VividWebProducer: export image requested invalid plane count=%u",
                  plane_count);
        return std::nullopt;
    }

    image.n_planes = plane_count;
    image.size = static_cast<uint64_t>(requirements.size);
    for (uint32_t plane = 0; plane < plane_count; plane++) {
        const VkImageSubresource subresource {
            .aspectMask = modifier_path ? plane_aspects[plane] : VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .arrayLayer = 0,
        };
        VkSubresourceLayout actual_layout {};
        vkGetImageSubresourceLayout(device, image.image, &subresource, &actual_layout);
        if (actual_layout.rowPitch > G_MAXUINT32 || actual_layout.offset > G_MAXUINT32) {
            g_warning("VividWebProducer: export image plane=%u layout is too large "
                      "rowPitch=%llu offset=%llu",
                      plane,
                      static_cast<unsigned long long>(actual_layout.rowPitch),
                      static_cast<unsigned long long>(actual_layout.offset));
            return std::nullopt;
        }

        image.plane_strides[plane] = static_cast<uint32_t>(actual_layout.rowPitch);
        image.plane_offsets[plane] = static_cast<uint32_t>(actual_layout.offset);
        image.plane_fds[plane] = plane == 0 ? image.fd : fcntl(image.fd, F_DUPFD_CLOEXEC, 3);
        if (image.plane_fds[plane] < 0) {
            g_warning("VividWebProducer: failed to duplicate export plane fd plane=%u: %s",
                      plane,
                      g_strerror(errno));
            return std::nullopt;
        }
    }

    image.offset = image.plane_offsets[0];
    image.stride = image.plane_strides[0];
    if (!modifier_path && image.stride < width * 4u) {
        g_warning("VividWebProducer: export image has invalid stride=%u width=%u",
                  image.stride,
                  width);
        return std::nullopt;
    }

    return image;
}

std::optional<VividWebVulkanImage>
VividWebVulkanRoute::import_dmabuf_image(uint32_t width,
                                          uint32_t height,
                                          VkFormat format,
                                          uint64_t modifier,
                                          int      fd,
                                          uint64_t object_size,
                                          uint32_t plane_offset,
                                          uint32_t plane_stride,
                                          bool     as_blit_target)
{
    if (device == VK_NULL_HANDLE || width == 0 || height == 0 || fd < 0 ||
        plane_stride == 0)
        return std::nullopt;
    if (modifier == DRM_FORMAT_MOD_INVALID)
        modifier = DRM_FORMAT_MOD_LINEAR;

    VividWebVulkanImage result_image;
    result_image.device = device;
    result_image.format = format;
    result_image.width = width;
    result_image.height = height;

    VkExternalMemoryImageCreateInfo external_image {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkSubresourceLayout plane_layout {
        .offset = static_cast<VkDeviceSize>(plane_offset),
        .size = static_cast<VkDeviceSize>(plane_stride) * height,
        .rowPitch = static_cast<VkDeviceSize>(plane_stride),
        .arrayPitch = 0,
        .depthPitch = 0,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = &external_image,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };
    const VkImageUsageFlags usage = static_cast<VkImageUsageFlags>(as_blit_target
        ? VK_IMAGE_USAGE_TRANSFER_DST_BIT
        : VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!validate_dmabuf_image_capabilities(physical_device,
                                            width,
                                            height,
                                            format,
                                            modifier,
                                            usage,
                                            as_blit_target)) {
        return std::nullopt;
    }

    VkImageCreateInfo image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = VkExtent3D { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vk_result = vkCreateImage(device, &image_info, nullptr, &result_image.image);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to create imported dmabuf image "
                  "%ux%u stride=%u modifier=0x%016" G_GINT64_MODIFIER "x result=%s",
                  width,
                  height,
                  plane_stride,
                  modifier,
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, result_image.image, &requirements);
    const auto memory_type = find_memory_type(memory_properties, requirements.memoryTypeBits);
    if (!memory_type.has_value()) {
        g_warning("VividWebProducer: no Vulkan memory type accepts the dmabuf import");
        return std::nullopt;
    }

    const int import_fd = dup(fd);
    if (import_fd < 0) {
        g_warning("VividWebProducer: failed to duplicate dmabuf fd: %s", g_strerror(errno));
        return std::nullopt;
    }

    VkMemoryDedicatedAllocateInfo dedicated_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = result_image.image,
        .buffer = VK_NULL_HANDLE,
    };
    VkImportMemoryFdInfoKHR import_info {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd,
    };
    VkMemoryAllocateInfo allocate_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = std::max(requirements.size, static_cast<VkDeviceSize>(object_size)),
        .memoryTypeIndex = memory_type.value(),
    };
    vk_result = vkAllocateMemory(device, &allocate_info, nullptr, &result_image.memory);
    if (vk_result != VK_SUCCESS) {
        close(import_fd);
        g_warning("VividWebProducer: failed to import dmabuf memory result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    vk_result = vkBindImageMemory(device, result_image.image, result_image.memory, 0);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividWebProducer: failed to bind imported dmabuf image result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    return result_image;
}

bool
VividWebVulkanRoute::blit_image(const VividWebVulkanImage& source,
                                 int32_t  src_x,
                                 int32_t  src_y,
                                 uint32_t src_width,
                                 uint32_t src_height,
                                 VividWebVulkanImage& target)
{
    if (device == VK_NULL_HANDLE || command_buffer == VK_NULL_HANDLE || !source || !target) {
        g_warning("VividWebProducer: Vulkan blit skipped because route/source/target "
                  "is unavailable device=%p command-buffer=%p source=%p target=%p",
                  (void*)device,
                  (void*)command_buffer,
                  (void*)source.image,
                  (void*)target.image);
        return false;
    }

    src_x = std::clamp(src_x, 0, static_cast<int32_t>(source.width) - 1);
    src_y = std::clamp(src_y, 0, static_cast<int32_t>(source.height) - 1);
    src_width = std::clamp(src_width,
                           1u,
                           source.width - static_cast<uint32_t>(src_x));
    src_height = std::clamp(src_height,
                            1u,
                            source.height - static_cast<uint32_t>(src_y));

    VkResult result = vkResetCommandBuffer(command_buffer, 0);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan command buffer reset failed result=%s",
                  vk_result_name(result));
        return false;
    }

    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan command buffer begin failed result=%s",
                  vk_result_name(result));
        return false;
    }

    const VkImageSubresourceRange color_range {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    VkImageMemoryBarrier source_to_transfer {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        /*
         * The source image is a fresh Vulkan wrapper around a DMA-BUF that CEF
         * has already rendered into outside this VkDevice. Treating it as
         * UNDEFINED lets the driver discard those external contents during the
         * layout transition, which can produce a perfectly successful black
         * blit. Acquire ownership from EXTERNAL and preserve the external
         * GENERAL layout so the browser pixels survive into TRANSFER_SRC.
         */
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = graphics_queue_family,
        .image = source.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &source_to_transfer);

    /*
     * Ring slots are read by the display consumer between writes; ownership
     * mirrors the video backend: acquire from EXTERNAL once the slot has been
     * published before, write, then release back to EXTERNAL before the
     * FRAME_READY index leaves the backend.
     */
    VkImageMemoryBarrier acquire {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = target.initialized
            ? static_cast<VkAccessFlags>(VK_ACCESS_MEMORY_READ_BIT)
            : static_cast<VkAccessFlags>(0),
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = target.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = target.initialized
            ? VK_QUEUE_FAMILY_EXTERNAL
            : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = target.initialized
            ? graphics_queue_family
            : VK_QUEUE_FAMILY_IGNORED,
        .image = target.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(command_buffer,
                         target.initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &acquire);

    const bool can_copy_without_scaling = source.format == target.format &&
        src_x == 0 &&
        src_y == 0 &&
        src_width == target.width &&
        src_height == target.height;
    if (can_copy_without_scaling) {
        /*
         * CEF normally hands us a full-size BGRA dmabuf for the exact output
         * size. Prefer vkCmdCopyImage for that common case: DRM modifier
         * images are guaranteed here for transfer usage, while BLIT feature
         * support is format/modifier dependent and NVIDIA can otherwise
         * accept the command stream but leave an all-zero target.
         */
        VkImageCopy copy {
            .srcSubresource =
                VkImageSubresourceLayers {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .srcOffset = VkOffset3D { 0, 0, 0 },
            .dstSubresource =
                VkImageSubresourceLayers {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .dstOffset = VkOffset3D { 0, 0, 0 },
            .extent = VkExtent3D { target.width, target.height, 1 },
        };
        vkCmdCopyImage(command_buffer,
                       source.image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &copy);
    } else {
        VkImageBlit blit {
            .srcSubresource =
                VkImageSubresourceLayers {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .srcOffsets = {
                VkOffset3D { src_x, src_y, 0 },
                VkOffset3D { src_x + static_cast<int32_t>(src_width),
                             src_y + static_cast<int32_t>(src_height),
                             1 },
            },
            .dstSubresource =
                VkImageSubresourceLayers {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .dstOffsets = {
                VkOffset3D { 0, 0, 0 },
                VkOffset3D { static_cast<int32_t>(target.width),
                             static_cast<int32_t>(target.height),
                             1 },
            },
        };
        vkCmdBlitImage(command_buffer,
                       source.image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &blit,
                       VK_FILTER_NEAREST);
    }

    VkImageMemoryBarrier release_source {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = graphics_queue_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = source.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &release_source);

    VkImageMemoryBarrier release {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = graphics_queue_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = target.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &release);

    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan command buffer end failed result=%s",
                  vk_result_name(result));
        return false;
    }

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    result = vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan blit submit failed result=%s",
                  vk_result_name(result));
        return false;
    }
    /*
     * The CEF buffer behind `source` is returned to its pool the moment
     * OnAcceleratedPaint returns, so the copy must be complete before this
     * function does.
     */
    result = vkQueueWaitIdle(graphics_queue);
    if (result != VK_SUCCESS) {
        g_warning("VividWebProducer: Vulkan blit wait failed result=%s",
                  vk_result_name(result));
        return false;
    }

    target.initialized = true;
    return true;
}
