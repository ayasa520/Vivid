#define _GNU_SOURCE

#include "vivid_wayland_egl.h"

#include "vivid_wayland_drm_fourcc.h"
#include "vivid_wayland_log.h"
#include "vivid_wayland_util.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-egl.h>

#ifdef VIVID_WAYLAND_HAVE_GBM
#include <gbm.h>
#endif

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT 0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT 0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT 0x327A
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_FD_EXT 0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT 0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT 0x3442
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
#endif

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#endif
#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif

static const char* k_vert_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char* k_frag_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static bool
has_ext(const char* haystack, const char* needle)
{
    if (!haystack || !needle)
        return false;
    size_t nlen = strlen(needle);
    const char* p = haystack;
    while (*p) {
        while (*p == ' ')
            p++;
        if (strncmp(p, needle, nlen) == 0 && (p[nlen] == '\0' || p[nlen] == ' '))
            return true;
        while (*p && *p != ' ')
            p++;
    }
    return false;
}

static GLuint
compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        vivid_wayland_error("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint
link_program(void)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_frag_src);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        vivid_wayland_error("program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void
fill_gpu_identity(VividWaylandEgl* egl)
{
    memset(&egl->identity, 0, sizeof(egl->identity));
    PFNEGLQUERYDISPLAYATTRIBEXTPROC query_display =
        (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device =
        (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (query_display && query_device) {
        EGLAttrib device_attr = 0;
        if (query_display(egl->display, EGL_DEVICE_EXT, &device_attr) && device_attr != 0) {
            EGLDeviceEXT device = (EGLDeviceEXT)device_attr;
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
            const char* render = query_device(device, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (render)
                vivid_wayland_strlcpy(egl->identity.render_node,
                                      render,
                                      sizeof(egl->identity.render_node));
#endif
#ifdef EGL_DRM_DEVICE_FILE_EXT
            if (!egl->identity.render_node[0]) {
                const char* drm = query_device(device, EGL_DRM_DEVICE_FILE_EXT);
                if (drm)
                    vivid_wayland_strlcpy(egl->identity.render_node,
                                          drm,
                                          sizeof(egl->identity.render_node));
            }
#endif
        } else {
            vivid_wayland_append_diag(&egl->identity,
                                      " eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed;");
        }
    } else {
        vivid_wayland_append_diag(&egl->identity, " EGL device query functions unavailable;");
    }

    vivid_wayland_gpu_identity_from_render_node(&egl->identity);
    vivid_wayland_gpu_identity_probe_vulkan_uuid(&egl->identity);

    if (!egl->identity.vendor[0]) {
        const char* vendor = (const char*)glGetString(GL_VENDOR);
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* text = vendor ? vendor : renderer;
        if (text) {
            if (strcasestr(text, "nvidia"))
                vivid_wayland_strlcpy(egl->identity.vendor, "nvidia", sizeof(egl->identity.vendor));
            else if (strcasestr(text, "intel"))
                vivid_wayland_strlcpy(egl->identity.vendor, "intel", sizeof(egl->identity.vendor));
            else if (strcasestr(text, "amd") || strcasestr(text, "radeon") ||
                     strcasestr(text, "ati"))
                vivid_wayland_strlcpy(egl->identity.vendor, "amd", sizeof(egl->identity.vendor));
        }
    }
}

bool
vivid_wayland_egl_init(VividWaylandEgl* egl, struct wl_display* display)
{
    memset(egl, 0, sizeof(*egl));
    egl->display = EGL_NO_DISPLAY;
    egl->context = EGL_NO_CONTEXT;
    egl->create_window_surface =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress(
            "eglCreatePlatformWindowSurfaceEXT");
    egl->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    egl->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    egl->query_dmabuf_formats =
        (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    egl->query_dmabuf_modifiers =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    egl->create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    egl->wait_sync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    egl->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    egl->dup_native_fence_fd =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");

    if (!display) {
        vivid_wayland_error("missing Wayland display for EGL");
        return false;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform)
        egl->display = get_platform(EGL_PLATFORM_WAYLAND_EXT, display, NULL);
    else
        egl->display = eglGetDisplay((EGLNativeDisplayType)display);

    if (egl->display == EGL_NO_DISPLAY) {
        vivid_wayland_error("eglGetDisplay failed");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl->display, &major, &minor)) {
        vivid_wayland_error("eglInitialize failed");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        vivid_wayland_error("eglBindAPI(OpenGL ES) failed");
        return false;
    }

    const char* display_extensions = eglQueryString(egl->display, EGL_EXTENSIONS);
    egl->native_fence_export = egl->create_sync && egl->destroy_sync &&
        egl->dup_native_fence_fd && has_ext(display_extensions, "EGL_ANDROID_native_fence_sync");

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLint n_configs = 0;
    if (!eglChooseConfig(egl->display, config_attrs, &egl->config, 1, &n_configs) ||
        n_configs < 1) {
        vivid_wayland_error("eglChooseConfig failed");
        return false;
    }

    const EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    egl->context = eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, context_attrs);
    if (egl->context == EGL_NO_CONTEXT) {
        vivid_wayland_error("eglCreateContext failed egl=0x%x", eglGetError());
        return false;
    }

    /* Surfaceless current context is enough to compile shaders and probe caps. */
    if (!eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context)) {
        vivid_wayland_warn("surfaceless eglMakeCurrent failed egl=0x%x; caps probe will retry after the first window surface",
                           eglGetError());
    } else {
        egl->program = link_program();
        egl->image_target_texture =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        fill_gpu_identity(egl);
        if (egl->program) {
            egl->loc_pos = 0;
            egl->loc_uv = 1;
            egl->loc_tex = glGetUniformLocation(egl->program, "u_tex");
        }
    }

    egl->ready = true;
    vivid_wayland_log("EGL ready version=%d.%d render-node=%s vendor=%s",
                      major,
                      minor,
                      egl->identity.render_node[0] ? egl->identity.render_node : "(unknown)",
                      egl->identity.vendor[0] ? egl->identity.vendor : "(unknown)");
    return true;
}

void
vivid_wayland_egl_finish(VividWaylandEgl* egl)
{
    if (!egl)
        return;
    if (egl->program) {
        if (egl->display != EGL_NO_DISPLAY && egl->context != EGL_NO_CONTEXT)
            eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context);
        glDeleteProgram(egl->program);
        egl->program = 0;
    }
    if (egl->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl->context != EGL_NO_CONTEXT)
            eglDestroyContext(egl->display, egl->context);
        eglTerminate(egl->display);
    }
    memset(egl, 0, sizeof(*egl));
}

EGLSurface
vivid_wayland_egl_create_window_surface(VividWaylandEgl* egl, struct wl_egl_window* window)
{
    if (!egl || !window || egl->display == EGL_NO_DISPLAY)
        return EGL_NO_SURFACE;
    if (egl->create_window_surface)
        return egl->create_window_surface(egl->display, egl->config, window, NULL);
    return eglCreateWindowSurface(egl->display, egl->config, (EGLNativeWindowType)window, NULL);
}

bool
vivid_wayland_egl_make_current(VividWaylandEgl* egl, EGLSurface surface)
{
    if (!egl || egl->display == EGL_NO_DISPLAY)
        return false;
    if (!eglMakeCurrent(egl->display, surface, surface, egl->context)) {
        vivid_wayland_warn("eglMakeCurrent failed egl=0x%x", eglGetError());
        return false;
    }
    if (!egl->program) {
        egl->program = link_program();
        egl->image_target_texture =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!egl->identity.render_node[0])
            fill_gpu_identity(egl);
        if (egl->program) {
            egl->loc_pos = 0;
            egl->loc_uv = 1;
            egl->loc_tex = glGetUniformLocation(egl->program, "u_tex");
        }
    }
    return egl->program != 0;
}

void
vivid_wayland_egl_disable_compositor_swap_throttle(VividWaylandEgl* egl, EGLSurface surface)
{
    static bool logged;

    if (!egl || egl->display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE)
        return;
    if (!vivid_wayland_egl_make_current(egl, surface))
        return;
    /*
     * Request immediate swaps so presentation is not intentionally throttled
     * to compositor refresh. EGL may still wait for an available native window
     * buffer; that native-window wait is independent of callback scheduling.
     */
    if (!eglSwapInterval(egl->display, 0) && !logged) {
        logged = true;
        vivid_wayland_warn("eglSwapInterval(0) failed egl=0x%x; compositor frame waits may still block",
                           eglGetError());
        return;
    }
    if (!logged) {
        logged = true;
        vivid_wayland_log("EGL swap interval 0 requested for layer-shell presentation");
    }
}

static void
json_array_add_u32_unique(json_object* array, uint32_t value)
{
    size_t n = json_object_array_length(array);
    for (size_t i = 0; i < n; i++) {
        json_object* existing = json_object_array_get_idx(array, i);
        if (existing && json_object_get_uint64(existing) == value)
            return;
    }
    json_object_array_add(array, json_object_new_int64((int64_t)value));
}

static json_object*
modifier_entry(uint32_t fourcc, uint64_t modifier, uint32_t plane_count)
{
    json_object* obj = json_object_new_object();
    json_object_object_add(obj, "fourcc", json_object_new_int64((int64_t)fourcc));
    char mod[32];
    snprintf(mod, sizeof(mod), "%llu", (unsigned long long)modifier);
    json_object_object_add(obj, "modifier", json_object_new_string(mod));
    json_object_object_add(obj, "planeCount", json_object_new_int64((int64_t)plane_count));
    return obj;
}

static uint32_t
probe_gbm_plane_count(uint32_t fourcc, uint64_t modifier, VividWaylandGpuIdentity* identity)
{
    if (modifier == VIVID_WAYLAND_DRM_FORMAT_MOD_LINEAR)
        return 1;
#ifdef VIVID_WAYLAND_HAVE_GBM
    if (!identity->render_node[0]) {
        vivid_wayland_append_diag(identity,
                                  " cannot prove plane count without an EGL render node;");
        return 0;
    }
    int fd = open(identity->render_node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        vivid_wayland_append_diag(identity,
                                  " cannot open %s for plane-count probing: %s;",
                                  identity->render_node,
                                  strerror(errno));
        return 0;
    }
    struct gbm_device* device = gbm_create_device(fd);
    if (!device) {
        close(fd);
        vivid_wayland_append_diag(identity, " GBM device creation failed for plane probing;");
        return 0;
    }
    uint64_t mods[] = { modifier };
    /*
     * This allocation only verifies the modifier's plane layout; the BO is
     * never rendered into or presented. The original modifiers API therefore
     * expresses the probe exactly and avoids requiring the newer flags-bearing
     * entry point from the host GBM implementation.
     */
    struct gbm_bo* bo = gbm_bo_create_with_modifiers(device, 64, 64, fourcc, mods, 1);
    uint32_t planes = 0;
    if (bo) {
        int count = gbm_bo_get_plane_count(bo);
        if (count > 0 && count <= (int)VIVID_DISPLAY_DMABUF_MAX_PLANES) {
            planes = (uint32_t)count;
        } else if (count > (int)VIVID_DISPLAY_DMABUF_MAX_PLANES) {
            vivid_wayland_append_diag(
                identity,
                " GBM reported %d planes above the display-v1 limit for fourcc=0x%08x "
                "modifier=0x%016llx;",
                count,
                fourcc,
                (unsigned long long)modifier);
        }
        gbm_bo_destroy(bo);
    } else {
        vivid_wayland_append_diag(identity,
                                  " GBM rejected fourcc=0x%08x modifier=0x%016llx;",
                                  fourcc,
                                  (unsigned long long)modifier);
    }
    gbm_device_destroy(device);
    close(fd);
    return planes;
#else
    (void)fourcc;
    (void)modifier;
    vivid_wayland_append_diag(identity,
                              " GBM is required to prove non-LINEAR modifier plane counts;");
    return 0;
#endif
}

static bool
dmabuf_requirements_available(VividWaylandEgl* egl)
{
    if (!egl || egl->display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT ||
        !egl->program) {
        vivid_wayland_error("DMA-BUF capability probing requires a current GLES context");
        return false;
    }

    const char* egl_exts = eglQueryString(egl->display, EGL_EXTENSIONS);
    const char* gl_exts = (const char*)glGetString(GL_EXTENSIONS);
    bool import_ready = has_ext(egl_exts, "EGL_EXT_image_dma_buf_import") &&
        has_ext(egl_exts, "EGL_EXT_image_dma_buf_import_modifiers") && egl->create_image &&
        egl->destroy_image && egl->query_dmabuf_formats && egl->query_dmabuf_modifiers &&
        has_ext(gl_exts, "GL_OES_EGL_image") && egl->image_target_texture;
    bool acquire_ready = has_ext(egl_exts, "EGL_ANDROID_native_fence_sync") &&
        has_ext(egl_exts, "EGL_KHR_wait_sync") && egl->create_sync && egl->wait_sync &&
        egl->destroy_sync;
    bool release_ready = vivid_wayland_release_syncobj_supported(egl->identity.render_node,
                                                                 "consumer-caps");

    if (!import_ready)
        vivid_wayland_error("EGL/GLES does not provide the required DMA-BUF image import path");
    if (!acquire_ready)
        vivid_wayland_error("EGL does not provide the required native-fence acquire path");
    if (!release_ready)
        vivid_wayland_error("DRM render node does not provide the required release syncobj path");
    return import_ready && acquire_ready && release_ready;
}

struct json_object*
vivid_wayland_egl_build_dmabuf_caps(VividWaylandEgl* egl)
{
    /*
     * display-v1 caps are authoritative: every tuple and synchronization mode
     * published here may be selected immediately by the producer. Refuse to
     * publish caps unless the exact import, acquire, and release operations
     * used by the frame path are available in the current EGL/GLES context.
     */
    if (!dmabuf_requirements_available(egl))
        return NULL;

    json_object* caps = json_object_new_object();
    json_object* fourccs = json_object_new_array();
    json_object* modifiers = json_object_new_array();
    json_object* implicit = json_object_new_array();
    json_object* relay = json_object_new_array();
    json_object* memory = json_object_new_array();
    json_object* sync = json_object_new_array();
    json_object* color = json_object_new_array();
    json_object* extent = json_object_new_object();

    json_object_array_add(relay, json_object_new_string("shadow-copy-v1"));
    json_object_array_add(memory, json_object_new_string("host-visible"));
    json_object_array_add(sync, json_object_new_string("implicit"));
    json_object_array_add(sync, json_object_new_string("explicit-sync-fd"));
    json_object_array_add(sync, json_object_new_string("drm-syncobj-release"));
    json_object_array_add(color, json_object_new_string("srgb"));
    json_object_array_add(color, json_object_new_string("limited-range"));
    json_object_array_add(color, json_object_new_string("premultiplied-alpha"));
    json_object_object_add(extent, "width", json_object_new_int(0));
    json_object_object_add(extent, "height", json_object_new_int(0));

    const char* probe = "egl-query";
    EGLint format_count = 0;
    if (egl->query_dmabuf_formats(egl->display, 0, NULL, &format_count) && format_count > 0) {
        EGLint* formats = calloc((size_t)format_count, sizeof(EGLint));
        if (formats &&
            egl->query_dmabuf_formats(egl->display, format_count, formats, &format_count)) {
            for (EGLint i = 0; i < format_count; i++) {
                uint32_t fourcc = (uint32_t)formats[i];
                if (!vivid_wayland_fourcc_supported(fourcc))
                    continue;
                EGLint mod_count = 0;
                if (!egl->query_dmabuf_modifiers(
                        egl->display, formats[i], 0, NULL, NULL, &mod_count))
                    continue;
                if (mod_count <= 0) {
                    json_array_add_u32_unique(fourccs, fourcc);
                    json_array_add_u32_unique(implicit, fourcc);
                    continue;
                }
                EGLuint64KHR* mods = calloc((size_t)mod_count, sizeof(EGLuint64KHR));
                EGLBoolean* external = calloc((size_t)mod_count, sizeof(EGLBoolean));
                if (!mods || !external ||
                    !egl->query_dmabuf_modifiers(egl->display,
                                                formats[i],
                                                mod_count,
                                                mods,
                                                external,
                                                &mod_count)) {
                    free(mods);
                    free(external);
                    continue;
                }
                for (EGLint m = 0; m < mod_count; m++) {
                    if (external[m])
                        continue;
                    uint64_t modifier = (uint64_t)mods[m];
                    uint32_t planes = probe_gbm_plane_count(fourcc, modifier, &egl->identity);
                    if (planes == 0)
                        continue;
                    json_array_add_u32_unique(fourccs, fourcc);
                    json_object_array_add(modifiers, modifier_entry(fourcc, modifier, planes));
                }
                free(mods);
                free(external);
            }
        }
        free(formats);
    }

    if (json_object_array_length(implicit) > 0)
        json_object_array_add(memory, json_object_new_string("implicit-linear"));

    if (json_object_array_length(fourccs) == 0) {
        vivid_wayland_error("EGL DMA-BUF query returned no importable format/modifier tuples");
        json_object_put(caps);
        json_object_put(fourccs);
        json_object_put(modifiers);
        json_object_put(implicit);
        json_object_put(relay);
        json_object_put(memory);
        json_object_put(sync);
        json_object_put(color);
        json_object_put(extent);
        return NULL;
    }

    json_object_object_add(caps, "version", json_object_new_int(3));
    json_object_object_add(caps, "backend", json_object_new_string("wayland-egl-gles-shadow"));
    json_object_object_add(caps, "probe", json_object_new_string(probe));
    json_object_object_add(caps, "relayModes", relay);
    json_object_object_add(caps, "renderNode", json_object_new_string(egl->identity.render_node));
    json_object_object_add(caps, "deviceUuid", json_object_new_string(egl->identity.device_uuid));
    json_object_object_add(caps, "driverUuid", json_object_new_string(egl->identity.driver_uuid));
    json_object_object_add(caps, "vendor", json_object_new_string(egl->identity.vendor));
    json_object_object_add(caps, "pciAddress", json_object_new_string(egl->identity.pci_address));
    json_object_object_add(caps, "fourccs", fourccs);
    json_object_object_add(caps, "modifiers", modifiers);
    json_object_object_add(caps, "implicitLinearFourccs", implicit);
    json_object_object_add(caps, "memoryHints", memory);
    json_object_object_add(caps, "syncCaps", sync);
    json_object_object_add(caps, "colorCaps", color);
    json_object_object_add(caps, "extentMax", extent);
    json_object_object_add(caps, "textureTarget", json_object_new_string("GL_TEXTURE_2D"));
    json_object_object_add(caps, "skipsExternalOnlyModifiers", json_object_new_boolean(1));
    json_object_object_add(caps, "diagnostics", json_object_new_string(egl->identity.diagnostics));
    return caps;
}

bool
vivid_wayland_egl_wait_acquire(VividWaylandEgl* egl, int* sync_fd, const char* context)
{
    if (!egl || !sync_fd || *sync_fd < 0) {
        vivid_wayland_warn("invalid EGL acquire fence fd context=%s", context ? context : "");
        return false;
    }
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        vivid_wayland_warn("EGL acquire wait without current context context=%s",
                           context ? context : "");
        return false;
    }

    if (!egl->create_sync || !egl->wait_sync || !egl->destroy_sync) {
        vivid_wayland_warn("EGL native-fence wait functions unavailable context=%s",
                           context ? context : "");
        return false;
    }

    EGLint attrs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
        *sync_fd,
        EGL_NONE,
    };
    EGLSyncKHR sync = egl->create_sync(egl->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
        vivid_wayland_warn("eglCreateSyncKHR(acquire) failed egl=0x%x context=%s",
                           eglGetError(),
                           context ? context : "");
        return false;
    }
    /* Successful createSync takes ownership of the native fence fd. */
    *sync_fd = -1;
    EGLBoolean waited = egl->wait_sync(egl->display, sync, 0);
    egl->destroy_sync(egl->display, sync);
    if (!waited) {
        vivid_wayland_warn("eglWaitSyncKHR(acquire) failed egl=0x%x context=%s",
                           eglGetError(),
                           context ? context : "");
        return false;
    }
    return true;
}

EGLImageKHR
vivid_wayland_egl_import_image(VividWaylandEgl* egl,
                               const VividWaylandDmaBufImport* import,
                               char* error,
                               size_t error_size)
{
    if (!egl || !egl->create_image || !import) {
        if (error && error_size)
            snprintf(error, error_size, "EGL DMA-BUF image import is unavailable");
        return EGL_NO_IMAGE_KHR;
    }

    bool modifier_implicit = import->modifier == VIVID_WAYLAND_DRM_FORMAT_MOD_INVALID ||
        import->modifier == VIVID_WAYLAND_DRM_FORMAT_MOD_LINEAR;
    const char* exts = eglQueryString(egl->display, EGL_EXTENSIONS);
    bool has_mod_import = has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    bool include_modifier = !modifier_implicit;
    if (include_modifier && !has_mod_import) {
        if (error && error_size)
            snprintf(error,
                     error_size,
                     "modifier=0x%llx requires EGL_EXT_image_dma_buf_import_modifiers",
                     (unsigned long long)import->modifier);
        return EGL_NO_IMAGE_KHR;
    }

    EGLint attrs[64];
    size_t n = 0;
    attrs[n++] = EGL_WIDTH;
    attrs[n++] = import->width;
    attrs[n++] = EGL_HEIGHT;
    attrs[n++] = import->height;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = (EGLint)import->fourcc;

    const EGLint fd_attr[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    const EGLint off_attr[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    const EGLint pitch_attr[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    const EGLint mod_lo[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    const EGLint mod_hi[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    for (uint32_t i = 0;
         i < import->n_planes && i < VIVID_DISPLAY_DMABUF_MAX_PLANES;
         i++) {
        attrs[n++] = fd_attr[i];
        attrs[n++] = import->planes[i].fd;
        attrs[n++] = off_attr[i];
        attrs[n++] = (EGLint)import->planes[i].offset;
        attrs[n++] = pitch_attr[i];
        attrs[n++] = (EGLint)import->planes[i].stride;
        if (include_modifier) {
            attrs[n++] = mod_lo[i];
            attrs[n++] = (EGLint)(import->modifier & 0xffffffffull);
            attrs[n++] = mod_hi[i];
            attrs[n++] = (EGLint)((import->modifier >> 32) & 0xffffffffull);
        }
    }
    attrs[n++] = EGL_NONE;

    EGLImageKHR image =
        egl->create_image(egl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR && error && error_size) {
        snprintf(error, error_size, "eglCreateImageKHR failed with EGL error 0x%x", eglGetError());
    }
    return image;
}

GLuint
vivid_wayland_egl_create_texture(VividWaylandEgl* egl,
                                 EGLImageKHR image,
                                 char* error,
                                 size_t error_size)
{
    if (!egl || !egl->image_target_texture || image == EGL_NO_IMAGE_KHR) {
        if (error && error_size)
            snprintf(error, error_size, "glEGLImageTargetTexture2DOES unavailable");
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->image_target_texture(GL_TEXTURE_2D, image);
    GLenum gl_error = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (gl_error != GL_NO_ERROR) {
        if (error && error_size)
            snprintf(error,
                     error_size,
                     "glEGLImageTargetTexture2DOES failed with GL error 0x%x",
                     gl_error);
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}

void
vivid_wayland_egl_destroy_image(VividWaylandEgl* egl, EGLImageKHR* image)
{
    if (!egl || !image || *image == EGL_NO_IMAGE_KHR)
        return;
    if (egl->destroy_image && egl->display != EGL_NO_DISPLAY)
        egl->destroy_image(egl->display, *image);
    *image = EGL_NO_IMAGE_KHR;
}

bool
vivid_wayland_egl_draw_frame(VividWaylandEgl* egl,
                             GLuint texture,
                             int surface_w,
                             int surface_h,
                             int buffer_w,
                             int buffer_h,
                             const VividWaylandRect* source,
                             const VividWaylandRect* dest,
                             const float clear_color[4])
{
    if (!egl || !egl->program || surface_w <= 0 || surface_h <= 0)
        return false;

    glViewport(0, 0, surface_w, surface_h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!texture || buffer_w <= 0 || buffer_h <= 0)
        return true;

    VividWaylandRect src = source ? *source
                                  : (VividWaylandRect) { 0, 0, (double)buffer_w, (double)buffer_h };
    VividWaylandRect dst = dest ? *dest
                                : (VividWaylandRect) { 0, 0, (double)surface_w, (double)surface_h };
    if (src.w <= 0.0)
        src.w = (double)buffer_w;
    if (src.h <= 0.0)
        src.h = (double)buffer_h;
    if (dst.w <= 0.0)
        dst.w = (double)surface_w;
    if (dst.h <= 0.0)
        dst.h = (double)surface_h;

    /*
     * SET_CONFIG dest is top-left in output physical pixels. OpenGL NDC is
     * Y-up, so the dest Y is flipped. Producer DMA-BUF rows are top-left;
     * GLES samples v=0 at the texture bottom, so UV.v is flipped here.
     */
    float x0 = (float)(2.0 * dst.x / (double)surface_w - 1.0);
    float x1 = (float)(2.0 * (dst.x + dst.w) / (double)surface_w - 1.0);
    float y_top = (float)(1.0 - 2.0 * dst.y / (double)surface_h);
    float y_bot = (float)(1.0 - 2.0 * (dst.y + dst.h) / (double)surface_h);

    float u0 = (float)(src.x / (double)buffer_w);
    float u1 = (float)((src.x + src.w) / (double)buffer_w);
    float v0 = (float)(src.y / (double)buffer_h);
    float v1 = (float)((src.y + src.h) / (double)buffer_h);
    float tmp = v0;
    v0 = 1.0f - v1;
    v1 = 1.0f - tmp;

    float pos[] = {
        x0, y_bot, x1, y_bot, x0, y_top, x1, y_top,
    };
    float uv[] = {
        u0, v1, u1, v1, u0, v0, u1, v0,
    };

    glUseProgram(egl->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(egl->loc_tex, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    /*
     * Deliberately no glFinish() here. The draw is queued behind the
     * producer's acquire fence, so finishing it on the CPU would block this
     * single-threaded consumer for the producer's whole GPU frame time. The
     * caller orders the producer's release against the draw with a fence
     * exported by vivid_wayland_egl_export_draw_fence() instead.
     */
    return true;
}

int
vivid_wayland_egl_export_draw_fence(VividWaylandEgl* egl, const char* context)
{
    if (!egl || egl->display == EGL_NO_DISPLAY)
        return -1;
    if (!egl->native_fence_export) {
        if (!egl->native_fence_export_logged) {
            egl->native_fence_export_logged = true;
            vivid_wayland_warn("EGL_ANDROID_native_fence_sync export unavailable; "
                               "release fences fall back to glFinish()");
        }
        return -1;
    }
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        vivid_wayland_warn("EGL draw fence export without current context context=%s",
                           context ? context : "");
        return -1;
    }

    /*
     * Creating a native fence sync without an fd inserts a fence at the
     * current point of the command stream. The fd only becomes available once
     * that command stream has been flushed, so flush explicitly instead of
     * relying on driver-specific implicit flushes.
     */
    const EGLint attrs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
        EGL_NO_NATIVE_FENCE_FD_ANDROID,
        EGL_NONE,
    };
    EGLSyncKHR sync = egl->create_sync(egl->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
        vivid_wayland_warn("eglCreateSyncKHR(draw fence) failed egl=0x%x context=%s",
                           eglGetError(),
                           context ? context : "");
        return -1;
    }
    glFlush();
    int fd = egl->dup_native_fence_fd(egl->display, sync);
    egl->destroy_sync(egl->display, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        vivid_wayland_warn("eglDupNativeFenceFDANDROID(draw fence) failed egl=0x%x context=%s",
                           eglGetError(),
                           context ? context : "");
        return -1;
    }
    return fd;
}

void
vivid_wayland_egl_wait_draw_idle(VividWaylandEgl* egl)
{
    if (!egl || egl->display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT)
        return;
    glFinish();
}
