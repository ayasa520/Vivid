/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#define _GNU_SOURCE

#include "vivid_display_consumer_frame_importer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cogl/cogl.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR \
    (vivid_display_consumer_frame_importer_error_quark())

typedef enum
{
    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNSUPPORTED_FORMAT,
    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_EGL,
    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_COGL,
} VividDisplayConsumerFrameImporterError;

typedef struct
{
    gint     fd;
    guint32  stride;
    guint32  offset;
} DmaBufPlane;

enum {
    MAX_DMABUF_PLANES = 4,
};

struct _VividDisplayConsumerFrameImporter
{
    GObject parent_instance;

    CoglContext* cogl_context;
    EGLDisplay   egl_display;
    gboolean     checked_backend;
    gboolean     available;
    gboolean     has_modifier_import;
    gchar*       last_error;
};

G_DEFINE_TYPE(VividDisplayConsumerFrameImporter,
              vivid_display_consumer_frame_importer,
              G_TYPE_OBJECT)

static GQuark
vivid_display_consumer_frame_importer_error_quark(void)
{
    return g_quark_from_static_string("vivid_display_consumer_frame_importer-error");
}

static void
set_last_error(VividDisplayConsumerFrameImporter* self,
               const gchar*                        message)
{
    g_free(self->last_error);
    self->last_error = g_strdup(message ? message : "unknown DMA-BUF importer error");
}

static gboolean
extension_list_has_token(const gchar* extensions,
                         const gchar* token)
{
    if (!extensions || !token || !*token)
        return FALSE;

    const gsize token_len = strlen(token);
    const gchar* cursor = extensions;
    while ((cursor = strstr(cursor, token)) != NULL) {
        const gboolean starts_token =
            cursor == extensions || g_ascii_isspace(*(cursor - 1));
        const gboolean ends_token =
            cursor[token_len] == '\0' || g_ascii_isspace(cursor[token_len]);
        if (starts_token && ends_token)
            return TRUE;
        cursor += token_len;
    }

    return FALSE;
}

static gboolean
ensure_backend(VividDisplayConsumerFrameImporter* self,
               GError**                            error)
{
    if (self->checked_backend) {
        if (!self->available) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
                        "%s",
                        self->last_error ? self->last_error : "DMA-BUF importer is unavailable");
        }
        return self->available;
    }

    self->checked_backend = TRUE;
    self->egl_display = EGL_NO_DISPLAY;

    ClutterBackend* backend = clutter_get_default_backend();
    if (!backend) {
        set_last_error(self, "Clutter default backend is unavailable");
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
                            self->last_error);
        return FALSE;
    }

    self->cogl_context = clutter_backend_get_cogl_context(backend);
    if (!self->cogl_context) {
        set_last_error(self, "Clutter backend does not expose a Cogl context");
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
                            self->last_error);
        return FALSE;
    }
    g_object_ref(self->cogl_context);

    self->egl_display = cogl_context_get_egl_display(self->cogl_context);
    if (self->egl_display == EGL_NO_DISPLAY) {
        set_last_error(self, "Cogl context does not expose an EGLDisplay");
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
                            self->last_error);
        return FALSE;
    }

    const gchar* extensions = eglQueryString(self->egl_display, EGL_EXTENSIONS);
    if (!extension_list_has_token(extensions, "EGL_EXT_image_dma_buf_import")) {
        set_last_error(self, "EGL_EXT_image_dma_buf_import is not available in GNOME Shell");
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
                            self->last_error);
        return FALSE;
    }

    self->has_modifier_import =
        extension_list_has_token(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    self->available = TRUE;
    set_last_error(self, NULL);
    return TRUE;
}

static gboolean
json_member_get_uint64(JsonObject*  object,
                       const gchar* member,
                       guint64      fallback,
                       guint64*     out_value)
{
    if (!json_object_has_member(object, member)) {
        *out_value = fallback;
        return TRUE;
    }

    JsonNode* node = json_object_get_member(object, member);
    if (!node)
        return FALSE;

    if (JSON_NODE_HOLDS_VALUE(node)) {
        GType value_type = json_node_get_value_type(node);
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
            if (number < 0.0)
                return FALSE;
            *out_value = (guint64)number;
            return TRUE;
        }
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
json_member_get_bool(JsonObject*  object,
                     const gchar* member,
                     gboolean     fallback)
{
    if (!json_object_has_member(object, member))
        return fallback;
    return json_object_get_boolean_member(object, member);
}

static JsonObject*
parse_bind_buffers_json(const gchar* json,
                        JsonParser** out_parser,
                        GError**     error)
{
    if (!json || !*json) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                            "BIND_BUFFERS JSON is empty");
        return NULL;
    }

    JsonParser* parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, error)) {
        g_prefix_error(error, "invalid BIND_BUFFERS JSON: ");
        g_object_unref(parser);
        return NULL;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                            "BIND_BUFFERS root is not an object");
        g_object_unref(parser);
        return NULL;
    }

    *out_parser = parser;
    return json_node_get_object(root);
}

static gboolean
drm_fourcc_to_cogl_pixel_format(guint32          fourcc,
                                gboolean         premultiplied,
                                CoglPixelFormat* out_format)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
        *out_format = COGL_PIXEL_FORMAT_XRGB_8888;
        return TRUE;
    case DRM_FORMAT_XBGR8888:
        *out_format = COGL_PIXEL_FORMAT_XBGR_8888;
        return TRUE;
    case DRM_FORMAT_RGBX8888:
        *out_format = COGL_PIXEL_FORMAT_RGBX_8888;
        return TRUE;
    case DRM_FORMAT_BGRX8888:
        *out_format = COGL_PIXEL_FORMAT_BGRX_8888;
        return TRUE;
    case DRM_FORMAT_ARGB8888:
        *out_format = premultiplied
            ? COGL_PIXEL_FORMAT_ARGB_8888_PRE
            : COGL_PIXEL_FORMAT_ARGB_8888;
        return TRUE;
    case DRM_FORMAT_ABGR8888:
        *out_format = premultiplied
            ? COGL_PIXEL_FORMAT_ABGR_8888_PRE
            : COGL_PIXEL_FORMAT_ABGR_8888;
        return TRUE;
    case DRM_FORMAT_RGBA8888:
        *out_format = premultiplied
            ? COGL_PIXEL_FORMAT_RGBA_8888_PRE
            : COGL_PIXEL_FORMAT_RGBA_8888;
        return TRUE;
    case DRM_FORMAT_BGRA8888:
        *out_format = premultiplied
            ? COGL_PIXEL_FORMAT_BGRA_8888_PRE
            : COGL_PIXEL_FORMAT_BGRA_8888;
        return TRUE;
    case DRM_FORMAT_XRGB2101010:
        *out_format = COGL_PIXEL_FORMAT_XRGB_2101010;
        return TRUE;
    case DRM_FORMAT_XBGR2101010:
        *out_format = COGL_PIXEL_FORMAT_XBGR_2101010;
        return TRUE;
    case DRM_FORMAT_ARGB2101010:
        *out_format = premultiplied
            ? COGL_PIXEL_FORMAT_ARGB_2101010_PRE
            : COGL_PIXEL_FORMAT_ARGB_2101010;
        return TRUE;
    case DRM_FORMAT_ABGR2101010:
        *out_format = premultiplied
            ? COGL_PIXEL_FORMAT_ABGR_2101010_PRE
            : COGL_PIXEL_FORMAT_ABGR_2101010;
        return TRUE;
    default:
        return FALSE;
    }
}

static JsonObject*
find_buffer_object(JsonArray* buffers,
                   guint     buffer_index)
{
    if (!buffers)
        return NULL;

    const guint count = json_array_get_length(buffers);
    for (guint i = 0; i < count; i++) {
        JsonObject* buffer = json_array_get_object_element(buffers, i);
        if (!buffer)
            continue;

        guint32 index = 0;
        if (!json_member_get_uint32(buffer, "index", i, &index))
            continue;
        if (index == buffer_index)
            return buffer;
    }

    return NULL;
}

static gboolean
append_plane_attributes(EGLint*          attrs,
                        guint*           n_attrs,
                        guint            plane,
                        const DmaBufPlane* plane_info,
                        gboolean         include_modifier,
                        guint64          modifier)
{
    static const EGLint plane_fd_attrs[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static const EGLint plane_offset_attrs[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static const EGLint plane_pitch_attrs[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static const EGLint plane_modifier_lo_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static const EGLint plane_modifier_hi_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    if (plane >= G_N_ELEMENTS(plane_fd_attrs))
        return FALSE;

    attrs[(*n_attrs)++] = plane_fd_attrs[plane];
    attrs[(*n_attrs)++] = plane_info->fd;
    attrs[(*n_attrs)++] = plane_offset_attrs[plane];
    attrs[(*n_attrs)++] = (EGLint)plane_info->offset;
    attrs[(*n_attrs)++] = plane_pitch_attrs[plane];
    attrs[(*n_attrs)++] = (EGLint)plane_info->stride;

    if (include_modifier) {
        attrs[(*n_attrs)++] = plane_modifier_lo_attrs[plane];
        attrs[(*n_attrs)++] = (EGLint)(modifier & 0xffffffffu);
        attrs[(*n_attrs)++] = plane_modifier_hi_attrs[plane];
        attrs[(*n_attrs)++] = (EGLint)(modifier >> 32);
    }

    return TRUE;
}

static void
close_planes(DmaBufPlane* planes,
             guint        n_planes)
{
    for (guint i = 0; i < n_planes; i++) {
        if (planes[i].fd >= 0)
            close(planes[i].fd);
        planes[i].fd = -1;
    }
}

static gboolean
read_planes(JsonObject*    buffer,
            GUnixFDList*   fd_list,
            DmaBufPlane    planes[static MAX_DMABUF_PLANES],
            guint*         out_n_planes,
            GError**       error)
{
    JsonNode* planes_node = json_object_get_member(buffer, "planes");
    if (!planes_node || !JSON_NODE_HOLDS_ARRAY(planes_node)) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                            "BIND_BUFFERS buffer has no planes array");
        return FALSE;
    }

    JsonArray* plane_array = json_node_get_array(planes_node);
    const guint n_planes = json_array_get_length(plane_array);
    if (n_planes == 0 || n_planes > MAX_DMABUF_PLANES) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                    "unsupported DMA-BUF plane count %u",
                    n_planes);
        return FALSE;
    }

    for (guint i = 0; i < n_planes; i++) {
        planes[i].fd = -1;

        JsonObject* plane = json_array_get_object_element(plane_array, i);
        if (!plane) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                        "plane %u is not an object",
                        i);
            close_planes(planes, i);
            return FALSE;
        }

        guint32 fd_index = 0;
        guint32 stride = 0;
        guint32 offset = 0;
        if (!json_member_get_uint32(plane, "fdIndex", i, &fd_index) ||
            !json_member_get_uint32(plane, "stride", 0, &stride) ||
            !json_member_get_uint32(plane, "offset", 0, &offset) ||
            stride == 0) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                        "plane %u has invalid fdIndex/stride/offset",
                        i);
            close_planes(planes, i);
            return FALSE;
        }

        GError* fd_error = NULL;
        const gint fd = g_unix_fd_list_get(fd_list, (gint)fd_index, &fd_error);
        if (fd < 0) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                        "failed to duplicate fdIndex=%u for plane %u: %s",
                        fd_index,
                        i,
                        fd_error ? fd_error->message : "unknown fd error");
            g_clear_error(&fd_error);
            close_planes(planes, i);
            return FALSE;
        }

        planes[i].fd = fd;
        planes[i].stride = stride;
        planes[i].offset = offset;
    }

    *out_n_planes = n_planes;
    return TRUE;
}

static EGLImageKHR
create_dmabuf_egl_image(VividDisplayConsumerFrameImporter* self,
                        guint32                             width,
                        guint32                             height,
                        guint32                             fourcc,
                        guint64                             modifier,
                        gboolean                            include_modifier,
                        const DmaBufPlane*                  planes,
                        guint                               n_planes,
                        GError**                            error)
{
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    if (!create_image) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_EGL,
                            "eglCreateImageKHR is unavailable");
        return EGL_NO_IMAGE_KHR;
    }

    /*
     * The socket frame gives us fd indexes and per-plane metadata; EGL is the
     * compositor-side import boundary. The fd duplicates are only needed for
     * eglCreateImageKHR. After a successful import, the EGLImage/GL texture
     * keeps its own kernel references and these duplicates can be closed.
     */
    EGLint attrs[6 + MAX_DMABUF_PLANES * 10 + 1];
    guint n_attrs = 0;
    attrs[n_attrs++] = EGL_WIDTH;
    attrs[n_attrs++] = (EGLint)width;
    attrs[n_attrs++] = EGL_HEIGHT;
    attrs[n_attrs++] = (EGLint)height;
    attrs[n_attrs++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n_attrs++] = (EGLint)fourcc;

    for (guint i = 0; i < n_planes; i++) {
        if (!append_plane_attributes(attrs,
                                     &n_attrs,
                                     i,
                                     &planes[i],
                                     include_modifier,
                                     modifier)) {
            g_set_error(error,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                        "too many DMA-BUF planes: %u",
                        n_planes);
            return EGL_NO_IMAGE_KHR;
        }
    }
    attrs[n_attrs++] = EGL_NONE;

    EGLImageKHR image = create_image(self->egl_display,
                                     EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT,
                                     NULL,
                                     attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_EGL,
                    "eglCreateImageKHR failed for fourcc=0x%08x modifier=0x%016" G_GINT64_MODIFIER "x egl_error=0x%x",
                    fourcc,
                    (guint64)modifier,
                    eglGetError());
    }

    return image;
}

static void
destroy_dmabuf_egl_image(VividDisplayConsumerFrameImporter* self,
                         EGLImageKHR                         image)
{
    if (image == EGL_NO_IMAGE_KHR)
        return;

    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    if (destroy_image)
        destroy_image(self->egl_display, image);
}

static void
vivid_display_consumer_frame_importer_dispose(GObject* object)
{
    VividDisplayConsumerFrameImporter* self =
        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER(object);

    g_clear_object(&self->cogl_context);

    G_OBJECT_CLASS(vivid_display_consumer_frame_importer_parent_class)->dispose(object);
}

static void
vivid_display_consumer_frame_importer_finalize(GObject* object)
{
    VividDisplayConsumerFrameImporter* self =
        VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER(object);

    g_clear_pointer(&self->last_error, g_free);

    G_OBJECT_CLASS(vivid_display_consumer_frame_importer_parent_class)->finalize(object);
}

static void
vivid_display_consumer_frame_importer_class_init(
    VividDisplayConsumerFrameImporterClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = vivid_display_consumer_frame_importer_dispose;
    object_class->finalize = vivid_display_consumer_frame_importer_finalize;
}

static void
vivid_display_consumer_frame_importer_init(VividDisplayConsumerFrameImporter* self)
{
    self->egl_display = EGL_NO_DISPLAY;
}

VividDisplayConsumerFrameImporter*
vivid_display_consumer_frame_importer_new(void)
{
    return g_object_new(VIVID_DISPLAY_CONSUMER_TYPE_FRAME_IMPORTER, NULL);
}

gboolean
vivid_display_consumer_frame_importer_get_available(
    VividDisplayConsumerFrameImporter* self)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_FRAME_IMPORTER(self), FALSE);

    GError* error = NULL;
    const gboolean available = ensure_backend(self, &error);
    if (error) {
        set_last_error(self, error->message);
        g_clear_error(&error);
    }

    return available;
}

const gchar*
vivid_display_consumer_frame_importer_get_last_error(
    VividDisplayConsumerFrameImporter* self)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_FRAME_IMPORTER(self), NULL);

    return self->last_error ? self->last_error : "";
}

gchar*
vivid_display_consumer_frame_importer_describe_capabilities(
    VividDisplayConsumerFrameImporter* self)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_FRAME_IMPORTER(self), NULL);

    GError* error = NULL;
    const gboolean available = ensure_backend(self, &error);
    if (error) {
        set_last_error(self, error->message);
        g_clear_error(&error);
    }

    return g_strdup_printf(
        "{\"available\":%s,"
        "\"memoryTypes\":[\"dmabuf\"],"
        "\"renderer\":\"gnome-shell-clutter-cogl-egl\","
        "\"modifiers\":%s,"
        "\"fourcc\":["
        "\"XRGB8888\",\"XBGR8888\",\"RGBX8888\",\"BGRX8888\","
        "\"ARGB8888\",\"ABGR8888\",\"RGBA8888\",\"BGRA8888\","
        "\"XRGB2101010\",\"XBGR2101010\",\"ARGB2101010\",\"ABGR2101010\"]}",
        available ? "true" : "false",
        self->has_modifier_import ? "true" : "false");
}

ClutterContent*
vivid_display_consumer_frame_importer_import_dmabuf_buffer(
    VividDisplayConsumerFrameImporter* self,
    const gchar*                        bind_buffers_json,
    GUnixFDList*                        fd_list,
    guint                               buffer_index,
    GError**                            error)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_FRAME_IMPORTER(self), NULL);
    g_return_val_if_fail(G_IS_UNIX_FD_LIST(fd_list), NULL);

    if (!ensure_backend(self, error))
        return NULL;

    JsonParser* parser = NULL;
    JsonObject* root = parse_bind_buffers_json(bind_buffers_json, &parser, error);
    if (!root)
        return NULL;

    const gchar* memory_type = json_object_get_string_member_with_default(
        root,
        "memoryType",
        "");
    if (g_strcmp0(memory_type, "dmabuf") != 0) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                    "memoryType=%s cannot be displayed by the DMA-BUF importer",
                    memory_type && *memory_type ? memory_type : "(missing)");
        g_object_unref(parser);
        return NULL;
    }

    guint32 width = 0;
    guint32 height = 0;
    guint32 fourcc = 0;
    guint64 modifier = DRM_FORMAT_MOD_INVALID;
    if (!json_member_get_uint32(root, "width", 0, &width) ||
        !json_member_get_uint32(root, "height", 0, &height) ||
        !json_member_get_uint32(root, "fourcc", 0, &fourcc) ||
        !json_member_get_uint64(root, "modifier", DRM_FORMAT_MOD_INVALID, &modifier) ||
        width == 0 || height == 0 || fourcc == 0) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                            "BIND_BUFFERS has invalid width/height/fourcc/modifier");
        g_object_unref(parser);
        return NULL;
    }

    const gboolean premultiplied = json_member_get_bool(root, "premultiplied", TRUE);
    CoglPixelFormat cogl_format = COGL_PIXEL_FORMAT_ANY;
    if (!drm_fourcc_to_cogl_pixel_format(fourcc, premultiplied, &cogl_format)) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNSUPPORTED_FORMAT,
                    "unsupported DRM fourcc=0x%08x",
                    fourcc);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode* buffers_node = json_object_get_member(root, "buffers");
    JsonArray* buffers = buffers_node && JSON_NODE_HOLDS_ARRAY(buffers_node)
        ? json_node_get_array(buffers_node)
        : NULL;
    JsonObject* buffer = find_buffer_object(buffers, buffer_index);
    if (!buffer) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_PROTOCOL,
                    "BIND_BUFFERS has no buffer index %u",
                    buffer_index);
        g_object_unref(parser);
        return NULL;
    }

    DmaBufPlane planes[MAX_DMABUF_PLANES] = {
        {.fd = -1},
        {.fd = -1},
        {.fd = -1},
        {.fd = -1},
    };
    guint n_planes = 0;
    if (!read_planes(buffer, fd_list, planes, &n_planes, error)) {
        g_object_unref(parser);
        return NULL;
    }

    const gboolean modifier_is_implicit =
        modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR;
    /*
     * Keep LINEAR/INVALID on the implicit-modifier EGL import path even when
     * EGL_EXT_image_dma_buf_import_modifiers is present. NVIDIA can expose a
     * modifier-aware LINEAR import that only binds as an external texture; Cogl
     * expects a normal 2D texture. Explicit modifier attributes are reserved for
     * non-linear layouts where the driver needs per-plane modifier metadata.
     */
    const gboolean include_modifier = !modifier_is_implicit;
    if (!self->has_modifier_import && !modifier_is_implicit) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_UNAVAILABLE,
                    "modifier=0x%016" G_GINT64_MODIFIER "x requires EGL_EXT_image_dma_buf_import_modifiers",
                    (guint64)modifier);
        close_planes(planes, n_planes);
        g_object_unref(parser);
        return NULL;
    }

    EGLImageKHR image = create_dmabuf_egl_image(self,
                                                width,
                                                height,
                                                fourcc,
                                                modifier,
                                                include_modifier,
                                                planes,
                                                n_planes,
                                                error);
    close_planes(planes, n_planes);
    if (image == EGL_NO_IMAGE_KHR) {
        g_object_unref(parser);
        return NULL;
    }

    GError* cogl_error = NULL;
    CoglTexture* texture = cogl_texture_2d_new_from_egl_image(
        self->cogl_context,
        (int)width,
        (int)height,
        cogl_format,
        image,
        COGL_EGL_IMAGE_FLAG_NO_GET_DATA,
        &cogl_error);
    destroy_dmabuf_egl_image(self, image);

    if (!texture) {
        g_set_error(error,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                    VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_COGL,
                    "cogl_texture_2d_new_from_egl_image failed: %s",
                    cogl_error ? cogl_error->message : "unknown Cogl error");
        g_clear_error(&cogl_error);
        g_object_unref(parser);
        return NULL;
    }

    ClutterContent* content = clutter_texture_content_new_from_texture(texture, NULL);
    g_object_unref(texture);
    g_object_unref(parser);

    if (!content) {
        g_set_error_literal(error,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR,
                            VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_ERROR_COGL,
                            "clutter_texture_content_new_from_texture returned NULL");
        return NULL;
    }

    return content;
}
