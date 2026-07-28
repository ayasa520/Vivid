#ifndef VIVID_AUTO_POLICY_H
#define VIVID_AUTO_POLICY_H

#include "vivid_producer_config.h"
#include "vivid_window_facts.h"

#include <glib.h>

#define VIVID_AUTO_POLICY_RESUME_DELAY_MSEC 500

typedef enum
{
    VIVID_AUTO_ACTION_NONE = 0,
    VIVID_AUTO_ACTION_MUTE = 1,
    VIVID_AUTO_ACTION_PAUSE = 2,
    VIVID_AUTO_ACTION_STOP = 3,
} VividAutoAction;

typedef struct
{
    guint32         last_flags;
    VividAutoAction raw;
    VividAutoAction requested;
    guint32         gen;
    gboolean        stop_applied;
    guint           resume_source_id;
} VividAutoReplayState;

typedef struct
{
    VividAutoAction action;
    gboolean        global_mute;
    gboolean        global_pause;
    gboolean        route_pause;
    gboolean        global_stop;
} VividPolicyContribution;

VividAutoAction vivid_playback_action_to_auto_action(VividPlaybackAction action);
VividAutoAction vivid_auto_action_max(VividAutoAction left, VividAutoAction right);
VividAutoAction vivid_auto_action_min(VividAutoAction left, VividAutoAction right);

void vivid_auto_policy_contribution_clear(VividPolicyContribution* contribution);
void vivid_auto_policy_apply_playback_action(VividPlaybackAction   action,
                                             VividPolicyContribution* contribution);

VividPolicyContribution vivid_auto_policy_evaluate(
    const VividAutoPolicy*        effective,
    const GPtrArray*              app_rules,
    const VividOutputWindowFacts* output_facts,
    const VividSessionFacts*      session);

void vivid_auto_replay_state_init(VividAutoReplayState* state);
void vivid_auto_replay_state_clear(VividAutoReplayState* state);

#endif
