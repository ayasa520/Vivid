#include "vivid_renderer_frame_pump.h"

void
vivid_renderer_frame_pump_init(VividRendererFramePump* pump, gboolean playing)
{
    g_return_if_fail(pump != NULL);

    g_mutex_init(&pump->lock);
    g_cond_init(&pump->cond);
    pump->stopping = FALSE;
    pump->playing = !!playing;
    pump->event_pending = FALSE;
    pump->last_dispatch_usec = 0;
}

void
vivid_renderer_frame_pump_clear(VividRendererFramePump* pump)
{
    if (!pump)
        return;
    g_cond_clear(&pump->cond);
    g_mutex_clear(&pump->lock);
}

void
vivid_renderer_frame_pump_notify(VividRendererFramePump* pump)
{
    g_return_if_fail(pump != NULL);

    g_mutex_lock(&pump->lock);
    if (!pump->event_pending) {
        pump->event_pending = TRUE;
        g_cond_broadcast(&pump->cond);
    }
    g_mutex_unlock(&pump->lock);
}

void
vivid_renderer_frame_pump_set_playing(VividRendererFramePump* pump,
                                      gboolean playing)
{
    g_return_if_fail(pump != NULL);

    g_mutex_lock(&pump->lock);
    pump->playing = !!playing;
    g_cond_broadcast(&pump->cond);
    g_mutex_unlock(&pump->lock);
}

void
vivid_renderer_frame_pump_start(VividRendererFramePump* pump)
{
    g_return_if_fail(pump != NULL);

    g_mutex_lock(&pump->lock);
    pump->stopping = FALSE;
    pump->event_pending = TRUE;
    pump->last_dispatch_usec = 0;
    g_cond_broadcast(&pump->cond);
    g_mutex_unlock(&pump->lock);
}

void
vivid_renderer_frame_pump_stop(VividRendererFramePump* pump)
{
    g_return_if_fail(pump != NULL);

    g_mutex_lock(&pump->lock);
    pump->stopping = TRUE;
    g_cond_broadcast(&pump->cond);
    g_mutex_unlock(&pump->lock);
}

gboolean
vivid_renderer_frame_pump_wait(VividRendererFramePump* pump,
                               gint64 min_interval_usec)
{
    g_return_val_if_fail(pump != NULL, FALSE);

    if (min_interval_usec < 0)
        min_interval_usec = 0;

    g_mutex_lock(&pump->lock);
    for (;;) {
        if (pump->stopping) {
            g_mutex_unlock(&pump->lock);
            return FALSE;
        }

        const gint64 now = g_get_monotonic_time();
        const gint64 next_allowed = pump->last_dispatch_usec + min_interval_usec;

        if (pump->event_pending || pump->playing) {
            if (now >= next_allowed) {
                pump->event_pending = FALSE;
                pump->last_dispatch_usec = now;
                g_mutex_unlock(&pump->lock);
                return TRUE;
            }
            g_cond_wait_until(&pump->cond, &pump->lock, next_allowed);
        } else {
            /* Paused with no pending frame: sleep until someone notifies. */
            g_cond_wait(&pump->cond, &pump->lock);
        }
    }
}
