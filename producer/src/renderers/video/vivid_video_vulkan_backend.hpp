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
    Cover = 1,
    Fill = 2,
    Stretch = 3,
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

/*
 * GPU-side ordering between the CUDA NV12->RGBA kernel and the Vulkan
 * buffer->image copy, replacing the historical CuStreamSynchronize +
 * vkQueueWaitIdle CPU stalls:
 *
 *   kernel_done  timeline: CUDA signals value N after kernel N,
 *                          Vulkan submit N waits value N.
 *   copy_done    timeline: Vulkan submit N signals value N,
 *                          the CUDA stream waits value N before kernel N+1
 *                          rewrites the shared transfer buffer.
 *
 * Both semaphores are Vulkan-created, exported as OPAQUE_FD and imported into
 * the GStreamer CUDA context. Any failure permanently invalidates the interop
 * and the producer falls back to the fully synchronous upload path.
 */
struct VividVideoCudaSyncInterop
{
    VkDevice device { VK_NULL_HANDLE };
    VkSemaphore kernel_done { VK_NULL_HANDLE };
    VkSemaphore copy_done { VK_NULL_HANDLE };
    CUexternalSemaphore cuda_kernel_done {};
    CUexternalSemaphore cuda_copy_done {};
    GstCudaContext* cuda_context { nullptr };
    uint64_t kernel_value { 0 };
    uint64_t copy_value { 0 };
    bool valid { false };

    VividVideoCudaSyncInterop() = default;
    VividVideoCudaSyncInterop(const VividVideoCudaSyncInterop&) = delete;
    VividVideoCudaSyncInterop& operator=(const VividVideoCudaSyncInterop&) = delete;
    ~VividVideoCudaSyncInterop();

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
    /*
     * True when the driver reports linear sampling for this format/modifier
     * tuple, so the scale blit into the export ring may use bilinear filtering
     * instead of point sampling.
     */
    bool linear_filter { false };

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
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd { nullptr };
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties {
        nullptr
    };
    /*
     * Async CUDA upload path: one command buffer + fence per export slot so the
     * CPU only ever waits on the submission from a full ring rotation ago, and
     * one exportable binary semaphore per slot whose SYNC_FD becomes the frame's
     * explicit acquire fence for the display transport.
     */
    bool async_cuda_capable { false };
    std::array<VkCommandBuffer, VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT> slot_command_buffers {};
    std::array<VkFence, VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT> slot_fences {};
    std::array<VkSemaphore, VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT> slot_acquire_semaphores {};
    std::array<bool, VIVID_VIDEO_VULKAN_EXPORT_BUFFER_COUNT> slot_fence_in_flight {};
    VividVideoCudaSyncInterop cuda_sync;
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

    /*
     * Imported VA frames are re-created every sample, so the per-tuple format
     * feature query is cached here instead of being repeated for each frame.
     */
    struct ImportedFormatFeatures
    {
        VkFormat format { VK_FORMAT_UNDEFINED };
        uint64_t modifier { 0 };
        VkFormatFeatureFlags features { 0 };
    };
    std::vector<ImportedFormatFeatures> imported_format_features;

    /*
     * Minification chain for imported VA frames. A linear blit only reaches two
     * source texels per output pixel, so a downscale beyond 2x is split into
     * halving steps through these device-local optimal-tiled images; they are
     * sized for the current source region and re-created when it changes.
     */
    struct DownsampleImage
    {
        VkImage image { VK_NULL_HANDLE };
        VkDeviceMemory memory { VK_NULL_HANDLE };
        uint32_t width { 0 };
        uint32_t height { 0 };
        VkFormat format { VK_FORMAT_UNDEFINED };
    };
    std::vector<DownsampleImage> downsample_chain;
    VkFormat downsample_probe_format { VK_FORMAT_UNDEFINED };
    bool downsample_format_supported { false };
    uint32_t last_downsample_steps { 0 };

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

    /*
     * Async CUDA/Vulkan ordering helpers. ensure_cuda_sync_interop() may fail
     * (old driver, missing extension); everything then degrades to the
     * synchronous CuStreamSynchronize + vkQueueWaitIdle path. The stream
     * helpers must be called with the GStreamer CUDA context pushed.
     */
    bool ensure_cuda_sync_interop(GstCudaContext* cuda_context);
    bool cuda_sync_interop_valid() const { return cuda_sync.valid; }
    bool cuda_stream_wait_copy_done(CUstream stream);
    bool cuda_stream_signal_kernel_done(CUstream stream);

    /*
     * On the async path *out_acquire_fd receives a SYNC_FD that signals when
     * the copy into the export slot completes; on the synchronous fallback it
     * stays -1 (the worker publishes a pre-signaled fence as before).
     */
    bool submit_rgba_buffer(const VividVideoCudaExternalBuffer& rgba_buffer,
                            int* out_acquire_fd);

    std::optional<VividVideoVulkanImportedImage>
    import_va_dmabuf_rgba_image(GstCaps* caps, GstBuffer* buffer);
    std::optional<VividVideoVulkanImportedImage>
    export_va_memory_rgba_image(GstCaps* caps, GstBuffer* buffer);
    bool submit_imported_image(const VividVideoVulkanImportedImage& source,
                               VideoFillMode fill_mode);
};
