#include "vivid_producer_config.h"

#include "../protocol/vivid_display_protocol.h"

#include <errno.h>
#include <string.h>

typedef struct
{
    JsonNode* value;
} WallpaperUserProperties;

static gchar*
default_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(),
                            "vivid-producer",
                            "config-v1.json",
                            NULL);
}

static WallpaperUserProperties*
wallpaper_user_properties_new(JsonNode* value)
{
    WallpaperUserProperties* properties = g_new0(WallpaperUserProperties, 1);
    properties->value = value ? json_node_copy(value) : json_node_new(JSON_NODE_OBJECT);
    return properties;
}

static void
wallpaper_user_properties_free(WallpaperUserProperties* properties)
{
    if (!properties)
        return;
    if (properties->value)
        json_node_unref(properties->value);
    g_free(properties);
}

static const gchar*
json_get_string(JsonObject* object, const gchar* member, const gchar* fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    if (json_object_get_null_member(object, member))
        return fallback;
    return json_object_get_string_member(object, member);
}

static gboolean
json_get_boolean(JsonObject* object, const gchar* member, gboolean fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    return json_object_get_boolean_member(object, member);
}

static gint
json_get_int_clamped(JsonObject* object,
                     const gchar* member,
                     gint         fallback,
                     gint         min_value,
                     gint         max_value)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;

    JsonNode* node = json_object_get_member(object, member);
    if (!node || json_node_get_node_type(node) != JSON_NODE_VALUE)
        return fallback;

    /*
     * WebUI select controls may send numeric schema keys as JSON strings such
     * as {"content-fit":"1"}.  json_object_get_int_member() reports that as a
     * type mismatch, so parse stringified integers explicitly at the socket
     * boundary and clamp them through the same schema range as numeric values.
     */
    const GType value_type = json_node_get_value_type(node);
    if (value_type == G_TYPE_INT64)
        return CLAMP((gint)json_node_get_int(node), min_value, max_value);
    if (value_type == G_TYPE_DOUBLE)
        return CLAMP((gint)json_node_get_double(node), min_value, max_value);
    if (value_type == G_TYPE_STRING) {
        const gchar* text = json_node_get_string(node);
        if (!text)
            return fallback;

        gchar* end = NULL;
        const gint64 parsed = g_ascii_strtoll(text, &end, 10);
        if (end == text)
            return fallback;
        while (end && g_ascii_isspace(*end))
            end++;
        if (end && *end != '\0')
            return fallback;
        return CLAMP((gint)parsed, min_value, max_value);
    }

    return fallback;
}

static void
replace_string(gchar** target, const gchar* value)
{
    g_free(*target);
    *target = g_strdup(value ? value : "");
}

static void
ensure_config_runtime_containers(VividProducerConfig* config)
{
    if (!config->stop_on_applications)
        config->stop_on_applications = g_ptr_array_new_with_free_func(g_free);
    if (!config->wallpaper_user_properties)
        config->wallpaper_user_properties =
            g_hash_table_new_full(g_str_hash,
                                  g_str_equal,
                                  g_free,
                                  (GDestroyNotify)wallpaper_user_properties_free);
}

void
vivid_producer_config_reset_defaults(VividProducerConfig* config)
{
    g_return_if_fail(config != NULL);

    /*
     * Reset is intentionally owned by the producer config layer instead of the
     * WebUI. This keeps one authoritative default set and lets a single control
     * request clear both global schema keys and every per-wallpaper override
     * that is persisted under the "wallpapers" table.
     */
    ensure_config_runtime_containers(config);

    replace_string(&config->project_path, "");
    replace_string(&config->user_properties, "");
    config->mute = FALSE;
    config->volume = 50;
    config->change_wallpaper = FALSE;
    replace_string(&config->change_wallpaper_directory_path, "");
    config->change_wallpaper_interval = 15;
    config->change_wallpaper_mode = 1;
    config->pause_on_maximize_or_fullscreen = 0;
    config->pause_on_focus = FALSE;
    config->pause_on_battery = 0;
    config->low_battery_threshold = 25;
    config->pause_on_mpris_playing = FALSE;
    g_ptr_array_set_size(config->stop_on_applications, 0);
    config->debug_mode = FALSE;
    replace_string(&config->render_device, "auto");
    config->content_fit = 1;
    config->scene_fps = 30;
    config->startup_delay = 1000;
    config->show_panel_menu = TRUE;
    replace_string(&config->project_browser_filter_state, "");
    replace_string(&config->project_browser_sort_key, "name");
    g_hash_table_remove_all(config->wallpaper_user_properties);
}

static void
load_string_member(JsonObject* object, const gchar* member, gchar** target)
{
    if (!object || !json_object_has_member(object, member) ||
        json_object_get_null_member(object, member))
        return;

    /*
     * Do not pass the existing *target as a fallback to replace_string().
     * replace_string() frees *target before duplicating the new value, so using
     * the old pointer as the source would turn omitted JSON fields into
     * use-after-free data in the persisted producer config.
     */
    replace_string(target, json_object_get_string_member(object, member));
}

static void
load_string_array_member(JsonObject* object, const gchar* member, GPtrArray* target)
{
    if (!object || !json_object_has_member(object, member))
        return;

    JsonArray* array = json_object_get_array_member(object, member);
    if (!array)
        return;

    g_ptr_array_set_size(target, 0);
    const guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        const gchar* value = json_array_get_string_element(array, i);
        if (!value || !*value)
            continue;

        g_autofree gchar* normalized = g_utf8_strdown(value, -1);
        g_strstrip(normalized);
        if (*normalized)
            g_ptr_array_add(target, g_strdup(normalized));
    }
}

static void
load_wallpaper_configs(VividProducerConfig* config, JsonObject* root)
{
    if (!json_object_has_member(root, "wallpapers"))
        return;

    JsonObject* wallpapers = json_object_get_object_member(root, "wallpapers");
    if (!wallpapers)
        return;

    g_hash_table_remove_all(config->wallpaper_user_properties);

    GList* members = json_object_get_members(wallpapers);
    for (GList* item = members; item != NULL; item = item->next) {
        const gchar* wallpaper_id = item->data;
        JsonObject* wallpaper = json_object_get_object_member(wallpapers, wallpaper_id);
        if (!wallpaper || !json_object_has_member(wallpaper, "user-properties"))
            continue;

        JsonNode* value = json_object_get_member(wallpaper, "user-properties");
        if (!value)
            continue;

        g_hash_table_replace(config->wallpaper_user_properties,
                             g_strdup(wallpaper_id),
                             wallpaper_user_properties_new(value));
    }
    g_list_free(members);
}

static gchar*
json_node_to_compact_string(JsonNode* node)
{
    JsonGenerator* generator = json_generator_new();
    json_generator_set_root(generator, node);
    json_generator_set_pretty(generator, FALSE);
    gchar* text = json_generator_to_data(generator, NULL);
    g_object_unref(generator);
    return text;
}

static JsonNode*
parse_user_properties_node(JsonObject* payload)
{
    if (!payload)
        return NULL;

    if (json_object_has_member(payload, "properties"))
        return json_node_copy(json_object_get_member(payload, "properties"));

    if (json_object_has_member(payload, "user-properties"))
        return json_node_copy(json_object_get_member(payload, "user-properties"));

    if (json_object_has_member(payload, "userProperties"))
        return json_node_copy(json_object_get_member(payload, "userProperties"));

    return NULL;
}

static void
sync_active_wallpaper_user_properties(VividProducerConfig* config)
{
    g_autofree gchar* active_id = vivid_producer_config_active_wallpaper_id(config);
    WallpaperUserProperties* properties =
        g_hash_table_lookup(config->wallpaper_user_properties, active_id);

    /*
     * The renderer consumes config->user_properties as the active wallpaper's
     * ready-to-apply payload, while config->wallpaper_user_properties is the
     * durable per-wallpaper store. Keep those two responsibilities separate:
     * project switches copy the selected wallpaper payload into the active slot,
     * and missing per-wallpaper config deliberately clears the active payload.
     * This prevents a plain SET_PROJECT request from leaking the previous
     * wallpaper's user-property JSON into the newly selected project.
     */
    if (properties && properties->value) {
        g_autofree gchar* text = json_node_to_compact_string(properties->value);
        replace_string(&config->user_properties, text);
    } else {
        replace_string(&config->user_properties, "{}");
    }
}

static gboolean
payload_has_string_member(JsonObject* payload, const gchar* member)
{
    return payload &&
        json_object_has_member(payload, member) &&
        !json_object_get_null_member(payload, member);
}

void
vivid_producer_config_init(VividProducerConfig* config, const gchar* config_path)
{
    g_return_if_fail(config != NULL);

    memset(config, 0, sizeof(*config));
    config->config_path = config_path && *config_path ? g_strdup(config_path) : default_config_path();

    /*
     * These defaults mirror src/schemas/io.github.jeffshee.vivid-extension.gschema.xml.
     * The producer now owns this state, but preserving the existing key names
     * keeps migration and WebUI/controller payloads predictable.
     */
    vivid_producer_config_reset_defaults(config);
}

void
vivid_producer_config_clear(VividProducerConfig* config)
{
    if (!config)
        return;

    g_free(config->config_path);
    g_free(config->project_path);
    g_free(config->user_properties);
    g_free(config->change_wallpaper_directory_path);
    g_free(config->render_device);
    g_free(config->project_browser_filter_state);
    g_free(config->project_browser_sort_key);
    g_clear_pointer(&config->stop_on_applications, g_ptr_array_unref);
    g_clear_pointer(&config->wallpaper_user_properties, g_hash_table_unref);
    memset(config, 0, sizeof(*config));
}

gboolean
vivid_producer_config_load(VividProducerConfig* config)
{
    g_return_val_if_fail(config != NULL, FALSE);

    if (!g_file_test(config->config_path, G_FILE_TEST_EXISTS))
        return TRUE;

    g_autoptr(JsonParser) parser = json_parser_new();
    GError* error = NULL;
    if (!json_parser_load_from_file(parser, config->config_path, &error)) {
        g_warning("VividProducer: config load failed at %s: %s",
                  config->config_path,
                  error->message);
        g_clear_error(&error);
        return FALSE;
    }

    JsonNode* root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
        g_warning("VividProducer: config root is not an object at %s", config->config_path);
        return FALSE;
    }

    JsonObject* root = json_node_get_object(root_node);
    JsonObject* global = json_object_has_member(root, "global")
        ? json_object_get_object_member(root, "global")
        : root;

    load_string_member(global, "project-path", &config->project_path);
    load_string_member(global, "user-properties", &config->user_properties);
    config->mute = json_get_boolean(global, "mute", config->mute);
    config->volume = json_get_int_clamped(global, "volume", config->volume, 0, 100);
    config->change_wallpaper = json_get_boolean(global, "change-wallpaper", config->change_wallpaper);
    load_string_member(global, "change-wallpaper-directory-path", &config->change_wallpaper_directory_path);
    config->change_wallpaper_interval =
        json_get_int_clamped(global, "change-wallpaper-interval", config->change_wallpaper_interval, 1, 1440);
    config->change_wallpaper_mode =
        json_get_int_clamped(global, "change-wallpaper-mode", config->change_wallpaper_mode, 0, 3);
    config->pause_on_maximize_or_fullscreen =
        json_get_int_clamped(global, "pause-on-maximize-or-fullscreen", config->pause_on_maximize_or_fullscreen, 0, 2);
    config->pause_on_focus = json_get_boolean(global, "pause-on-focus", config->pause_on_focus);
    config->pause_on_battery =
        json_get_int_clamped(global, "pause-on-battery", config->pause_on_battery, 0, 2);
    config->low_battery_threshold =
        json_get_int_clamped(global, "low-battery-threshold", config->low_battery_threshold, 0, 100);
    config->pause_on_mpris_playing =
        json_get_boolean(global, "pause-on-mpris-playing", config->pause_on_mpris_playing);
    load_string_array_member(global, "stop-on-applications", config->stop_on_applications);
    config->debug_mode = json_get_boolean(global, "debug-mode", config->debug_mode);
    /*
     * render-device is the only supported GPU selection key. Configs that only
     * carry the previous policy key intentionally fall back to the auto default
     * because the old nvidia/va names have no meaningful render-node mapping.
     */
    load_string_member(global, "render-device", &config->render_device);
    config->content_fit = json_get_int_clamped(global, "content-fit", config->content_fit, 1, 3);
    config->scene_fps = json_get_int_clamped(global, "scene-fps", config->scene_fps, 5, 240);
    config->startup_delay = json_get_int_clamped(global, "startup-delay", config->startup_delay, 0, 10000);
    config->show_panel_menu = json_get_boolean(global, "show-panel-menu", config->show_panel_menu);
    load_string_member(global, "project-browser-filter-state", &config->project_browser_filter_state);
    load_string_member(global, "project-browser-sort-key", &config->project_browser_sort_key);
    load_wallpaper_configs(config, root);

    return TRUE;
}

static void
builder_add_config_global(JsonBuilder* builder, const VividProducerConfig* config)
{
    json_builder_set_member_name(builder, "global");
    json_builder_begin_object(builder);

#define ADD_STRING(member_name, value) \
    G_STMT_START { \
        json_builder_set_member_name(builder, member_name); \
        json_builder_add_string_value(builder, value ? value : ""); \
    } G_STMT_END
#define ADD_BOOL(member_name, value) \
    G_STMT_START { \
        json_builder_set_member_name(builder, member_name); \
        json_builder_add_boolean_value(builder, value); \
    } G_STMT_END
#define ADD_INT(member_name, value) \
    G_STMT_START { \
        json_builder_set_member_name(builder, member_name); \
        json_builder_add_int_value(builder, value); \
    } G_STMT_END

    ADD_STRING("project-path", config->project_path);
    ADD_STRING("user-properties", config->user_properties);
    ADD_BOOL("mute", config->mute);
    ADD_INT("volume", config->volume);
    ADD_BOOL("change-wallpaper", config->change_wallpaper);
    ADD_STRING("change-wallpaper-directory-path", config->change_wallpaper_directory_path);
    ADD_INT("change-wallpaper-interval", config->change_wallpaper_interval);
    ADD_INT("change-wallpaper-mode", config->change_wallpaper_mode);
    ADD_INT("pause-on-maximize-or-fullscreen", config->pause_on_maximize_or_fullscreen);
    ADD_BOOL("pause-on-focus", config->pause_on_focus);
    ADD_INT("pause-on-battery", config->pause_on_battery);
    ADD_INT("low-battery-threshold", config->low_battery_threshold);
    ADD_BOOL("pause-on-mpris-playing", config->pause_on_mpris_playing);

    json_builder_set_member_name(builder, "stop-on-applications");
    json_builder_begin_array(builder);
    for (guint i = 0; i < config->stop_on_applications->len; i++)
        json_builder_add_string_value(builder, g_ptr_array_index(config->stop_on_applications, i));
    json_builder_end_array(builder);

    ADD_BOOL("debug-mode", config->debug_mode);
    ADD_STRING("render-device", config->render_device);
    ADD_INT("content-fit", config->content_fit);
    ADD_INT("scene-fps", config->scene_fps);
    ADD_INT("startup-delay", config->startup_delay);
    ADD_BOOL("show-panel-menu", config->show_panel_menu);
    ADD_STRING("project-browser-filter-state", config->project_browser_filter_state);
    ADD_STRING("project-browser-sort-key", config->project_browser_sort_key);

#undef ADD_STRING
#undef ADD_BOOL
#undef ADD_INT

    json_builder_end_object(builder);
}

static void
builder_add_wallpapers(JsonBuilder* builder, const VividProducerConfig* config)
{
    json_builder_set_member_name(builder, "wallpapers");
    json_builder_begin_object(builder);

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, config->wallpaper_user_properties);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        WallpaperUserProperties* properties = value;

        json_builder_set_member_name(builder, key);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "user-properties");
        json_builder_add_value(builder, json_node_copy(properties->value));
        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);
}

gboolean
vivid_producer_config_save(const VividProducerConfig* config)
{
    g_return_val_if_fail(config != NULL, FALSE);

    g_autofree gchar* parent = g_path_get_dirname(config->config_path);
    if (g_mkdir_with_parents(parent, 0700) < 0) {
        g_warning("VividProducer: failed to create config dir %s: %s",
                  parent,
                  g_strerror(errno));
        return FALSE;
    }

    JsonBuilder* builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schemaVersion");
    json_builder_add_int_value(builder, 1);
    builder_add_config_global(builder, config);
    builder_add_wallpapers(builder, config);
    json_builder_end_object(builder);

    JsonNode* root = json_builder_get_root(builder);
    JsonGenerator* generator = json_generator_new();
    json_generator_set_pretty(generator, TRUE);
    json_generator_set_root(generator, root);

    GError* error = NULL;
    const gboolean ok = json_generator_to_file(generator, config->config_path, &error);
    if (!ok) {
        g_warning("VividProducer: config save failed at %s: %s",
                  config->config_path,
                  error->message);
        g_clear_error(&error);
    }

    json_node_unref(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return ok;
}

static void
apply_stop_on_applications(VividProducerConfig* config, JsonObject* payload)
{
    if (!json_object_has_member(payload, "stop-on-applications") &&
        !json_object_has_member(payload, "stopOnApplications"))
        return;

    const gchar* key = json_object_has_member(payload, "stop-on-applications")
        ? "stop-on-applications"
        : "stopOnApplications";
    load_string_array_member(payload, key, config->stop_on_applications);
}

gboolean
vivid_producer_config_apply_control(VividProducerConfig* config,
                                     guint16               control_opcode,
                                     JsonObject*           payload)
{
    g_return_val_if_fail(config != NULL, FALSE);

    if (!payload)
        return FALSE;

    switch (control_opcode) {
    case VIVID_DISPLAY_CONTROL_SET_PROJECT: {
        const gchar* project_path =
            json_get_string(payload,
                            "projectPath",
                            json_get_string(payload,
                                            "project-path",
                                            json_get_string(payload, "path", NULL)));
        if (!project_path)
            return FALSE;

        replace_string(&config->project_path, project_path);
        sync_active_wallpaper_user_properties(config);
        return TRUE;
    }

    case VIVID_DISPLAY_CONTROL_SET_MUTED:
        config->mute = json_get_boolean(payload, "mute", json_get_boolean(payload, "muted", config->mute));
        return TRUE;

    case VIVID_DISPLAY_CONTROL_SET_VOLUME:
        config->volume = json_get_int_clamped(payload, "volume", config->volume, 0, 100);
        return TRUE;

    case VIVID_DISPLAY_CONTROL_SET_CONTENT_FIT:
        config->content_fit =
            json_get_int_clamped(payload, "contentFit", config->content_fit, 1, 3);
        config->content_fit =
            json_get_int_clamped(payload, "content-fit", config->content_fit, 1, 3);
        return TRUE;

    case VIVID_DISPLAY_CONTROL_SET_SCENE_FPS:
        config->scene_fps =
            json_get_int_clamped(payload, "sceneFps", config->scene_fps, 5, 240);
        config->scene_fps =
            json_get_int_clamped(payload, "scene-fps", config->scene_fps, 5, 240);
        return TRUE;

    case VIVID_DISPLAY_CONTROL_SET_USER_PROPERTIES: {
        g_autofree gchar* active_id = vivid_producer_config_active_wallpaper_id(config);
        const gchar* wallpaper_id =
            json_get_string(payload,
                            "wallpaperId",
                            json_get_string(payload, "wallpaper-id", active_id));
        JsonNode* node = parse_user_properties_node(payload);
        if (!node)
            return FALSE;

        g_autofree gchar* text = json_node_to_compact_string(node);
        const gchar* effective_wallpaper_id =
            wallpaper_id && *wallpaper_id ? wallpaper_id : active_id;
        if (g_strcmp0(effective_wallpaper_id, active_id) == 0)
            replace_string(&config->user_properties, text);
        g_hash_table_replace(config->wallpaper_user_properties,
                             g_strdup(effective_wallpaper_id),
                             wallpaper_user_properties_new(node));
        json_node_unref(node);
        return TRUE;
    }

    case VIVID_DISPLAY_CONTROL_SET_STATE:
        /*
         * WebUI/controllers can batch schema-shaped values through SET_STATE in
         * one control frame. The producer still owns validation and persistence;
         * controllers only send schema-shaped patches over the socket.
         */
    {
        if (json_get_boolean(payload,
                             "reset-defaults",
                             json_get_boolean(payload, "resetDefaults", FALSE))) {
            vivid_producer_config_reset_defaults(config);
            return TRUE;
        }

        const gboolean project_path_changed =
            payload_has_string_member(payload, "project-path") &&
            g_strcmp0(json_object_get_string_member(payload, "project-path"),
                      config->project_path) != 0;
        const gboolean user_properties_explicit =
            json_object_has_member(payload, "user-properties") ||
            json_object_has_member(payload, "userProperties");

        load_string_member(payload, "project-path", &config->project_path);
        load_string_member(payload, "user-properties", &config->user_properties);
        config->mute = json_get_boolean(payload, "mute", config->mute);
        config->volume = json_get_int_clamped(payload, "volume", config->volume, 0, 100);
        config->change_wallpaper = json_get_boolean(payload, "change-wallpaper", config->change_wallpaper);
        load_string_member(payload, "change-wallpaper-directory-path", &config->change_wallpaper_directory_path);
        config->change_wallpaper_interval =
            json_get_int_clamped(payload, "change-wallpaper-interval", config->change_wallpaper_interval, 1, 1440);
        config->change_wallpaper_mode =
            json_get_int_clamped(payload, "change-wallpaper-mode", config->change_wallpaper_mode, 0, 3);
        config->pause_on_maximize_or_fullscreen =
            json_get_int_clamped(payload, "pause-on-maximize-or-fullscreen", config->pause_on_maximize_or_fullscreen, 0, 2);
        config->pause_on_focus = json_get_boolean(payload, "pause-on-focus", config->pause_on_focus);
        config->pause_on_battery =
            json_get_int_clamped(payload, "pause-on-battery", config->pause_on_battery, 0, 2);
        config->low_battery_threshold =
            json_get_int_clamped(payload, "low-battery-threshold", config->low_battery_threshold, 0, 100);
        config->pause_on_mpris_playing =
            json_get_boolean(payload, "pause-on-mpris-playing", config->pause_on_mpris_playing);
        apply_stop_on_applications(config, payload);
        config->debug_mode = json_get_boolean(payload, "debug-mode", config->debug_mode);
        load_string_member(payload, "render-device", &config->render_device);
        config->content_fit = json_get_int_clamped(payload, "content-fit", config->content_fit, 1, 3);
        config->scene_fps = json_get_int_clamped(payload, "scene-fps", config->scene_fps, 5, 240);
        config->startup_delay = json_get_int_clamped(payload, "startup-delay", config->startup_delay, 0, 10000);
        config->show_panel_menu = json_get_boolean(payload, "show-panel-menu", config->show_panel_menu);
        load_string_member(payload, "project-browser-filter-state", &config->project_browser_filter_state);
        load_string_member(payload, "project-browser-sort-key", &config->project_browser_sort_key);
        if (project_path_changed && !user_properties_explicit)
            sync_active_wallpaper_user_properties(config);
        return TRUE;
    }

    default:
        return FALSE;
    }
}

gchar*
vivid_producer_config_to_json(const VividProducerConfig* config)
{
    JsonBuilder* builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "configPath");
    json_builder_add_string_value(builder, config->config_path);
    builder_add_config_global(builder, config);
    builder_add_wallpapers(builder, config);
    json_builder_end_object(builder);

    JsonNode* root = json_builder_get_root(builder);
    gchar* text = json_node_to_compact_string(root);
    json_node_unref(root);
    g_object_unref(builder);
    return text;
}

gchar*
vivid_producer_config_active_wallpaper_id(const VividProducerConfig* config)
{
    if (config->project_path && *config->project_path)
        return g_strdup(config->project_path);
    return g_strdup("default");
}

gboolean
vivid_producer_config_stop_matcher_contains(const VividProducerConfig* config,
                                             const gchar*               identifier)
{
    if (!identifier || !*identifier)
        return FALSE;

    g_autofree gchar* normalized = g_utf8_strdown(identifier, -1);
    g_strstrip(normalized);
    if (!*normalized)
        return FALSE;

    for (guint i = 0; i < config->stop_on_applications->len; i++) {
        const gchar* matcher = g_ptr_array_index(config->stop_on_applications, i);
        if (g_strcmp0(matcher, normalized) == 0)
            return TRUE;
    }

    return FALSE;
}
