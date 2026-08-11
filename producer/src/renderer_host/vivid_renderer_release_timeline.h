#ifndef VIVID_RENDERER_RELEASE_TIMELINE_H
#define VIVID_RENDERER_RELEASE_TIMELINE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
    gint drm_fd;
    guint32 handle;
    guint64 next_point;
    gboolean export_taken;
    GMutex lock;
} VividRendererReleaseTimeline;

/*
 * Initialize an unsignaled DRM timeline syncobj on drm_fd. The render-node FD
 * remains borrowed and must outlive the timeline.
 */
gboolean vivid_renderer_release_timeline_init(
    VividRendererReleaseTimeline* timeline,
    gint drm_fd,
    GError** error);

/*
 * Export the timeline exactly once. On success, out_fd owns a CLOEXEC syncobj
 * descriptor intended to move into RELEASE_TIMELINE's SCM_RIGHTS transaction.
 */
gboolean vivid_renderer_release_timeline_take_export_fd(
    VividRendererReleaseTimeline* timeline,
    gint* out_fd,
    GError** error);

/* Allocate the next non-zero, monotonically increasing release point. */
guint64 vivid_renderer_release_timeline_allocate_point(
    VividRendererReleaseTimeline* timeline);

/* Wait before reusing the buffer whose most recent frame owns release_point. */
gboolean vivid_renderer_release_timeline_wait(
    VividRendererReleaseTimeline* timeline,
    guint64 release_point,
    guint timeout_ms,
    GError** error);

/* Complete a point locally when its frame could not be published. */
gboolean vivid_renderer_release_timeline_signal(
    VividRendererReleaseTimeline* timeline,
    guint64 release_point,
    GError** error);

void vivid_renderer_release_timeline_clear(
    VividRendererReleaseTimeline* timeline);

G_END_DECLS

#endif
