/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_display_consumer_dmabuf_texture.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <gdk/wayland/gdkwayland.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

typedef EGLBoolean (*EglQueryDisplayAttribExt)(EGLDisplay dpy,
                                               EGLint     attribute,
                                               EGLAttrib* value);
typedef const char* (*EglQueryDeviceStringExt)(EGLDeviceEXT device,
                                               EGLint       name);

typedef struct
{
    gboolean found;
    guint8   device_uuid[16];
    guint8   driver_uuid[16];
} VulkanUuidMatch;

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

static gpointer
resolve_egl_proc(const char* name)
{
    return (gpointer)eglGetProcAddress(name);
}

static char*
normalize_render_node(const char* path)
{
    if (!path || !*path)
        return NULL;

    char* canonical = realpath(path, NULL);
    if (canonical)
        return canonical;

    return g_strdup(path);
}

static char*
uuid_bytes_to_hex(const guint8 uuid[16])
{
    GString* text = g_string_sized_new(32);
    for (guint i = 0; i < 16; i++)
        g_string_append_printf(text, "%02x", uuid[i]);
    return g_string_free(text, FALSE);
}

static char*
read_sysfs_text(const char* path)
{
    gchar* contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;

    return g_strstrip(contents);
}

static char*
vendor_from_id_text(const char* text)
{
    if (!text || !*text)
        return NULL;

    char* end = NULL;
    errno = 0;
    const guint64 id = g_ascii_strtoull(text, &end, 0);
    if (errno != 0 || !end || (*end != '\0' && !g_ascii_isspace(*end)))
        return NULL;

    switch (id) {
    case 0x10de:
        return g_strdup("nvidia");
    case 0x8086:
        return g_strdup("intel");
    case 0x1002:
    case 0x1022:
        return g_strdup("amd");
    default:
        return NULL;
    }
}

static char*
render_node_sysfs_device_path(const char* render_node)
{
    if (!render_node || !*render_node)
        return NULL;

    struct stat st;
    if (stat(render_node, &st) != 0) {
        g_warning("VividDisplayConsumer: stat(%s) failed during GPU identity probe: %s",
                  render_node,
                  g_strerror(errno));
        return NULL;
    }

    return g_strdup_printf("/sys/dev/char/%u:%u/device",
                           major(st.st_rdev),
                           minor(st.st_rdev));
}

static char*
render_node_pci_address(const char* render_node)
{
    g_autofree char* device_path = render_node_sysfs_device_path(render_node);
    if (!device_path)
        return NULL;

    char* canonical = realpath(device_path, NULL);
    if (!canonical)
        return NULL;

    char* basename = g_path_get_basename(canonical);
    g_free(canonical);
    return basename;
}

static char*
render_node_vendor(const char* render_node)
{
    g_autofree char* device_path = render_node_sysfs_device_path(render_node);
    if (!device_path)
        return NULL;

    g_autofree char* vendor_path = g_build_filename(device_path, "vendor", NULL);
    g_autofree char* vendor_text = read_sysfs_text(vendor_path);
    return vendor_from_id_text(vendor_text);
}

static EGLDisplay
egl_display_from_gdk_display(GdkDisplay* display)
{
    GdkDisplay* target = display ? display : gdk_display_get_default();
    if (!target || !GDK_IS_WAYLAND_DISPLAY(target))
        return EGL_NO_DISPLAY;

    return (EGLDisplay)gdk_wayland_display_get_egl_display(target);
}

static char*
render_node_from_egl_display(EGLDisplay egl_display)
{
    if (egl_display == EGL_NO_DISPLAY)
        return NULL;

    EglQueryDisplayAttribExt query_display_attrib =
        (EglQueryDisplayAttribExt)resolve_egl_proc("eglQueryDisplayAttribEXT");
    EglQueryDeviceStringExt query_device_string =
        (EglQueryDeviceStringExt)resolve_egl_proc("eglQueryDeviceStringEXT");
    if (!query_display_attrib || !query_device_string)
        return NULL;

    EGLAttrib device_attrib = 0;
    if (!query_display_attrib(egl_display, EGL_DEVICE_EXT, &device_attrib) ||
        device_attrib == 0) {
        return NULL;
    }

    EGLDeviceEXT device = (EGLDeviceEXT)device_attrib;
    const char* render_node = NULL;
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
    render_node = query_device_string(device, EGL_DRM_RENDER_NODE_FILE_EXT);
#endif
#ifdef EGL_DRM_DEVICE_FILE_EXT
    if (!render_node || !*render_node)
        render_node = query_device_string(device, EGL_DRM_DEVICE_FILE_EXT);
#endif

    return normalize_render_node(render_node);
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

static VulkanUuidMatch
vulkan_uuid_for_render_node(const char* render_node)
{
    VulkanUuidMatch match = {0};
    g_autofree char* normalized = normalize_render_node(render_node);
    if (!normalized || !*normalized)
        return match;

    const VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vivid consumer GPU identity probe",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "VividDisplayConsumer",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        g_warning("VividDisplayConsumer: vkCreateInstance for UUID probe failed result=%d",
                  (int)result);
        return match;
    }

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        vkDestroyInstance(instance, NULL);
        return match;
    }

    VkPhysicalDevice* devices = g_new0(VkPhysicalDevice, count);
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS) {
        g_free(devices);
        vkDestroyInstance(instance, NULL);
        return match;
    }

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceDrmPropertiesEXT drm_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
        };
        VkPhysicalDeviceIDProperties id_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id_props,
        };
        id_props.pNext = &drm_props;
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        if (!drm_props.hasRender)
            continue;

        gchar candidate[64];
        find_render_node_for_drm_ids(drm_props.renderMajor,
                                     drm_props.renderMinor,
                                     candidate,
                                     sizeof(candidate));
        g_autofree char* normalized_candidate = normalize_render_node(candidate);
        if (!normalized_candidate ||
            g_strcmp0(normalized_candidate, normalized) != 0) {
            continue;
        }

        memcpy(match.device_uuid, id_props.deviceUUID, sizeof(match.device_uuid));
        memcpy(match.driver_uuid, id_props.driverUUID, sizeof(match.driver_uuid));
        match.found = TRUE;
        break;
    }

    g_free(devices);
    vkDestroyInstance(instance, NULL);
    if (!match.found) {
        g_message("VividDisplayConsumer: Vulkan UUID probe found no physical "
                  "device matching render node %s",
                  normalized);
    }
    return match;
}

/**
 * vivid_display_consumer_dmabuf_texture_builder_build:
 * @builder: a configured #GdkDmabufTextureBuilder
 * @error: return location for a #GError
 *
 * Builds a #GdkTexture from a DMA-BUF texture builder.
 *
 * GDK's public C API accepts a nullable GDestroyNotify for closing resources
 * when the returned texture is released. The GIR metadata exposes that
 * DestroyNotify as a standalone parameter, and GJS refuses to call such
 * functions because there is no introspectable callback/data pairing. The
 * helper process already keeps the duplicated DMA-BUF fds alive for the whole
 * protocol BIND_BUFFERS generation, so this wrapper deliberately passes NULL
 * for the destroy callback and lets the existing generation owner close fds on
 * UNBIND or disconnect.
 *
 * Returns: (transfer full): the built texture
 */
GdkTexture*
vivid_display_consumer_dmabuf_texture_builder_build(
    GdkDmabufTextureBuilder* builder,
    GError**                 error)
{
    g_return_val_if_fail(GDK_IS_DMABUF_TEXTURE_BUILDER(builder), NULL);

    return gdk_dmabuf_texture_builder_build(builder, NULL, NULL, error);
}

/**
 * vivid_display_consumer_dmabuf_texture_get_render_node:
 * @display: (nullable): the #GdkDisplay used for DMA-BUF imports
 *
 * Returns the DRM render node behind GDK's current Wayland/EGL display.
 *
 * The producer's optimized DMA-BUF path must compare the producer's resolved
 * Vulkan render device against the consumer's actual import device. GDK exposes
 * the Wayland EGLDisplay, and EGL_EXT_device_query gives us that identity
 * without guessing from environment variables or scanning unrelated GPUs.
 *
 * Returns: (transfer full) (nullable): canonical render node path
 */
char*
vivid_display_consumer_dmabuf_texture_get_render_node(GdkDisplay* display)
{
    return render_node_from_egl_display(egl_display_from_gdk_display(display));
}

/**
 * vivid_display_consumer_dmabuf_texture_get_vendor:
 * @display: (nullable): the #GdkDisplay used for DMA-BUF imports
 *
 * Returns: (transfer full) (nullable): normalized vendor name
 */
char*
vivid_display_consumer_dmabuf_texture_get_vendor(GdkDisplay* display)
{
    g_autofree char* render_node =
        vivid_display_consumer_dmabuf_texture_get_render_node(display);
    return render_node_vendor(render_node);
}

/**
 * vivid_display_consumer_dmabuf_texture_get_pci_address:
 * @display: (nullable): the #GdkDisplay used for DMA-BUF imports
 *
 * Returns: (transfer full) (nullable): PCI address backing the render node
 */
char*
vivid_display_consumer_dmabuf_texture_get_pci_address(GdkDisplay* display)
{
    g_autofree char* render_node =
        vivid_display_consumer_dmabuf_texture_get_render_node(display);
    return render_node_pci_address(render_node);
}

char*
vivid_display_consumer_dmabuf_texture_get_device_uuid(GdkDisplay* display)
{
    g_autofree char* render_node =
        vivid_display_consumer_dmabuf_texture_get_render_node(display);
    VulkanUuidMatch match = vulkan_uuid_for_render_node(render_node);
    return match.found ? uuid_bytes_to_hex(match.device_uuid) : NULL;
}

char*
vivid_display_consumer_dmabuf_texture_get_driver_uuid(GdkDisplay* display)
{
    g_autofree char* render_node =
        vivid_display_consumer_dmabuf_texture_get_render_node(display);
    VulkanUuidMatch match = vulkan_uuid_for_render_node(render_node);
    return match.found ? uuid_bytes_to_hex(match.driver_uuid) : NULL;
}

/**
 * vivid_display_consumer_dmabuf_texture_probe_plane_count:
 * @display: (nullable): the #GdkDisplay used for DMA-BUF imports
 * @fourcc: DRM fourcc
 * @modifier: DRM format modifier
 *
 * Probes the real plane count for a GDK-advertised import tuple.
 *
 * GdkDmabufFormats exposes importable fourcc/modifier pairs but not plane
 * counts. Waywallen's negotiation treats (fourcc, modifier, plane_count) as
 * the indivisible compatibility tuple, so the helper performs the same style
 * of GBM BO probe on GDK's actual render node. Returning 0 means "unknown";
 * callers must advertise planeCount=1 in that case so a multi-plane modifier
 * is never selected by accident.
 *
 * Returns: probed plane count, or 0 when unknown
 */
guint
vivid_display_consumer_dmabuf_texture_probe_plane_count(GdkDisplay* display,
                                                        guint32     fourcc,
                                                        guint64     modifier)
{
    if (modifier == DRM_FORMAT_MOD_INVALID)
        return 1;

    g_autofree char* render_node =
        vivid_display_consumer_dmabuf_texture_get_render_node(display);
    if (!render_node || !*render_node) {
        g_warning("VividDisplayConsumer: GBM plane probe skipped because GDK "
                  "render node is unknown");
        return 0;
    }

    int fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        g_warning("VividDisplayConsumer: open(%s) failed during GBM plane probe: %s",
                  render_node,
                  g_strerror(errno));
        return 0;
    }

    struct gbm_device* device = gbm_create_device(fd);
    if (!device) {
        g_warning("VividDisplayConsumer: gbm_create_device(%s) failed during "
                  "plane probe",
                  render_node);
        close(fd);
        return 0;
    }

    const uint64_t modifiers[] = { modifier };
    struct gbm_bo* bo = gbm_bo_create_with_modifiers2(device,
                                                      64,
                                                      64,
                                                      fourcc,
                                                      modifiers,
                                                      1,
                                                      GBM_BO_USE_RENDERING);
    if (!bo) {
        g_warning("VividDisplayConsumer: GBM plane probe failed fourcc=0x%08x "
                  "modifier=0x%016" G_GINT64_MODIFIER "x node=%s",
                  fourcc,
                  modifier,
                  render_node);
        gbm_device_destroy(device);
        close(fd);
        return 0;
    }

    int planes = gbm_bo_get_plane_count(bo);
    gbm_bo_destroy(bo);
    gbm_device_destroy(device);
    close(fd);

    if (planes <= 0) {
        g_warning("VividDisplayConsumer: GBM plane probe returned invalid plane "
                  "count fourcc=0x%08x modifier=0x%016" G_GINT64_MODIFIER "x "
                  "planes=%d",
                  fourcc,
                  modifier,
                  planes);
        return 0;
    }

    return (guint)planes;
}

/**
 * vivid_display_consumer_dmabuf_texture_wait_sync_file:
 * @sync_fd: acquire fence fd received with FRAME_READY
 * @timeout_msec: bounded wait in milliseconds
 *
 * Waits until the acquire dma_fence sync_file is readable. The caller keeps fd
 * ownership so GJS can close it in a single finally block regardless of success
 * or error.
 *
 * Returns: %TRUE when the fence signaled before the timeout
 */
gboolean
vivid_display_consumer_dmabuf_texture_wait_sync_file(gint     sync_fd,
                                                     gint     timeout_msec,
                                                     GError** error)
{
    if (sync_fd < 0) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                            "invalid acquire sync_file fd");
        return FALSE;
    }

    struct pollfd pfd = {
        .fd = sync_fd,
        .events = POLLIN,
        .revents = 0,
    };
    for (;;) {
        const int result = poll(&pfd, 1, timeout_msec < 0 ? -1 : timeout_msec);
        if (result > 0) {
            if ((pfd.revents & POLLIN) != 0)
                return TRUE;
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                        VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                        "acquire sync_file fd=%d returned unexpected poll events=0x%x",
                        sync_fd,
                        pfd.revents);
            return FALSE;
        }
        if (result == 0) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                        VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                        "timed out waiting for acquire sync_file fd=%d after %dms",
                        sync_fd,
                        timeout_msec);
            return FALSE;
        }
        if (errno == EINTR)
            continue;

        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                    "poll(acquire sync_file fd=%d) failed: %s",
                    sync_fd,
                    g_strerror(errno));
        return FALSE;
    }
}

/**
 * vivid_display_consumer_dmabuf_texture_signal_release_syncobj:
 * @render_node: producer render node named in BIND_BUFFERS
 * @syncobj_fd: binary release syncobj fd received with FRAME_READY
 *
 * Signals the per-frame release syncobj. GTK/GDK does not currently expose a
 * callback for "this dmabuf texture is no longer referenced by GPU work", so
 * the helper signals after the frame is accepted into the paintable path. This
 * keeps fd ownership and failure behavior explicit while the renderer process
 * migration moves the real signal point into renderer-side code.
 *
 * Returns: %TRUE when the release syncobj was signaled
 */
gboolean
vivid_display_consumer_dmabuf_texture_signal_release_syncobj(const gchar* render_node,
                                                             gint         syncobj_fd,
                                                             GError**     error)
{
    if (!render_node || !*render_node) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                            VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                            "missing render node for release syncobj signal");
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
                    "open(%s) for release syncobj signal failed: %s",
                    render_node,
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
    result = drmSyncobjSignal(drm_fd, &handle, 1);
    const int signal_error = errno != 0 ? errno : -result;
    errno = 0;
    const int destroy_result = drmSyncobjDestroy(drm_fd, handle);
    if (destroy_result != 0) {
        g_warning("VividDisplayConsumer: drmSyncobjDestroy(release handle=%u) "
                  "failed after signal: %s",
                  handle,
                  g_strerror(errno != 0 ? errno : -destroy_result));
    }
    close(drm_fd);

    if (result != 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR,
                    VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_ERROR_FAILED,
                    "drmSyncobjSignal(%s handle=%u) failed: %s",
                    render_node,
                    handle,
                    g_strerror(signal_error));
        return FALSE;
    }

    return TRUE;
}

void
vivid_display_consumer_dmabuf_texture_close_fd(gint fd)
{
    if (fd >= 0)
        close(fd);
}
