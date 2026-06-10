/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_DMABUF_NEGOTIATION_H
#define VIVID_DMABUF_NEGOTIATION_H

#include <glib.h>

#define VIVID_DMABUF_NEGOTIATION_MAX_FORMAT_CAPS 128u
#define VIVID_DMABUF_NEGOTIATION_MAX_BLACKLIST 64u
#define VIVID_DMABUF_DEVICE_UUID_BYTES 16u

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VividDmaBufModifierCap;

typedef struct
{
    guint32 n_modifiers;
    VividDmaBufModifierCap modifiers[VIVID_DMABUF_NEGOTIATION_MAX_FORMAT_CAPS];
} VividDmaBufFormatCaps;

typedef struct
{
    guint8  device_uuid[VIVID_DMABUF_DEVICE_UUID_BYTES];
    guint8  driver_uuid[VIVID_DMABUF_DEVICE_UUID_BYTES];
    guint32 drm_render_major;
    guint32 drm_render_minor;
    gchar   render_node[256];
} VividDmaBufDeviceIdentity;

typedef enum
{
    VIVID_DMABUF_SYNC_CAP_IMPLICIT = 1u << 0,
    VIVID_DMABUF_SYNC_CAP_SYNCOBJ_BINARY = 1u << 1,
    VIVID_DMABUF_SYNC_CAP_SYNCOBJ_TIMELINE = 1u << 2,
} VividDmaBufSyncCap;

typedef enum
{
    VIVID_DMABUF_COLOR_CAP_SRGB = 1u << 0,
    VIVID_DMABUF_COLOR_CAP_LINEAR = 1u << 1,
    VIVID_DMABUF_COLOR_CAP_BT601 = 1u << 2,
    VIVID_DMABUF_COLOR_CAP_BT709 = 1u << 3,
    VIVID_DMABUF_COLOR_CAP_BT2020 = 1u << 4,
    VIVID_DMABUF_COLOR_CAP_RANGE_FULL = 1u << 5,
    VIVID_DMABUF_COLOR_CAP_RANGE_LIMITED = 1u << 6,
    VIVID_DMABUF_COLOR_CAP_ALPHA_PREMULTIPLIED = 1u << 7,
    VIVID_DMABUF_COLOR_CAP_ALPHA_STRAIGHT = 1u << 8,
} VividDmaBufColorCap;

#define VIVID_DMABUF_REQUIRED_SYNC_CAPS \
    (VIVID_DMABUF_SYNC_CAP_IMPLICIT | VIVID_DMABUF_SYNC_CAP_SYNCOBJ_BINARY)

#define VIVID_DMABUF_DEFAULT_COLOR_CAPS \
    (VIVID_DMABUF_COLOR_CAP_SRGB | \
     VIVID_DMABUF_COLOR_CAP_RANGE_LIMITED | \
     VIVID_DMABUF_COLOR_CAP_ALPHA_PREMULTIPLIED)

typedef enum
{
    VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE = 0,
    VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR = 2,
    VIVID_DMABUF_NEGOTIATED_COMPAT_CPU_READBACK_RESERVED = 3,
} VividDmaBufNegotiatedPath;

typedef enum
{
    VIVID_DMABUF_MEMORY_SOURCE_GPU_NATIVE = 0,
    VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR = 1,
    VIVID_DMABUF_MEMORY_SOURCE_DMABUF_HEAP_RESERVED = 2,
} VividDmaBufMemorySource;

typedef enum
{
    VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL = 1u << 0,
    VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE = 1u << 1,
    VIVID_DMABUF_MEMORY_HINT_SCANOUT_CAPABLE = 1u << 2,
    VIVID_DMABUF_MEMORY_HINT_PROTECTED = 1u << 3,
    VIVID_DMABUF_MEMORY_HINT_LINEAR_ONLY_RESERVED = 1u << 4,
} VividDmaBufMemoryHint;

typedef enum
{
    VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT = 1u << 0,
    VIVID_DMABUF_RELAY_MODE_SHADOW_COPY = 1u << 1,
} VividDmaBufRelayMode;

typedef struct
{
    VividDmaBufFormatCaps formats;
    VividDmaBufDeviceIdentity identity;
    guint32 memory_hints;
    guint32 sync_caps;
    guint32 color_caps;
    guint32 relay_modes;
    guint32 extent_max_w;
    guint32 extent_max_h;
    guint32 n_blacklist;
    VividDmaBufModifierCap blacklist[VIVID_DMABUF_NEGOTIATION_MAX_BLACKLIST];
} VividDmaBufPeerCaps;

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
    VividDmaBufNegotiatedPath path;
    VividDmaBufMemorySource memory_source;
    guint32 memory_hint;
    guint32 relay_mode;
    gboolean same_device;
} VividDmaBufNegotiatedScheme;

typedef enum
{
    VIVID_DMABUF_NEGOTIATE_ERROR_NONE = 0,
    VIVID_DMABUF_NEGOTIATE_ERROR_NO_FORMAT_INTERSECTION,
    VIVID_DMABUF_NEGOTIATE_ERROR_NO_SYNC_INTERSECTION,
    VIVID_DMABUF_NEGOTIATE_ERROR_NO_RELAY_MODE,
} VividDmaBufNegotiateError;

void vivid_dmabuf_peer_caps_init(VividDmaBufPeerCaps* caps);
gboolean vivid_dmabuf_peer_caps_add_modifier(VividDmaBufPeerCaps* caps,
                                             guint32               fourcc,
                                             guint64               modifier,
                                             guint32               plane_count);
gboolean vivid_dmabuf_peer_caps_blacklist_modifier(VividDmaBufPeerCaps* caps,
                                                   guint32               fourcc,
                                                   guint64               modifier);

const gchar* vivid_dmabuf_negotiated_path_name(VividDmaBufNegotiatedPath path);
const gchar* vivid_dmabuf_memory_source_name(VividDmaBufMemorySource source);
const gchar* vivid_dmabuf_relay_mode_name(guint32 relay_mode);

gboolean vivid_dmabuf_device_identity_same_device(
    const VividDmaBufDeviceIdentity* a,
    const VividDmaBufDeviceIdentity* b);
gboolean vivid_dmabuf_peer_caps_accepts_extent(const VividDmaBufPeerCaps* caps,
                                               guint32                    width,
                                               guint32                    height);

gboolean vivid_dmabuf_negotiate_pick(const VividDmaBufPeerCaps* producer,
                                     const VividDmaBufPeerCaps* consumer,
                                     VividDmaBufNegotiatedScheme* out_scheme,
                                     VividDmaBufNegotiateError*   out_error);

#endif
