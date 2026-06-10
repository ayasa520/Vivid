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

#ifdef __cplusplus
extern "C" {
#endif

#define VIVID_DISPLAY_PROTOCOL_NAME "vivid-display-v1"
#define VIVID_DISPLAY_PROTOCOL_VERSION 1u

#define VIVID_DISPLAY_CODEC_MAX_BODY_BYTES ((size_t)65531u)
#define VIVID_DISPLAY_CODEC_MAX_FDS_PER_MESSAGE ((size_t)64u)
#define VIVID_DISPLAY_DMABUF_MAX_PLANES 4u
#define VIVID_DISPLAY_CONTROL_HEADER_BYTES 8u
#define VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES 28u
#define VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES 36u
#define VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES 48u
#define VIVID_DISPLAY_FRAME_READY_BODY_BYTES 36u
#define VIVID_DISPLAY_FRAME_READY_FD_COUNT 2u
#define VIVID_DISPLAY_UNBIND_BODY_BYTES 12u

#define VIVID_DISPLAY_CODEC_FRAME_DONE 1
#define VIVID_DISPLAY_CODEC_FRAME_NEED_IO 2

typedef enum
{
    VIVID_DISPLAY_OK = 0,
    VIVID_DISPLAY_ERR_INVAL = -1,
    VIVID_DISPLAY_ERR_IO = -2,
    VIVID_DISPLAY_ERR_PROTOCOL = -3,
    VIVID_DISPLAY_ERR_NO_SPACE = -4,
} VividDisplayResult;

typedef enum
{
    VIVID_DISPLAY_CLIENT_CONSUMER = 1,
    VIVID_DISPLAY_CLIENT_CONTROLLER = 2,
    VIVID_DISPLAY_CLIENT_CONSUMER_AND_CONTROLLER = 3,
    VIVID_DISPLAY_CLIENT_PRODUCER = 4,
    VIVID_DISPLAY_CLIENT_DISPLAY = VIVID_DISPLAY_CLIENT_CONSUMER,
    VIVID_DISPLAY_CLIENT_DISPLAY_AND_CONTROLLER = VIVID_DISPLAY_CLIENT_CONSUMER_AND_CONTROLLER,
} VividDisplayClientRole;

typedef enum
{
    VIVID_DISPLAY_REQ_HELLO = 1,
    VIVID_DISPLAY_REQ_REGISTER_OUTPUT = 2,
    VIVID_DISPLAY_REQ_UPDATE_OUTPUT = 3,
    VIVID_DISPLAY_REQ_CONSUMER_CAPS = 4,
    VIVID_DISPLAY_REQ_BYE = 5,
    VIVID_DISPLAY_REQ_POINTER_MOTION = 7,
    VIVID_DISPLAY_REQ_POINTER_BUTTON = 8,
    VIVID_DISPLAY_REQ_POINTER_AXIS = 9,
    VIVID_DISPLAY_REQ_WINDOW_STATE = 10,
    VIVID_DISPLAY_REQ_CONTROL = 11,
    VIVID_DISPLAY_REQ_MEDIA_STATE = 12,
    VIVID_DISPLAY_REQ_AUDIO_SAMPLES = 13,
    VIVID_DISPLAY_REQ_BIND_FAILED = 14,
    VIVID_DISPLAY_REQ_UNBIND_DONE = 15,
} VividDisplayRequestOpcode;

typedef enum
{
    VIVID_DISPLAY_EVT_WELCOME = 1,
    VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED = 2,
    VIVID_DISPLAY_EVT_BIND_BUFFERS = 3,
    VIVID_DISPLAY_EVT_SET_CONFIG = 4,
    VIVID_DISPLAY_EVT_FRAME_READY = 5,
    VIVID_DISPLAY_EVT_UNBIND = 6,
    VIVID_DISPLAY_EVT_CONTROL = 7,
    VIVID_DISPLAY_EVT_AUDIO_FRAME = 8,
    VIVID_DISPLAY_EVT_ERROR = 9,
} VividDisplayEventOpcode;

typedef enum
{
    VIVID_DISPLAY_CONTROL_GET_STATE = 1,
    VIVID_DISPLAY_CONTROL_STATE_SNAPSHOT = 2,
    VIVID_DISPLAY_CONTROL_SET_PROJECT = 3,
    VIVID_DISPLAY_CONTROL_SET_PLAYING = 4,
    VIVID_DISPLAY_CONTROL_SET_MUTED = 5,
    VIVID_DISPLAY_CONTROL_SET_VOLUME = 6,
    VIVID_DISPLAY_CONTROL_SET_CONTENT_FIT = 7,
    VIVID_DISPLAY_CONTROL_SET_SCENE_FPS = 8,
    VIVID_DISPLAY_CONTROL_SET_USER_PROPERTIES = 9,
    VIVID_DISPLAY_CONTROL_OPEN_SETTINGS = 10,
    VIVID_DISPLAY_CONTROL_ACK = 11,
    VIVID_DISPLAY_CONTROL_ERROR = 12,
    VIVID_DISPLAY_CONTROL_SET_STATE = 13,
} VividDisplayControlOpcode;

typedef enum
{
    VIVID_DISPLAY_BUTTON_RELEASED = 0,
    VIVID_DISPLAY_BUTTON_PRESSED = 1,
} VividDisplayButtonState;

typedef enum
{
    VIVID_DISPLAY_AXIS_WHEEL = 0,
    VIVID_DISPLAY_AXIS_FINGER = 1,
    VIVID_DISPLAY_AXIS_CONTINUOUS = 2,
} VividDisplayAxisSource;

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
