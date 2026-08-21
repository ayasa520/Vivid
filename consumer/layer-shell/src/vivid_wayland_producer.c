#define _GNU_SOURCE

#include "vivid_wayland_app.h"

#include "vivid_wayland_log.h"
#include "vivid_wayland_util.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static uint64_t
json_u64(json_object* object, const char* key, uint64_t fallback)
{
    json_object* member = NULL;
    if (!object || !json_object_object_get_ex(object, key, &member) || !member)
        return fallback;
    if (json_object_is_type(member, json_type_string)) {
        const char* text = json_object_get_string(member);
        if (!text || !text[0])
            return fallback;
        char* end = NULL;
        unsigned long long value = strtoull(text, &end, 0);
        if (end == text)
            return fallback;
        return (uint64_t)value;
    }
    if (json_object_is_type(member, json_type_int) || json_object_is_type(member, json_type_double))
        return json_object_get_uint64(member);
    return fallback;
}

static uint32_t
json_u32(json_object* object, const char* key, uint32_t fallback)
{
    return (uint32_t)json_u64(object, key, fallback);
}

static const char*
json_str(json_object* object, const char* key, const char* fallback)
{
    json_object* member = NULL;
    if (!object || !json_object_object_get_ex(object, key, &member) || !member)
        return fallback;
    const char* text = json_object_get_string(member);
    return text ? text : fallback;
}

static bool
json_bool_default(json_object* object, const char* key, bool fallback)
{
    json_object* member = NULL;
    if (!object || !json_object_object_get_ex(object, key, &member) || !member)
        return fallback;
    return json_object_get_boolean(member);
}

static double
json_member_double(json_object* object, const char* key, double fallback)
{
    json_object* member = NULL;
    if (!object || !json_object_object_get_ex(object, key, &member) || !member)
        return fallback;
    return json_object_get_double(member);
}

static void
outbox_clear(VividWaylandApp* app)
{
    free(app->outbox);
    app->outbox = NULL;
    app->outbox_len = 0;
    app->outbox_off = 0;
}

static bool
queue_bytes(VividWaylandApp* app, const uint8_t* data, size_t len)
{
    uint8_t* next = realloc(app->outbox, app->outbox_len + len);
    if (!next)
        return false;
    memcpy(next + app->outbox_len, data, len);
    app->outbox = next;
    app->outbox_len += len;
    return true;
}

static bool
flush_outbox(VividWaylandApp* app)
{
    if (app->producer_fd < 0 || app->producer_connecting || !app->outbox)
        return true;
    while (app->outbox_off < app->outbox_len) {
        ssize_t sent = vivid_display_send_bytes_nonblocking(
            app->producer_fd,
            app->outbox + app->outbox_off,
            app->outbox_len - app->outbox_off);
        if (sent < 0) {
            vivid_wayland_error("producer write failed: %s", strerror((int)-sent));
            vivid_wayland_producer_disconnect(app, true);
            return false;
        }
        if (sent == 0)
            return true;
        app->outbox_off += (size_t)sent;
    }
    outbox_clear(app);
    return true;
}

static bool
send_json(VividWaylandApp* app, uint16_t opcode, json_object* object)
{
    if (!object || app->producer_fd < 0)
        return false;
    const char* text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    if (!text)
        return false;
    size_t body_len = strlen(text);
    uint8_t header[4];
    uint16_t total = (uint16_t)(4u + body_len);
    header[0] = (uint8_t)(opcode & 0xffu);
    header[1] = (uint8_t)((opcode >> 8) & 0xffu);
    header[2] = (uint8_t)(total & 0xffu);
    header[3] = (uint8_t)((total >> 8) & 0xffu);
    if (!queue_bytes(app, header, sizeof(header)) ||
        !queue_bytes(app, (const uint8_t*)text, body_len)) {
        vivid_wayland_error("outbox OOM");
        return false;
    }
    return flush_outbox(app);
}

static bool
send_pointer_frame(VividWaylandApp* app, uint16_t opcode, const uint8_t* body, size_t body_len)
{
    if (!app || app->producer_fd < 0 || !app->handshake_complete || !body ||
        body_len > VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES) {
        return false;
    }

    /*
     * Pointer frames never carry file descriptors. Queue one contiguous frame
     * so an allocation failure cannot leave a header without its body in the
     * stream outbox. The axis body is the largest pointer message.
     */
    uint8_t frame[VIVID_DISPLAY_FRAME_HEADER_BYTES + VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES];
    const uint16_t total = (uint16_t)(VIVID_DISPLAY_FRAME_HEADER_BYTES + body_len);
    frame[0] = (uint8_t)(opcode & 0xffu);
    frame[1] = (uint8_t)((opcode >> 8) & 0xffu);
    frame[2] = (uint8_t)(total & 0xffu);
    frame[3] = (uint8_t)((total >> 8) & 0xffu);
    memcpy(frame + VIVID_DISPLAY_FRAME_HEADER_BYTES, body, body_len);
    if (!queue_bytes(app, frame, (size_t)total)) {
        vivid_wayland_error("pointer outbox OOM");
        return false;
    }
    return flush_outbox(app);
}

static json_object*
make_hello(void)
{
    json_object* object = json_object_new_object();
    json_object* features = json_object_new_array();
    json_object_object_add(object, VIVID_JSON_HELLO_PROTOCOL,
                           json_object_new_string(VIVID_DISPLAY_PROTOCOL_NAME));
    json_object_object_add(object, VIVID_JSON_HELLO_VERSION,
                           json_object_new_int((int)VIVID_DISPLAY_PROTOCOL_VERSION));
    json_object_object_add(object, VIVID_JSON_HELLO_CLIENT_NAME,
                           json_object_new_string(VIVID_WAYLAND_CLIENT_NAME));
    json_object_object_add(object, VIVID_JSON_HELLO_ROLE, json_object_new_string("consumer"));
    json_object_array_add(features, json_object_new_string("dmabuf-egl-image-v1"));
    json_object_array_add(features, json_object_new_string("dmabuf-caps-v3"));
    json_object_array_add(features, json_object_new_string("explicit-sync-fd-v1"));
    json_object_array_add(features, json_object_new_string("dmabuf-bind-failed-v1"));
    json_object_array_add(features, json_object_new_string("dmabuf-unbind-done-v1"));
    json_object_array_add(features, json_object_new_string("unbind-v2"));
    json_object_array_add(features, json_object_new_string("dmabuf-shadow-copy-v1"));
    json_object_object_add(object, VIVID_JSON_HELLO_FEATURES, features);
    return object;
}

static bool
send_hello_and_caps(VividWaylandApp* app)
{
    json_object* caps = vivid_wayland_egl_build_dmabuf_caps(&app->egl);
    if (!caps) {
        vivid_wayland_error("consumer DMA-BUF capabilities are incomplete; stopping");
        app->running = false;
        return false;
    }

    json_object* hello = make_hello();
    bool hello_sent = send_json(app, VIVID_DISPLAY_REQ_HELLO, hello);
    json_object_put(hello);

    json_object* envelope = json_object_new_object();
    json_object_object_add(envelope, VIVID_JSON_CONSUMER_CAPS_DMABUF_CAPS, caps);
    vivid_wayland_log("sending consumer caps backend=wayland-egl-gles-shadow render-node=%s",
                      app->egl.identity.render_node[0] ? app->egl.identity.render_node : "(unknown)");
    bool caps_sent = hello_sent && send_json(app, VIVID_DISPLAY_REQ_CONSUMER_CAPS, envelope);
    json_object_put(envelope);
    app->caps_sent = caps_sent;
    return caps_sent;
}

static json_object*
output_payload(const VividWaylandOutput* output, uint32_t layout_count, bool is_update)
{
    json_object* object = json_object_new_object();
    double scale = vivid_wayland_output_scale(output);
    int32_t physical_w = 0;
    int32_t physical_h = 0;
    vivid_wayland_output_buffer_size(output, &physical_w, &physical_h);
    int32_t logical_w = output->configured_w > 0 ? output->configured_w : output->logical_w;
    int32_t logical_h = output->configured_h > 0 ? output->configured_h : output->logical_h;
    if (logical_w <= 0)
        logical_w = output->mode_w > 0 ? output->mode_w : 1280;
    if (logical_h <= 0)
        logical_h = output->mode_h > 0 ? output->mode_h : 720;

    if (is_update) {
        json_object_object_add(object,
                               VIVID_JSON_UPDATE_OUTPUT_OUTPUT_ID,
                               json_object_new_int64(output->producer_output_id));
    }
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_CONSUMER_OUTPUT_ID
                                     : VIVID_JSON_REGISTER_OUTPUT_CONSUMER_OUTPUT_ID,
                           json_object_new_int64(output->consumer_output_id));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_MONITOR_INDEX
                                     : VIVID_JSON_REGISTER_OUTPUT_MONITOR_INDEX,
                           json_object_new_int64(output->monitor_index));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_LAYOUT_OUTPUT_COUNT
                                     : VIVID_JSON_REGISTER_OUTPUT_LAYOUT_OUTPUT_COUNT,
                           json_object_new_int64(layout_count));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_X : VIVID_JSON_REGISTER_OUTPUT_X,
                           json_object_new_int(output->x));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_Y : VIVID_JSON_REGISTER_OUTPUT_Y,
                           json_object_new_int(output->y));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_WIDTH
                                     : VIVID_JSON_REGISTER_OUTPUT_WIDTH,
                           json_object_new_int(logical_w));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_HEIGHT
                                     : VIVID_JSON_REGISTER_OUTPUT_HEIGHT,
                           json_object_new_int(logical_h));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_PHYSICAL_WIDTH
                                     : VIVID_JSON_REGISTER_OUTPUT_PHYSICAL_WIDTH,
                           json_object_new_int(physical_w));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_PHYSICAL_HEIGHT
                                     : VIVID_JSON_REGISTER_OUTPUT_PHYSICAL_HEIGHT,
                           json_object_new_int(physical_h));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_SCALE
                                     : VIVID_JSON_REGISTER_OUTPUT_SCALE,
                           json_object_new_double(scale));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_TRANSFORM
                                     : VIVID_JSON_REGISTER_OUTPUT_TRANSFORM,
                           json_object_new_string(vivid_wayland_transform_string(output->transform)));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_REFRESH_RATE_MHZ
                                     : VIVID_JSON_REGISTER_OUTPUT_REFRESH_RATE_MHZ,
                           json_object_new_int(output->refresh_mhz));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_DESKTOP
                                     : VIVID_JSON_REGISTER_OUTPUT_DESKTOP,
                           json_object_new_string(VIVID_WAYLAND_DESKTOP_ID));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_DISPLAY_KEY
                                     : VIVID_JSON_REGISTER_OUTPUT_DISPLAY_KEY,
                           json_object_new_string(output->name[0] ? output->name : "unknown"));
    json_object_object_add(object,
                           is_update ? VIVID_JSON_UPDATE_OUTPUT_DISPLAY_NAME
                                     : VIVID_JSON_REGISTER_OUTPUT_DISPLAY_NAME,
                           json_object_new_string(output->description[0] ? output->description
                                                                         : output->name));
    return object;
}

static bool
output_is_publishable(const VividWaylandOutput* output)
{
    return output && !output->destroyed && !output->layer_closed && output->layer_surface &&
        output->configured;
}

void
vivid_wayland_producer_register_outputs(VividWaylandApp* app)
{
    if (app->producer_fd < 0 || !app->caps_sent)
        return;
    uint32_t count = 0;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        if (output_is_publishable(&app->outputs[i]))
            count++;
    }
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        VividWaylandOutput* output = &app->outputs[i];
        if (!output_is_publishable(output) || output->registered)
            continue;
        json_object* payload = output_payload(output, count, false);
        vivid_wayland_log(
            "REGISTER_OUTPUT monitor=%u consumerOutputId=%u logical=%dx%d scale=%.3f name=%s",
            output->monitor_index,
            output->consumer_output_id,
            output->configured_w,
            output->configured_h,
            vivid_wayland_output_scale(output),
            output->name[0] ? output->name : "(unnamed)");
        send_json(app, VIVID_DISPLAY_REQ_REGISTER_OUTPUT, payload);
        json_object_put(payload);
        output->registered = true;
    }
}

void
vivid_wayland_producer_update_output(VividWaylandApp* app, VividWaylandOutput* output)
{
    if (!app || !output_is_publishable(output) || app->producer_fd < 0 || !output->registered ||
        output->producer_output_id == 0)
        return;
    uint32_t count = 0;
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        if (output_is_publishable(&app->outputs[i]))
            count++;
    }
    json_object* payload = output_payload(output, count, true);
    send_json(app, VIVID_DISPLAY_REQ_UPDATE_OUTPUT, payload);
    json_object_put(payload);
}

void
vivid_wayland_producer_send_bind_failed(VividWaylandApp* app,
                                        const VividWaylandGeneration* generation,
                                        uint32_t reason,
                                        const char* message)
{
    if (!app || !generation || generation->fourcc == 0)
        return;
    json_object* object = json_object_new_object();
    char generation_text[32];
    char modifier_text[32];
    snprintf(generation_text, sizeof(generation_text), "%llu", (unsigned long long)generation->id);
    snprintf(modifier_text, sizeof(modifier_text), "%llu", (unsigned long long)generation->modifier);
    json_object_object_add(object, VIVID_JSON_BIND_FAILED_OUTPUT_ID,
                           json_object_new_int64(generation->output_id));
    json_object_object_add(object, VIVID_JSON_BIND_FAILED_GENERATION,
                           json_object_new_string(generation_text));
    json_object_object_add(object, VIVID_JSON_BIND_FAILED_FOURCC,
                           json_object_new_int64(generation->fourcc));
    json_object_object_add(object, VIVID_JSON_BIND_FAILED_MODIFIER,
                           json_object_new_string(modifier_text));
    json_object_object_add(object, VIVID_JSON_BIND_FAILED_REASON, json_object_new_int((int)reason));
    json_object_object_add(object, VIVID_JSON_BIND_FAILED_MESSAGE,
                           json_object_new_string(message ? message : "DMA-BUF import failed"));
    vivid_wayland_warn("BIND_FAILED output=%u generation=%llu fourcc=0x%08x reason=%u: %s",
                       generation->output_id,
                       (unsigned long long)generation->id,
                       generation->fourcc,
                       reason,
                       message ? message : "");
    send_json(app, VIVID_DISPLAY_REQ_BIND_FAILED, object);
    json_object_put(object);
}

void
vivid_wayland_producer_send_unbind_done(VividWaylandApp* app,
                                        uint32_t output_id,
                                        uint64_t generation)
{
    json_object* object = json_object_new_object();
    char generation_text[32];
    snprintf(generation_text, sizeof(generation_text), "%llu", (unsigned long long)generation);
    json_object_object_add(object, VIVID_JSON_UNBIND_DONE_OUTPUT_ID,
                           json_object_new_int64(output_id));
    json_object_object_add(object, VIVID_JSON_UNBIND_DONE_GENERATION,
                           json_object_new_string(generation_text));
    send_json(app, VIVID_DISPLAY_REQ_UNBIND_DONE, object);
    json_object_put(object);
}

static bool
pointer_output_ready(const VividWaylandOutput* output)
{
    return output && output->app && output->registered && output->producer_output_id != 0;
}

void
vivid_wayland_producer_send_pointer_motion(VividWaylandOutput* output,
                                            double x,
                                            double y,
                                            uint64_t time_usec)
{
    if (!pointer_output_ready(output))
        return;
    uint8_t body[VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES];
    vivid_display_pointer_motion_body_write(body,
                                            output->producer_output_id,
                                            x,
                                            y,
                                            time_usec);
    send_pointer_frame(output->app, VIVID_DISPLAY_REQ_POINTER_MOTION, body, sizeof(body));
}

void
vivid_wayland_producer_send_pointer_button(VividWaylandOutput* output,
                                            double x,
                                            double y,
                                            uint32_t button,
                                            uint32_t state,
                                            uint64_t time_usec)
{
    if (!pointer_output_ready(output))
        return;
    uint8_t body[VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES];
    vivid_display_pointer_button_body_write(body,
                                            output->producer_output_id,
                                            x,
                                            y,
                                            button,
                                            state,
                                            time_usec);
    send_pointer_frame(output->app, VIVID_DISPLAY_REQ_POINTER_BUTTON, body, sizeof(body));
}

void
vivid_wayland_producer_send_pointer_axis(VividWaylandOutput* output,
                                          double x,
                                          double y,
                                          double delta_x,
                                          double delta_y,
                                          uint32_t source,
                                          uint64_t time_usec)
{
    if (!pointer_output_ready(output))
        return;
    uint8_t body[VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES];
    vivid_display_pointer_axis_body_write(body,
                                          output->producer_output_id,
                                          x,
                                          y,
                                          delta_x,
                                          delta_y,
                                          source,
                                          time_usec);
    send_pointer_frame(output->app, VIVID_DISPLAY_REQ_POINTER_AXIS, body, sizeof(body));
}

static void
close_recv_fds(VividDisplayRecvState* state)
{
    if (!state)
        return;
    for (size_t i = 0; i < state->n_fds; i++) {
        int fd = vivid_display_recv_state_steal_fd(state, i);
        vivid_wayland_close_fd(&fd);
    }
}

static void
close_generation_fds(VividWaylandGeneration* generation)
{
    if (!generation)
        return;
    /*
     * Scan every slot rather than only generation->n_buffers. The buffer being
     * decoded can already own one or more stolen SCM_RIGHTS descriptors before
     * it is complete enough to increment n_buffers.
     */
    for (uint32_t i = 0; i < VIVID_WAYLAND_MAX_BUFFERS; i++) {
        VividWaylandBoundBuffer* buffer = &generation->buffers[i];
        for (uint32_t p = 0; p < buffer->n_planes; p++)
            vivid_wayland_close_fd(&buffer->planes[p].fd);
        buffer->n_planes = 0;
    }
}

static VividWaylandGeneration*
find_generation(VividWaylandOutput* output, uint64_t id)
{
    if (!output)
        return NULL;
    for (uint32_t i = 0; i < output->n_generations; i++) {
        if (output->generations[i].id == id)
            return &output->generations[i];
    }
    return NULL;
}

static VividWaylandBoundBuffer*
find_buffer(VividWaylandGeneration* generation, uint32_t index)
{
    if (!generation)
        return NULL;
    for (uint32_t i = 0; i < generation->n_buffers; i++) {
        if (generation->buffers[i].index == index)
            return &generation->buffers[i];
    }
    return NULL;
}

static void
handle_welcome(VividWaylandApp* app, json_object* object)
{
    app->negotiated_version = json_u32(object, VIVID_JSON_WELCOME_NEGOTIATED_VERSION,
                                       VIVID_DISPLAY_PROTOCOL_VERSION);
    app->handshake_complete = true;
    app->reconnect_delay_ms = VIVID_WAYLAND_RECONNECT_INITIAL_MS;
    vivid_wayland_log("welcome negotiatedVersion=%u", app->negotiated_version);
}

static void
handle_output_accepted(VividWaylandApp* app, json_object* object)
{
    uint32_t consumer_id = json_u32(object, VIVID_JSON_OUTPUT_ACCEPTED_CONSUMER_OUTPUT_ID, 0);
    uint32_t output_id = json_u32(object, VIVID_JSON_OUTPUT_ACCEPTED_OUTPUT_ID, 0);
    VividWaylandOutput* output = vivid_wayland_app_find_output_by_consumer_id(app, consumer_id);
    if (!output || output_id == 0) {
        vivid_wayland_warn("invalid OUTPUT_ACCEPTED consumer=%u output=%u", consumer_id, output_id);
        return;
    }
    output->producer_output_id = output_id;
    vivid_wayland_log("output accepted consumer=%u output=%u name=%s",
                      consumer_id,
                      output_id,
                      output->name);
    if (app->pointer_output == output) {
        vivid_wayland_producer_send_pointer_motion(output,
                                                    app->pointer_x,
                                                    app->pointer_y,
                                                    vivid_wayland_monotonic_usec());
    }
}

static void
handle_bind_buffers(VividWaylandApp* app, json_object* object, VividDisplayRecvState* state)
{
    uint32_t output_id = json_u32(object, VIVID_JSON_BIND_BUFFERS_OUTPUT_ID, 0);
    VividWaylandOutput* output = vivid_wayland_app_find_output_by_producer_id(app, output_id);
    if (!output) {
        vivid_wayland_warn("BIND_BUFFERS for unknown output=%u", output_id);
        close_recv_fds(state);
        return;
    }

    VividWaylandGeneration generation;
    memset(&generation, 0, sizeof(generation));
    generation.output_id = output_id;
    generation.id = json_u64(object, VIVID_JSON_BIND_BUFFERS_GENERATION, 0);
    generation.width = (int32_t)json_u32(object, VIVID_JSON_BIND_BUFFERS_WIDTH, 0);
    generation.height = (int32_t)json_u32(object, VIVID_JSON_BIND_BUFFERS_HEIGHT, 0);
    generation.fourcc = json_u32(object, VIVID_JSON_BIND_BUFFERS_FOURCC, 0);
    generation.modifier = json_u64(object, VIVID_JSON_BIND_BUFFERS_MODIFIER,
                                   VIVID_WAYLAND_DRM_FORMAT_MOD_INVALID);
    generation.planes_per_buffer = json_u32(object, VIVID_JSON_BIND_BUFFERS_PLANES_PER_BUFFER, 0);
    vivid_wayland_strlcpy(generation.render_node,
                          json_str(object, VIVID_JSON_BIND_BUFFERS_PRODUCER_RENDER_NODE, ""),
                          sizeof(generation.render_node));
    if (!generation.render_node[0]) {
        vivid_wayland_strlcpy(generation.render_node,
                              json_str(object, VIVID_JSON_BIND_BUFFERS_RENDER_NODE, ""),
                              sizeof(generation.render_node));
    }
    vivid_wayland_strlcpy(generation.presentation_path,
                          json_str(object, VIVID_JSON_BIND_BUFFERS_PRESENTATION_PATH, ""),
                          sizeof(generation.presentation_path));
    generation.premultiplied = json_bool_default(object, VIVID_JSON_BIND_BUFFERS_PREMULTIPLIED, true);

    json_object* buffers = NULL;
    json_object_object_get_ex(object, VIVID_JSON_BIND_BUFFERS_BUFFERS, &buffers);
    if (generation.output_id == 0 || generation.id == 0 || generation.width <= 0 ||
        generation.height <= 0 || generation.fourcc == 0 || !buffers ||
        json_object_array_length(buffers) == 0) {
        const char* message = "BIND_BUFFERS is missing output/generation/size/format/buffers";
        close_recv_fds(state);
        vivid_wayland_producer_send_bind_failed(app, &generation, 1, message);
        return;
    }
    size_t n_buffers = json_object_array_length(buffers);
    if (n_buffers > VIVID_WAYLAND_MAX_BUFFERS) {
        close_recv_fds(state);
        vivid_wayland_producer_send_bind_failed(
            app, &generation, 1, "BIND_BUFFERS exceeds the consumer buffer limit");
        return;
    }
    if (strcmp(generation.presentation_path, "shadow-copy") != 0) {
        char message[160];
        snprintf(message,
                 sizeof(message),
                 "wayland consumer supports only shadow-copy presentation; producer selected '%s'",
                 generation.presentation_path[0] ? generation.presentation_path : "(missing)");
        close_recv_fds(state);
        vivid_wayland_producer_send_bind_failed(app, &generation, 1, message);
        return;
    }
    if (!vivid_wayland_release_syncobj_supported(generation.render_node, "bind-buffers")) {
        close_recv_fds(state);
        vivid_wayland_producer_send_bind_failed(
            app, &generation, 1, "producer render node cannot import and signal release syncobjs");
        return;
    }

    for (size_t i = 0; i < n_buffers; i++) {
        json_object* buffer_obj = json_object_array_get_idx(buffers, i);
        VividWaylandBoundBuffer* buffer = &generation.buffers[generation.n_buffers];
        memset(buffer, 0, sizeof(*buffer));
        buffer->egl_image = EGL_NO_IMAGE_KHR;
        buffer->index = json_u32(buffer_obj, VIVID_JSON_BIND_BUFFERS_BUFFER_INDEX, (uint32_t)i);
        buffer->size = json_u64(buffer_obj, VIVID_JSON_BIND_BUFFERS_BUFFER_SIZE, 0);
        json_object* planes = NULL;
        json_object_object_get_ex(buffer_obj, VIVID_JSON_BIND_BUFFERS_BUFFER_PLANES, &planes);
        if (!planes || json_object_array_length(planes) == 0) {
            close_recv_fds(state);
            close_generation_fds(&generation);
            vivid_wayland_producer_send_bind_failed(app, &generation, 1,
                                                    "BIND_BUFFERS buffer has no planes");
            return;
        }
        size_t n_planes = json_object_array_length(planes);
        if (n_planes > VIVID_DISPLAY_DMABUF_MAX_PLANES) {
            close_recv_fds(state);
            close_generation_fds(&generation);
            vivid_wayland_producer_send_bind_failed(
                app, &generation, 1, "BIND_BUFFERS exceeds the display-v1 plane limit");
            return;
        }
        for (size_t p = 0; p < n_planes; p++) {
            json_object* plane_obj = json_object_array_get_idx(planes, p);
            uint32_t fd_index = json_u32(plane_obj, VIVID_JSON_BIND_BUFFERS_PLANE_FD_INDEX, 0);
            int fd = vivid_display_recv_state_steal_fd(state, fd_index);
            if (fd < 0) {
                close_recv_fds(state);
                close_generation_fds(&generation);
                vivid_wayland_producer_send_bind_failed(app, &generation, 1,
                                                        "BIND_BUFFERS references missing fd index");
                return;
            }
            buffer->planes[p].fd = fd;
            buffer->planes[p].stride = json_u32(plane_obj, VIVID_JSON_BIND_BUFFERS_PLANE_STRIDE, 0);
            buffer->planes[p].offset = json_u32(plane_obj, VIVID_JSON_BIND_BUFFERS_PLANE_OFFSET, 0);
            buffer->n_planes++;
        }
        if (generation.planes_per_buffer != 0 &&
            buffer->n_planes != generation.planes_per_buffer) {
            close_recv_fds(state);
            close_generation_fds(&generation);
            vivid_wayland_producer_send_bind_failed(app, &generation, 1,
                                                    "BIND_BUFFERS planes_per_buffer mismatch");
            return;
        }
        generation.n_buffers++;
    }
    close_recv_fds(state);

    /*
     * Keep the current generation alive until the replacement message owns a
     * complete, internally consistent set of DMA-BUF descriptors. This makes
     * BIND_BUFFERS transactional from the output's point of view: rejecting a
     * malformed replacement cannot blank an otherwise usable wallpaper.
     */
    vivid_wayland_output_retire_generations(output, "bind-replaced");
    if (output->n_generations >= VIVID_WAYLAND_MAX_GENERATIONS)
        vivid_wayland_output_retire_generations(output, "generation-overflow");
    if (output->n_generations >= VIVID_WAYLAND_MAX_GENERATIONS) {
        close_generation_fds(&generation);
        vivid_wayland_producer_send_bind_failed(app, &generation, 3, "too many live generations");
        return;
    }
    output->generations[output->n_generations++] = generation;
    vivid_wayland_log(
        "bound output=%u generation=%llu size=%dx%d fourcc=0x%08x modifier=0x%016llx "
        "planes=%u buffers=%u presentation=%s producer-render-node=%s",
        generation.output_id,
        (unsigned long long)generation.id,
        generation.width,
        generation.height,
        generation.fourcc,
        (unsigned long long)generation.modifier,
        generation.planes_per_buffer,
        generation.n_buffers,
        generation.presentation_path,
        generation.render_node[0] ? generation.render_node : "(unknown)");
}

static void
handle_set_config(VividWaylandApp* app, json_object* object)
{
    uint32_t output_id = json_u32(object, VIVID_JSON_SET_CONFIG_OUTPUT_ID, 0);
    uint64_t generation_id = json_u64(object, VIVID_JSON_SET_CONFIG_GENERATION, 0);
    VividWaylandOutput* output = vivid_wayland_app_find_output_by_producer_id(app, output_id);
    if (!output) {
        vivid_wayland_warn("SET_CONFIG for unknown output=%u", output_id);
        return;
    }
    VividWaylandGeneration* generation = generation_id ? find_generation(output, generation_id)
                                                       : NULL;
    if (!generation) {
        for (uint32_t i = output->n_generations; i > 0; i--) {
            if (!output->generations[i - 1].retired && !output->generations[i - 1].has_config) {
                generation = &output->generations[i - 1];
                break;
            }
        }
    }
    if (!generation) {
        vivid_wayland_warn("SET_CONFIG unknown generation=%llu output=%u",
                           (unsigned long long)generation_id,
                           output_id);
        return;
    }

    json_object* source = NULL;
    json_object* dest = NULL;
    json_object* clear = NULL;
    json_object_object_get_ex(object, VIVID_JSON_SET_CONFIG_SOURCE, &source);
    json_object_object_get_ex(object, VIVID_JSON_SET_CONFIG_DESTINATION, &dest);
    json_object_object_get_ex(object, VIVID_JSON_SET_CONFIG_CLEAR_COLOR, &clear);

    output->source.x = json_member_double(source, "x", 0.0);
    output->source.y = json_member_double(source, "y", 0.0);
    output->source.w = json_member_double(source, "width", 0.0);
    output->source.h = json_member_double(source, "height", 0.0);
    output->dest.x = json_member_double(dest, "x", 0.0);
    output->dest.y = json_member_double(dest, "y", 0.0);
    output->dest.w = json_member_double(dest, "width", 0.0);
    output->dest.h = json_member_double(dest, "height", 0.0);
    output->clear_color[0] = 0.0f;
    output->clear_color[1] = 0.0f;
    output->clear_color[2] = 0.0f;
    output->clear_color[3] = 1.0f;
    if (clear && json_object_array_length(clear) >= 4) {
        output->clear_color[0] = (float)json_object_get_double(json_object_array_get_idx(clear, 0));
        output->clear_color[1] = (float)json_object_get_double(json_object_array_get_idx(clear, 1));
        output->clear_color[2] = (float)json_object_get_double(json_object_array_get_idx(clear, 2));
        output->clear_color[3] = (float)json_object_get_double(json_object_array_get_idx(clear, 3));
    }
    generation->has_config = true;
}

static void
handle_frame_ready(VividWaylandApp* app, const uint8_t* body, size_t body_len,
                   VividDisplayRecvState* state)
{
    if (body_len != VIVID_DISPLAY_FRAME_READY_BODY_BYTES) {
        vivid_wayland_warn("invalid FRAME_READY body length %zu", body_len);
        close_recv_fds(state);
        return;
    }
    if (!state || state->n_fds != VIVID_DISPLAY_FRAME_READY_FD_COUNT) {
        vivid_wayland_warn("FRAME_READY invalid explicit sync fd count=%zu expected=%zu",
                           state ? state->n_fds : 0,
                           (size_t)VIVID_DISPLAY_FRAME_READY_FD_COUNT);
        close_recv_fds(state);
        return;
    }
    int acquire_fd = vivid_display_recv_state_steal_fd(state, 0);
    int release_fd = vivid_display_recv_state_steal_fd(state, 1);
    uint32_t output_id = 0;
    uint64_t generation_id = 0;
    uint32_t buffer_index = 0;
    uint64_t sequence = 0;
    uint64_t target_time = 0;
    uint32_t flags = 0;
    if (vivid_display_frame_ready_body_read(body, body_len, &output_id, &generation_id,
                                            &buffer_index, &sequence, &target_time, &flags) < 0) {
        vivid_wayland_close_fd(&acquire_fd);
        vivid_wayland_close_fd(&release_fd);
        return;
    }

    VividWaylandOutput* output = vivid_wayland_app_find_output_by_producer_id(app, output_id);
    VividWaylandGeneration* generation = output ? find_generation(output, generation_id) : NULL;
    VividWaylandBoundBuffer* buffer = find_buffer(generation, buffer_index);
    if (!output || !generation || generation->retired || !buffer) {
        if (generation)
            vivid_wayland_signal_release_syncobj(generation->render_node, release_fd,
                                                 "unknown-or-retired-generation");
        vivid_wayland_close_fd(&acquire_fd);
        vivid_wayland_close_fd(&release_fd);
        return;
    }
    if (!generation->has_config) {
        vivid_wayland_warn("FRAME_READY rejected before SET_CONFIG output=%u generation=%llu",
                           output_id,
                           (unsigned long long)generation_id);
        vivid_wayland_signal_release_syncobj(generation->render_node, release_fd, "pending-config");
        vivid_wayland_close_fd(&acquire_fd);
        vivid_wayland_close_fd(&release_fd);
        return;
    }

    if (output->pending_frame) {
        vivid_wayland_signal_release_syncobj(generation->render_node, output->pending_release_fd,
                                             "superseded");
        vivid_wayland_close_fd(&output->pending_acquire_fd);
        vivid_wayland_close_fd(&output->pending_release_fd);
        output->pending_frame = false;
    }
    output->pending_acquire_fd = acquire_fd;
    output->pending_release_fd = release_fd;
    output->pending_generation = generation_id;
    output->pending_buffer = buffer_index;
    output->pending_frame = true;
    vivid_wayland_output_present(output);
}

static void
handle_unbind(VividWaylandApp* app, const uint8_t* body, size_t body_len)
{
    if (body_len != VIVID_DISPLAY_UNBIND_BODY_BYTES)
        return;
    uint32_t output_id = 0;
    uint64_t generation = 0;
    if (vivid_display_unbind_body_read(body, body_len, &output_id, &generation) < 0)
        return;
    VividWaylandOutput* output = vivid_wayland_app_find_output_by_producer_id(app, output_id);
    if (output)
        vivid_wayland_output_retire_generation(output, generation);
    vivid_wayland_producer_send_unbind_done(app, output_id, generation);
}

static void
handle_incoming(VividWaylandApp* app, uint16_t opcode, const uint8_t* body, size_t body_len,
                VividDisplayRecvState* state)
{
    json_object* object = NULL;
    if (opcode == VIVID_DISPLAY_EVT_WELCOME || opcode == VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED ||
        opcode == VIVID_DISPLAY_EVT_BIND_BUFFERS || opcode == VIVID_DISPLAY_EVT_SET_CONFIG ||
        opcode == VIVID_DISPLAY_EVT_ERROR) {
        char* text = malloc(body_len + 1);
        if (text) {
            memcpy(text, body, body_len);
            text[body_len] = '\0';
            object = json_tokener_parse(text);
            free(text);
        }
    }

    switch (opcode) {
    case VIVID_DISPLAY_EVT_WELCOME:
        handle_welcome(app, object);
        break;
    case VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED:
        handle_output_accepted(app, object);
        break;
    case VIVID_DISPLAY_EVT_BIND_BUFFERS:
        handle_bind_buffers(app, object, state);
        break;
    case VIVID_DISPLAY_EVT_SET_CONFIG:
        handle_set_config(app, object);
        break;
    case VIVID_DISPLAY_EVT_FRAME_READY:
        handle_frame_ready(app, body, body_len, state);
        break;
    case VIVID_DISPLAY_EVT_UNBIND:
        handle_unbind(app, body, body_len);
        break;
    case VIVID_DISPLAY_EVT_ERROR: {
        uint32_t code = json_u32(object, VIVID_JSON_EVT_ERROR_CODE, 0);
        bool fatal = json_bool_default(object, VIVID_JSON_EVT_ERROR_FATAL, false);
        const char* message = json_str(object, VIVID_JSON_EVT_ERROR_MESSAGE, "");
        vivid_wayland_warn("producer error %u fatal=%s: %s",
                           code,
                           fatal ? "true" : "false",
                           message);
        if (fatal)
            vivid_wayland_producer_disconnect(app, true);
        break;
    }
    default:
        close_recv_fds(state);
        break;
    }
    if (object)
        json_object_put(object);
}

void
vivid_wayland_producer_disconnect(VividWaylandApp* app, bool schedule_reconnect)
{
    if (!app)
        return;
    if (app->producer_fd >= 0) {
        close(app->producer_fd);
        app->producer_fd = -1;
    }
    app->producer_connecting = false;
    app->caps_sent = false;
    app->handshake_complete = false;
    outbox_clear(app);
    vivid_display_recv_state_clear(&app->recv_state);
    for (uint32_t i = 0; i < app->n_outputs; i++) {
        app->outputs[i].registered = false;
        app->outputs[i].producer_output_id = 0;
        vivid_wayland_output_retire_generations(&app->outputs[i], "transport-close");
    }
    if (schedule_reconnect) {
        if (app->reconnect_delay_ms < VIVID_WAYLAND_RECONNECT_INITIAL_MS)
            app->reconnect_delay_ms = VIVID_WAYLAND_RECONNECT_INITIAL_MS;
        app->next_reconnect_usec =
            vivid_wayland_monotonic_usec() + (uint64_t)app->reconnect_delay_ms * 1000ull;
        if (app->reconnect_delay_ms < VIVID_WAYLAND_RECONNECT_MAX_MS)
            app->reconnect_delay_ms *= 2;
    }
}

void
vivid_wayland_producer_restart_for_topology(VividWaylandApp* app, const char* reason)
{
    if (!app || app->producer_fd < 0)
        return;

    /*
     * display-v1 has no consumer-side UNREGISTER_OUTPUT request. Reconnecting
     * is therefore the only authoritative way to replace the producer's
     * output set after a Wayland hotplug or a compositor-closed layer surface.
     * Reconnect immediately: transport backoff is reserved for connection
     * failures, not for a topology change initiated by this consumer.
     */
    vivid_wayland_log("restarting producer transport for output topology change: %s",
                      reason ? reason : "unspecified");
    vivid_wayland_producer_disconnect(app, false);
    app->reconnect_delay_ms = VIVID_WAYLAND_RECONNECT_INITIAL_MS;
    app->next_reconnect_usec = vivid_wayland_monotonic_usec();
}

bool
vivid_wayland_producer_connect(VividWaylandApp* app)
{
    if (app->producer_fd >= 0)
        return true;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        vivid_wayland_warn("socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    vivid_wayland_strlcpy(addr.sun_path, app->socket_path, sizeof(addr.sun_path));
    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    int err = errno;
    if (rc != 0 && err != EINPROGRESS) {
        close(fd);
        if (err != ENOENT && err != ECONNREFUSED)
            vivid_wayland_warn("connect(%s) failed: %s", app->socket_path, strerror(err));
        app->next_reconnect_usec =
            vivid_wayland_monotonic_usec() + (uint64_t)app->reconnect_delay_ms * 1000ull;
        return false;
    }

    app->producer_fd = fd;
    app->producer_connecting = (rc != 0);
    if (!app->producer_connecting) {
        vivid_display_recv_state_init(&app->recv_state);
        if (!send_hello_and_caps(app)) {
            vivid_wayland_producer_disconnect(app, app->running);
            return false;
        }
        vivid_wayland_producer_register_outputs(app);
        vivid_wayland_log("connected to %s", app->socket_path);
    }
    return true;
}

void
vivid_wayland_producer_on_writable(VividWaylandApp* app)
{
    if (!app || app->producer_fd < 0)
        return;
    if (app->producer_connecting) {
        vivid_wayland_producer_on_readable(app);
        return;
    }
    flush_outbox(app);
}

void
vivid_wayland_producer_on_readable(VividWaylandApp* app)
{
    if (app->producer_fd < 0)
        return;
    if (app->producer_connecting) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(app->producer_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
            error = errno;
        if (error != 0) {
            vivid_wayland_producer_disconnect(app, true);
            return;
        }
        app->producer_connecting = false;
        vivid_display_recv_state_init(&app->recv_state);
        if (!send_hello_and_caps(app)) {
            vivid_wayland_producer_disconnect(app, app->running);
            return;
        }
        vivid_wayland_producer_register_outputs(app);
        vivid_wayland_log("connected to %s", app->socket_path);
        return;
    }

    for (;;) {
        int result = vivid_display_recv_frame_nonblocking(app->producer_fd, &app->recv_state);
        if (result == VIVID_DISPLAY_CODEC_FRAME_NEED_IO)
            return;
        if (result != VIVID_DISPLAY_CODEC_FRAME_DONE) {
            vivid_wayland_warn("producer read failed: %s", strerror(-result));
            vivid_wayland_producer_disconnect(app, true);
            return;
        }
        handle_incoming(app, app->recv_state.opcode, app->recv_state.body,
                        app->recv_state.body_len, &app->recv_state);
        vivid_display_recv_state_clear(&app->recv_state);
    }
}
