#define _GNU_SOURCE

#include "vivid_display_protocol.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define VIVID_DISPLAY_CMSG_SPACE \
    CMSG_SPACE(sizeof(int) * VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE)

static uint16_t
read_u16_le(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void
write_u16_le(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint32_t
read_u32_le(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static void
write_u32_le(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

void
vivid_display_close_fds(int* fds, size_t n_fds)
{
    if (!fds)
        return;

    for (size_t i = 0; i < n_fds; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

static int
harvest_control_fds(struct msghdr* message, int* fd_buf, size_t fd_cap, size_t* n_fds)
{
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(message);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;

        const size_t payload_len = (size_t)cmsg->cmsg_len - (size_t)CMSG_LEN(0);
        const size_t count = payload_len / sizeof(int);
        const unsigned char* payload = (const unsigned char*)CMSG_DATA(cmsg);

        for (size_t i = 0; i < count; i++) {
            int got_fd = -1;
            memcpy(&got_fd, payload + i * sizeof(int), sizeof(int));

            if (*n_fds < fd_cap) {
                fd_buf[*n_fds] = got_fd;
                *n_fds += 1;
                continue;
            }

            /*
             * The receiver's fd array is part of the protocol boundary. If a
             * peer sends more descriptors than the current message type allows,
             * close every descriptor we have already received before returning
             * an error. That keeps fd ownership single and explicit even when a
             * malformed peer tries to overflow the ancillary-data budget.
             */
            close(got_fd);
            for (size_t j = i + 1; j < count; j++) {
                int extra_fd = -1;
                memcpy(&extra_fd, payload + j * sizeof(int), sizeof(int));
                close(extra_fd);
            }
            vivid_display_close_fds(fd_buf, *n_fds);
            *n_fds = 0;
            return -EMSGSIZE;
        }
    }

    return 0;
}

int
vivid_display_send_frame(int fd,
                          uint16_t opcode,
                          const uint8_t* body,
                          size_t body_len,
                          const int* fds,
                          size_t n_fds)
{
    if (fd < 0)
        return -EBADF;
    if (body_len > VIVID_DISPLAY_CODEC_MAX_BODY_BYTES)
        return -EMSGSIZE;
    if (body_len > 0 && !body)
        return -EINVAL;
    if (n_fds > VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE)
        return -EMSGSIZE;
    if (n_fds > 0 && !fds)
        return -EINVAL;

    const size_t total_len = 4u + body_len;
    uint8_t header[4];
    write_u16_le(&header[0], opcode);
    write_u16_le(&header[2], (uint16_t)total_len);

    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = (void*)(uintptr_t)body;
    iov[1].iov_len = body_len;

    union {
        char raw[VIVID_DISPLAY_CMSG_SPACE];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = iov;
    message.msg_iovlen = body_len > 0 ? 2 : 1;

    if (n_fds > 0) {
        message.msg_control = control.raw;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * n_fds);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n_fds);
    }

    for (;;) {
        const ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }

        /*
         * fd-bearing stream frames cannot be resumed after a partial write,
         * because the ancillary data belongs to the first sendmsg call. Treat a
         * partial write as fatal and let the owner close/reconnect the socket.
         */
        if ((size_t)sent != total_len)
            return -EIO;

        return 0;
    }
}

static int
read_exact(int fd, uint8_t* data, size_t len)
{
    while (len > 0) {
        const ssize_t n = read(fd, data, len);
        if (n == 0)
            return -ECONNRESET;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }

        data += (size_t)n;
        len -= (size_t)n;
    }

    return 0;
}

int
vivid_display_recv_frame(int fd,
                          uint16_t* opcode,
                          uint8_t* body_buf,
                          size_t body_cap,
                          size_t* body_len,
                          int* fd_buf,
                          size_t fd_cap,
                          size_t* n_fds)
{
    if (fd < 0)
        return -EBADF;
    if (!opcode || !body_len || !n_fds)
        return -EINVAL;
    if (body_cap > 0 && !body_buf)
        return -EINVAL;
    if (fd_cap > 0 && !fd_buf)
        return -EINVAL;

    *opcode = 0;
    *body_len = 0;
    *n_fds = 0;

    uint8_t header[4];
    size_t header_filled = 0;

    while (header_filled < sizeof(header)) {
        struct iovec iov;
        iov.iov_base = header + header_filled;
        iov.iov_len = sizeof(header) - header_filled;

        union {
            char raw[VIVID_DISPLAY_CMSG_SPACE];
            struct cmsghdr align;
        } control;
        memset(&control, 0, sizeof(control));

        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.raw;
        message.msg_controllen = sizeof(control.raw);

        const ssize_t n = recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            vivid_display_close_fds(fd_buf, *n_fds);
            *n_fds = 0;
            return -errno;
        }

        const int fd_result = harvest_control_fds(&message, fd_buf, fd_cap, n_fds);
        if (fd_result < 0)
            return fd_result;

        if (n == 0) {
            vivid_display_close_fds(fd_buf, *n_fds);
            *n_fds = 0;
            return -ECONNRESET;
        }

        header_filled += (size_t)n;
    }

    const uint16_t total_len = read_u16_le(&header[2]);
    if (total_len < 4) {
        vivid_display_close_fds(fd_buf, *n_fds);
        *n_fds = 0;
        return -EBADMSG;
    }

    const size_t incoming_body_len = (size_t)total_len - 4u;
    if (incoming_body_len > body_cap) {
        vivid_display_close_fds(fd_buf, *n_fds);
        *n_fds = 0;
        return -EMSGSIZE;
    }

    const int body_result = read_exact(fd, body_buf, incoming_body_len);
    if (body_result < 0) {
        vivid_display_close_fds(fd_buf, *n_fds);
        *n_fds = 0;
        return body_result;
    }

    *opcode = read_u16_le(&header[0]);
    *body_len = incoming_body_len;
    return 0;
}

void
vivid_display_recv_state_init(VividDisplayRecvState* state)
{
    if (!state)
        return;

    memset(state, 0, sizeof(*state));
    for (size_t i = 0; i < VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE; i++)
        state->fds[i] = -1;
}

void
vivid_display_recv_state_clear(VividDisplayRecvState* state)
{
    if (!state)
        return;

    vivid_display_close_fds(state->fds, state->n_fds);
    vivid_display_recv_state_init(state);
}

int
vivid_display_recv_state_steal_fd(VividDisplayRecvState* state, size_t index)
{
    if (!state || index >= state->n_fds)
        return -1;

    const int fd = state->fds[index];
    state->fds[index] = -1;
    return fd;
}

static int
recv_header_nonblocking(int fd, VividDisplayRecvState* state)
{
    while (state->header_filled < sizeof(state->header)) {
        struct iovec iov;
        iov.iov_base = state->header + state->header_filled;
        iov.iov_len = sizeof(state->header) - state->header_filled;

        union {
            char raw[VIVID_DISPLAY_CMSG_SPACE];
            struct cmsghdr align;
        } control;
        memset(&control, 0, sizeof(control));

        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.raw;
        message.msg_controllen = sizeof(control.raw);

        const ssize_t n = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return VIVID_DISPLAY_CODEC_FRAME_NEED_IO;
            return -errno;
        }

        const int fd_result =
            harvest_control_fds(&message,
                                state->fds,
                                VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE,
                                &state->n_fds);
        if (fd_result < 0) {
            vivid_display_recv_state_clear(state);
            return fd_result;
        }

        if (n == 0) {
            vivid_display_recv_state_clear(state);
            return -ECONNRESET;
        }

        state->header_filled += (size_t)n;
    }

    state->opcode = read_u16_le(&state->header[0]);
    const uint16_t total_len = read_u16_le(&state->header[2]);
    if (total_len < 4) {
        vivid_display_recv_state_clear(state);
        return -EBADMSG;
    }

    state->body_len = (size_t)total_len - 4u;
    if (state->body_len > VIVID_DISPLAY_CODEC_MAX_BODY_BYTES) {
        vivid_display_recv_state_clear(state);
        return -EMSGSIZE;
    }

    return 0;
}

int
vivid_display_recv_frame_nonblocking(int fd, VividDisplayRecvState* state)
{
    if (fd < 0)
        return -EBADF;
    if (!state)
        return -EINVAL;

    const int header_result = recv_header_nonblocking(fd, state);
    if (header_result != 0)
        return header_result;

    while (state->body_filled < state->body_len) {
        const ssize_t n =
            recv(fd,
                 state->body + state->body_filled,
                 state->body_len - state->body_filled,
                 MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return VIVID_DISPLAY_CODEC_FRAME_NEED_IO;
            return -errno;
        }
        if (n == 0) {
            vivid_display_recv_state_clear(state);
            return -ECONNRESET;
        }

        state->body_filled += (size_t)n;
    }

    return VIVID_DISPLAY_CODEC_FRAME_DONE;
}

ssize_t
vivid_display_send_bytes_nonblocking(int fd, const uint8_t* data, size_t len)
{
    if (fd < 0)
        return -EBADF;
    if (len > 0 && !data)
        return -EINVAL;
    if (len == 0)
        return 0;

    for (;;) {
        const ssize_t sent = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent >= 0)
            return sent;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -errno;
    }
}

int
vivid_display_control_header_encode(uint8_t out[VIVID_DISPLAY_CONTROL_HEADER_BYTES],
                                     uint16_t opcode,
                                     uint16_t flags,
                                     uint32_t json_length)
{
    if (!out)
        return -EINVAL;

    write_u16_le(&out[0], opcode);
    write_u16_le(&out[2], flags);
    write_u32_le(&out[4], json_length);
    return 0;
}

int
vivid_display_control_header_decode(const uint8_t* data,
                                     size_t len,
                                     VividDisplayControlHeader* out)
{
    if (!data || !out)
        return -EINVAL;
    if (len < VIVID_DISPLAY_CONTROL_HEADER_BYTES)
        return -EMSGSIZE;

    out->opcode = read_u16_le(&data[0]);
    out->flags = read_u16_le(&data[2]);
    out->json_length = read_u32_le(&data[4]);
    return 0;
}
