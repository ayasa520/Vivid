#include "vivid_route_table.h"

#include <math.h>

#define OUTPUT_MAX_DIMENSION 8192u

typedef struct
{
    gpointer     opaque_client;
    RenderRoute* route;
} OutputRouteSlot;

static void
output_set_route(Output* output, RenderRoute* route)
{
    if (!output)
        return;

    ((OutputRouteSlot*)output)->route = route;
}

void
vivid_route_table_init(VividRouteTable* table)
{
    if (!table)
        return;

    table->links = g_ptr_array_new_with_free_func(route_link_free);
    table->next_link_id = 0;
}

void
vivid_route_table_clear(VividRouteTable* table)
{
    if (!table)
        return;

    g_clear_pointer(&table->links, g_ptr_array_unref);
    table->next_link_id = 0;
}

void
route_link_free(gpointer data)
{
    RouteLink* link = data;
    if (!link)
        return;
    g_free(link);
}

RouteLink*
route_link_for_output(VividRouteTable* table, const Output* output)
{
    if (!table || !output || !table->links)
        return NULL;

    for (guint i = 0; i < table->links->len; i++) {
        RouteLink* link = g_ptr_array_index(table->links, i);
        if (link && link->output == output)
            return link;
    }

    return NULL;
}

guint
route_link_count_for_route(VividRouteTable* table, const RenderRoute* route)
{
    if (!table || !route || !table->links)
        return 0;

    guint count = 0;
    for (guint i = 0; i < table->links->len; i++) {
        RouteLink* link = g_ptr_array_index(table->links, i);
        if (link && link->route == route && link->enabled)
            count++;
    }

    return count;
}

void
route_link_detach_output(VividRouteTable* table, Output* output)
{
    if (!table || !output || !table->links)
        return;

    for (gint i = (gint)table->links->len - 1; i >= 0; i--) {
        RouteLink* link = g_ptr_array_index(table->links, (guint)i);
        if (!link || link->output != output)
            continue;
        g_ptr_array_remove_index(table->links, (guint)i);
        output_set_route(output, NULL);
        return;
    }

    output_set_route(output, NULL);
}

RouteLink*
route_link_attach(VividRouteTable* table, Output* output, RenderRoute* route)
{
    if (!table || !output || !route)
        return NULL;

    RouteLink* existing = route_link_for_output(table, output);
    if (existing && existing->route == route) {
        output_set_route(output, route);
        producer_route_cancel_orphan_reap(route);
        return existing;
    }

    if (!table->links)
        vivid_route_table_init(table);

    route_link_detach_output(table, output);

    RouteLink* link = g_new0(RouteLink, 1);
    link->link_id = ++table->next_link_id;
    link->route = route;
    link->output = output;
    link->enabled = TRUE;
    link->clear_rgba[0] = 0.0f;
    link->clear_rgba[1] = 0.0f;
    link->clear_rgba[2] = 0.0f;
    link->clear_rgba[3] = 1.0f;
    g_ptr_array_add(table->links, link);
    output_set_route(output, route);
    producer_route_cancel_orphan_reap(route);
    return link;
}
