#ifndef VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_H
#define VIVID_DISPLAY_CONSUMER_FRAME_IMPORTER_H

/*
 * Mutter's bundled Cogl keeps the EGL import entry points behind HAVE_EGL.
 * GNOME Shell itself is built with that define, but extensions compiling
 * against the public headers do not inherit it from pkg-config. Define it
 * before Clutter includes Cogl so the DMA-BUF import declarations are visible.
 */
#ifndef HAVE_EGL
#define HAVE_EGL 1
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <clutter/clutter.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

G_BEGIN_DECLS

#define VIVID_DISPLAY_CONSUMER_TYPE_FRAME_IMPORTER \
    (vivid_display_consumer_frame_importer_get_type())

G_DECLARE_FINAL_TYPE(VividDisplayConsumerFrameImporter,
                     vivid_display_consumer_frame_importer,
                     VIVID_DISPLAY_CONSUMER,
                     FRAME_IMPORTER,
                     GObject)

VividDisplayConsumerFrameImporter*
vivid_display_consumer_frame_importer_new(void);

gboolean vivid_display_consumer_frame_importer_get_available(
    VividDisplayConsumerFrameImporter* self);

const gchar* vivid_display_consumer_frame_importer_get_last_error(
    VividDisplayConsumerFrameImporter* self);

gchar* vivid_display_consumer_frame_importer_describe_capabilities(
    VividDisplayConsumerFrameImporter* self);

ClutterContent* vivid_display_consumer_frame_importer_import_dmabuf_buffer(
    VividDisplayConsumerFrameImporter* self,
    const gchar*                        bind_buffers_json,
    GUnixFDList*                        fd_list,
    guint                               buffer_index,
    GError**                            error);

G_END_DECLS

#endif
