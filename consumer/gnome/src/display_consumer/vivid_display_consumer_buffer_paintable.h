/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_H
#define VIVID_DISPLAY_CONSUMER_BUFFER_PAINTABLE_H

#include <gdk/gdk.h>
#include <gio/gunixfdlist.h>

G_BEGIN_DECLS

#define VIVID_DISPLAY_CONSUMER_TYPE_BUFFER_PAINTABLE \
    (vivid_display_consumer_buffer_paintable_get_type())

G_DECLARE_FINAL_TYPE(VividDisplayConsumerBufferPaintable,
                     vivid_display_consumer_buffer_paintable,
                     VIVID_DISPLAY_CONSUMER,
                     BUFFER_PAINTABLE,
                     GObject)

VividDisplayConsumerBufferPaintable*
vivid_display_consumer_buffer_paintable_new(void);

gboolean vivid_display_consumer_buffer_paintable_bind_json(
    VividDisplayConsumerBufferPaintable* self,
    const gchar*                          bind_json,
    GUnixFDList*                          fd_list,
    GError**                              error);

gboolean vivid_display_consumer_buffer_paintable_show_frame(
    VividDisplayConsumerBufferPaintable* self,
    guint64                               generation,
    guint32                               buffer_index,
    GError**                              error);

gboolean vivid_display_consumer_buffer_paintable_show_frame_with_sync(
    VividDisplayConsumerBufferPaintable* self,
    guint64                               generation,
    guint32                               buffer_index,
    gint                                  acquire_sync_fd,
    gint                                  release_syncobj_fd,
    GError**                              error);

gboolean vivid_display_consumer_buffer_paintable_attach_release_syncobj(
    VividDisplayConsumerBufferPaintable* self,
    guint64                               generation,
    guint32                               buffer_index,
    gint                                  release_syncobj_fd,
    GError**                              error);

void vivid_display_consumer_buffer_paintable_flush_pending_release_syncobj(
    VividDisplayConsumerBufferPaintable* self,
    const gchar*                          reason);

void vivid_display_consumer_buffer_paintable_set_config(
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
    gdouble                               clear_a);

void vivid_display_consumer_buffer_paintable_unbind(
    VividDisplayConsumerBufferPaintable* self,
    guint64                               generation);

void vivid_display_consumer_buffer_paintable_clear(
    VividDisplayConsumerBufferPaintable* self);

G_END_DECLS

#endif
