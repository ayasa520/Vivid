#ifndef VIVID_RENDER_LAYOUT_H
#define VIVID_RENDER_LAYOUT_H

#include <glib.h>

typedef struct
{
    gdouble x;
    gdouble y;
    gdouble width;
    gdouble height;
} VividRenderRect;

typedef struct
{
    guint32 width;
    guint32 height;
} VividRenderSize;

typedef enum
{
    VIVID_FILL_MODE_STRETCHED = 0,
    VIVID_FILL_MODE_PRESERVE_ASPECT_FIT = 1,
    VIVID_FILL_MODE_PRESERVE_ASPECT_CROP = 2,
    VIVID_FILL_MODE_CENTERED = 3,
} VividFillMode;

typedef struct
{
    gdouble x;
    gdouble y;
} VividLayoutLocation;

typedef enum
{
    VIVID_ROTATION_NORMAL = 0,
    VIVID_ROTATION_CW90 = 1,
    VIVID_ROTATION_CW180 = 2,
    VIVID_ROTATION_CW270 = 3,
} VividRotation;

VividFillMode vivid_render_layout_fillmode_from_content_fit(gint content_fit);
VividLayoutLocation vivid_render_layout_location_from_preset(gint preset);
const gchar* vivid_render_layout_transform_string(guint32 wl_transform);
guint32      vivid_render_layout_transform_from_string(const gchar* text);

void vivid_render_layout_cover_source(guint32          source_width,
                                      guint32          source_height,
                                      guint32          destination_width,
                                      guint32          destination_height,
                                      VividRenderRect* out_source);

void vivid_render_layout_compute(VividFillMode          fillmode,
                                 VividLayoutLocation    location,
                                 VividRenderSize        tex_size,
                                 VividRenderSize        display_size,
                                 VividRenderRect*       out_src,
                                 VividRenderRect*       out_dst);

void vivid_render_layout_display_point_to_texture(gdouble           display_x,
                                                  gdouble           display_y,
                                                  guint32           transform,
                                                  const VividRenderRect* src,
                                                  const VividRenderRect* dst,
                                                  gdouble*          out_tex_x,
                                                  gdouble*          out_tex_y);

VividRenderSize vivid_render_layout_clone_route_size(VividRenderSize        primary,
                                                     const VividRenderSize* outputs,
                                                     guint                  n_outputs,
                                                     guint32                max_dimension);

#endif
