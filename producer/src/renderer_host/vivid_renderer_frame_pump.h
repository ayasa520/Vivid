#ifndef VIVID_RENDERER_FRAME_PUMP_H
#define VIVID_RENDERER_FRAME_PUMP_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Wakeup gate for a renderer worker's frame-relay thread.
 *
 * The relay thread used to run a fixed-rate poll loop: sleep 1/fps, call the
 * backend's next_frame(), publish if a frame appeared. That burns wakeups
 * while the wallpaper is paused and adds up to one frame interval of phase
 * error between "backend produced a frame" and "worker published it".
 *
 * The pump replaces the sleep. Producers signal it from their own threads
 * (GStreamer streaming thread, CEF UI thread, the scene render thread)
 * whenever a frame may be available, and the worker blocks on wait():
 *
 *   - stopping            -> wait() returns FALSE, the thread exits.
 *   - frame event pending -> wait() returns TRUE as soon as the fps interval
 *                            since the previous dispatch has elapsed.
 *   - playing, no event   -> wait() still returns TRUE at the fps cadence.
 *                            This keeps the historical poll as a safety net:
 *                            a lost producer notification degrades to the old
 *                            behavior instead of freezing the wallpaper.
 *   - paused, no event    -> wait() blocks indefinitely: zero wakeups.
 *                            One-shot frames rendered while paused (rebind
 *                            preroll, reconfigure) must notify the pump.
 *
 * Events are a coalesced "there may be a new frame" hint, not a queue: the
 * backends internally keep only the latest frame, so multiple notifications
 * between dispatches are equivalent to one.
 */
typedef struct
{
    GMutex lock;
    GCond cond;
    gboolean stopping;
    gboolean playing;
    gboolean event_pending;
    gint64 last_dispatch_usec;
} VividRendererFramePump;

void vivid_renderer_frame_pump_init(VividRendererFramePump* pump,
                                    gboolean playing);
void vivid_renderer_frame_pump_clear(VividRendererFramePump* pump);

/* Safe from any thread; coalesces into a single pending event. */
void vivid_renderer_frame_pump_notify(VividRendererFramePump* pump);

void vivid_renderer_frame_pump_set_playing(VividRendererFramePump* pump,
                                           gboolean playing);

/*
 * Re-arm the pump for a (re)started relay thread: clears stopping, seeds one
 * event so the first loop iteration attempts a frame immediately, and resets
 * the dispatch clock.
 */
void vivid_renderer_frame_pump_start(VividRendererFramePump* pump);

/* Wakes every waiter; wait() returns FALSE from now on until start(). */
void vivid_renderer_frame_pump_stop(VividRendererFramePump* pump);

/*
 * Block until the next frame attempt should run. min_interval_usec is the
 * fps rate cap measured between successive TRUE returns. Returns FALSE when
 * the pump is stopping.
 */
gboolean vivid_renderer_frame_pump_wait(VividRendererFramePump* pump,
                                        gint64 min_interval_usec);

G_END_DECLS

#endif
