/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_DISPLAY_PROTOCOL_H
#define VIVID_DISPLAY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "vivid_display_protocol_ids.h"
#include "vivid_display_protocol_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    float x;
    float y;
    float w;
    float h;
} VividDisplayRect;

typedef struct
{
    uint16_t opcode;
    uint16_t flags;
    uint32_t json_length;
} VividDisplayControlHeader;

typedef struct
{
    int fd;
    uint32_t stride;
    uint32_t offset;
} VividDisplayDmaBufPlane;

typedef struct
{
    uint64_t buffer_generation;
    uint32_t buffer_index;
    int32_t source_frame_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint64_t modifier;
    uint64_t size;
    uint32_t n_planes;
    uint32_t premultiplied;
    VividDisplayDmaBufPlane planes[VIVID_DISPLAY_DMABUF_MAX_PLANES];
} VividDisplayDmaBufFrame;

typedef struct
{
    uint8_t header[4];
    size_t header_filled;
    uint16_t opcode;
    size_t body_len;
    uint8_t body[VIVID_DISPLAY_CODEC_MAX_BODY_BYTES];
    size_t body_filled;
    int fds[VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE];
    size_t n_fds;
} VividDisplayRecvState;

/*
 * Send one framed protocol message.
 *
 * The caller keeps ownership of the fds passed in. The kernel duplicates the fd
 * references into the receiving process through SCM_RIGHTS. A return value of 0
 * means the whole frame, including ancillary data, was accepted by sendmsg(2).
 */
int vivid_display_send_frame(int fd,
                              uint16_t opcode,
                              const uint8_t* body,
                              size_t body_len,
                              const int* fds,
                              size_t n_fds);

/*
 * Blocking receive for tools and tests. The caller owns every fd written into
 * fd_buf and must close them, usually with vivid_display_close_fds().
 */
int vivid_display_recv_frame(int fd,
                              uint16_t* opcode,
                              uint8_t* body_buf,
                              size_t body_cap,
                              size_t* body_len,
                              int* fd_buf,
                              size_t fd_cap,
                              size_t* n_fds);

/*
 * Non-blocking receive state for GLib, Qt, Wayland, or poll based event loops.
 * Return values:
 *
 *   VIVID_DISPLAY_CODEC_FRAME_DONE    state contains one complete frame.
 *   VIVID_DISPLAY_CODEC_FRAME_NEED_IO retry after the socket is readable.
 *   negative errno-style value         fatal socket/protocol error.
 */
void vivid_display_recv_state_init(VividDisplayRecvState* state);
void vivid_display_recv_state_clear(VividDisplayRecvState* state);
int vivid_display_recv_state_steal_fd(VividDisplayRecvState* state, size_t index);
int vivid_display_recv_frame_nonblocking(int fd, VividDisplayRecvState* state);

/*
 * Non-blocking raw byte sender for outbox implementations. This is only for
 * frames without SCM_RIGHTS. fd-bearing frames must be sent in a single
 * sendmsg(2) call with vivid_display_send_frame().
 */
ssize_t vivid_display_send_bytes_nonblocking(int fd, const uint8_t* data, size_t len);

int vivid_display_control_header_encode(uint8_t out[VIVID_DISPLAY_CONTROL_HEADER_BYTES],
                                         uint16_t opcode,
                                         uint16_t flags,
                                         uint32_t json_length);
int vivid_display_control_header_decode(const uint8_t* data,
                                         size_t len,
                                         VividDisplayControlHeader* out);

void vivid_display_close_fds(int* fds, size_t n_fds);

#ifdef __cplusplus
}
#endif

#endif
