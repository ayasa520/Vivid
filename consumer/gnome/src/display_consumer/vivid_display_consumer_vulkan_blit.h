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
 * host's queue.
 *
 * The copy is fully asynchronous with respect to the calling thread.
 * The GNOME helper runs the blitter on its GTK main thread, so the
 * blitter must never wait for GPU progress there: the producer
 * publishes frames right after vkQueueSubmit (its acquire sync_file is
 * still pending), and on a saturated GPU each frame's GPU work can take
 * tens of milliseconds. A CPU-side wait per frame would block the main
 * loop for almost 100% of the time and GTK would never paint the
 * wallpaper window. Instead the blit waits for the acquire fence on the
 * GPU, exports its own completion as a sync_file, and hands that fence
 * to the shadow dmabuf (implicit sync for GSK) and to the daemon's
 * release syncobj (so the producer may reuse the source slot once the
 * copy has finished). A small ring of command buffers keeps several
 * copies in flight; when the ring is full the frame is dropped and its
 * source slot is released immediately.
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

/* Number of copies that may be in flight on the GPU at once. The producer
 * exports a triple-buffered ring, so three pending copies cover the case
 * where every producer slot has been published but none has completed yet. */
#    define WW_VK_BLIT_RING_SIZE 3

/*
 * One in-flight copy. Each slot owns its command pool (vkResetCommandPool
 * must not touch a pool whose other buffers are still executing), the fence
 * that tells us when the slot may be recycled, and the two semaphores the
 * submission references. Semaphores referenced by a pending submission must
 * not be destroyed or re-imported until that submission has completed, so
 * they live for the whole blitter lifetime and are reused only after the
 * slot's fence has signaled.
 */
typedef struct ww_vk_blit_slot {
    VkCommandPool   pool;
    VkCommandBuffer cb;
    VkFence         fence;
    /* True while a submission that signals `fence` may still be executing. */
    bool armed;

    /* Binary semaphore the acquire sync_file is imported into (temporary
     * payload). The queue wait consumes the payload, after which the next
     * import into the same semaphore is legal once `fence` has signaled. */
    VkSemaphore acquire_sem;

    /* Signal semaphore for the blit submission, exportable as SYNC_FD.
     * After submit, vkGetSemaphoreFdKHR(SYNC_FD) gives us a sync_file fd
     * for the still-pending copy. We ioctl-import it into the shadow
     * dmabuf's dma_resv as a DMA_BUF_SYNC_WRITE fence (GSK's later sample
     * submission then waits for the copy via kernel implicit DMA-BUF sync,
     * the pattern in gsk/gpu/gskgpudownloadop.c) and into the daemon's
     * release syncobj (the producer's release gate opens when the copy is
     * done). SYNC_FD export has copy transference, so the semaphore is
     * reset by the export and reusable for the slot's next submission. */
    VkSemaphore export_sem;
    /* Set when a submission signaled `export_sem` but the export failed, so
     * the semaphore stayed signaled. It is replaced once the slot is idle. */
    bool export_sem_stale;
} ww_vk_blit_slot_t;

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
    PFN_vkGetFenceStatus         vkGetFenceStatus;
    PFN_vkWaitForFences          vkWaitForFences;
    PFN_vkQueueSubmit            vkQueueSubmit;

    /* Resolved lazily on first ensure_shadow_exportable; NULL when the
     * relay path was never used. Required only for DMABUF_RELAY. */
    PFN_vkGetMemoryFdKHR            vkGetMemoryFdKHR;
    PFN_vkGetImageSubresourceLayout vkGetImageSubresourceLayout;
    PFN_vkGetSemaphoreFdKHR         vkGetSemaphoreFdKHR;

    ww_vk_blit_slot_t ring[WW_VK_BLIT_RING_SIZE];
    /* Index of the slot the next blit will use; always the oldest one. */
    unsigned next_slot;

    /* Diagnostics for the ring-full frame drop path; the counter is
     * reported in a rate-limited log line. */
    uint64_t dropped_frames;
    uint64_t dropped_frames_logged;

    /* Set once vkGetSemaphoreFdKHR(SYNC_FD) failed for a completed submit.
     * The blitter then falls back to a bounded CPU wait plus host-side
     * release signal for every frame, and logs the downgrade once. */
    bool sync_file_export_unavailable;

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
 * Host callbacks through which the blitter hands the source DMA-BUF back to
 * the daemon. Both receive the release syncobj fd owned by the caller of
 * ww_vk_blitter_blit; neither closes it.
 *
 * `signal_release` signals the syncobj from the host. The blitter uses it
 * when no GPU work references the source image (frame dropped, failure
 * before submit) and as the fallback after a bounded CPU wait when the
 * copy's completion could not be exported as a sync_file.
 *
 * `import_release_sync_file` attaches `sync_file_fd` (the pending copy's
 * completion fence) to the syncobj, so the daemon's release wait completes
 * on the GPU without this thread waiting. It must not close `sync_file_fd`.
 * Return 0 on success, negative errno on failure; on failure the blitter
 * falls back to the bounded wait plus `signal_release`.
 */
typedef struct ww_vk_blit_release_ops {
    int (*signal_release)(int release_syncobj_fd, void* user_data);
    int (*import_release_sync_file)(int release_syncobj_fd, int sync_file_fd, void* user_data);
} ww_vk_blit_release_ops_t;

typedef enum ww_vk_blit_status {
    /* The copy was submitted; the shadow will hold this frame once it lands. */
    WW_VK_BLIT_SUBMITTED = 0,
    /* Every ring slot is still executing; the frame was skipped and its
     * source slot released. The shadow keeps the previous frame. */
    WW_VK_BLIT_DROPPED = 1,
} ww_vk_blit_status_t;

/*
 * Copy `imported` into the shadow. The producer releases imported
 * DMA-BUF images to VK_QUEUE_FAMILY_FOREIGN_EXT in GENERAL layout; the
 * blitter acquires that ownership, copies from TRANSFER_SRC_OPTIMAL, and
 * releases the image back to FOREIGN/GENERAL before completing.
 *
 * `acquire_sync_fd` is the producer's explicit acquire fence; ownership
 * transfers in (it is consumed by the semaphore import or closed). The copy
 * waits for it on the GPU. Pass -1 to copy without an acquire wait.
 *
 * `release_syncobj_fd` ownership transfers in: the blitter routes it through
 * `release_ops` (see above) and always closes it before returning. Pass -1
 * if the caller has no syncobj to release.
 *
 * Never blocks on GPU progress unless sync_file export is unavailable on
 * this driver. The caller must keep `imported` alive until
 * ww_vk_blitter_drain() has returned successfully or the blitter is shut
 * down, because the submission may still be executing when this returns.
 *
 * Returns WW_VK_BLIT_SUBMITTED or WW_VK_BLIT_DROPPED on success, negative
 * errno on failure.
 */
int ww_vk_blitter_blit(ww_vk_blitter_t* b, VkImage imported, uint32_t w, uint32_t h,
                       int acquire_sync_fd, int release_syncobj_fd,
                       const ww_vk_blit_release_ops_t* release_ops, void* release_user_data);

/*
 * Wait until every in-flight copy has completed, bounded by `timeout_ns`.
 * Required before destroying an imported source image or replacing the
 * shadow. Returns 0 when the ring is idle, -ETIMEDOUT when a copy is still
 * executing after the timeout (the caller must then keep the referenced
 * resources alive), negative errno on other failures.
 */
int ww_vk_blitter_drain(ww_vk_blitter_t* b, uint64_t timeout_ns);

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
