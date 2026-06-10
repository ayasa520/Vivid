/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_UNBIND_ACK_TRACKER_H
#define VIVID_UNBIND_ACK_TRACKER_H

#include <glib.h>

typedef struct
{
    guint32 output_id;
    guint64 generation;
    gint64  created_usec;
} VividPendingUnbindAck;

typedef struct
{
    GArray* pending;
} VividUnbindAckTracker;

typedef enum
{
    VIVID_UNBIND_ACK_RESULT_MATCHED,
    VIVID_UNBIND_ACK_RESULT_STALE_OR_UNKNOWN,
} VividUnbindAckResult;

void vivid_unbind_ack_tracker_init(VividUnbindAckTracker* tracker);
void vivid_unbind_ack_tracker_clear(VividUnbindAckTracker* tracker);
gboolean vivid_unbind_ack_tracker_register(VividUnbindAckTracker* tracker,
                                           guint32                 output_id,
                                           guint64                 generation,
                                           gint64                  now_usec);
VividUnbindAckResult vivid_unbind_ack_tracker_ack(VividUnbindAckTracker* tracker,
                                                  guint32                 output_id,
                                                  guint64                 generation);
gboolean vivid_unbind_ack_tracker_forget(VividUnbindAckTracker* tracker,
                                         guint32                 output_id,
                                         guint64                 generation);
gboolean vivid_unbind_ack_tracker_contains(const VividUnbindAckTracker* tracker,
                                           guint32                       output_id,
                                           guint64                       generation);
guint vivid_unbind_ack_tracker_count(const VividUnbindAckTracker* tracker);
gint64 vivid_unbind_ack_tracker_age_usec(const VividUnbindAckTracker* tracker,
                                         guint32                       output_id,
                                         guint64                       generation,
                                         gint64                        now_usec);

#endif
