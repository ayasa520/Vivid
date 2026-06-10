/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

/*
 * KDE-side naming wrapper for the existing display-v1 C protocol implementation.
 * The protocol source is shared with the GNOME consumer so both display sinks
 * stay byte-for-byte compatible with the same producer.
 */
#include "vivid_display_protocol.h"

typedef VividDisplayRecvState VividDisplayRecvState;

#define VIVID_DISPLAY_PROTOCOL_NAME "display-v1"
#define VIVID_DISPLAY_PROTOCOL_VERSION VIVID_DISPLAY_PROTOCOL_VERSION

#define VIVID_DISPLAY_CODEC_MAX_BODY_BYTES VIVID_DISPLAY_CODEC_MAX_BODY_BYTES
#define VIVID_DISPLAY_CODEC_FRAME_DONE VIVID_DISPLAY_CODEC_FRAME_DONE
#define VIVID_DISPLAY_CODEC_FRAME_NEED_IO VIVID_DISPLAY_CODEC_FRAME_NEED_IO

#define VIVID_DISPLAY_REQ_HELLO VIVID_DISPLAY_REQ_HELLO
#define VIVID_DISPLAY_REQ_CONSUMER_CAPS VIVID_DISPLAY_REQ_CONSUMER_CAPS
#define VIVID_DISPLAY_REQ_REGISTER_OUTPUT VIVID_DISPLAY_REQ_REGISTER_OUTPUT
#define VIVID_DISPLAY_REQ_WINDOW_STATE VIVID_DISPLAY_REQ_WINDOW_STATE
#define VIVID_DISPLAY_REQ_POINTER_MOTION VIVID_DISPLAY_REQ_POINTER_MOTION
#define VIVID_DISPLAY_REQ_POINTER_BUTTON VIVID_DISPLAY_REQ_POINTER_BUTTON
#define VIVID_DISPLAY_REQ_POINTER_AXIS VIVID_DISPLAY_REQ_POINTER_AXIS
#define VIVID_DISPLAY_REQ_MEDIA_STATE VIVID_DISPLAY_REQ_MEDIA_STATE
#define VIVID_DISPLAY_REQ_AUDIO_SAMPLES VIVID_DISPLAY_REQ_AUDIO_SAMPLES
#define VIVID_DISPLAY_REQ_BIND_FAILED VIVID_DISPLAY_REQ_BIND_FAILED
#define VIVID_DISPLAY_REQ_UNBIND_DONE VIVID_DISPLAY_REQ_UNBIND_DONE

#define VIVID_DISPLAY_EVT_WELCOME VIVID_DISPLAY_EVT_WELCOME
#define VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED
#define VIVID_DISPLAY_EVT_BIND_BUFFERS VIVID_DISPLAY_EVT_BIND_BUFFERS
#define VIVID_DISPLAY_EVT_SET_CONFIG VIVID_DISPLAY_EVT_SET_CONFIG
#define VIVID_DISPLAY_EVT_FRAME_READY VIVID_DISPLAY_EVT_FRAME_READY
#define VIVID_DISPLAY_EVT_UNBIND VIVID_DISPLAY_EVT_UNBIND
#define VIVID_DISPLAY_EVT_ERROR VIVID_DISPLAY_EVT_ERROR

#define VIVID_DISPLAY_FRAME_READY_BODY_BYTES VIVID_DISPLAY_FRAME_READY_BODY_BYTES
#define VIVID_DISPLAY_FRAME_READY_FD_COUNT VIVID_DISPLAY_FRAME_READY_FD_COUNT
#define VIVID_DISPLAY_UNBIND_BODY_BYTES VIVID_DISPLAY_UNBIND_BODY_BYTES
#define VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES
#define VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES
#define VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES
#define VIVID_DISPLAY_BUTTON_PRESSED VIVID_DISPLAY_BUTTON_PRESSED
#define VIVID_DISPLAY_BUTTON_RELEASED VIVID_DISPLAY_BUTTON_RELEASED
#define VIVID_DISPLAY_AXIS_WHEEL VIVID_DISPLAY_AXIS_WHEEL

static inline void
vivid_display_recv_state_init(VividDisplayRecvState* state)
{
    vivid_display_recv_state_init(state);
}

static inline void
vivid_display_recv_state_clear(VividDisplayRecvState* state)
{
    vivid_display_recv_state_clear(state);
}

static inline int
vivid_display_recv_state_steal_fd(VividDisplayRecvState* state, size_t index)
{
    return vivid_display_recv_state_steal_fd(state, index);
}

static inline int
vivid_display_recv_frame_nonblocking(int fd, VividDisplayRecvState* state)
{
    return vivid_display_recv_frame_nonblocking(fd, state);
}

static inline ssize_t
vivid_display_send_bytes_nonblocking(int fd, const uint8_t* data, size_t len)
{
    return vivid_display_send_bytes_nonblocking(fd, data, len);
}
