#define _GNU_SOURCE

#include "vivid_wayland_input.h"

#include "vivid_wayland_log.h"
#include "vivid_wayland_util.h"

#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-cursor.h>

#define VIVID_WAYLAND_MAX_SEAT_VERSION 5u
#define VIVID_WAYLAND_DEFAULT_CURSOR_SIZE 24
#define VIVID_WAYLAND_MAX_CURSOR_SIZE 512

static VividWaylandOutput*
output_for_surface(VividWaylandApp* app, struct wl_surface* surface)
{
    if (!app || !surface)
        return NULL;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        VividWaylandOutput* output = &app->outputs[i];
        if (!output->destroyed && output->surface == surface)
            return output;
    }
    return NULL;
}

static int32_t
cursor_base_size(void)
{
    const char* text = getenv("XCURSOR_SIZE");
    if (!text || !text[0])
        return VIVID_WAYLAND_DEFAULT_CURSOR_SIZE;

    errno = 0;
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 ||
        value > VIVID_WAYLAND_MAX_CURSOR_SIZE) {
        vivid_wayland_warn("ignoring invalid XCURSOR_SIZE=%s", text);
        return VIVID_WAYLAND_DEFAULT_CURSOR_SIZE;
    }
    return (int32_t)value;
}

static bool
load_cursor_for_scale(VividWaylandApp* app, int32_t scale)
{
    if (!app || !app->shm || !app->compositor)
        return false;
    if (scale < 1)
        scale = 1;
    if (app->cursor_theme && app->cursor && app->cursor_theme_scale == scale)
        return true;

    const int32_t base_size = cursor_base_size();
    if (base_size > INT32_MAX / scale)
        return false;
    struct wl_cursor_theme* theme =
        wl_cursor_theme_load(getenv("XCURSOR_THEME"), base_size * scale, app->shm);
    if (!theme) {
        vivid_wayland_warn("failed to load Wayland cursor theme");
        return false;
    }
    struct wl_cursor* cursor = wl_cursor_theme_get_cursor(theme, "left_ptr");
    if (!cursor)
        cursor = wl_cursor_theme_get_cursor(theme, "default");
    if (!cursor || cursor->image_count == 0) {
        vivid_wayland_warn("Wayland cursor theme has no default pointer");
        wl_cursor_theme_destroy(theme);
        return false;
    }

    if (!app->cursor_surface)
        app->cursor_surface = wl_compositor_create_surface(app->compositor);
    if (!app->cursor_surface) {
        vivid_wayland_warn("failed to create Wayland cursor surface");
        wl_cursor_theme_destroy(theme);
        return false;
    }

    if (app->cursor_theme)
        wl_cursor_theme_destroy(app->cursor_theme);
    app->cursor_theme = theme;
    app->cursor = cursor;
    app->cursor_theme_scale = scale;
    return true;
}

static void
set_pointer_cursor(VividWaylandApp* app, struct wl_pointer* pointer, uint32_t serial,
                   const VividWaylandOutput* output)
{
    int32_t scale = output && output->scale_int > 0 ? output->scale_int : 1;
    if (!load_cursor_for_scale(app, scale))
        return;

    struct wl_cursor_image* image = app->cursor->images[0];
    struct wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (!buffer)
        return;
    wl_surface_set_buffer_scale(app->cursor_surface, scale);
    wl_surface_attach(app->cursor_surface, buffer, 0, 0);
    if (wl_proxy_get_version((struct wl_proxy*)app->cursor_surface) >= 4)
        wl_surface_damage_buffer(app->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
    else
        wl_surface_damage(app->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(app->cursor_surface);
    wl_pointer_set_cursor(pointer,
                          serial,
                          app->cursor_surface,
                          (int32_t)image->hotspot_x / scale,
                          (int32_t)image->hotspot_y / scale);
}

static void
update_pointer_position(VividWaylandApp* app, VividWaylandOutput* output,
                        wl_fixed_t surface_x, wl_fixed_t surface_y, bool send)
{
    if (!app || !output)
        return;
    const double scale = vivid_wayland_output_scale(output);
    app->pointer_x = wl_fixed_to_double(surface_x) * scale;
    app->pointer_y = wl_fixed_to_double(surface_y) * scale;
    if (send) {
        vivid_wayland_producer_send_pointer_motion(output,
                                                    app->pointer_x,
                                                    app->pointer_y,
                                                    vivid_wayland_monotonic_usec());
    }
}

static void
reset_axis_frame(VividWaylandApp* app)
{
    app->pointer_axis_dx = 0.0;
    app->pointer_axis_dy = 0.0;
    app->pointer_axis_discrete_dx = 0.0;
    app->pointer_axis_discrete_dy = 0.0;
    app->pointer_axis_source = VIVID_DISPLAY_AXIS_CONTINUOUS;
}

static void
flush_axis_frame(VividWaylandApp* app)
{
    if (!app)
        return;
    const double dx = app->pointer_axis_discrete_dx != 0.0
        ? app->pointer_axis_discrete_dx
        : app->pointer_axis_dx;
    const double dy = app->pointer_axis_discrete_dy != 0.0
        ? app->pointer_axis_discrete_dy
        : app->pointer_axis_dy;
    if (app->pointer_output && (dx != 0.0 || dy != 0.0)) {
        vivid_wayland_producer_send_pointer_axis(app->pointer_output,
                                                  app->pointer_x,
                                                  app->pointer_y,
                                                  dx,
                                                  dy,
                                                  app->pointer_axis_source,
                                                  vivid_wayland_monotonic_usec());
    }
    reset_axis_frame(app);
}

static void
handle_pointer_enter(void* data, struct wl_pointer* pointer, uint32_t serial,
                     struct wl_surface* surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    VividWaylandApp* app = data;
    VividWaylandOutput* output = output_for_surface(app, surface);
    app->pointer_output = output;
    reset_axis_frame(app);
    if (!output) {
        vivid_wayland_warn("wl_pointer entered an unknown surface");
        return;
    }
    set_pointer_cursor(app, pointer, serial, output);
    update_pointer_position(app, output, surface_x, surface_y, true);
    vivid_wayland_log("pointer entered output=%s", output->name[0] ? output->name : "(unnamed)");
}

static void
handle_pointer_leave(void* data, struct wl_pointer* pointer, uint32_t serial,
                     struct wl_surface* surface)
{
    VividWaylandApp* app = data;
    (void)pointer;
    (void)serial;
    VividWaylandOutput* output = output_for_surface(app, surface);
    if (!output || app->pointer_output == output)
        app->pointer_output = NULL;
    reset_axis_frame(app);
}

static void
handle_pointer_motion(void* data, struct wl_pointer* pointer, uint32_t time,
                      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    VividWaylandApp* app = data;
    (void)pointer;
    (void)time;
    if (app->pointer_output)
        update_pointer_position(app, app->pointer_output, surface_x, surface_y, true);
}

static uint32_t
protocol_button(uint32_t button)
{
    switch (button) {
    case BTN_LEFT:
        return 1;
    case BTN_MIDDLE:
        return 2;
    case BTN_RIGHT:
        return 3;
    default:
        return 0;
    }
}

static void
handle_pointer_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time,
                      uint32_t button, uint32_t state)
{
    VividWaylandApp* app = data;
    (void)pointer;
    (void)serial;
    (void)time;
    uint32_t mapped = protocol_button(button);
    if (!app->pointer_output || mapped == 0)
        return;
    uint32_t mapped_state = state == WL_POINTER_BUTTON_STATE_PRESSED
        ? VIVID_DISPLAY_BUTTON_PRESSED
        : VIVID_DISPLAY_BUTTON_RELEASED;
    vivid_wayland_producer_send_pointer_button(app->pointer_output,
                                                app->pointer_x,
                                                app->pointer_y,
                                                mapped,
                                                mapped_state,
                                                vivid_wayland_monotonic_usec());
    vivid_wayland_log("pointer button output=%s button=%u state=%s",
                      app->pointer_output->name[0] ? app->pointer_output->name : "(unnamed)",
                      mapped,
                      mapped_state == VIVID_DISPLAY_BUTTON_PRESSED ? "pressed" : "released");
}

static void
handle_pointer_axis(void* data, struct wl_pointer* pointer, uint32_t time,
                    uint32_t axis, wl_fixed_t value)
{
    VividWaylandApp* app = data;
    (void)pointer;
    (void)time;
    double delta = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        app->pointer_axis_dx += delta;
    else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        app->pointer_axis_dy += delta;
    if (app->seat_version < VIVID_WAYLAND_MAX_SEAT_VERSION)
        flush_axis_frame(app);
}

static void
handle_pointer_frame(void* data, struct wl_pointer* pointer)
{
    (void)pointer;
    flush_axis_frame(data);
}

static void
handle_pointer_axis_source(void* data, struct wl_pointer* pointer, uint32_t source)
{
    VividWaylandApp* app = data;
    (void)pointer;
    switch (source) {
    case WL_POINTER_AXIS_SOURCE_WHEEL:
    case WL_POINTER_AXIS_SOURCE_WHEEL_TILT:
        app->pointer_axis_source = VIVID_DISPLAY_AXIS_WHEEL;
        break;
    case WL_POINTER_AXIS_SOURCE_FINGER:
        app->pointer_axis_source = VIVID_DISPLAY_AXIS_FINGER;
        break;
    case WL_POINTER_AXIS_SOURCE_CONTINUOUS:
    default:
        app->pointer_axis_source = VIVID_DISPLAY_AXIS_CONTINUOUS;
        break;
    }
}

static void
handle_pointer_axis_stop(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void
handle_pointer_axis_discrete(void* data, struct wl_pointer* pointer,
                             uint32_t axis, int32_t discrete)
{
    VividWaylandApp* app = data;
    (void)pointer;
    const double delta = (double)discrete * 120.0;
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        app->pointer_axis_discrete_dx = delta;
    else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        app->pointer_axis_discrete_dy = delta;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = handle_pointer_enter,
    .leave = handle_pointer_leave,
    .motion = handle_pointer_motion,
    .button = handle_pointer_button,
    .axis = handle_pointer_axis,
    .frame = handle_pointer_frame,
    .axis_source = handle_pointer_axis_source,
    .axis_stop = handle_pointer_axis_stop,
    .axis_discrete = handle_pointer_axis_discrete,
};

static void
release_pointer(VividWaylandApp* app)
{
    if (!app || !app->pointer)
        return;
    uint32_t version = wl_proxy_get_version((struct wl_proxy*)app->pointer);
    if (version >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(app->pointer);
    else
        wl_pointer_destroy(app->pointer);
    app->pointer = NULL;
    app->pointer_output = NULL;
    reset_axis_frame(app);
}

static void
handle_seat_capabilities(void* data, struct wl_seat* seat, uint32_t capabilities)
{
    VividWaylandApp* app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
        vivid_wayland_log("wl_seat pointer capability enabled");
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && app->pointer) {
        release_pointer(app);
        vivid_wayland_log("wl_seat pointer capability disabled");
    }
}

static void
handle_seat_name(void* data, struct wl_seat* seat, const char* name)
{
    (void)data;
    (void)seat;
    vivid_wayland_log("wl_seat name=%s", name ? name : "(unnamed)");
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = handle_seat_capabilities,
    .name = handle_seat_name,
};

void
vivid_wayland_input_bind_shm(VividWaylandApp* app, struct wl_registry* registry,
                             uint32_t name, uint32_t version)
{
    if (!app || app->shm)
        return;
    (void)version;
    app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}

void
vivid_wayland_input_bind_seat(VividWaylandApp* app, struct wl_registry* registry,
                              uint32_t name, uint32_t version)
{
    if (!app || app->seat)
        return;
    app->seat_version = version < VIVID_WAYLAND_MAX_SEAT_VERSION
        ? version
        : VIVID_WAYLAND_MAX_SEAT_VERSION;
    app->seat_registry_name = name;
    app->seat = wl_registry_bind(registry, name, &wl_seat_interface, app->seat_version);
    wl_seat_add_listener(app->seat, &seat_listener, app);
}

bool
vivid_wayland_input_handle_global_remove(VividWaylandApp* app, uint32_t name)
{
    if (!app || !app->seat || app->seat_registry_name != name)
        return false;
    release_pointer(app);
    if (app->seat_version >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(app->seat);
    else
        wl_seat_destroy(app->seat);
    app->seat = NULL;
    app->seat_registry_name = 0;
    app->seat_version = 0;
    return true;
}

void
vivid_wayland_input_output_destroying(VividWaylandApp* app, VividWaylandOutput* output)
{
    if (!app || !output || app->pointer_output != output)
        return;
    app->pointer_output = NULL;
    reset_axis_frame(app);
}

void
vivid_wayland_input_finish(VividWaylandApp* app)
{
    if (!app)
        return;
    release_pointer(app);
    if (app->seat) {
        if (app->seat_version >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(app->seat);
        else
            wl_seat_destroy(app->seat);
        app->seat = NULL;
    }
    if (app->cursor_surface) {
        wl_surface_destroy(app->cursor_surface);
        app->cursor_surface = NULL;
    }
    if (app->cursor_theme) {
        wl_cursor_theme_destroy(app->cursor_theme);
        app->cursor_theme = NULL;
        app->cursor = NULL;
    }
    if (app->shm) {
        wl_shm_destroy(app->shm);
        app->shm = NULL;
    }
}
