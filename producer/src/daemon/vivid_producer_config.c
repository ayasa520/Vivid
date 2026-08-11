#include "vivid_producer_config.h"
#include "vivid_producer_config_schema.h"

#include "../protocol/vivid_display_protocol.h"

#include <errno.h>
#include <string.h>

/*
 * SET_STATE schema: kebab-case is the canonical disk spelling; camel_key is the
 * accepted protocol alias. Unknown members log a warning; a type mismatch rejects
 * the entire patch. Table is generated from vivid_display_v1.toml.
 */
static gchar*
default_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(),
                            "vivid-producer",
                            "config-v1.json",
                            NULL);
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

static const gchar*
normalize_multi_display_mode(const gchar* value)
{
    if (g_strcmp0(value, "independent") == 0)
        return "independent";
    return "clone";
}

static VividPlaybackAction
normalize_playback_action(gint value, VividPlaybackAction fallback)
{
    if (value < VIVID_PLAYBACK_ACTION_KEEP_RUNNING ||
        value > VIVID_PLAYBACK_ACTION_STOP)
        return fallback;
    return (VividPlaybackAction)value;
}

static VividPlaybackAction
json_get_playback_action(JsonObject*         object,
                         const gchar*        member,
                         VividPlaybackAction fallback,
                         VividPlaybackAction max_value)
{
    return normalize_playback_action(
        json_get_int_clamped(object, member, (gint)fallback, 0, (gint)max_value),
        fallback);
}

static VividApplicationRuleCondition
normalize_application_rule_condition(gint value,
                                     VividApplicationRuleCondition fallback)
{
    if (value < VIVID_APPLICATION_RULE_CONDITION_RUNNING ||
        value > VIVID_APPLICATION_RULE_CONDITION_PLAYING_AUDIO)
        return fallback;
    return (VividApplicationRuleCondition)value;
}

static VividApplicationRuleCondition
json_get_application_rule_condition(JsonObject*                    object,
                                    const gchar*                   member,
                                    VividApplicationRuleCondition  fallback)
{
    return normalize_application_rule_condition(
        json_get_int_clamped(object,
                             member,
                             (gint)fallback,
                             VIVID_APPLICATION_RULE_CONDITION_RUNNING,
                             VIVID_APPLICATION_RULE_CONDITION_PLAYING_AUDIO),
        fallback);
}

static void
ensure_config_runtime_containers(VividProducerConfig* config)
{
    if (!config->application_rules)
        config->application_rules =
            g_ptr_array_new_with_free_func(vivid_application_playback_rule_free);
    if (!config->display_prefs)
        config->display_prefs =
            g_hash_table_new_full(g_str_hash,
                                  g_str_equal,
                                  g_free,
                                  (GDestroyNotify)vivid_display_prefs_free);
}

static void
replace_saved_projects(VividDisplayPrefs* prefs, JsonObject* source)
{
    if (!prefs)
        return;

    g_clear_pointer(&prefs->saved_projects, json_object_unref);
    prefs->saved_projects = json_object_new();
    if (!source)
        return;

    GList* project_paths = json_object_get_members(source);
    for (GList* iter = project_paths; iter; iter = iter->next) {
        const gchar* project_path = iter->data;
        JsonNode*    node = json_object_get_member(source, project_path);
        if (!project_path || !*project_path || !node)
            continue;

        json_object_set_member(prefs->saved_projects,
                               project_path,
                               json_node_copy(node));
    }
    g_list_free(project_paths);
}

static gchar* json_node_to_compact_string(JsonNode* node);

static gchar*
json_object_member_to_compact_string(JsonObject*  object,
                                     const gchar* kebab_member,
                                     const gchar* camel_member)
{
    JsonNode* node = NULL;

    if (!object)
        return g_strdup("{}");
    if (json_object_has_member(object, kebab_member))
        node = json_object_get_member(object, kebab_member);
    else if (camel_member && json_object_has_member(object, camel_member))
        node = json_object_get_member(object, camel_member);
    if (!node)
        return g_strdup("{}");

    JsonNode* copied = json_node_copy(node);
    gchar*    text = json_node_to_compact_string(copied);
    json_node_unref(copied);
    return text;
}

static void
json_object_move_member(JsonObject* destination,
                        JsonObject* source,
                        const gchar* member)
{
    if (!destination || !source || !member || !json_object_has_member(source, member))
        return;

    JsonNode* node = json_object_get_member(source, member);
    if (!node)
        return;

    json_object_set_member(destination, member, json_node_copy(node));
}

static void
vivid_auto_policy_clear(VividAutoPolicy* policy)
{
    if (!policy)
        return;
    memset(policy, 0, sizeof(*policy));
}

void
vivid_producer_config_effective_auto_policy(const VividProducerConfig* config,
                                            const gchar*               display_key,
                                            VividAutoPolicy*           out)
{
    if (!config || !out)
        return;

    vivid_auto_policy_clear(out);
    out->has_playback_on_focus = TRUE;
    out->playback_on_focus = config->playback_on_focus;
    out->has_playback_on_maximize_or_fullscreen = TRUE;
    out->playback_on_maximize_or_fullscreen =
        config->playback_on_maximize_or_fullscreen;
    out->has_playback_on_audio = TRUE;
    out->playback_on_audio = config->playback_on_audio;
    out->has_playback_on_battery = TRUE;
    out->playback_on_battery = config->playback_on_battery;

    const VividDisplayPrefs* prefs =
        vivid_producer_config_display_prefs(config, display_key);
    if (!prefs)
        return;

    if (prefs->auto_policy.has_playback_on_focus)
        out->playback_on_focus = prefs->auto_policy.playback_on_focus;
    if (prefs->auto_policy.has_playback_on_maximize_or_fullscreen)
        out->playback_on_maximize_or_fullscreen =
            prefs->auto_policy.playback_on_maximize_or_fullscreen;
    if (prefs->auto_policy.has_playback_on_audio)
        out->playback_on_audio = prefs->auto_policy.playback_on_audio;
    if (prefs->auto_policy.has_playback_on_battery)
        out->playback_on_battery = prefs->auto_policy.playback_on_battery;
}

static gboolean
vivid_auto_policy_is_empty(const VividAutoPolicy* policy)
{
    if (!policy)
        return TRUE;

    return !policy->has_playback_on_focus &&
        !policy->has_playback_on_maximize_or_fullscreen &&
        !policy->has_playback_on_audio &&
        !policy->has_playback_on_battery;
}

static void
vivid_auto_policy_load(VividAutoPolicy* policy, JsonObject* object)
{
    vivid_auto_policy_clear(policy);
    if (!policy || !object || !json_object_has_member(object, "auto-policy"))
        return;

    JsonObject* auto_policy = json_object_get_object_member(object, "auto-policy");
    if (!auto_policy)
        return;

    if (json_object_has_member(auto_policy, "playback-on-focus")) {
        policy->has_playback_on_focus = TRUE;
        policy->playback_on_focus =
            json_get_playback_action(auto_policy,
                                     "playback-on-focus",
                                     VIVID_PLAYBACK_ACTION_KEEP_RUNNING,
                                     VIVID_PLAYBACK_ACTION_STOP);
    }
    if (json_object_has_member(auto_policy, "playback-on-maximize-or-fullscreen")) {
        policy->has_playback_on_maximize_or_fullscreen = TRUE;
        policy->playback_on_maximize_or_fullscreen =
            json_get_playback_action(auto_policy,
                                     "playback-on-maximize-or-fullscreen",
                                     VIVID_PLAYBACK_ACTION_KEEP_RUNNING,
                                     VIVID_PLAYBACK_ACTION_STOP);
    }
    if (json_object_has_member(auto_policy, "playback-on-audio")) {
        policy->has_playback_on_audio = TRUE;
        policy->playback_on_audio =
            json_get_playback_action(auto_policy,
                                     "playback-on-audio",
                                     VIVID_PLAYBACK_ACTION_KEEP_RUNNING,
                                     VIVID_PLAYBACK_ACTION_STOP);
    }
    if (json_object_has_member(auto_policy, "playback-on-battery")) {
        policy->has_playback_on_battery = TRUE;
        policy->playback_on_battery =
            json_get_playback_action(auto_policy,
                                     "playback-on-battery",
                                     VIVID_PLAYBACK_ACTION_KEEP_RUNNING,
                                     VIVID_PLAYBACK_ACTION_STOP);
    }
}

static void
vivid_auto_policy_save(JsonBuilder* builder, const VividAutoPolicy* policy)
{
    if (!builder || vivid_auto_policy_is_empty(policy))
        return;

    json_builder_set_member_name(builder, "auto-policy");
    json_builder_begin_object(builder);
    if (policy->has_playback_on_focus) {
        json_builder_set_member_name(builder, "playback-on-focus");
        json_builder_add_int_value(builder, (gint)policy->playback_on_focus);
    }
    if (policy->has_playback_on_maximize_or_fullscreen) {
        json_builder_set_member_name(builder, "playback-on-maximize-or-fullscreen");
        json_builder_add_int_value(builder,
                                   (gint)policy->playback_on_maximize_or_fullscreen);
    }
    if (policy->has_playback_on_audio) {
        json_builder_set_member_name(builder, "playback-on-audio");
        json_builder_add_int_value(builder, (gint)policy->playback_on_audio);
    }
    if (policy->has_playback_on_battery) {
        json_builder_set_member_name(builder, "playback-on-battery");
        json_builder_add_int_value(builder, (gint)policy->playback_on_battery);
    }
    json_builder_end_object(builder);
}

VividDisplayPrefs*
vivid_display_prefs_new(void)
{
    VividDisplayPrefs* prefs = g_new0(VividDisplayPrefs, 1);
    prefs->project = json_object_new();
    prefs->saved_projects = json_object_new();
    prefs->fillmode = VIVID_DISPLAY_PREFS_UNSET;
    prefs->location = VIVID_DISPLAY_PREFS_UNSET;
    prefs->rotation = VIVID_DISPLAY_PREFS_UNSET;
    vivid_auto_policy_clear(&prefs->auto_policy);
    return prefs;
}

void
vivid_display_prefs_free(VividDisplayPrefs* prefs)
{
    if (!prefs)
        return;

    if (prefs->project)
        json_object_unref(prefs->project);
    g_clear_pointer(&prefs->saved_projects, json_object_unref);
    g_free(prefs->alias);
    g_free(prefs);
}

static gboolean
display_prefs_project_is_empty(const VividDisplayPrefs* prefs)
{
    if (!prefs || !prefs->project)
        return TRUE;

    const gchar* project_path = json_get_string(prefs->project, "project-path", "");
    if (project_path && *project_path)
        return FALSE;

    if (json_object_has_member(prefs->project, "type"))
        return FALSE;
    if (json_object_has_member(prefs->project, "content-fit"))
        return FALSE;
    if (json_object_has_member(prefs->project, "mute"))
        return FALSE;

    return TRUE;
}

gboolean
vivid_display_prefs_is_empty(const VividDisplayPrefs* prefs)
{
    if (!prefs)
        return TRUE;

    if (prefs->saved_projects && json_object_get_size(prefs->saved_projects) > 0)
        return FALSE;

    return display_prefs_project_is_empty(prefs) &&
        prefs->fillmode == VIVID_DISPLAY_PREFS_UNSET &&
        prefs->location == VIVID_DISPLAY_PREFS_UNSET &&
        prefs->rotation == VIVID_DISPLAY_PREFS_UNSET &&
        vivid_auto_policy_is_empty(&prefs->auto_policy) &&
        (!prefs->alias || !*prefs->alias);
}

static void
project_move_alias(JsonObject*  project,
                   JsonObject*  entry,
                   const gchar* kebab_key,
                   const gchar* camel_key)
{
    if (!project || !entry)
        return;

    if (json_object_has_member(entry, kebab_key)) {
        json_object_move_member(project, entry, kebab_key);
        return;
    }

    if (camel_key && json_object_has_member(entry, camel_key)) {
        JsonNode* node = json_object_get_member(entry, camel_key);
        if (node)
            json_object_set_member(project, kebab_key, json_node_copy(node));
    }
}

static VividDisplayPrefs*
display_prefs_from_json_object(JsonObject* entry)
{
    VividDisplayPrefs* prefs = vivid_display_prefs_new();
    if (!entry)
        return prefs;

    project_move_alias(prefs->project, entry, "project-path", "projectPath");
    project_move_alias(prefs->project, entry, "project-type", "projectType");
    project_move_alias(prefs->project, entry, "type", NULL);
    project_move_alias(prefs->project, entry, "content-fit", "contentFit");
    if (json_object_has_member(entry, "mute"))
        json_object_move_member(prefs->project, entry, "mute");
    else if (json_object_has_member(entry, "muted")) {
        JsonNode* node = json_object_get_member(entry, "muted");
        if (node)
            json_object_set_member(prefs->project, "mute", json_node_copy(node));
    }

    if (json_object_has_member(entry, "fillmode"))
        prefs->fillmode =
            json_get_int_clamped(entry, "fillmode", VIVID_DISPLAY_PREFS_UNSET, 0, 3);
    if (json_object_has_member(entry, "location"))
        prefs->location =
            json_get_int_clamped(entry, "location", VIVID_DISPLAY_PREFS_UNSET, 0, 8);
    if (json_object_has_member(entry, "rotation"))
        prefs->rotation =
            json_get_int_clamped(entry, "rotation", VIVID_DISPLAY_PREFS_UNSET, 0, 3);
    if (json_object_has_member(entry, "alias"))
        replace_string(&prefs->alias, json_get_string(entry, "alias", ""));

    if (json_object_has_member(entry, "saved-projects"))
        replace_saved_projects(prefs,
                               json_object_get_object_member(entry, "saved-projects"));
    else if (json_object_has_member(entry, "savedProjects"))
        replace_saved_projects(prefs,
                               json_object_get_object_member(entry, "savedProjects"));
    else
        replace_saved_projects(prefs, NULL);

    vivid_auto_policy_load(&prefs->auto_policy, entry);
    return prefs;
}

static JsonObject*
display_prefs_to_json_object(const VividDisplayPrefs* prefs)
{
    JsonObject* entry = json_object_new();
    if (!prefs)
        return entry;

    GList* members = json_object_get_members(prefs->project);
    for (GList* iter = members; iter; iter = iter->next) {
        const gchar* member = iter->data;
        if (g_strcmp0(member, "user-properties") == 0 ||
            g_strcmp0(member, "userProperties") == 0)
            continue;
        JsonNode* node = json_object_get_member(prefs->project, member);
        if (node)
            json_object_set_member(entry, member, json_node_copy(node));
    }
    g_list_free(members);

    if (prefs->fillmode != VIVID_DISPLAY_PREFS_UNSET)
        json_object_set_int_member(entry, "fillmode", prefs->fillmode);
    if (prefs->location != VIVID_DISPLAY_PREFS_UNSET)
        json_object_set_int_member(entry, "location", prefs->location);
    if (prefs->rotation != VIVID_DISPLAY_PREFS_UNSET)
        json_object_set_int_member(entry, "rotation", prefs->rotation);
    if (prefs->alias && *prefs->alias)
        json_object_set_string_member(entry, "alias", prefs->alias);

    if (prefs->saved_projects && json_object_get_size(prefs->saved_projects) > 0) {
        JsonObject* saved_projects = json_object_new();
        GList*      project_paths = json_object_get_members(prefs->saved_projects);
        for (GList* iter = project_paths; iter; iter = iter->next) {
            const gchar* project_path = iter->data;
            JsonNode*    node =
                json_object_get_member(prefs->saved_projects, project_path);
            if (!project_path || !*project_path || !node)
                continue;

            json_object_set_member(saved_projects,
                                   project_path,
                                   json_node_copy(node));
        }
        g_list_free(project_paths);
        json_object_set_object_member(entry, "saved-projects", saved_projects);
    }

    if (!vivid_auto_policy_is_empty(&prefs->auto_policy)) {
        JsonBuilder* builder = json_builder_new();
        vivid_auto_policy_save(builder, &prefs->auto_policy);
        JsonNode* node = json_builder_get_root(builder);
        if (node && JSON_NODE_HOLDS_OBJECT(node))
            json_object_set_member(entry,
                                   "auto-policy",
                                   json_node_copy(node));
        if (node)
            json_node_unref(node);
        g_object_unref(builder);
    }

    return entry;
}

static VividDisplayPrefs*
display_prefs_copy(const VividDisplayPrefs* prefs)
{
    if (!prefs)
        return NULL;

    g_autoptr(JsonObject) entry = display_prefs_to_json_object(prefs);
    return display_prefs_from_json_object(entry);
}

static void
config_replace_display_prefs_from_object(VividProducerConfig* config,
                                         JsonObject*          projects)
{
    ensure_config_runtime_containers(config);
    g_hash_table_remove_all(config->display_prefs);
    if (!projects)
        return;

    GList* members = json_object_get_members(projects);
    for (GList* iter = members; iter; iter = iter->next) {
        const gchar* display_key = iter->data;
        JsonObject* entry = json_object_get_object_member(projects, display_key);
        if (!display_key || !*display_key || !entry)
            continue;

        VividDisplayPrefs* prefs = display_prefs_from_json_object(entry);
        if (vivid_display_prefs_is_empty(prefs)) {
            vivid_display_prefs_free(prefs);
            continue;
        }

        g_hash_table_insert(config->display_prefs,
                            g_strdup(display_key),
                            prefs);
    }
    g_list_free(members);
}

const VividDisplayPrefs*
vivid_producer_config_display_prefs(const VividProducerConfig* config,
                                    const gchar*               display_key)
{
    if (!config || !display_key || !*display_key || !config->display_prefs)
        return NULL;

    return g_hash_table_lookup(config->display_prefs, display_key);
}

const JsonObject*
vivid_producer_config_display_project_view(const VividProducerConfig* config,
                                           const gchar*               display_key)
{
    const VividDisplayPrefs* prefs =
        vivid_producer_config_display_prefs(config, display_key);
    return prefs ? prefs->project : NULL;
}

gchar*
vivid_producer_config_stored_user_properties_json(const VividProducerConfig* config,
                                                 const gchar*               display_key,
                                                 const gchar*               project_path)
{
    const VividDisplayPrefs* prefs =
        vivid_producer_config_display_prefs(config, display_key);
    JsonObject*              entry = NULL;

    if (!prefs || !project_path || !*project_path || !prefs->saved_projects)
        return g_strdup("{}");

    if (!json_object_has_member(prefs->saved_projects, project_path))
        return g_strdup("{}");

    entry = json_object_get_object_member(prefs->saved_projects, project_path);
    return json_object_member_to_compact_string(entry,
                                                "user-properties",
                                                "userProperties");
}

GHashTable*
vivid_producer_config_snapshot_display_prefs(const VividProducerConfig* config)
{
    GHashTable* snapshot =
        g_hash_table_new_full(g_str_hash,
                              g_str_equal,
                              g_free,
                              (GDestroyNotify)vivid_display_prefs_free);
    if (!config || !config->display_prefs)
        return snapshot;

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, config->display_prefs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_hash_table_insert(snapshot,
                            g_strdup(key),
                            display_prefs_copy(value));
    }
    return snapshot;
}

void
vivid_producer_config_free_display_prefs_snapshot(GHashTable* snapshot)
{
    g_hash_table_destroy(snapshot);
}

static gboolean
display_prefs_render_contract_changed(const VividDisplayPrefs* previous,
                                      const VividDisplayPrefs* current)
{
    const JsonObject* previous_project = previous ? previous->project : NULL;
    const JsonObject* current_project = current ? current->project : NULL;

    const gchar* previous_project_path =
        json_get_string((JsonObject*)previous_project, "project-path", "");
    const gchar* current_project_path =
        json_get_string((JsonObject*)current_project, "project-path", "");
    if (g_strcmp0(previous_project_path, current_project_path) != 0)
        return TRUE;

    const gint previous_content_fit =
        previous_project
            ? json_get_int_clamped((JsonObject*)previous_project,
                                   "content-fit",
                                   -1,
                                   -1,
                                   3)
            : -1;
    const gint current_content_fit =
        current_project
            ? json_get_int_clamped((JsonObject*)current_project,
                                   "content-fit",
                                   -1,
                                   -1,
                                   3)
            : -1;
    return previous_content_fit != current_content_fit;
}

gboolean
vivid_producer_config_display_prefs_render_contract_changed(
    GHashTable*                previous_snapshot,
    const VividProducerConfig* config)
{
    if (!config)
        return FALSE;

    GHashTable* previous = previous_snapshot;
    GHashTable* current_keys = config->display_prefs;
    if (!previous && !current_keys)
        return FALSE;

    if (previous) {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, previous);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            const VividDisplayPrefs* current =
                current_keys
                    ? g_hash_table_lookup(current_keys, key)
                    : NULL;
            if (display_prefs_render_contract_changed(value, current))
                return TRUE;
        }
    }

    if (current_keys) {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, current_keys);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            if (previous && g_hash_table_lookup(previous, key))
                continue;
            if (display_prefs_render_contract_changed(NULL, value))
                return TRUE;
        }
    }

    return FALSE;
}

static const VividConfigSchemaEntry*
schema_entry_for_member(const gchar* member)
{
    if (!member)
        return NULL;

    for (guint i = 0; i < G_N_ELEMENTS(vivid_set_state_schema); i++) {
        const VividConfigSchemaEntry* entry = &vivid_set_state_schema[i];
        if (g_strcmp0(member, entry->kebab_key) == 0)
            return entry;
        if (entry->camel_key && g_strcmp0(member, entry->camel_key) == 0)
            return entry;
    }

    return NULL;
}

static gboolean
json_node_matches_schema_type(JsonNode* node, VividConfigSchemaType type)
{
    if (!node)
        return FALSE;

    switch (type) {
    case VIVID_CONFIG_SCHEMA_BOOL:
        return JSON_NODE_HOLDS_VALUE(node) &&
            (json_node_get_value_type(node) == G_TYPE_BOOLEAN ||
             json_node_get_value_type(node) == G_TYPE_INT64 ||
             json_node_get_value_type(node) == G_TYPE_STRING);
    case VIVID_CONFIG_SCHEMA_INT:
        return JSON_NODE_HOLDS_VALUE(node) &&
            (json_node_get_value_type(node) == G_TYPE_INT64 ||
             json_node_get_value_type(node) == G_TYPE_DOUBLE ||
             json_node_get_value_type(node) == G_TYPE_STRING);
    case VIVID_CONFIG_SCHEMA_STRING:
        return JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_STRING;
    case VIVID_CONFIG_SCHEMA_ARRAY:
        return JSON_NODE_HOLDS_ARRAY(node);
    case VIVID_CONFIG_SCHEMA_OBJECT:
        return JSON_NODE_HOLDS_OBJECT(node);
    default:
        return FALSE;
    }
}

static gboolean
validate_set_state_payload(JsonObject* payload, GError** error)
{
    if (!payload)
        return TRUE;

    /*
     * Canonicalize camelCase aliases onto kebab-case keys before validation and
     * apply so a single read path handles every accepted spelling.
     */
    GList* alias_members = json_object_get_members(payload);
    for (GList* iter = alias_members; iter; iter = iter->next) {
        const gchar* member = iter->data;
        const VividConfigSchemaEntry* entry = schema_entry_for_member(member);
        if (!entry || g_strcmp0(member, entry->kebab_key) == 0)
            continue;
        if (!entry->camel_key || g_strcmp0(member, entry->camel_key) != 0)
            continue;

        JsonNode* node = json_object_get_member(payload, member);
        if (!node)
            continue;

        json_object_set_member(payload,
                               entry->kebab_key,
                               json_node_copy(node));
        json_object_remove_member(payload, member);
    }
    g_list_free(alias_members);

    GList* members = json_object_get_members(payload);
    for (GList* iter = members; iter; iter = iter->next) {
        const gchar* member = iter->data;
        if (g_strcmp0(member, "requestId") == 0)
            continue;
        const VividConfigSchemaEntry* entry = schema_entry_for_member(member);
        JsonNode* node = json_object_get_member(payload, member);
        if (!entry) {
            g_warning("VividProducer: SET_STATE ignored unknown key %s", member);
            continue;
        }

        if (!json_node_matches_schema_type(node, entry->type)) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_DATA,
                        "SET_STATE key %s has invalid type",
                        member);
            g_list_free(members);
            return FALSE;
        }
    }
    g_list_free(members);
    return TRUE;
}

static gboolean
config_debounced_save_timeout(gpointer user_data)
{
    VividProducerConfig* config = user_data;
    if (!config)
        return G_SOURCE_REMOVE;

    config->save_source_id = 0;
    vivid_producer_config_save(config);
    return G_SOURCE_REMOVE;
}

void
vivid_producer_config_schedule_save(VividProducerConfig* config)
{
    if (!config)
        return;

    if (config->save_source_id != 0)
        g_source_remove(config->save_source_id);

    config->save_source_id =
        g_timeout_add(VIVID_PRODUCER_CONFIG_SAVE_DEBOUNCE_MSEC,
                      config_debounced_save_timeout,
                      config);
}

void
vivid_producer_config_flush_save(VividProducerConfig* config)
{
    if (!config)
        return;

    if (config->save_source_id != 0) {
        g_source_remove(config->save_source_id);
        config->save_source_id = 0;
    }

    vivid_producer_config_save(config);
}

void
vivid_producer_config_reset_defaults(VividProducerConfig* config)
{
    g_return_if_fail(config != NULL);

    ensure_config_runtime_containers(config);

    config->mute = FALSE;
    config->volume = 50;
    config->change_wallpaper = FALSE;
    replace_string(&config->change_wallpaper_directory_path, "");
    config->change_wallpaper_interval = 15;
    config->change_wallpaper_mode = 1;
    config->playback_on_focus = VIVID_PLAYBACK_ACTION_KEEP_RUNNING;
    config->playback_on_maximize_or_fullscreen = VIVID_PLAYBACK_ACTION_KEEP_RUNNING;
    config->playback_on_audio = VIVID_PLAYBACK_ACTION_KEEP_RUNNING;
    config->playback_on_battery = VIVID_PLAYBACK_ACTION_KEEP_RUNNING;
    g_ptr_array_set_size(config->application_rules, 0);
    config->debug_mode = FALSE;
    replace_string(&config->render_device, "auto");
    config->content_fit = 1;
    config->scene_fps = 30;
    config->startup_delay = 1000;
    config->show_panel_menu = TRUE;
    replace_string(&config->project_browser_filter_state, "");
    replace_string(&config->project_browser_sort_key, "name");
    replace_string(&config->multi_display_mode, "clone");
    replace_string(&config->primary_display_key, "");
    replace_string(&config->current_config_display_key, "");
    g_hash_table_remove_all(config->display_prefs);
}

static void
load_string_member(JsonObject* object, const gchar* member, gchar** target)
{
    if (!object || !json_object_has_member(object, member) ||
        json_object_get_null_member(object, member))
        return;

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

VividApplicationPlaybackRule*
vivid_application_playback_rule_new(const gchar*                  name,
                                    VividApplicationRuleCondition condition,
                                    VividPlaybackAction           playback)
{
    VividApplicationPlaybackRule* rule = g_new0(VividApplicationPlaybackRule, 1);
    rule->name = g_strdup(name ? name : "");
    g_strstrip(rule->name);
    rule->condition =
        normalize_application_rule_condition((gint)condition,
                                             VIVID_APPLICATION_RULE_CONDITION_RUNNING);
    rule->playback =
        normalize_playback_action((gint)playback, VIVID_PLAYBACK_ACTION_MUTE);
    return rule;
}

void
vivid_application_playback_rule_free(gpointer data)
{
    VividApplicationPlaybackRule* rule = data;
    if (!rule)
        return;

    g_free(rule->name);
    g_free(rule);
}

static void
load_application_rules_member(JsonObject* object,
                              const gchar* member,
                              GPtrArray* target)
{
    if (!object || !json_object_has_member(object, member))
        return;

    JsonArray* array = json_object_get_array_member(object, member);
    if (!array)
        return;

    g_ptr_array_set_size(target, 0);
    const guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        JsonObject* rule_object = json_array_get_object_element(array, i);
        if (!rule_object)
            continue;

        const gchar* name = json_get_string(rule_object, "name", "");
        if (!name || !*name)
            name = json_get_string(rule_object, "application", "");
        if (!name || !*name)
            continue;

        VividApplicationPlaybackRule* rule =
            vivid_application_playback_rule_new(
                name,
                json_get_application_rule_condition(
                    rule_object,
                    "condition",
                    VIVID_APPLICATION_RULE_CONDITION_RUNNING),
                json_get_playback_action(rule_object,
                                         "playback",
                                         VIVID_PLAYBACK_ACTION_MUTE,
                                         VIVID_PLAYBACK_ACTION_STOP));
        if (rule->name && *rule->name)
            g_ptr_array_add(target, rule);
        else
            vivid_application_playback_rule_free(rule);
    }
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

void
vivid_producer_config_init(VividProducerConfig* config, const gchar* config_path)
{
    g_return_if_fail(config != NULL);

    memset(config, 0, sizeof(*config));
    config->config_path = config_path && *config_path ? g_strdup(config_path) : default_config_path();
    vivid_producer_config_reset_defaults(config);
}

void
vivid_producer_config_clear(VividProducerConfig* config)
{
    if (!config)
        return;

    if (config->save_source_id != 0)
        g_source_remove(config->save_source_id);

    g_free(config->config_path);
    g_free(config->change_wallpaper_directory_path);
    g_free(config->render_device);
    g_free(config->project_browser_filter_state);
    g_free(config->project_browser_sort_key);
    g_free(config->multi_display_mode);
    g_free(config->primary_display_key);
    g_free(config->current_config_display_key);
    g_clear_pointer(&config->display_prefs, g_hash_table_unref);
    g_clear_pointer(&config->application_rules, g_ptr_array_unref);
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

    config->mute = json_get_boolean(global, "mute", config->mute);
    config->volume = json_get_int_clamped(global, "volume", config->volume, 0, 100);
    config->change_wallpaper = json_get_boolean(global, "change-wallpaper", config->change_wallpaper);
    load_string_member(global, "change-wallpaper-directory-path", &config->change_wallpaper_directory_path);
    config->change_wallpaper_interval =
        json_get_int_clamped(global, "change-wallpaper-interval", config->change_wallpaper_interval, 1, 1440);
    config->change_wallpaper_mode =
        json_get_int_clamped(global, "change-wallpaper-mode", config->change_wallpaper_mode, 0, 3);
    config->playback_on_focus =
        json_get_playback_action(global,
                                 "playback-on-focus",
                                 config->playback_on_focus,
                                 VIVID_PLAYBACK_ACTION_PAUSE_ALL);
    config->playback_on_maximize_or_fullscreen =
        json_get_playback_action(global,
                                 "playback-on-maximize-or-fullscreen",
                                 config->playback_on_maximize_or_fullscreen,
                                 VIVID_PLAYBACK_ACTION_STOP);
    config->playback_on_audio =
        json_get_playback_action(global,
                                 "playback-on-audio",
                                 config->playback_on_audio,
                                 VIVID_PLAYBACK_ACTION_PAUSE_ALL);
    config->playback_on_battery =
        json_get_playback_action(global,
                                 "playback-on-battery",
                                 config->playback_on_battery,
                                 VIVID_PLAYBACK_ACTION_STOP);
    if (json_get_boolean(global, "pause-on-focus", FALSE))
        config->playback_on_focus = VIVID_PLAYBACK_ACTION_PAUSE_ALL;
    if (json_get_int_clamped(global, "pause-on-maximize-or-fullscreen", 0, 0, 2) > 0)
        config->playback_on_maximize_or_fullscreen =
            VIVID_PLAYBACK_ACTION_PAUSE_PER_MONITOR;
    if (json_get_int_clamped(global, "pause-on-battery", 0, 0, 2) > 0)
        config->playback_on_battery = VIVID_PLAYBACK_ACTION_PAUSE_ALL;
    if (json_get_boolean(global, "pause-on-mpris-playing", FALSE))
        config->playback_on_audio = VIVID_PLAYBACK_ACTION_PAUSE_ALL;
    load_application_rules_member(global, "application-rules", config->application_rules);
    if (config->application_rules->len == 0 && json_object_has_member(global, "stop-on-applications")) {
        g_autoptr(GPtrArray) stop_on_applications = g_ptr_array_new_with_free_func(g_free);
        load_string_array_member(global, "stop-on-applications", stop_on_applications);
        for (guint i = 0; i < stop_on_applications->len; i++) {
            const gchar* name = g_ptr_array_index(stop_on_applications, i);
            g_ptr_array_add(config->application_rules,
                            vivid_application_playback_rule_new(
                                name,
                                VIVID_APPLICATION_RULE_CONDITION_RUNNING,
                                VIVID_PLAYBACK_ACTION_STOP));
        }
    }
    config->debug_mode = json_get_boolean(global, "debug-mode", config->debug_mode);
    load_string_member(global, "render-device", &config->render_device);
    config->content_fit = json_get_int_clamped(global, "content-fit", config->content_fit, 1, 3);
    config->scene_fps = json_get_int_clamped(global, "scene-fps", config->scene_fps, 5, 240);
    config->startup_delay = json_get_int_clamped(global, "startup-delay", config->startup_delay, 0, 10000);
    config->show_panel_menu = json_get_boolean(global, "show-panel-menu", config->show_panel_menu);
    load_string_member(global, "project-browser-filter-state", &config->project_browser_filter_state);
    load_string_member(global, "project-browser-sort-key", &config->project_browser_sort_key);
    if (json_object_has_member(global, "multi-display-mode")) {
        replace_string(&config->multi_display_mode,
                       normalize_multi_display_mode(json_get_string(global,
                                                                    "multi-display-mode",
                                                                    "clone")));
    }
    load_string_member(global, "primary-display-key", &config->primary_display_key);
    load_string_member(global,
                       "current-config-display-key",
                       &config->current_config_display_key);
    if (json_object_has_member(global, "per-output-projects")) {
        JsonObject* projects = json_object_get_object_member(global, "per-output-projects");
        config_replace_display_prefs_from_object(config, projects);
    }
    return TRUE;
}

static void
builder_add_display_prefs(JsonBuilder* builder, const VividProducerConfig* config)
{
    json_builder_set_member_name(builder, "per-output-projects");
    json_builder_begin_object(builder);
    if (config && config->display_prefs) {
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, config->display_prefs);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            const VividDisplayPrefs* prefs = value;
            if (!prefs || vivid_display_prefs_is_empty(prefs))
                continue;

            JsonObject* entry = display_prefs_to_json_object(prefs);
            JsonNode* node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, entry);
            json_builder_set_member_name(builder, key);
            json_builder_add_value(builder, node);
        }
    }
    json_builder_end_object(builder);
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

    ADD_BOOL("mute", config->mute);
    ADD_INT("volume", config->volume);
    ADD_BOOL("change-wallpaper", config->change_wallpaper);
    ADD_STRING("change-wallpaper-directory-path", config->change_wallpaper_directory_path);
    ADD_INT("change-wallpaper-interval", config->change_wallpaper_interval);
    ADD_INT("change-wallpaper-mode", config->change_wallpaper_mode);
    ADD_INT("playback-on-focus", (gint)config->playback_on_focus);
    ADD_INT("playback-on-maximize-or-fullscreen",
            (gint)config->playback_on_maximize_or_fullscreen);
    ADD_INT("playback-on-audio", (gint)config->playback_on_audio);
    ADD_INT("playback-on-battery", (gint)config->playback_on_battery);

    json_builder_set_member_name(builder, "application-rules");
    json_builder_begin_array(builder);
    for (guint i = 0; i < config->application_rules->len; i++) {
        const VividApplicationPlaybackRule* rule =
            g_ptr_array_index(config->application_rules, i);
        if (!rule || !rule->name || !*rule->name)
            continue;
        json_builder_begin_object(builder);
        ADD_STRING("name", rule->name);
        ADD_INT("condition", (gint)rule->condition);
        ADD_INT("playback", (gint)rule->playback);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    ADD_BOOL("debug-mode", config->debug_mode);
    ADD_STRING("render-device", config->render_device);
    ADD_INT("content-fit", config->content_fit);
    ADD_INT("scene-fps", config->scene_fps);
    ADD_INT("startup-delay", config->startup_delay);
    ADD_BOOL("show-panel-menu", config->show_panel_menu);
    ADD_STRING("project-browser-filter-state", config->project_browser_filter_state);
    ADD_STRING("project-browser-sort-key", config->project_browser_sort_key);
    ADD_STRING("multi-display-mode",
               normalize_multi_display_mode(config->multi_display_mode));
    ADD_STRING("primary-display-key",
               config->primary_display_key ? config->primary_display_key : "");
    ADD_STRING("current-config-display-key",
               config->current_config_display_key
                   ? config->current_config_display_key
                   : "");
    builder_add_display_prefs(builder, config);

#undef ADD_STRING
#undef ADD_BOOL
#undef ADD_INT

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
apply_set_state_payload(VividProducerConfig* config, JsonObject* payload)
{
    if (json_get_boolean(payload, "reset-defaults", FALSE)) {
        vivid_producer_config_reset_defaults(config);
        return;
    }

    config->mute = json_get_boolean(payload, "mute", config->mute);
    config->volume = json_get_int_clamped(payload, "volume", config->volume, 0, 100);
    config->change_wallpaper = json_get_boolean(payload, "change-wallpaper", config->change_wallpaper);
    load_string_member(payload, "change-wallpaper-directory-path", &config->change_wallpaper_directory_path);
    config->change_wallpaper_interval =
        json_get_int_clamped(payload, "change-wallpaper-interval", config->change_wallpaper_interval, 1, 1440);
    config->change_wallpaper_mode =
        json_get_int_clamped(payload, "change-wallpaper-mode", config->change_wallpaper_mode, 0, 3);
    config->playback_on_focus =
        json_get_playback_action(payload,
                                 "playback-on-focus",
                                 config->playback_on_focus,
                                 VIVID_PLAYBACK_ACTION_PAUSE_ALL);
    config->playback_on_maximize_or_fullscreen =
        json_get_playback_action(payload,
                                 "playback-on-maximize-or-fullscreen",
                                 config->playback_on_maximize_or_fullscreen,
                                 VIVID_PLAYBACK_ACTION_STOP);
    config->playback_on_audio =
        json_get_playback_action(payload,
                                 "playback-on-audio",
                                 config->playback_on_audio,
                                 VIVID_PLAYBACK_ACTION_PAUSE_ALL);
    config->playback_on_battery =
        json_get_playback_action(payload,
                                 "playback-on-battery",
                                 config->playback_on_battery,
                                 VIVID_PLAYBACK_ACTION_STOP);
    load_application_rules_member(payload,
                                  "application-rules",
                                  config->application_rules);
    config->debug_mode = json_get_boolean(payload, "debug-mode", config->debug_mode);
    load_string_member(payload, "render-device", &config->render_device);
    config->content_fit = json_get_int_clamped(payload, "content-fit", config->content_fit, 1, 3);
    config->scene_fps = json_get_int_clamped(payload, "scene-fps", config->scene_fps, 5, 240);
    config->startup_delay = json_get_int_clamped(payload, "startup-delay", config->startup_delay, 0, 10000);
    config->show_panel_menu = json_get_boolean(payload, "show-panel-menu", config->show_panel_menu);
    load_string_member(payload, "project-browser-filter-state", &config->project_browser_filter_state);
    load_string_member(payload, "project-browser-sort-key", &config->project_browser_sort_key);
    if (json_object_has_member(payload, "multi-display-mode")) {
        replace_string(&config->multi_display_mode,
                       normalize_multi_display_mode(json_get_string(payload,
                                                                    "multi-display-mode",
                                                                    "clone")));
    }
    if (json_object_has_member(payload, "primary-display-key"))
        load_string_member(payload,
                           "primary-display-key",
                           &config->primary_display_key);
    if (json_object_has_member(payload, "current-config-display-key"))
        load_string_member(payload,
                           "current-config-display-key",
                           &config->current_config_display_key);
    if (json_object_has_member(payload, "per-output-projects")) {
        JsonObject* projects = json_object_get_object_member(payload, "per-output-projects");
        config_replace_display_prefs_from_object(config, projects);
    }
}

gboolean
vivid_producer_config_apply_control(VividProducerConfig* config,
                                     guint16               control_opcode,
                                     JsonObject*           payload,
                                     GError**              error)
{
    g_return_val_if_fail(config != NULL, FALSE);

    if (!payload) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "control payload is missing");
        return FALSE;
    }

    switch (control_opcode) {
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

    case VIVID_DISPLAY_CONTROL_SET_STATE:
        if (!validate_set_state_payload(payload, error))
            return FALSE;
        apply_set_state_payload(config, payload);
        return TRUE;

    default:
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "unsupported control opcode %u",
                    control_opcode);
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
    json_builder_end_object(builder);

    JsonNode* root = json_builder_get_root(builder);
    gchar* text = json_node_to_compact_string(root);
    json_node_unref(root);
    g_object_unref(builder);
    return text;
}
