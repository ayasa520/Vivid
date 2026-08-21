#ifndef VIVID_WAYLAND_LOG_H
#define VIVID_WAYLAND_LOG_H

#include <stdio.h>

#define vivid_wayland_log(fmt, ...) \
    fprintf(stderr, "vivid-layer-shell: " fmt "\n", ##__VA_ARGS__)

#define vivid_wayland_warn(fmt, ...) \
    fprintf(stderr, "vivid-layer-shell: warning: " fmt "\n", ##__VA_ARGS__)

#define vivid_wayland_error(fmt, ...) \
    fprintf(stderr, "vivid-layer-shell: error: " fmt "\n", ##__VA_ARGS__)

#endif
