#ifndef VIVID_WINDOW_FACTS_H
#define VIVID_WINDOW_FACTS_H

#include "../protocol/vivid_display_protocol.h"

#include <glib.h>
#include <json-glib/json-glib.h>

#define VIVID_WINDOW_STATE_FLAG_NON_MINIMIZED VIVID_WINDOW_FLAG_NON_MINIMIZED
#define VIVID_WINDOW_STATE_FLAG_FOCUSED      VIVID_WINDOW_FLAG_FOCUSED
#define VIVID_WINDOW_STATE_FLAG_MAXIMIZED     VIVID_WINDOW_FLAG_MAXIMIZED
#define VIVID_WINDOW_STATE_FLAG_FULLSCREEN    VIVID_WINDOW_FLAG_FULLSCREEN

typedef struct
{
    guint32 output_id;
    guint32 flags;
} VividOutputWindowFacts;

typedef struct
{
    gchar*   name;
    gboolean playing;
} VividMprisPlayerFact;

typedef struct
{
    GPtrArray* identifiers;
    guint32    output_id;
    gboolean   focused;
    gboolean   maximized;
    gboolean   fullscreen;
} VividWindowFact;

typedef struct
{
    gboolean   on_battery;
    gboolean   mpris_playing;
    GPtrArray* mpris_players;
    GPtrArray* windows;
    GPtrArray* application_identifiers;
} VividSessionFacts;

typedef struct
{
    GArray*           outputs;
    VividSessionFacts session;
} VividWindowFacts;

typedef struct
{
    guint32      monitor_index;
    guint32      output_id;
    const gchar* display_key;
} VividOutputIndexEntry;

VividWindowFacts* vivid_window_facts_parse(JsonObject*                  payload,
                                           const VividOutputIndexEntry* entries,
                                           guint                        n_entries);

void vivid_window_facts_free(VividWindowFacts* facts);

void vivid_window_facts_describe(const VividWindowFacts* facts, GString* detail);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VividWindowFacts, vivid_window_facts_free)

#endif
