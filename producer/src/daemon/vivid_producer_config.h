#ifndef VIVID_PRODUCER_CONFIG_H
#define VIVID_PRODUCER_CONFIG_H

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct _VividProducerConfig VividProducerConfig;
typedef struct _VividDisplayPrefs VividDisplayPrefs;

#define VIVID_DISPLAY_PREFS_UNSET (-1)
#define VIVID_PRODUCER_CONFIG_SAVE_DEBOUNCE_MSEC 2000

/*
 * SET_STATE / disk key conventions:
 * - On-disk config JSON uses kebab-case only.
 * - CONTROL SET_STATE payloads may use camelCase aliases listed in the schema
 *   table inside vivid_producer_config_schema.h (generated from spec).
 */

typedef enum
{
    VIVID_PLAYBACK_ACTION_KEEP_RUNNING = 0,
    VIVID_PLAYBACK_ACTION_MUTE = 1,
    VIVID_PLAYBACK_ACTION_PAUSE_PER_MONITOR = 2,
    VIVID_PLAYBACK_ACTION_PAUSE_ALL = 3,
    VIVID_PLAYBACK_ACTION_STOP = 4,
} VividPlaybackAction;

typedef enum
{
    VIVID_APPLICATION_RULE_CONDITION_RUNNING = 0,
    VIVID_APPLICATION_RULE_CONDITION_FOCUSED = 1,
    VIVID_APPLICATION_RULE_CONDITION_MAXIMIZED = 2,
    VIVID_APPLICATION_RULE_CONDITION_FULLSCREEN = 3,
    VIVID_APPLICATION_RULE_CONDITION_PLAYING_AUDIO = 4,
} VividApplicationRuleCondition;

typedef struct
{
    gchar* name;
    VividApplicationRuleCondition condition;
    VividPlaybackAction playback;
} VividApplicationPlaybackRule;

typedef struct
{
    gboolean              has_playback_on_focus;
    VividPlaybackAction   playback_on_focus;
    gboolean              has_playback_on_maximize_or_fullscreen;
    VividPlaybackAction   playback_on_maximize_or_fullscreen;
    gboolean              has_playback_on_audio;
    VividPlaybackAction   playback_on_audio;
    gboolean              has_playback_on_battery;
    VividPlaybackAction   playback_on_battery;
} VividAutoPolicy;

struct _VividDisplayPrefs
{
    JsonObject* project;
    JsonObject* saved_projects;
    gint        fillmode;
    gint        location;
    gint        rotation;
    VividAutoPolicy auto_policy;
    gchar*      alias;
};

struct _VividProducerConfig
{
    gchar* config_path;

    gboolean mute;
    gint volume;
    gboolean change_wallpaper;
    gchar* change_wallpaper_directory_path;
    gint change_wallpaper_interval;
    gint change_wallpaper_mode;
    VividPlaybackAction playback_on_focus;
    VividPlaybackAction playback_on_maximize_or_fullscreen;
    VividPlaybackAction playback_on_audio;
    VividPlaybackAction playback_on_battery;
    GPtrArray* application_rules;
    gboolean debug_mode;
    gchar* render_device;
    gint content_fit;
    gint scene_fps;
    gboolean gfx_reflections;
    gint gfx_volumetrics;
    gint gfx_shadows;
    gint gfx_postprocessing;
    gint startup_delay;
    gboolean show_panel_menu;
    gchar* project_browser_filter_state;
    gchar* project_browser_sort_key;
    gchar* multi_display_mode;
    gchar* primary_display_key;
    gchar* current_config_display_key;
    GHashTable* display_prefs;
    guint save_source_id;
};

VividDisplayPrefs* vivid_display_prefs_new(void);
void               vivid_display_prefs_free(VividDisplayPrefs* prefs);
gboolean           vivid_display_prefs_is_empty(const VividDisplayPrefs* prefs);

void vivid_producer_config_init(VividProducerConfig* config, const gchar* config_path);
void vivid_producer_config_clear(VividProducerConfig* config);
void vivid_producer_config_reset_defaults(VividProducerConfig* config);

gboolean vivid_producer_config_load(VividProducerConfig* config);
gboolean vivid_producer_config_save(const VividProducerConfig* config);
void     vivid_producer_config_schedule_save(VividProducerConfig* config);
void     vivid_producer_config_flush_save(VividProducerConfig* config);

void vivid_producer_config_effective_auto_policy(const VividProducerConfig* config,
                                               const gchar*               display_key,
                                               VividAutoPolicy*           out);

gboolean vivid_producer_config_apply_control(VividProducerConfig* config,
                                              guint16               control_opcode,
                                              JsonObject*           payload,
                                              GError**              error);

const VividDisplayPrefs* vivid_producer_config_display_prefs(
    const VividProducerConfig* config,
    const gchar*               display_key);
const JsonObject* vivid_producer_config_display_project_view(
    const VividProducerConfig* config,
    const gchar*               display_key);

gchar* vivid_producer_config_stored_user_properties_json(
    const VividProducerConfig* config,
    const gchar*               display_key,
    const gchar*               project_path);

GHashTable* vivid_producer_config_snapshot_display_prefs(
    const VividProducerConfig* config);
void vivid_producer_config_free_display_prefs_snapshot(GHashTable* snapshot);
gboolean vivid_producer_config_display_prefs_render_contract_changed(
    GHashTable*                previous_snapshot,
    const VividProducerConfig* config);

gchar* vivid_producer_config_to_json(const VividProducerConfig* config);
VividApplicationPlaybackRule* vivid_application_playback_rule_new(
    const gchar*                  name,
    VividApplicationRuleCondition condition,
    VividPlaybackAction           playback);
void vivid_application_playback_rule_free(gpointer data);

#endif
