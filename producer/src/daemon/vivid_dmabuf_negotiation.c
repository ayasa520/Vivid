/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_dmabuf_negotiation.h"

#include <drm_fourcc.h>
#include <string.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

static gboolean
uuid_is_known(const guint8 uuid[VIVID_DMABUF_DEVICE_UUID_BYTES])
{
    for (guint i = 0; i < VIVID_DMABUF_DEVICE_UUID_BYTES; i++) {
        if (uuid[i] != 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
drm_render_identity_is_known(const VividDmaBufDeviceIdentity* identity)
{
    return identity &&
        (identity->drm_render_major != 0 || identity->drm_render_minor != 0);
}

static guint32
pick_sync_mode(guint32 producer_sync, guint32 consumer_sync)
{
    const guint32 common = producer_sync & consumer_sync;
    if ((common & VIVID_DMABUF_SYNC_CAP_SYNCOBJ_TIMELINE) != 0)
        return VIVID_DMABUF_SYNC_CAP_SYNCOBJ_TIMELINE;
    if ((common & VIVID_DMABUF_SYNC_CAP_SYNCOBJ_BINARY) != 0)
        return VIVID_DMABUF_SYNC_CAP_SYNCOBJ_BINARY;
    if ((common & VIVID_DMABUF_SYNC_CAP_IMPLICIT) != 0)
        return VIVID_DMABUF_SYNC_CAP_IMPLICIT;
    return 0;
}

static gboolean
modifier_is_linear(guint64 modifier)
{
    return modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;
}

void
vivid_dmabuf_peer_caps_init(VividDmaBufPeerCaps* caps)
{
    if (caps)
        memset(caps, 0, sizeof(*caps));
}

gboolean
vivid_dmabuf_peer_caps_add_modifier(VividDmaBufPeerCaps* caps,
                                    guint32               fourcc,
                                    guint64               modifier,
                                    guint32               plane_count)
{
    if (!caps || plane_count == 0)
        return FALSE;

    if (caps->formats.n_modifiers >= VIVID_DMABUF_NEGOTIATION_MAX_FORMAT_CAPS)
        return FALSE;

    for (guint32 i = 0; i < caps->formats.n_modifiers; i++) {
        const VividDmaBufModifierCap* existing = &caps->formats.modifiers[i];
        if (existing->fourcc == fourcc &&
            existing->modifier == modifier &&
            existing->plane_count == plane_count) {
            return TRUE;
        }
    }

    caps->formats.modifiers[caps->formats.n_modifiers++] =
        (VividDmaBufModifierCap) {
            .fourcc = fourcc,
            .modifier = modifier,
            .plane_count = plane_count,
        };
    return TRUE;
}

gboolean
vivid_dmabuf_peer_caps_blacklist_modifier(VividDmaBufPeerCaps* caps,
                                          guint32               fourcc,
                                          guint64               modifier)
{
    if (!caps)
        return FALSE;

    for (guint32 i = 0; i < caps->n_blacklist; i++) {
        const VividDmaBufModifierCap* entry = &caps->blacklist[i];
        if (entry->fourcc == fourcc && entry->modifier == modifier)
            return FALSE;
    }

    if (caps->n_blacklist >= VIVID_DMABUF_NEGOTIATION_MAX_BLACKLIST)
        return FALSE;

    caps->blacklist[caps->n_blacklist++] = (VividDmaBufModifierCap) {
        .fourcc = fourcc,
        .modifier = modifier,
        .plane_count = 0,
    };
    return TRUE;
}

const gchar*
vivid_dmabuf_negotiated_path_name(VividDmaBufNegotiatedPath path)
{
    switch (path) {
    case VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE:
        return "optimized-same-device";
    case VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR:
        return "compat-linear";
    case VIVID_DMABUF_NEGOTIATED_COMPAT_CPU_READBACK_RESERVED:
        return "compat-cpu-readback-reserved";
    default:
        return "unknown";
    }
}

const gchar*
vivid_dmabuf_memory_source_name(VividDmaBufMemorySource source)
{
    switch (source) {
    case VIVID_DMABUF_MEMORY_SOURCE_GPU_NATIVE:
        return "gpu-native";
    case VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR:
        return "gpu-linear";
    case VIVID_DMABUF_MEMORY_SOURCE_DMABUF_HEAP_RESERVED:
        return "dmabuf-heap-reserved";
    default:
        return "unknown";
    }
}

const gchar*
vivid_dmabuf_relay_mode_name(guint32 relay_mode)
{
    switch (relay_mode) {
    case VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT:
        return "direct";
    case VIVID_DMABUF_RELAY_MODE_SHADOW_COPY:
        return "shadow-copy";
    default:
        return "unknown";
    }
}

gboolean
vivid_dmabuf_device_identity_same_device(const VividDmaBufDeviceIdentity* a,
                                         const VividDmaBufDeviceIdentity* b)
{
    if (!a || !b)
        return FALSE;

    const gboolean a_uuid_known = uuid_is_known(a->device_uuid);
    const gboolean b_uuid_known = uuid_is_known(b->device_uuid);
    if (a_uuid_known && b_uuid_known) {
        /*
         * Vulkan deviceUUID is the strongest identity source: when both sides
         * provide it, a mismatch must stay cross-device even if the render-node
         * major/minor happens to match through aliasing or namespace tricks.
         */
        return memcmp(a->device_uuid, b->device_uuid, VIVID_DMABUF_DEVICE_UUID_BYTES) == 0;
    }

    /*
     * Some consumers cannot expose Vulkan UUIDs but can still report the DRM
     * render node they import with. Match the kernel device identity, not the
     * path string, so /dev aliases and bind mounts do not change protocol
     * semantics. If either side lacks both UUID and DRM numbers, remain
     * conservative and negotiate the cross-GPU linear path.
     */
    if (drm_render_identity_is_known(a) && drm_render_identity_is_known(b)) {
        return a->drm_render_major == b->drm_render_major &&
            a->drm_render_minor == b->drm_render_minor;
    }

    return FALSE;
}

gboolean
vivid_dmabuf_peer_caps_accepts_extent(const VividDmaBufPeerCaps* caps,
                                      guint32                    width,
                                      guint32                    height)
{
    if (!caps)
        return TRUE;

    /*
     * Waywallen treats zero maxima as "unknown/unbounded". Keep that semantic
     * isolated from bind code so every producer path applies the same hard
     * consumer import limit before publishing a DMA-BUF pool.
     */
    if (caps->extent_max_w != 0 && width > caps->extent_max_w)
        return FALSE;
    if (caps->extent_max_h != 0 && height > caps->extent_max_h)
        return FALSE;
    return TRUE;
}

static gboolean
peer_caps_blacklisted(const VividDmaBufPeerCaps* caps,
                      guint32                    fourcc,
                      guint64                    modifier)
{
    if (!caps)
        return FALSE;

    for (guint32 i = 0; i < caps->n_blacklist; i++) {
        const VividDmaBufModifierCap* entry = &caps->blacklist[i];
        if (entry->fourcc == fourcc && entry->modifier == modifier)
            return TRUE;
    }
    return FALSE;
}

static gboolean
peer_caps_has_fourcc(const VividDmaBufPeerCaps* caps,
                     guint32                    fourcc)
{
    if (!caps)
        return FALSE;

    for (guint32 i = 0; i < caps->formats.n_modifiers; i++) {
        if (caps->formats.modifiers[i].fourcc == fourcc)
            return TRUE;
    }
    return FALSE;
}

static guint32
pick_memory_hint_same_device(guint32 producer_hints, guint32 consumer_hints)
{
    const guint32 common = producer_hints & consumer_hints;
    if ((common & VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL) != 0)
        return VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL;
    if ((common & VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE) != 0)
        return VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    if (common != 0)
        return common;
    return VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
}

static gboolean
pick_cross_device_fourcc(const VividDmaBufPeerCaps* producer,
                         const VividDmaBufPeerCaps* consumer,
                         guint32*                   out_fourcc)
{
    for (guint32 i = 0; i < producer->formats.n_modifiers; i++) {
        const guint32 fourcc = producer->formats.modifiers[i].fourcc;
        if (!peer_caps_has_fourcc(consumer, fourcc))
            continue;
        if (peer_caps_blacklisted(producer, fourcc, DRM_FORMAT_MOD_LINEAR) ||
            peer_caps_blacklisted(consumer, fourcc, DRM_FORMAT_MOD_LINEAR)) {
            continue;
        }

        *out_fourcc = fourcc;
        return TRUE;
    }
    return FALSE;
}

static gboolean
pick_same_device_format(const VividDmaBufPeerCaps* producer,
                        const VividDmaBufPeerCaps* consumer,
                        VividDmaBufModifierCap*    out_cap)
{
    gboolean have_linear = FALSE;
    VividDmaBufModifierCap linear = {0};

    /*
     * This mirrors waywallen's picker boundary: exact modifier and plane-count
     * intersection for same-device paths, with non-LINEAR preferred. The
     * producer order is preserved so renderers can advertise their currently
     * pinned tuple first and avoid a bind_failed bounce through less desirable
     * modifiers.
     */
    for (guint32 producer_i = 0;
         producer_i < producer->formats.n_modifiers;
         producer_i++) {
        const VividDmaBufModifierCap* producer_cap =
            &producer->formats.modifiers[producer_i];
        if (peer_caps_blacklisted(producer,
                                  producer_cap->fourcc,
                                  producer_cap->modifier)) {
            continue;
        }

        for (guint32 consumer_i = 0;
             consumer_i < consumer->formats.n_modifiers;
             consumer_i++) {
            const VividDmaBufModifierCap* consumer_cap =
                &consumer->formats.modifiers[consumer_i];
            if (consumer_cap->fourcc != producer_cap->fourcc ||
                consumer_cap->modifier != producer_cap->modifier ||
                consumer_cap->plane_count != producer_cap->plane_count) {
                continue;
            }
            if (peer_caps_blacklisted(consumer,
                                      consumer_cap->fourcc,
                                      consumer_cap->modifier)) {
                continue;
            }

            if (!modifier_is_linear(producer_cap->modifier)) {
                *out_cap = *producer_cap;
                return TRUE;
            }
            if (!have_linear) {
                linear = *producer_cap;
                have_linear = TRUE;
            }
        }
    }

    if (have_linear)
        *out_cap = linear;
    return have_linear;
}

gboolean
vivid_dmabuf_negotiate_pick(const VividDmaBufPeerCaps* producer,
                            const VividDmaBufPeerCaps* consumer,
                            VividDmaBufNegotiatedScheme* out_scheme,
                            VividDmaBufNegotiateError*   out_error)
{
    if (out_error)
        *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NONE;
    if (out_scheme)
        memset(out_scheme, 0, sizeof(*out_scheme));

    if (!producer || !consumer || !out_scheme ||
        producer->formats.n_modifiers == 0 ||
        consumer->formats.n_modifiers == 0) {
        if (out_error)
            *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NO_FORMAT_INTERSECTION;
        return FALSE;
    }

    const gboolean same_device =
        vivid_dmabuf_device_identity_same_device(&producer->identity,
                                                 &consumer->identity);
    const guint32 sync_mode = pick_sync_mode(producer->sync_caps,
                                             consumer->sync_caps);
    if (sync_mode == 0) {
        if (out_error)
            *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NO_SYNC_INTERSECTION;
        return FALSE;
    }

    if (same_device) {
        if ((consumer->relay_modes & VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT) == 0) {
            if (out_error)
                *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NO_RELAY_MODE;
            return FALSE;
        }

        VividDmaBufModifierCap cap = {0};
        if (!pick_same_device_format(producer, consumer, &cap)) {
            if (out_error)
                *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NO_FORMAT_INTERSECTION;
            return FALSE;
        }

        out_scheme->fourcc = cap.fourcc;
        out_scheme->modifier = modifier_is_linear(cap.modifier)
            ? DRM_FORMAT_MOD_LINEAR
            : cap.modifier;
        out_scheme->plane_count = cap.plane_count;
        out_scheme->same_device = TRUE;
        out_scheme->relay_mode = VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT;
        out_scheme->memory_hint =
            pick_memory_hint_same_device(producer->memory_hints,
                                         consumer->memory_hints);
        if (modifier_is_linear(cap.modifier)) {
            out_scheme->path = VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR;
            out_scheme->memory_source = VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR;
        } else {
            out_scheme->path = VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE;
            out_scheme->memory_source = VIVID_DMABUF_MEMORY_SOURCE_GPU_NATIVE;
        }
        return TRUE;
    }

    if ((consumer->relay_modes & VIVID_DMABUF_RELAY_MODE_SHADOW_COPY) == 0) {
        if (out_error)
            *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NO_RELAY_MODE;
        return FALSE;
    }

    guint32 fourcc = 0;
    if (!pick_cross_device_fourcc(producer, consumer, &fourcc)) {
        if (out_error)
            *out_error = VIVID_DMABUF_NEGOTIATE_ERROR_NO_FORMAT_INTERSECTION;
        return FALSE;
    }

    out_scheme->fourcc = fourcc;
    out_scheme->modifier = DRM_FORMAT_MOD_LINEAR;
    out_scheme->plane_count = 1;
    out_scheme->path = VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR;
    out_scheme->memory_source = VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR;
    out_scheme->memory_hint = VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    out_scheme->relay_mode = VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
    out_scheme->same_device = FALSE;
    return TRUE;
}
