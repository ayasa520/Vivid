/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#define _GNU_SOURCE

#include "vivid_display_protocol.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
test_blocking_frame_with_fd(void)
{
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);

    const char pipe_payload[] = "fd-payload";
    assert(write(pipe_fds[1], pipe_payload, strlen(pipe_payload)) == (ssize_t)strlen(pipe_payload));

    const uint8_t body[] = {0x48, 0x41, 0x4e, 0x41, 0x42, 0x49};
    const int send_fds[] = {pipe_fds[0]};
    assert(vivid_display_send_frame(sockets[0],
                                     VIVID_DISPLAY_EVT_BIND_BUFFERS,
                                     body,
                                     sizeof(body),
                                     send_fds,
                                     1) == 0);

    uint16_t opcode = 0;
    uint8_t received_body[32] = {0};
    size_t body_len = 0;
    int received_fds[VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE];
    size_t n_fds = 0;
    for (size_t i = 0; i < VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE; i++)
        received_fds[i] = -1;

    assert(vivid_display_recv_frame(sockets[1],
                                     &opcode,
                                     received_body,
                                     sizeof(received_body),
                                     &body_len,
                                     received_fds,
                                     VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE,
                                     &n_fds) == 0);
    assert(opcode == VIVID_DISPLAY_EVT_BIND_BUFFERS);
    assert(body_len == sizeof(body));
    assert(memcmp(received_body, body, sizeof(body)) == 0);
    assert(n_fds == 1);
    assert(received_fds[0] >= 0);

    char fd_buffer[sizeof(pipe_payload)] = {0};
    assert(read(received_fds[0], fd_buffer, strlen(pipe_payload)) ==
           (ssize_t)strlen(pipe_payload));
    assert(memcmp(fd_buffer, pipe_payload, strlen(pipe_payload)) == 0);

    vivid_display_close_fds(received_fds, n_fds);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(sockets[0]);
    close(sockets[1]);
}

static void
test_nonblocking_frame(void)
{
    int sockets[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    const int flags = fcntl(sockets[1], F_GETFL, 0);
    assert(flags >= 0);
    assert(fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK) == 0);

    const uint8_t body[] = {0x01, 0x02, 0x03, 0x04};
    assert(vivid_display_send_frame(sockets[0],
                                     VIVID_DISPLAY_REQ_POINTER_MOTION,
                                     body,
                                     sizeof(body),
                                     NULL,
                                     0) == 0);

    VividDisplayRecvState state;
    vivid_display_recv_state_init(&state);

    const int result = vivid_display_recv_frame_nonblocking(sockets[1], &state);
    assert(result == VIVID_DISPLAY_CODEC_FRAME_DONE);
    assert(state.opcode == VIVID_DISPLAY_REQ_POINTER_MOTION);
    assert(state.body_len == sizeof(body));
    assert(memcmp(state.body, body, sizeof(body)) == 0);
    assert(state.n_fds == 0);

    vivid_display_recv_state_clear(&state);
    close(sockets[0]);
    close(sockets[1]);
}

static void
test_frame_ready_carries_exact_sync_fds(void)
{
    int sockets[2] = {-1, -1};
    int acquire_pipe[2] = {-1, -1};
    int release_pipe[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(pipe2(acquire_pipe, O_CLOEXEC) == 0);
    assert(pipe2(release_pipe, O_CLOEXEC) == 0);

    uint8_t body[VIVID_DISPLAY_FRAME_READY_BODY_BYTES] = {0};
    const int send_fds[VIVID_DISPLAY_FRAME_READY_FD_COUNT] = {
        acquire_pipe[0],
        release_pipe[0],
    };
    assert(vivid_display_send_frame(sockets[0],
                                     VIVID_DISPLAY_EVT_FRAME_READY,
                                     body,
                                     sizeof(body),
                                     send_fds,
                                     VIVID_DISPLAY_FRAME_READY_FD_COUNT) == 0);

    VividDisplayRecvState state;
    vivid_display_recv_state_init(&state);
    assert(vivid_display_recv_frame_nonblocking(sockets[1], &state) ==
           VIVID_DISPLAY_CODEC_FRAME_DONE);
    assert(state.opcode == VIVID_DISPLAY_EVT_FRAME_READY);
    assert(state.body_len == VIVID_DISPLAY_FRAME_READY_BODY_BYTES);
    assert(state.n_fds == VIVID_DISPLAY_FRAME_READY_FD_COUNT);

    int acquire_fd = vivid_display_recv_state_steal_fd(&state, 0);
    int release_fd = vivid_display_recv_state_steal_fd(&state, 1);
    assert(acquire_fd >= 0);
    assert(release_fd >= 0);
    close(acquire_fd);
    close(release_fd);

    vivid_display_recv_state_clear(&state);
    close(acquire_pipe[0]);
    close(acquire_pipe[1]);
    close(release_pipe[0]);
    close(release_pipe[1]);
    close(sockets[0]);
    close(sockets[1]);
}

static void
test_control_header(void)
{
    uint8_t encoded[VIVID_DISPLAY_CONTROL_HEADER_BYTES] = {0};
    assert(vivid_display_control_header_encode(encoded,
                                                VIVID_DISPLAY_CONTROL_SET_VOLUME,
                                                0x1234,
                                                0x01020304u) == 0);
    assert(encoded[0] == VIVID_DISPLAY_CONTROL_SET_VOLUME);
    assert(encoded[1] == 0);
    assert(encoded[2] == 0x34);
    assert(encoded[3] == 0x12);
    assert(encoded[4] == 0x04);
    assert(encoded[5] == 0x03);
    assert(encoded[6] == 0x02);
    assert(encoded[7] == 0x01);

    VividDisplayControlHeader decoded = {0};
    assert(vivid_display_control_header_decode(encoded, sizeof(encoded), &decoded) == 0);
    assert(decoded.opcode == VIVID_DISPLAY_CONTROL_SET_VOLUME);
    assert(decoded.flags == 0x1234);
    assert(decoded.json_length == 0x01020304u);
}

static void
test_protocol_constants(void)
{
    assert(VIVID_DISPLAY_FRAME_READY_BODY_BYTES == 36u);
    assert(VIVID_DISPLAY_FRAME_READY_FD_COUNT == 2u);
    assert(VIVID_DISPLAY_REQ_BIND_FAILED == 14);
    assert(VIVID_DISPLAY_REQ_UNBIND_DONE == 15);
}

int
main(void)
{
    test_blocking_frame_with_fd();
    test_nonblocking_frame();
    test_frame_ready_carries_exact_sync_fds();
    test_control_header();
    test_protocol_constants();
    return 0;
}
