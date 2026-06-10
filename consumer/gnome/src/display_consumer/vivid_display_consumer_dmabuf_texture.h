/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_H
#define VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_H

#include <gdk/gdk.h>

G_BEGIN_DECLS

GdkTexture* vivid_display_consumer_dmabuf_texture_builder_build(
    GdkDmabufTextureBuilder* builder,
    GError**                 error);

char* vivid_display_consumer_dmabuf_texture_get_render_node(GdkDisplay* display);

char* vivid_display_consumer_dmabuf_texture_get_vendor(GdkDisplay* display);

char* vivid_display_consumer_dmabuf_texture_get_pci_address(GdkDisplay* display);

char* vivid_display_consumer_dmabuf_texture_get_device_uuid(GdkDisplay* display);

char* vivid_display_consumer_dmabuf_texture_get_driver_uuid(GdkDisplay* display);

guint vivid_display_consumer_dmabuf_texture_probe_plane_count(GdkDisplay* display,
                                                              guint32     fourcc,
                                                              guint64     modifier);

gboolean vivid_display_consumer_dmabuf_texture_wait_sync_file(gint     sync_fd,
                                                              gint     timeout_msec,
                                                              GError** error);

gboolean vivid_display_consumer_dmabuf_texture_signal_release_syncobj(
    const gchar* render_node,
    gint         syncobj_fd,
    GError**     error);

void vivid_display_consumer_dmabuf_texture_close_fd(gint fd);

G_END_DECLS

#endif
