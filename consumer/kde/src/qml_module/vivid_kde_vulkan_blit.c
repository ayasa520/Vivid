/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

/*
 * libwaywallen_display — Vulkan dmabuf -> shadow blitter.
 *
 * Compiled only when WW_HAVE_VULKAN is defined. Reuses
 * ww_vk_backend_t for the device-level fns shared with the dmabuf
 * import path; only the command-recording / fence / submit fns are
 * resolved here.
 */

#ifdef WW_HAVE_VULKAN

#    include "vivid_kde_vulkan_blit.h"
#    include "log_internal.h"

#    include <errno.h>
#    include <inttypes.h>
#    include <stdint.h>
#    include <string.h>
#    include <sys/ioctl.h>
#    include <unistd.h>

#    ifndef DMA_BUF_BASE
#        define DMA_BUF_BASE 'b'
#    endif
#    ifndef DMA_BUF_IOCTL_IMPORT_SYNC_FILE
/* Field order must match <linux/dma-buf.h> exactly (flags then fd) —
 * the ioctl reads at fixed offsets. */
struct ww_dma_buf_sync_file {
    uint32_t flags;
    int32_t  fd;
};
#        define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW(DMA_BUF_BASE, 3, struct ww_dma_buf_sync_file)
#        define DMA_BUF_SYNC_WRITE             (2u)
#    endif

static uint32_t pick_memory_type(const ww_vk_backend_t* backend, uint32_t type_bits,
                                 VkMemoryPropertyFlags req) {
    if (! backend->vkGetPhysicalDeviceMemoryProperties) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties props;
    backend->vkGetPhysicalDeviceMemoryProperties(backend->physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int resolve_cmd_fns(ww_vk_blitter_t* b) {
    PFN_vkGetDeviceProcAddr gdpa   = b->backend.vkGetDeviceProcAddr;
    VkDevice                device = b->backend.device;

#    define RESOLVE(SLOT, TYPE, NAME)                                                        \
        do {                                                                                 \
            b->SLOT = (TYPE)gdpa(device, NAME);                                              \
            if (! b->SLOT) {                                                                 \
                ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: gdpa(\"%s\") returned NULL", NAME); \
                return -ENOSYS;                                                              \
            }                                                                                \
        } while (0)

    RESOLVE(vkCreateCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    RESOLVE(vkDestroyCommandPool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    RESOLVE(vkAllocateCommandBuffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    RESOLVE(vkResetCommandPool, PFN_vkResetCommandPool, "vkResetCommandPool");
    RESOLVE(vkBeginCommandBuffer, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer");
    RESOLVE(vkEndCommandBuffer, PFN_vkEndCommandBuffer, "vkEndCommandBuffer");
    RESOLVE(vkCmdPipelineBarrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    RESOLVE(vkCmdCopyImage, PFN_vkCmdCopyImage, "vkCmdCopyImage");
    RESOLVE(vkCreateFence, PFN_vkCreateFence, "vkCreateFence");
    RESOLVE(vkDestroyFence, PFN_vkDestroyFence, "vkDestroyFence");
    RESOLVE(vkResetFences, PFN_vkResetFences, "vkResetFences");
    RESOLVE(vkGetFenceStatus, PFN_vkGetFenceStatus, "vkGetFenceStatus");
    RESOLVE(vkWaitForFences, PFN_vkWaitForFences, "vkWaitForFences");
    RESOLVE(vkQueueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");

#    undef RESOLVE
    return 0;
}

static int create_slot_objects(ww_vk_blitter_t* b, ww_vk_blit_slot_t* slot) {
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        /* No flags: the slot's pool is recycled as a whole via
         * vkResetCommandPool once its fence has signaled. */
        .flags            = 0,
        .queueFamilyIndex = b->backend.queue_family_index,
    };
    VkResult vr = b->vkCreateCommandPool(b->backend.device, &pci, NULL, &slot->pool);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateCommandPool failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkCommandBufferAllocateInfo cbi = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = slot->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vr = b->vkAllocateCommandBuffers(b->backend.device, &cbi, &slot->cb);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateCommandBuffers failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = 0,
    };
    vr = b->vkCreateFence(b->backend.device, &fci, NULL, &slot->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkCreateFence failed: %s", ww_vk_result_str(vr));
        return -EIO;
    }

    /* Plain binary semaphore that receives the producer's acquire
     * sync_file as a temporary payload right before each submission. */
    VkSemaphoreCreateInfo acquire_sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    vr = b->backend.vkCreateSemaphore(b->backend.device, &acquire_sci, NULL, &slot->acquire_sem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateSemaphore(acquire) failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    /* Exportable signal semaphore (SYNC_FD). Pattern matches GTK's
     * gsk/gpu/gskgpudownloadop.c — signal in submit, vkGetSemaphoreFdKHR
     * gives a real sync_file fd, ioctl-import into dma_resv. */
    VkExportSemaphoreCreateInfo exp_sem = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exp_sem,
    };
    vr = b->backend.vkCreateSemaphore(b->backend.device, &sci, NULL, &slot->export_sem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateSemaphore(export) failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }
    return 0;
}

static void destroy_slot_objects(ww_vk_blitter_t* b, ww_vk_blit_slot_t* slot) {
    if (slot->export_sem != VK_NULL_HANDLE && b->backend.vkDestroySemaphore) {
        b->backend.vkDestroySemaphore(b->backend.device, slot->export_sem, NULL);
        slot->export_sem = VK_NULL_HANDLE;
    }
    if (slot->acquire_sem != VK_NULL_HANDLE && b->backend.vkDestroySemaphore) {
        b->backend.vkDestroySemaphore(b->backend.device, slot->acquire_sem, NULL);
        slot->acquire_sem = VK_NULL_HANDLE;
    }
    if (slot->fence != VK_NULL_HANDLE && b->vkDestroyFence) {
        b->vkDestroyFence(b->backend.device, slot->fence, NULL);
        slot->fence = VK_NULL_HANDLE;
    }
    if (slot->pool != VK_NULL_HANDLE && b->vkDestroyCommandPool) {
        b->vkDestroyCommandPool(b->backend.device, slot->pool, NULL);
        slot->pool = VK_NULL_HANDLE;
    }
    slot->cb    = VK_NULL_HANDLE;
    slot->armed = false;
}

static int create_cmd_objects(ww_vk_blitter_t* b) {
    for (unsigned i = 0; i < WW_VK_BLIT_RING_SIZE; i++) {
        int rc = create_slot_objects(b, &b->ring[i]);
        if (rc != 0) return rc;
    }
    b->next_slot = 0;

    b->vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)b->backend.vkGetDeviceProcAddr(
        b->backend.device, "vkGetSemaphoreFdKHR");
    if (! b->vkGetSemaphoreFdKHR) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkGetSemaphoreFdKHR not resolvable");
        return -ENOSYS;
    }
    return 0;
}

/*
 * Bounded wait for every armed slot. Fences that signaled are reset so the
 * slots are immediately reusable; a slot whose fence is still pending after
 * the timeout stays armed because resetting or recycling it would be
 * undefined behaviour while the GPU may still execute its command buffer.
 */
int ww_vk_blitter_drain(ww_vk_blitter_t* b, uint64_t timeout_ns) {
    if (! b || ! b->initialized) return -EINVAL;

    VkFence  pending[WW_VK_BLIT_RING_SIZE];
    uint32_t pending_count = 0;
    for (unsigned i = 0; i < WW_VK_BLIT_RING_SIZE; i++) {
        if (b->ring[i].armed) pending[pending_count++] = b->ring[i].fence;
    }
    if (pending_count == 0) return 0;

    VkResult vr =
        b->vkWaitForFences(b->backend.device, pending_count, pending, VK_TRUE, timeout_ns);
    if (vr == VK_TIMEOUT) {
        /* Reclaim whatever did finish so a wedged copy does not pin the
         * whole ring. */
        for (unsigned i = 0; i < WW_VK_BLIT_RING_SIZE; i++) {
            ww_vk_blit_slot_t* slot = &b->ring[i];
            if (! slot->armed) continue;
            if (b->vkGetFenceStatus(b->backend.device, slot->fence) == VK_SUCCESS) {
                b->vkResetFences(b->backend.device, 1, &slot->fence);
                slot->armed = false;
            }
        }
        return -ETIMEDOUT;
    }
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: drain vkWaitForFences failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }
    for (unsigned i = 0; i < WW_VK_BLIT_RING_SIZE; i++) {
        ww_vk_blit_slot_t* slot = &b->ring[i];
        if (! slot->armed) continue;
        b->vkResetFences(b->backend.device, 1, &slot->fence);
        slot->armed = false;
    }
    return 0;
}

int ww_vk_blitter_init(ww_vk_blitter_t* b, VkInstance instance, VkPhysicalDevice physical_device,
                       VkDevice device, uint32_t queue_family_index, VkQueue queue,
                       ww_vk_get_instance_proc_addr_fn host_get_proc) {
    if (! b) return -EINVAL;
    if (b->initialized) return 0;
    if (! instance || ! physical_device || ! device || ! queue) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: missing handle "
               "(instance=%p phys=%p device=%p queue=%p)",
               (void*)instance,
               (void*)physical_device,
               (void*)device,
               (void*)queue);
        return -EINVAL;
    }
    memset(b, 0, sizeof(*b));

    int rc = ww_vk_backend_load(&b->backend,
                                instance,
                                physical_device,
                                device,
                                queue_family_index,
                                host_get_proc,
                                /* install_debug_utils */ false);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: backend load failed: %d", rc);
        return rc;
    }
    b->queue = queue;

    rc = resolve_cmd_fns(b);
    if (rc != 0) {
        ww_vk_backend_unload(&b->backend);
        return rc;
    }
    rc = create_cmd_objects(b);
    if (rc != 0) {
        ww_vk_blitter_shutdown(b);
        return rc;
    }

    b->shadow_export_fd = -1;
    b->initialized      = true;
    ww_log(
        WAYWALLEN_LOG_INFO, "vk blitter ready (qfi=%u queue=%p)", queue_family_index, (void*)queue);
    return 0;
}

static void destroy_shadow(ww_vk_blitter_t* b) {
    if (b->shadow_image != VK_NULL_HANDLE) {
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
    }
    if (b->shadow_mem != VK_NULL_HANDLE) {
        b->backend.vkFreeMemory(b->backend.device, b->shadow_mem, NULL);
        b->shadow_mem = VK_NULL_HANDLE;
    }
    if (b->shadow_export_fd >= 0) {
        close(b->shadow_export_fd);
        b->shadow_export_fd = -1;
    }
    b->shadow_export_n_planes = 0;
    b->shadow_w               = 0;
    b->shadow_h               = 0;
    b->shadow_fmt             = VK_FORMAT_UNDEFINED;
}

/* Push the current shadow onto the deferred-destroy queue without
 * touching it. Used by ensure_shadow on size/format change so that
 * Qt RHI's still-live VkImageView (deleted via Qt's release queue at
 * the next frame boundary) doesn't trip
 * VUID-vkDestroyImage-image-01000. When the queue is full, force the
 * oldest entry's countdown to zero and tick once to free a slot —
 * this only sacrifices the extra frame of jitter slack, never falls
 * back to vkDeviceWaitIdle (which is precisely the race we deferred
 * destruction to avoid). */
static void enqueue_shadow_destroy(ww_vk_blitter_t* b) {
    if (b->shadow_image == VK_NULL_HANDLE && b->shadow_mem == VK_NULL_HANDLE) {
        return;
    }
    const int cap = (int)(sizeof(b->pending_shadow_destroy) / sizeof(b->pending_shadow_destroy[0]));
    if (b->pending_shadow_destroy_count >= cap) {
        int oldest = 0;
        for (int i = 1; i < b->pending_shadow_destroy_count; i++) {
            if (b->pending_shadow_destroy[i].frames_remaining <
                b->pending_shadow_destroy[oldest].frames_remaining) {
                oldest = i;
            }
        }
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: pending_shadow_destroy queue full; forcing "
               "oldest entry (frames_remaining=%d) to fire now",
               b->pending_shadow_destroy[oldest].frames_remaining);
        b->pending_shadow_destroy[oldest].frames_remaining = 1;
        ww_vk_blitter_tick_pending_destroys(b);
    }
    int idx                               = b->pending_shadow_destroy_count++;
    b->pending_shadow_destroy[idx].image  = b->shadow_image;
    b->pending_shadow_destroy[idx].memory = b->shadow_mem;
    /* 2 frames: Qt RHI typically releases on next frame boundary,
     * one extra frame of slack absorbs jitter. */
    b->pending_shadow_destroy[idx].frames_remaining = 2;
    b->shadow_image                                 = VK_NULL_HANDLE;
    b->shadow_mem                                   = VK_NULL_HANDLE;
    /* Exporting consumers hold their own dup; releasing our fd here
     * just drops the lib's reference and doesn't unmap the dmabuf. */
    if (b->shadow_export_fd >= 0) {
        close(b->shadow_export_fd);
        b->shadow_export_fd = -1;
    }
    b->shadow_export_n_planes = 0;
    b->shadow_w               = 0;
    b->shadow_h               = 0;
    b->shadow_fmt             = VK_FORMAT_UNDEFINED;
}

void ww_vk_blitter_tick_pending_destroys(ww_vk_blitter_t* b) {
    if (! b || b->pending_shadow_destroy_count == 0) return;
    int j = 0;
    for (int i = 0; i < b->pending_shadow_destroy_count; i++) {
        if (--b->pending_shadow_destroy[i].frames_remaining > 0) {
            /* Still parked; keep it. */
            if (j != i) b->pending_shadow_destroy[j] = b->pending_shadow_destroy[i];
            j++;
            continue;
        }
        /* Countdown elapsed: Qt RHI has had at least one frame
         * boundary to process its release queue, so the dependent
         * VkImageView is gone. Safe to destroy now. */
        if (b->pending_shadow_destroy[i].image != VK_NULL_HANDLE) {
            b->backend.vkDestroyImage(b->backend.device, b->pending_shadow_destroy[i].image, NULL);
        }
        if (b->pending_shadow_destroy[i].memory != VK_NULL_HANDLE) {
            b->backend.vkFreeMemory(b->backend.device, b->pending_shadow_destroy[i].memory, NULL);
        }
    }
    b->pending_shadow_destroy_count = j;
}

int ww_vk_blitter_ensure_shadow(ww_vk_blitter_t* b, uint32_t w, uint32_t h, VkFormat fmt) {
    if (! b || ! b->initialized) return -EINVAL;
    if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) return -EINVAL;
    if (b->shadow_image != VK_NULL_HANDLE && b->shadow_w == w && b->shadow_h == h &&
        b->shadow_fmt == fmt) {
        return 0;
    }

    /* Drain the in-flight copies referencing the old shadow before
     * tearing it down. This is the one place a CPU wait is acceptable:
     * shadow replacement only happens on a size/format change. Bounded
     * wait: if a fence is wedged the GPU is effectively hung — Qt RHI's
     * own next submit will hit the same hang and trip DEVICE_LOST, which
     * triggers sceneGraphInvalidated → cleanup → blitter shutdown.
     * Stalling for 2s here is bounded recovery time, not infinite. */
    static const uint64_t WW_SHADOW_DRAIN_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (ww_vk_blitter_drain(b, WW_SHADOW_DRAIN_NS) != 0) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: shadow-drain fence wait timed out (>2s); "
               "GPU likely hung, leaving slots armed and bailing — "
               "sceneGraphInvalidated will recover us");
        /* Cannot vkResetFences on an in-flight fence (UB), nor
         * vkDestroyFence / vkFreeCommandBuffers on resources still
         * referenced by an in-flight submission. Bail out, keep the
         * slots armed, leak old shadow until shutdown. */
        return -EIO;
    }
    /* Defer the actual vkDestroyImage to a later frame so Qt RHI's
     * released VkImageView (which Qt processes async on its release
     * queue at frame boundaries) doesn't trip
     * VUID-vkDestroyImage-image-01000. */
    enqueue_shadow_destroy(b);

    VkImageCreateInfo ici = {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = fmt,
        .extent                = { w, h, 1 },
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices   = &b->backend.queue_family_index,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = b->backend.vkCreateImage(b->backend.device, &ici, NULL, &b->shadow_image);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateImage(shadow %ux%u fmt=%d) failed: %s",
               w,
               h,
               (int)fmt,
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkMemoryRequirements req;
    b->backend.vkGetImageMemoryRequirements(b->backend.device, b->shadow_image, &req);

    /* Some integrated GPUs only expose HOST_VISIBLE for the bits we
     * need; fall back to "any" matching type when DEVICE_LOCAL fails. */
    uint32_t mtype =
        pick_memory_type(&b->backend, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = pick_memory_type(&b->backend, req.memoryTypeBits, 0);
    }
    if (mtype == UINT32_MAX) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: no memory type for shadow image "
               "(typeBits=0x%08x)",
               req.memoryTypeBits);
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = b->backend.vkAllocateMemory(b->backend.device, &mai, NULL, &b->shadow_mem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateMemory(shadow size=%" PRIu64 ") failed: %s",
               (uint64_t)req.size,
               ww_vk_result_str(vr));
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }
    vr = b->backend.vkBindImageMemory(b->backend.device, b->shadow_image, b->shadow_mem, 0);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBindImageMemory(shadow) failed: %s",
               ww_vk_result_str(vr));
        destroy_shadow(b);
        return -EIO;
    }

    b->shadow_w   = w;
    b->shadow_h   = h;
    b->shadow_fmt = fmt;
    /* Fresh image; layout is VK_IMAGE_LAYOUT_UNDEFINED. Until the
     * first blit transitions it to SHADER_READ_ONLY_OPTIMAL, the
     * host MUST NOT expose this shadow as a sampled texture. */
    b->shadow_has_content = false;
    ww_log(WAYWALLEN_LOG_INFO,
           "vk blitter: shadow %ux%u fmt=%d ready (mtype=%u size=%" PRIu64 ")",
           w,
           h,
           (int)fmt,
           mtype,
           (uint64_t)req.size);
    return 0;
}

/* Lazily resolve vkGetMemoryFdKHR + vkGetImageSubresourceLayout the
 * first time the relay path needs them. Both are core / KHR entry
 * points on a device with VK_KHR_external_memory_fd. */
static int resolve_export_fns(ww_vk_blitter_t* b) {
    PFN_vkGetDeviceProcAddr gdpa   = b->backend.vkGetDeviceProcAddr;
    VkDevice                device = b->backend.device;
    if (! b->vkGetMemoryFdKHR) {
        b->vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)gdpa(device, "vkGetMemoryFdKHR");
    }
    if (! b->vkGetImageSubresourceLayout) {
        b->vkGetImageSubresourceLayout =
            (PFN_vkGetImageSubresourceLayout)gdpa(device, "vkGetImageSubresourceLayout");
    }
    if (! b->vkGetMemoryFdKHR || ! b->vkGetImageSubresourceLayout) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: export fns missing (KHR_external_memory_fd?)");
        return -ENOSYS;
    }
    return 0;
}

int ww_vk_blitter_ensure_shadow_exportable(ww_vk_blitter_t* b, uint32_t w, uint32_t h,
                                           VkFormat fmt) {
    if (! b || ! b->initialized) return -EINVAL;
    if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) return -EINVAL;
    /* Same shape + already exportable -> nothing to do. */
    if (b->shadow_image != VK_NULL_HANDLE && b->shadow_w == w && b->shadow_h == h &&
        b->shadow_fmt == fmt && b->shadow_export_fd >= 0) {
        return 0;
    }

    int rc = resolve_export_fns(b);
    if (rc != 0) return rc;

    /* Drain in-flight copies referencing the old shadow first. Same
     * bounded-wait shape as `ensure_shadow`. */
    static const uint64_t WW_SHADOW_DRAIN_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (ww_vk_blitter_drain(b, WW_SHADOW_DRAIN_NS) != 0) {
        ww_log(WAYWALLEN_LOG_WARN, "vk blitter: exportable-shadow drain wait timed out");
        return -EIO;
    }
    enqueue_shadow_destroy(b);

    VkExternalMemoryImageCreateInfo ext_img = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext       = &ext_img,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = fmt,
        .extent      = { w, h, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        /* LINEAR + DRM_FORMAT_MOD_LINEAR is the only safe path without
         * pulling in modifier negotiation; produces a single-plane
         * dmabuf every consumer can import. */
        .tiling                = VK_IMAGE_TILING_LINEAR,
        .usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices   = &b->backend.queue_family_index,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = b->backend.vkCreateImage(b->backend.device, &ici, NULL, &b->shadow_image);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateImage(exportable %ux%u fmt=%d) failed: %s",
               w,
               h,
               (int)fmt,
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkMemoryRequirements req;
    b->backend.vkGetImageMemoryRequirements(b->backend.device, b->shadow_image, &req);

    uint32_t mtype =
        pick_memory_type(&b->backend, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        /* Some integrated GPUs only expose HOST_VISIBLE for LINEAR. */
        mtype = pick_memory_type(&b->backend, req.memoryTypeBits, 0);
    }
    if (mtype == UINT32_MAX) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: no memory type for exportable shadow "
               "(typeBits=0x%08x)",
               req.memoryTypeBits);
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }

    VkExportMemoryAllocateInfo exp_mem = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    /* Dedicated allocation is mandated for many external-memory drivers
     * and harmless otherwise. */
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &exp_mem,
        .image = b->shadow_image,
    };
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &ded,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = b->backend.vkAllocateMemory(b->backend.device, &mai, NULL, &b->shadow_mem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkAllocateMemory(exportable size=%" PRIu64 ") failed: %s",
               (uint64_t)req.size,
               ww_vk_result_str(vr));
        b->backend.vkDestroyImage(b->backend.device, b->shadow_image, NULL);
        b->shadow_image = VK_NULL_HANDLE;
        return -EIO;
    }
    vr = b->backend.vkBindImageMemory(b->backend.device, b->shadow_image, b->shadow_mem, 0);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBindImageMemory(exportable) failed: %s",
               ww_vk_result_str(vr));
        destroy_shadow(b);
        return -EIO;
    }

    /* Single LINEAR plane: query its row pitch + offset. */
    VkImageSubresource sub = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel   = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    memset(&layout, 0, sizeof(layout));
    b->vkGetImageSubresourceLayout(b->backend.device, b->shadow_image, &sub, &layout);

    VkMemoryGetFdInfoKHR gfd = {
        .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory     = b->shadow_mem,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int dmabuf_fd = -1;
    vr            = b->vkGetMemoryFdKHR(b->backend.device, &gfd, &dmabuf_fd);
    if (vr != VK_SUCCESS || dmabuf_fd < 0) {
        ww_log(
            WAYWALLEN_LOG_ERROR, "vk blitter: vkGetMemoryFdKHR failed: %s", ww_vk_result_str(vr));
        destroy_shadow(b);
        return -EIO;
    }

    b->shadow_w               = w;
    b->shadow_h               = h;
    b->shadow_fmt             = fmt;
    b->shadow_export_fd       = dmabuf_fd;
    b->shadow_export_n_planes = 1;
    /* rowPitch fits in uint32_t for any realistic surface; explicit cast
     * keeps -Wconversion silent. */
    b->shadow_export_strides[0] = (uint32_t)layout.rowPitch;
    b->shadow_export_offsets[0] = (uint64_t)layout.offset;
    b->shadow_export_modifier   = 0ull; /* DRM_FORMAT_MOD_LINEAR */
    b->shadow_has_content       = false;

    ww_log(WAYWALLEN_LOG_INFO,
           "vk blitter: exportable shadow %ux%u fmt=%d ready "
           "(mtype=%u size=%" PRIu64 " fd=%d stride=%u offset=%" PRIu64 ")",
           w,
           h,
           (int)fmt,
           mtype,
           (uint64_t)req.size,
           dmabuf_fd,
           b->shadow_export_strides[0],
           b->shadow_export_offsets[0]);
    return 0;
}

int ww_vk_blitter_get_export(const ww_vk_blitter_t* b, int* out_fd, uint32_t* out_n_planes,
                             uint32_t out_strides[4], uint64_t out_offsets[4],
                             uint64_t* out_modifier) {
    if (! b || ! out_fd || ! out_n_planes || ! out_strides || ! out_offsets || ! out_modifier) {
        return -EINVAL;
    }
    if (b->shadow_export_fd < 0 || b->shadow_export_n_planes == 0) {
        return -EINVAL;
    }
    *out_fd       = b->shadow_export_fd;
    *out_n_planes = b->shadow_export_n_planes;
    for (uint32_t i = 0; i < b->shadow_export_n_planes && i < 4u; i++) {
        out_strides[i] = b->shadow_export_strides[i];
        out_offsets[i] = b->shadow_export_offsets[i];
    }
    *out_modifier = b->shadow_export_modifier;
    return 0;
}

static VkImageSubresourceRange full_color_range(void) {
    VkImageSubresourceRange r = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    return r;
}

static void close_release_syncobj_fd(int* release_syncobj_fd) {
    if (release_syncobj_fd && *release_syncobj_fd >= 0) {
        close(*release_syncobj_fd);
        *release_syncobj_fd = -1;
    }
}

static void close_acquire_sync_fd(int* acquire_sync_fd) {
    if (acquire_sync_fd && *acquire_sync_fd >= 0) {
        close(*acquire_sync_fd);
        *acquire_sync_fd = -1;
    }
}

/* Host-signal the release syncobj and close it. Used whenever no GPU work
 * references the source image any more (or never did). */
static void finish_release_syncobj_fd(int* release_syncobj_fd,
                                      const ww_vk_blit_release_ops_t* release_ops,
                                      void* release_user_data, const char* context) {
    if (! release_syncobj_fd || *release_syncobj_fd < 0) return;

    if (release_ops && release_ops->signal_release) {
        int rc = release_ops->signal_release(*release_syncobj_fd, release_user_data);
        if (rc != 0) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: release syncobj signal failed rc=%d context=%s",
                   rc,
                   context ? context : "unknown");
        }
    }
    close_release_syncobj_fd(release_syncobj_fd);
}

/*
 * A binary semaphore that was signaled by a submission but never consumed
 * by an export (or a queue wait) stays signaled, and signaling it again from
 * the slot's next submission would be invalid. There is no host-side reset
 * for binary semaphores, so replace it. Only legal once the slot's fence has
 * signaled, i.e. no submission references the semaphore any more.
 */
static void recreate_export_semaphore(ww_vk_blitter_t* b, ww_vk_blit_slot_t* slot) {
    if (slot->export_sem != VK_NULL_HANDLE) {
        b->backend.vkDestroySemaphore(b->backend.device, slot->export_sem, NULL);
        slot->export_sem = VK_NULL_HANDLE;
    }
    VkExportSemaphoreCreateInfo exp_sem = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exp_sem,
    };
    VkResult vr = b->backend.vkCreateSemaphore(b->backend.device, &sci, NULL, &slot->export_sem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: vkCreateSemaphore(export) recreate failed: %s",
               ww_vk_result_str(vr));
        slot->export_sem = VK_NULL_HANDLE;
    }
    slot->export_sem_stale = false;
}

/*
 * Fallback for drivers where the copy's completion cannot be exported as a
 * sync_file: wait for the slot's fence on the CPU (bounded) and then
 * host-signal the release. This is the pre-ring behaviour and is only taken
 * after an export failure, so the steady state never blocks the caller.
 */
static void finish_release_after_cpu_wait(ww_vk_blitter_t* b, ww_vk_blit_slot_t* slot,
                                          int* release_syncobj_fd,
                                          const ww_vk_blit_release_ops_t* release_ops,
                                          void* release_user_data) {
    /* 2 s is well above any plausible copy duration on a healthy GPU; a
     * longer wait means the device is wedged and the daemon's own release
     * reaper will force the slot free. */
    static const uint64_t WW_BLIT_FALLBACK_WAIT_NS = 2ull * 1000ull * 1000ull * 1000ull;
    VkResult vr = b->vkWaitForFences(
        b->backend.device, 1, &slot->fence, VK_TRUE, WW_BLIT_FALLBACK_WAIT_NS);
    if (vr == VK_SUCCESS) {
        b->vkResetFences(b->backend.device, 1, &slot->fence);
        slot->armed = false;
        if (slot->export_sem_stale) recreate_export_semaphore(b, slot);
        finish_release_syncobj_fd(
            release_syncobj_fd, release_ops, release_user_data, "shadow-copy-complete-cpu-wait");
        return;
    }
    if (vr == VK_TIMEOUT) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: fallback fence wait timed out (>%llu ms); "
               "leaving the source slot for the daemon's release reaper",
               (unsigned long long)(WW_BLIT_FALLBACK_WAIT_NS / 1000000ull));
    } else {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: fallback vkWaitForFences failed: %s",
               ww_vk_result_str(vr));
    }
    /* The GPU may still read the source image: do not host-signal. */
    close_release_syncobj_fd(release_syncobj_fd);
}

/*
 * Export the pending copy's completion and hand it to both readers that
 * must order against the copy: the shadow dmabuf's dma_resv (GSK samples
 * the shadow through kernel implicit sync) and the daemon's release syncobj
 * (the producer's release gate waits on it before rendering into the source
 * slot again). Neither wait happens on this thread.
 *
 * Returns 0 when the release was attached to the fence (or no release
 * syncobj was supplied), -ENOSYS when the export itself failed (the slot's
 * export semaphore is now stale), -EIO when only the release attachment
 * failed. Callers fall back to a CPU wait for the release on any failure.
 */
static int publish_copy_completion(ww_vk_blitter_t* b, ww_vk_blit_slot_t* slot,
                                   int release_syncobj_fd,
                                   const ww_vk_blit_release_ops_t* release_ops,
                                   void* release_user_data) {
    int                     sync_fd = -1;
    VkSemaphoreGetFdInfoKHR get     = {
        .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore  = slot->export_sem,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkResult er = b->vkGetSemaphoreFdKHR(b->backend.device, &get, &sync_fd);
    if (er != VK_SUCCESS || sync_fd < 0) {
        if (! b->sync_file_export_unavailable) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: vkGetSemaphoreFdKHR(SYNC_FD) failed: %s; "
                   "falling back to CPU fence waits for release signaling",
                   ww_vk_result_str(er));
            b->sync_file_export_unavailable = true;
        }
        slot->export_sem_stale = true;
        return -ENOSYS;
    }
    /* SYNC_FD export has copy transference and resets the semaphore. */
    slot->export_sem_stale = false;

    if (b->shadow_export_fd >= 0) {
        struct ww_dma_buf_sync_file sf = {
            .flags = DMA_BUF_SYNC_WRITE,
            .fd    = sync_fd,
        };
        if (ioctl(b->shadow_export_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &sf) != 0) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: dma_buf import_sync_file(fd=%d shadow=%d) failed: %s",
                   sync_fd,
                   b->shadow_export_fd,
                   strerror(errno));
        }
    }

    int rc = 0;
    if (release_syncobj_fd >= 0) {
        rc = -ENOSYS;
        if (release_ops && release_ops->import_release_sync_file) {
            rc = release_ops->import_release_sync_file(
                release_syncobj_fd, sync_fd, release_user_data);
        }
        if (rc != 0) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: attaching copy fence to release syncobj failed rc=%d; "
                   "falling back to a CPU wait for this frame",
                   rc);
            rc = -EIO;
        }
    }
    close(sync_fd);
    return rc;
}

/*
 * Pick the oldest ring slot. Returns the slot when it is idle (or its copy
 * has completed and it was recycled), NULL when every slot is still
 * executing and the frame has to be dropped. `*out_device_failed` is set
 * when the fence query reports a device fault.
 */
static ww_vk_blit_slot_t* acquire_ring_slot(ww_vk_blitter_t* b, bool* out_device_failed) {
    *out_device_failed      = false;
    ww_vk_blit_slot_t* slot = &b->ring[b->next_slot];
    if (slot->armed) {
        VkResult status = b->vkGetFenceStatus(b->backend.device, slot->fence);
        if (status == VK_NOT_READY) return NULL;
        if (status != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_ERROR,
                   "vk blitter: vkGetFenceStatus failed: %s",
                   ww_vk_result_str(status));
            *out_device_failed = true;
            return NULL;
        }
        b->vkResetFences(b->backend.device, 1, &slot->fence);
        slot->armed = false;
    }
    if (slot->export_sem_stale) recreate_export_semaphore(b, slot);
    return slot;
}

static void log_dropped_frame(ww_vk_blitter_t* b) {
    b->dropped_frames++;
    /* First drop, then every 64th: enough to see sustained overload in the
     * journal without flooding it at frame rate. */
    if (b->dropped_frames == 1 || b->dropped_frames - b->dropped_frames_logged >= 64) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: all %u shadow copies still in flight; dropped frame "
               "(total dropped=%" PRIu64 ")",
               (unsigned)WW_VK_BLIT_RING_SIZE,
               b->dropped_frames);
        b->dropped_frames_logged = b->dropped_frames;
    }
}

int ww_vk_blitter_blit(ww_vk_blitter_t* b, VkImage imported, uint32_t w, uint32_t h,
                       int acquire_sync_fd, int release_syncobj_fd,
                       const ww_vk_blit_release_ops_t* release_ops, void* release_user_data) {
    if (! b || ! b->initialized || b->shadow_image == VK_NULL_HANDLE) {
        close_acquire_sync_fd(&acquire_sync_fd);
        finish_release_syncobj_fd(
            &release_syncobj_fd, release_ops, release_user_data, "invalid-blitter");
        return -EINVAL;
    }
    if (imported == VK_NULL_HANDLE || w == 0 || h == 0) {
        close_acquire_sync_fd(&acquire_sync_fd);
        finish_release_syncobj_fd(
            &release_syncobj_fd, release_ops, release_user_data, "invalid-frame");
        return -EINVAL;
    }
    if (w != b->shadow_w || h != b->shadow_h) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: size mismatch (frame=%ux%u shadow=%ux%u)",
               w,
               h,
               b->shadow_w,
               b->shadow_h);
        close_acquire_sync_fd(&acquire_sync_fd);
        finish_release_syncobj_fd(
            &release_syncobj_fd, release_ops, release_user_data, "size-mismatch");
        return -EINVAL;
    }

    bool               device_failed = false;
    ww_vk_blit_slot_t* slot          = acquire_ring_slot(b, &device_failed);
    if (! slot) {
        /* Nothing was submitted for this frame, so the source slot can go
         * straight back to the producer. The shadow keeps the last frame
         * whose copy landed, which is the correct image to keep showing
         * while the GPU catches up. */
        close_acquire_sync_fd(&acquire_sync_fd);
        finish_release_syncobj_fd(&release_syncobj_fd,
                                  release_ops,
                                  release_user_data,
                                  device_failed ? "fence-status-failed" : "blit-ring-full");
        if (device_failed) return -EIO;
        log_dropped_frame(b);
        return WW_VK_BLIT_DROPPED;
    }
    /* Only signal the export semaphore when the export path is usable;
     * otherwise every submission would leave it signaled. */
    const bool use_export_sem =
        ! b->sync_file_export_unavailable && slot->export_sem != VK_NULL_HANDLE;

    /* The queue wait consumed the previous temporary payload when this
     * slot's earlier submission executed, so importing again is legal. The
     * import consumes the fd on success. */
    VkSemaphore wait_sem = VK_NULL_HANDLE;
    if (acquire_sync_fd >= 0) {
        int import_rc = ww_vk_import_sync_fd(&b->backend, slot->acquire_sem, acquire_sync_fd);
        if (import_rc != 0) {
            close_acquire_sync_fd(&acquire_sync_fd);
            finish_release_syncobj_fd(
                &release_syncobj_fd, release_ops, release_user_data, "acquire-import-failed");
            return -EIO;
        }
        acquire_sync_fd = -1;
        wait_sem        = slot->acquire_sem;
    }

    b->vkResetCommandPool(b->backend.device, slot->pool, 0);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = b->vkBeginCommandBuffer(slot->cb, &bi);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBeginCommandBuffer failed: %s",
               ww_vk_result_str(vr));
        finish_release_syncobj_fd(
            &release_syncobj_fd, release_ops, release_user_data, "begin-command-buffer-failed");
        return -EIO;
    }

    /* The producer releases DMA-BUF images to FOREIGN in GENERAL layout.
     * Acquiring from UNDEFINED would legally discard contents on drivers that
     * keep modifier metadata such as DCC, so mirror waywallen's ownership
     * boundary exactly: FOREIGN/GENERAL -> local TRANSFER_SRC, then release
     * the image back to FOREIGN/GENERAL after the copy. */
    VkImageMemoryBarrier in_bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = b->backend.queue_family_index,
        .image               = imported,
        .subresourceRange    = full_color_range(),
    };
    /* Shadow: discard prior layout (we overwrite the whole image).
     * Visibility to the external reader (GSK) is published after submit
     * via DMA_BUF_IOCTL_IMPORT_SYNC_FILE, not via this barrier. */
    VkImageMemoryBarrier shadow_bar0 = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = b->shadow_image,
        .subresourceRange    = full_color_range(),
    };
    VkImageMemoryBarrier pre_bars[2] = { in_bar, shadow_bar0 };
    /* Several copies into the single shadow can be in flight. They execute
     * in submission order on this queue, and the TRANSFER->TRANSFER
     * dependency below orders this copy's writes after the previous one's,
     * so an older copy never lands on top of a newer frame. */
    b->vkCmdPipelineBarrier(slot->cb,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT |
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            2,
                            pre_bars);

    VkImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .dstOffset = {0, 0, 0},
        .extent = {w, h, 1},
    };
    b->vkCmdCopyImage(slot->cb,
                      imported,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      b->shadow_image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1,
                      &region);

    VkImageMemoryBarrier out_bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask       = 0,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = b->backend.queue_family_index,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image               = imported,
        .subresourceRange    = full_color_range(),
    };
    /* Plain layout transition to SHADER_READ_ONLY_OPTIMAL. The external
     * reader (GSK) gets write-fence visibility via the dma_resv
     * injection after submit, so QUEUE_FAMILY_IGNORED is correct here. */
    VkImageMemoryBarrier shadow_bar1 = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = b->shadow_image,
        .subresourceRange    = full_color_range(),
    };
    VkImageMemoryBarrier post_bars[2] = { out_bar, shadow_bar1 };
    b->vkCmdPipelineBarrier(slot->cb,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            2,
                            post_bars);

    vr = b->vkEndCommandBuffer(slot->cb);
    if (vr != VK_SUCCESS) {
        ww_log(
            WAYWALLEN_LOG_ERROR, "vk blitter: vkEndCommandBuffer failed: %s", ww_vk_result_str(vr));
        finish_release_syncobj_fd(
            &release_syncobj_fd, release_ops, release_user_data, "end-command-buffer-failed");
        return -EIO;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo         si         = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = (wait_sem != VK_NULL_HANDLE) ? 1u : 0u,
        .pWaitSemaphores      = (wait_sem != VK_NULL_HANDLE) ? &wait_sem : NULL,
        .pWaitDstStageMask    = (wait_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &slot->cb,
        .signalSemaphoreCount = use_export_sem ? 1u : 0u,
        .pSignalSemaphores    = use_export_sem ? &slot->export_sem : NULL,
    };
    /* Don't try to signal release_syncobj_fd from this submit via
     * vkImportSemaphoreFdKHR(OPAQUE_FD): NVIDIA rejects drm_syncobj
     * fds with "Failed to allocate semaphore device memory". Export
     * the signal semaphore as a sync_file instead and attach it to the
     * syncobj with a kernel ioctl below — works on every driver. */
    vr = b->vkQueueSubmit(b->queue, 1, &si, slot->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkQueueSubmit failed: %s", ww_vk_result_str(vr));
        finish_release_syncobj_fd(
            &release_syncobj_fd, release_ops, release_user_data, "queue-submit-failed");
        return -EIO;
    }
    slot->armed  = true;
    b->next_slot = (b->next_slot + 1u) % WW_VK_BLIT_RING_SIZE;

    if (! use_export_sem) {
        /* Legacy path: nothing can wait on the GPU for us, so block here
         * exactly like the original single-buffered blitter did. */
        finish_release_after_cpu_wait(b, slot, &release_syncobj_fd, release_ops, release_user_data);
        if (! slot->armed) b->shadow_has_content = true;
        return WW_VK_BLIT_SUBMITTED;
    }

    /* The copy is queued behind the producer's frame on the GPU. Everything
     * that must observe its completion waits on the exported fence, so this
     * thread returns to its main loop immediately. Sampling the shadow is
     * safe as soon as the dma_resv fence is attached in
     * publish_copy_completion(): GSK orders its read after the pending write. */
    const int publish_rc =
        publish_copy_completion(b, slot, release_syncobj_fd, release_ops, release_user_data);
    if (publish_rc == 0) {
        b->shadow_has_content = true;
        close_release_syncobj_fd(&release_syncobj_fd);
        return WW_VK_BLIT_SUBMITTED;
    }
    /* -EIO: the export worked and the shadow fence is attached; only the
     * release attachment failed. -ENOSYS: no fence at all, so the shadow is
     * only known-good once the CPU wait below has seen the copy finish. */
    if (publish_rc == -EIO) b->shadow_has_content = true;
    finish_release_after_cpu_wait(b, slot, &release_syncobj_fd, release_ops, release_user_data);
    if (! slot->armed) b->shadow_has_content = true;
    return WW_VK_BLIT_SUBMITTED;
}

static bool any_slot_objects_live(const ww_vk_blitter_t* b) {
    for (unsigned i = 0; i < WW_VK_BLIT_RING_SIZE; i++) {
        const ww_vk_blit_slot_t* slot = &b->ring[i];
        if (slot->pool != VK_NULL_HANDLE || slot->fence != VK_NULL_HANDLE ||
            slot->acquire_sem != VK_NULL_HANDLE || slot->export_sem != VK_NULL_HANDLE) {
            return true;
        }
    }
    return false;
}

void ww_vk_blitter_shutdown(ww_vk_blitter_t* b) {
    if (! b) return;
    if (! b->initialized && ! any_slot_objects_live(b) && b->shadow_image == VK_NULL_HANDLE) {
        memset(b, 0, sizeof(*b));
        return;
    }
    /* In-flight copies reference the slots' command buffers, semaphores and
     * the shadow; wait for the whole device before destroying any of them. */
    if (b->backend.device != VK_NULL_HANDLE && b->backend.vkDeviceWaitIdle) {
        b->backend.vkDeviceWaitIdle(b->backend.device);
    }
    /* Drain anything still parked in the deferred-destroy queue.
     * vkDeviceWaitIdle above ensures no in-flight cmd buffer
     * references these; we don't strictly need Qt RHI's view to be
     * gone by now (the host is tearing the whole session down) but
     * if it isn't, validation will yell — that's acceptable on
     * shutdown. */
    for (int i = 0; i < b->pending_shadow_destroy_count; i++) {
        if (b->pending_shadow_destroy[i].image != VK_NULL_HANDLE) {
            b->backend.vkDestroyImage(b->backend.device, b->pending_shadow_destroy[i].image, NULL);
        }
        if (b->pending_shadow_destroy[i].memory != VK_NULL_HANDLE) {
            b->backend.vkFreeMemory(b->backend.device, b->pending_shadow_destroy[i].memory, NULL);
        }
    }
    b->pending_shadow_destroy_count = 0;
    destroy_shadow(b);
    for (unsigned i = 0; i < WW_VK_BLIT_RING_SIZE; i++) {
        destroy_slot_objects(b, &b->ring[i]);
    }
    ww_vk_backend_unload(&b->backend);
    memset(b, 0, sizeof(*b));
}

#endif /* WW_HAVE_VULKAN */
