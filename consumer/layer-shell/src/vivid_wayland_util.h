#ifndef VIVID_WAYLAND_UTIL_H
#define VIVID_WAYLAND_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIVID_WAYLAND_DESKTOP_ID "layer-shell"
#define VIVID_WAYLAND_CLIENT_NAME "layer-shell"
#define VIVID_WAYLAND_SURFACE_NAMESPACE "vivid"

enum VividWaylandCompositor {
    VIVID_WAYLAND_COMPOSITOR_UNKNOWN = 0,
    VIVID_WAYLAND_COMPOSITOR_HYPRLAND,
    VIVID_WAYLAND_COMPOSITOR_SWAY,
    VIVID_WAYLAND_COMPOSITOR_NIRI,
};

void vivid_wayland_close_fd(int* fd);
void vivid_wayland_strlcpy(char* dest, const char* src, size_t dest_size);
const char* vivid_wayland_transform_string(int32_t wl_transform);
const char* vivid_wayland_default_socket_path(char* buffer, size_t buffer_size);
uint64_t vivid_wayland_monotonic_usec(void);

const char* vivid_wayland_compositor_name(enum VividWaylandCompositor compositor);
enum VividWaylandCompositor vivid_wayland_detect_compositor(void);
enum VividWaylandCompositor vivid_wayland_detect_compositor_from_env(
    const char* xdg_current_desktop,
    const char* hyprland_instance_signature,
    const char* swaysock,
    const char* niri_socket);
bool vivid_wayland_prefer_background_layer(enum VividWaylandCompositor compositor);

#endif
