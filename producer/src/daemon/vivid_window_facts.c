#include "vivid_window_facts.h"

#include <string.h>

static void
window_fact_free(gpointer data)
{
    VividWindowFact* fact = data;
    if (!fact)
        return;

    if (fact->identifiers)
        g_ptr_array_unref(fact->identifiers);
    g_free(fact);
}

static void
mpris_player_fact_free(gpointer data)
{
    VividMprisPlayerFact* fact = data;
    if (!fact)
        return;

    g_free(fact->name);
    g_free(fact);
}

static void
session_facts_clear(VividSessionFacts* session)
{
    if (!session)
        return;

    if (session->mpris_players) {
        g_ptr_array_unref(session->mpris_players);
        session->mpris_players = NULL;
    }
    if (session->windows) {
        g_ptr_array_unref(session->windows);
        session->windows = NULL;
    }
    if (session->application_identifiers) {
        g_ptr_array_unref(session->application_identifiers);
        session->application_identifiers = NULL;
    }

    memset(session, 0, sizeof(*session));
}

static gboolean
json_bool_member(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return FALSE;
    return json_object_get_boolean_member(object, member);
}

static const gchar*
json_string_member(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;
    if (json_object_get_null_member(object, member))
        return NULL;
    return json_object_get_string_member(object, member);
}

static gint
json_int_member(JsonObject* object, const gchar* member, gint fallback)
{
    if (!object || !json_object_has_member(object, member))
        return fallback;
    if (json_object_get_null_member(object, member))
        return fallback;

    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    const GType value_type = json_node_get_value_type(node);
    if (value_type == G_TYPE_INT64 || value_type == G_TYPE_DOUBLE)
        return (gint)json_node_get_int(node);

    return fallback;
}

static JsonObject*
json_object_member_or_null(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;
    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;
    return json_node_get_object(node);
}

static JsonArray*
json_array_member_or_null(JsonObject* object, const gchar* member)
{
    if (!object || !json_object_has_member(object, member))
        return NULL;
    JsonNode* node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;
    return json_node_get_array(node);
}

static guint32
lookup_output_id_for_display_key(const VividOutputIndexEntry* entries,
                                 guint                        n_entries,
                                 const gchar*                 display_key)
{
    if (!display_key || !*display_key)
        return 0;

    for (guint i = 0; i < n_entries; i++) {
        if (entries[i].display_key &&
            g_strcmp0(entries[i].display_key, display_key) == 0)
            return entries[i].output_id;
    }

    return 0;
}

static guint32
lookup_output_id_for_monitor(const VividOutputIndexEntry* entries,
                             guint                        n_entries,
                             gint                         monitor_index)
{
    if (monitor_index < 0)
        return 0;

    for (guint i = 0; i < n_entries; i++) {
        if (entries[i].monitor_index == (guint32)monitor_index)
            return entries[i].output_id;
    }

    return 0;
}

static gint
output_facts_index_for_id(GArray* outputs, guint32 output_id)
{
    if (!outputs || output_id == 0)
        return -1;

    for (guint i = 0; i < outputs->len; i++) {
        const VividOutputWindowFacts* entry =
            &g_array_index(outputs, VividOutputWindowFacts, i);
        if (entry->output_id == output_id)
            return (gint)i;
    }

    return -1;
}

static void
output_facts_set_flags(GArray* outputs, guint32 output_id, guint32 flags)
{
    const gint index = output_facts_index_for_id(outputs, output_id);
    if (index < 0)
        return;

    VividOutputWindowFacts* entry =
        &g_array_index(outputs, VividOutputWindowFacts, (guint)index);
    entry->flags = flags & (VIVID_WINDOW_FLAG_NON_MINIMIZED |
                            VIVID_WINDOW_FLAG_FOCUSED |
                            VIVID_WINDOW_FLAG_MAXIMIZED |
                            VIVID_WINDOW_FLAG_FULLSCREEN);
}

static gchar*
normalize_identifier(const gchar* identifier)
{
    if (!identifier || !*identifier)
        return NULL;

    g_autofree gchar* normalized = g_utf8_strdown(identifier, -1);
    g_strstrip(normalized);
    if (!*normalized)
        return NULL;

    return g_strdup(normalized);
}

static GPtrArray*
parse_identifier_array(JsonArray* identifiers)
{
    GPtrArray* result = g_ptr_array_new_with_free_func(g_free);
    if (!identifiers)
        return result;

    const guint length = json_array_get_length(identifiers);
    for (guint i = 0; i < length; i++) {
        const gchar* identifier = json_array_get_string_element(identifiers, i);
        gchar* normalized = normalize_identifier(identifier);
        if (normalized)
            g_ptr_array_add(result, normalized);
    }

    return result;
}

static VividWindowFact*
parse_window_fact(JsonObject*                  window,
                  const VividOutputIndexEntry* entries,
                  guint                        n_entries)
{
    if (!window)
        return NULL;

    VividWindowFact* fact = g_new0(VividWindowFact, 1);
    fact->identifiers =
        parse_identifier_array(json_array_member_or_null(window, "identifiers"));

    const gchar* display_key = json_string_member(window, "displayKey");
    if (display_key && *display_key) {
        fact->output_id =
            lookup_output_id_for_display_key(entries, n_entries, display_key);
    } else {
        fact->output_id =
            lookup_output_id_for_monitor(entries,
                                         n_entries,
                                         json_int_member(window, "monitorIndex", -1));
    }

    fact->focused = json_bool_member(window, "focused");
    fact->maximized = json_bool_member(window, "maximized");
    fact->fullscreen = json_bool_member(window, "fullscreen");
    return fact;
}

static void
parse_session_facts(JsonObject*                  session,
                    const VividOutputIndexEntry* entries,
                    guint                        n_entries,
                    VividSessionFacts*           out_session)
{
    if (!session || !out_session)
        return;

    out_session->on_battery = json_bool_member(session, "onBattery");
    out_session->mpris_playing = json_bool_member(session, "mprisPlaying");

    out_session->mpris_players =
        g_ptr_array_new_with_free_func(mpris_player_fact_free);
    JsonArray* players = json_array_member_or_null(session, "mprisPlayers");
    const guint player_count = players ? json_array_get_length(players) : 0;
    for (guint i = 0; i < player_count; i++) {
        JsonObject* player = json_array_get_object_element(players, i);
        if (!player)
            continue;

        VividMprisPlayerFact* fact = g_new0(VividMprisPlayerFact, 1);
        const gchar* name = json_string_member(player, "name");
        if (name && *name)
            fact->name = g_strdup(name);
        fact->playing =
            g_strcmp0(json_string_member(player, "playbackStatus"), "Playing") == 0;
        g_ptr_array_add(out_session->mpris_players, fact);
    }

    out_session->windows = g_ptr_array_new_with_free_func(window_fact_free);
    JsonArray* windows = json_array_member_or_null(session, "windows");
    const guint window_count = windows ? json_array_get_length(windows) : 0;
    for (guint i = 0; i < window_count; i++) {
        VividWindowFact* window_fact =
            parse_window_fact(json_array_get_object_element(windows, i),
                              entries,
                              n_entries);
        if (window_fact)
            g_ptr_array_add(out_session->windows, window_fact);
    }

    out_session->application_identifiers = g_ptr_array_new_with_free_func(g_free);
    JsonArray* identifiers =
        json_array_member_or_null(session, "applicationIdentifiers");
    const guint identifier_count =
        identifiers ? json_array_get_length(identifiers) : 0;
    for (guint i = 0; i < identifier_count; i++) {
        gchar* normalized =
            normalize_identifier(json_array_get_string_element(identifiers, i));
        if (normalized)
            g_ptr_array_add(out_session->application_identifiers, normalized);
    }
}

static void
parse_display_flags(JsonObject*                  displays,
                    GArray*                      outputs,
                    const VividOutputIndexEntry* entries,
                    guint                        n_entries)
{
    if (!displays || !outputs)
        return;

    GList* members = json_object_get_members(displays);
    for (GList* iter = members; iter; iter = iter->next) {
        const gchar* display_key = iter->data;
        JsonObject* display = json_object_get_object_member(displays, display_key);
        if (!display)
            continue;

        const guint32 output_id =
            lookup_output_id_for_display_key(entries, n_entries, display_key);
        if (output_id == 0)
            continue;

        const guint32 flags =
            (guint32)json_int_member(display, "flags", 0);
        output_facts_set_flags(outputs, output_id, flags);
    }
    g_list_free(members);
}

VividWindowFacts*
vivid_window_facts_parse(JsonObject*                  payload,
                         const VividOutputIndexEntry* entries,
                         guint                        n_entries)
{
    if (!payload || !entries || n_entries == 0)
        return NULL;

    VividWindowFacts* facts = g_new0(VividWindowFacts, 1);
    facts->outputs = g_array_new(FALSE, FALSE, sizeof(VividOutputWindowFacts));

    for (guint i = 0; i < n_entries; i++) {
        const VividOutputWindowFacts entry = {
            .output_id = entries[i].output_id,
            .flags = 0,
        };
        g_array_append_val(facts->outputs, entry);
    }

    JsonObject* session = json_object_member_or_null(payload, "session");
    if (session)
        parse_session_facts(session, entries, n_entries, &facts->session);

    JsonObject* displays = json_object_member_or_null(payload, "displays");
    if (displays)
        parse_display_flags(displays, facts->outputs, entries, n_entries);

    return facts;
}

void
vivid_window_facts_free(VividWindowFacts* facts)
{
    if (!facts)
        return;

    if (facts->outputs)
        g_array_free(facts->outputs, TRUE);
    session_facts_clear(&facts->session);
    g_free(facts);
}

void
vivid_window_facts_describe(const VividWindowFacts* facts, GString* detail)
{
    if (!detail)
        return;

    if (!facts) {
        g_string_append(detail, " facts=(none)");
        return;
    }

    gboolean focused = FALSE;
    gboolean covering = FALSE;

    if (facts->outputs) {
        for (guint i = 0; i < facts->outputs->len; i++) {
            const VividOutputWindowFacts* output =
                &g_array_index(facts->outputs, VividOutputWindowFacts, i);
            if (output->flags & VIVID_WINDOW_STATE_FLAG_FOCUSED)
                focused = TRUE;
            if (output->flags & (VIVID_WINDOW_STATE_FLAG_MAXIMIZED |
                                 VIVID_WINDOW_STATE_FLAG_FULLSCREEN))
                covering = TRUE;
        }
    }

    g_string_append_printf(detail,
                           " facts.focused=%s max-or-fullscreen=%s",
                           focused ? "true" : "false",
                           covering ? "true" : "false");
    g_string_append_printf(detail,
                           " session.onBattery=%s mprisPlaying=%s",
                           facts->session.on_battery ? "true" : "false",
                           facts->session.mpris_playing ? "true" : "false");
}
