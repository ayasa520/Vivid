#include "vivid_renderer_registry.h"

#include <json-glib/json-glib.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define VIVID_RENDERER_MANIFEST_SUFFIX ".renderer.json"
#define VIVID_RENDERER_MANIFEST_VERSION 1u

struct _VividRendererDescriptor
{
    gchar* id;
    VividRendererKind kind;
    gchar* executable;
    gchar* preload;
    gchar* manifest_path;
    gchar* identity_defaults_json;
    gchar* runtime_defaults_json;
    guint32 spawn_version;
    GPtrArray* wallpaper_types;
    GPtrArray* identity_settings;
    GPtrArray* runtime_settings;
    GPtrArray* events;
};

struct _VividRendererRegistry
{
    gchar* root;
    GPtrArray* descriptors;
    GHashTable* by_id;
    GHashTable* by_wallpaper_type;
};

G_DEFINE_QUARK(vivid-renderer-registry-error, vivid_renderer_registry_error)

static void
vivid_renderer_descriptor_free(VividRendererDescriptor* descriptor)
{
    if (!descriptor)
        return;
    g_free(descriptor->id);
    g_free(descriptor->executable);
    g_free(descriptor->preload);
    g_free(descriptor->manifest_path);
    g_free(descriptor->identity_defaults_json);
    g_free(descriptor->runtime_defaults_json);
    g_clear_pointer(&descriptor->wallpaper_types, g_ptr_array_unref);
    g_clear_pointer(&descriptor->identity_settings, g_ptr_array_unref);
    g_clear_pointer(&descriptor->runtime_settings, g_ptr_array_unref);
    g_clear_pointer(&descriptor->events, g_ptr_array_unref);
    g_free(descriptor);
}

static gboolean
name_is_stable(const gchar* value, gboolean allow_dot)
{
    if (!value ||
        (!g_ascii_islower(value[0]) && !g_ascii_isdigit(value[0]))) {
        return FALSE;
    }
    for (const gchar* cursor = value; *cursor; cursor++) {
        if (g_ascii_islower(*cursor) || g_ascii_isdigit(*cursor) || *cursor == '-' ||
            (allow_dot && *cursor == '.')) {
            continue;
        }
        return FALSE;
    }
    return TRUE;
}

static gboolean
json_required_string(JsonObject* object,
                     const gchar* member,
                     const gchar* manifest_path,
                     const gchar** out_value,
                     GError** error)
{
    if (!json_object_has_member(object, member)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s is missing string member '%s'",
                    manifest_path,
                    member);
        return FALSE;
    }
    JsonNode* node = json_object_get_member(object, member);
    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member '%s' must be a string",
                    manifest_path,
                    member);
        return FALSE;
    }
    const gchar* value = json_node_get_string(node);
    if (!value || !*value) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member '%s' must not be empty",
                    manifest_path,
                    member);
        return FALSE;
    }
    *out_value = value;
    return TRUE;
}

static gboolean
json_required_uint32(JsonObject* object,
                     const gchar* member,
                     const gchar* manifest_path,
                     guint32* out_value,
                     GError** error)
{
    if (!json_object_has_member(object, member)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s is missing integer member '%s'",
                    manifest_path,
                    member);
        return FALSE;
    }
    JsonNode* node = json_object_get_member(object, member);
    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member '%s' must be an integer",
                    manifest_path,
                    member);
        return FALSE;
    }
    const gint64 value = json_node_get_int(node);
    if (value < 0 || (guint64)value > G_MAXUINT32) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member '%s' is outside uint32 range",
                    manifest_path,
                    member);
        return FALSE;
    }
    *out_value = (guint32)value;
    return TRUE;
}

static GPtrArray*
json_required_name_array(JsonObject* object,
                         const gchar* member,
                         const gchar* manifest_path,
                         gboolean require_nonempty,
                         GError** error)
{
    if (!json_object_has_member(object, member)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s is missing array member '%s'",
                    manifest_path,
                    member);
        return NULL;
    }
    JsonNode* node = json_object_get_member(object, member);
    if (!JSON_NODE_HOLDS_ARRAY(node)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member '%s' must be an array",
                    manifest_path,
                    member);
        return NULL;
    }

    JsonArray* array = json_node_get_array(node);
    if (require_nonempty && json_array_get_length(array) == 0) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member '%s' must not be empty",
                    manifest_path,
                    member);
        return NULL;
    }

    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray* result = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonNode* element = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_VALUE(element) ||
            json_node_get_value_type(element) != G_TYPE_STRING) {
            g_set_error(error,
                        VIVID_RENDERER_REGISTRY_ERROR,
                        VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                        "renderer manifest %s member '%s' element %u must be a string",
                        manifest_path,
                        member,
                        i);
            g_ptr_array_unref(result);
            return NULL;
        }
        const gchar* value = json_node_get_string(element);
        if (!name_is_stable(value, FALSE)) {
            g_set_error(error,
                        VIVID_RENDERER_REGISTRY_ERROR,
                        VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                        "renderer manifest %s member '%s' has invalid stable name '%s'",
                        manifest_path,
                        member,
                        value ? value : "(null)");
            g_ptr_array_unref(result);
            return NULL;
        }
        if (g_hash_table_contains(seen, value)) {
            g_set_error(error,
                        VIVID_RENDERER_REGISTRY_ERROR,
                        VIVID_RENDERER_REGISTRY_ERROR_DUPLICATE,
                        "renderer manifest %s member '%s' repeats '%s'",
                        manifest_path,
                        member,
                        value);
            g_ptr_array_unref(result);
            return NULL;
        }
        gchar* owned = g_strdup(value);
        g_hash_table_add(seen, owned);
        g_ptr_array_add(result, owned);
    }
    return result;
}

static gboolean
ptr_array_contains(const GPtrArray* array, const gchar* value)
{
    if (!array || !value)
        return FALSE;
    for (guint i = 0; i < array->len; i++) {
        if (g_str_equal(g_ptr_array_index((GPtrArray*)array, i), value))
            return TRUE;
    }
    return FALSE;
}

static gboolean
parse_setting_defaults(JsonObject* manifest,
                       const gchar* manifest_path,
                       const GPtrArray* identity_settings,
                       const GPtrArray* runtime_settings,
                       gchar** out_identity_json,
                       gchar** out_runtime_json,
                       GError** error)
{
    if (!json_object_has_member(manifest, "setting-defaults")) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s is missing object member 'setting-defaults'",
                    manifest_path);
        return FALSE;
    }
    JsonNode* node = json_object_get_member(manifest, "setting-defaults");
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s member 'setting-defaults' must be an object",
                    manifest_path);
        return FALSE;
    }

    JsonObject* defaults = json_node_get_object(node);
    g_autoptr(JsonBuilder) identity_builder = json_builder_new();
    g_autoptr(JsonBuilder) runtime_builder = json_builder_new();
    json_builder_begin_object(identity_builder);
    json_builder_begin_object(runtime_builder);

    /*
     * Defaults are split at registry-load time so the orchestration layer never
     * needs renderer-specific branches. Identity defaults participate in the
     * spawn hash and INIT only; runtime defaults can be resent to a live worker.
     */
    g_autoptr(GList) members = json_object_get_members(defaults);
    for (const GList* item = members; item; item = item->next) {
        const gchar* setting = item->data;
        const gboolean is_identity =
            ptr_array_contains(identity_settings, setting);
        const gboolean is_runtime = ptr_array_contains(runtime_settings, setting);
        if (!name_is_stable(setting, FALSE) || (!is_identity && !is_runtime)) {
            g_set_error(error,
                        VIVID_RENDERER_REGISTRY_ERROR,
                        VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                        "renderer manifest %s default '%s' is not a declared identity or runtime setting",
                        manifest_path,
                        setting ? setting : "(null)");
            return FALSE;
        }
        JsonBuilder* builder = is_identity ? identity_builder : runtime_builder;
        json_builder_set_member_name(builder, setting);
        json_builder_add_value(
            builder,
            json_node_copy(json_object_get_member(defaults, setting)));
    }
    json_builder_end_object(identity_builder);
    json_builder_end_object(runtime_builder);

    JsonNode* identity_root = json_builder_get_root(identity_builder);
    JsonNode* runtime_root = json_builder_get_root(runtime_builder);
    *out_identity_json = json_to_string(identity_root, FALSE);
    *out_runtime_json = json_to_string(runtime_root, FALSE);
    json_node_free(identity_root);
    json_node_free(runtime_root);
    return TRUE;
}

static gboolean
parse_renderer_kind(const gchar* value, VividRendererKind* out_kind)
{
    if (g_strcmp0(value, "scene") == 0)
        *out_kind = VIVID_RENDERER_KIND_SCENE;
    else if (g_strcmp0(value, "web") == 0)
        *out_kind = VIVID_RENDERER_KIND_WEB;
    else if (g_strcmp0(value, "video") == 0)
        *out_kind = VIVID_RENDERER_KIND_VIDEO;
    else
        return FALSE;
    return TRUE;
}

static gboolean
path_is_below_root(const gchar* root, const gchar* path)
{
    if (!root || !path)
        return FALSE;
    if (g_str_equal(root, path))
        return FALSE;
    g_autofree gchar* prefix = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);
    return g_str_has_prefix(path, prefix);
}

static gchar*
resolve_executable(const gchar* registry_root,
                   const gchar* relative_path,
                   const gchar* manifest_path,
                   GError** error)
{
    if (g_path_is_absolute(relative_path)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_EXECUTABLE,
                    "renderer manifest %s executable must be relative to registry root: %s",
                    manifest_path,
                    relative_path);
        return NULL;
    }

    g_autofree gchar* joined = g_build_filename(registry_root, relative_path, NULL);
    errno = 0;
    gchar* resolved = realpath(joined, NULL);
    if (!resolved) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_EXECUTABLE,
                    "renderer manifest %s executable cannot be resolved: %s: %s",
                    manifest_path,
                    joined,
                    g_strerror(errno));
        return NULL;
    }

    /*
     * realpath resolves every symlink before this containment check. A manifest
     * therefore cannot smuggle an executable outside the single configured
     * installation root with '..' components or an in-root symlink.
     */
    if (!path_is_below_root(registry_root, resolved) ||
        !g_file_test(resolved, G_FILE_TEST_IS_REGULAR) ||
        !g_file_test(resolved, G_FILE_TEST_IS_EXECUTABLE)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_EXECUTABLE,
                    "renderer manifest %s executable is not an executable file below registry root: %s",
                    manifest_path,
                    resolved);
        free(resolved);
        return NULL;
    }
    return resolved;
}

static gchar*
resolve_preload(const gchar* registry_root,
                const gchar* relative_path,
                const gchar* manifest_path,
                GError** error)
{
    if (!relative_path)
        return NULL;
    if (g_path_is_absolute(relative_path)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_EXECUTABLE,
                    "renderer manifest %s preload must be relative to registry root: %s",
                    manifest_path,
                    relative_path);
        return NULL;
    }
    g_autofree gchar* joined =
        g_build_filename(registry_root, relative_path, NULL);
    errno = 0;
    gchar* resolved = realpath(joined, NULL);
    if (!resolved || !path_is_below_root(registry_root, resolved) ||
        !g_file_test(resolved, G_FILE_TEST_IS_REGULAR)) {
        const gint saved_errno = errno;
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_EXECUTABLE,
                    "renderer manifest %s preload is not a regular file below registry root: %s: %s",
                    manifest_path,
                    joined,
                    g_strerror(saved_errno != 0 ? saved_errno : EINVAL));
        free(resolved);
        return NULL;
    }
    return resolved;
}

static VividRendererDescriptor*
load_descriptor(const gchar* registry_root,
                const gchar* manifest_path,
                GError** error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, manifest_path, error))
        return NULL;
    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s root must be an object",
                    manifest_path);
        return NULL;
    }

    JsonObject* object = json_node_get_object(root);
    guint32 manifest_version = 0;
    guint32 spawn_version = 0;
    const gchar* id = NULL;
    const gchar* kind_name = NULL;
    const gchar* executable = NULL;
    const gchar* preload = NULL;
    if (!json_required_uint32(object,
                              "manifest-version",
                              manifest_path,
                              &manifest_version,
                              error) ||
        !json_required_uint32(object,
                              "spawn-version",
                              manifest_path,
                              &spawn_version,
                              error) ||
        !json_required_string(object, "id", manifest_path, &id, error) ||
        !json_required_string(object, "kind", manifest_path, &kind_name, error) ||
        !json_required_string(object,
                              "executable",
                              manifest_path,
                              &executable,
                              error)) {
        return NULL;
    }
    if (json_object_has_member(object, "preload") &&
        !json_required_string(object,
                              "preload",
                              manifest_path,
                              &preload,
                              error)) {
        return NULL;
    }
    if (manifest_version != VIVID_RENDERER_MANIFEST_VERSION) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s version %u is unsupported; expected %u",
                    manifest_path,
                    manifest_version,
                    VIVID_RENDERER_MANIFEST_VERSION);
        return NULL;
    }
    if (spawn_version != VIVID_RENDERER_SPAWN_VERSION) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s spawn version %u does not match protocol spawn version %u",
                    manifest_path,
                    spawn_version,
                    VIVID_RENDERER_SPAWN_VERSION);
        return NULL;
    }
    if (!name_is_stable(id, TRUE)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s has invalid stable id '%s'",
                    manifest_path,
                    id);
        return NULL;
    }

    VividRendererKind kind;
    if (!parse_renderer_kind(kind_name, &kind)) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer manifest %s has unknown renderer kind '%s'",
                    manifest_path,
                    kind_name);
        return NULL;
    }

    g_autoptr(GPtrArray) wallpaper_types =
        json_required_name_array(object,
                                 "wallpaper-types",
                                 manifest_path,
                                 TRUE,
                                 error);
    if (!wallpaper_types)
        return NULL;
    g_autoptr(GPtrArray) identity_settings =
        json_required_name_array(object,
                                 "identity-settings",
                                 manifest_path,
                                 FALSE,
                                 error);
    if (!identity_settings)
        return NULL;
    g_autoptr(GPtrArray) runtime_settings =
        json_required_name_array(object,
                                 "runtime-settings",
                                 manifest_path,
                                 FALSE,
                                 error);
    if (!runtime_settings)
        return NULL;
    g_autoptr(GPtrArray) events =
        json_required_name_array(object, "events", manifest_path, FALSE, error);
    if (!events)
        return NULL;

    for (guint i = 0; i < identity_settings->len; i++) {
        const gchar* setting = g_ptr_array_index(identity_settings, i);
        if (ptr_array_contains(runtime_settings, setting)) {
            g_set_error(error,
                        VIVID_RENDERER_REGISTRY_ERROR,
                        VIVID_RENDERER_REGISTRY_ERROR_DUPLICATE,
                        "renderer manifest %s setting '%s' cannot be both identity and runtime",
                        manifest_path,
                        setting);
            return NULL;
        }
    }

    g_autofree gchar* identity_defaults_json = NULL;
    g_autofree gchar* runtime_defaults_json = NULL;
    if (!parse_setting_defaults(object,
                                manifest_path,
                                identity_settings,
                                runtime_settings,
                                &identity_defaults_json,
                                &runtime_defaults_json,
                                error)) {
        return NULL;
    }

    g_autofree gchar* resolved_executable =
        resolve_executable(registry_root, executable, manifest_path, error);
    if (!resolved_executable)
        return NULL;
    g_autofree gchar* resolved_preload =
        resolve_preload(registry_root, preload, manifest_path, error);
    if (preload && !resolved_preload)
        return NULL;

    VividRendererDescriptor* descriptor = g_new0(VividRendererDescriptor, 1);
    descriptor->id = g_strdup(id);
    descriptor->kind = kind;
    descriptor->executable = g_steal_pointer(&resolved_executable);
    descriptor->preload = g_steal_pointer(&resolved_preload);
    descriptor->manifest_path = g_strdup(manifest_path);
    descriptor->identity_defaults_json =
        g_steal_pointer(&identity_defaults_json);
    descriptor->runtime_defaults_json = g_steal_pointer(&runtime_defaults_json);
    descriptor->spawn_version = spawn_version;
    descriptor->wallpaper_types = g_steal_pointer(&wallpaper_types);
    descriptor->identity_settings = g_steal_pointer(&identity_settings);
    descriptor->runtime_settings = g_steal_pointer(&runtime_settings);
    descriptor->events = g_steal_pointer(&events);
    return descriptor;
}

static gint
compare_string_pointers(gconstpointer left, gconstpointer right)
{
    return g_strcmp0(*(gchar* const*)left, *(gchar* const*)right);
}

VividRendererRegistry*
vivid_renderer_registry_load(const gchar* root, GError** error)
{
    if (!root || !*root) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_IO,
                    "renderer registry root is required");
        return NULL;
    }

    errno = 0;
    g_autofree gchar* resolved_root = realpath(root, NULL);
    if (!resolved_root || !g_file_test(resolved_root, G_FILE_TEST_IS_DIR)) {
        const gchar* detail = resolved_root
            ? "resolved path is not a directory"
            : g_strerror(errno);
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_IO,
                    "renderer registry root cannot be resolved as a directory: %s: %s",
                    root,
                    detail);
        return NULL;
    }

    g_autoptr(GDir) directory = g_dir_open(resolved_root, 0, error);
    if (!directory)
        return NULL;
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    const gchar* entry = NULL;
    while ((entry = g_dir_read_name(directory)) != NULL) {
        if (g_str_has_suffix(entry, VIVID_RENDERER_MANIFEST_SUFFIX))
            g_ptr_array_add(names, g_strdup(entry));
    }
    if (names->len == 0) {
        g_set_error(error,
                    VIVID_RENDERER_REGISTRY_ERROR,
                    VIVID_RENDERER_REGISTRY_ERROR_SCHEMA,
                    "renderer registry root contains no %s manifests: %s",
                    VIVID_RENDERER_MANIFEST_SUFFIX,
                    resolved_root);
        return NULL;
    }
    g_ptr_array_sort(names, compare_string_pointers);

    VividRendererRegistry* registry = g_new0(VividRendererRegistry, 1);
    registry->root = g_steal_pointer(&resolved_root);
    registry->descriptors =
        g_ptr_array_new_with_free_func((GDestroyNotify)vivid_renderer_descriptor_free);
    registry->by_id = g_hash_table_new(g_str_hash, g_str_equal);
    registry->by_wallpaper_type = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < names->len; i++) {
        const gchar* name = g_ptr_array_index(names, i);
        g_autofree gchar* manifest_path =
            g_build_filename(registry->root, name, NULL);
        VividRendererDescriptor* descriptor =
            load_descriptor(registry->root, manifest_path, error);
        if (!descriptor)
            goto fail;
        if (g_hash_table_contains(registry->by_id, descriptor->id)) {
            g_set_error(error,
                        VIVID_RENDERER_REGISTRY_ERROR,
                        VIVID_RENDERER_REGISTRY_ERROR_DUPLICATE,
                        "renderer registry repeats renderer id '%s' in %s",
                        descriptor->id,
                        descriptor->manifest_path);
            vivid_renderer_descriptor_free(descriptor);
            goto fail;
        }
        for (guint type_i = 0; type_i < descriptor->wallpaper_types->len; type_i++) {
            const gchar* wallpaper_type =
                g_ptr_array_index(descriptor->wallpaper_types, type_i);
            if (g_hash_table_contains(registry->by_wallpaper_type,
                                      wallpaper_type)) {
                const VividRendererDescriptor* existing =
                    g_hash_table_lookup(registry->by_wallpaper_type,
                                        wallpaper_type);
                g_set_error(error,
                            VIVID_RENDERER_REGISTRY_ERROR,
                            VIVID_RENDERER_REGISTRY_ERROR_DUPLICATE,
                            "renderer type '%s' is declared by both '%s' and '%s'",
                            wallpaper_type,
                            existing->id,
                            descriptor->id);
                vivid_renderer_descriptor_free(descriptor);
                goto fail;
            }
        }

        g_ptr_array_add(registry->descriptors, descriptor);
        g_hash_table_insert(registry->by_id, descriptor->id, descriptor);
        for (guint type_i = 0; type_i < descriptor->wallpaper_types->len; type_i++) {
            const gchar* wallpaper_type =
                g_ptr_array_index(descriptor->wallpaper_types, type_i);
            g_hash_table_insert(registry->by_wallpaper_type,
                                (gpointer)wallpaper_type,
                                descriptor);
        }
    }

    return registry;

fail:
    vivid_renderer_registry_free(registry);
    return NULL;
}

void
vivid_renderer_registry_free(VividRendererRegistry* registry)
{
    if (!registry)
        return;
    g_clear_pointer(&registry->by_wallpaper_type, g_hash_table_unref);
    g_clear_pointer(&registry->by_id, g_hash_table_unref);
    g_clear_pointer(&registry->descriptors, g_ptr_array_unref);
    g_free(registry->root);
    g_free(registry);
}

const gchar*
vivid_renderer_registry_root(const VividRendererRegistry* registry)
{
    return registry ? registry->root : NULL;
}

guint
vivid_renderer_registry_size(const VividRendererRegistry* registry)
{
    return registry && registry->descriptors ? registry->descriptors->len : 0;
}

const VividRendererDescriptor*
vivid_renderer_registry_at(const VividRendererRegistry* registry, guint index)
{
    if (!registry || !registry->descriptors || index >= registry->descriptors->len)
        return NULL;
    return g_ptr_array_index(registry->descriptors, index);
}

const VividRendererDescriptor*
vivid_renderer_registry_lookup_id(const VividRendererRegistry* registry,
                                  const gchar* renderer_id)
{
    return registry && renderer_id
        ? g_hash_table_lookup(registry->by_id, renderer_id)
        : NULL;
}

const VividRendererDescriptor*
vivid_renderer_registry_lookup_wallpaper_type(
    const VividRendererRegistry* registry,
    const gchar* wallpaper_type)
{
    return registry && wallpaper_type
        ? g_hash_table_lookup(registry->by_wallpaper_type, wallpaper_type)
        : NULL;
}

const gchar*
vivid_renderer_descriptor_id(const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->id : NULL;
}

VividRendererKind
vivid_renderer_descriptor_kind(const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->kind : 0;
}

const gchar*
vivid_renderer_descriptor_executable(const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->executable : NULL;
}

const gchar*
vivid_renderer_descriptor_preload(const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->preload : NULL;
}

const gchar*
vivid_renderer_descriptor_manifest_path(const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->manifest_path : NULL;
}

guint32
vivid_renderer_descriptor_spawn_version(const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->spawn_version : 0;
}

const gchar*
vivid_renderer_descriptor_identity_defaults_json(
    const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->identity_defaults_json : NULL;
}

const gchar*
vivid_renderer_descriptor_runtime_defaults_json(
    const VividRendererDescriptor* descriptor)
{
    return descriptor ? descriptor->runtime_defaults_json : NULL;
}

gboolean
vivid_renderer_descriptor_has_identity_setting(
    const VividRendererDescriptor* descriptor,
    const gchar* setting)
{
    return descriptor && ptr_array_contains(descriptor->identity_settings, setting);
}

gboolean
vivid_renderer_descriptor_has_runtime_setting(
    const VividRendererDescriptor* descriptor,
    const gchar* setting)
{
    return descriptor && ptr_array_contains(descriptor->runtime_settings, setting);
}

gboolean
vivid_renderer_descriptor_supports_event(
    const VividRendererDescriptor* descriptor,
    const gchar* event_name)
{
    return descriptor && ptr_array_contains(descriptor->events, event_name);
}
