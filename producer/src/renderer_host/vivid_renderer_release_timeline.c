#include "vivid_renderer_release_timeline.h"

#include <gio/gio.h>
#include <xf86drm.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static gint
drm_error(gint result)
{
    if (result == 0)
        return 0;
    if (errno != 0)
        return errno;
    return result < 0 ? -result : result;
}

static gint64
wait_deadline_nsec(guint timeout_ms)
{
    return ((gint64)g_get_monotonic_time() * 1000) +
        ((gint64)timeout_ms * G_TIME_SPAN_MILLISECOND * 1000);
}

gboolean
vivid_renderer_release_timeline_init(VividRendererReleaseTimeline* timeline,
                                     gint drm_fd,
                                     GError** error)
{
    if (!timeline || drm_fd < 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "release timeline requires an open DRM render-node FD");
        return FALSE;
    }
    memset(timeline, 0, sizeof(*timeline));
    timeline->drm_fd = drm_fd;
    g_mutex_init(&timeline->lock);

    errno = 0;
    const gint result = drmSyncobjCreate(drm_fd, 0, &timeline->handle);
    if (result != 0) {
        const gint saved_error = drm_error(result);
        timeline->drm_fd = -1;
        g_mutex_clear(&timeline->lock);
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_error),
                    "failed to create worker release timeline: %s",
                    g_strerror(saved_error));
        return FALSE;
    }
    return TRUE;
}

gboolean
vivid_renderer_release_timeline_take_export_fd(
    VividRendererReleaseTimeline* timeline,
    gint* out_fd,
    GError** error)
{
    if (!out_fd)
        return FALSE;
    *out_fd = -1;
    if (!timeline || timeline->drm_fd < 0 || timeline->handle == 0 ||
        timeline->export_taken) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "release timeline export is unavailable or was already taken");
        return FALSE;
    }

    errno = 0;
    const gint result = drmSyncobjHandleToFD(timeline->drm_fd,
                                             timeline->handle,
                                             out_fd);
    if (result != 0 || *out_fd < 0) {
        const gint saved_error = drm_error(result);
        if (*out_fd >= 0) {
            close(*out_fd);
            *out_fd = -1;
        }
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_error),
                    "failed to export worker release timeline: %s",
                    g_strerror(saved_error));
        return FALSE;
    }
    const gint old_flags = fcntl(*out_fd, F_GETFD);
    if (old_flags < 0 || fcntl(*out_fd, F_SETFD, old_flags | FD_CLOEXEC) < 0) {
        const gint saved_error = errno;
        close(*out_fd);
        *out_fd = -1;
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_error),
                    "failed to mark worker release timeline FD CLOEXEC: %s",
                    g_strerror(saved_error));
        return FALSE;
    }
    timeline->export_taken = TRUE;
    return TRUE;
}

guint64
vivid_renderer_release_timeline_allocate_point(
    VividRendererReleaseTimeline* timeline)
{
    if (!timeline || timeline->drm_fd < 0 || timeline->handle == 0)
        return 0;
    g_mutex_lock(&timeline->lock);
    if (timeline->next_point != G_MAXUINT64) {
        timeline->next_point++;
        const guint64 point = timeline->next_point;
        g_mutex_unlock(&timeline->lock);
        return point;
    }
    g_mutex_unlock(&timeline->lock);
    return 0;
}

gboolean
vivid_renderer_release_timeline_wait(VividRendererReleaseTimeline* timeline,
                                     guint64 release_point,
                                     guint timeout_ms,
                                     GError** error)
{
    if (release_point == 0)
        return TRUE;
    if (!timeline || timeline->drm_fd < 0 || timeline->handle == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "release timeline is not initialized");
        return FALSE;
    }
    guint32 handle = timeline->handle;
    guint64 point = release_point;
    guint32 first_signaled = 0;
    errno = 0;
    const gint result = drmSyncobjTimelineWait(
        timeline->drm_fd,
        &handle,
        &point,
        1,
        wait_deadline_nsec(timeout_ms),
        DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
        &first_signaled);
    if (result == 0)
        return TRUE;
    const gint saved_error = drm_error(result);
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_error),
                "worker release point %" G_GUINT64_FORMAT
                " did not complete within %u ms: %s",
                release_point,
                timeout_ms,
                g_strerror(saved_error));
    return FALSE;
}

gboolean
vivid_renderer_release_timeline_signal(VividRendererReleaseTimeline* timeline,
                                       guint64 release_point,
                                       GError** error)
{
    if (!timeline || timeline->drm_fd < 0 || timeline->handle == 0 ||
        release_point == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "release timeline signal requires an initialized non-zero point");
        return FALSE;
    }
    guint32 handle = timeline->handle;
    guint64 point = release_point;
    errno = 0;
    const gint result = drmSyncobjTimelineSignal(timeline->drm_fd,
                                                 &handle,
                                                 &point,
                                                 1);
    if (result == 0)
        return TRUE;
    const gint saved_error = drm_error(result);
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_error),
                "failed to signal local worker release point %" G_GUINT64_FORMAT ": %s",
                release_point,
                g_strerror(saved_error));
    return FALSE;
}

void
vivid_renderer_release_timeline_clear(VividRendererReleaseTimeline* timeline)
{
    if (!timeline)
        return;
    if (timeline->drm_fd >= 0 && timeline->handle != 0) {
        errno = 0;
        const gint result = drmSyncobjDestroy(timeline->drm_fd, timeline->handle);
        if (result != 0) {
            g_warning("VividRendererReleaseTimeline: destroy handle=%u failed: %s",
                      timeline->handle,
                      g_strerror(drm_error(result)));
        }
    }
    timeline->drm_fd = -1;
    timeline->handle = 0;
    timeline->next_point = 0;
    timeline->export_taken = FALSE;
    g_mutex_clear(&timeline->lock);
}
