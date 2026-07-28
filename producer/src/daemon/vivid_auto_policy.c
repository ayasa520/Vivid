#include "vivid_auto_policy.h"

#include <string.h>

static gboolean
identifier_matches_rule_name(const gchar* identifier, const gchar* rule_name)
{
    if (!identifier || !*identifier || !rule_name || !*rule_name)
        return FALSE;

    g_autofree gchar* normalized_identifier = g_utf8_strdown(identifier, -1);
    g_autofree gchar* normalized_rule = g_utf8_strdown(rule_name, -1);
    g_strstrip(normalized_identifier);
    g_strstrip(normalized_rule);
    if (!*normalized_identifier || !*normalized_rule)
        return FALSE;

    return g_strcmp0(normalized_identifier, normalized_rule) == 0;
}

static gboolean
identifier_array_matches_rule_name(GPtrArray* identifiers, const gchar* rule_name)
{
    if (!identifiers || !rule_name || !*rule_name)
        return FALSE;

    for (guint i = 0; i < identifiers->len; i++) {
        const gchar* identifier = g_ptr_array_index(identifiers, i);
        if (identifier_matches_rule_name(identifier, rule_name))
            return TRUE;
    }
    return FALSE;
}

static gboolean
window_fact_matches_rule_name(const VividWindowFact* window, const gchar* rule_name)
{
    if (!window)
        return FALSE;

    return identifier_array_matches_rule_name(window->identifiers, rule_name);
}

VividAutoAction
vivid_playback_action_to_auto_action(VividPlaybackAction action)
{
    switch (action) {
    case VIVID_PLAYBACK_ACTION_MUTE:
        return VIVID_AUTO_ACTION_MUTE;
    case VIVID_PLAYBACK_ACTION_PAUSE_PER_MONITOR:
    case VIVID_PLAYBACK_ACTION_PAUSE_ALL:
        return VIVID_AUTO_ACTION_PAUSE;
    case VIVID_PLAYBACK_ACTION_STOP:
        return VIVID_AUTO_ACTION_STOP;
    case VIVID_PLAYBACK_ACTION_KEEP_RUNNING:
    default:
        return VIVID_AUTO_ACTION_NONE;
    }
}

VividAutoAction
vivid_auto_action_max(VividAutoAction left, VividAutoAction right)
{
    return (VividAutoAction)MAX((gint)left, (gint)right);
}

VividAutoAction
vivid_auto_action_min(VividAutoAction left, VividAutoAction right)
{
    return (VividAutoAction)MIN((gint)left, (gint)right);
}

void
vivid_auto_policy_contribution_clear(VividPolicyContribution* contribution)
{
    if (!contribution)
        return;

    memset(contribution, 0, sizeof(*contribution));
}

void
vivid_auto_policy_apply_playback_action(VividPlaybackAction    action,
                                        VividPolicyContribution* contribution)
{
    if (!contribution)
        return;

    contribution->action = vivid_auto_action_max(
        contribution->action,
        vivid_playback_action_to_auto_action(action));

    switch (action) {
    case VIVID_PLAYBACK_ACTION_MUTE:
        contribution->global_mute = TRUE;
        break;
    case VIVID_PLAYBACK_ACTION_PAUSE_PER_MONITOR:
        contribution->route_pause = TRUE;
        break;
    case VIVID_PLAYBACK_ACTION_PAUSE_ALL:
        contribution->global_pause = TRUE;
        break;
    case VIVID_PLAYBACK_ACTION_STOP:
        contribution->global_stop = TRUE;
        break;
    case VIVID_PLAYBACK_ACTION_KEEP_RUNNING:
    default:
        break;
    }
}

static void
auto_policy_contribute_from_flags(const VividAutoPolicy*     effective,
                                  guint32                    flags,
                                  VividPolicyContribution*   contribution)
{
    if (!effective || !contribution)
        return;

    if ((flags & VIVID_WINDOW_STATE_FLAG_MAXIMIZED) ||
        (flags & VIVID_WINDOW_STATE_FLAG_FULLSCREEN)) {
        vivid_auto_policy_apply_playback_action(
            effective->playback_on_maximize_or_fullscreen,
            contribution);
    }

    if (flags & VIVID_WINDOW_STATE_FLAG_FOCUSED) {
        vivid_auto_policy_apply_playback_action(effective->playback_on_focus,
                                                contribution);
    }
}

static void
auto_policy_contribute_session_overlay(const VividAutoPolicy*   effective,
                                       const VividSessionFacts* session,
                                       VividPolicyContribution* contribution)
{
    if (!effective || !session || !contribution)
        return;

    if (session->on_battery) {
        vivid_auto_policy_apply_playback_action(effective->playback_on_battery,
                                                contribution);
    }

    if (session->mpris_playing) {
        vivid_auto_policy_apply_playback_action(effective->playback_on_audio,
                                                contribution);
    }
}

static void
auto_policy_app_rule_contribute(const VividAutoPolicy*              effective,
                                const VividSessionFacts*            session,
                                const VividApplicationPlaybackRule* rule,
                                const VividOutputWindowFacts*       output_facts,
                                VividPolicyContribution*            contribution)
{
    if (!effective || !session || !rule || !rule->name || !*rule->name || !contribution)
        return;

    switch (rule->condition) {
    case VIVID_APPLICATION_RULE_CONDITION_RUNNING:
        if (identifier_array_matches_rule_name(session->application_identifiers,
                                               rule->name)) {
            vivid_auto_policy_apply_playback_action(rule->playback, contribution);
        }
        return;

    case VIVID_APPLICATION_RULE_CONDITION_FOCUSED:
    {
        if (!output_facts)
            return;

        for (guint i = 0; session->windows && i < session->windows->len; i++) {
            const VividWindowFact* window =
                g_ptr_array_index(session->windows, i);
            if (!window || !window->focused ||
                !window_fact_matches_rule_name(window, rule->name))
                continue;
            if (window->output_id != output_facts->output_id)
                continue;
            vivid_auto_policy_apply_playback_action(rule->playback, contribution);
            return;
        }
        return;
    }

    case VIVID_APPLICATION_RULE_CONDITION_PLAYING_AUDIO:
    {
        for (guint i = 0; session->mpris_players && i < session->mpris_players->len;
             i++) {
            const VividMprisPlayerFact* player =
                g_ptr_array_index(session->mpris_players, i);
            if (!player || !player->playing)
                continue;
            if (identifier_matches_rule_name(player->name, rule->name))
                vivid_auto_policy_apply_playback_action(rule->playback, contribution);
        }
        return;
    }

    case VIVID_APPLICATION_RULE_CONDITION_MAXIMIZED:
    case VIVID_APPLICATION_RULE_CONDITION_FULLSCREEN:
        break;

    default:
        return;
    }

    if (!output_facts)
        return;

    for (guint i = 0; session->windows && i < session->windows->len; i++) {
        const VividWindowFact* window = g_ptr_array_index(session->windows, i);
        if (!window || !window_fact_matches_rule_name(window, rule->name))
            continue;

        const gboolean matched_state =
            (rule->condition == VIVID_APPLICATION_RULE_CONDITION_MAXIMIZED &&
             window->maximized) ||
            (rule->condition == VIVID_APPLICATION_RULE_CONDITION_FULLSCREEN &&
             window->fullscreen);
        if (!matched_state)
            continue;

        if (window->output_id != output_facts->output_id)
            continue;

        vivid_auto_policy_apply_playback_action(rule->playback, contribution);
    }
}

static void
auto_policy_contribute_app_rules(const VividAutoPolicy*        effective,
                                 const GPtrArray*              app_rules,
                                 const VividOutputWindowFacts* output_facts,
                                 const VividSessionFacts*      session,
                                 VividPolicyContribution*    contribution)
{
    if (!effective || !session || !contribution || !app_rules)
        return;

    for (guint i = 0; i < app_rules->len; i++) {
        const VividApplicationPlaybackRule* rule = g_ptr_array_index(app_rules, i);
        auto_policy_app_rule_contribute(effective,
                                        session,
                                        rule,
                                        output_facts,
                                        contribution);
    }
}

VividPolicyContribution
vivid_auto_policy_evaluate(const VividAutoPolicy*        effective,
                           const GPtrArray*              app_rules,
                           const VividOutputWindowFacts* output_facts,
                           const VividSessionFacts*      session)
{
    VividPolicyContribution contribution;
    vivid_auto_policy_contribution_clear(&contribution);

    if (!effective || !output_facts)
        return contribution;

    auto_policy_contribute_from_flags(effective,
                                      output_facts->flags,
                                      &contribution);
    if (session)
        auto_policy_contribute_session_overlay(effective, session, &contribution);
    auto_policy_contribute_app_rules(effective,
                                     app_rules,
                                     output_facts,
                                     session,
                                     &contribution);

    return contribution;
}

void
vivid_auto_replay_state_init(VividAutoReplayState* state)
{
    if (!state)
        return;

    memset(state, 0, sizeof(*state));
}

void
vivid_auto_replay_state_clear(VividAutoReplayState* state)
{
    if (!state)
        return;

    if (state->resume_source_id) {
        g_source_remove(state->resume_source_id);
        state->resume_source_id = 0;
    }

    memset(state, 0, sizeof(*state));
}
