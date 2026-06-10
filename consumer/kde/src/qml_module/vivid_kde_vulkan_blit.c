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
    RESOLVE(vkWaitForFences, PFN_vkWaitForFences, "vkWaitForFences");
    RESOLVE(vkQueueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");

#    undef RESOLVE
    return 0;
}

static int create_cmd_objects(ww_vk_blitter_t* b) {
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        /* No flags: we recycle the whole pool each frame via vkResetCommandPool. */
        .flags            = 0,
        .queueFamilyIndex = b->backend.queue_family_index,
    };
    VkResult vr = b->vkCreateCommandPool(b->backend.device, &pci, NULL, &b->pool);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateCommandPool failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }

    VkCommandBufferAllocateInfo cbi = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = b->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vr = b->vkAllocateCommandBuffers(b->backend.device, &cbi, &b->cb);
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
    vr = b->vkCreateFence(b->backend.device, &fci, NULL, &b->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkCreateFence failed: %s", ww_vk_result_str(vr));
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
    vr = b->backend.vkCreateSemaphore(b->backend.device, &sci, NULL, &b->export_sem);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkCreateSemaphore(export) failed: %s",
               ww_vk_result_str(vr));
        return -EIO;
    }
    b->vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)b->backend.vkGetDeviceProcAddr(
        b->backend.device, "vkGetSemaphoreFdKHR");
    if (! b->vkGetSemaphoreFdKHR) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkGetSemaphoreFdKHR not resolvable");
        return -ENOSYS;
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

    /* Drain any in-flight blit referencing the old shadow before
     * tearing it down. Bounded wait: if the fence is wedged the GPU
     * is effectively hung — Qt RHI's own next submit will hit the
     * same hang and trip DEVICE_LOST, which triggers
     * sceneGraphInvalidated → cleanup → blitter shutdown. Stalling
     * for 2s per frame here is bounded recovery time, not infinite. */
    static const uint64_t WW_SHADOW_DRAIN_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (b->fence_armed) {
        VkResult vrw =
            b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_SHADOW_DRAIN_NS);
        if (vrw == VK_TIMEOUT) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: shadow-drain fence wait timed out (>2s); "
                   "GPU likely hung, leaving fence armed and bailing — "
                   "sceneGraphInvalidated will recover us");
            /* Cannot vkResetFences on an in-flight fence (UB), nor
             * vkDestroyFence / vkFreeCommandBuffers on resources still
             * referenced by an in-flight submission. Bail out, keep
             * fence_armed=true, leak old shadow until shutdown. */
            return -EIO;
        }
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
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

    /* Drain in-flight blit referencing the old shadow first. Same
     * bounded-wait shape as `ensure_shadow`. */
    static const uint64_t WW_SHADOW_DRAIN_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (b->fence_armed) {
        VkResult vrw =
            b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_SHADOW_DRAIN_NS);
        if (vrw == VK_TIMEOUT) {
            ww_log(WAYWALLEN_LOG_WARN, "vk blitter: exportable-shadow drain wait timed out");
            return -EIO;
        }
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
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

int ww_vk_blitter_blit(ww_vk_blitter_t* b, VkImage imported, uint32_t w, uint32_t h,
                       VkSemaphore acquire_sem, int release_syncobj_fd) {
    if (! b || ! b->initialized || b->shadow_image == VK_NULL_HANDLE) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EINVAL;
    }
    if (imported == VK_NULL_HANDLE || w == 0 || h == 0) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EINVAL;
    }
    if (w != b->shadow_w || h != b->shadow_h) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: size mismatch (frame=%ux%u shadow=%ux%u)",
               w,
               h,
               b->shadow_w,
               b->shadow_h);
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EINVAL;
    }

    /* 2 s is well above any plausible blit duration (hundreds of µs
     * even on AMD with DCC). Producer death between FrameReady and
     * its acquire dma_fence signal is the only realistic path to a
     * stuck wait — we'd rather log + bail than freeze the QML render
     * thread for 10s+ until the kernel TDR fires. */
    static const uint64_t WW_BLIT_FENCE_WAIT_NS = 2ull * 1000ull * 1000ull * 1000ull;
    if (b->fence_armed) {
        VkResult vrw =
            b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_BLIT_FENCE_WAIT_NS);
        if (vrw == VK_TIMEOUT) {
            /* Fence is still in flight; vkResetFences would be UB and
             * submitting a new cmd buffer to the same fence is forbidden.
             * Bail out, keep fence_armed=true. The next call retries. */
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: pre-submit fence wait timed out (>%llu ms); "
                   "skipping this blit",
                   (unsigned long long)(WW_BLIT_FENCE_WAIT_NS / 1000000ull));
            if (release_syncobj_fd >= 0) close(release_syncobj_fd);
            return -EIO;
        }
        if (vrw != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: vkWaitForFences failed: %s",
                   ww_vk_result_str(vrw));
        }
        b->vkResetFences(b->backend.device, 1, &b->fence);
        b->fence_armed = false;
    }
    b->vkResetCommandPool(b->backend.device, b->pool, 0);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = b->vkBeginCommandBuffer(b->cb, &bi);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkBeginCommandBuffer failed: %s",
               ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }

    /* Acquire imported image: implicit acquire from EXTERNAL via
     * UNDEFINED layout. After acquire_sem signals, the producer's
     * GPU work is visible, so this barrier is safe. */
    VkImageMemoryBarrier in_bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = imported,
        .subresourceRange    = full_color_range(),
    };
    /* Shadow: discard prior layout (we overwrite the whole image).
     * Visibility to the external reader (GSK) is published after submit
     * via DMA_BUF_IOCTL_IMPORT_SYNC_FILE, not via this barrier. */
    VkImageMemoryBarrier shadow_bar0 = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = b->shadow_image,
        .subresourceRange    = full_color_range(),
    };
    VkImageMemoryBarrier pre_bars[2] = { in_bar, shadow_bar0 };
    b->vkCmdPipelineBarrier(b->cb,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
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
    b->vkCmdCopyImage(b->cb,
                      imported,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      b->shadow_image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1,
                      &region);

    /* Plain layout transition to SHADER_READ_ONLY_OPTIMAL. The external
     * reader (GSK) gets write-fence visibility via the dma_resv
     * injection below, so QUEUE_FAMILY_IGNORED is correct here. */
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
    b->vkCmdPipelineBarrier(b->cb,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            1,
                            &shadow_bar1);

    vr = b->vkEndCommandBuffer(b->cb);
    if (vr != VK_SUCCESS) {
        ww_log(
            WAYWALLEN_LOG_ERROR, "vk blitter: vkEndCommandBuffer failed: %s", ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo         si         = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = (acquire_sem != VK_NULL_HANDLE) ? 1u : 0u,
        .pWaitSemaphores      = (acquire_sem != VK_NULL_HANDLE) ? &acquire_sem : NULL,
        .pWaitDstStageMask    = (acquire_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &b->cb,
        .signalSemaphoreCount = (b->export_sem != VK_NULL_HANDLE) ? 1u : 0u,
        .pSignalSemaphores    = (b->export_sem != VK_NULL_HANDLE) ? &b->export_sem : NULL,
    };
    /* Don't try to signal release_syncobj_fd from this submit via
     * vkImportSemaphoreFdKHR(OPAQUE_FD): NVIDIA rejects drm_syncobj
     * fds with "Failed to allocate semaphore device memory". Wait on
     * the fence below and signal the syncobj host-side via
     * waywallen_display_signal_release_syncobj — works on every driver
     * because it's a kernel ioctl. */
    vr = b->vkQueueSubmit(b->queue, 1, &si, b->fence);
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk blitter: vkQueueSubmit failed: %s", ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }
    b->fence_armed = true;

    /* Bounded wait: see WW_BLIT_FENCE_WAIT_NS comment above for the
     * 2 s rationale. On VK_TIMEOUT the fence is still in-flight, so
     * we leave fence_armed=true and let the next blit's pre-submit
     * wait try again — the daemon will time out the buffer slot if
     * we never recover. */
    vr = b->vkWaitForFences(b->backend.device, 1, &b->fence, VK_TRUE, WW_BLIT_FENCE_WAIT_NS);
    if (vr == VK_TIMEOUT) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk blitter: post-submit fence wait timed out (>%llu ms); "
               "shadow may be stale until GPU recovers",
               (unsigned long long)(WW_BLIT_FENCE_WAIT_NS / 1000000ull));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }
    if (vr != VK_SUCCESS) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "vk blitter: vkWaitForFences post-submit failed: %s",
               ww_vk_result_str(vr));
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        return -EIO;
    }
    /* Publish blit's signal as a sync_file → ioctl-import into shadow
     * dmabuf's dma_resv as DMA_BUF_SYNC_WRITE. GSK's later read
     * submission picks it up via kernel implicit DMA-BUF sync, so the
     * long-lived imported VkImage's sampler sees fresh content without
     * us having to rebuild the GdkTexture every frame. Direct copy of
     * the gsk/gpu/gskgpudownloadop.c pattern. */
    if (b->shadow_export_fd >= 0 && b->vkGetSemaphoreFdKHR) {
        int                     sync_fd = -1;
        VkSemaphoreGetFdInfoKHR get     = {
            .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore  = b->export_sem,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkResult er = b->vkGetSemaphoreFdKHR(b->backend.device, &get, &sync_fd);
        if (er == VK_SUCCESS && sync_fd >= 0) {
            struct ww_dma_buf_sync_file sf = {
                .flags = DMA_BUF_SYNC_WRITE,
                .fd    = sync_fd,
            };
            if (ioctl(b->shadow_export_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &sf) != 0) {
                ww_log(WAYWALLEN_LOG_WARN,
                       "vk blitter: dma_buf import_sync_file(fd=%d shadow=%d) "
                       "failed: %s",
                       sync_fd,
                       b->shadow_export_fd,
                       strerror(errno));
            }
            close(sync_fd);
        } else if (er != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk blitter: vkGetSemaphoreFdKHR failed: %s",
                   ww_vk_result_str(er));
        }
    }
    b->vkResetFences(b->backend.device, 1, &b->fence);
    b->fence_armed = false;
    /* Blit transitioned shadow into SHADER_READ_ONLY_OPTIMAL; the
     * host can now safely sample it. */
    b->shadow_has_content = true;

    if (release_syncobj_fd >= 0) close(release_syncobj_fd);
    return 0;
}

void ww_vk_blitter_shutdown(ww_vk_blitter_t* b) {
    if (! b) return;
    if (! b->initialized && b->pool == VK_NULL_HANDLE && b->fence == VK_NULL_HANDLE &&
        b->shadow_image == VK_NULL_HANDLE) {
        memset(b, 0, sizeof(*b));
        return;
    }
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
    if (b->export_sem != VK_NULL_HANDLE && b->backend.vkDestroySemaphore) {
        b->backend.vkDestroySemaphore(b->backend.device, b->export_sem, NULL);
        b->export_sem = VK_NULL_HANDLE;
    }
    if (b->fence != VK_NULL_HANDLE && b->vkDestroyFence) {
        b->vkDestroyFence(b->backend.device, b->fence, NULL);
        b->fence = VK_NULL_HANDLE;
    }
    if (b->pool != VK_NULL_HANDLE && b->vkDestroyCommandPool) {
        b->vkDestroyCommandPool(b->backend.device, b->pool, NULL);
        b->pool = VK_NULL_HANDLE;
    }
    b->cb          = VK_NULL_HANDLE;
    b->fence_armed = false;
    ww_vk_backend_unload(&b->backend);
    memset(b, 0, sizeof(*b));
}

#endif /* WW_HAVE_VULKAN */
