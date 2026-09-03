#define _GNU_SOURCE

#include "vivid_wayland_drm.h"

#include "vivid_wayland_drm_fourcc.h"
#include "vivid_wayland_log.h"
#include "vivid_wayland_util.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifdef WW_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

struct vivid_drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
    uint64_t point;
};

struct vivid_drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};

struct vivid_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};

struct vivid_drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
};

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif

#define VIVID_DRM_IOCTL_SYNCOBJ_DESTROY \
    _IOWR(DRM_IOCTL_BASE, 0xC0, struct vivid_drm_syncobj_destroy)
#define VIVID_DRM_IOCTL_SYNCOBJ_CREATE \
    _IOWR(DRM_IOCTL_BASE, 0xBF, struct vivid_drm_syncobj_create)
#define VIVID_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD \
    _IOWR(DRM_IOCTL_BASE, 0xC1, struct vivid_drm_syncobj_handle)
#define VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE \
    _IOWR(DRM_IOCTL_BASE, 0xC2, struct vivid_drm_syncobj_handle)
#define VIVID_DRM_IOCTL_SYNCOBJ_SIGNAL \
    _IOWR(DRM_IOCTL_BASE, 0xC5, struct vivid_drm_syncobj_array)

/* FD_TO_HANDLE flag: instead of importing a syncobj fd, replace the fence of
 * an existing handle with the fence carried by a sync_file fd. */
#define VIVID_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1u << 0)

bool
vivid_wayland_fourcc_supported(uint32_t fourcc)
{
    return ww_drm_fourcc_supported(fourcc);
}

void
vivid_wayland_append_diag(VividWaylandGpuIdentity* identity, const char* fmt, ...)
{
    if (!identity || !fmt)
        return;
    size_t used = strlen(identity->diagnostics);
    if (used + 1 >= sizeof(identity->diagnostics))
        return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(identity->diagnostics + used,
              sizeof(identity->diagnostics) - used,
              fmt,
              args);
    va_end(args);
}

bool
vivid_wayland_release_syncobj_supported(const char* render_node, const char* context)
{
    if (!render_node || !render_node[0]) {
        vivid_wayland_warn("cannot probe release syncobj support context=%s: missing render node",
                           context ? context : "(none)");
        return false;
    }

    int drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        vivid_wayland_warn("open(%s) for release syncobj probe failed context=%s: %s",
                           render_node,
                           context ? context : "(none)",
                           strerror(errno));
        return false;
    }

    bool supported = false;
    bool created = false;
    bool imported = false;
    struct vivid_drm_syncobj_create create = { 0 };
    struct vivid_drm_syncobj_handle exported = {
        .fd = -1,
    };
    struct vivid_drm_syncobj_handle imported_handle = { 0 };

    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_CREATE(%s) failed context=%s: %s",
                           render_node,
                           context ? context : "(none)",
                           strerror(errno));
        goto cleanup;
    }
    created = true;

    /*
     * Probe the same binary-syncobj operations used for every release fence.
     * CREATE alone is insufficient: a driver may expose syncobjs while the
     * fd import or SIGNAL ioctl required by the frame path is unavailable.
     */
    exported.handle = create.handle;
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &exported) != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD(%s handle=%u) failed context=%s: %s",
                           render_node,
                           create.handle,
                           context ? context : "(none)",
                           strerror(errno));
        goto cleanup;
    }

    imported_handle.fd = exported.fd;
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imported_handle) != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE(%s fd=%d) failed context=%s: %s",
                           render_node,
                           exported.fd,
                           context ? context : "(none)",
                           strerror(errno));
        goto cleanup;
    }
    imported = true;

    uint32_t signal_handle = imported_handle.handle;
    struct vivid_drm_syncobj_array signal = {
        .handles = (uint64_t)(uintptr_t)&signal_handle,
        .count_handles = 1,
        .pad = 0,
    };
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal) != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_SIGNAL(%s handle=%u) failed context=%s: %s",
                           render_node,
                           imported_handle.handle,
                           context ? context : "(none)",
                           strerror(errno));
        goto cleanup;
    }
    supported = true;

cleanup:
    if (imported) {
        struct vivid_drm_syncobj_destroy destroy = {
            .handle = imported_handle.handle,
            .pad = 0,
        };
        if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) != 0)
            vivid_wayland_warn("release syncobj probe destroy imported handle=%u failed: %s",
                               imported_handle.handle,
                               strerror(errno));
    }
    if (exported.fd >= 0)
        close(exported.fd);
    if (created) {
        struct vivid_drm_syncobj_destroy destroy = {
            .handle = create.handle,
            .pad = 0,
        };
        if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) != 0)
            vivid_wayland_warn("release syncobj probe destroy created handle=%u failed: %s",
                               create.handle,
                               strerror(errno));
    }
    close(drm_fd);
    return supported;
}

bool
vivid_wayland_signal_release_syncobj(const char* render_node,
                                     int syncobj_fd,
                                     const char* context)
{
    if (!render_node || !render_node[0] || syncobj_fd < 0) {
        vivid_wayland_warn("cannot signal release syncobj context=%s render-node=%s fd=%d",
                           context ? context : "(none)",
                           render_node && render_node[0] ? render_node : "(missing)",
                           syncobj_fd);
        return false;
    }

    int drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        vivid_wayland_warn("open(%s) for release syncobj failed context=%s: %s",
                           render_node,
                           context ? context : "(none)",
                           strerror(errno));
        return false;
    }

    struct vivid_drm_syncobj_handle import = {
        .handle = 0,
        .flags = 0,
        .fd = syncobj_fd,
        .pad = 0,
    };
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE(%s) failed context=%s: %s",
                           render_node,
                           context ? context : "(none)",
                           strerror(errno));
        close(drm_fd);
        return false;
    }

    uint32_t handles[1] = { import.handle };
    struct vivid_drm_syncobj_array signal = {
        .handles = (uint64_t)(uintptr_t)handles,
        .count_handles = 1,
        .pad = 0,
    };
    int signal_rc = ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal);
    int signal_err = errno;

    struct vivid_drm_syncobj_destroy destroy = {
        .handle = import.handle,
        .pad = 0,
    };
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) != 0) {
        vivid_wayland_warn("drmSyncobjDestroy(handle=%u) failed context=%s: %s",
                           import.handle,
                           context ? context : "(none)",
                           strerror(errno));
    }
    close(drm_fd);

    if (signal_rc != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_SIGNAL(%s handle=%u) failed context=%s: %s",
                           render_node,
                           import.handle,
                           context ? context : "(none)",
                           strerror(signal_err));
        return false;
    }
    return true;
}

bool
vivid_wayland_attach_release_sync_file(const char* render_node,
                                       int syncobj_fd,
                                       int sync_file_fd,
                                       const char* context)
{
    if (!render_node || !render_node[0] || syncobj_fd < 0 || sync_file_fd < 0) {
        vivid_wayland_warn("cannot attach release fence context=%s render-node=%s syncobj=%d sync-file=%d",
                           context ? context : "(none)",
                           render_node && render_node[0] ? render_node : "(missing)",
                           syncobj_fd,
                           sync_file_fd);
        return false;
    }

    int drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        vivid_wayland_warn("open(%s) for release fence attach failed context=%s: %s",
                           render_node,
                           context ? context : "(none)",
                           strerror(errno));
        return false;
    }

    struct vivid_drm_syncobj_handle import = {
        .handle = 0,
        .flags = 0,
        .fd = syncobj_fd,
        .pad = 0,
    };
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE(%s) failed context=%s: %s",
                           render_node,
                           context ? context : "(none)",
                           strerror(errno));
        close(drm_fd);
        return false;
    }

    /*
     * The handle references the daemon's own syncobj, so replacing its fence
     * here is what the daemon's release wait observes: it completes when the
     * GPU finishes the work the sync_file describes, without any CPU wait on
     * this side. Equivalent to libdrm's drmSyncobjImportSyncFile().
     */
    struct vivid_drm_syncobj_handle attach = {
        .handle = import.handle,
        .flags = VIVID_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
        .fd = sync_file_fd,
        .pad = 0,
    };
    int attach_rc = ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &attach);
    int attach_err = errno;

    struct vivid_drm_syncobj_destroy destroy = {
        .handle = import.handle,
        .pad = 0,
    };
    if (ioctl(drm_fd, VIVID_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) != 0) {
        vivid_wayland_warn("drmSyncobjDestroy(handle=%u) failed context=%s: %s",
                           import.handle,
                           context ? context : "(none)",
                           strerror(errno));
    }
    close(drm_fd);

    if (attach_rc != 0) {
        vivid_wayland_warn("DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE(import-sync-file %s handle=%u) failed context=%s: %s",
                           render_node,
                           import.handle,
                           context ? context : "(none)",
                           strerror(attach_err));
        return false;
    }
    return true;
}

static char*
read_sysfs_text(const char* path, char* buffer, size_t buffer_size)
{
    FILE* file = fopen(path, "r");
    if (!file)
        return NULL;
    if (!fgets(buffer, (int)buffer_size, file)) {
        fclose(file);
        return NULL;
    }
    fclose(file);
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
        buffer[--len] = '\0';
    return buffer;
}

static const char*
vendor_from_id_text(const char* text)
{
    if (!text || !text[0])
        return NULL;
    unsigned long id = strtoul(text, NULL, 0);
    switch (id) {
    case 0x10de:
        return "nvidia";
    case 0x8086:
        return "intel";
    case 0x1002:
    case 0x1022:
        return "amd";
    default:
        return NULL;
    }
}

void
vivid_wayland_gpu_identity_from_render_node(VividWaylandGpuIdentity* identity)
{
    if (!identity || !identity->render_node[0])
        return;

    struct stat st;
    if (stat(identity->render_node, &st) != 0) {
        vivid_wayland_append_diag(identity,
                                  " stat(%s) failed: %s;",
                                  identity->render_node,
                                  strerror(errno));
        return;
    }

    char device_path[256];
    snprintf(device_path,
             sizeof(device_path),
             "/sys/dev/char/%u:%u/device",
             major(st.st_rdev),
             minor(st.st_rdev));

    char resolved[256];
    if (realpath(device_path, resolved)) {
        const char* slash = strrchr(resolved, '/');
        if (slash && slash[1])
            vivid_wayland_strlcpy(identity->pci_address, slash + 1, sizeof(identity->pci_address));
    }

    if (!identity->vendor[0]) {
        char vendor_path[300];
        char vendor_text[64];
        snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", device_path);
        if (read_sysfs_text(vendor_path, vendor_text, sizeof(vendor_text))) {
            const char* vendor = vendor_from_id_text(vendor_text);
            if (vendor)
                vivid_wayland_strlcpy(identity->vendor, vendor, sizeof(identity->vendor));
        }
    }
}

static void
uuid_bytes_to_hex(const uint8_t bytes[16], char* out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    if (out_size < 33)
        return;
    for (size_t i = 0; i < 16; i++) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    out[32] = '\0';
}

#ifdef WW_HAVE_VULKAN
static bool
load_vulkan_symbol(void* library, const char* name, void* destination, size_t destination_size)
{
    void* symbol = dlsym(library, name);
    if (!symbol || destination_size != sizeof(symbol))
        return false;

    /*
     * POSIX specifies that dlsym can resolve function symbols, while ISO C
     * still rejects a direct object-pointer-to-function-pointer cast under
     * -Wpedantic. Copy the representation only after verifying equal pointer
     * sizes so the dynamic Vulkan probe builds without relying on that cast.
     */
    memcpy(destination, &symbol, destination_size);
    return true;
}
#endif

void
vivid_wayland_gpu_identity_probe_vulkan_uuid(VividWaylandGpuIdentity* identity)
{
    if (!identity || !identity->render_node[0])
        return;

#ifndef WW_HAVE_VULKAN
    vivid_wayland_append_diag(identity, " Vulkan UUID probe disabled at build time;");
    return;
#else
    void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        vivid_wayland_append_diag(identity, " Vulkan UUID probe dlopen failed;");
        return;
    }

    PFN_vkGetInstanceProcAddr get_proc = NULL;
    if (!load_vulkan_symbol(
            library, "vkGetInstanceProcAddr", &get_proc, sizeof(get_proc))) {
        vivid_wayland_append_diag(identity, " Vulkan UUID probe missing vkGetInstanceProcAddr;");
        dlclose(library);
        return;
    }

    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)get_proc(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance)
        load_vulkan_symbol(library,
                           "vkCreateInstance",
                           &create_instance,
                           sizeof(create_instance));
    if (!create_instance) {
        vivid_wayland_append_diag(identity, " Vulkan UUID probe missing vkCreateInstance;");
        dlclose(library);
        return;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vivid layer-shell consumer GPU identity probe",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "vivid-layer-shell",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&create_info, NULL, &instance) != VK_SUCCESS) {
        vivid_wayland_append_diag(identity, " vkCreateInstance UUID probe failed;");
        dlclose(library);
        return;
    }

    PFN_vkDestroyInstance destroy_instance =
        (PFN_vkDestroyInstance)get_proc(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)get_proc(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 get_props2 =
        (PFN_vkGetPhysicalDeviceProperties2)get_proc(instance, "vkGetPhysicalDeviceProperties2");
    if (!get_props2)
        get_props2 = (PFN_vkGetPhysicalDeviceProperties2)get_proc(instance,
                                                                  "vkGetPhysicalDeviceProperties2KHR");
    if (!destroy_instance || !enumerate || !get_props2) {
        vivid_wayland_append_diag(identity, " Vulkan UUID probe missing instance functions;");
        if (destroy_instance)
            destroy_instance(instance, NULL);
        dlclose(library);
        return;
    }

    uint32_t count = 0;
    if (enumerate(instance, &count, NULL) != VK_SUCCESS || count == 0) {
        destroy_instance(instance, NULL);
        dlclose(library);
        return;
    }

    VkPhysicalDevice devices[16];
    if (count > 16)
        count = 16;
    if (enumerate(instance, &count, devices) != VK_SUCCESS) {
        destroy_instance(instance, NULL);
        dlclose(library);
        return;
    }

    struct stat wanted;
    if (stat(identity->render_node, &wanted) != 0) {
        destroy_instance(instance, NULL);
        dlclose(library);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceDrmPropertiesEXT drm_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
        };
        VkPhysicalDeviceIDProperties id_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            .pNext = &drm_props,
        };
        VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id_props,
        };
        get_props2(devices[i], &props);
        if (!drm_props.hasRender)
            continue;

        for (uint32_t minor_id = 128; minor_id <= 192; minor_id++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/dri/renderD%u", minor_id);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
                continue;
            if ((uint32_t)major(st.st_rdev) == (uint32_t)drm_props.renderMajor &&
                (uint32_t)minor(st.st_rdev) == (uint32_t)drm_props.renderMinor &&
                st.st_rdev == wanted.st_rdev) {
                uuid_bytes_to_hex(id_props.deviceUUID,
                                  identity->device_uuid,
                                  sizeof(identity->device_uuid));
                uuid_bytes_to_hex(id_props.driverUUID,
                                  identity->driver_uuid,
                                  sizeof(identity->driver_uuid));
                destroy_instance(instance, NULL);
                dlclose(library);
                return;
            }
        }
    }

    destroy_instance(instance, NULL);
    dlclose(library);
#endif
}
