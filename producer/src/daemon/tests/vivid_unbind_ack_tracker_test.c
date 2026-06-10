/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "../vivid_unbind_ack_tracker.h"

#include <assert.h>

static void
test_ack_matches_and_clears_pending(void)
{
    VividUnbindAckTracker tracker;
    vivid_unbind_ack_tracker_init(&tracker);

    assert(vivid_unbind_ack_tracker_register(&tracker, 7, 11, 1000));
    assert(vivid_unbind_ack_tracker_count(&tracker) == 1);
    assert(vivid_unbind_ack_tracker_contains(&tracker, 7, 11));
    assert(vivid_unbind_ack_tracker_age_usec(&tracker, 7, 11, 2500) == 1500);

    assert(vivid_unbind_ack_tracker_ack(&tracker, 7, 11) ==
           VIVID_UNBIND_ACK_RESULT_MATCHED);
    assert(!vivid_unbind_ack_tracker_contains(&tracker, 7, 11));
    assert(vivid_unbind_ack_tracker_count(&tracker) == 0);

    vivid_unbind_ack_tracker_clear(&tracker);
}

static void
test_unknown_ack_does_not_mutate_state(void)
{
    VividUnbindAckTracker tracker;
    vivid_unbind_ack_tracker_init(&tracker);

    assert(vivid_unbind_ack_tracker_register(&tracker, 1, 2, 1000));
    assert(vivid_unbind_ack_tracker_ack(&tracker, 1, 3) ==
           VIVID_UNBIND_ACK_RESULT_STALE_OR_UNKNOWN);
    assert(vivid_unbind_ack_tracker_count(&tracker) == 1);
    assert(vivid_unbind_ack_tracker_contains(&tracker, 1, 2));

    vivid_unbind_ack_tracker_clear(&tracker);
}

static void
test_duplicate_register_refreshes_timestamp(void)
{
    VividUnbindAckTracker tracker;
    vivid_unbind_ack_tracker_init(&tracker);

    assert(vivid_unbind_ack_tracker_register(&tracker, 3, 5, 1000));
    assert(!vivid_unbind_ack_tracker_register(&tracker, 3, 5, 4000));
    assert(vivid_unbind_ack_tracker_count(&tracker) == 1);
    assert(vivid_unbind_ack_tracker_age_usec(&tracker, 3, 5, 4500) == 500);

    vivid_unbind_ack_tracker_clear(&tracker);
}

static void
test_timeout_forget_removes_pending(void)
{
    VividUnbindAckTracker tracker;
    vivid_unbind_ack_tracker_init(&tracker);

    assert(vivid_unbind_ack_tracker_register(&tracker, 9, 10, 1000));
    assert(vivid_unbind_ack_tracker_forget(&tracker, 9, 10));
    assert(!vivid_unbind_ack_tracker_forget(&tracker, 9, 10));
    assert(vivid_unbind_ack_tracker_count(&tracker) == 0);
    assert(vivid_unbind_ack_tracker_ack(&tracker, 9, 10) ==
           VIVID_UNBIND_ACK_RESULT_STALE_OR_UNKNOWN);

    vivid_unbind_ack_tracker_clear(&tracker);
}

int
main(void)
{
    test_ack_matches_and_clears_pending();
    test_unknown_ack_does_not_mutate_state();
    test_duplicate_register_refreshes_timestamp();
    test_timeout_forget_removes_pending();
    return 0;
}
