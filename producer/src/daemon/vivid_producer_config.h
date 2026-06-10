#ifndef VIVID_PRODUCER_CONFIG_H
#define VIVID_PRODUCER_CONFIG_H

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct _VividProducerConfig VividProducerConfig;

struct _VividProducerConfig
{
    gchar* config_path;

    gchar* project_path;
    gchar* user_properties;
    gboolean mute;
    gint volume;
    gboolean change_wallpaper;
    gchar* change_wallpaper_directory_path;
    gint change_wallpaper_interval;
    gint change_wallpaper_mode;
    gint pause_on_maximize_or_fullscreen;
    gboolean pause_on_focus;
    gint pause_on_battery;
    gint low_battery_threshold;
    gboolean pause_on_mpris_playing;
    GPtrArray* stop_on_applications;
    gboolean debug_mode;
    gchar* render_device;
    gint content_fit;
    gint scene_fps;
    gint startup_delay;
    gboolean show_panel_menu;
    gchar* project_browser_filter_state;
    gchar* project_browser_sort_key;

    GHashTable* wallpaper_user_properties;
};

void vivid_producer_config_init(VividProducerConfig* config, const gchar* config_path);
void vivid_producer_config_clear(VividProducerConfig* config);
void vivid_producer_config_reset_defaults(VividProducerConfig* config);

gboolean vivid_producer_config_load(VividProducerConfig* config);
gboolean vivid_producer_config_save(const VividProducerConfig* config);

gboolean vivid_producer_config_apply_control(VividProducerConfig* config,
                                              guint16               control_opcode,
                                              JsonObject*           payload);

gchar* vivid_producer_config_to_json(const VividProducerConfig* config);
gchar* vivid_producer_config_active_wallpaper_id(const VividProducerConfig* config);
gboolean vivid_producer_config_stop_matcher_contains(const VividProducerConfig* config,
                                                      const gchar*               identifier);

#endif
