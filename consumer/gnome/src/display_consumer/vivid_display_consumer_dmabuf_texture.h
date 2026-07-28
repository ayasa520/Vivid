/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_H
#define VIVID_DISPLAY_CONSUMER_DMABUF_TEXTURE_H

#include <glib.h>

G_BEGIN_DECLS

char* vivid_display_consumer_dmabuf_texture_query_vulkan_relay_caps_json(void);

gboolean vivid_display_consumer_dmabuf_texture_signal_release_syncobj(
    const gchar* render_node,
    gint         syncobj_fd,
    GError**     error);

void vivid_display_consumer_dmabuf_texture_close_fd(gint fd);

G_END_DECLS

#endif
