#ifndef VIVID_RENDERER_MANAGER_H
#define VIVID_RENDERER_MANAGER_H

#include <glib.h>

#include "vivid_renderer_process.h"
#include "vivid_renderer_registry.h"

G_BEGIN_DECLS

typedef struct _VividRendererManager VividRendererManager;

typedef enum
{
    VIVID_RENDERER_MANAGER_ERROR_INVALID,
    VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
    VIVID_RENDERER_MANAGER_ERROR_SPAWN,
} VividRendererManagerError;

#define VIVID_RENDERER_MANAGER_ERROR vivid_renderer_manager_error_quark()
GQuark vivid_renderer_manager_error_quark(void);

VividRendererManager* vivid_renderer_manager_new(
    const VividRendererRegistry* registry,
    GMainContext* context,
    const VividRendererLifecyclePolicy* policy,
    const VividRendererProcessObserver* observer,
    gpointer observer_data,
    GError** error);
void vivid_renderer_manager_free(VividRendererManager* manager);

VividRendererProcess* vivid_renderer_manager_spawn(
    VividRendererManager* manager,
    const VividRendererDescriptor* descriptor,
    const gchar* route_id,
    const gchar* identity_hash,
    GError** error);

void vivid_renderer_manager_collect_reaped(VividRendererManager* manager);

guint vivid_renderer_manager_process_count(const VividRendererManager* manager);
VividRendererProcess* vivid_renderer_manager_process_at(
    const VividRendererManager* manager,
    guint index);

G_END_DECLS

#endif
