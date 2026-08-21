#ifndef VIVID_WAYLAND_APP_H
#define VIVID_WAYLAND_APP_H

#include "vivid_wayland_egl.h"

#include "vivid_display_protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#define VIVID_WAYLAND_MAX_OUTPUTS 16
#define VIVID_WAYLAND_MAX_GENERATIONS 8
#define VIVID_WAYLAND_MAX_BUFFERS 8
#define VIVID_WAYLAND_RECONNECT_INITIAL_MS 1200u
#define VIVID_WAYLAND_RECONNECT_MAX_MS 30000u

enum VividWaylandLayer {
    VIVID_WAYLAND_LAYER_BACKGROUND = 0,
    VIVID_WAYLAND_LAYER_BOTTOM = 1,
    VIVID_WAYLAND_LAYER_TOP = 2,
    VIVID_WAYLAND_LAYER_OVERLAY = 3,
};

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct wl_cursor;
struct wl_cursor_theme;
struct wl_pointer;
struct wl_seat;
struct wl_shm;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
struct json_object;

typedef struct {
    uint32_t index;
    uint64_t size;
    uint32_t n_planes;
    VividWaylandDmaBufPlane planes[VIVID_DISPLAY_DMABUF_MAX_PLANES];
    EGLImageKHR egl_image;
    GLuint gl_texture;
    bool import_attempted;
} VividWaylandBoundBuffer;

typedef struct {
    uint64_t id;
    uint32_t output_id;
    int32_t width;
    int32_t height;
    uint32_t fourcc;
    uint64_t modifier;
    uint32_t planes_per_buffer;
    char render_node[128];
    char presentation_path[64];
    bool premultiplied;
    bool retired;
    bool has_config;
    uint32_t n_buffers;
    VividWaylandBoundBuffer buffers[VIVID_WAYLAND_MAX_BUFFERS];
} VividWaylandGeneration;

typedef struct VividWaylandOutput {
    struct VividWaylandApp* app;
    uint32_t registry_name;
    struct wl_output* wl_output;
    struct zxdg_output_v1* xdg_output;
    struct wl_surface* surface;
    struct zwlr_layer_surface_v1* layer_surface;
    struct wp_viewport* viewport;
    struct wp_fractional_scale_v1* fractional_scale;
    struct wl_egl_window* egl_window;
    EGLSurface egl_surface;

    int32_t x;
    int32_t y;
    int32_t logical_w;
    int32_t logical_h;
    int32_t mode_w;
    int32_t mode_h;
    int32_t refresh_mhz;
    int32_t scale_int;
    uint32_t fractional_120;
    int32_t transform;
    char name[64];
    char description[256];
    bool have_geometry;
    bool have_mode;
    bool have_xdg_position;
    bool have_name;
    bool wl_done;
    bool configured;
    bool registered;
    bool layer_closed;
    bool destroyed;

    uint32_t pending_configure_serial;
    int32_t configured_w;
    int32_t configured_h;

    uint32_t consumer_output_id;
    uint32_t monitor_index;
    uint32_t producer_output_id;

    VividWaylandGeneration generations[VIVID_WAYLAND_MAX_GENERATIONS];
    uint32_t n_generations;
    VividWaylandRect source;
    VividWaylandRect dest;
    float clear_color[4];

    int pending_acquire_fd;
    int pending_release_fd;
    uint64_t pending_generation;
    uint32_t pending_buffer;
    bool pending_frame;

    /*
     * Producer DMA-BUF shadow-copy is independent of compositor vsync.
     * The EGL native window can still wait for a compositor-released buffer
     * even with swap interval zero; callback scheduling cannot interrupt it.
     * swap_pending = GLES back-buffer has a new frame to post.
     * awaiting_frame = waiting for wl_surface.frame or the refresh timer.
     */
    struct wl_callback* frame_callback;
    bool swap_pending;
    bool awaiting_frame;
    bool frame_pacing_timer;
    uint32_t present_count;
    uint64_t next_swap_usec;
} VividWaylandOutput;

typedef struct VividWaylandApp {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    struct wl_shm* shm;
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct zwlr_layer_shell_v1* layer_shell;
    struct zxdg_output_manager_v1* xdg_output_manager;
    struct wp_viewporter* viewporter;
    struct wp_fractional_scale_manager_v1* fractional_manager;

    VividWaylandEgl egl;
    VividWaylandOutput outputs[VIVID_WAYLAND_MAX_OUTPUTS];
    uint32_t n_outputs;

    uint32_t seat_registry_name;
    uint32_t seat_version;
    VividWaylandOutput* pointer_output;
    double pointer_x;
    double pointer_y;
    double pointer_axis_dx;
    double pointer_axis_dy;
    double pointer_axis_discrete_dx;
    double pointer_axis_discrete_dy;
    uint32_t pointer_axis_source;
    struct wl_cursor_theme* cursor_theme;
    struct wl_cursor* cursor;
    struct wl_surface* cursor_surface;
    int32_t cursor_theme_scale;

    char socket_path[512];
    enum VividWaylandLayer layer;
    bool running;
    bool initial_topology_ready;

    int producer_fd;
    bool producer_connecting;
    VividDisplayRecvState recv_state;
    uint32_t reconnect_delay_ms;
    uint64_t next_reconnect_usec;
    uint32_t negotiated_version;
    bool caps_sent;
    bool handshake_complete;
    uint8_t* outbox;
    size_t outbox_len;
    size_t outbox_off;
} VividWaylandApp;

bool vivid_wayland_app_init(VividWaylandApp* app,
                            enum VividWaylandLayer layer);
int vivid_wayland_app_run(VividWaylandApp* app);
void vivid_wayland_app_finish(VividWaylandApp* app);

void vivid_wayland_app_request_stop(VividWaylandApp* app);
void vivid_wayland_output_present(VividWaylandOutput* output);
void vivid_wayland_output_ensure_egl(VividWaylandOutput* output);
void vivid_wayland_output_destroy_layer(VividWaylandOutput* output);
VividWaylandOutput* vivid_wayland_app_find_output_by_producer_id(VividWaylandApp* app,
                                                                 uint32_t output_id);
VividWaylandOutput* vivid_wayland_app_find_output_by_consumer_id(VividWaylandApp* app,
                                                                 uint32_t consumer_id);
void vivid_wayland_output_retire_generations(VividWaylandOutput* output, const char* reason);
void vivid_wayland_output_retire_generation(VividWaylandOutput* output, uint64_t generation);

bool vivid_wayland_producer_connect(VividWaylandApp* app);
void vivid_wayland_producer_disconnect(VividWaylandApp* app, bool schedule_reconnect);
void vivid_wayland_producer_restart_for_topology(VividWaylandApp* app, const char* reason);
void vivid_wayland_producer_on_readable(VividWaylandApp* app);
void vivid_wayland_producer_on_writable(VividWaylandApp* app);
void vivid_wayland_producer_register_outputs(VividWaylandApp* app);
void vivid_wayland_producer_update_output(VividWaylandApp* app, VividWaylandOutput* output);
void vivid_wayland_producer_send_bind_failed(VividWaylandApp* app,
                                             const VividWaylandGeneration* generation,
                                             uint32_t reason,
                                             const char* message);
void vivid_wayland_producer_send_unbind_done(VividWaylandApp* app,
                                             uint32_t output_id,
                                             uint64_t generation);
void vivid_wayland_producer_send_pointer_motion(VividWaylandOutput* output,
                                                double x,
                                                double y,
                                                uint64_t time_usec);
void vivid_wayland_producer_send_pointer_button(VividWaylandOutput* output,
                                                double x,
                                                double y,
                                                uint32_t button,
                                                uint32_t state,
                                                uint64_t time_usec);
void vivid_wayland_producer_send_pointer_axis(VividWaylandOutput* output,
                                              double x,
                                              double y,
                                              double delta_x,
                                              double delta_y,
                                              uint32_t source,
                                              uint64_t time_usec);

double vivid_wayland_output_scale(const VividWaylandOutput* output);
void vivid_wayland_output_buffer_size(const VividWaylandOutput* output, int32_t* w, int32_t* h);

#endif
