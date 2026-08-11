/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#define _GNU_SOURCE

#include "vivid_display_consumer_buffer_paintable.h"
#include "vivid_display_consumer_dmabuf_texture.h"
#include "vivid_display_consumer_vulkan_backend.h"
#include "vivid_display_consumer_vulkan_blit.h"
#include "vivid_display_protocol_json.h"

#include <errno.h>
#include <fcntl.h>
#include <graphene.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR \
    (vivid_display_consumer_buffer_paintable_error_quark())

typedef enum
{
    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
} VividDisplayConsumerBufferPaintableError;

enum {
    MAX_DMABUF_PLANES = 4,
};

typedef struct
{
    gint    fd;
    guint32 fd_index;
    guint32 stride;
    guint32 offset;
} VividDmaBufPlane;

typedef struct
{
    guint32 index;
    guint64 size;
    guint   n_planes;
    VividDmaBufPlane planes[MAX_DMABUF_PLANES];
    ww_vk_backend_t*       vk_backend;
    ww_vk_imported_image_t vk_image;
    gboolean               has_vk_image;
} VividDmaBufBuffer;

typedef struct
{
    guint64   generation;
    guint32   width;
    guint32   height;
    guint32   fourcc;
    guint64   modifier;
    gchar*    render_node;
    guint8    consumer_device_uuid[VK_UUID_SIZE];
    gboolean  premultiplied;
    GPtrArray* buffers;
} VividDmaBufGeneration;

typedef struct
{
    gint  fds[MAX_DMABUF_PLANES];
    guint n_fds;
} VividTextureDupFds;

typedef struct
{
    const gchar* render_node;
    guint64      generation;
    guint32      buffer_index;
    const gchar* context;
} VividReleaseSignalContext;


struct _VividDisplayConsumerBufferPaintable
{
    GObject parent_instance;

    GPtrArray*  generations;
    GdkTexture* texture;
    GdkDisplay* display;

    guint64 current_generation;
    guint32 current_buffer_index;
    guint32 current_width;
    guint32 current_height;

    gboolean have_config;
    gfloat   source[4];
    gfloat   dest[4];
    guint    transform;
    gfloat   clear[4];

    ww_vk_owned_t   vk_owned;
    ww_vk_blitter_t vk_blitter;
    gboolean        vk_relay_ready;
    guint8          vk_device_uuid[VK_UUID_SIZE];
};

static void vivid_display_consumer_buffer_paintable_iface_init(
    GdkPaintableInterface* iface);
static void clear_state(VividDisplayConsumerBufferPaintable* self,
                        gboolean                             invalidate);
static gboolean signal_release_syncobj_fd(const gchar* render_node,
                                          gint         release_syncobj_fd,
                                          guint64      generation,
                                          guint32      buffer_index,
                                          const gchar* context);
static gboolean ensure_vulkan_relay(VividDisplayConsumerBufferPaintable* self,
                                     const guint8 expected_device_uuid[VK_UUID_SIZE],
                                     GError**                             error);
static gboolean import_shadow_buffer(VividDisplayConsumerBufferPaintable* self,
                                     VividDmaBufGeneration*               generation,
                                     VividDmaBufBuffer*                   buffer,
                                     GError**                             error);
static GdkTexture* build_shadow_texture_for_generation(
    VividDisplayConsumerBufferPaintable* self,
    VividDmaBufGeneration*               generation,
    GError**                             error);

G_DEFINE_TYPE_WITH_CODE(
    VividDisplayConsumerBufferPaintable,
    vivid_display_consumer_buffer_paintable,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE,
                          vivid_display_consumer_buffer_paintable_iface_init))

static GQuark
vivid_display_consumer_buffer_paintable_error_quark(void)
{
    return g_quark_from_static_string("vivid_display_consumer_buffer_paintable-error");
}

static gboolean
hex_uuid_to_bytes(const gchar* text,
                  guint8       out_uuid[VK_UUID_SIZE])
{
    if (!text || !*text || !out_uuid)
        return FALSE;

    gchar compact[(VK_UUID_SIZE * 2) + 1] = {0};
    guint cursor = 0;
    for (const gchar* p = text; *p; p++) {
        if (*p == '-')
            continue;
        if (!g_ascii_isxdigit(*p) || cursor >= sizeof(compact) - 1)
            return FALSE;
        compact[cursor++] = *p;
    }
    if (cursor != VK_UUID_SIZE * 2)
        return FALSE;

    for (guint i = 0; i < VK_UUID_SIZE; i++) {
        gchar byte_text[3] = {compact[i * 2], compact[i * 2 + 1], '\0'};
        gchar* end = NULL;
        const guint64 value = g_ascii_strtoull(byte_text, &end, 16);
        if (!end || *end != '\0' || value > 0xff)
            return FALSE;
        out_uuid[i] = (guint8)value;
    }
    return TRUE;
}

static void
uuid_bytes_to_hex(const guint8 uuid[VK_UUID_SIZE],
                  gchar        out_text[(VK_UUID_SIZE * 2) + 1])
{
    for (guint i = 0; i < VK_UUID_SIZE; i++)
        g_snprintf(out_text + (i * 2), 3, "%02x", uuid[i]);
}

static void
close_texture_dup_fds(gpointer data)
{
    VividTextureDupFds* dup_fds = data;
    if (!dup_fds)
        return;

    for (guint i = 0; i < dup_fds->n_fds; i++) {
        if (dup_fds->fds[i] >= 0)
            close(dup_fds->fds[i]);
    }
    g_free(dup_fds);
}

static void
buffer_free(gpointer data)
{
    VividDmaBufBuffer* buffer = data;
    if (!buffer)
        return;

    for (guint i = 0; i < buffer->n_planes; i++) {
        if (buffer->planes[i].fd >= 0)
            close(buffer->planes[i].fd);
        buffer->planes[i].fd = -1;
    }
    if (buffer->has_vk_image && buffer->vk_backend && buffer->vk_backend->loaded)
        ww_vk_destroy_imported_image(buffer->vk_backend, &buffer->vk_image);
    buffer->has_vk_image = FALSE;
    g_free(buffer);
}

static void
generation_free(gpointer data)
{
    VividDmaBufGeneration* generation = data;
    if (!generation)
        return;

    g_clear_pointer(&generation->buffers, g_ptr_array_unref);
    g_free(generation->render_node);
    g_free(generation);
}

static const gchar*
json_object_get_string_member_default(JsonObject*  object,
                                      const gchar* member,
                                      const gchar* fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;

    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return fallback;

    const gchar* value = json_node_get_string(node);
    return value ? value : fallback;
}

static gboolean
json_member_get_uint64(JsonObject*  object,
                       const gchar* member,
                       guint64      fallback,
                       guint64*     out_value)
{
    if (!object || !out_value)
        return FALSE;
    if (!json_object_has_member(object, member)) {
        *out_value = fallback;
        return TRUE;
    }

    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;

    const GType value_type = json_node_get_value_type(node);
    if (value_type == G_TYPE_STRING) {
        const gchar* text = json_node_get_string(node);
        if (!text || !*text)
            return FALSE;

        gchar* end = NULL;
        errno = 0;
        const guint64 value = g_ascii_strtoull(text, &end, 0);
        if (errno != 0 || !end || *end != '\0')
            return FALSE;

        *out_value = value;
        return TRUE;
    }

    if (value_type == G_TYPE_INT64 || value_type == G_TYPE_INT ||
        value_type == G_TYPE_LONG || value_type == G_TYPE_UINT64 ||
        value_type == G_TYPE_UINT || value_type == G_TYPE_ULONG) {
        const gint64 signed_value = json_node_get_int(node);
        if (signed_value < 0)
            return FALSE;
        *out_value = (guint64)signed_value;
        return TRUE;
    }

    if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_FLOAT) {
        const gdouble number = json_node_get_double(node);
        if (!isfinite(number) || number < 0.0)
            return FALSE;
        *out_value = (guint64)number;
        return TRUE;
    }

    return FALSE;
}

static gboolean
json_member_get_uint32(JsonObject*  object,
                       const gchar* member,
                       guint32      fallback,
                       guint32*     out_value)
{
    guint64 value = 0;
    if (!json_member_get_uint64(object, member, fallback, &value) || value > G_MAXUINT32)
        return FALSE;

    *out_value = (guint32)value;
    return TRUE;
}

static gboolean
json_member_get_boolean_default(JsonObject*  object,
                                const gchar* member,
                                gboolean     fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    return json_object_get_boolean_member(object, member);
}

static JsonArray*
json_object_get_array_member_or_null(JsonObject* object,
                                     const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;

    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;
    return json_node_get_array(node);
}

static VividDmaBufGeneration*
find_generation(VividDisplayConsumerBufferPaintable* self,
                guint64                               generation)
{
    for (guint i = 0; i < self->generations->len; i++) {
        VividDmaBufGeneration* candidate = g_ptr_array_index(self->generations, i);
        if (candidate->generation == generation)
            return candidate;
    }
    return NULL;
}

static VividDmaBufBuffer*
find_buffer(VividDmaBufGeneration* generation,
            guint32                 buffer_index)
{
    if (!generation)
        return NULL;

    for (guint i = 0; i < generation->buffers->len; i++) {
        VividDmaBufBuffer* candidate = g_ptr_array_index(generation->buffers, i);
        if (candidate->index == buffer_index)
            return candidate;
    }
    return NULL;
}

static gboolean
signal_release_syncobj_fd(const gchar* render_node,
                          gint         release_syncobj_fd,
                          guint64      generation,
                          guint32      buffer_index,
                          const gchar* context)
{
    g_autoptr(GError) error = NULL;
    if (!vivid_display_consumer_dmabuf_texture_signal_release_syncobj(render_node,
                                                                      release_syncobj_fd,
                                                                      &error)) {
        g_warning("VividDisplayConsumer: release syncobj signal failed "
                  "generation=%" G_GUINT64_FORMAT " buffer=%u context=%s: %s",
                  generation,
                  buffer_index,
                  context ? context : "unknown",
                  error ? error->message : "unknown release signal error");
        return FALSE;
    }

    return TRUE;
}

static int
blitter_signal_release_syncobj(int release_syncobj_fd,
                               void* user_data)
{
    const VividReleaseSignalContext* context = user_data;
    if (!context)
        return -EINVAL;

    return signal_release_syncobj_fd(context->render_node,
                                     release_syncobj_fd,
                                     context->generation,
                                     context->buffer_index,
                                     context->context)
        ? 0
        : -EIO;
}

static void
remove_generation(VividDisplayConsumerBufferPaintable* self,
                  guint64                               generation)
{
    for (guint i = 0; i < self->generations->len; i++) {
        VividDmaBufGeneration* candidate = g_ptr_array_index(self->generations, i);
        if (candidate->generation == generation) {
            g_ptr_array_remove_index(self->generations, i);
            return;
        }
    }
}

static gboolean
ensure_vulkan_relay(VividDisplayConsumerBufferPaintable* self,
                    const guint8 expected_device_uuid[VK_UUID_SIZE],
                    GError**                             error)
{
    if (self->vk_relay_ready) {
        if (memcmp(self->vk_device_uuid, expected_device_uuid, VK_UUID_SIZE) != 0) {
            g_set_error_literal(error,
                                VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                                VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                                "consumer deviceUUID changed within one GNOME paintable lifetime");
            return FALSE;
        }
        return TRUE;
    }

    /*
     * Capability discovery and buffer import happen at different times in the
     * GNOME helper. Selecting an arbitrary physical device here can therefore
     * advertise modifiers from the preferred GPU but import them on another
     * GPU. BIND_BUFFERS carries the exact deviceUUID accepted by negotiation;
     * make that identity the creation constraint so preference, modifier
     * support, and the live Vulkan relay remain one atomic device contract.
     */
    int rc = ww_vk_create_owned_for_device_uuid(&self->vk_owned,
                                                expected_device_uuid);
    if (rc != 0) {
        gchar uuid_text[(VK_UUID_SIZE * 2) + 1] = {0};
        uuid_bytes_to_hex(expected_device_uuid, uuid_text);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to create Vulkan relay device for consumer deviceUUID=%s rc=%d",
                    uuid_text,
                    rc);
        return FALSE;
    }

    rc = ww_vk_blitter_init(&self->vk_blitter,
                            self->vk_owned.instance,
                            self->vk_owned.physical_device,
                            self->vk_owned.device,
                            self->vk_owned.queue_family_index,
                            self->vk_owned.queue,
                            NULL);
    if (rc != 0) {
        ww_vk_destroy_owned(&self->vk_owned);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to initialize Vulkan DMA-BUF relay blitter rc=%d",
                    rc);
        return FALSE;
    }

    memcpy(self->vk_device_uuid, expected_device_uuid, VK_UUID_SIZE);
    self->vk_relay_ready = TRUE;
    gchar uuid_text[(VK_UUID_SIZE * 2) + 1] = {0};
    uuid_bytes_to_hex(expected_device_uuid, uuid_text);
    g_message("VividDisplayConsumer: Vulkan relay pinned to consumer deviceUUID=%s",
              uuid_text);
    return TRUE;
}

static gboolean
ensure_shadow_export(VividDisplayConsumerBufferPaintable* self,
                     VividDmaBufGeneration*               generation,
                     GError**                             error)
{
    VkFormat format = ww_fourcc_to_vk_format(generation->fourcc);
    if (format == VK_FORMAT_UNDEFINED) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "unsupported fourcc for Vulkan shadow relay fourcc=0x%08x",
                    generation->fourcc);
        return FALSE;
    }

    const int rc = ww_vk_blitter_ensure_shadow_exportable(&self->vk_blitter,
                                                          generation->width,
                                                          generation->height,
                                                          format);
    if (rc != 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to create exportable Vulkan shadow DMA-BUF rc=%d "
                    "generation=%" G_GUINT64_FORMAT " size=%ux%u fourcc=0x%08x",
                    rc,
                    generation->generation,
                    generation->width,
                    generation->height,
                    generation->fourcc);
        return FALSE;
    }
    return TRUE;
}

static gboolean
import_shadow_buffer(VividDisplayConsumerBufferPaintable* self,
                     VividDmaBufGeneration*               generation,
                     VividDmaBufBuffer*                   buffer,
                     GError**                             error)
{
    if (!ensure_vulkan_relay(self, generation->consumer_device_uuid, error))
        return FALSE;

    ww_vk_dmabuf_import_t import = {
        .fourcc = generation->fourcc,
        .width = generation->width,
        .height = generation->height,
        .modifier = generation->modifier,
        .n_planes = buffer->n_planes,
    };
    for (guint plane = 0; plane < buffer->n_planes; plane++) {
        import.fds[plane] = buffer->planes[plane].fd;
        import.strides[plane] = buffer->planes[plane].stride;
        import.offsets[plane] = buffer->planes[plane].offset;
    }
    for (guint plane = 1; plane < buffer->n_planes; plane++) {
        if (buffer->planes[plane].fd_index != buffer->planes[0].fd_index) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                        "Vulkan shadow relay requires one shared DMA-BUF fd per buffer; "
                        "generation=%" G_GUINT64_FORMAT " buffer=%u plane=%u has fdIndex=%u "
                        "fdIndex0=%u",
                        generation->generation,
                        buffer->index,
                        plane,
                        buffer->planes[plane].fd_index,
                        buffer->planes[0].fd_index);
            return FALSE;
        }
    }

    /*
     * The Vulkan external-memory import consumes the fd it receives on
     * success. Keep the protocol generation's original fd alive for error
     * reporting and cleanup by importing a duplicate, matching waywallen's
     * ownership contract without coupling the direct GDK path to relay state.
     */
    import.fds[0] = fcntl(buffer->planes[0].fd, F_DUPFD_CLOEXEC, 0);
    if (import.fds[0] < 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to duplicate DMA-BUF fd for Vulkan relay buffer=%u: %s",
                    buffer->index,
                    g_strerror(errno));
        return FALSE;
    }

    const int rc = ww_vk_import_dmabuf(&self->vk_blitter.backend,
                                       &import,
                                       &buffer->vk_image);
    if (rc != 0) {
        errno = 0;
        if (fcntl(import.fds[0], F_GETFD) != -1 || errno != EBADF)
            close(import.fds[0]);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to import producer DMA-BUF into Vulkan relay rc=%d "
                    "generation=%" G_GUINT64_FORMAT " buffer=%u fourcc=0x%08x modifier=0x%016"
                    G_GINT64_MODIFIER "x planes=%u",
                    rc,
                    generation->generation,
                    buffer->index,
                    generation->fourcc,
                    generation->modifier,
                    buffer->n_planes);
        return FALSE;
    }

    buffer->vk_backend = &self->vk_blitter.backend;
    buffer->has_vk_image = TRUE;
    return TRUE;
}

static GdkTexture*
build_shadow_texture_for_generation(VividDisplayConsumerBufferPaintable* self,
                                    VividDmaBufGeneration*               generation,
                                    GError**                             error)
{
    int shadow_fd = -1;
    uint32_t n_planes = 0;
    uint32_t strides[MAX_DMABUF_PLANES] = {0};
    uint64_t offsets[MAX_DMABUF_PLANES] = {0};
    uint64_t modifier = 0;
    int rc = ww_vk_blitter_get_export(&self->vk_blitter,
                                      &shadow_fd,
                                      &n_planes,
                                      strides,
                                      offsets,
                                      &modifier);
    if (rc != 0 || shadow_fd < 0 || n_planes == 0 || n_planes > MAX_DMABUF_PLANES) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "Vulkan relay has no exportable shadow DMA-BUF rc=%d",
                    rc);
        return NULL;
    }
    if (!self->display)
        self->display = gdk_display_get_default();
    if (!self->display) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                            "GDK display is unavailable for Vulkan shadow DMA-BUF import");
        return NULL;
    }

    VividTextureDupFds* dup_fds = g_new0(VividTextureDupFds, 1);
    for (guint i = 0; i < MAX_DMABUF_PLANES; i++)
        dup_fds->fds[i] = -1;
    GdkDmabufTextureBuilder* builder = gdk_dmabuf_texture_builder_new();
    gdk_dmabuf_texture_builder_set_display(builder, self->display);
    gdk_dmabuf_texture_builder_set_width(builder, generation->width);
    gdk_dmabuf_texture_builder_set_height(builder, generation->height);
    gdk_dmabuf_texture_builder_set_fourcc(builder, generation->fourcc);
    gdk_dmabuf_texture_builder_set_modifier(builder, modifier);
    gdk_dmabuf_texture_builder_set_premultiplied(builder, generation->premultiplied);
    gdk_dmabuf_texture_builder_set_n_planes(builder, n_planes);

    for (uint32_t plane = 0; plane < n_planes; plane++) {
        const gint dup_fd = fcntl(shadow_fd, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                        "failed to duplicate Vulkan shadow DMA-BUF fd: %s",
                        g_strerror(errno));
            close_texture_dup_fds(dup_fds);
            g_object_unref(builder);
            return NULL;
        }
        dup_fds->fds[plane] = dup_fd;
        dup_fds->n_fds = plane + 1;
        gdk_dmabuf_texture_builder_set_fd(builder, plane, dup_fd);
        gdk_dmabuf_texture_builder_set_stride(builder, plane, strides[plane]);
        gdk_dmabuf_texture_builder_set_offset(builder, plane, offsets[plane]);
    }

    GdkTexture* texture =
        gdk_dmabuf_texture_builder_build(builder, close_texture_dup_fds, dup_fds, error);
    g_object_unref(builder);
    if (!texture)
        close_texture_dup_fds(dup_fds);
    return texture;
}

static gboolean
append_generation_from_json(VividDisplayConsumerBufferPaintable* self,
                            JsonObject*                           object,
                            GUnixFDList*                          fd_list,
                            GError**                              error)
{
    guint64 generation_id = 0;
    guint32 width = 0;
    guint32 height = 0;
    guint32 fourcc = 0;
    guint64 modifier = 0;
    guint8 consumer_device_uuid[VK_UUID_SIZE] = {0};
    const gchar* consumer_device_uuid_text =
        json_object_get_string_member_default(
            object,
            VIVID_JSON_BIND_BUFFERS_CONSUMER_DEVICE_UUID,
            "");
    if (!json_member_get_uint64(object, "generation", 0, &generation_id) ||
        generation_id == 0 ||
        !json_member_get_uint32(object, "width", 0, &width) ||
        width == 0 ||
        !json_member_get_uint32(object, "height", 0, &height) ||
        height == 0 ||
        !json_member_get_uint32(object, "fourcc", 0, &fourcc) ||
        fourcc == 0 ||
        !json_member_get_uint64(object, "modifier", 0, &modifier) ||
        !hex_uuid_to_bytes(consumer_device_uuid_text, consumer_device_uuid)) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                            "BIND_BUFFERS JSON is missing generation/size/fourcc/modifier/consumerDeviceUuid");
        return FALSE;
    }

    const gchar* presentation_path =
        json_object_get_string_member_default(
            object,
            VIVID_JSON_BIND_BUFFERS_PRESENTATION_PATH,
            "");
    if (g_strcmp0(presentation_path, "shadow-copy") != 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                    "GNOME consumer supports only shadow-copy presentation; received %s",
                    presentation_path && *presentation_path
                        ? presentation_path
                        : "(missing)");
        return FALSE;
    }

    JsonArray* buffers = json_object_get_array_member_or_null(object, "buffers");
    if (!buffers || json_array_get_length(buffers) == 0) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                            "BIND_BUFFERS JSON contains no buffers");
        return FALSE;
    }

    VividDmaBufGeneration* generation = g_new0(VividDmaBufGeneration, 1);
    generation->generation = generation_id;
    generation->width = width;
    generation->height = height;
    generation->fourcc = fourcc;
    generation->modifier = modifier;
    generation->render_node =
        g_strdup(json_object_get_string_member_default(
            object,
            VIVID_JSON_BIND_BUFFERS_RENDER_NODE,
            ""));
    memcpy(generation->consumer_device_uuid,
           consumer_device_uuid,
           VK_UUID_SIZE);
    generation->premultiplied =
        json_member_get_boolean_default(object, "premultiplied", FALSE);
    generation->buffers = g_ptr_array_new_with_free_func(buffer_free);

    if (!ensure_vulkan_relay(self, generation->consumer_device_uuid, error) ||
        !ensure_shadow_export(self, generation, error)) {
        generation_free(generation);
        return FALSE;
    }

    for (guint buffer_i = 0; buffer_i < json_array_get_length(buffers); buffer_i++) {
        JsonObject* buffer_object = json_array_get_object_element(buffers, buffer_i);
        if (!buffer_object) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                        "buffer %u is not an object",
                        buffer_i);
            generation_free(generation);
            return FALSE;
        }

        JsonArray* planes = json_object_get_array_member_or_null(buffer_object, "planes");
        if (!planes ||
            json_array_get_length(planes) == 0 ||
            json_array_get_length(planes) > MAX_DMABUF_PLANES) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                        "buffer %u has invalid plane count",
                        buffer_i);
            generation_free(generation);
            return FALSE;
        }

        VividDmaBufBuffer* buffer = g_new0(VividDmaBufBuffer, 1);
        for (guint i = 0; i < MAX_DMABUF_PLANES; i++)
            buffer->planes[i].fd = -1;

        if (!json_member_get_uint32(buffer_object, "index", buffer_i, &buffer->index) ||
            !json_member_get_uint64(buffer_object, "size", 0, &buffer->size)) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                        "buffer %u is missing index/size",
                        buffer_i);
            buffer_free(buffer);
            generation_free(generation);
            return FALSE;
        }

        buffer->n_planes = json_array_get_length(planes);
        for (guint plane_i = 0; plane_i < buffer->n_planes; plane_i++) {
            JsonObject* plane_object = json_array_get_object_element(planes, plane_i);
            if (!plane_object) {
                g_set_error(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                            "buffer %u plane %u is not an object",
                            buffer_i,
                            plane_i);
                buffer_free(buffer);
                generation_free(generation);
                return FALSE;
            }

            guint32 fd_index = plane_i;
            guint32 stride = 0;
            guint32 offset = 0;
            if (!json_member_get_uint32(plane_object, "fdIndex", plane_i, &fd_index) ||
                !json_member_get_uint32(plane_object, "stride", 0, &stride) ||
                stride == 0 ||
                !json_member_get_uint32(plane_object, "offset", 0, &offset)) {
                g_set_error(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                            "buffer %u plane %u is missing fdIndex/stride/offset",
                            buffer_i,
                            plane_i);
                buffer_free(buffer);
                generation_free(generation);
                return FALSE;
            }

            GError* fd_error = NULL;
            const gint fd = g_unix_fd_list_get(fd_list, (gint)fd_index, &fd_error);
            if (fd < 0) {
                g_set_error(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                            "failed to acquire fdIndex=%u for buffer %u plane %u: %s",
                            fd_index,
                            buffer_i,
                            plane_i,
                            fd_error ? fd_error->message : "unknown fd-list error");
                g_clear_error(&fd_error);
                buffer_free(buffer);
                generation_free(generation);
                return FALSE;
            }

            buffer->planes[plane_i].fd = fd;
            buffer->planes[plane_i].fd_index = fd_index;
            buffer->planes[plane_i].stride = stride;
            buffer->planes[plane_i].offset = offset;
        }

        if (!import_shadow_buffer(self, generation, buffer, error)) {
            buffer_free(buffer);
            generation_free(generation);
            return FALSE;
        }

        g_ptr_array_add(generation->buffers, buffer);
    }

    remove_generation(self, generation_id);
    g_ptr_array_add(self->generations, generation);
    return TRUE;
}

static void
draw_source_to_dest(VividDisplayConsumerBufferPaintable* self,
                    GtkSnapshot*                         snapshot,
                    const gfloat                         source[4],
                    const gfloat                         dest[4])
{
    const gfloat source_width = source[2] > 0.0f
        ? source[2]
        : (gfloat)self->current_width;
    const gfloat source_height = source[3] > 0.0f
        ? source[3]
        : (gfloat)self->current_height;
    const gfloat scale_x = dest[2] / source_width;
    const gfloat scale_y = dest[3] / source_height;

    graphene_rect_t clip;
    graphene_rect_init(&clip, dest[0], dest[1], dest[2], dest[3]);
    gtk_snapshot_push_clip(snapshot, &clip);
    gtk_snapshot_save(snapshot);

    graphene_point_t offset;
    graphene_point_init(&offset, dest[0] - source[0] * scale_x, dest[1] - source[1] * scale_y);
    gtk_snapshot_translate(snapshot, &offset);
    gtk_snapshot_scale(snapshot, scale_x, scale_y);

    graphene_rect_t full;
    graphene_rect_init(&full,
                       0.0f,
                       0.0f,
                       (gfloat)self->current_width,
                       (gfloat)self->current_height);
    gtk_snapshot_append_texture(snapshot, self->texture, &full);

    gtk_snapshot_restore(snapshot);
    gtk_snapshot_pop(snapshot);
}

static void
snapshot_vfunc(GdkPaintable* paintable,
               GdkSnapshot*  snapshot,
               gdouble       width,
               gdouble       height)
{
    VividDisplayConsumerBufferPaintable* self =
        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE(paintable);
    GtkSnapshot* gtk_snapshot = GTK_SNAPSHOT(snapshot);

    if (!isfinite(width) || width <= 0.0)
        width = self->current_width > 0 ? (gdouble)self->current_width : 0.0;
    if (!isfinite(height) || height <= 0.0)
        height = self->current_height > 0 ? (gdouble)self->current_height : 0.0;
    if (width <= 0.0 || height <= 0.0)
        return;

    if (self->clear[3] > 0.0f) {
        GdkRGBA clear = {
            self->clear[0],
            self->clear[1],
            self->clear[2],
            self->clear[3],
        };
        graphene_rect_t full;
        graphene_rect_init(&full, 0.0f, 0.0f, (gfloat)width, (gfloat)height);
        gtk_snapshot_append_color(gtk_snapshot, &clear, &full);
    }

    if (!self->texture)
        return;

    if (!self->have_config) {
        graphene_rect_t rect;
        graphene_rect_init(&rect, 0.0f, 0.0f, (gfloat)width, (gfloat)height);
        gtk_snapshot_append_texture(gtk_snapshot, self->texture, &rect);
        return;
    }

    const gint transform = (gint)self->transform;
    const gboolean swap = transform == 1 || transform == 3 || transform == 5 || transform == 7;
    const gfloat pre_width = swap ? (gfloat)height : (gfloat)width;
    const gfloat pre_height = swap ? (gfloat)width : (gfloat)height;

    gtk_snapshot_save(gtk_snapshot);

    graphene_point_t center;
    graphene_point_init(&center, (gfloat)width / 2.0f, (gfloat)height / 2.0f);
    gtk_snapshot_translate(gtk_snapshot, &center);
    if (transform >= 4) {
        gtk_snapshot_scale(gtk_snapshot, -1.0f, 1.0f);
        gtk_snapshot_rotate(gtk_snapshot, (gfloat)((transform - 4) * 90));
    } else if (transform != 0) {
        gtk_snapshot_rotate(gtk_snapshot, (gfloat)(transform * 90));
    }
    graphene_point_init(&center, -pre_width / 2.0f, -pre_height / 2.0f);
    gtk_snapshot_translate(gtk_snapshot, &center);

    draw_source_to_dest(self, gtk_snapshot, self->source, self->dest);

    gtk_snapshot_restore(gtk_snapshot);
}

static gint
intrinsic_width_vfunc(GdkPaintable* paintable)
{
    return (gint)VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE(paintable)->current_width;
}

static gint
intrinsic_height_vfunc(GdkPaintable* paintable)
{
    return (gint)VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE(paintable)->current_height;
}

static gdouble
intrinsic_aspect_vfunc(GdkPaintable* paintable)
{
    VividDisplayConsumerBufferPaintable* self =
        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE(paintable);
    return self->current_height > 0
        ? (gdouble)self->current_width / (gdouble)self->current_height
        : 0.0;
}

static void
vivid_display_consumer_buffer_paintable_iface_init(GdkPaintableInterface* iface)
{
    iface->snapshot = snapshot_vfunc;
    iface->get_intrinsic_width = intrinsic_width_vfunc;
    iface->get_intrinsic_height = intrinsic_height_vfunc;
    iface->get_intrinsic_aspect_ratio = intrinsic_aspect_vfunc;
}

static void
vivid_display_consumer_buffer_paintable_init(
    VividDisplayConsumerBufferPaintable* self)
{
    self->generations = g_ptr_array_new_with_free_func(generation_free);
    self->current_generation = 0;
    self->current_buffer_index = 0;
    self->clear[3] = 1.0f;
}

static void
vivid_display_consumer_buffer_paintable_finalize(GObject* object)
{
    VividDisplayConsumerBufferPaintable* self =
        VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE(object);

    clear_state(self, FALSE);
    g_clear_pointer(&self->generations, g_ptr_array_unref);
    if (self->vk_relay_ready) {
        ww_vk_blitter_shutdown(&self->vk_blitter);
        ww_vk_destroy_owned(&self->vk_owned);
        self->vk_relay_ready = FALSE;
    }

    G_OBJECT_CLASS(vivid_display_consumer_buffer_paintable_parent_class)->finalize(object);
}

static void
vivid_display_consumer_buffer_paintable_class_init(
    VividDisplayConsumerBufferPaintableClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = vivid_display_consumer_buffer_paintable_finalize;
}

VividDisplayConsumerBufferPaintable*
vivid_display_consumer_buffer_paintable_new(void)
{
    return g_object_new(VIVID_DISPLAY_CONSUMER_TYPE_BUFFER_PAINTABLE, NULL);
}

gboolean
vivid_display_consumer_buffer_paintable_bind_json(
    VividDisplayConsumerBufferPaintable* self,
    const gchar*                          bind_json,
    GUnixFDList*                          fd_list,
    GError**                              error)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_BUFFER_PAINTABLE(self), FALSE);
    g_return_val_if_fail(bind_json != NULL, FALSE);
    g_return_val_if_fail(G_IS_UNIX_FD_LIST(fd_list), FALSE);

    JsonParser* parser = json_parser_new();
    if (!json_parser_load_from_data(parser, bind_json, -1, error)) {
        g_prefix_error(error, "invalid BIND_BUFFERS JSON: ");
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                            VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                            "BIND_BUFFERS root is not an object");
        g_object_unref(parser);
        return FALSE;
    }

    const gboolean ok =
        append_generation_from_json(self, json_node_get_object(root), fd_list, error);
    g_object_unref(parser);
    return ok;
}

gboolean
vivid_display_consumer_buffer_paintable_show_frame_with_sync(
    VividDisplayConsumerBufferPaintable* self,
    guint64                               generation_id,
    guint32                               buffer_index,
    gint                                  acquire_sync_fd,
    gint                                  release_syncobj_fd,
    GError**                              error)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_BUFFER_PAINTABLE(self), FALSE);

    VividDmaBufGeneration* generation = find_generation(self, generation_id);
    VividDmaBufBuffer* buffer = find_buffer(generation, buffer_index);
    if (!generation || !buffer) {
        if (generation && release_syncobj_fd >= 0) {
            signal_release_syncobj_fd(generation->render_node,
                                      release_syncobj_fd,
                                      generation_id,
                                      buffer_index,
                                      "shadow-copy-missing-buffer");
            release_syncobj_fd = -1;
        }
        if (acquire_sync_fd >= 0)
            close(acquire_sync_fd);
        if (release_syncobj_fd >= 0)
            close(release_syncobj_fd);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                    "FRAME_READY references unknown generation=%" G_GUINT64_FORMAT " buffer=%u",
                    generation_id,
                    buffer_index);
        return FALSE;
    }
    if (acquire_sync_fd < 0 || release_syncobj_fd < 0) {
        if (acquire_sync_fd >= 0)
            close(acquire_sync_fd);
        if (release_syncobj_fd >= 0) {
            signal_release_syncobj_fd(generation->render_node,
                                      release_syncobj_fd,
                                      generation_id,
                                      buffer_index,
                                      "missing-shadow-sync-fd");
        }
        if (release_syncobj_fd >= 0)
            close(release_syncobj_fd);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_PROTOCOL,
                    "shadow-copy FRAME_READY requires acquire sync_file and release syncobj "
                    "generation=%" G_GUINT64_FORMAT " buffer=%u acquire=%d release=%d",
                    generation_id,
                    buffer_index,
                    acquire_sync_fd,
                    release_syncobj_fd);
        return FALSE;
    }
    if (!buffer->has_vk_image || buffer->vk_image.image == VK_NULL_HANDLE) {
        close(acquire_sync_fd);
        signal_release_syncobj_fd(generation->render_node,
                                  release_syncobj_fd,
                                  generation_id,
                                  buffer_index,
                                  "shadow-copy-missing-import");
        close(release_syncobj_fd);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "Vulkan relay buffer is not imported generation=%" G_GUINT64_FORMAT
                    " buffer=%u",
                    generation_id,
                    buffer_index);
        return FALSE;
    }

    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkResult vr = self->vk_blitter.backend.vkCreateSemaphore(
        self->vk_blitter.backend.device, &sem_info, NULL, &acquire_sem);
    if (vr != VK_SUCCESS) {
        close(acquire_sync_fd);
        signal_release_syncobj_fd(generation->render_node,
                                  release_syncobj_fd,
                                  generation_id,
                                  buffer_index,
                                  "shadow-copy-create-acquire-semaphore-failed");
        close(release_syncobj_fd);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to create Vulkan acquire semaphore: %s",
                    ww_vk_result_str(vr));
        return FALSE;
    }

    int import_rc = ww_vk_import_sync_fd(&self->vk_blitter.backend,
                                         acquire_sem,
                                         acquire_sync_fd);
    if (import_rc != 0) {
        close(acquire_sync_fd);
        signal_release_syncobj_fd(generation->render_node,
                                  release_syncobj_fd,
                                  generation_id,
                                  buffer_index,
                                  "shadow-copy-acquire-import-failed");
        close(release_syncobj_fd);
        self->vk_blitter.backend.vkDestroySemaphore(
            self->vk_blitter.backend.device, acquire_sem, NULL);
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "failed to import acquire sync_file into Vulkan semaphore rc=%d "
                    "generation=%" G_GUINT64_FORMAT " buffer=%u",
                    import_rc,
                    generation_id,
                    buffer_index);
        return FALSE;
    }
    acquire_sync_fd = -1;

    VividReleaseSignalContext release_context = {
        .render_node = generation->render_node,
        .generation = generation_id,
        .buffer_index = buffer_index,
        .context = "shadow-copy-complete",
    };

    ww_vk_blitter_tick_pending_destroys(&self->vk_blitter);
    const int blit_rc = ww_vk_blitter_blit(&self->vk_blitter,
                                           buffer->vk_image.image,
                                           generation->width,
                                           generation->height,
                                           acquire_sem,
                                           release_syncobj_fd,
                                           blitter_signal_release_syncobj,
                                           &release_context);
    release_syncobj_fd = -1;
    self->vk_blitter.backend.vkDestroySemaphore(
        self->vk_blitter.backend.device, acquire_sem, NULL);
    acquire_sem = VK_NULL_HANDLE;
    if (blit_rc != 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR,
                    VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_ERROR_DMABUF,
                    "Vulkan shadow-copy blit failed rc=%d generation=%" G_GUINT64_FORMAT
                    " buffer=%u",
                    blit_rc,
                    generation_id,
                    buffer_index);
        return FALSE;
    }

    g_autoptr(GdkTexture) texture = build_shadow_texture_for_generation(self, generation, error);
    if (!texture)
        return FALSE;

    const gboolean size_changed =
        self->current_width != generation->width ||
        self->current_height != generation->height;

    g_clear_object(&self->texture);
    self->texture = g_steal_pointer(&texture);
    self->current_generation = generation_id;
    self->current_buffer_index = buffer_index;
    self->current_width = generation->width;
    self->current_height = generation->height;

    if (size_changed)
        gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
    return TRUE;
}

void
vivid_display_consumer_buffer_paintable_set_config(
    VividDisplayConsumerBufferPaintable* self,
    gdouble                               source_x,
    gdouble                               source_y,
    gdouble                               source_width,
    gdouble                               source_height,
    gdouble                               dest_x,
    gdouble                               dest_y,
    gdouble                               dest_width,
    gdouble                               dest_height,
    guint                                 transform,
    gdouble                               clear_r,
    gdouble                               clear_g,
    gdouble                               clear_b,
    gdouble                               clear_a)
{
    g_return_if_fail(VIVID_DISPLAY_CONSUMER_IS_BUFFER_PAINTABLE(self));

    self->source[0] = (gfloat)source_x;
    self->source[1] = (gfloat)source_y;
    self->source[2] = (gfloat)source_width;
    self->source[3] = (gfloat)source_height;
    self->dest[0] = (gfloat)dest_x;
    self->dest[1] = (gfloat)dest_y;
    self->dest[2] = (gfloat)dest_width;
    self->dest[3] = (gfloat)dest_height;
    self->transform = transform;
    self->clear[0] = (gfloat)clear_r;
    self->clear[1] = (gfloat)clear_g;
    self->clear[2] = (gfloat)clear_b;
    self->clear[3] = (gfloat)clear_a;
    self->have_config = TRUE;

    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

void
vivid_display_consumer_buffer_paintable_unbind(
    VividDisplayConsumerBufferPaintable* self,
    guint64                               generation)
{
    g_return_if_fail(VIVID_DISPLAY_CONSUMER_IS_BUFFER_PAINTABLE(self));

    /*
     * Match waywallen's renderer behavior: unbinding a generation releases the
     * protocol pool but keeps the last already-built GdkTexture on screen. This
     * avoids white flashes when producer switches content and sends UNBIND after
     * the first frame of the new generation.
     */
    remove_generation(self, generation);
}

void
vivid_display_consumer_buffer_paintable_clear(
    VividDisplayConsumerBufferPaintable* self)
{
    g_return_if_fail(VIVID_DISPLAY_CONSUMER_IS_BUFFER_PAINTABLE(self));

    clear_state(self, TRUE);
}

static void
clear_state(VividDisplayConsumerBufferPaintable* self,
            gboolean                             invalidate)
{
    g_clear_object(&self->texture);
    if (self->generations)
        g_ptr_array_set_size(self->generations, 0);
    self->current_generation = 0;
    self->current_buffer_index = 0;
    self->current_width = 0;
    self->current_height = 0;
    self->have_config = FALSE;
    if (invalidate) {
        gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
        gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
    }
}
