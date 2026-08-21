#define _GNU_SOURCE

#include "vivid_wayland_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

void
vivid_wayland_close_fd(int* fd)
{
    if (!fd || *fd < 0)
        return;
    close(*fd);
    *fd = -1;
}

void
vivid_wayland_strlcpy(char* dest, const char* src, size_t dest_size)
{
    if (!dest || dest_size == 0)
        return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < dest_size && src[i]; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

const char*
vivid_wayland_transform_string(int32_t wl_transform)
{
    switch (wl_transform) {
    case WL_OUTPUT_TRANSFORM_90:
        return "90";
    case WL_OUTPUT_TRANSFORM_180:
        return "180";
    case WL_OUTPUT_TRANSFORM_270:
        return "270";
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        return "flipped";
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        return "flipped-90";
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        return "flipped-180";
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        return "flipped-270";
    case WL_OUTPUT_TRANSFORM_NORMAL:
    default:
        return "normal";
    }
}

const char*
vivid_wayland_default_socket_path(char* buffer, size_t buffer_size)
{
    const char* override = getenv("VIVID_DISPLAY_SOCKET");
    if (override && override[0]) {
        vivid_wayland_strlcpy(buffer, override, buffer_size);
        return buffer;
    }

    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !runtime[0])
        runtime = "/tmp";
    snprintf(buffer, buffer_size, "%s/vivid/display-v1.sock", runtime);
    return buffer;
}

uint64_t
vivid_wayland_monotonic_usec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static bool
env_contains_ci(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !needle[0])
        return false;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return true;
    }
    return false;
}

const char*
vivid_wayland_compositor_name(enum VividWaylandCompositor compositor)
{
    switch (compositor) {
    case VIVID_WAYLAND_COMPOSITOR_HYPRLAND:
        return "hyprland";
    case VIVID_WAYLAND_COMPOSITOR_SWAY:
        return "sway";
    case VIVID_WAYLAND_COMPOSITOR_NIRI:
        return "niri";
    case VIVID_WAYLAND_COMPOSITOR_UNKNOWN:
    default:
        return "unknown";
    }
}

enum VividWaylandCompositor
vivid_wayland_detect_compositor_from_env(const char* xdg_current_desktop,
                                         const char* hyprland_instance_signature,
                                         const char* swaysock,
                                         const char* niri_socket)
{
    /*
     * Session sockets beat XDG_CURRENT_DESKTOP: a nested Sway under Hyprland
     * still inherits HYPRLAND_INSTANCE_SIGNATURE, but SWAYSOCK is the compositor
     * that owns WAYLAND_DISPLAY.
     */
    if (niri_socket && niri_socket[0])
        return VIVID_WAYLAND_COMPOSITOR_NIRI;
    if (swaysock && swaysock[0])
        return VIVID_WAYLAND_COMPOSITOR_SWAY;
    if (hyprland_instance_signature && hyprland_instance_signature[0])
        return VIVID_WAYLAND_COMPOSITOR_HYPRLAND;
    if (env_contains_ci(xdg_current_desktop, "niri"))
        return VIVID_WAYLAND_COMPOSITOR_NIRI;
    if (env_contains_ci(xdg_current_desktop, "hyprland"))
        return VIVID_WAYLAND_COMPOSITOR_HYPRLAND;
    if (env_contains_ci(xdg_current_desktop, "sway"))
        return VIVID_WAYLAND_COMPOSITOR_SWAY;
    return VIVID_WAYLAND_COMPOSITOR_UNKNOWN;
}

enum VividWaylandCompositor
vivid_wayland_detect_compositor(void)
{
    return vivid_wayland_detect_compositor_from_env(getenv("XDG_CURRENT_DESKTOP"),
                                                    getenv("HYPRLAND_INSTANCE_SIGNATURE"),
                                                    getenv("SWAYSOCK"),
                                                    getenv("NIRI_SOCKET"));
}

bool
vivid_wayland_prefer_background_layer(enum VividWaylandCompositor compositor)
{
    return compositor == VIVID_WAYLAND_COMPOSITOR_NIRI;
}
