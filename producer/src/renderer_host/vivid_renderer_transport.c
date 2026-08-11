#include "vivid_renderer_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static void
packet_reset_without_closing(VividRendererPacket* packet)
{
    if (!packet)
        return;
    memset(&packet->header, 0, sizeof(packet->header));
    packet->payload = NULL;
    packet->payload_length = 0;
    packet->n_fds = 0;
    for (size_t i = 0; i < VIVID_RENDERER_MAX_FDS_PER_MESSAGE; i++)
        packet->fds[i] = -1;
}

void
vivid_renderer_transport_close_fds(int* fds, size_t n_fds)
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

int
vivid_renderer_transport_socket_pair(int sockets[2])
{
    if (!sockets)
        return -EINVAL;
    sockets[0] = -1;
    sockets[1] = -1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0)
        return -errno;
    return VIVID_RENDERER_TRANSPORT_OK;
}

int
vivid_renderer_transport_set_nonblocking(int fd, int nonblocking)
{
    if (fd < 0)
        return -EINVAL;
    const int old_flags = fcntl(fd, F_GETFL);
    if (old_flags < 0)
        return -errno;
    const int new_flags = nonblocking ? old_flags | O_NONBLOCK : old_flags & ~O_NONBLOCK;
    if (new_flags != old_flags && fcntl(fd, F_SETFL, new_flags) < 0)
        return -errno;
    return VIVID_RENDERER_TRANSPORT_OK;
}

void
vivid_renderer_packet_init(VividRendererPacket* packet)
{
    packet_reset_without_closing(packet);
}

void
vivid_renderer_packet_clear(VividRendererPacket* packet)
{
    if (!packet)
        return;
    vivid_renderer_transport_close_fds(packet->fds, packet->n_fds);
    free(packet->payload);
    packet_reset_without_closing(packet);
}

int
vivid_renderer_packet_steal_fd(VividRendererPacket* packet, size_t index)
{
    if (!packet || index >= packet->n_fds)
        return -1;
    const int fd = packet->fds[index];
    packet->fds[index] = -1;
    return fd;
}

static int
validate_outgoing_packet(uint64_t renderer_instance_id,
                         VividRendererMessageDirection direction,
                         uint16_t opcode,
                         uint64_t request_id,
                         const uint8_t* payload,
                         size_t payload_length,
                         const int* fds,
                         size_t n_fds,
                         VividRendererMessageHeader* header)
{
    if (!header || renderer_instance_id == 0u ||
        (payload_length != 0u && !payload) ||
        (n_fds != 0u && !fds)) {
        return -EINVAL;
    }
    for (size_t i = 0; i < n_fds; i++) {
        if (fds[i] < 0)
            return -EBADF;
    }

    *header = (VividRendererMessageHeader) {
        .magic = VIVID_RENDERER_PROTOCOL_MAGIC,
        .version = VIVID_RENDERER_PROTOCOL_VERSION,
        .opcode = opcode,
        .payload_length = (uint32_t)payload_length,
        .request_id = request_id,
        .renderer_instance_id = renderer_instance_id,
    };
    if (!vivid_renderer_header_contract_valid(header,
                                              renderer_instance_id,
                                              direction) ||
        !vivid_renderer_fd_count_valid(opcode,
                                       payload,
                                       payload_length,
                                       n_fds)) {
        return -EPROTO;
    }
    return VIVID_RENDERER_TRANSPORT_OK;
}

int
vivid_renderer_transport_send_borrowed(
    int fd,
    uint64_t renderer_instance_id,
    VividRendererMessageDirection direction,
    uint16_t opcode,
    uint64_t request_id,
    const void* payload,
    size_t payload_length,
    const int* fds,
    size_t n_fds)
{
    if (fd < 0)
        return -EINVAL;

    VividRendererMessageHeader header;
    const uint8_t* payload_bytes = (const uint8_t*)payload;
    int result = validate_outgoing_packet(renderer_instance_id,
                                          direction,
                                          opcode,
                                          request_id,
                                          payload_bytes,
                                          payload_length,
                                          fds,
                                          n_fds,
                                          &header);
    if (result != VIVID_RENDERER_TRANSPORT_OK)
        return result;

    uint8_t header_bytes[VIVID_RENDERER_HEADER_BYTES];
    if (!vivid_renderer_header_encode(header_bytes, &header))
        return -EINVAL;

    struct iovec iov[2] = {
        {.iov_base = header_bytes, .iov_len = sizeof(header_bytes)},
        {.iov_base = (void*)payload_bytes, .iov_len = payload_length},
    };
    union
    {
        struct cmsghdr align;
        uint8_t bytes[CMSG_SPACE(sizeof(int) * VIVID_RENDERER_MAX_FDS_PER_MESSAGE)];
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = iov;
    message.msg_iovlen = payload_length == 0u ? 1u : 2u;
    if (n_fds != 0u) {
        message.msg_control = control.bytes;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * n_fds);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n_fds);
    }

    const size_t expected_bytes = VIVID_RENDERER_HEADER_BYTES + payload_length;
    const ssize_t sent = sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return VIVID_RENDERER_TRANSPORT_WOULD_BLOCK;
        return -errno;
    }
    /* SOCK_SEQPACKET must accept the packet atomically or report an error. */
    if ((size_t)sent != expected_bytes)
        return -EIO;
    return VIVID_RENDERER_TRANSPORT_OK;
}

int
vivid_renderer_transport_send_moved(
    int fd,
    uint64_t renderer_instance_id,
    VividRendererMessageDirection direction,
    uint16_t opcode,
    uint64_t request_id,
    const void* payload,
    size_t payload_length,
    int* fds,
    size_t n_fds)
{
    /*
     * Ownership transfers into this function before validation. This invariant
     * prevents a caller from retaining an acquire fence on an EAGAIN/error path
     * after it has already handed the transaction to a serialized IPC outbox.
     */
    const int result = vivid_renderer_transport_send_borrowed(fd,
                                                               renderer_instance_id,
                                                               direction,
                                                               opcode,
                                                               request_id,
                                                               payload,
                                                               payload_length,
                                                               fds,
                                                               n_fds);
    vivid_renderer_transport_close_fds(fds, n_fds);
    return result;
}

static int
collect_received_fds(struct msghdr* message,
                     int fds[VIVID_RENDERER_MAX_FDS_PER_MESSAGE],
                     size_t* n_fds)
{
    int result = VIVID_RENDERER_TRANSPORT_OK;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(message);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) {
            result = -EPROTO;
            continue;
        }
        const size_t data_bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (data_bytes % sizeof(int) != 0u) {
            result = -EPROTO;
            continue;
        }
        const size_t count = data_bytes / sizeof(int);
        const int* cmsg_fds = (const int*)CMSG_DATA(cmsg);
        for (size_t i = 0; i < count; i++) {
            if (*n_fds < VIVID_RENDERER_MAX_FDS_PER_MESSAGE) {
                fds[(*n_fds)++] = cmsg_fds[i];
            } else {
                /*
                 * SCM_RIGHTS descriptors already belong to this process once
                 * recvmsg succeeds. Even a peer that exceeds the generated
                 * cardinality must not be able to leak the overflow entries.
                 */
                close(cmsg_fds[i]);
                result = -EMSGSIZE;
            }
        }
    }
    return result;
}

int
vivid_renderer_transport_receive(
    int fd,
    uint64_t expected_instance_id,
    VividRendererMessageDirection expected_direction,
    VividRendererPacket* packet)
{
    if (fd < 0 || expected_instance_id == 0u || !packet)
        return -EINVAL;

    vivid_renderer_packet_clear(packet);
    uint8_t* wire = malloc(VIVID_RENDERER_MAX_PACKET_BYTES);
    if (!wire)
        return -ENOMEM;

    union
    {
        struct cmsghdr align;
        uint8_t bytes[CMSG_SPACE(sizeof(int) * VIVID_RENDERER_MAX_FDS_PER_MESSAGE)];
    } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = {
        .iov_base = wire,
        .iov_len = VIVID_RENDERER_MAX_PACKET_BYTES,
    };
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);

    const ssize_t received = recvmsg(fd,
                                     &message,
                                     MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (received < 0) {
        const int saved_errno = errno;
        free(wire);
        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)
            return VIVID_RENDERER_TRANSPORT_WOULD_BLOCK;
        return -saved_errno;
    }
    if (received == 0) {
        free(wire);
        return VIVID_RENDERER_TRANSPORT_EOF;
    }

    int received_fds[VIVID_RENDERER_MAX_FDS_PER_MESSAGE];
    for (size_t i = 0; i < VIVID_RENDERER_MAX_FDS_PER_MESSAGE; i++)
        received_fds[i] = -1;
    size_t n_fds = 0;
    int result = collect_received_fds(&message, received_fds, &n_fds);

    /*
     * MSG_TRUNC means the record exceeded the generated packet bound.
     * MSG_CTRUNC means the kernel could not expose the complete ownership set.
     * In both cases the transaction is indivisible and every visible FD must be
     * closed before the worker is treated as protocol-failed.
     */
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
        result = -EMSGSIZE;
    if ((size_t)received < VIVID_RENDERER_HEADER_BYTES && result == 0)
        result = -EPROTO;

    VividRendererMessageHeader header;
    memset(&header, 0, sizeof(header));
    if (result == 0 &&
        !vivid_renderer_header_decode(wire,
                                      VIVID_RENDERER_HEADER_BYTES,
                                      &header)) {
        result = -EPROTO;
    }
    if (result == 0 &&
        ((size_t)received != VIVID_RENDERER_HEADER_BYTES + header.payload_length ||
         !vivid_renderer_header_contract_valid(&header,
                                               expected_instance_id,
                                               expected_direction))) {
        result = -EPROTO;
    }

    const uint8_t* payload = wire + VIVID_RENDERER_HEADER_BYTES;
    if (result == 0 &&
        !vivid_renderer_fd_count_valid(header.opcode,
                                       payload,
                                       header.payload_length,
                                       n_fds)) {
        result = -EPROTO;
    }
    if (result != 0) {
        vivid_renderer_transport_close_fds(received_fds, n_fds);
        free(wire);
        return result;
    }

    if (header.payload_length != 0u)
        memmove(wire, payload, header.payload_length);
    packet->header = header;
    packet->payload = wire;
    packet->payload_length = header.payload_length;
    memcpy(packet->fds, received_fds, sizeof(int) * n_fds);
    packet->n_fds = n_fds;
    return VIVID_RENDERER_TRANSPORT_OK;
}
