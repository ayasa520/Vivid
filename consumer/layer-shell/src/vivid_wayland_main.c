#define _GNU_SOURCE

#include "vivid_wayland_app.h"
#include "vivid_wayland_log.h"
#include "vivid_wayland_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
usage(const char* argv0)
{
    fprintf(stderr,
            "Usage: %s [--layer background|bottom|top|overlay]\n"
            "\n"
            "Layer-shell wallpaper consumer for compositors that advertise\n"
            "zwlr_layer_shell_v1 (Sway, Hyprland, labwc, river, wayfire, niri, ...).\n"
            "GNOME still needs consumer/gnome; KDE still has consumer/kde.\n"
            "\n"
            "  --layer background|bottom|top|overlay\n"
            "                          wallpaper layer. Default: bottom, or background on niri.\n"
            "                          Hyprland widgets on the bottom layer: --layer background\n",
            argv0);
}

static const char*
layer_name(enum VividWaylandLayer layer)
{
    switch (layer) {
    case VIVID_WAYLAND_LAYER_BACKGROUND:
        return "background";
    case VIVID_WAYLAND_LAYER_TOP:
        return "top";
    case VIVID_WAYLAND_LAYER_OVERLAY:
        return "overlay";
    case VIVID_WAYLAND_LAYER_BOTTOM:
    default:
        return "bottom";
    }
}

int
main(int argc, char** argv)
{
    enum VividWaylandLayer layer = VIVID_WAYLAND_LAYER_BOTTOM;
    bool layer_set = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--layer") == 0) && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bottom") == 0)
                layer = VIVID_WAYLAND_LAYER_BOTTOM;
            else if (strcmp(argv[i], "background") == 0)
                layer = VIVID_WAYLAND_LAYER_BACKGROUND;
            else if (strcmp(argv[i], "top") == 0)
                layer = VIVID_WAYLAND_LAYER_TOP;
            else if (strcmp(argv[i], "overlay") == 0)
                layer = VIVID_WAYLAND_LAYER_OVERLAY;
            else {
                fprintf(stderr, "unknown layer '%s'\n", argv[i]);
                return 2;
            }
            layer_set = true;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    enum VividWaylandCompositor compositor = vivid_wayland_detect_compositor();
    if (!layer_set && vivid_wayland_prefer_background_layer(compositor))
        layer = VIVID_WAYLAND_LAYER_BACKGROUND;

    VividWaylandApp app;
    if (!vivid_wayland_app_init(&app, layer)) {
        vivid_wayland_app_finish(&app);
        return 1;
    }

    vivid_wayland_log("listening on %s compositor=%s layer=%s namespace=%s",
                      app.socket_path,
                      vivid_wayland_compositor_name(compositor),
                      layer_name(layer),
                      VIVID_WAYLAND_SURFACE_NAMESPACE);
    int rc = vivid_wayland_app_run(&app);
    vivid_wayland_app_finish(&app);
    return rc;
}
