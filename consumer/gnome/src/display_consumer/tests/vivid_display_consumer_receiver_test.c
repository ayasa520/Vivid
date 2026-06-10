#define _GNU_SOURCE

#include "../vivid_display_consumer_receiver.h"

#include "../../protocol/vivid_display_protocol.h"

#include <assert.h>
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
    GMainLoop* loop;
    gboolean saw_frame;
} TestContext;

static void
on_frame(VividDisplayConsumerReceiver* receiver,
         guint                          opcode,
         GBytes*                        body,
         GUnixFDList*                   fd_list,
         gpointer                       user_data)
{
    (void)receiver;
    TestContext* ctx = user_data;

    assert(opcode == VIVID_DISPLAY_EVT_BIND_BUFFERS);

    gsize body_len = 0;
    const guint8* body_data = g_bytes_get_data(body, &body_len);
    const guint8 expected_body[] = {0x48, 0x41, 0x4e, 0x41, 0x42, 0x49};
    assert(body_len == sizeof(expected_body));
    assert(memcmp(body_data, expected_body, sizeof(expected_body)) == 0);

    assert(g_unix_fd_list_get_length(fd_list) == 1);

    GError* error = NULL;
    const int fd = g_unix_fd_list_get(fd_list, 0, &error);
    assert(error == NULL);
    assert(fd >= 0);

    const char expected_fd_payload[] = "receiver-fd-payload";
    char fd_payload[sizeof(expected_fd_payload)] = {0};
    assert(read(fd, fd_payload, strlen(expected_fd_payload)) ==
           (ssize_t)strlen(expected_fd_payload));
    assert(memcmp(fd_payload,
                  expected_fd_payload,
                  strlen(expected_fd_payload)) == 0);
    close(fd);

    ctx->saw_frame = TRUE;
    g_main_loop_quit(ctx->loop);
}

static void
on_protocol_error(VividDisplayConsumerReceiver* receiver,
                  gint                           code,
                  const char*                    message,
                  gpointer                       user_data)
{
    (void)receiver;
    (void)user_data;
    g_error("unexpected receiver protocol error code=%d message=%s",
            code,
            message ? message : "(none)");
}

static gboolean
on_timeout(gpointer user_data)
{
    TestContext* ctx = user_data;
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}

int
main(void)
{
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);

    const char fd_payload[] = "receiver-fd-payload";
    assert(write(pipe_fds[1], fd_payload, strlen(fd_payload)) ==
           (ssize_t)strlen(fd_payload));

    GError* error = NULL;
    GSocket* socket = g_socket_new_from_fd(sockets[1], &error);
    g_assert_no_error(error);
    assert(socket != NULL);
    sockets[1] = -1;

    GSocketConnection* connection =
        g_socket_connection_factory_create_connection(socket);
    assert(connection != NULL);

    VividDisplayConsumerReceiver* receiver =
        vivid_display_consumer_receiver_new(connection);
    assert(receiver != NULL);

    TestContext ctx = {0};
    ctx.loop = g_main_loop_new(NULL, FALSE);

    g_signal_connect(receiver, "frame", G_CALLBACK(on_frame), &ctx);
    g_signal_connect(receiver, "protocol-error", G_CALLBACK(on_protocol_error), &ctx);
    assert(vivid_display_consumer_receiver_start(receiver));

    const guint8 body[] = {0x48, 0x41, 0x4e, 0x41, 0x42, 0x49};
    const int send_fds[] = {pipe_fds[0]};
    assert(vivid_display_send_frame(sockets[0],
                                     VIVID_DISPLAY_EVT_BIND_BUFFERS,
                                     body,
                                     sizeof(body),
                                     send_fds,
                                     1) == 0);

    g_timeout_add(1000, on_timeout, &ctx);
    g_main_loop_run(ctx.loop);

    assert(ctx.saw_frame);

    vivid_display_consumer_receiver_stop(receiver);
    g_object_unref(receiver);
    g_object_unref(connection);
    g_object_unref(socket);
    g_main_loop_unref(ctx.loop);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(sockets[0]);
    if (sockets[1] >= 0)
        close(sockets[1]);

    return 0;
}
