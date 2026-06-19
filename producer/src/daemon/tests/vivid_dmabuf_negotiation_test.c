/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "../vivid_dmabuf_negotiation.h"

#include <assert.h>
#include <drm_fourcc.h>
#include <string.h>

static VividDmaBufPeerCaps
make_caps(guint8 uuid_byte)
{
    VividDmaBufPeerCaps caps;
    vivid_dmabuf_peer_caps_init(&caps);
    memset(caps.identity.device_uuid, uuid_byte, sizeof(caps.identity.device_uuid));
    caps.memory_hints =
        VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL | VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    caps.sync_caps = VIVID_DMABUF_REQUIRED_SYNC_CAPS;
    caps.color_caps = VIVID_DMABUF_DEFAULT_COLOR_CAPS;
    caps.relay_modes =
        VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT |
        VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
    return caps;
}

static void
set_drm_identity(VividDmaBufPeerCaps* caps, guint32 major, guint32 minor)
{
    caps->identity.drm_render_major = major;
    caps->identity.drm_render_minor = minor;
}

static void
test_same_device_prefers_non_linear(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE);
    assert(scheme.memory_source == VIVID_DMABUF_MEMORY_SOURCE_GPU_NATIVE);
    assert(scheme.modifier == tiled);
}

static void
test_cross_device_forces_compat_linear(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0xaa);
    VividDmaBufPeerCaps consumer = make_caps(0xbb);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR);
    assert(scheme.memory_source == VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR);
    assert(scheme.modifier == DRM_FORMAT_MOD_LINEAR);
    assert(scheme.plane_count == 1);
}

static void
test_cross_device_ignores_modifier_intersection(void)
{
    const guint64 producer_tiled = 0x0100000000000001ull;
    const guint64 consumer_tiled = 0x0200000000000002ull;
    VividDmaBufPeerCaps producer = make_caps(0xaa);
    VividDmaBufPeerCaps consumer = make_caps(0xbb);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, producer_tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, consumer_tiled, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR);
    assert(scheme.memory_source == VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR);
    assert(scheme.modifier == DRM_FORMAT_MOD_LINEAR);
    assert(scheme.plane_count == 1);
    assert(!scheme.same_device);
}

static void
test_blacklist_retries_to_linear(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
    vivid_dmabuf_peer_caps_blacklist_modifier(&producer, DRM_FORMAT_ABGR8888, tiled);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR);
    assert(scheme.modifier == DRM_FORMAT_MOD_LINEAR);
}

static void
test_plane_count_mismatch_is_no_intersection(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 2);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);

    VividDmaBufNegotiateError error = VIVID_DMABUF_NEGOTIATE_ERROR_NONE;
    VividDmaBufNegotiatedScheme scheme = {0};
    assert(!vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, &error));
    assert(error == VIVID_DMABUF_NEGOTIATE_ERROR_NO_FORMAT_INTERSECTION);
}

static void
test_unknown_uuid_same_drm_identity_prefers_non_linear(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x00);
    VividDmaBufPeerCaps consumer = make_caps(0x00);
    set_drm_identity(&producer, 226, 128);
    set_drm_identity(&consumer, 226, 128);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE);
    assert(scheme.modifier == tiled);
    assert(scheme.same_device);
}

static void
test_uuid_mismatch_overrides_same_drm_identity(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x99);
    set_drm_identity(&producer, 226, 128);
    set_drm_identity(&consumer, 226, 128);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR);
    assert(scheme.modifier == DRM_FORMAT_MOD_LINEAR);
    assert(!scheme.same_device);
}

static void
test_unknown_uuid_without_drm_identity_is_cross_device(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x00);
    VividDmaBufPeerCaps consumer = make_caps(0x00);
    g_strlcpy(producer.identity.render_node,
              "/dev/dri/renderD128",
              sizeof(producer.identity.render_node));
    g_strlcpy(consumer.identity.render_node,
              "/dev/dri/renderD128",
              sizeof(consumer.identity.render_node));
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR);
    assert(scheme.modifier == DRM_FORMAT_MOD_LINEAR);
    assert(!scheme.same_device);
}

static void
test_cross_device_requires_shadow_copy(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0xaa);
    VividDmaBufPeerCaps consumer = make_caps(0xbb);
    consumer.relay_modes = VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT;
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);

    VividDmaBufNegotiateError error = VIVID_DMABUF_NEGOTIATE_ERROR_NONE;
    VividDmaBufNegotiatedScheme scheme = {0};
    assert(!vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, &error));
    assert(error == VIVID_DMABUF_NEGOTIATE_ERROR_NO_RELAY_MODE);
}

static void
test_same_device_shadow_copy_prefers_non_linear_without_direct(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    consumer.relay_modes = VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_OPTIMIZED_SAME_DEVICE);
    assert(scheme.memory_source == VIVID_DMABUF_MEMORY_SOURCE_GPU_NATIVE);
    assert(scheme.relay_mode == VIVID_DMABUF_RELAY_MODE_SHADOW_COPY);
    assert(scheme.modifier == tiled);
    assert(scheme.same_device);
}

static void
test_same_device_shadow_copy_allows_linear_without_direct(void)
{
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    consumer.relay_modes = VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
    vivid_dmabuf_peer_caps_add_modifier(&producer,
                                        DRM_FORMAT_ABGR8888,
                                        DRM_FORMAT_MOD_LINEAR,
                                        1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer,
                                        DRM_FORMAT_ABGR8888,
                                        DRM_FORMAT_MOD_LINEAR,
                                        1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.path == VIVID_DMABUF_NEGOTIATED_COMPAT_LINEAR);
    assert(scheme.memory_source == VIVID_DMABUF_MEMORY_SOURCE_GPU_LINEAR);
    assert(scheme.relay_mode == VIVID_DMABUF_RELAY_MODE_SHADOW_COPY);
    assert(scheme.modifier == DRM_FORMAT_MOD_LINEAR);
    assert(scheme.same_device);
}

static void
test_kde_egl_caps_remain_host_visible(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    consumer.memory_hints = VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    consumer.relay_modes =
        VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT |
        VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.memory_hint == VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE);
    assert(scheme.relay_mode == VIVID_DMABUF_RELAY_MODE_DIRECT_IMPORT);
}

static void
test_kde_vulkan_shadow_copy_uses_device_local_only_when_both_advertise(void)
{
    const guint64 tiled = 0x0100000000000001ull;
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    consumer.memory_hints = VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE;
    consumer.relay_modes = VIVID_DMABUF_RELAY_MODE_SHADOW_COPY;
    vivid_dmabuf_peer_caps_add_modifier(&producer, DRM_FORMAT_ABGR8888, tiled, 1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer, DRM_FORMAT_ABGR8888, tiled, 1);

    VividDmaBufNegotiatedScheme scheme = {0};
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.memory_hint == VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE);
    assert(scheme.relay_mode == VIVID_DMABUF_RELAY_MODE_SHADOW_COPY);

    consumer.memory_hints =
        VIVID_DMABUF_MEMORY_HINT_HOST_VISIBLE |
        VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL;
    memset(&scheme, 0, sizeof(scheme));
    assert(vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, NULL));
    assert(scheme.memory_hint == VIVID_DMABUF_MEMORY_HINT_DEVICE_LOCAL);
    assert(scheme.relay_mode == VIVID_DMABUF_RELAY_MODE_SHADOW_COPY);
}

static void
test_missing_sync_caps_rejects(void)
{
    VividDmaBufPeerCaps producer = make_caps(0x42);
    VividDmaBufPeerCaps consumer = make_caps(0x42);
    consumer.sync_caps = 0;
    vivid_dmabuf_peer_caps_add_modifier(&producer,
                                        DRM_FORMAT_ABGR8888,
                                        DRM_FORMAT_MOD_LINEAR,
                                        1);
    vivid_dmabuf_peer_caps_add_modifier(&consumer,
                                        DRM_FORMAT_ABGR8888,
                                        DRM_FORMAT_MOD_LINEAR,
                                        1);

    VividDmaBufNegotiateError error = VIVID_DMABUF_NEGOTIATE_ERROR_NONE;
    VividDmaBufNegotiatedScheme scheme = {0};
    assert(!vivid_dmabuf_negotiate_pick(&producer, &consumer, &scheme, &error));
    assert(error == VIVID_DMABUF_NEGOTIATE_ERROR_NO_SYNC_INTERSECTION);
}

static void
test_extent_zero_is_unbounded(void)
{
    VividDmaBufPeerCaps caps = make_caps(0x42);

    caps.extent_max_w = 0;
    caps.extent_max_h = 0;

    assert(vivid_dmabuf_peer_caps_accepts_extent(&caps, 7680, 4320));
}

static void
test_extent_limit_rejects_oversized_buffer(void)
{
    VividDmaBufPeerCaps caps = make_caps(0x42);

    caps.extent_max_w = 1920;
    caps.extent_max_h = 1080;

    assert(vivid_dmabuf_peer_caps_accepts_extent(&caps, 1920, 1080));
    assert(!vivid_dmabuf_peer_caps_accepts_extent(&caps, 1921, 1080));
    assert(!vivid_dmabuf_peer_caps_accepts_extent(&caps, 1920, 1081));
}

int
main(void)
{
    test_same_device_prefers_non_linear();
    test_cross_device_forces_compat_linear();
    test_cross_device_ignores_modifier_intersection();
    test_blacklist_retries_to_linear();
    test_plane_count_mismatch_is_no_intersection();
    test_unknown_uuid_same_drm_identity_prefers_non_linear();
    test_uuid_mismatch_overrides_same_drm_identity();
    test_unknown_uuid_without_drm_identity_is_cross_device();
    test_cross_device_requires_shadow_copy();
    test_same_device_shadow_copy_prefers_non_linear_without_direct();
    test_same_device_shadow_copy_allows_linear_without_direct();
    test_kde_egl_caps_remain_host_visible();
    test_kde_vulkan_shadow_copy_uses_device_local_only_when_both_advertise();
    test_missing_sync_caps_rejects();
    test_extent_zero_is_unbounded();
    test_extent_limit_rejects_oversized_buffer();
    return 0;
}
