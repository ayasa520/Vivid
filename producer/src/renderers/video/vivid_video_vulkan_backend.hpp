/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

#include "vivid_gpu_devices.h"
#include "vivid_video_producer.h"

#include <drm/drm_fourcc.h>
#include <ffnvcodec/dynlink_cuda.h>
#include <gst/gst.h>
#include <vulkan/vulkan.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

extern "C" {
typedef struct _GstCudaContext GstCudaContext;
}

constexpr guint64 VIVID_VIDEO_DMABUF_MODIFIER = DRM_FORMAT_MOD_LINEAR;
constexpr guint VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT =
    VIVID_VIDEO_PRODUCER_MAX_BUFFERS;

enum class VividVideoVulkanExportMemory
{
    HostVisible,
    DeviceLocal,
};

struct VividVideoVulkanFormatCap
{
    uint32_t fourcc { 0 };
    uint64_t modifier { 0 };
    uint32_t plane_count { 1 };
};

struct VividVideoVulkanExportRequest
{
    uint32_t fourcc { 0 };
    uint64_t modifier { 0 };
    uint32_t plane_count { 1 };
    bool require_modifier { false };
    VividVideoVulkanExportMemory memory { VividVideoVulkanExportMemory::HostVisible };
};

/*
 * Private video upload detail, not a GPU selection policy.
 *
 * render-device resolves the physical GPU before this backend is reached. This
 * enum only describes the memory shape produced by the decoder and the matching
 * upload/import path used to fill the Vulkan DMA-BUF export ring on that exact
 * GPU.
 */
enum class VideoFrameTransferPath
{
    None,
    CudaNv12,
    VaMemoryBgra,
};

enum class VideoFillMode
{
    Stretch = 0,
    AspectFit = 1,
    AspectCrop = 2,
    ScaleDown = 3,
};

/*
 * The direct-video path owns its Vulkan runtime because it can run without any
 * scene renderer. Keeping every Vulkan object that can destroy or wait on that
 * device in this backend makes runtime ownership explicit, while the producer
 * publishes exported slots through the same route contract used by scene.
 */
struct VividVideoVulkanExportImage
{
    static constexpr uint32_t kMaxPlanes = 4;

    VkDevice device { VK_NULL_HANDLE };
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkDeviceSize size { 0 };
    uint32_t index { 0 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t stride { 0 };
    uint32_t offset { 0 };
    uint64_t modifier { DRM_FORMAT_MOD_LINEAR };
    int fd { -1 };
    uint32_t n_planes { 0 };
    std::array<int, kMaxPlanes> plane_fds { -1, -1, -1, -1 };
    std::array<uint32_t, kMaxPlanes> plane_strides {};
    std::array<uint32_t, kMaxPlanes> plane_offsets {};
    bool initialized { false };

    VividVideoVulkanExportImage() = default;
    VividVideoVulkanExportImage(const VividVideoVulkanExportImage&) = delete;
    VividVideoVulkanExportImage& operator=(const VividVideoVulkanExportImage&) = delete;
    VividVideoVulkanExportImage(VividVideoVulkanExportImage&& other) noexcept;
    VividVideoVulkanExportImage& operator=(VividVideoVulkanExportImage&& other) noexcept;
    ~VividVideoVulkanExportImage();

    explicit operator bool() const;
    void reset();
};

struct VividVideoCudaExternalBuffer
{
    VkDevice device { VK_NULL_HANDLE };
    VkBuffer buffer { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkDeviceSize size { 0 };
    int fd { -1 };
    CUexternalMemory cuda_memory {};
    CUdeviceptr cuda_ptr {};
    GstCudaContext* cuda_context { nullptr };

    VividVideoCudaExternalBuffer() = default;
    VividVideoCudaExternalBuffer(const VividVideoCudaExternalBuffer&) = delete;
    VividVideoCudaExternalBuffer& operator=(const VividVideoCudaExternalBuffer&) = delete;
    VividVideoCudaExternalBuffer(VividVideoCudaExternalBuffer&& other) noexcept;
    VividVideoCudaExternalBuffer& operator=(VividVideoCudaExternalBuffer&& other) noexcept;
    ~VividVideoCudaExternalBuffer();

    explicit operator bool() const;
    void reset();
};

struct VividVideoVulkanImportedImage
{
    VkDevice device { VK_NULL_HANDLE };
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    uint32_t width { 0 };
    uint32_t height { 0 };

    VividVideoVulkanImportedImage() = default;
    VividVideoVulkanImportedImage(const VividVideoVulkanImportedImage&) = delete;
    VividVideoVulkanImportedImage& operator=(const VividVideoVulkanImportedImage&) = delete;
    VividVideoVulkanImportedImage(VividVideoVulkanImportedImage&& other) noexcept;
    VividVideoVulkanImportedImage& operator=(VividVideoVulkanImportedImage&& other) noexcept;
    ~VividVideoVulkanImportedImage();

    explicit operator bool() const;
    void reset();
};

struct VividVideoVulkanBackend
{
    VkInstance instance { VK_NULL_HANDLE };
    VkPhysicalDevice physical_device { VK_NULL_HANDLE };
    VkDevice device { VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties memory_properties {};
    VkQueue graphics_queue { VK_NULL_HANDLE };
    uint32_t graphics_queue_family { 0 };
    VkCommandPool command_pool { VK_NULL_HANDLE };
    VkCommandBuffer command_buffer { VK_NULL_HANDLE };
    PFN_vkGetMemoryFdKHR get_memory_fd { nullptr };
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties {
        nullptr
    };
    std::array<VividVideoVulkanExportImage, VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT> images {};
    VkFormat target_format { VK_FORMAT_R8G8B8A8_UNORM };
    guint32 target_fourcc { DRM_FORMAT_ABGR8888 };
    uint64_t target_modifier { DRM_FORMAT_MOD_LINEAR };
    uint32_t presented_index { 0 };
    uint32_t ready_index { 1 };
    uint32_t in_progress_index { 2 };
    bool dirty { false };
    bool export_requires_dedicated { false };
    bool export_forbids_device_local_memory { false };
    std::string device_name;

    VividVideoVulkanBackend() = default;
    VividVideoVulkanBackend(const VividVideoVulkanBackend&) = delete;
    VividVideoVulkanBackend& operator=(const VividVideoVulkanBackend&) = delete;
    ~VividVideoVulkanBackend();

    /*
     * The device is the one resolved from the configured render-device value;
     * choose_physical_device() matches its Vulkan deviceUUID exactly instead
     * of scoring candidates, so video can never land on a different card than
     * the scene backend for the same configuration.
     */
    bool ensure(const VividGpuDevice&        gpu_device,
                VideoFrameTransferPath       transfer_path,
                uint32_t width,
                uint32_t height,
                const VividVideoVulkanExportRequest& request);
    void reset();
    static std::vector<VividVideoVulkanFormatCap> query_export_caps(
        const VividGpuDevice& gpu_device,
        VkImageUsageFlags     usage);

    VividVideoVulkanExportImage& in_progress_image();
    guint32 in_progress_buffer_index() const { return in_progress_index; }
    void mark_frame_ready();
    VividVideoVulkanExportImage* eat_frame();

    const auto& export_images() const { return images; }
    guint32 drm_fourcc() const { return target_fourcc; }
    guint64 drm_modifier() const { return target_modifier; }

    std::optional<VividVideoCudaExternalBuffer>
    create_cuda_external_transfer_buffer(guint64 size, GstCudaContext* cuda_context);
    bool submit_rgba_buffer(const VividVideoCudaExternalBuffer& rgba_buffer);

    std::optional<VividVideoVulkanImportedImage>
    import_va_dmabuf_rgba_image(GstCaps* caps, GstBuffer* buffer);
    std::optional<VividVideoVulkanImportedImage>
    export_va_memory_rgba_image(GstCaps* caps, GstBuffer* buffer);
    bool submit_imported_image(const VividVideoVulkanImportedImage& source,
                               VideoFillMode fill_mode);
};
