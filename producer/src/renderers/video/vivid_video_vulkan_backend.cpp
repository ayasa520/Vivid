/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_video_vulkan_backend.hpp"

#include <gst/allocators/gstdmabuf.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video-info-dma.h>
#include <gst/video/video.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
gboolean gst_cuda_context_push(GstCudaContext* ctx);
gboolean gst_cuda_context_pop(CUcontext* cuda_ctx);

CUresult CUDAAPI CuGetErrorName(CUresult error, const char** pStr);
CUresult CUDAAPI CuImportExternalMemory(CUexternalMemory* extMem_out,
                                        const CUDA_EXTERNAL_MEMORY_HANDLE_DESC* memHandleDesc);
CUresult CUDAAPI CuDestroyExternalMemory(CUexternalMemory extMem);
CUresult CUDAAPI CuExternalMemoryGetMappedBuffer(CUdeviceptr* devPtr,
                                                 CUexternalMemory extMem,
                                                 const CUDA_EXTERNAL_MEMORY_BUFFER_DESC* bufferDesc);
}

namespace
{

constexpr std::array kRequiredDeviceExtensions {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
};

constexpr std::array kVaImportDeviceExtensions {
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

const char*
bool_to_string(bool value)
{
    return value ? "true" : "false";
}

const char*
cuda_error_name(CUresult result)
{
    const char* name = nullptr;
    if (CuGetErrorName(result, &name) == CUDA_SUCCESS && name != nullptr)
        return name;
    return "unknown";
}

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
    default: return "VK_RESULT_UNKNOWN";
    }
}

VkFormat
target_vk_format_for_transfer_path(VideoFrameTransferPath)
{
    return VK_FORMAT_R8G8B8A8_UNORM;
}

guint32
target_drm_fourcc_for_transfer_path(VideoFrameTransferPath)
{
    return DRM_FORMAT_ABGR8888;
}

struct VideoBlitRegions
{
    VkOffset3D src0 { 0, 0, 0 };
    VkOffset3D src1 { 1, 1, 1 };
    VkOffset3D dst0 { 0, 0, 0 };
    VkOffset3D dst1 { 1, 1, 1 };
    uint32_t draw_width { 1 };
    uint32_t draw_height { 1 };
};

uint32_t
clamped_round_to_u32(double value, uint32_t minimum, uint32_t maximum)
{
    if (!std::isfinite(value))
        return minimum;
    value = std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum));
    return static_cast<uint32_t>(std::llround(value));
}

VideoBlitRegions
compute_video_blit_regions(VideoFillMode mode,
                           uint32_t src_width,
                           uint32_t src_height,
                           uint32_t dst_width,
                           uint32_t dst_height)
{
    src_width = std::max(src_width, 1u);
    src_height = std::max(src_height, 1u);
    dst_width = std::max(dst_width, 1u);
    dst_height = std::max(dst_height, 1u);

    VideoBlitRegions regions;
    regions.src0 = VkOffset3D { 0, 0, 0 };
    regions.src1 =
        VkOffset3D { static_cast<int32_t>(src_width), static_cast<int32_t>(src_height), 1 };
    regions.dst0 = VkOffset3D { 0, 0, 0 };
    regions.dst1 =
        VkOffset3D { static_cast<int32_t>(dst_width), static_cast<int32_t>(dst_height), 1 };
    regions.draw_width = dst_width;
    regions.draw_height = dst_height;

    const double src_aspect =
        static_cast<double>(src_width) / static_cast<double>(src_height);
    const double dst_aspect =
        static_cast<double>(dst_width) / static_cast<double>(dst_height);

    if (mode == VideoFillMode::Fill) {
        double scale = std::min(static_cast<double>(dst_width) / static_cast<double>(src_width),
                                static_cast<double>(dst_height) / static_cast<double>(src_height));
        regions.draw_width =
            clamped_round_to_u32(static_cast<double>(src_width) * scale, 1, dst_width);
        regions.draw_height =
            clamped_round_to_u32(static_cast<double>(src_height) * scale, 1, dst_height);
        const uint32_t dst_x = (dst_width - regions.draw_width) / 2u;
        const uint32_t dst_y = (dst_height - regions.draw_height) / 2u;
        regions.dst0 = VkOffset3D { static_cast<int32_t>(dst_x),
                                    static_cast<int32_t>(dst_y),
                                    0 };
        regions.dst1 =
            VkOffset3D { static_cast<int32_t>(dst_x + regions.draw_width),
                         static_cast<int32_t>(dst_y + regions.draw_height),
                         1 };
    } else if (mode == VideoFillMode::Cover) {
        double src_x = 0.0;
        double src_y = 0.0;
        double sample_width = static_cast<double>(src_width);
        double sample_height = static_cast<double>(src_height);
        if (src_aspect > dst_aspect) {
            sample_width = std::clamp(static_cast<double>(src_height) * dst_aspect,
                                      1.0,
                                      static_cast<double>(src_width));
            src_x = (static_cast<double>(src_width) - sample_width) * 0.5;
        } else {
            sample_height = std::clamp(static_cast<double>(src_width) / dst_aspect,
                                       1.0,
                                       static_cast<double>(src_height));
            src_y = (static_cast<double>(src_height) - sample_height) * 0.5;
        }
        const uint32_t src_x0 =
            clamped_round_to_u32(std::floor(src_x), 0, src_width - 1u);
        const uint32_t src_y0 =
            clamped_round_to_u32(std::floor(src_y), 0, src_height - 1u);
        const uint32_t src_x1 =
            clamped_round_to_u32(std::ceil(src_x + sample_width), src_x0 + 1u, src_width);
        const uint32_t src_y1 =
            clamped_round_to_u32(std::ceil(src_y + sample_height), src_y0 + 1u, src_height);
        regions.src0 =
            VkOffset3D { static_cast<int32_t>(src_x0), static_cast<int32_t>(src_y0), 0 };
        regions.src1 =
            VkOffset3D { static_cast<int32_t>(src_x1), static_cast<int32_t>(src_y1), 1 };
    }
    return regions;
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
enumerate_instance_extensions()
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkExtensionProperties> result(count);
    if (count > 0 &&
        vkEnumerateInstanceExtensionProperties(nullptr, &count, result.data()) != VK_SUCCESS)
        return {};
    return result;
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
                 uint32_t memory_type_bits,
                 VkMemoryPropertyFlags preferred_flags)
{
    if (preferred_flags != 0) {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
            if ((memory_type_bits & (1u << i)) == 0)
                continue;
            if ((memory_properties.memoryTypes[i].propertyFlags & preferred_flags) == preferred_flags)
                return i;
        }
    }

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

VkFormat
fourcc_to_vk_format(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
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

VividVideoVulkanExportImage::VividVideoVulkanExportImage(
    VividVideoVulkanExportImage&& other) noexcept
{
    *this = std::move(other);
}

VividVideoVulkanExportImage&
VividVideoVulkanExportImage::operator=(VividVideoVulkanExportImage&& other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    device = std::exchange(other.device, VK_NULL_HANDLE);
    image = std::exchange(other.image, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    size = std::exchange(other.size, 0);
    index = std::exchange(other.index, 0);
    width = std::exchange(other.width, 0);
    height = std::exchange(other.height, 0);
    stride = std::exchange(other.stride, 0);
    offset = std::exchange(other.offset, 0);
    modifier = std::exchange(other.modifier, DRM_FORMAT_MOD_LINEAR);
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

VividVideoVulkanExportImage::~VividVideoVulkanExportImage()
{
    reset();
}

VividVideoVulkanExportImage::operator bool() const
{
    return image != VK_NULL_HANDLE && fd >= 0;
}

void
VividVideoVulkanExportImage::reset()
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
    size = 0;
    width = 0;
    height = 0;
    stride = 0;
    offset = 0;
    modifier = DRM_FORMAT_MOD_LINEAR;
    n_planes = 0;
    plane_fds.fill(-1);
    plane_strides = {};
    plane_offsets = {};
    initialized = false;
}

VividVideoCudaExternalBuffer::VividVideoCudaExternalBuffer(
    VividVideoCudaExternalBuffer&& other) noexcept
{
    *this = std::move(other);
}

VividVideoCudaExternalBuffer&
VividVideoCudaExternalBuffer::operator=(VividVideoCudaExternalBuffer&& other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    device = std::exchange(other.device, VK_NULL_HANDLE);
    buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    size = std::exchange(other.size, 0);
    fd = std::exchange(other.fd, -1);
    cuda_memory = std::exchange(other.cuda_memory, nullptr);
    cuda_ptr = std::exchange(other.cuda_ptr, 0);
    cuda_context = std::exchange(other.cuda_context, nullptr);
    return *this;
}

VividVideoCudaExternalBuffer::~VividVideoCudaExternalBuffer()
{
    reset();
}

VividVideoCudaExternalBuffer::operator bool() const
{
    return buffer != VK_NULL_HANDLE && cuda_ptr != 0;
}

void
VividVideoCudaExternalBuffer::reset()
{
    if (cuda_memory) {
        bool pushed = false;
        if (cuda_context)
            pushed = gst_cuda_context_push(cuda_context);
        (void)CuDestroyExternalMemory(cuda_memory);
        if (pushed) {
            CUcontext popped {};
            (void)gst_cuda_context_pop(&popped);
        }
        cuda_memory = nullptr;
        cuda_ptr = 0;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    if (device != VK_NULL_HANDLE) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }
    if (cuda_context) {
        gst_object_unref(cuda_context);
        cuda_context = nullptr;
    }
    device = VK_NULL_HANDLE;
    size = 0;
}

VividVideoVulkanImportedImage::VividVideoVulkanImportedImage(
    VividVideoVulkanImportedImage&& other) noexcept
{
    *this = std::move(other);
}

VividVideoVulkanImportedImage&
VividVideoVulkanImportedImage::operator=(VividVideoVulkanImportedImage&& other) noexcept
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
    return *this;
}

VividVideoVulkanImportedImage::~VividVideoVulkanImportedImage()
{
    reset();
}

VividVideoVulkanImportedImage::operator bool() const
{
    return image != VK_NULL_HANDLE;
}

void
VividVideoVulkanImportedImage::reset()
{
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
}

VividVideoVulkanBackend::~VividVideoVulkanBackend()
{
    reset();
}

void
VividVideoVulkanBackend::reset()
{
    if (device != VK_NULL_HANDLE)
        (void)vkDeviceWaitIdle(device);
    for (auto& image : images)
        image.reset();
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
    memory_properties = {};
    target_format = VK_FORMAT_R8G8B8A8_UNORM;
    target_fourcc = DRM_FORMAT_ABGR8888;
    target_modifier = DRM_FORMAT_MOD_LINEAR;
    presented_index = 0;
    ready_index = 1;
    in_progress_index = 2;
    dirty = false;
    export_requires_dedicated = false;
    export_forbids_device_local_memory = false;
    device_name.clear();
}

VividVideoVulkanExportImage&
VividVideoVulkanBackend::in_progress_image()
{
    return images[in_progress_index];
}

void
VividVideoVulkanBackend::mark_frame_ready()
{
    std::swap(in_progress_index, ready_index);
    dirty = true;
}

VividVideoVulkanExportImage*
VividVideoVulkanBackend::eat_frame()
{
    if (!dirty)
        return nullptr;
    std::swap(presented_index, ready_index);
    dirty = false;
    return &images[presented_index];
}

namespace
{

bool
create_vulkan_instance(VividVideoVulkanBackend& backend)
{
    const auto instance_extensions = enumerate_instance_extensions();
    std::vector<const char*> enabled_extensions;
    if (has_extension(instance_extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        enabled_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "Vivid direct video",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "VividVideoProducer",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
    };

    VkResult result = vkCreateInstance(&create_info, nullptr, &backend.instance);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to create Vulkan instance result=%s",
                  vk_result_name(result));
        return false;
    }
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

bool
device_supports_required_extensions(VkPhysicalDevice gpu,
                                    const std::vector<VkExtensionProperties>& extensions,
                                    const char** missing_extension)
{
    for (const char* extension : kRequiredDeviceExtensions) {
        if (!has_extension(extensions, extension)) {
            if (missing_extension)
                *missing_extension = extension;
            return false;
        }
    }
    return true;
}

bool
device_supports_va_import_extensions(const std::vector<VkExtensionProperties>& extensions,
                                     const char** missing_extension)
{
    for (const char* extension : kVaImportDeviceExtensions) {
        if (!has_extension(extensions, extension)) {
            if (missing_extension)
                *missing_extension = extension;
            return false;
        }
    }
    return true;
}

bool
device_supports_linear_export(VkPhysicalDevice gpu,
                              VkFormat format,
                              bool& requires_dedicated,
                              std::string& reason)
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
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
    VkResult result =
        vkGetPhysicalDeviceImageFormatProperties2(gpu, &image_info, &image_properties);
    if (result != VK_SUCCESS) {
        reason = std::string("vkGetPhysicalDeviceImageFormatProperties2=") +
            vk_result_name(result);
        return false;
    }

    const VkExternalMemoryFeatureFlags features =
        external_properties.externalMemoryProperties.externalMemoryFeatures;
    if ((features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
        reason = "linear RGBA image is not DMA-BUF exportable";
        return false;
    }

    requires_dedicated =
        (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    return true;
}

bool
choose_physical_device(VividVideoVulkanBackend& backend,
                       const VividGpuDevice&    gpu_device,
                       VideoFrameTransferPath    transfer_path)
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(backend.instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        g_warning("VividVideoProducer: no Vulkan physical devices result=%s",
                  vk_result_name(result));
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(backend.instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to enumerate Vulkan physical devices result=%s",
                  vk_result_name(result));
        return false;
    }

    /*
     * The render-device decision was already made by the shared resolver; this
     * function only locates that exact card by deviceUUID and validates that
     * it can actually run the selected transfer path. Failing loudly is better than
     * silently decoding on a different card than the one the user picked.
     */
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected_properties {};
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
            selected = gpu;
            selected_properties = properties2.properties;
            break;
        }
    }

    if (selected == VK_NULL_HANDLE) {
        g_warning("VividVideoProducer: resolved GPU %s (%s) was not found by "
                  "Vulkan deviceUUID",
                  gpu_device.name,
                  gpu_device.render_node[0] ? gpu_device.render_node : "unknown-node");
        return false;
    }

    const auto queue_family = find_graphics_queue_family(selected);
    if (!queue_family.has_value()) {
        g_warning("VividVideoProducer: selected Vulkan GPU %s has no graphics queue",
                  selected_properties.deviceName);
        return false;
    }

    const auto extensions = enumerate_device_extensions(selected);
    const char* missing_extension = nullptr;
    if (!device_supports_required_extensions(selected, extensions, &missing_extension)) {
        g_warning("VividVideoProducer: selected Vulkan GPU %s is missing extension %s",
                  selected_properties.deviceName,
                  missing_extension ? missing_extension : "(unknown)");
        return false;
    }
    if (transfer_path == VideoFrameTransferPath::VaMemoryBgra &&
        !device_supports_va_import_extensions(extensions, &missing_extension)) {
        g_warning("VividVideoProducer: selected Vulkan GPU %s is missing VA import "
                  "extension %s",
                  selected_properties.deviceName,
                  missing_extension ? missing_extension : "(unknown)");
        return false;
    }

    bool requires_dedicated = false;
    std::string reason;
    if (!device_supports_linear_export(selected,
                                       target_vk_format_for_transfer_path(transfer_path),
                                       requires_dedicated,
                                       reason)) {
        g_warning("VividVideoProducer: selected Vulkan GPU %s cannot export linear "
                  "RGBA DMA-BUF images: %s",
                  selected_properties.deviceName,
                  reason.c_str());
        return false;
    }

    g_message("VividVideoProducer: Vulkan GPU selected by uuid name=%s node=%s "
              "type=%d dedicated-export=%s forbid-device-local-export=%s",
              selected_properties.deviceName,
              gpu_device.render_node[0] ? gpu_device.render_node : "(unknown)",
              static_cast<int>(selected_properties.deviceType),
              bool_to_string(requires_dedicated),
              bool_to_string(selected_properties.deviceType ==
                             VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));

    backend.physical_device = selected;
    backend.graphics_queue_family = queue_family.value();
    backend.export_requires_dedicated = requires_dedicated;
    backend.export_forbids_device_local_memory =
        selected_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    backend.device_name = selected_properties.deviceName;
    vkGetPhysicalDeviceMemoryProperties(selected, &backend.memory_properties);
    return true;
}

bool
create_vulkan_device(VividVideoVulkanBackend& backend)
{
    const auto available_extensions = enumerate_device_extensions(backend.physical_device);
    std::vector<const char*> enabled_extensions(kRequiredDeviceExtensions.begin(),
                                                kRequiredDeviceExtensions.end());
    for (const char* extension : kVaImportDeviceExtensions) {
        if (has_extension(available_extensions, extension))
            enabled_extensions.push_back(extension);
    }
    if (has_extension(available_extensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
        enabled_extensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    if (has_extension(available_extensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
        enabled_extensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = backend.graphics_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    VkDeviceCreateInfo create_info {
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

    VkResult result =
        vkCreateDevice(backend.physical_device, &create_info, nullptr, &backend.device);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to create Vulkan device gpu=%s result=%s",
                  backend.device_name.c_str(),
                  vk_result_name(result));
        return false;
    }

    vkGetDeviceQueue(backend.device, backend.graphics_queue_family, 0, &backend.graphics_queue);
    backend.get_memory_fd =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(backend.device,
                                                                    "vkGetMemoryFdKHR"));
    backend.get_image_drm_format_modifier_properties =
        reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(backend.device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (!backend.graphics_queue || !backend.get_memory_fd) {
        g_warning("VividVideoProducer: Vulkan device gpu=%s missing queue/getMemoryFd",
                  backend.device_name.c_str());
        return false;
    }

    VkCommandPoolCreateInfo pool_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = backend.graphics_queue_family,
    };
    result = vkCreateCommandPool(backend.device, &pool_info, nullptr, &backend.command_pool);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to create Vulkan command pool result=%s",
                  vk_result_name(result));
        return false;
    }

    VkCommandBufferAllocateInfo command_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = backend.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = vkAllocateCommandBuffers(backend.device, &command_info, &backend.command_buffer);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to allocate Vulkan command buffer result=%s",
                  vk_result_name(result));
        return false;
    }

    return true;
}

std::optional<VividVideoVulkanExportImage>
create_export_image(VividVideoVulkanBackend& backend,
                    uint32_t width,
                    uint32_t height,
                    uint32_t index,
                    const VividVideoVulkanExportRequest& request)
{
    const bool modifier_path = request.require_modifier &&
        request.modifier != DRM_FORMAT_MOD_LINEAR &&
        request.modifier != DRM_FORMAT_MOD_INVALID;
    if (modifier_path && !backend.get_image_drm_format_modifier_properties) {
        g_warning("VividVideoProducer: cannot create modifier export image without "
                  "vkGetImageDrmFormatModifierPropertiesEXT");
        return std::nullopt;
    }

    VividVideoVulkanExportImage image;
    image.device = backend.device;
    image.index = index;
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
        .format = backend.target_format,
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
    VkResult result = vkCreateImage(backend.device, &image_info, nullptr, &image.image);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to create export image index=%u result=%s",
                  index,
                  vk_result_name(result));
        return std::nullopt;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(backend.device, image.image, &requirements);
    const bool prefer_device_local =
        modifier_path || request.memory == VividVideoVulkanExportMemory::DeviceLocal;
    const VkMemoryPropertyFlags required_memory_flags = prefer_device_local
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkMemoryPropertyFlags forbidden_memory_flags =
        (!prefer_device_local && backend.export_forbids_device_local_memory)
            ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            : 0;
    const auto memory_type =
        find_memory_type_exact(backend.memory_properties,
                               requirements.memoryTypeBits,
                               required_memory_flags,
                               forbidden_memory_flags);
    if (!memory_type.has_value()) {
        g_warning("VividVideoProducer: no suitable export memory type for export "
                  "image index=%u modifier=0x%016" G_GINT64_MODIFIER "x "
                  "memoryTypeBits=0x%x required=0x%x forbidden=0x%x gpu=%s",
                  index,
                  static_cast<guint64>(image.modifier),
                  requirements.memoryTypeBits,
                  required_memory_flags,
                  forbidden_memory_flags,
                  backend.device_name.c_str());
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
        .pNext = backend.export_requires_dedicated
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
    result = vkAllocateMemory(backend.device, &allocate_info, nullptr, &image.memory);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to allocate export image memory index=%u result=%s",
                  index,
                  vk_result_name(result));
        return std::nullopt;
    }

    result = vkBindImageMemory(backend.device, image.image, image.memory, 0);
    if (result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to bind export image index=%u result=%s",
                  index,
                  vk_result_name(result));
        return std::nullopt;
    }

    VkMemoryGetFdInfoKHR fd_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = image.memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    result = backend.get_memory_fd(backend.device, &fd_info, &image.fd);
    if (result != VK_SUCCESS || image.fd < 0) {
        g_warning("VividVideoProducer: failed to export image DMA-BUF index=%u result=%s",
                  index,
                  vk_result_name(result));
        return std::nullopt;
    }

    if (modifier_path) {
        VkImageDrmFormatModifierPropertiesEXT modifier_properties {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
            .pNext = nullptr,
            .drmFormatModifier = 0,
        };
        const VkResult modifier_result =
            backend.get_image_drm_format_modifier_properties(backend.device,
                                                             image.image,
                                                             &modifier_properties);
        if (modifier_result != VK_SUCCESS ||
            modifier_properties.drmFormatModifier != request.modifier) {
            g_warning("VividVideoProducer: export image modifier mismatch requested=0x%016"
                      G_GINT64_MODIFIER "x actual=0x%016" G_GINT64_MODIFIER
                      "x result=%s",
                      static_cast<guint64>(request.modifier),
                      static_cast<guint64>(modifier_properties.drmFormatModifier),
                      vk_result_name(modifier_result));
            return std::nullopt;
        }
        image.modifier = modifier_properties.drmFormatModifier;
    }

    const std::array<VkImageAspectFlagBits, VividVideoVulkanExportImage::kMaxPlanes>
        plane_aspects {
            VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
        };
    const uint32_t plane_count = modifier_path ? request.plane_count : 1u;
    if (plane_count == 0 || plane_count > VividVideoVulkanExportImage::kMaxPlanes) {
        g_warning("VividVideoProducer: export image requested invalid plane count=%u",
                  plane_count);
        return std::nullopt;
    }

    image.n_planes = plane_count;
    image.size = requirements.size;
    for (uint32_t plane = 0; plane < plane_count; plane++) {
        VkImageSubresource subresource {
            .aspectMask = modifier_path ? plane_aspects[plane] : VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .arrayLayer = 0,
        };
        VkSubresourceLayout layout {};
        vkGetImageSubresourceLayout(backend.device, image.image, &subresource, &layout);
        if (layout.rowPitch > std::numeric_limits<uint32_t>::max() ||
            layout.offset > std::numeric_limits<uint32_t>::max()) {
            g_warning("VividVideoProducer: export image layout is too large index=%u "
                      "plane=%u rowPitch=%llu offset=%llu",
                      index,
                      plane,
                      static_cast<unsigned long long>(layout.rowPitch),
                      static_cast<unsigned long long>(layout.offset));
            return std::nullopt;
        }

        image.plane_strides[plane] = static_cast<uint32_t>(layout.rowPitch);
        image.plane_offsets[plane] = static_cast<uint32_t>(layout.offset);
        image.plane_fds[plane] = plane == 0 ? image.fd : fcntl(image.fd, F_DUPFD_CLOEXEC, 3);
        if (image.plane_fds[plane] < 0) {
            g_warning("VividVideoProducer: failed to duplicate export plane fd "
                      "index=%u plane=%u: %s",
                      index,
                      plane,
                      g_strerror(errno));
            return std::nullopt;
        }
    }

    image.stride = image.plane_strides[0];
    image.offset = image.plane_offsets[0];
    return image;
}

bool
create_export_images(VividVideoVulkanBackend& backend,
                     uint32_t width,
                     uint32_t height,
                     const VividVideoVulkanExportRequest& request)
{
    for (uint32_t i = 0; i < VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT; i++) {
        auto image = create_export_image(backend, width, height, i, request);
        if (!image.has_value())
            return false;
        backend.images[i] = std::move(image.value());
    }

    backend.presented_index = 0;
    backend.ready_index = 1;
    backend.in_progress_index = 2;
    backend.dirty = false;
    return true;
}


} // namespace

bool
VividVideoVulkanBackend::ensure(const VividGpuDevice& gpu_device,
                                 VideoFrameTransferPath transfer_path,
                                 uint32_t width,
                                 uint32_t height,
                                 const VividVideoVulkanExportRequest& request)
{
    auto& backend = *this;
    backend.reset();
    backend.target_format = target_vk_format_for_transfer_path(transfer_path);
    backend.target_fourcc = target_drm_fourcc_for_transfer_path(transfer_path);
    backend.target_modifier = request.require_modifier &&
        request.modifier != DRM_FORMAT_MOD_INVALID
        ? request.modifier
        : DRM_FORMAT_MOD_LINEAR;
    if (!create_vulkan_instance(backend))
        return false;
    if (!choose_physical_device(backend, gpu_device, transfer_path))
        return false;
    if (!create_vulkan_device(backend))
        return false;
    if (!create_export_images(backend, width, height, request))
        return false;

    g_message("VividVideoProducer: prepared video producer Vulkan route ring %ux%u "
              "fourcc=0x%08x modifier=0x%016" G_GINT64_MODIFIER "x gpu=%s buffers=%u",
              width,
              height,
              backend.target_fourcc,
              static_cast<guint64>(backend.target_modifier),
              backend.device_name.c_str(),
              VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT);
    return true;
}

std::vector<VividVideoVulkanFormatCap>
VividVideoVulkanBackend::query_export_caps(const VividGpuDevice& gpu_device,
                                           VkImageUsageFlags     usage)
{
    (void)usage;
    std::vector<VividVideoVulkanFormatCap> caps;
    if (!gpu_device.render_node[0])
        return caps;

    VkInstance probe_instance = VK_NULL_HANDLE;
    if (!create_probe_instance(probe_instance, "Vivid video producer caps"))
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
    const char* missing_extension = nullptr;
    if (!device_supports_required_extensions(selected, extensions, &missing_extension)) {
        vkDestroyInstance(probe_instance, nullptr);
        return caps;
    }

    const bool has_modifier_export =
        device_supports_va_import_extensions(extensions, &missing_extension);
    const VkFormatFeatureFlags want_features =
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const std::array<uint32_t, 2> fourccs {
        DRM_FORMAT_ABGR8888,
        DRM_FORMAT_ARGB8888,
    };

    for (uint32_t fourcc : fourccs) {
        const VkFormat vk_format = fourcc_to_vk_format(fourcc);
        if (has_modifier_export) {
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
             * Mirror waywallen's producer cap probe. The exported image metadata
             * path below duplicates the single Vulkan dma-buf fd across each memory
             * plane and publishes the per-plane layout, so multi-plane modifier
             * tuples are safe to expose when the consumer advertises the same tuple.
             */
            for (const auto& modifier : modifiers) {
                if (modifier.drmFormatModifier == DRM_FORMAT_MOD_LINEAR ||
                    modifier.drmFormatModifierPlaneCount == 0 ||
                    modifier.drmFormatModifierPlaneCount >
                        VividVideoVulkanExportImage::kMaxPlanes ||
                    (modifier.drmFormatModifierTilingFeatures & want_features) !=
                        want_features) {
                    continue;
                }
                caps.push_back(VividVideoVulkanFormatCap {
                    .fourcc = fourcc,
                    .modifier = modifier.drmFormatModifier,
                    .plane_count = modifier.drmFormatModifierPlaneCount,
                });
            }
        }

        bool requires_dedicated = false;
        std::string reason;
        if (device_supports_linear_export(selected,
                                          vk_format,
                                          requires_dedicated,
                                          reason)) {
            caps.push_back(VividVideoVulkanFormatCap {
                .fourcc = fourcc,
                .modifier = DRM_FORMAT_MOD_LINEAR,
                .plane_count = 1,
            });
        }
    }

    g_message("VividVideoProducer: probed Vulkan export caps gpu=%s node=%s caps=%zu",
              properties.deviceName,
              gpu_device.render_node,
              caps.size());
    vkDestroyInstance(probe_instance, nullptr);
    return caps;
}

namespace
{

std::optional<off_t>
query_fd_size(int fd)
{
    if (fd < 0)
        return std::nullopt;
    const off_t current = lseek(fd, 0, SEEK_CUR);
    const off_t end = lseek(fd, 0, SEEK_END);
    if (current >= 0)
        (void)lseek(fd, current, SEEK_SET);
    if (end <= 0)
        return std::nullopt;
    return end;
}

std::string
drm_fourcc_to_string(uint32_t fourcc)
{
    char text[5] {
        static_cast<char>(fourcc & 0xff),
        static_cast<char>((fourcc >> 8) & 0xff),
        static_cast<char>((fourcc >> 16) & 0xff),
        static_cast<char>((fourcc >> 24) & 0xff),
        0,
    };
    return text;
}

std::optional<VkFormat>
vk_format_for_rgba_drm_fourcc(uint32_t drm_format)
{
    if (drm_format == DRM_FORMAT_ABGR8888)
        return VK_FORMAT_R8G8B8A8_UNORM;
    if (drm_format == DRM_FORMAT_ARGB8888)
        return VK_FORMAT_B8G8R8A8_UNORM;
    return std::nullopt;
}

bool
parse_va_rgba_dmabuf_caps(GstCaps* caps,
                          GstVideoInfoDmaDrm& drm_info,
                          VkFormat& vk_format,
                          uint64_t& drm_modifier)
{
    if (!caps || !gst_video_is_dma_drm_caps(caps))
        return false;

    gst_video_info_dma_drm_init(&drm_info);
    if (!gst_video_info_dma_drm_from_caps(&drm_info, caps))
        return false;

    auto format = vk_format_for_rgba_drm_fourcc(drm_info.drm_fourcc);
    if (!format.has_value())
        return false;

    vk_format = format.value();
    drm_modifier = drm_info.drm_modifier == DRM_FORMAT_MOD_INVALID
        ? static_cast<uint64_t>(DRM_FORMAT_MOD_LINEAR)
        : drm_info.drm_modifier;
    return true;
}

std::optional<VividVideoVulkanImportedImage>
import_single_plane_drm_rgba_image(VividVideoVulkanBackend& backend,
                                   uint32_t width,
                                   uint32_t height,
                                   VkFormat vk_format,
                                   uint64_t drm_modifier,
                                   int fd,
                                   uint32_t object_size,
                                   uint32_t plane_offset,
                                   uint32_t plane_stride)
{
    if (backend.device == VK_NULL_HANDLE || width == 0 || height == 0 ||
        fd < 0 || plane_stride == 0)
        return std::nullopt;
    if (drm_modifier == DRM_FORMAT_MOD_INVALID)
        drm_modifier = DRM_FORMAT_MOD_LINEAR;

    VividVideoVulkanImportedImage result;
    result.device = backend.device;
    result.format = vk_format;
    result.width = width;
    result.height = height;

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
        .drmFormatModifier = drm_modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };
    VkImageCreateInfo image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = VkExtent3D { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vk_result = vkCreateImage(backend.device, &image_info, nullptr, &result.image);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to create imported VA DMA-BUF image result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(backend.device, result.image, &requirements);
    const auto memory_type =
        find_memory_type(backend.memory_properties, requirements.memoryTypeBits, 0);
    if (!memory_type.has_value()) {
        g_warning("VividVideoProducer: no Vulkan memory type accepts VA DMA-BUF import");
        return std::nullopt;
    }

    const int import_fd = dup(fd);
    if (import_fd < 0) {
        g_warning("VividVideoProducer: failed to duplicate VA DMA-BUF fd: %s",
                  g_strerror(errno));
        return std::nullopt;
    }

    VkMemoryDedicatedAllocateInfo dedicated_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = result.image,
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
    vk_result = vkAllocateMemory(backend.device, &allocate_info, nullptr, &result.memory);
    if (vk_result != VK_SUCCESS) {
        close(import_fd);
        g_warning("VividVideoProducer: failed to import VA DMA-BUF memory result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    vk_result = vkBindImageMemory(backend.device, result.image, result.memory, 0);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to bind imported VA DMA-BUF image result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    return result;
}

void
close_va_descriptor_fds(VADRMPRIMESurfaceDescriptor& descriptor)
{
    for (uint32_t i = 0; i < descriptor.num_objects && i < 4; i++) {
        if (descriptor.objects[i].fd >= 0) {
            close(descriptor.objects[i].fd);
            descriptor.objects[i].fd = -1;
        }
    }
}


} // namespace
std::optional<VividVideoCudaExternalBuffer>
VividVideoVulkanBackend::create_cuda_external_transfer_buffer(guint64 size,
                                                              GstCudaContext* cuda_context)
{
    auto& backend = *this;
    if (size == 0 || !cuda_context || backend.device == VK_NULL_HANDLE)
        return std::nullopt;

    VividVideoCudaExternalBuffer result;
    result.device = backend.device;
    result.size = size;
    result.cuda_context = reinterpret_cast<GstCudaContext*>(gst_object_ref(cuda_context));

    VkExternalMemoryBufferCreateInfo external_buffer {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer,
        .flags = 0,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkResult vk_result = vkCreateBuffer(backend.device, &buffer_info, nullptr, &result.buffer);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to create CUDA/Vulkan transfer buffer result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(backend.device, result.buffer, &requirements);
    const auto memory_type =
        find_memory_type(backend.memory_properties,
                         requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type.has_value()) {
        g_warning("VividVideoProducer: no memory type for CUDA/Vulkan transfer buffer");
        return std::nullopt;
    }

    VkExportMemoryAllocateInfo export_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkMemoryAllocateInfo alloc_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type.value(),
    };
    vk_result = vkAllocateMemory(backend.device, &alloc_info, nullptr, &result.memory);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to allocate CUDA/Vulkan transfer memory result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }
    vk_result = vkBindBufferMemory(backend.device, result.buffer, result.memory, 0);
    if (vk_result != VK_SUCCESS) {
        g_warning("VividVideoProducer: failed to bind CUDA/Vulkan transfer buffer result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    VkMemoryGetFdInfoKHR fd_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = result.memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    vk_result = backend.get_memory_fd(backend.device, &fd_info, &result.fd);
    if (vk_result != VK_SUCCESS || result.fd < 0) {
        g_warning("VividVideoProducer: failed to export transfer buffer fd result=%s",
                  vk_result_name(vk_result));
        return std::nullopt;
    }

    bool pushed = gst_cuda_context_push(cuda_context);
    if (!pushed) {
        g_warning("VividVideoProducer: failed to push CUDA context for Vulkan buffer import");
        return std::nullopt;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC import_desc {};
    import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    import_desc.handle.fd = result.fd;
    import_desc.size = static_cast<unsigned long long>(requirements.size);
    CUresult cuda_result = CuImportExternalMemory(&result.cuda_memory, &import_desc);
    if (cuda_result == CUDA_SUCCESS)
        result.fd = -1;

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc {};
    buffer_desc.offset = 0;
    buffer_desc.size = static_cast<unsigned long long>(size);
    if (cuda_result == CUDA_SUCCESS)
        cuda_result =
            CuExternalMemoryGetMappedBuffer(&result.cuda_ptr, result.cuda_memory, &buffer_desc);

    CUcontext popped {};
    (void)gst_cuda_context_pop(&popped);

    if (cuda_result != CUDA_SUCCESS || result.cuda_ptr == 0) {
        g_warning("VividVideoProducer: failed to import Vulkan transfer buffer into CUDA "
                  "result=%s",
                  cuda_error_name(cuda_result));
        return std::nullopt;
    }

    return result;
}

bool
VividVideoVulkanBackend::submit_rgba_buffer(const VividVideoCudaExternalBuffer& rgba_buffer)
{
    auto& backend = *this;
    if (backend.device == VK_NULL_HANDLE || backend.command_buffer == VK_NULL_HANDLE ||
        !rgba_buffer)
        return false;

    VividVideoVulkanExportImage& target = backend.in_progress_image();
    if (!target)
        return false;

    VkResult result = vkResetCommandBuffer(backend.command_buffer, 0);
    if (result != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(backend.command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return false;

    const VkImageSubresourceRange color_range {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    /*
     * Ownership mirrors scene's final export model without sharing scene code:
     * each frame acquires the in-progress image from EXTERNAL if it has ever
     * been published, writes it on this Vulkan queue, and releases it back to
     * EXTERNAL before FRAME_READY exposes the buffer index to the renderer.
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
            ? backend.graphics_queue_family
            : VK_QUEUE_FAMILY_IGNORED,
        .image = target.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(backend.command_buffer,
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

    VkBufferMemoryBarrier cuda_to_transfer {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = rgba_buffer.buffer,
        .offset = 0,
        .size = rgba_buffer.size,
    };
    vkCmdPipelineBarrier(backend.command_buffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         1,
                         &cuda_to_transfer,
                         0,
                         nullptr);

    VkBufferImageCopy copy {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            VkImageSubresourceLayers {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = VkOffset3D { 0, 0, 0 },
        .imageExtent = VkExtent3D { target.width, target.height, 1 },
    };
    vkCmdCopyBufferToImage(backend.command_buffer,
                           rgba_buffer.buffer,
                           target.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &copy);

    VkImageMemoryBarrier release {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = backend.graphics_queue_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = target.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(backend.command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &release);

    result = vkEndCommandBuffer(backend.command_buffer);
    if (result != VK_SUCCESS)
        return false;

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &backend.command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    result = vkQueueSubmit(backend.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        return false;
    result = vkQueueWaitIdle(backend.graphics_queue);
    if (result != VK_SUCCESS)
        return false;

    target.initialized = true;
    backend.mark_frame_ready();
    return true;
}

std::optional<VividVideoVulkanImportedImage>
VividVideoVulkanBackend::import_va_dmabuf_rgba_image(GstCaps* caps, GstBuffer* buffer)
{
    auto& backend = *this;
    if (!caps || !buffer || gst_buffer_n_memory(buffer) != 1)
        return std::nullopt;

    GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
    if (!memory || !gst_is_dmabuf_memory(memory))
        return std::nullopt;

    GstVideoInfoDmaDrm drm_info;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    uint64_t drm_modifier = DRM_FORMAT_MOD_INVALID;
    if (!parse_va_rgba_dmabuf_caps(caps, drm_info, vk_format, drm_modifier))
        return std::nullopt;

    gsize memory_offset = 0;
    gsize memory_size = 0;
    const gsize max_memory_size = gst_memory_get_sizes(memory, &memory_offset, &memory_size);
    const int fd = gst_dmabuf_memory_get_fd(memory);
    if (fd < 0 || max_memory_size == 0)
        return std::nullopt;

    GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
    const gsize plane_offset = meta && meta->n_planes > 0
        ? static_cast<gsize>(meta->offset[0])
        : static_cast<gsize>(GST_VIDEO_INFO_PLANE_OFFSET(&drm_info.vinfo, 0));
    const gint plane_stride = meta && meta->n_planes > 0
        ? meta->stride[0]
        : GST_VIDEO_INFO_PLANE_STRIDE(&drm_info.vinfo, 0);
    if (plane_stride <= 0)
        return std::nullopt;

    const gint width = GST_VIDEO_INFO_WIDTH(&drm_info.vinfo);
    const gint height = GST_VIDEO_INFO_HEIGHT(&drm_info.vinfo);
    if (width <= 0 || height <= 0)
        return std::nullopt;

    return import_single_plane_drm_rgba_image(
        backend,
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        vk_format,
        drm_modifier,
        fd,
        static_cast<uint32_t>(query_fd_size(fd).value_or(static_cast<off_t>(max_memory_size))),
        static_cast<uint32_t>(memory_offset + plane_offset),
        static_cast<uint32_t>(plane_stride));
}

std::optional<VividVideoVulkanImportedImage>
VividVideoVulkanBackend::export_va_memory_rgba_image(GstCaps* caps, GstBuffer* buffer)
{
    auto& backend = *this;
    if (!caps || !buffer || gst_buffer_n_memory(buffer) == 0)
        return std::nullopt;

    GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
    if (!memory || !gst_memory_is_type(memory, GST_ALLOCATOR_VASURFACE))
        return std::nullopt;

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps))
        return std::nullopt;
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA &&
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA)
        return std::nullopt;

    GstVaDisplay* display = gst_va_memory_peek_display(memory);
    VASurfaceID surface = gst_va_memory_get_surface(memory);
    if (!display)
        display = gst_va_buffer_peek_display(buffer);
    if (surface == VA_INVALID_SURFACE)
        surface = gst_va_buffer_get_surface(buffer);
    if (!display || surface == VA_INVALID_SURFACE)
        return std::nullopt;

    auto* va_display = reinterpret_cast<VADisplay>(gst_va_display_get_va_dpy(display));
    if (!va_display)
        return std::nullopt;

    VAStatus status = vaSyncSurface(va_display, surface);
    if (status != VA_STATUS_SUCCESS) {
        g_warning("VividVideoProducer: vaSyncSurface failed status=%s", vaErrorStr(status));
        return std::nullopt;
    }

    VADRMPRIMESurfaceDescriptor descriptor {};
    for (auto& object : descriptor.objects)
        object.fd = -1;
    /*
     * VA decoders often keep the ready frame as VAMemory instead of negotiating
     * memory:DMABuf through GStreamer caps. Exporting the already post-processed
     * surface here keeps decode, colorspace conversion, scale/crop, and final
     * DMA-BUF publication on GPU memory without introducing a CPU readback path.
     */
    status = vaExportSurfaceHandle(va_display,
                                   surface,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY |
                                       VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &descriptor);
    if (status != VA_STATUS_SUCCESS) {
        g_warning("VividVideoProducer: vaExportSurfaceHandle failed status=%s",
                  vaErrorStr(status));
        return std::nullopt;
    }

    std::optional<VividVideoVulkanImportedImage> imported;
    if (descriptor.num_layers == 1 &&
        descriptor.layers[0].num_planes == 1 &&
        descriptor.num_objects > 0 &&
        descriptor.layers[0].object_index[0] < descriptor.num_objects) {
        const auto& layer = descriptor.layers[0];
        const auto& object = descriptor.objects[layer.object_index[0]];
        auto format = vk_format_for_rgba_drm_fourcc(layer.drm_format);
        if (format.has_value()) {
            imported = import_single_plane_drm_rgba_image(backend,
                                                          descriptor.width,
                                                          descriptor.height,
                                                          format.value(),
                                                          object.drm_format_modifier,
                                                          object.fd,
                                                          object.size,
                                                          layer.offset[0],
                                                          layer.pitch[0]);
        }
    }

    if (!imported.has_value()) {
        g_warning("VividVideoProducer: unsupported VA exported surface fourcc=%s "
                  "layers=%u objects=%u layer0-format=%s layer0-planes=%u",
                  drm_fourcc_to_string(descriptor.fourcc).c_str(),
                  descriptor.num_layers,
                  descriptor.num_objects,
                  descriptor.num_layers > 0
                      ? drm_fourcc_to_string(descriptor.layers[0].drm_format).c_str()
                      : "none",
                  descriptor.num_layers > 0 ? descriptor.layers[0].num_planes : 0);
    }

    close_va_descriptor_fds(descriptor);
    return imported;
}

bool
VividVideoVulkanBackend::submit_imported_image(const VividVideoVulkanImportedImage& source,
                                                   VideoFillMode fill_mode)
{
    auto& backend = *this;
    if (backend.device == VK_NULL_HANDLE || backend.command_buffer == VK_NULL_HANDLE ||
        !source)
        return false;

    VividVideoVulkanExportImage& target = backend.in_progress_image();
    if (!target)
        return false;

    VkResult result = vkResetCommandBuffer(backend.command_buffer, 0);
    if (result != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(backend.command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return false;

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
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = source.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(backend.command_buffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &source_to_transfer);

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
            ? backend.graphics_queue_family
            : VK_QUEUE_FAMILY_IGNORED,
        .image = target.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(backend.command_buffer,
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

    VkClearColorValue clear_color {};
    clear_color.float32[3] = 1.0f;
    vkCmdClearColorImage(backend.command_buffer,
                         target.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear_color,
                         1,
                         &color_range);

    const VideoBlitRegions regions =
        compute_video_blit_regions(fill_mode, source.width, source.height, target.width, target.height);
    VkImageBlit blit {
        .srcSubresource =
            VkImageSubresourceLayers {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .srcOffsets = { regions.src0, regions.src1 },
        .dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .dstOffsets = { regions.dst0, regions.dst1 },
    };
    vkCmdBlitImage(backend.command_buffer,
                   source.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &blit,
                   VK_FILTER_NEAREST);

    VkImageMemoryBarrier release {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = backend.graphics_queue_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = target.image,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(backend.command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &release);

    result = vkEndCommandBuffer(backend.command_buffer);
    if (result != VK_SUCCESS)
        return false;

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &backend.command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    result = vkQueueSubmit(backend.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        return false;
    result = vkQueueWaitIdle(backend.graphics_queue);
    if (result != VK_SUCCESS)
        return false;

    target.initialized = true;
    backend.mark_frame_ready();
    return true;
}
