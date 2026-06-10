/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Single source of truth for GPU device selection across the producer stack.
 *
 * The user-visible device identity is a DRM render node path such as
 * "/dev/dri/renderD128" (or "auto"). The internal identity used to pick the
 * exact Vulkan physical device is the 16-byte Vulkan deviceUUID; driverUUID is
 * carried alongside it for protocol diagnostics and future stricter matching
 * without making consumers re-query Vulkan. The decoder route (NVDEC vs VA) is
 * derived from the Vulkan vendorID instead of being a separate user-facing knob.
 * The core renderer, the scene backend and the video backend all resolve
 * devices through this module so they can never pick different cards for the
 * same configuration value.
 *
 * Plain C on purpose: the producer core is built with a C compiler while the
 * scene/video bridges are C++ CMake targets, and both compile this same file.
 */
/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */
#ifndef VIVID_GPU_DEVICES_H
#define VIVID_GPU_DEVICES_H

#include <glib.h>

G_BEGIN_DECLS

#define VIVID_GPU_DEVICE_UUID_BYTES 16u
#define VIVID_GPU_DEVICES_MAX 16u
#define VIVID_GPU_DEVICE_RENDER_NODE_MAX 64u
#define VIVID_GPU_DEVICE_NAME_MAX 256u
#define VIVID_GPU_DEVICE_PCI_ADDRESS_MAX 32u
#define VIVID_GPU_DEVICE_DMABUF_CAPS_MAX 64u

#define VIVID_GPU_VENDOR_ID_AMD 0x1002u
#define VIVID_GPU_VENDOR_ID_NVIDIA 0x10deu
#define VIVID_GPU_VENDOR_ID_INTEL 0x8086u

typedef enum
{
    VIVID_GPU_DECODER_ROUTE_NONE = 0,
    VIVID_GPU_DECODER_ROUTE_NVIDIA,
    VIVID_GPU_DECODER_ROUTE_VA,
} VividGpuDecoderRoute;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividGpuDmaBufFormatCap;

typedef struct
{
    /* "/dev/dri/renderD128", or "" when VK_EXT_physical_device_drm is absent. */
    gchar    render_node[VIVID_GPU_DEVICE_RENDER_NODE_MAX];
    gchar    name[VIVID_GPU_DEVICE_NAME_MAX];
    gchar    pci_address[VIVID_GPU_DEVICE_PCI_ADDRESS_MAX];
    guint32  vendor_id;
    guint32  drm_render_major;
    guint32  drm_render_minor;
    guint8   uuid[VIVID_GPU_DEVICE_UUID_BYTES];
    guint8   driver_uuid[VIVID_GPU_DEVICE_UUID_BYTES];
    gboolean is_discrete;
    guint32  scene_dmabuf_n_caps;
    VividGpuDmaBufFormatCap scene_dmabuf_caps[VIVID_GPU_DEVICE_DMABUF_CAPS_MAX];
} VividGpuDevice;

typedef struct
{
    guint           n_devices;
    VividGpuDevice devices[VIVID_GPU_DEVICES_MAX];
} VividGpuDeviceList;

/*
 * Enumerate Vulkan physical devices. CPU/software implementations (llvmpipe)
 * are skipped. Returns FALSE when Vulkan is unavailable or no device remains.
 */
gboolean vivid_gpu_devices_enumerate(VividGpuDeviceList* out_list);

/*
 * Resolve a configured render-device value against an enumerated list.
 * NULL/""/"auto" picks the first discrete device, else the first device.
 * Any other value must match a render node. Unknown explicit values fail
 * instead of falling back to another GPU, preserving the same-card invariant.
 */
gboolean vivid_gpu_devices_resolve(const VividGpuDeviceList* list,
                                    const gchar*               render_device_value,
                                    VividGpuDevice*           out_device);

/* Convenience wrapper: enumerate, then resolve. */
gboolean vivid_gpu_device_resolve(const gchar*     render_device_value,
                                   VividGpuDevice* out_device);

/* The only vendor -> decoder mapping in the tree. Never user-visible. */
VividGpuDecoderRoute vivid_gpu_decoder_route_for_vendor(guint32 vendor_id);

const gchar* vivid_gpu_decoder_route_name(VividGpuDecoderRoute route);

/* Stable lowercase vendor tag for protocol JSON: nvidia/intel/amd/unknown. */
const gchar* vivid_gpu_vendor_name(guint32 vendor_id);

G_END_DECLS

#endif
