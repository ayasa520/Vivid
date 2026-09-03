#ifndef VIVID_WAYLAND_DRM_H
#define VIVID_WAYLAND_DRM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIVID_WAYLAND_DRM_FORMAT_MOD_INVALID ((1ull << 56) - 1ull)
#define VIVID_WAYLAND_DRM_FORMAT_MOD_LINEAR 0ull

typedef struct {
    char render_node[128];
    char vendor[32];
    char pci_address[32];
    char device_uuid[64];
    char driver_uuid[64];
    char diagnostics[2048];
} VividWaylandGpuIdentity;

bool vivid_wayland_fourcc_supported(uint32_t fourcc);
bool vivid_wayland_signal_release_syncobj(const char* render_node,
                                          int syncobj_fd,
                                          const char* context);
/* Makes the release syncobj complete when the fence in sync_file_fd signals.
 * Neither fd is consumed or closed. */
bool vivid_wayland_attach_release_sync_file(const char* render_node,
                                           int syncobj_fd,
                                           int sync_file_fd,
                                           const char* context);
bool vivid_wayland_release_syncobj_supported(const char* render_node,
                                             const char* context);
void vivid_wayland_gpu_identity_from_render_node(VividWaylandGpuIdentity* identity);
void vivid_wayland_gpu_identity_probe_vulkan_uuid(VividWaylandGpuIdentity* identity);
void vivid_wayland_append_diag(VividWaylandGpuIdentity* identity, const char* fmt, ...);

#endif
