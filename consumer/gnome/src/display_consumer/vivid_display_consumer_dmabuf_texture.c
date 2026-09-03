/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_display_consumer_dmabuf_texture.h"
#include "vivid_display_consumer_vulkan_backend.h"

#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

typedef struct
{
    guint32 fourcc;
    guint64 modifier;
    guint32 plane_count;
} VulkanRelayModifierCap;

typedef struct
{
    GArray* caps; /* VulkanRelayModifierCap */
} VulkanRelayCapsProbe;

typedef enum
{
    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
} VividDisplayConsumerDmaBufTextureError;

static GQuark
vivid_display_consumer_dmabuf_texture_error_quark(void)
{
    return g_quark_from_static_string("vivid_display_consumer_dmabuf_texture-error");
}

#define VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR \
    (vivid_display_consumer_dmabuf_texture_error_quark())

static char*
uuid_bytes_to_hex(const guint8 uuid[16])
{
    GString* text = g_string_sized_new(32);
    for (guint i = 0; i < 16; i++)
        g_string_append_printf(text, "%02x", uuid[i]);
    return g_string_free(text, FALSE);
}

static void
find_render_node_for_drm_ids(gint64 render_major,
                             gint64 render_minor,
                             gchar* out_node,
                             gsize  out_size)
{
    out_node[0] = '\0';

    for (guint minor_id = 128; minor_id <= 192; minor_id++) {
        gchar path[64];
        g_snprintf(path, sizeof(path), "/dev/dri/renderD%u", minor_id);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
            continue;

        if ((gint64)major(st.st_rdev) == render_major &&
            (gint64)minor(st.st_rdev) == render_minor) {
            g_strlcpy(out_node, path, out_size);
            return;
        }
    }
}

static void
vulkan_relay_caps_emit(uint32_t fourcc,
                       uint64_t modifier,
                       uint32_t plane_count,
                       void*    user_data)
{
    VulkanRelayCapsProbe* probe = user_data;
    if (!probe || !probe->caps)
        return;

    const VulkanRelayModifierCap cap = {
        .fourcc = fourcc,
        .modifier = modifier,
        .plane_count = plane_count > 0 ? plane_count : 1,
    };
    g_array_append_val(probe->caps, cap);
}

static void
json_builder_add_uint64_string(JsonBuilder* builder,
                               const char*  member,
                               guint64      value)
{
    gchar text[32];
    g_snprintf(text, sizeof(text), "%" G_GUINT64_FORMAT, value);
    json_builder_set_member_name(builder, member);
    json_builder_add_string_value(builder, text);
}

static char*
build_vulkan_relay_caps_error_json(const char* stage,
                                   gint        rc)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_set_member_name(builder, "backend");
    json_builder_add_string_value(builder, "vulkan-dmabuf-relay");
    json_builder_set_member_name(builder, "probe");
    json_builder_add_string_value(builder, "vulkan-relay-unavailable");
    json_builder_set_member_name(builder, "stage");
    json_builder_add_string_value(builder, stage ? stage : "unknown");
    json_builder_set_member_name(builder, "rc");
    json_builder_add_int_value(builder, rc);
    json_builder_end_object(builder);

    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    return json_to_string(root, FALSE);
}

/**
 * vivid_display_consumer_dmabuf_texture_query_vulkan_relay_caps_json:
 *
 * Returns a compact JSON description of the private Vulkan relay device.
 *
 * The GNOME helper cannot expose a host-owned VkDevice through GJS, so the
 * shadow-copy path mirrors waywallen's DMABUF_RELAY backend: this library owns
 * a small Vulkan instance/device, imports producer DMA-BUFs there, blits into a
 * LINEAR export, and hands that shadow DMA-BUF to GDK. These caps describe the
 * first leg of that route, not GDK's final shadow import. Advertising them lets
 * the daemon choose same-device vendor modifiers and DEVICE_LOCAL producer
 * allocations without pretending that GdkDmabufTextureBuilder can import those
 * producer buffers directly.
 *
 * Returns: (transfer full): JSON object with available/probe/renderNode/UUID
 * fields and a formats array of {fourcc, modifier, planeCount}.
 */
char*
vivid_display_consumer_dmabuf_texture_query_vulkan_relay_caps_json(void)
{
#ifndef WW_HAVE_VULKAN
    return build_vulkan_relay_caps_error_json("compile-disabled", -ENOSYS);
#else
    ww_vk_owned_t owned = {0};
    int rc = ww_vk_create_owned(&owned);
    if (rc != 0)
        return build_vulkan_relay_caps_error_json("create-owned-device", rc);

    ww_vk_backend_t backend = {0};
    rc = ww_vk_backend_load(&backend,
                            owned.instance,
                            owned.physical_device,
                            owned.device,
                            owned.queue_family_index,
                            NULL,
                            false);
    if (rc != 0) {
        char* json = build_vulkan_relay_caps_error_json("load-backend", rc);
        ww_vk_destroy_owned(&owned);
        return json;
    }

    VulkanRelayCapsProbe probe = {
        .caps = g_array_new(FALSE, FALSE, sizeof(VulkanRelayModifierCap)),
    };
    const uint32_t want_features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    rc = ww_vk_query_format_caps(&backend, want_features, vulkan_relay_caps_emit, &probe);

    guint32 render_major = 0;
    guint32 render_minor = 0;
    gchar render_node[64] = "";
    if (ww_vk_query_drm_render_node(&backend, &render_major, &render_minor) == 0)
        find_render_node_for_drm_ids(render_major, render_minor, render_node, sizeof(render_node));

    guint8 device_uuid[16] = {0};
    guint8 driver_uuid[16] = {0};
    g_autofree gchar* device_uuid_text = NULL;
    g_autofree gchar* driver_uuid_text = NULL;
    if (ww_vk_query_device_uuid(&backend, device_uuid, driver_uuid) == 0) {
        device_uuid_text = uuid_bytes_to_hex(device_uuid);
        driver_uuid_text = uuid_bytes_to_hex(driver_uuid);
    }

    int supports_device_local = 0;
    (void)ww_vk_query_supports_device_local(&backend, &supports_device_local);

    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(builder, rc == 0 && probe.caps && probe.caps->len > 0);
    json_builder_set_member_name(builder, "backend");
    json_builder_add_string_value(builder, "vulkan-dmabuf-relay");
    json_builder_set_member_name(builder, "probe");
    json_builder_add_string_value(builder,
                                  rc == 0 ? "vulkan-relay-format-probe"
                                          : "vulkan-relay-format-probe-failed");
    json_builder_set_member_name(builder, "rc");
    json_builder_add_int_value(builder, rc);
    json_builder_set_member_name(builder, "renderNode");
    json_builder_add_string_value(builder, render_node);
    json_builder_set_member_name(builder, "drmRenderMajor");
    json_builder_add_int_value(builder, render_major);
    json_builder_set_member_name(builder, "drmRenderMinor");
    json_builder_add_int_value(builder, render_minor);
    json_builder_set_member_name(builder, "deviceUuid");
    json_builder_add_string_value(builder, device_uuid_text ? device_uuid_text : "");
    json_builder_set_member_name(builder, "driverUuid");
    json_builder_add_string_value(builder, driver_uuid_text ? driver_uuid_text : "");
    json_builder_set_member_name(builder, "supportsDeviceLocal");
    json_builder_add_boolean_value(builder, supports_device_local != 0);
    json_builder_set_member_name(builder, "wantFeatures");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "sampled-image");
    json_builder_add_string_value(builder, "transfer-src");
    json_builder_end_array(builder);
    json_builder_set_member_name(builder, "formats");
    json_builder_begin_array(builder);
    if (rc == 0 && probe.caps) {
        for (guint i = 0; i < probe.caps->len; i++) {
            const VulkanRelayModifierCap cap =
                g_array_index(probe.caps, VulkanRelayModifierCap, i);
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "fourcc");
            json_builder_add_int_value(builder, cap.fourcc);
            json_builder_add_uint64_string(builder, "modifier", cap.modifier);
            json_builder_set_member_name(builder, "planeCount");
            json_builder_add_int_value(builder, cap.plane_count);
            json_builder_end_object(builder);
        }
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    char* json = json_to_string(root, FALSE);

    if (probe.caps)
        g_array_unref(probe.caps);
    ww_vk_backend_unload(&backend);
    ww_vk_destroy_owned(&owned);
    return json;
#endif
}

/*
 * Both release operations need a local handle for the daemon's syncobj. The
 * handle is a per-file-description reference to the same kernel object the
 * daemon waits on, so whatever fence state we attach through it is what the
 * daemon observes; destroying the handle afterwards only drops our reference.
 */
typedef int (*ReleaseSyncobjOp)(int drm_fd, guint32 handle, gpointer op_data);

static gboolean
with_release_syncobj_handle(const gchar*     render_node,
                            gint             syncobj_fd,
                            const gchar*     op_name,
                            ReleaseSyncobjOp op,
                            gpointer         op_data,
                            GError**         error)
{
    if (!render_node || !*render_node) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                    "missing render node for release syncobj %s",
                    op_name);
        return FALSE;
    }
    if (syncobj_fd < 0) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                            "invalid release syncobj fd");
        return FALSE;
    }

    const int drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                    "open(%s) for release syncobj %s failed: %s",
                    render_node,
                    op_name,
                    g_strerror(errno));
        return FALSE;
    }

    guint32 handle = 0;
    errno = 0;
    int result = drmSyncobjFDToHandle(drm_fd, syncobj_fd, &handle);
    if (result != 0) {
        const int error_code = errno != 0 ? errno : -result;
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                    "drmSyncobjFDToHandle(%s) failed: %s",
                    render_node,
                    g_strerror(error_code));
        close(drm_fd);
        return FALSE;
    }

    errno = 0;
    result = op(drm_fd, handle, op_data);
    const int op_error = errno != 0 ? errno : -result;
    errno = 0;
    const int destroy_result = drmSyncobjDestroy(drm_fd, handle);
    if (destroy_result != 0) {
        g_warning("VividDisplayConsumer: drmSyncobjDestroy(release handle=%u) "
                  "failed after %s: %s",
                  handle,
                  op_name,
                  g_strerror(errno != 0 ? errno : -destroy_result));
    }
    close(drm_fd);

    if (result != 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                    "release syncobj %s(%s handle=%u) failed: %s",
                    op_name,
                    render_node,
                    handle,
                    g_strerror(op_error));
        return FALSE;
    }

    return TRUE;
}

static int
release_syncobj_signal_op(int drm_fd, guint32 handle, gpointer op_data)
{
    (void)op_data;
    return drmSyncobjSignal(drm_fd, &handle, 1);
}

static int
release_syncobj_import_sync_file_op(int drm_fd, guint32 handle, gpointer op_data)
{
    const int sync_file_fd = *(const int*)op_data;
    return drmSyncobjImportSyncFile(drm_fd, handle, sync_file_fd);
}

/**
 * vivid_display_consumer_dmabuf_texture_signal_release_syncobj:
 * @render_node: producer render node named in BIND_BUFFERS
 * @syncobj_fd: binary release syncobj fd received with FRAME_READY
 *
 * Signals the per-frame release syncobj from the host. Used when the
 * consumer never submitted GPU work that reads the producer image, or once
 * such work is known to have completed.
 *
 * Returns: %TRUE when the release syncobj was signaled
 */
gboolean
vivid_display_consumer_dmabuf_texture_signal_release_syncobj(const gchar* render_node,
                                                             gint         syncobj_fd,
                                                             GError**     error)
{
    return with_release_syncobj_handle(render_node,
                                       syncobj_fd,
                                       "signal",
                                       release_syncobj_signal_op,
                                       NULL,
                                       error);
}

/**
 * vivid_display_consumer_dmabuf_texture_attach_release_sync_file:
 * @render_node: producer render node named in BIND_BUFFERS
 * @syncobj_fd: binary release syncobj fd received with FRAME_READY
 * @sync_file_fd: sync_file whose fence completes when the consumer's copy
 *   of the producer image has finished on the GPU; not consumed
 *
 * Replaces the release syncobj's fence with @sync_file_fd's fence. The
 * daemon's release wait then completes when the GPU finishes the copy,
 * without any consumer thread blocking on that copy. This is the
 * explicit-sync counterpart of the host signal above.
 *
 * Returns: %TRUE when the fence was attached
 */
gboolean
vivid_display_consumer_dmabuf_texture_attach_release_sync_file(const gchar* render_node,
                                                               gint         syncobj_fd,
                                                               gint         sync_file_fd,
                                                               GError**     error)
{
    if (sync_file_fd < 0) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                            "invalid release sync_file fd");
        return FALSE;
    }
    int fd = sync_file_fd;
    return with_release_syncobj_handle(render_node,
                                       syncobj_fd,
                                       "import-sync-file",
                                       release_syncobj_import_sync_file_op,
                                       &fd,
                                       error);
}

void
vivid_display_consumer_dmabuf_texture_close_fd(gint fd)
{
    if (fd >= 0)
        close(fd);
}
