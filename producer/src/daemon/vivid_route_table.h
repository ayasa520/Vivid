#ifndef VIVID_ROUTE_TABLE_H
#define VIVID_ROUTE_TABLE_H

#include "vivid_render_layout.h"

#include <glib.h>

typedef struct _Output Output;
typedef struct _RenderRoute RenderRoute;

#define VIVID_ROUTE_ORPHAN_REAP_TIMEOUT_MSEC 5000

typedef struct _RouteLink
{
    guint32          link_id;
    RenderRoute*     route;
    Output*          output;
    VividRenderRect  src_rect;
    VividRenderRect  dst_rect;
    guint32          transform;
    gfloat           clear_rgba[4];
    gboolean         enabled;
} RouteLink;

typedef struct
{
    GPtrArray* links;
    guint32    next_link_id;
} VividRouteTable;

void vivid_route_table_init(VividRouteTable* table);
void vivid_route_table_clear(VividRouteTable* table);

void route_link_free(gpointer data);

RouteLink* route_link_for_output(VividRouteTable* table, const Output* output);
guint      route_link_count_for_route(VividRouteTable* table, const RenderRoute* route);
RouteLink* route_link_attach(VividRouteTable* table, Output* output, RenderRoute* route);
void       route_link_detach_output(VividRouteTable* table, Output* output);

gboolean route_link_apply_layout(RouteLink*          link,
                                 Output*             output,
                                 guint32             route_width,
                                 guint32             route_height,
                                 VividFillMode       fillmode,
                                 VividLayoutLocation location,
                                 VividRotation       rotation);

gboolean route_link_apply_clone_layout(RouteLink* link,
                                       Output*    output,
                                       guint32    route_width,
                                       guint32    route_height);

void producer_route_cancel_orphan_reap(RenderRoute* route);

#endif
