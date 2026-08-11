#ifndef VIVID_RENDERER_REGISTRY_H
#define VIVID_RENDERER_REGISTRY_H

#include <glib.h>

#include "vivid_renderer_protocol.h"

G_BEGIN_DECLS

typedef struct _VividRendererDescriptor VividRendererDescriptor;
typedef struct _VividRendererRegistry VividRendererRegistry;

typedef enum
{
    VIVID_RENDERER_REGISTRY_ERROR_IO,
    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
    VIVID_RENDERER_REGISTRY_ERROR_DUPLICATE,
    VIVID_RENDERER_REGISTRY_ERROR_EXECUTABLE,
} VividRendererRegistryError;

#define VIVID_RENDERER_REGISTRY_ERROR vivid_renderer_registry_error_quark()
GQuark vivid_renderer_registry_error_quark(void);

VividRendererRegistry* vivid_renderer_registry_load(const gchar* root,
                                                     GError** error);
void vivid_renderer_registry_free(VividRendererRegistry* registry);

const gchar* vivid_renderer_registry_root(const VividRendererRegistry* registry);
guint vivid_renderer_registry_size(const VividRendererRegistry* registry);
const VividRendererDescriptor* vivid_renderer_registry_at(
    const VividRendererRegistry* registry,
    guint index);
const VividRendererDescriptor* vivid_renderer_registry_lookup_id(
    const VividRendererRegistry* registry,
    const gchar* renderer_id);
const VividRendererDescriptor* vivid_renderer_registry_lookup_wallpaper_type(
    const VividRendererRegistry* registry,
    const gchar* wallpaper_type);

const gchar* vivid_renderer_descriptor_id(const VividRendererDescriptor* descriptor);
VividRendererKind vivid_renderer_descriptor_kind(
    const VividRendererDescriptor* descriptor);
const gchar* vivid_renderer_descriptor_executable(
    const VividRendererDescriptor* descriptor);
const gchar* vivid_renderer_descriptor_preload(
    const VividRendererDescriptor* descriptor);
const gchar* vivid_renderer_descriptor_manifest_path(
    const VividRendererDescriptor* descriptor);
guint32 vivid_renderer_descriptor_spawn_version(
    const VividRendererDescriptor* descriptor);
const gchar* vivid_renderer_descriptor_identity_defaults_json(
    const VividRendererDescriptor* descriptor);
const gchar* vivid_renderer_descriptor_runtime_defaults_json(
    const VividRendererDescriptor* descriptor);
gboolean vivid_renderer_descriptor_has_identity_setting(
    const VividRendererDescriptor* descriptor,
    const gchar* setting);
gboolean vivid_renderer_descriptor_has_runtime_setting(
    const VividRendererDescriptor* descriptor,
    const gchar* setting);
gboolean vivid_renderer_descriptor_supports_event(
    const VividRendererDescriptor* descriptor,
    const gchar* event_name);

G_END_DECLS

#endif
