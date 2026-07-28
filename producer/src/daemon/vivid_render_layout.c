#include "vivid_render_layout.h"

static VividRenderSize
normalize_size(VividRenderSize size)
{
    size.width = MAX(size.width, 1u);
    size.height = MAX(size.height, 1u);
    return size;
}

VividFillMode
vivid_render_layout_fillmode_from_content_fit(gint content_fit)
{
    switch (CLAMP(content_fit, 1, 3)) {
    case 1:
        return VIVID_FILL_MODE_PRESERVE_ASPECT_CROP;
    case 2:
        return VIVID_FILL_MODE_PRESERVE_ASPECT_FIT;
    case 3:
        return VIVID_FILL_MODE_STRETCHED;
    default:
        return VIVID_FILL_MODE_PRESERVE_ASPECT_CROP;
    }
}

VividLayoutLocation
vivid_render_layout_location_from_preset(gint preset)
{
    static const VividLayoutLocation anchors[9] = {
        { 0.0, 0.0 },
        { 50.0, 0.0 },
        { 100.0, 0.0 },
        { 0.0, 50.0 },
        { 50.0, 50.0 },
        { 100.0, 50.0 },
        { 0.0, 100.0 },
        { 50.0, 100.0 },
        { 100.0, 100.0 },
    };

    if (preset < 0)
        return (VividLayoutLocation) { 50.0, 50.0 };

    const guint index = (guint)CLAMP(preset, 0, 8);
    return anchors[index];
}

const gchar*
vivid_render_layout_transform_string(guint32 wl_transform)
{
    switch (wl_transform) {
    case 1:
        return "90";
    case 2:
        return "180";
    case 3:
        return "270";
    case 4:
        return "flipped";
    case 5:
        return "flipped-90";
    case 6:
        return "flipped-180";
    case 7:
        return "flipped-270";
    case 0:
    default:
        return "normal";
    }
}

guint32
vivid_render_layout_transform_from_string(const gchar* text)
{
    if (!text || !*text)
        return 0;

    if (g_strcmp0(text, "90") == 0)
        return 1;
    if (g_strcmp0(text, "180") == 0)
        return 2;
    if (g_strcmp0(text, "270") == 0)
        return 3;
    if (g_strcmp0(text, "flipped") == 0)
        return 4;
    if (g_strcmp0(text, "flipped-90") == 0)
        return 5;
    if (g_strcmp0(text, "flipped-180") == 0)
        return 6;
    if (g_strcmp0(text, "flipped-270") == 0)
        return 7;

    return 0;
}

static void
set_full_texture(VividRenderSize tex_size, VividRenderRect* out_src)
{
    out_src->x = 0.0;
    out_src->y = 0.0;
    out_src->width = tex_size.width;
    out_src->height = tex_size.height;
}

static void
anchor_dst_rect(VividLayoutLocation location,
                gdouble             draw_width,
                gdouble             draw_height,
                gdouble             display_width,
                gdouble             display_height,
                VividRenderRect*    out_dst)
{
    const gdouble anchor_x = CLAMP(location.x, 0.0, 100.0) / 100.0;
    const gdouble anchor_y = CLAMP(location.y, 0.0, 100.0) / 100.0;
    out_dst->x = anchor_x * display_width - draw_width * anchor_x;
    out_dst->y = anchor_y * display_height - draw_height * anchor_y;
    out_dst->width = draw_width;
    out_dst->height = draw_height;
}

void
vivid_render_layout_compute(VividFillMode       fillmode,
                            VividLayoutLocation location,
                            VividRenderSize     tex_size,
                            VividRenderSize     display_size,
                            VividRenderRect*    out_src,
                            VividRenderRect*    out_dst)
{
    g_return_if_fail(out_src != NULL && out_dst != NULL);

    tex_size = normalize_size(tex_size);
    display_size = normalize_size(display_size);

    const gdouble tex_w = tex_size.width;
    const gdouble tex_h = tex_size.height;
    const gdouble disp_w = display_size.width;
    const gdouble disp_h = display_size.height;
    const gdouble tex_aspect = tex_w / tex_h;
    const gdouble disp_aspect = disp_w / disp_h;

    switch (fillmode) {
    case VIVID_FILL_MODE_STRETCHED:
        set_full_texture(tex_size, out_src);
        out_dst->x = 0.0;
        out_dst->y = 0.0;
        out_dst->width = disp_w;
        out_dst->height = disp_h;
        return;

    case VIVID_FILL_MODE_PRESERVE_ASPECT_FIT: {
        gdouble draw_w = disp_w;
        gdouble draw_h = disp_h;
        if (disp_aspect > tex_aspect) {
            draw_w = disp_h * tex_aspect;
        } else {
            draw_h = disp_w / tex_aspect;
        }
        set_full_texture(tex_size, out_src);
        anchor_dst_rect(location, draw_w, draw_h, disp_w, disp_h, out_dst);
        return;
    }

    case VIVID_FILL_MODE_CENTERED: {
        gdouble draw_w = MIN(tex_w, disp_w);
        gdouble draw_h = MIN(tex_h, disp_h);
        if (tex_w > disp_w || tex_h > disp_h) {
            if (tex_aspect > disp_aspect) {
                draw_w = disp_w;
                draw_h = disp_w / tex_aspect;
            } else {
                draw_h = disp_h;
                draw_w = disp_h * tex_aspect;
            }
            out_src->x = (tex_w - draw_w) * 0.5;
            out_src->y = (tex_h - draw_h) * 0.5;
            out_src->width = draw_w;
            out_src->height = draw_h;
        } else {
            out_src->x = 0.0;
            out_src->y = 0.0;
            out_src->width = tex_w;
            out_src->height = tex_h;
        }
        anchor_dst_rect(location, draw_w, draw_h, disp_w, disp_h, out_dst);
        return;
    }

    case VIVID_FILL_MODE_PRESERVE_ASPECT_CROP:
    default:
        set_full_texture(tex_size, out_src);
        out_dst->x = 0.0;
        out_dst->y = 0.0;
        out_dst->width = disp_w;
        out_dst->height = disp_h;
        if (tex_aspect > disp_aspect) {
            out_src->width = tex_h * disp_aspect;
            out_src->x = (tex_w - out_src->width) * 0.5;
        } else if (tex_aspect < disp_aspect) {
            out_src->height = tex_w / disp_aspect;
            out_src->y = (tex_h - out_src->height) * 0.5;
        }
        return;
    }
}

static void
inverse_transform_normalized(guint32 transform, gdouble* x, gdouble* y)
{
    gdouble cx = *x - 0.5;
    gdouble cy = *y - 0.5;
    const gboolean flip = transform >= 4;
    const guint rotation = flip ? transform - 4 : transform;

    if (flip)
        cx = -cx;

    gdouble rx = cx;
    gdouble ry = cy;
    switch (rotation % 4) {
    case 0:
        break;
    case 1:
        rx = -cy;
        ry = cx;
        break;
    case 2:
        rx = -cx;
        ry = -cy;
        break;
    case 3:
        rx = cy;
        ry = -cx;
        break;
    default:
        break;
    }

    *x = rx + 0.5;
    *y = ry + 0.5;
}

void
vivid_render_layout_display_point_to_texture(gdouble                display_x,
                                             gdouble                display_y,
                                             guint32                transform,
                                             const VividRenderRect* src,
                                             const VividRenderRect* dst,
                                             gdouble*               out_tex_x,
                                             gdouble*               out_tex_y)
{
    g_return_if_fail(src != NULL && dst != NULL && out_tex_x != NULL && out_tex_y != NULL);

    const gdouble dst_width = MAX(dst->width, 1.0);
    const gdouble dst_height = MAX(dst->height, 1.0);
    gdouble nx = (display_x - dst->x) / dst_width;
    gdouble ny = (display_y - dst->y) / dst_height;

    nx = CLAMP(nx, 0.0, 1.0);
    ny = CLAMP(ny, 0.0, 1.0);
    inverse_transform_normalized(transform, &nx, &ny);

    *out_tex_x = src->x + nx * MAX(src->width, 1.0);
    *out_tex_y = src->y + ny * MAX(src->height, 1.0);
}

void
vivid_render_layout_cover_source(guint32          source_width,
                                   guint32          source_height,
                                   guint32          destination_width,
                                   guint32          destination_height,
                                   VividRenderRect* out_source)
{
    g_return_if_fail(out_source != NULL);

    source_width = MAX(source_width, 1u);
    source_height = MAX(source_height, 1u);
    destination_width = MAX(destination_width, 1u);
    destination_height = MAX(destination_height, 1u);

    const gdouble source_aspect =
        (gdouble)source_width / (gdouble)source_height;
    const gdouble destination_aspect =
        (gdouble)destination_width / (gdouble)destination_height;

    out_source->x = 0.0;
    out_source->y = 0.0;
    out_source->width = source_width;
    out_source->height = source_height;

    if (source_aspect > destination_aspect) {
        out_source->width = (gdouble)source_height * destination_aspect;
        out_source->x = ((gdouble)source_width - out_source->width) * 0.5;
    } else if (source_aspect < destination_aspect) {
        out_source->height = (gdouble)source_width / destination_aspect;
        out_source->y = ((gdouble)source_height - out_source->height) * 0.5;
    }
}

VividRenderSize
vivid_render_layout_clone_route_size(VividRenderSize        primary,
                                     const VividRenderSize* outputs,
                                     guint                  n_outputs,
                                     guint32                max_dimension)
{
    (void)outputs;
    (void)n_outputs;

    max_dimension = MAX(max_dimension, 1u);
    primary.width = CLAMP(primary.width, 1u, max_dimension);
    primary.height = CLAMP(primary.height, 1u, max_dimension);
    return primary;
}
