#ifndef VIVID_WAYLAND_EGL_H
#define VIVID_WAYLAND_EGL_H

#include "vivid_display_protocol_ids.h"
#include "vivid_wayland_drm.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <stdint.h>

struct json_object;
struct wl_display;
struct wl_egl_window;

typedef struct {
    int fd;
    uint32_t stride;
    uint32_t offset;
} VividWaylandDmaBufPlane;

typedef struct {
    uint32_t fourcc;
    uint64_t modifier;
    int32_t width;
    int32_t height;
    uint32_t n_planes;
    VividWaylandDmaBufPlane planes[VIVID_DISPLAY_DMABUF_MAX_PLANES];
} VividWaylandDmaBufImport;

typedef struct {
    double x;
    double y;
    double w;
    double h;
} VividWaylandRect;

typedef struct {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_window_surface;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_dmabuf_formats;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dmabuf_modifiers;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;
    /* EGL_ANDROID_native_fence_sync is exposed by the display, so a fence
     * inserted after the shadow draw can be exported as a sync_file. */
    bool native_fence_export;
    bool native_fence_export_logged;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
    GLuint program;
    GLint loc_pos;
    GLint loc_uv;
    GLint loc_tex;
    bool ready;
    VividWaylandGpuIdentity identity;
} VividWaylandEgl;

bool vivid_wayland_egl_init(VividWaylandEgl* egl, struct wl_display* display);
void vivid_wayland_egl_finish(VividWaylandEgl* egl);
EGLSurface vivid_wayland_egl_create_window_surface(VividWaylandEgl* egl,
                                                   struct wl_egl_window* window);
bool vivid_wayland_egl_make_current(VividWaylandEgl* egl, EGLSurface surface);
void vivid_wayland_egl_disable_compositor_swap_throttle(VividWaylandEgl* egl,
                                                        EGLSurface surface);
struct json_object* vivid_wayland_egl_build_dmabuf_caps(VividWaylandEgl* egl);
bool vivid_wayland_egl_wait_acquire(VividWaylandEgl* egl, int* sync_fd, const char* context);
EGLImageKHR vivid_wayland_egl_import_image(VividWaylandEgl* egl,
                                           const VividWaylandDmaBufImport* import,
                                           char* error,
                                           size_t error_size);
GLuint vivid_wayland_egl_create_texture(VividWaylandEgl* egl,
                                        EGLImageKHR image,
                                        char* error,
                                        size_t error_size);
void vivid_wayland_egl_destroy_image(VividWaylandEgl* egl, EGLImageKHR* image);
/*
 * Records the draw of the producer texture into the current surface. The GL
 * commands are only queued; call vivid_wayland_egl_export_draw_fence() to
 * obtain a fence for their completion, or vivid_wayland_egl_wait_draw_idle()
 * to block until they have executed.
 */
bool vivid_wayland_egl_draw_frame(VividWaylandEgl* egl,
                                  GLuint texture,
                                  int surface_w,
                                  int surface_h,
                                  int buffer_w,
                                  int buffer_h,
                                  const VividWaylandRect* source,
                                  const VividWaylandRect* dest,
                                  const float clear_color[4]);
/*
 * Inserts a fence after everything queued on the current context and exports
 * it as a sync_file. Returns the fd (caller closes it) or -1 when native fence
 * export is unavailable or failed.
 */
int vivid_wayland_egl_export_draw_fence(VividWaylandEgl* egl, const char* context);
/* CPU wait until every queued GL command on the current context has executed. */
void vivid_wayland_egl_wait_draw_idle(VividWaylandEgl* egl);

#endif
