/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_unbind_ack_tracker.h"

static gint
find_pending_ack(const VividUnbindAckTracker* tracker,
                 guint32                       output_id,
                 guint64                       generation)
{
    if (!tracker || !tracker->pending)
        return -1;

    for (guint i = 0; i < tracker->pending->len; i++) {
        const VividPendingUnbindAck* ack =
            &g_array_index(tracker->pending, VividPendingUnbindAck, i);
        if (ack->output_id == output_id && ack->generation == generation)
            return (gint)i;
    }
    return -1;
}

void
vivid_unbind_ack_tracker_init(VividUnbindAckTracker* tracker)
{
    if (!tracker)
        return;

    tracker->pending = g_array_new(FALSE, FALSE, sizeof(VividPendingUnbindAck));
}

void
vivid_unbind_ack_tracker_clear(VividUnbindAckTracker* tracker)
{
    if (!tracker)
        return;

    g_clear_pointer(&tracker->pending, g_array_unref);
}

gboolean
vivid_unbind_ack_tracker_register(VividUnbindAckTracker* tracker,
                                  guint32                 output_id,
                                  guint64                 generation,
                                  gint64                  now_usec)
{
    if (!tracker)
        return FALSE;
    if (!tracker->pending)
        vivid_unbind_ack_tracker_init(tracker);

    const gint existing = find_pending_ack(tracker, output_id, generation);
    if (existing >= 0) {
        VividPendingUnbindAck* ack =
            &g_array_index(tracker->pending, VividPendingUnbindAck, (guint)existing);
        ack->created_usec = now_usec;
        return FALSE;
    }

    const VividPendingUnbindAck ack = {
        .output_id = output_id,
        .generation = generation,
        .created_usec = now_usec,
    };
    g_array_append_val(tracker->pending, ack);
    return TRUE;
}

VividUnbindAckResult
vivid_unbind_ack_tracker_ack(VividUnbindAckTracker* tracker,
                             guint32                 output_id,
                             guint64                 generation)
{
    const gint index = find_pending_ack(tracker, output_id, generation);
    if (index < 0)
        return VIVID_UNBIND_ACK_RESULT_STALE_OR_UNKNOWN;

    g_array_remove_index(tracker->pending, (guint)index);
    return VIVID_UNBIND_ACK_RESULT_MATCHED;
}

gboolean
vivid_unbind_ack_tracker_forget(VividUnbindAckTracker* tracker,
                                guint32                 output_id,
                                guint64                 generation)
{
    const gint index = find_pending_ack(tracker, output_id, generation);
    if (index < 0)
        return FALSE;

    g_array_remove_index(tracker->pending, (guint)index);
    return TRUE;
}

gboolean
vivid_unbind_ack_tracker_contains(const VividUnbindAckTracker* tracker,
                                  guint32                       output_id,
                                  guint64                       generation)
{
    return find_pending_ack(tracker, output_id, generation) >= 0;
}

guint
vivid_unbind_ack_tracker_count(const VividUnbindAckTracker* tracker)
{
    return tracker && tracker->pending ? tracker->pending->len : 0;
}

gint64
vivid_unbind_ack_tracker_age_usec(const VividUnbindAckTracker* tracker,
                                  guint32                       output_id,
                                  guint64                       generation,
                                  gint64                        now_usec)
{
    const gint index = find_pending_ack(tracker, output_id, generation);
    if (index < 0)
        return -1;

    const VividPendingUnbindAck* ack =
        &g_array_index(tracker->pending, VividPendingUnbindAck, (guint)index);
    return ack->created_usec > 0 ? now_usec - ack->created_usec : -1;
}
