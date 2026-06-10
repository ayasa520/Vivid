/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Vulkan machinery for the web backend's accelerated frame path.
 *
 * The web backend publishes one Vulkan-created export ring as its DMA-BUF
 * contract. Each CEF OnAcceleratedPaint dmabuf is imported as a transient blit
 * source and copied by the GPU into one exported ring image. This route is
 * deliberately strict: if Vulkan says the CEF modifier cannot be imported for
 * the requested usage, the frame is rejected instead of publishing black or
 * stale pixels.
 */
/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */
#pragma once

#include "vivid_gpu_devices.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class VividWebVulkanExportMemory
{
    HostVisible,
    DeviceLocal,
};

struct VividWebVulkanFormatCap
{
    uint32_t fourcc { 0 };
    uint64_t modifier { 0 };
    uint32_t plane_count { 1 };
};

struct VividWebVulkanExportRequest
{
    uint32_t fourcc { 0 };
    uint64_t modifier { 0 };
    uint32_t plane_count { 1 };
    bool require_modifier { false };
    VividWebVulkanExportMemory memory { VividWebVulkanExportMemory::HostVisible };
};

struct VividWebVulkanImage
{
    static constexpr uint32_t kMaxPlanes = 4;

    VkDevice device { VK_NULL_HANDLE };
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t stride { 0 };
    uint32_t offset { 0 };
    uint64_t size { 0 };
    uint64_t modifier { 0 };
    int fd { -1 };
    uint32_t n_planes { 0 };
    std::array<int, kMaxPlanes> plane_fds { -1, -1, -1, -1 };
    std::array<uint32_t, kMaxPlanes> plane_strides {};
    std::array<uint32_t, kMaxPlanes> plane_offsets {};
    bool initialized { false };

    VividWebVulkanImage() = default;
    VividWebVulkanImage(const VividWebVulkanImage&) = delete;
    VividWebVulkanImage& operator=(const VividWebVulkanImage&) = delete;
    VividWebVulkanImage(VividWebVulkanImage&& other) noexcept;
    VividWebVulkanImage& operator=(VividWebVulkanImage&& other) noexcept;
    ~VividWebVulkanImage();

    explicit operator bool() const { return image != VK_NULL_HANDLE; }
    void reset();
};

struct VividWebVulkanRoute
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
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties { nullptr };
    bool export_requires_dedicated { false };
    bool export_forbids_device_local_memory { false };
    std::string device_name;

    VividWebVulkanRoute() = default;
    VividWebVulkanRoute(const VividWebVulkanRoute&) = delete;
    VividWebVulkanRoute& operator=(const VividWebVulkanRoute&) = delete;
    ~VividWebVulkanRoute();

    explicit operator bool() const { return device != VK_NULL_HANDLE; }

    /*
     * The physical device is matched by the resolved deviceUUID, exactly like
     * the scene and video backends, so the export ring always lives on the
     * card the render-device value selected.
     */
    bool ensure(const VividGpuDevice& gpu_device);
    void reset();
    void abandon_for_process_lifetime();
    static std::vector<VividWebVulkanFormatCap> query_export_caps(
        const VividGpuDevice& gpu_device,
        VkImageUsageFlags     usage);

    std::optional<VividWebVulkanImage> create_export_image(uint32_t width,
                                                            uint32_t height,
                                                            VkFormat format,
                                                            const VividWebVulkanExportRequest& request);

    /*
     * Import a single-plane RGBA/BGRA dmabuf. fd is dup'ed internally; the
     * caller keeps ownership of its descriptor. as_blit_target selects
     * TRANSFER_DST usage versus TRANSFER_SRC usage (one CEF frame).
     */
    std::optional<VividWebVulkanImage> import_dmabuf_image(uint32_t width,
                                                            uint32_t height,
                                                            VkFormat format,
                                                            uint64_t modifier,
                                                            int      fd,
                                                            uint64_t object_size,
                                                            uint32_t plane_offset,
                                                            uint32_t plane_stride,
                                                            bool     as_blit_target);

    /*
     * Blit source_rect of source into the whole target and wait for the queue
     * to finish, so the caller may release the source dmabuf (CEF reuses its
     * pool buffer as soon as OnAcceleratedPaint returns) and immediately mark
     * the ring slot ready.
     */
    bool blit_image(const VividWebVulkanImage& source,
                    int32_t  src_x,
                    int32_t  src_y,
                    uint32_t src_width,
                    uint32_t src_height,
                    VividWebVulkanImage& target);
};
