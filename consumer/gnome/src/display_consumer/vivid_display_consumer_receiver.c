#define _GNU_SOURCE

#include "vivid_display_consumer_receiver.h"

#include "../protocol/vivid_display_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <string.h>

struct _VividDisplayConsumerReceiver {
    GObject parent_instance;

    GSocketConnection* connection;
    gint socket_fd;
    guint source_id;
    VividDisplayRecvState* recv_state;
};

G_DEFINE_TYPE(VividDisplayConsumerReceiver,
              vivid_display_consumer_receiver,
              G_TYPE_OBJECT)

enum {
    SIGNAL_FRAME,
    SIGNAL_PROTOCOL_ERROR,
    SIGNAL_CLOSED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void
receiver_clear_recv_state(VividDisplayConsumerReceiver* self)
{
    if (self->recv_state)
        vivid_display_recv_state_clear(self->recv_state);
}

static void
emit_protocol_error(VividDisplayConsumerReceiver* self,
                    gint                          code,
                    const char*                   message)
{
    g_signal_emit(self,
                  signals[SIGNAL_PROTOCOL_ERROR],
                  0,
                  code,
                  message ? message : "unknown display receiver error");
}

static gboolean
set_fd_nonblocking(gint fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return FALSE;

    if ((flags & O_NONBLOCK) != 0)
        return TRUE;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static GUnixFDList*
build_fd_list_from_recv_state(VividDisplayRecvState* state,
                              GError**                error)
{
    GUnixFDList* fd_list = g_unix_fd_list_new();

    for (size_t i = 0; i < state->n_fds; i++) {
        if (state->fds[i] < 0)
            continue;

        /*
         * GUnixFDList duplicates each fd on append. The recv state keeps owning
         * the original SCM_RIGHTS descriptors until vivid_display_recv_state_clear()
         * closes them, while the emitted fd list owns a second set that remains
         * valid for consumers after this function returns.
         */
        if (g_unix_fd_list_append(fd_list, state->fds[i], error) < 0) {
            g_object_unref(fd_list);
            return NULL;
        }
    }

    return fd_list;
}

static void
emit_complete_frame(VividDisplayConsumerReceiver* self)
{
    GError* error = NULL;
    GBytes* body = g_bytes_new(self->recv_state->body, self->recv_state->body_len);
    GUnixFDList* fd_list = build_fd_list_from_recv_state(self->recv_state, &error);

    if (!fd_list) {
        emit_protocol_error(self,
                            error ? error->code : -EMFILE,
                            error ? error->message : "failed to duplicate received fds");
        g_clear_error(&error);
        g_bytes_unref(body);
        return;
    }

    /*
     * The frame signal is the display module boundary between socket transport and the
     * higher-level GNOME consumer. It intentionally exposes the raw opcode,
     * body bytes, and fd list without knowing about Clutter/COGL. A later
     * display actor can consume these same objects to import DMA-BUFs.
     */
    g_signal_emit(self,
                  signals[SIGNAL_FRAME],
                  0,
                  (guint)self->recv_state->opcode,
                  body,
                  fd_list);

    g_object_unref(fd_list);
    g_bytes_unref(body);
}

static gboolean
receiver_fd_ready(gint         fd,
                  GIOCondition condition,
                  gpointer     user_data)
{
    VividDisplayConsumerReceiver* self = VIVID_DISPLAY_CONSUMER_RECEIVER(user_data);

    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        self->source_id = 0;
        receiver_clear_recv_state(self);
        g_signal_emit(self, signals[SIGNAL_CLOSED], 0);
        return G_SOURCE_REMOVE;
    }

    for (;;) {
        const int result = vivid_display_recv_frame_nonblocking(fd, self->recv_state);

        if (result == VIVID_DISPLAY_CODEC_FRAME_NEED_IO)
            return G_SOURCE_CONTINUE;

        if (result == VIVID_DISPLAY_CODEC_FRAME_DONE) {
            emit_complete_frame(self);
            receiver_clear_recv_state(self);
            continue;
        }

        self->source_id = 0;
        receiver_clear_recv_state(self);
        emit_protocol_error(self,
                            result,
                            g_strerror(result < 0 ? -result : result));
        g_signal_emit(self, signals[SIGNAL_CLOSED], 0);
        return G_SOURCE_REMOVE;
    }
}

static void
vivid_display_consumer_receiver_dispose(GObject* object)
{
    VividDisplayConsumerReceiver* self = VIVID_DISPLAY_CONSUMER_RECEIVER(object);

    vivid_display_consumer_receiver_stop(self);
    g_clear_object(&self->connection);

    G_OBJECT_CLASS(vivid_display_consumer_receiver_parent_class)->dispose(object);
}

static void
vivid_display_consumer_receiver_finalize(GObject* object)
{
    VividDisplayConsumerReceiver* self = VIVID_DISPLAY_CONSUMER_RECEIVER(object);

    /*
     * VividDisplayRecvState contains the protocol maximum body buffer and fd
     * array. Keeping it on the heap avoids a >64 KiB GObject instance, which
     * GObject Introspection cannot scan because instance_size is guint16-sized.
     */
    receiver_clear_recv_state(self);
    g_clear_pointer(&self->recv_state, g_free);

    G_OBJECT_CLASS(vivid_display_consumer_receiver_parent_class)->finalize(object);
}

static void
vivid_display_consumer_receiver_class_init(VividDisplayConsumerReceiverClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = vivid_display_consumer_receiver_dispose;
    object_class->finalize = vivid_display_consumer_receiver_finalize;

    /*
     * Emitted for every complete wire frame. fd_list may be empty, but it is
     * always non-NULL so GJS and later display module code can use one signal
     * path for byte-only and fd-bearing frames.
     */
    signals[SIGNAL_FRAME] =
        g_signal_new("frame",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE,
                     3,
                     G_TYPE_UINT,
                     G_TYPE_BYTES,
                     G_TYPE_UNIX_FD_LIST);

    signals[SIGNAL_PROTOCOL_ERROR] =
        g_signal_new("protocol-error",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE,
                     2,
                     G_TYPE_INT,
                     G_TYPE_STRING);

    signals[SIGNAL_CLOSED] =
        g_signal_new("closed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL,
                     NULL,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE,
                     0);
}

static void
vivid_display_consumer_receiver_init(VividDisplayConsumerReceiver* self)
{
    self->socket_fd = -1;
    self->recv_state = g_new0(VividDisplayRecvState, 1);
    vivid_display_recv_state_init(self->recv_state);
}

VividDisplayConsumerReceiver*
vivid_display_consumer_receiver_new(GSocketConnection* connection)
{
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), NULL);

    VividDisplayConsumerReceiver* self =
        g_object_new(VIVID_DISPLAY_CONSUMER_TYPE_RECEIVER, NULL);

    self->connection = g_object_ref(connection);
    self->socket_fd = g_socket_get_fd(g_socket_connection_get_socket(connection));

    return self;
}

gboolean
vivid_display_consumer_receiver_start(VividDisplayConsumerReceiver* self)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_RECEIVER(self), FALSE);

    if (self->source_id != 0)
        return TRUE;
    if (self->socket_fd < 0) {
        emit_protocol_error(self, -EBADF, "display receiver socket fd is invalid");
        return FALSE;
    }
    if (!set_fd_nonblocking(self->socket_fd)) {
        emit_protocol_error(self, -errno, g_strerror(errno));
        return FALSE;
    }

    /*
     * The source owns a reference while active because GJS may drop its wrapper
     * during extension shutdown or reconnect. dispose() removes the source
     * first, so this reference never keeps an already-stopped receiver alive.
     */
    self->source_id = g_unix_fd_add_full(G_PRIORITY_DEFAULT,
                                         self->socket_fd,
                                         G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                         receiver_fd_ready,
                                         g_object_ref(self),
                                         g_object_unref);
    return self->source_id != 0;
}

void
vivid_display_consumer_receiver_stop(VividDisplayConsumerReceiver* self)
{
    g_return_if_fail(VIVID_DISPLAY_CONSUMER_IS_RECEIVER(self));

    if (self->source_id != 0) {
        g_source_remove(self->source_id);
        self->source_id = 0;
    }

    receiver_clear_recv_state(self);
}

gboolean
vivid_display_consumer_receiver_get_running(VividDisplayConsumerReceiver* self)
{
    g_return_val_if_fail(VIVID_DISPLAY_CONSUMER_IS_RECEIVER(self), FALSE);

    return self->source_id != 0;
}
