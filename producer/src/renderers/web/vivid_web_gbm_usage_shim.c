/*
 * Narrow LD_PRELOAD shim for the CEF/NVIDIA GBM path.
 *
 * CEF's Linux OSR shared-texture path can allocate its exported GBM shared
 * image on the display render node in hybrid systems, and NVIDIA rejects the
 * linear-only scanout shape Chromium may request for this path. Vivid preloads
 * this shim before CEF forks so the GPU process allocates on the resolved render
 * node and narrowly rewrites the page color buffer into a renderable scanout.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <drm/drm_fourcc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct gbm_bo* (*gbm_bo_create_fn)(struct gbm_device*,
                                           uint32_t,
                                           uint32_t,
                                           uint32_t,
                                           uint32_t);
typedef struct gbm_bo* (*gbm_bo_create_with_modifiers_fn)(struct gbm_device*,
                                                          uint32_t,
                                                          uint32_t,
                                                          uint32_t,
                                                          const uint64_t*,
                                                          const unsigned int);
typedef struct gbm_bo* (*gbm_bo_create_with_modifiers2_fn)(struct gbm_device*,
                                                           uint32_t,
                                                           uint32_t,
                                                           uint32_t,
                                                           const uint64_t*,
                                                           const unsigned int,
                                                           uint32_t);
typedef struct gbm_device* (*gbm_create_device_fn)(int);
typedef void (*gbm_device_destroy_fn)(struct gbm_device*);
typedef int (*gbm_device_is_format_supported_fn)(struct gbm_device*,
                                                 uint32_t,
                                                 uint32_t);
typedef const char* (*gbm_device_get_backend_name_fn)(struct gbm_device*);

struct forced_gbm_fd
{
    struct gbm_device* device;
    int fd;
};

static pthread_mutex_t forced_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct forced_gbm_fd forced_fds[64];

static bool env_enabled(const char* name, bool default_value)
{
    const char* value = getenv(name);
    if (!value || !*value)
        return default_value;
    return value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
           value[0] == 't' || value[0] == 'T';
}

static void* resolve_symbol(const char* name)
{
    void* symbol = dlsym(RTLD_NEXT, name);
    if (!symbol)
        fprintf(stderr, "VividGBMShim: failed to resolve %s: %s\n", name, dlerror());
    return symbol;
}

static void readlink_or_unknown(const char* path, char* out, size_t out_size)
{
    if (!out_size)
        return;
    ssize_t len = readlink(path, out, out_size - 1);
    if (len < 0) {
        snprintf(out, out_size, "(unknown)");
        return;
    }
    out[len] = '\0';
}

static bool path_is_render_node(const char* path)
{
    return path && strncmp(path, "/dev/dri/renderD", strlen("/dev/dri/renderD")) == 0;
}

static void remember_forced_fd(struct gbm_device* device, int fd)
{
    if (!device || fd < 0)
        return;

    pthread_mutex_lock(&forced_fd_lock);
    for (size_t i = 0; i < sizeof(forced_fds) / sizeof(forced_fds[0]); i++) {
        if (!forced_fds[i].device) {
            forced_fds[i].device = device;
            forced_fds[i].fd = fd;
            pthread_mutex_unlock(&forced_fd_lock);
            return;
        }
    }
    pthread_mutex_unlock(&forced_fd_lock);

    fprintf(stderr,
            "VividGBMShim: forced fd table is full; closing untracked fd=%d "
            "for gbm=%p\n",
            fd,
            device);
    close(fd);
}

static int take_forced_fd(struct gbm_device* device)
{
    int fd = -1;
    pthread_mutex_lock(&forced_fd_lock);
    for (size_t i = 0; i < sizeof(forced_fds) / sizeof(forced_fds[0]); i++) {
        if (forced_fds[i].device == device) {
            fd = forced_fds[i].fd;
            forced_fds[i].device = NULL;
            forced_fds[i].fd = -1;
            break;
        }
    }
    pthread_mutex_unlock(&forced_fd_lock);
    return fd;
}

static int maybe_open_forced_render_node(const char* original_target, char* forced_target, size_t forced_target_size)
{
    if (forced_target_size)
        forced_target[0] = '\0';

    const char* forced_path = getenv("VIVID_GBM_SHIM_FORCE_RENDER_NODE");
    if (!forced_path || !*forced_path || !path_is_render_node(original_target) ||
        strcmp(original_target, forced_path) == 0)
        return -1;

    int forced_fd = open(forced_path, O_RDWR | O_CLOEXEC);
    if (forced_fd < 0) {
        fprintf(stderr,
                "VividGBMShim: failed to open forced render node %s for "
                "original=%s errno=%d(%s)\n",
                forced_path,
                original_target,
                errno,
                strerror(errno));
        return -1;
    }

    if (forced_target_size) {
        char fd_path_name[64];
        snprintf(fd_path_name, sizeof(fd_path_name), "/proc/self/fd/%d", forced_fd);
        readlink_or_unknown(fd_path_name, forced_target, forced_target_size);
    }
    return forced_fd;
}

static const char* backend_name(struct gbm_device* gbm)
{
    static gbm_device_get_backend_name_fn real_backend_name;
    if (!real_backend_name)
        real_backend_name =
            (gbm_device_get_backend_name_fn)resolve_symbol("gbm_device_get_backend_name");
    if (!real_backend_name || !gbm)
        return "(unknown)";
    const char* name = real_backend_name(gbm);
    return name && *name ? name : "(unknown)";
}

static void fourcc_to_string(uint32_t format, char out[5])
{
    out[0] = (char)(format & 0xff);
    out[1] = (char)((format >> 8) & 0xff);
    out[2] = (char)((format >> 16) & 0xff);
    out[3] = (char)((format >> 24) & 0xff);
    out[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (out[i] < 32 || out[i] > 126)
            out[i] = '.';
    }
}

static void append_flag(char* out, size_t out_size, const char* flag)
{
    size_t used = 0;
    while (used < out_size && out[used])
        used++;
    if (used + 1 >= out_size)
        return;
    int written = snprintf(out + used,
                           out_size - used,
                           "%s%s",
                           used ? "|" : "",
                           flag);
    (void)written;
}

static void flags_to_string(uint32_t flags, char* out, size_t out_size)
{
    if (!out_size)
        return;
    out[0] = '\0';
    if (flags & GBM_BO_USE_SCANOUT)
        append_flag(out, out_size, "SCANOUT");
    if (flags & GBM_BO_USE_CURSOR)
        append_flag(out, out_size, "CURSOR");
    if (flags & GBM_BO_USE_RENDERING)
        append_flag(out, out_size, "RENDERING");
    if (flags & GBM_BO_USE_WRITE)
        append_flag(out, out_size, "WRITE");
    if (flags & GBM_BO_USE_LINEAR)
        append_flag(out, out_size, "LINEAR");
    if (flags & GBM_BO_USE_PROTECTED)
        append_flag(out, out_size, "PROTECTED");
    if (!out[0])
        snprintf(out, out_size, "0");
}

static void modifiers_to_string(const uint64_t* modifiers,
                                unsigned int count,
                                char* out,
                                size_t out_size)
{
    if (!out_size)
        return;
    out[0] = '\0';
    if (!modifiers || count == 0) {
        snprintf(out, out_size, "(none)");
        return;
    }

    const unsigned int limit = count < 8 ? count : 8;
    size_t used = 0;
    for (unsigned int i = 0; i < limit && used < out_size; i++) {
        int written = snprintf(out + used,
                               out_size - used,
                               "%s0x%016llx",
                               i ? "," : "",
                               (unsigned long long)modifiers[i]);
        if (written < 0)
            break;
        used += (size_t)written;
    }
    if (count > limit && used < out_size)
        snprintf(out + used, out_size - used, ",...(+%u)", count - limit);
}

static bool is_web_color_format(uint32_t format)
{
    return format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
}

static bool can_rewrite_gpu_scanout_usage(uint32_t format, uint32_t flags)
{
    return env_enabled("VIVID_GBM_SHIM_REWRITE", false) &&
           is_web_color_format(format) &&
           (flags & GBM_BO_USE_SCANOUT) &&
           !(flags & GBM_BO_USE_WRITE);
}

static bool can_rewrite_gpu_scanout_bo(uint32_t width,
                                       uint32_t height,
                                       uint32_t format,
                                       uint32_t flags)
{
    return width > 1 && height > 1 && can_rewrite_gpu_scanout_usage(format, flags);
}

static uint32_t rewrite_flags_for_usage(uint32_t format, uint32_t flags)
{
    if (!can_rewrite_gpu_scanout_usage(format, flags))
        return flags;

    return (flags & ~GBM_BO_USE_LINEAR) | GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
}

static uint32_t rewrite_flags_for_bo(uint32_t width,
                                     uint32_t height,
                                     uint32_t format,
                                     uint32_t flags)
{
    if (!can_rewrite_gpu_scanout_bo(width, height, format, flags))
        return flags;

    return rewrite_flags_for_usage(format, flags);
}

static bool modifiers_are_linear_only(const uint64_t* modifiers, unsigned int count)
{
    if (!modifiers || count == 0)
        return false;

    for (unsigned int i = 0; i < count; i++) {
        if (modifiers[i] != DRM_FORMAT_MOD_LINEAR)
            return false;
    }
    return true;
}

static void log_modifier_fallback(const char* function,
                                  struct gbm_device* gbm,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t format,
                                  uint32_t flags,
                                  uint32_t effective_flags)
{
    if (!env_enabled("VIVID_GBM_SHIM_LOG", false))
        return;

    char original[160];
    char rewritten[160];
    char fourcc[5];
    flags_to_string(flags, original, sizeof(original));
    flags_to_string(effective_flags, rewritten, sizeof(rewritten));
    fourcc_to_string(format, fourcc);
    fprintf(stderr,
            "VividGBMShim: %s linear-only modifier fallback backend=%s "
            "size=%ux%u format=%s/0x%08x flags=0x%08x(%s) "
            "effective=0x%08x(%s)\n",
            function,
            backend_name(gbm),
            width,
            height,
            fourcc,
            format,
            flags,
            original,
            effective_flags,
            rewritten);
}

static void log_call(const char* function,
                     struct gbm_device* gbm,
                     uint32_t width,
                     uint32_t height,
                     uint32_t format,
                     uint32_t original_flags,
                     uint32_t rewritten_flags,
                     const void* result)
{
    if (!env_enabled("VIVID_GBM_SHIM_LOG", false))
        return;

    char original[160];
    char rewritten[160];
    char fourcc[5];
    flags_to_string(original_flags, original, sizeof(original));
    flags_to_string(rewritten_flags, rewritten, sizeof(rewritten));
    fourcc_to_string(format, fourcc);

    fprintf(stderr,
            "VividGBMShim: %s backend=%s size=%ux%u format=%s/0x%08x "
            "flags=0x%08x(%s) effective=0x%08x(%s) result=%p\n",
            function,
            backend_name(gbm),
            width,
            height,
            fourcc,
            format,
            original_flags,
            original,
            rewritten_flags,
            rewritten,
            result);
}

static void log_modifier_request(const char* function,
                                 const uint64_t* modifiers,
                                 unsigned int count)
{
    if (!env_enabled("VIVID_GBM_SHIM_LOG", false))
        return;

    char text[256];
    modifiers_to_string(modifiers, count, text, sizeof(text));
    fprintf(stderr,
            "VividGBMShim: %s requested-modifiers count=%u [%s]\n",
            function,
            count,
            text);
}

struct gbm_device* gbm_create_device(int fd)
{
    static gbm_create_device_fn real_create_device;
    if (!real_create_device)
        real_create_device = (gbm_create_device_fn)resolve_symbol("gbm_create_device");
    if (!real_create_device)
        return NULL;

    char fd_path_name[64];
    char fd_target[512];
    char effective_target[512];
    snprintf(fd_path_name, sizeof(fd_path_name), "/proc/self/fd/%d", fd);
    readlink_or_unknown(fd_path_name, fd_target, sizeof(fd_target));

    int forced_fd = maybe_open_forced_render_node(fd_target,
                                                  effective_target,
                                                  sizeof(effective_target));
    const int effective_fd = forced_fd >= 0 ? forced_fd : fd;
    if (forced_fd < 0)
        snprintf(effective_target, sizeof(effective_target), "%s", fd_target);

    struct gbm_device* result = real_create_device(effective_fd);
    if (forced_fd >= 0) {
        if (result)
            remember_forced_fd(result, forced_fd);
        else
            close(forced_fd);
    }

    if (env_enabled("VIVID_GBM_SHIM_LOG", false)) {
        fprintf(stderr,
                "VividGBMShim: gbm_create_device pid=%ld fd=%d "
                "fd-target=%s effective-fd=%d effective-target=%s "
                "result=%p backend=%s forced=%s\n",
                (long)getpid(),
                fd,
                fd_target,
                effective_fd,
                effective_target,
                result,
                backend_name(result),
                forced_fd >= 0 ? "true" : "false");
    }
    return result;
}

void gbm_device_destroy(struct gbm_device* gbm)
{
    static gbm_device_destroy_fn real_destroy;
    if (!real_destroy)
        real_destroy = (gbm_device_destroy_fn)resolve_symbol("gbm_device_destroy");

    int forced_fd = take_forced_fd(gbm);
    if (real_destroy)
        real_destroy(gbm);

    if (forced_fd >= 0) {
        if (env_enabled("VIVID_GBM_SHIM_LOG", false)) {
            fprintf(stderr,
                    "VividGBMShim: gbm_device_destroy closed forced fd=%d "
                    "for gbm=%p\n",
                    forced_fd,
                    gbm);
        }
        close(forced_fd);
    }
}

struct gbm_bo* gbm_bo_create(struct gbm_device* gbm,
                             uint32_t width,
                             uint32_t height,
                             uint32_t format,
                             uint32_t flags)
{
    static gbm_bo_create_fn real_create;
    if (!real_create)
        real_create = (gbm_bo_create_fn)resolve_symbol("gbm_bo_create");
    if (!real_create)
        return NULL;

    const uint32_t effective_flags = rewrite_flags_for_bo(width, height, format, flags);
    struct gbm_bo* result = real_create(gbm, width, height, format, effective_flags);
    log_call("gbm_bo_create", gbm, width, height, format, flags, effective_flags, result);
    return result;
}

struct gbm_bo* gbm_bo_create_with_modifiers(struct gbm_device* gbm,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t format,
                                            const uint64_t* modifiers,
                                            const unsigned int count)
{
    static gbm_bo_create_with_modifiers_fn real_create_with_modifiers;
    if (!real_create_with_modifiers)
        real_create_with_modifiers = (gbm_bo_create_with_modifiers_fn)
            resolve_symbol("gbm_bo_create_with_modifiers");
    if (!real_create_with_modifiers)
        return NULL;

    struct gbm_bo* result =
        real_create_with_modifiers(gbm, width, height, format, modifiers, count);
    log_modifier_request("gbm_bo_create_with_modifiers", modifiers, count);
    log_call("gbm_bo_create_with_modifiers",
             gbm,
             width,
             height,
             format,
             0,
             0,
             result);
    return result;
}

struct gbm_bo* gbm_bo_create_with_modifiers2(struct gbm_device* gbm,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t format,
                                             const uint64_t* modifiers,
                                             const unsigned int count,
                                             uint32_t flags)
{
    static gbm_bo_create_with_modifiers2_fn real_create_with_modifiers2;
    if (!real_create_with_modifiers2)
        real_create_with_modifiers2 = (gbm_bo_create_with_modifiers2_fn)
            resolve_symbol("gbm_bo_create_with_modifiers2");
    if (!real_create_with_modifiers2)
        return NULL;

    const uint32_t effective_flags = rewrite_flags_for_bo(width, height, format, flags);
    struct gbm_bo* result = NULL;
    if (effective_flags != flags && modifiers_are_linear_only(modifiers, count)) {
        static gbm_bo_create_fn real_create;
        if (!real_create)
            real_create = (gbm_bo_create_fn)resolve_symbol("gbm_bo_create");
        if (real_create) {
            log_modifier_fallback("gbm_bo_create_with_modifiers2",
                                  gbm,
                                  width,
                                  height,
                                  format,
                                  flags,
                                  effective_flags);
            result = real_create(gbm, width, height, format, effective_flags);
        }
    }
    if (!result) {
        result = real_create_with_modifiers2(gbm,
                                             width,
                                             height,
                                             format,
                                             modifiers,
                                             count,
                                             effective_flags);
    }
    log_modifier_request("gbm_bo_create_with_modifiers2", modifiers, count);
    log_call("gbm_bo_create_with_modifiers2",
             gbm,
             width,
             height,
             format,
             flags,
             effective_flags,
             result);
    return result;
}

int gbm_device_is_format_supported(struct gbm_device* gbm,
                                   uint32_t format,
                                   uint32_t flags)
{
    static gbm_device_is_format_supported_fn real_is_format_supported;
    if (!real_is_format_supported)
        real_is_format_supported = (gbm_device_is_format_supported_fn)
            resolve_symbol("gbm_device_is_format_supported");
    if (!real_is_format_supported)
        return 0;

    const uint32_t effective_flags = rewrite_flags_for_usage(format, flags);
    const int result = real_is_format_supported(gbm, format, effective_flags);

    if (env_enabled("VIVID_GBM_SHIM_LOG", false)) {
        char original[160];
        char rewritten[160];
        char fourcc[5];
        flags_to_string(flags, original, sizeof(original));
        flags_to_string(effective_flags, rewritten, sizeof(rewritten));
        fourcc_to_string(format, fourcc);
        fprintf(stderr,
                "VividGBMShim: gbm_device_is_format_supported backend=%s "
                "format=%s/0x%08x flags=0x%08x(%s) effective=0x%08x(%s) "
                "result=%d\n",
                backend_name(gbm),
                fourcc,
                format,
                flags,
                original,
                effective_flags,
                rewritten,
                result);
    }

    return result;
}
