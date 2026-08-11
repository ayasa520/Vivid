#ifndef VIVID_RENDERER_TRANSPORT_H
#define VIVID_RENDERER_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "vivid_renderer_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    VIVID_RENDERER_TRANSPORT_OK = 0,
    VIVID_RENDERER_TRANSPORT_WOULD_BLOCK = 1,
    VIVID_RENDERER_TRANSPORT_EOF = 2,
};

typedef struct
{
    VividRendererMessageHeader header;
    uint8_t* payload;
    size_t payload_length;
    int fds[VIVID_RENDERER_MAX_FDS_PER_MESSAGE];
    size_t n_fds;
} VividRendererPacket;

/* Create the only supported renderer transport: an AF_UNIX SEQPACKET pair. */
int vivid_renderer_transport_socket_pair(int sockets[2]);

/* Set or clear O_NONBLOCK without changing any other descriptor flags. */
int vivid_renderer_transport_set_nonblocking(int fd, int nonblocking);

void vivid_renderer_packet_init(VividRendererPacket* packet);

/*
 * Close every FD still owned by packet and release its payload allocation.
 * A successfully received packet owns all SCM_RIGHTS descriptors until the
 * caller steals individual entries or clears the packet.
 */
void vivid_renderer_packet_clear(VividRendererPacket* packet);
int vivid_renderer_packet_steal_fd(VividRendererPacket* packet, size_t index);

/*
 * Send one complete renderer packet. The caller retains ownership of every FD
 * in fds; SCM_RIGHTS creates new references owned by the receiver.
 */
int vivid_renderer_transport_send_borrowed(
    int fd,
    uint64_t renderer_instance_id,
    VividRendererMessageDirection direction,
    uint16_t opcode,
    uint64_t request_id,
    const void* payload,
    size_t payload_length,
    const int* fds,
    size_t n_fds);

/*
 * Send one complete renderer packet and consume the caller's FD references.
 * Every non-negative entry is closed and changed to -1 before this function
 * returns, including validation, EAGAIN, and sendmsg failure paths.
 */
int vivid_renderer_transport_send_moved(
    int fd,
    uint64_t renderer_instance_id,
    VividRendererMessageDirection direction,
    uint16_t opcode,
    uint64_t request_id,
    const void* payload,
    size_t payload_length,
    int* fds,
    size_t n_fds);

/*
 * Receive and validate one complete packet without blocking. Protocol errors
 * close all descriptors from the rejected transaction before returning a
 * negative errno-style result. EOF is terminal for the renderer instance. The
 * packet must have been initialized with vivid_renderer_packet_init().
 */
int vivid_renderer_transport_receive(
    int fd,
    uint64_t expected_instance_id,
    VividRendererMessageDirection expected_direction,
    VividRendererPacket* packet);

void vivid_renderer_transport_close_fds(int* fds, size_t n_fds);

#ifdef __cplusplus
}
#endif

#endif
