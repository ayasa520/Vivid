#ifndef VIVID_WAYLAND_INPUT_H
#define VIVID_WAYLAND_INPUT_H

#include "vivid_wayland_app.h"

void vivid_wayland_input_bind_shm(VividWaylandApp* app,
                                   struct wl_registry* registry,
                                   uint32_t name,
                                   uint32_t version);
void vivid_wayland_input_bind_seat(VividWaylandApp* app,
                                    struct wl_registry* registry,
                                    uint32_t name,
                                    uint32_t version);
bool vivid_wayland_input_handle_global_remove(VividWaylandApp* app, uint32_t name);
void vivid_wayland_input_output_destroying(VividWaylandApp* app, VividWaylandOutput* output);
void vivid_wayland_input_finish(VividWaylandApp* app);

#endif
