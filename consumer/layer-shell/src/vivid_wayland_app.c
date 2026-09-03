#define _GNU_SOURCE

#include "vivid_wayland_app.h"

#include "vivid_wayland_input.h"
#include "vivid_wayland_log.h"
#include "vivid_wayland_util.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-egl.h>

#include "fractional-scale-v1-protocol.h"
#include "viewporter-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"

static VividWaylandApp* g_app_for_signal;

static void renumber_outputs(VividWaylandApp* app);

static void
on_signal(int signo)
{
    (void)signo;
    if (g_app_for_signal)
        g_app_for_signal->running = false;
}

double
vivid_wayland_output_scale(const VividWaylandOutput* output)
{
    if (!output)
        return 1.0;
    if (output->fractional_120 > 0)
        return (double)output->fractional_120 / 120.0;
    if (output->scale_int > 0)
        return (double)output->scale_int;
    return 1.0;
}

void
vivid_wayland_output_buffer_size(const VividWaylandOutput* output, int32_t* w, int32_t* h)
{
    int32_t logical_w = output->configured_w > 0 ? output->configured_w : output->logical_w;
    int32_t logical_h = output->configured_h > 0 ? output->configured_h : output->logical_h;
    if (logical_w <= 0)
        logical_w = output->mode_w > 0 ? output->mode_w : 1280;
    if (logical_h <= 0)
        logical_h = output->mode_h > 0 ? output->mode_h : 720;
    double scale = vivid_wayland_output_scale(output);
    if (w)
        *w = (int32_t)(logical_w * scale + 0.5);
    if (h)
        *h = (int32_t)(logical_h * scale + 0.5);
    if (w && *w < 1)
        *w = 1;
    if (h && *h < 1)
        *h = 1;
}

VividWaylandOutput*
vivid_wayland_app_find_output_by_producer_id(VividWaylandApp* app, uint32_t output_id)
{
    if (!app || output_id == 0)
        return NULL;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        if (!app->outputs[i].destroyed && app->outputs[i].producer_output_id == output_id)
            return &app->outputs[i];
    }
    return NULL;
}

VividWaylandOutput*
vivid_wayland_app_find_output_by_consumer_id(VividWaylandApp* app, uint32_t consumer_id)
{
    if (!app || consumer_id == 0)
        return NULL;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        if (!app->outputs[i].destroyed && app->outputs[i].consumer_output_id == consumer_id)
            return &app->outputs[i];
    }
    return NULL;
}

static void
close_generation_gpu(VividWaylandOutput* output, VividWaylandGeneration* generation)
{
    if (!output || !generation)
        return;
    if (output->egl_surface != EGL_NO_SURFACE)
        vivid_wayland_egl_make_current(&output->app->egl, output->egl_surface);
    for (uint32_t i = 0; i < generation->n_buffers; i++) {
        VividWaylandBoundBuffer* buffer = &generation->buffers[i];
        for (uint32_t p = 0; p < buffer->n_planes; p++)
            vivid_wayland_close_fd(&buffer->planes[p].fd);
        if (buffer->gl_texture) {
            glDeleteTextures(1, &buffer->gl_texture);
            buffer->gl_texture = 0;
        }
        vivid_wayland_egl_destroy_image(&output->app->egl, &buffer->egl_image);
    }
    generation->n_buffers = 0;
    generation->retired = true;
}

void
vivid_wayland_output_retire_generation(VividWaylandOutput* output, uint64_t generation_id)
{
    if (!output)
        return;
    if (output->pending_frame && output->pending_generation == generation_id) {
        VividWaylandGeneration* generation = NULL;
        for (uint32_t i = 0; i < output->n_generations; i++) {
            if (output->generations[i].id == generation_id)
                generation = &output->generations[i];
        }
        if (generation)
            vivid_wayland_signal_release_syncobj(generation->render_node,
                                                 output->pending_release_fd,
                                                 "generation-retired");
        vivid_wayland_close_fd(&output->pending_acquire_fd);
        vivid_wayland_close_fd(&output->pending_release_fd);
        output->pending_frame = false;
    }
    for (uint32_t i = 0; i < output->n_generations; i++) {
        if (output->generations[i].id == generation_id)
            close_generation_gpu(output, &output->generations[i]);
    }
}

void
vivid_wayland_output_retire_generations(VividWaylandOutput* output, const char* reason)
{
    if (!output)
        return;
    if (output->pending_frame) {
        for (uint32_t i = 0; i < output->n_generations; i++) {
            if (output->generations[i].id == output->pending_generation) {
                vivid_wayland_signal_release_syncobj(output->generations[i].render_node,
                                                     output->pending_release_fd,
                                                     reason);
                break;
            }
        }
        vivid_wayland_close_fd(&output->pending_acquire_fd);
        vivid_wayland_close_fd(&output->pending_release_fd);
        output->pending_frame = false;
    }
    for (uint32_t i = 0; i < output->n_generations; i++)
        close_generation_gpu(output, &output->generations[i]);
    output->n_generations = 0;
}

void
vivid_wayland_output_destroy_layer(VividWaylandOutput* output)
{
    if (!output)
        return;
    if (output->app)
        vivid_wayland_input_output_destroying(output->app, output);
    vivid_wayland_output_retire_generations(output, "layer-destroy");
    if (output->frame_callback) {
        wl_callback_destroy(output->frame_callback);
        output->frame_callback = NULL;
    }
    output->swap_pending = false;
    output->awaiting_frame = false;
    if (output->egl_surface != EGL_NO_SURFACE && output->app) {
        eglMakeCurrent(output->app->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(output->app->egl.display, output->egl_surface);
        output->egl_surface = EGL_NO_SURFACE;
    }
    if (output->egl_window) {
        wl_egl_window_destroy(output->egl_window);
        output->egl_window = NULL;
    }
    if (output->viewport) {
        wp_viewport_destroy(output->viewport);
        output->viewport = NULL;
    }
    if (output->fractional_scale) {
        wp_fractional_scale_v1_destroy(output->fractional_scale);
        output->fractional_scale = NULL;
    }
    if (output->layer_surface) {
        zwlr_layer_surface_v1_destroy(output->layer_surface);
        output->layer_surface = NULL;
    }
    if (output->surface) {
        wl_surface_destroy(output->surface);
        output->surface = NULL;
    }
    output->configured = false;
}

static void
apply_surface_scale(VividWaylandOutput* output)
{
    if (!output->surface)
        return;
    int32_t buffer_w = 0, buffer_h = 0;
    vivid_wayland_output_buffer_size(output, &buffer_w, &buffer_h);
    int32_t logical_w = output->configured_w > 0 ? output->configured_w : output->logical_w;
    int32_t logical_h = output->configured_h > 0 ? output->configured_h : output->logical_h;
    if (logical_w <= 0)
        logical_w = buffer_w;
    if (logical_h <= 0)
        logical_h = buffer_h;

    if (output->viewport) {
        wp_viewport_set_destination(output->viewport, logical_w, logical_h);
        wl_surface_set_buffer_scale(output->surface, 1);
    } else {
        int32_t scale = output->scale_int > 0 ? output->scale_int : 1;
        wl_surface_set_buffer_scale(output->surface, scale);
    }
}

void
vivid_wayland_output_ensure_egl(VividWaylandOutput* output)
{
    if (!output || !output->surface || !output->configured || !output->app)
        return;
    int32_t buffer_w = 0, buffer_h = 0;
    vivid_wayland_output_buffer_size(output, &buffer_w, &buffer_h);
    if (!output->egl_window) {
        output->egl_window = wl_egl_window_create(output->surface, buffer_w, buffer_h);
        if (!output->egl_window) {
            vivid_wayland_error("wl_egl_window_create failed for %s", output->name);
            return;
        }
    } else {
        wl_egl_window_resize(output->egl_window, buffer_w, buffer_h, 0, 0);
    }

    /*
     * A layer configure can be dispatched while EGL initialization is still in
     * progress (notably with wl_output v1). In that ordering the native window
     * already exists but the first EGLSurface creation cannot succeed. Keep the
     * two lifecycles independent so every later ensure call retries the missing
     * EGLSurface instead of treating the native window as proof of success.
     */
    if (output->egl_surface == EGL_NO_SURFACE) {
        output->egl_surface =
            vivid_wayland_egl_create_window_surface(&output->app->egl, output->egl_window);
        if (output->egl_surface == EGL_NO_SURFACE) {
            vivid_wayland_error("eglCreateWindowSurface failed for %s egl=0x%x",
                                output->name,
                                eglGetError());
            return;
        }
        vivid_wayland_egl_disable_compositor_swap_throttle(&output->app->egl,
                                                           output->egl_surface);
        /* Same as linux-wallpaperengine setupLS: commit the new EGL window. */
        wl_surface_commit(output->surface);
        wl_display_flush(output->app->display);
    }
    apply_surface_scale(output);
}

static VividWaylandGeneration*
live_generation(VividWaylandOutput* output, uint64_t id)
{
    for (uint32_t i = 0; i < output->n_generations; i++) {
        if (output->generations[i].id == id && !output->generations[i].retired)
            return &output->generations[i];
    }
    return NULL;
}

static VividWaylandBoundBuffer*
live_buffer(VividWaylandGeneration* generation, uint32_t index)
{
    for (uint32_t i = 0; i < generation->n_buffers; i++) {
        if (generation->buffers[i].index == index)
            return &generation->buffers[i];
    }
    return NULL;
}

static uint64_t
output_frame_period_usec(const VividWaylandOutput* output)
{
    /* refresh_mhz stores wl_output millihertz. 0/unknown → ~60 Hz. */
    int32_t millihertz = output && output->refresh_mhz > 0 ? output->refresh_mhz : 60000;
    if (millihertz < 1000)
        millihertz = 60000;
    return 1000000000ull / (uint64_t)millihertz;
}

static void
output_destroy_frame_callback(VividWaylandOutput* output)
{
    if (!output || !output->frame_callback)
        return;
    wl_callback_destroy(output->frame_callback);
    output->frame_callback = NULL;
}

static void vivid_wayland_output_commit_swap(VividWaylandOutput* output);

static void
handle_surface_frame(void* data, struct wl_callback* callback, uint32_t time)
{
    VividWaylandOutput* output = data;
    (void)time;
    if (output->frame_callback == callback)
        output->frame_callback = NULL;
    wl_callback_destroy(callback);
    output->awaiting_frame = false;
    if (output->frame_pacing_timer) {
        output->frame_pacing_timer = false;
    }
    vivid_wayland_output_commit_swap(output);
}

static const struct wl_callback_listener surface_frame_listener = {
    .done = handle_surface_frame,
};

static void
damage_whole_surface(VividWaylandOutput* output)
{
    if (!output->surface)
        return;
    if (wl_proxy_get_version((struct wl_proxy*)output->surface) >= 4)
        wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
    else
        wl_surface_damage(output->surface, 0, 0, INT32_MAX, INT32_MAX);
}

static void
vivid_wayland_output_commit_swap(VividWaylandOutput* output)
{
    if (!output || !output->app || !output->swap_pending)
        return;
    if (output->egl_surface == EGL_NO_SURFACE)
        return;

    uint64_t now = vivid_wayland_monotonic_usec();
    if (output->awaiting_frame) {
        if (now < output->next_swap_usec)
            return;
        /*
         * Hyprland often skips wl_surface.frame on wallpaper layers. Use the
         * output refresh period to schedule another presentation attempt when
         * the callback is absent.
         */
        if (!output->frame_pacing_timer) {
            output->frame_pacing_timer = true;
            vivid_wayland_log("no wl_surface.frame on %s within %llums; scheduling swap attempts with a timer",
                              output->name[0] ? output->name : "(unnamed)",
                              (unsigned long long)(output_frame_period_usec(output) / 1000ull));
        }
        output->awaiting_frame = false;
        output_destroy_frame_callback(output);
    }

    if (!vivid_wayland_egl_make_current(&output->app->egl, output->egl_surface))
        return;

    apply_surface_scale(output);
    if (output->pending_configure_serial && output->layer_surface) {
        zwlr_layer_surface_v1_ack_configure(output->layer_surface,
                                            output->pending_configure_serial);
        output->pending_configure_serial = 0;
    }

    /*
     * linux-wallpaperengine's WaylandOutputViewport::swapOutput sequence:
     * request wl_surface.frame, eglSwapBuffers, then set scale + damage the
     * whole buffer and commit again. Hyprland often ignores later EGL swaps
     * on layer-shell wallpaper unless that follow-up damage/commit lands.
     * Frame callbacks, when they arrive, pace the next swap; a refresh-period
     * timer schedules another attempt on compositors that never send them.
     * The timer cannot interrupt a synchronous native-window buffer wait.
     */
    output_destroy_frame_callback(output);
    output->frame_callback = wl_surface_frame(output->surface);
    wl_callback_add_listener(output->frame_callback, &surface_frame_listener, output);

    EGLBoolean swapped = eglSwapBuffers(output->app->egl.display, output->egl_surface);
    if (!swapped) {
        vivid_wayland_warn("eglSwapBuffers failed egl=0x%x on %s",
                           eglGetError(),
                           output->name[0] ? output->name : "(unnamed)");
        output_destroy_frame_callback(output);
        output->swap_pending = false;
        output->awaiting_frame = false;
        return;
    }

    apply_surface_scale(output);
    damage_whole_surface(output);
    wl_surface_commit(output->surface);

    output->swap_pending = false;
    output->awaiting_frame = true;
    output->next_swap_usec = vivid_wayland_monotonic_usec() + output_frame_period_usec(output);
    output->present_count++;
    wl_display_flush(output->app->display);
}

static int
present_timeout_ms(const VividWaylandApp* app)
{
    int timeout_ms = -1;
    uint64_t now = vivid_wayland_monotonic_usec();
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        const VividWaylandOutput* output = &app->outputs[i];
        if (output->destroyed || !output->swap_pending)
            continue;
        if (!output->awaiting_frame)
            return 0;
        if (now >= output->next_swap_usec)
            return 0;
        int wait_ms = (int)((output->next_swap_usec - now + 999ull) / 1000ull);
        if (wait_ms < 1)
            wait_ms = 1;
        if (timeout_ms < 0 || wait_ms < timeout_ms)
            timeout_ms = wait_ms;
    }
    return timeout_ms;
}

static void
flush_pending_swaps(VividWaylandApp* app)
{
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        VividWaylandOutput* output = &app->outputs[i];
        if (!output->destroyed && output->swap_pending)
            vivid_wayland_output_commit_swap(output);
    }
}

void
vivid_wayland_output_present(VividWaylandOutput* output)
{
    if (!output || !output->app || !output->pending_frame)
        return;
    vivid_wayland_output_ensure_egl(output);
    if (output->egl_surface == EGL_NO_SURFACE)
        return;
    if (!vivid_wayland_egl_make_current(&output->app->egl, output->egl_surface))
        return;

    VividWaylandGeneration* generation = live_generation(output, output->pending_generation);
    VividWaylandBoundBuffer* buffer =
        generation ? live_buffer(generation, output->pending_buffer) : NULL;
    if (!generation || !buffer) {
        vivid_wayland_signal_release_syncobj(generation ? generation->render_node : "",
                                             output->pending_release_fd,
                                             "present-missing-buffer");
        vivid_wayland_close_fd(&output->pending_acquire_fd);
        vivid_wayland_close_fd(&output->pending_release_fd);
        output->pending_frame = false;
        return;
    }

    char context[128];
    snprintf(context,
             sizeof(context),
             "output=%u generation=%llu buffer=%u",
             generation->output_id,
             (unsigned long long)generation->id,
             buffer->index);
    bool acquired =
        vivid_wayland_egl_wait_acquire(&output->app->egl, &output->pending_acquire_fd, context);
    if (!acquired) {
        vivid_wayland_signal_release_syncobj(generation->render_node, output->pending_release_fd,
                                             "acquire-wait-failed");
        vivid_wayland_close_fd(&output->pending_acquire_fd);
        vivid_wayland_close_fd(&output->pending_release_fd);
        output->pending_frame = false;
        return;
    }
    vivid_wayland_close_fd(&output->pending_acquire_fd);

    if (!buffer->import_attempted) {
        buffer->import_attempted = true;
        VividWaylandDmaBufImport import = {
            .fourcc = generation->fourcc,
            .modifier = generation->modifier,
            .width = generation->width,
            .height = generation->height,
            .n_planes = buffer->n_planes,
        };
        memcpy(import.planes, buffer->planes, sizeof(import.planes));
        char error[256] = { 0 };
        buffer->egl_image =
            vivid_wayland_egl_import_image(&output->app->egl, &import, error, sizeof(error));
        if (buffer->egl_image == EGL_NO_IMAGE_KHR) {
            vivid_wayland_producer_send_bind_failed(output->app, generation, 1, error);
            vivid_wayland_signal_release_syncobj(generation->render_node,
                                                 output->pending_release_fd,
                                                 "import-failed");
            vivid_wayland_close_fd(&output->pending_release_fd);
            output->pending_frame = false;
            vivid_wayland_output_retire_generation(output, generation->id);
            return;
        }
        /* EGL retains the dma-buf; protocol fds can close. */
        for (uint32_t p = 0; p < buffer->n_planes; p++)
            vivid_wayland_close_fd(&buffer->planes[p].fd);
        buffer->gl_texture =
            vivid_wayland_egl_create_texture(&output->app->egl, buffer->egl_image, error,
                                             sizeof(error));
        if (!buffer->gl_texture) {
            vivid_wayland_producer_send_bind_failed(output->app, generation, 1, error);
            vivid_wayland_signal_release_syncobj(generation->render_node,
                                                 output->pending_release_fd,
                                                 "texture-failed");
            vivid_wayland_close_fd(&output->pending_release_fd);
            output->pending_frame = false;
            vivid_wayland_output_retire_generation(output, generation->id);
            return;
        }
    }

    int32_t surface_w = 0, surface_h = 0;
    vivid_wayland_output_buffer_size(output, &surface_w, &surface_h);
    VividWaylandRect source = output->source;
    VividWaylandRect dest = output->dest;
    if (dest.w <= 0.0 || dest.h <= 0.0) {
        dest.x = 0;
        dest.y = 0;
        dest.w = (double)surface_w;
        dest.h = (double)surface_h;
    }
    if (source.w <= 0.0 || source.h <= 0.0) {
        source.x = 0;
        source.y = 0;
        source.w = (double)generation->width;
        source.h = (double)generation->height;
    }

    bool drawn = vivid_wayland_egl_draw_frame(&output->app->egl,
                                              buffer->gl_texture,
                                              surface_w,
                                              surface_h,
                                              generation->width,
                                              generation->height,
                                              &source,
                                              &dest,
                                              output->clear_color);
    if (!drawn) {
        vivid_wayland_producer_send_bind_failed(output->app, generation, 2,
                                                "EGL shadow-copy draw failed");
        vivid_wayland_signal_release_syncobj(generation->render_node, output->pending_release_fd,
                                             "draw-failed");
        vivid_wayland_close_fd(&output->pending_release_fd);
        output->pending_frame = false;
        return;
    }

    /*
     * Hand the source slot back to the producer as soon as the GLES draw has
     * finished reading it, but let the GPU decide when that is: the draw sits
     * behind the producer's acquire fence, and on a GPU that is already busy
     * with the wallpaper itself that can be tens of milliseconds. Attaching
     * the draw's completion fence to the release syncobj keeps this
     * single-threaded main loop free for Wayland events and the swap below.
     * Only when no native fence can be exported do we fall back to glFinish()
     * and a host-side signal. Never wait for compositor wl_surface.frame here:
     * that would stall the DMA-BUF pipeline at 0–1 fps on wallpaper
     * layer-shell surfaces.
     */
    int draw_fence_fd = vivid_wayland_egl_export_draw_fence(&output->app->egl, context);
    bool released = false;
    if (draw_fence_fd >= 0) {
        released = vivid_wayland_attach_release_sync_file(generation->render_node,
                                                          output->pending_release_fd,
                                                          draw_fence_fd,
                                                          "shadow-copy-fence");
        vivid_wayland_close_fd(&draw_fence_fd);
    }
    if (!released) {
        vivid_wayland_egl_wait_draw_idle(&output->app->egl);
        vivid_wayland_signal_release_syncobj(generation->render_node, output->pending_release_fd,
                                             "shadow-copy-complete");
    }
    vivid_wayland_close_fd(&output->pending_release_fd);
    output->pending_frame = false;
    output->swap_pending = true;
    vivid_wayland_output_commit_swap(output);
}

static void
draw_clear_placeholder(VividWaylandOutput* output)
{
    vivid_wayland_output_ensure_egl(output);
    if (output->egl_surface == EGL_NO_SURFACE)
        return;
    if (!vivid_wayland_egl_make_current(&output->app->egl, output->egl_surface))
        return;
    int32_t surface_w = 0, surface_h = 0;
    vivid_wayland_output_buffer_size(output, &surface_w, &surface_h);
    float clear[4] = { 0.02f, 0.02f, 0.04f, 1.0f };
    vivid_wayland_egl_draw_frame(&output->app->egl, 0, surface_w, surface_h, 1, 1, NULL, NULL,
                                 clear);
    output->swap_pending = true;
    vivid_wayland_output_commit_swap(output);
}

static void
handle_layer_configure(void* data,
                       struct zwlr_layer_surface_v1* layer,
                       uint32_t serial,
                       uint32_t width,
                       uint32_t height)
{
    VividWaylandOutput* output = data;
    (void)layer;
    output->pending_configure_serial = serial;
    if (width > 0)
        output->configured_w = (int32_t)width;
    if (height > 0)
        output->configured_h = (int32_t)height;
    if (output->configured_w <= 0)
        output->configured_w = output->logical_w > 0 ? output->logical_w : output->mode_w;
    if (output->configured_h <= 0)
        output->configured_h = output->logical_h > 0 ? output->logical_h : output->mode_h;
    bool first = !output->configured;
    output->configured = true;
    vivid_wayland_log("layer configure %s %dx%d serial=%u",
                      output->name[0] ? output->name : "(unnamed)",
                      output->configured_w,
                      output->configured_h,
                      serial);
    if (first && output->app && output->app->initial_topology_ready)
        vivid_wayland_producer_restart_for_topology(output->app, "layer surface configured");
    if (first)
        draw_clear_placeholder(output);
    else
        vivid_wayland_output_ensure_egl(output);
    if (output->pending_frame)
        vivid_wayland_output_present(output);
    if (first && output->app && output->app->caps_sent)
        vivid_wayland_producer_register_outputs(output->app);
    else if (!first && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_layer_closed(void* data, struct zwlr_layer_surface_v1* layer)
{
    VividWaylandOutput* output = data;
    (void)layer;
    vivid_wayland_warn("layer closed for %s", output->name[0] ? output->name : "(unnamed)");
    bool changed_topology = output->configured || output->registered;
    vivid_wayland_output_destroy_layer(output);
    output->layer_closed = true;
    if (output->app) {
        renumber_outputs(output->app);
        if (changed_topology && output->app->initial_topology_ready)
            vivid_wayland_producer_restart_for_topology(output->app, "layer surface closed");
    }
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = handle_layer_configure,
    .closed = handle_layer_closed,
};

static void
handle_fractional_scale(void* data, struct wp_fractional_scale_v1* scale, uint32_t scale_120)
{
    VividWaylandOutput* output = data;
    (void)scale;
    if (output->fractional_120 == scale_120)
        return;
    output->fractional_120 = scale_120;
    if (output->configured) {
        vivid_wayland_output_ensure_egl(output);
        if (output->registered)
            vivid_wayland_producer_update_output(output->app, output);
    }
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = handle_fractional_scale,
};

static void
setup_layer_surface(VividWaylandOutput* output)
{
    if (!output || !output->app)
        return;
    VividWaylandApp* app = output->app;
    if (output->destroyed || output->layer_closed || !output->wl_output || !output->wl_done ||
        !app->compositor || !app->layer_shell || output->layer_surface)
        return;
    output->surface = wl_compositor_create_surface(app->compositor);
    uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
    switch (output->app->layer) {
    case VIVID_WAYLAND_LAYER_BACKGROUND:
        layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        break;
    case VIVID_WAYLAND_LAYER_TOP:
        layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        break;
    case VIVID_WAYLAND_LAYER_OVERLAY:
        layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        break;
    case VIVID_WAYLAND_LAYER_BOTTOM:
    default:
        break;
    }
    output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, output->surface, output->wl_output, layer, VIVID_WAYLAND_SURFACE_NAMESPACE);
    zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_listener, output);
    zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(output->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(output->layer_surface, 0);

    struct wl_region* input = wl_compositor_create_region(app->compositor);
    wl_region_add(input, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_input_region(output->surface, input);
    wl_region_destroy(input);

    struct wl_region* opaque = wl_compositor_create_region(app->compositor);
    wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(output->surface, opaque);
    wl_region_destroy(opaque);

    if (app->viewporter)
        output->viewport = wp_viewporter_get_viewport(app->viewporter, output->surface);
    if (app->fractional_manager) {
        output->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(app->fractional_manager,
                                                                output->surface);
        wp_fractional_scale_v1_add_listener(output->fractional_scale, &fractional_listener, output);
    }
    wl_surface_commit(output->surface);
    renumber_outputs(app);
}

static void
handle_xdg_logical_position(void* data, struct zxdg_output_v1* xdg, int32_t x, int32_t y)
{
    VividWaylandOutput* output = data;
    (void)xdg;
    bool changed = !output->have_xdg_position || output->x != x || output->y != y;
    output->x = x;
    output->y = y;
    output->have_xdg_position = true;
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_xdg_logical_size(void* data, struct zxdg_output_v1* xdg, int32_t width, int32_t height)
{
    VividWaylandOutput* output = data;
    (void)xdg;
    bool changed = output->logical_w != width || output->logical_h != height;
    output->logical_w = width;
    output->logical_h = height;
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_xdg_done(void* data, struct zxdg_output_v1* xdg)
{
    (void)data;
    (void)xdg;
}

static void
handle_xdg_name(void* data, struct zxdg_output_v1* xdg, const char* name)
{
    VividWaylandOutput* output = data;
    (void)xdg;
    char previous[sizeof(output->name)];
    memcpy(previous, output->name, sizeof(previous));
    if (name && name[0])
        vivid_wayland_strlcpy(output->name, name, sizeof(output->name));
    output->have_name = output->name[0] != '\0';
    if (output->wl_done)
        setup_layer_surface(output);
    if (output->registered && strcmp(previous, output->name) != 0)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_xdg_description(void* data, struct zxdg_output_v1* xdg, const char* description)
{
    VividWaylandOutput* output = data;
    (void)xdg;
    bool changed = description && strcmp(output->description, description) != 0;
    if (description)
        vivid_wayland_strlcpy(output->description, description, sizeof(output->description));
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_xdg_logical_position,
    .logical_size = handle_xdg_logical_size,
    .done = handle_xdg_done,
    .name = handle_xdg_name,
    .description = handle_xdg_description,
};

static void
ensure_xdg_output(VividWaylandOutput* output)
{
    if (!output || !output->app || output->destroyed || !output->wl_output || output->xdg_output ||
        !output->app->xdg_output_manager)
        return;
    output->xdg_output = zxdg_output_manager_v1_get_xdg_output(output->app->xdg_output_manager,
                                                               output->wl_output);
    zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
}

static void
handle_output_geometry(void* data,
                       struct wl_output* wl_output,
                       int32_t x,
                       int32_t y,
                       int32_t physical_width,
                       int32_t physical_height,
                       int32_t subpixel,
                       const char* make,
                       const char* model,
                       int32_t transform)
{
    VividWaylandOutput* output = data;
    (void)wl_output;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    bool changed = output->transform != transform;
    if (!output->have_xdg_position) {
        changed = changed || output->x != x || output->y != y;
        output->x = x;
        output->y = y;
    }
    output->transform = transform;
    output->have_geometry = true;
    if ((!output->description[0]) && (make || model)) {
        snprintf(output->description,
                 sizeof(output->description),
                 "%s %s",
                 make ? make : "",
                 model ? model : "");
    }
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_output_mode(void* data,
                   struct wl_output* wl_output,
                   uint32_t flags,
                   int32_t width,
                   int32_t height,
                   int32_t refresh)
{
    VividWaylandOutput* output = data;
    (void)wl_output;
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    int32_t normalized_refresh = refresh > 0 ? refresh : 0;
    bool changed = output->mode_w != width || output->mode_h != height ||
        output->refresh_mhz != normalized_refresh;
    output->mode_w = width;
    output->mode_h = height;
    /* wl_output.refresh is millihertz (60000 = 60 Hz). */
    output->refresh_mhz = normalized_refresh;
    output->have_mode = true;
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_output_done(void* data, struct wl_output* wl_output)
{
    VividWaylandOutput* output = data;
    (void)wl_output;
    output->wl_done = true;
    setup_layer_surface(output);
}

static void
handle_output_scale(void* data, struct wl_output* wl_output, int32_t factor)
{
    VividWaylandOutput* output = data;
    (void)wl_output;
    int32_t normalized_scale = factor > 0 ? factor : 1;
    if (output->scale_int == normalized_scale)
        return;
    output->scale_int = normalized_scale;
    if (output->configured)
        vivid_wayland_output_ensure_egl(output);
    if (output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_output_name(void* data, struct wl_output* wl_output, const char* name)
{
    VividWaylandOutput* output = data;
    (void)wl_output;
    bool changed = name && name[0] && !output->name[0];
    if (name && name[0] && !output->name[0])
        vivid_wayland_strlcpy(output->name, name, sizeof(output->name));
    output->have_name = output->name[0] != '\0';
    if (output->wl_done)
        setup_layer_surface(output);
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static void
handle_output_description(void* data, struct wl_output* wl_output, const char* description)
{
    VividWaylandOutput* output = data;
    (void)wl_output;
    bool changed = description && !output->description[0];
    if (description && !output->description[0])
        vivid_wayland_strlcpy(output->description, description, sizeof(output->description));
    if (changed && output->registered)
        vivid_wayland_producer_update_output(output->app, output);
}

static const struct wl_output_listener output_listener = {
    .geometry = handle_output_geometry,
    .mode = handle_output_mode,
    .done = handle_output_done,
    .scale = handle_output_scale,
    .name = handle_output_name,
    .description = handle_output_description,
};

static VividWaylandOutput*
add_output(VividWaylandApp* app,
           uint32_t name,
           uint32_t advertised_version,
           struct wl_output* wl_output)
{
    uint32_t slot = app->n_outputs;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        if (app->outputs[i].destroyed) {
            slot = i;
            break;
        }
    }
    if (slot == app->n_outputs && app->n_outputs >= VIVID_WAYLAND_MAX_OUTPUTS) {
        vivid_wayland_warn("ignoring extra wl_output (max %d)", VIVID_WAYLAND_MAX_OUTPUTS);
        wl_output_destroy(wl_output);
        return NULL;
    }
    if (slot == app->n_outputs)
        app->n_outputs++;
    VividWaylandOutput* output = &app->outputs[slot];
    memset(output, 0, sizeof(*output));
    output->app = app;
    output->registry_name = name;
    output->wl_output = wl_output;
    output->egl_surface = EGL_NO_SURFACE;
    output->pending_acquire_fd = -1;
    output->pending_release_fd = -1;
    output->scale_int = 1;
    output->clear_color[3] = 1.0f;
    /* wl_output.done was introduced in version 2; version 1 has no batch marker. */
    output->wl_done = advertised_version < 2;
    output->monitor_index = slot;
    output->consumer_output_id = output->monitor_index + 1;
    wl_output_add_listener(wl_output, &output_listener, output);
    ensure_xdg_output(output);
    if (output->wl_done)
        setup_layer_surface(output);
    return output;
}

static void
remove_output_by_name(VividWaylandApp* app, uint32_t registry_name)
{
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        VividWaylandOutput* output = &app->outputs[i];
        if (output->registry_name != registry_name)
            continue;
        vivid_wayland_log("output removed %s", output->name[0] ? output->name : "(unnamed)");
        bool changed_topology = output->layer_surface || output->configured || output->registered;
        vivid_wayland_output_destroy_layer(output);
        if (output->xdg_output) {
            zxdg_output_v1_destroy(output->xdg_output);
            output->xdg_output = NULL;
        }
        if (output->wl_output) {
            wl_output_destroy(output->wl_output);
            output->wl_output = NULL;
        }
        output->destroyed = true;
        renumber_outputs(app);
        if (changed_topology && app->initial_topology_ready)
            vivid_wayland_producer_restart_for_topology(app, "wl_output removed");
        return;
    }
}

static void
handle_global(void* data,
              struct wl_registry* registry,
              uint32_t name,
              const char* interface,
              uint32_t version)
{
    VividWaylandApp* app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                           version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        vivid_wayland_input_bind_shm(app, registry, name, version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        vivid_wayland_input_bind_seat(app, registry, name, version);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        app->xdg_output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                                                   version < 3 ? version : 3);
        /* Registry globals have no ordering guarantee; attach outputs seen earlier. */
        for (uint32_t i = 0; i < app->n_outputs; i++)
            ensure_xdg_output(&app->outputs[i]);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        app->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        app->fractional_manager =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output* wl_output =
            wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        add_output(app, name, version, wl_output);
    }
}

static void
handle_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)registry;
    if (vivid_wayland_input_handle_global_remove(data, name))
        return;
    remove_output_by_name(data, name);
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void
renumber_outputs(VividWaylandApp* app)
{
    uint32_t index = 0;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        if (app->outputs[i].destroyed || app->outputs[i].layer_closed)
            continue;
        app->outputs[i].monitor_index = index;
        app->outputs[i].consumer_output_id = index + 1;
        index++;
    }
}

static bool
bind_output_layers(VividWaylandApp* app)
{
    if (!app->layer_shell) {
        vivid_wayland_error("compositor does not advertise zwlr_layer_shell_v1 "
                            "(GNOME/Mutter cannot use this consumer; use consumer/gnome)");
        return false;
    }
    if (!app->compositor) {
        vivid_wayland_error("missing wl_compositor");
        return false;
    }
    renumber_outputs(app);
    uint32_t created = 0;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        VividWaylandOutput* output = &app->outputs[i];
        if (output->destroyed)
            continue;
        setup_layer_surface(output);
        if (output->layer_surface)
            created++;
    }
    if (created == 0) {
        vivid_wayland_error("no usable wl_output");
        return false;
    }
    return true;
}

bool
vivid_wayland_app_init(VividWaylandApp* app,
                       enum VividWaylandLayer layer)
{
    memset(app, 0, sizeof(*app));
    app->producer_fd = -1;
    app->layer = layer;
    app->pointer_axis_source = VIVID_DISPLAY_AXIS_CONTINUOUS;
    app->running = true;
    app->reconnect_delay_ms = VIVID_WAYLAND_RECONNECT_INITIAL_MS;
    vivid_wayland_default_socket_path(app->socket_path, sizeof(app->socket_path));

    app->display = wl_display_connect(NULL);
    if (!app->display) {
        vivid_wayland_error("wl_display_connect failed (is WAYLAND_DISPLAY set?)");
        return false;
    }
    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registry_listener, app);
    wl_display_roundtrip(app->display);
    wl_display_roundtrip(app->display);
    if (!app->seat)
        vivid_wayland_warn("compositor advertises no wl_seat; pointer interaction is unavailable");

    if (!bind_output_layers(app))
        return false;
    if (!vivid_wayland_egl_init(&app->egl, app->display))
        return false;

    /*
     * Configure may have arrived before EGL was ready. Retry the first commit
     * so we do not sit forever with an unacked zwlr_layer_surface configure.
     */
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        VividWaylandOutput* output = &app->outputs[i];
        if (output->destroyed || !output->configured)
            continue;
        if (output->egl_surface == EGL_NO_SURFACE)
            draw_clear_placeholder(output);
        if (output->pending_frame)
            vivid_wayland_output_present(output);
    }

    wl_display_roundtrip(app->display);
    app->initial_topology_ready = true;
    return true;
}

void
vivid_wayland_app_request_stop(VividWaylandApp* app)
{
    if (app)
        app->running = false;
}

void
vivid_wayland_app_finish(VividWaylandApp* app)
{
    if (!app)
        return;
    vivid_wayland_producer_disconnect(app, false);
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        vivid_wayland_output_destroy_layer(&app->outputs[i]);
        if (app->outputs[i].xdg_output)
            zxdg_output_v1_destroy(app->outputs[i].xdg_output);
        if (app->outputs[i].wl_output)
            wl_output_destroy(app->outputs[i].wl_output);
    }
    vivid_wayland_input_finish(app);
    vivid_wayland_egl_finish(&app->egl);
    if (app->fractional_manager)
        wp_fractional_scale_manager_v1_destroy(app->fractional_manager);
    if (app->viewporter)
        wp_viewporter_destroy(app->viewporter);
    if (app->xdg_output_manager)
        zxdg_output_manager_v1_destroy(app->xdg_output_manager);
    if (app->layer_shell)
        zwlr_layer_shell_v1_destroy(app->layer_shell);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display)
        wl_display_disconnect(app->display);
    memset(app, 0, sizeof(*app));
    app->producer_fd = -1;
}

int
vivid_wayland_app_run(VividWaylandApp* app)
{
    g_app_for_signal = app;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    vivid_wayland_producer_connect(app);

    while (app->running) {
        while (wl_display_prepare_read(app->display) != 0) {
            if (wl_display_dispatch_pending(app->display) < 0) {
                vivid_wayland_error("wl_display_dispatch_pending failed");
                app->running = false;
                break;
            }
        }
        if (!app->running) {
            wl_display_cancel_read(app->display);
            break;
        }
        wl_display_flush(app->display);

        struct pollfd fds[2];
        nfds_t n = 0;
        fds[n].fd = wl_display_get_fd(app->display);
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
        int producer_index = -1;
        if (app->producer_fd >= 0) {
            producer_index = (int)n;
            fds[n].fd = app->producer_fd;
            fds[n].events = POLLIN;
            if (app->outbox_off < app->outbox_len || app->producer_connecting)
                fds[n].events |= POLLOUT;
            fds[n].revents = 0;
            n++;
        }

        int timeout_ms = present_timeout_ms(app);
        if (app->producer_fd < 0) {
            uint64_t now = vivid_wayland_monotonic_usec();
            int reconnect_ms = 0;
            if (now < app->next_reconnect_usec)
                reconnect_ms = (int)((app->next_reconnect_usec - now) / 1000ull);
            if (timeout_ms < 0 || reconnect_ms < timeout_ms)
                timeout_ms = reconnect_ms;
        }

        int rc = poll(fds, n, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(app->display);
                continue;
            }
            vivid_wayland_error("poll failed: %s", strerror(errno));
            wl_display_cancel_read(app->display);
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(app->display) < 0) {
                vivid_wayland_error("wl_display_read_events failed");
                app->running = false;
                break;
            }
        } else {
            wl_display_cancel_read(app->display);
        }
        if (wl_display_dispatch_pending(app->display) < 0) {
            vivid_wayland_error("wl_display_dispatch_pending failed");
            app->running = false;
            break;
        }

        if (producer_index >= 0) {
            if (fds[producer_index].revents & (POLLIN | POLLERR | POLLHUP))
                vivid_wayland_producer_on_readable(app);
            if (app->producer_fd >= 0 && (fds[producer_index].revents & POLLOUT))
                vivid_wayland_producer_on_writable(app);
        }

        if (app->producer_fd < 0 &&
            vivid_wayland_monotonic_usec() >= app->next_reconnect_usec)
            vivid_wayland_producer_connect(app);

        flush_pending_swaps(app);
    }

    g_app_for_signal = NULL;
    return 0;
}
