/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

/*
 * Vulkan dmabuf -> shadow image blitter for libwaywallen_display hosts.
 *
 * Imported dmabuf VkImages are created with TRANSFER_SRC usage only
 * (per-modifier format features often exclude SAMPLED on vendor
 * tilings). The host needs a sampler-friendly OPTIMAL VkImage; this
 * blitter owns that "shadow" and copies each frame into it on the
 * host's queue, then host-signals the daemon's release_syncobj after
 * the GPU copy completes.
 *
 * Reuses ww_vk_backend_t for the device-level fns it shares with the
 * dmabuf import path; resolves command-recording / fence / submit
 * fns separately.
 */

#ifndef WAYWALLEN_DISPLAY_BACKEND_VULKAN_BLIT_H
#define WAYWALLEN_DISPLAY_BACKEND_VULKAN_BLIT_H

#ifdef WW_HAVE_VULKAN

#    include "vivid_display_consumer_vulkan_backend.h"

#    include <vulkan/vulkan.h>

#    include <stdbool.h>
#    include <stdint.h>

#    ifdef __cplusplus
extern "C" {
#    endif

typedef struct ww_vk_blitter {
    /* Embedded backend, loaded with install_debug_utils=false to avoid
     * doubling up driver log forwarding when the same VkInstance is
     * already bound via waywallen_display_bind_vulkan. */
    ww_vk_backend_t backend;
    VkQueue         queue;

    PFN_vkCreateCommandPool      vkCreateCommandPool;
    PFN_vkDestroyCommandPool     vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkResetCommandPool       vkResetCommandPool;
    PFN_vkBeginCommandBuffer     vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer       vkEndCommandBuffer;
    PFN_vkCmdPipelineBarrier     vkCmdPipelineBarrier;
    PFN_vkCmdCopyImage           vkCmdCopyImage;
    PFN_vkCreateFence            vkCreateFence;
    PFN_vkDestroyFence           vkDestroyFence;
    PFN_vkResetFences            vkResetFences;
    PFN_vkWaitForFences          vkWaitForFences;
    PFN_vkQueueSubmit            vkQueueSubmit;

    /* Resolved lazily on first ensure_shadow_exportable; NULL when the
     * relay path was never used. Required only for DMABUF_RELAY. */
    PFN_vkGetMemoryFdKHR            vkGetMemoryFdKHR;
    PFN_vkGetImageSubresourceLayout vkGetImageSubresourceLayout;
    PFN_vkGetSemaphoreFdKHR         vkGetSemaphoreFdKHR;

    VkCommandPool   pool;
    VkCommandBuffer cb;
    VkFence         fence;
    bool            fence_armed;

    /* Signal semaphore for each blit submission, exportable as SYNC_FD.
     * After submit, vkGetSemaphoreFdKHR(SYNC_FD) gives us a sync_file
     * fd which we ioctl-import into the shadow dmabuf's dma_resv as a
     * DMA_BUF_SYNC_WRITE fence. GSK's later sample submission then
     * sees fresh content via kernel implicit DMA-BUF sync. Matches the
     * exact pattern in gsk/gpu/gskgpudownloadop.c. */
    VkSemaphore export_sem;

    VkImage        shadow_image;
    VkDeviceMemory shadow_mem;
    uint32_t       shadow_w;
    uint32_t       shadow_h;
    VkFormat       shadow_fmt;
    /* false until the first ww_vk_blitter_blit succeeds for this
     * shadow; reset to false on every shadow recreate. The host
     * checks this before exposing the shadow to Qt RHI as a sampled
     * texture — sampling a shadow whose layout is still UNDEFINED
     * (because no blit ran yet, e.g. textures_ready arrived but
     * frame_ready didn't) trips
     * VUID-vkCmdDraw-None-09600 and on NVIDIA ends in DEVICE_LOST. */
    bool shadow_has_content;

    /* Old shadows queued for delayed destruction. When the host's
     * size changes, the prior QSGVulkanTexture's VkImageView is
     * scheduled for destruction by Qt RHI's release queue but
     * doesn't actually go away until the *next* frame's
     * sync/render. Calling vkDestroyImage on the old shadow
     * inline trips VUID-vkDestroyImage-image-01000. We park the
     * old shadow handles here and the host calls
     * `ww_vk_blitter_tick_pending_destroys` once per frame; when
     * the per-entry countdown reaches zero (default 2 frames,
     * comfortably above Qt RHI's typical 1-frame deferred
     * release), we run vkDestroyImage / vkFreeMemory then. The
     * queue is small (4) — rapid simultaneous size changes beyond
     * that fall back to immediate destroy with a best-effort
     * vkDeviceWaitIdle, which is racy but unlikely. */
    struct {
        VkImage        image;
        VkDeviceMemory memory;
        int            frames_remaining;
    } pending_shadow_destroy[4];
    int pending_shadow_destroy_count;

    /* DMABUF_RELAY only: exported DMA-BUF fd + per-plane layout for the
     * current shadow image. `shadow_export_fd = -1` when the shadow was
     * created via the regular (non-exportable) path. Closed in
     * shutdown / replaced on every ensure_shadow_exportable call. */
    int      shadow_export_fd;
    uint32_t shadow_export_n_planes;
    uint32_t shadow_export_strides[4];
    uint64_t shadow_export_offsets[4];
    uint64_t shadow_export_modifier;

    bool initialized;
} ww_vk_blitter_t;

/*
 * Initialize. Returns 0 on success, negative errno on failure (struct
 * left zeroed). `host_get_proc` is the same callback shape backend_vulkan
 * uses; pass NULL to fall back to dlopen("libvulkan.so.1").
 */
int ww_vk_blitter_init(ww_vk_blitter_t* b, VkInstance instance, VkPhysicalDevice physical_device,
                       VkDevice device, uint32_t queue_family_index, VkQueue queue,
                       ww_vk_get_instance_proc_addr_fn host_get_proc);

/* Idempotent, safe to call on a zero-initialized struct. */
void ww_vk_blitter_shutdown(ww_vk_blitter_t* b);

/*
 * (Re-)create the shadow image when (w, h, fmt) differ from the
 * current one. No-op when they match. Returns 0 on success, negative
 * errno on failure.
 */
int ww_vk_blitter_ensure_shadow(ww_vk_blitter_t* b, uint32_t w, uint32_t h, VkFormat fmt);

/*
 * Allocate (or reallocate) a LINEAR-tiled, externally-exportable shadow
 * image. Used by WAYWALLEN_BACKEND_DMABUF_RELAY: the lib re-publishes
 * the shadow as a DMA-BUF for the host to import via its toolkit
 * (GdkDmabufTexture / wl_dmabuf).
 *
 * After success, the exported DMA-BUF fd + per-plane layout are stored
 * on the blitter and retrievable via `ww_vk_blitter_get_export`.
 *
 * Returns 0 on success, -EIO on any Vulkan failure, -ENOSYS when
 * vkGetMemoryFdKHR cannot be resolved on the device.
 */
int ww_vk_blitter_ensure_shadow_exportable(ww_vk_blitter_t* b, uint32_t w, uint32_t h,
                                           VkFormat fmt);

/*
 * Read back the exported DMA-BUF fd + per-plane layout of the current
 * shadow. The fd is lib-owned — callers MUST `dup(2)` if they want to
 * outlive the next `ensure_shadow_exportable`/shutdown call.
 *
 * `out_strides`/`out_offsets` must be arrays of length
 * WAYWALLEN_DMABUF_MAX_PLANES (4); only the first `*out_n_planes`
 * entries are written.
 *
 * Returns 0 on success, -EINVAL if no exportable shadow is currently
 * bound.
 */
int ww_vk_blitter_get_export(const ww_vk_blitter_t* b, int* out_fd, uint32_t* out_n_planes,
                             uint32_t out_strides[4], uint64_t out_offsets[4],
                             uint64_t* out_modifier);

/*
 * Copy `imported` (UNDEFINED layout, TRANSFER_SRC contents valid) into
 * the shadow. Waits on `acquire_sem` (may be VK_NULL_HANDLE), then
 * blocks the calling thread until the copy completes (vkWaitForFences).
 *
 * `release_syncobj_fd` ownership transfers in. On success, the blitter
 * invokes `signal_release_syncobj` after the copy fence signals, then
 * closes the fd. On failures before the copy is submitted, the blitter
 * also invokes the callback because the source DMA-BUF is no longer
 * needed. On failures after submit, it only closes the fd because the
 * GPU may still reference the source image. Pass -1 if the caller has no
 * syncobj to signal.
 *
 * Returns 0 on success, negative errno on failure.
 */
typedef int (*ww_vk_release_syncobj_fn)(int release_syncobj_fd, void* user_data);

int ww_vk_blitter_blit(ww_vk_blitter_t* b, VkImage imported, uint32_t w, uint32_t h,
                       VkSemaphore acquire_sem, int release_syncobj_fd,
                       ww_vk_release_syncobj_fn signal_release_syncobj,
                       void* signal_release_user_data);

static inline VkImage ww_vk_blitter_shadow(const ww_vk_blitter_t* b) {
    return b ? b->shadow_image : VK_NULL_HANDLE;
}

static inline VkImageLayout ww_vk_blitter_shadow_layout(const ww_vk_blitter_t* b) {
    (void)b;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static inline bool ww_vk_blitter_initialized(const ww_vk_blitter_t* b) {
    return b && b->initialized;
}

/* True once at least one blit has populated the current shadow. The
 * host MUST gate any sampling of the shadow on this — see the comment
 * on `shadow_has_content` for the failure mode if it doesn't. */
static inline bool ww_vk_blitter_shadow_has_content(const ww_vk_blitter_t* b) {
    return b && b->shadow_has_content;
}

/* Process the deferred-destroy queue: decrement each entry's frame
 * countdown; for entries whose countdown reaches 0, run
 * vkDestroyImage + vkFreeMemory. Call this once per frame from the
 * host's render thread (typically at the top of updatePaintNode),
 * AFTER any Qt RHI frame boundary that would have processed its
 * own release queue — i.e. one frame after `ensure_shadow` queued
 * the old shadow. */
void ww_vk_blitter_tick_pending_destroys(ww_vk_blitter_t* b);

#    ifdef __cplusplus
} /* extern "C" */
#    endif

#endif /* WW_HAVE_VULKAN */
#endif /* WAYWALLEN_DISPLAY_BACKEND_VULKAN_BLIT_H */
