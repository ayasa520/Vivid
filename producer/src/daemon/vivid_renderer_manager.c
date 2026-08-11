#define _GNU_SOURCE

#include "vivid_renderer_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define VIVID_RENDERER_CHILD_IPC_FD 3

struct _VividRendererManager
{
    const VividRendererRegistry* registry;
    GMainContext* context;
    VividRendererLifecyclePolicy policy;
    VividRendererProcessObserver observer;
    gpointer observer_data;
    guint64 next_instance_id;
    GPtrArray* processes;
};

G_DEFINE_QUARK(vivid-renderer-manager-error, vivid_renderer_manager_error)

static gboolean
descriptor_belongs_to_registry(const VividRendererManager* manager,
                               const VividRendererDescriptor* descriptor)
{
    if (!manager || !descriptor)
        return FALSE;
    const gchar* id = vivid_renderer_descriptor_id(descriptor);
    return vivid_renderer_registry_lookup_id(manager->registry, id) == descriptor;
}

static gint
open_pidfd(pid_t pid)
{
    const long result = syscall(SYS_pidfd_open, pid, 0u);
    return result < 0 ? -errno : (gint)result;
}

static void
kill_and_reap_spawn_failure(pid_t pid)
{
    if (pid <= 0)
        return;
    if (kill(-pid, SIGKILL) < 0 && errno != ESRCH)
        g_warning("VividRendererManager: failed to kill spawn transaction pgid=%d: %s",
                  pid,
                  g_strerror(errno));
    gint status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

static gint
configure_spawn_actions(posix_spawn_file_actions_t* actions,
                        gint child_socket,
                        gint log_write_fd)
{
    gint result = posix_spawn_file_actions_init(actions);
    if (result != 0)
        return result;
    result = posix_spawn_file_actions_addopen(actions,
                                              STDIN_FILENO,
                                              "/dev/null",
                                              O_RDONLY,
                                              0);
    if (result == 0) {
        result = posix_spawn_file_actions_adddup2(actions,
                                                  child_socket,
                                                  VIVID_RENDERER_CHILD_IPC_FD);
    }
    if (result == 0)
        result = posix_spawn_file_actions_adddup2(actions, log_write_fd, STDOUT_FILENO);
    if (result == 0)
        result = posix_spawn_file_actions_adddup2(actions, log_write_fd, STDERR_FILENO);
    if (result == 0) {
        /*
         * The daemon is multi-threaded, so there is no child-side close loop.
         * glibc applies closefrom as part of posix_spawn and leaves only
         * stdin, the log descriptors, and the fixed IPC descriptor reachable
         * after exec, independently of the daemon's current client FD table.
         */
        result = posix_spawn_file_actions_addclosefrom_np(
            actions,
            VIVID_RENDERER_CHILD_IPC_FD + 1);
    }
    if (result != 0)
        posix_spawn_file_actions_destroy(actions);
    return result;
}

static gint
configure_spawn_attributes(posix_spawnattr_t* attributes)
{
    gint result = posix_spawnattr_init(attributes);
    if (result != 0)
        return result;

    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    short flags = POSIX_SPAWN_SETPGROUP |
        POSIX_SPAWN_SETSIGMASK |
        POSIX_SPAWN_SETSIGDEF;
    result = posix_spawnattr_setflags(attributes, flags);
    if (result == 0)
        result = posix_spawnattr_setpgroup(attributes, 0);
    if (result == 0)
        result = posix_spawnattr_setsigmask(attributes, &empty_mask);
    if (result == 0)
        result = posix_spawnattr_setsigdefault(attributes, &defaults);
    if (result != 0)
        posix_spawnattr_destroy(attributes);
    return result;
}

static guint64
manager_next_instance_id(VividRendererManager* manager)
{
    guint64 instance_id = manager->next_instance_id++;
    if (instance_id == 0u)
        instance_id = manager->next_instance_id++;
    return instance_id;
}

VividRendererManager*
vivid_renderer_manager_new(
    const VividRendererRegistry* registry,
    GMainContext* context,
    const VividRendererLifecyclePolicy* policy,
    const VividRendererProcessObserver* observer,
    gpointer observer_data,
    GError** error)
{
    if (!registry) {
        g_set_error_literal(error,
                            VIVID_RENDERER_MANAGER_ERROR,
                            VIVID_RENDERER_MANAGER_ERROR_INVALID,
                            "renderer manager requires a loaded registry");
        return NULL;
    }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0) {
        g_set_error(error,
                    VIVID_RENDERER_MANAGER_ERROR,
                    VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
                    "failed to make vivid-producer a renderer child subreaper: %s",
                    g_strerror(errno));
        return NULL;
    }

    VividRendererManager* manager = g_new0(VividRendererManager, 1);
    manager->registry = registry;
    manager->context = g_main_context_ref(context ? context : g_main_context_default());
    vivid_renderer_lifecycle_policy_init(&manager->policy);
    if (policy)
        manager->policy = *policy;
    if (observer)
        manager->observer = *observer;
    manager->observer_data = observer_data;
    manager->next_instance_id = 1;
    manager->processes =
        g_ptr_array_new_with_free_func((GDestroyNotify)vivid_renderer_process_free);
    return manager;
}

void
vivid_renderer_manager_free(VividRendererManager* manager)
{
    if (!manager)
        return;
    g_clear_pointer(&manager->processes, g_ptr_array_unref);
    g_clear_pointer(&manager->context, g_main_context_unref);
    g_free(manager);
}

VividRendererProcess*
vivid_renderer_manager_spawn(
    VividRendererManager* manager,
    const VividRendererDescriptor* descriptor,
    const gchar* route_id,
    const gchar* identity_hash,
    GError** error)
{
    if (!manager || !descriptor_belongs_to_registry(manager, descriptor) ||
        !route_id || !*route_id || !identity_hash || !*identity_hash) {
        g_set_error_literal(error,
                            VIVID_RENDERER_MANAGER_ERROR,
                            VIVID_RENDERER_MANAGER_ERROR_INVALID,
                            "renderer spawn requires a registry descriptor, route id, and identity hash");
        return NULL;
    }

    gint sockets[2] = {-1, -1};
    gint result = vivid_renderer_transport_socket_pair(sockets);
    if (result < 0) {
        g_set_error(error,
                    VIVID_RENDERER_MANAGER_ERROR,
                    VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
                    "failed to create renderer SEQPACKET socketpair: %s",
                    g_strerror(-result));
        return NULL;
    }
    gint log_pipe[2] = {-1, -1};
    if (pipe2(log_pipe, O_CLOEXEC) < 0) {
        const gint saved_errno = errno;
        close(sockets[0]);
        close(sockets[1]);
        g_set_error(error,
                    VIVID_RENDERER_MANAGER_ERROR,
                    VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
                    "failed to create renderer log pipe: %s",
                    g_strerror(saved_errno));
        return NULL;
    }

    const guint64 instance_id = manager_next_instance_id(manager);
    g_autofree gchar* ipc_argument =
        g_strdup_printf("--renderer-ipc-fd=%d", VIVID_RENDERER_CHILD_IPC_FD);
    g_autofree gchar* id_argument =
        g_strdup_printf("--renderer-id=%s", vivid_renderer_descriptor_id(descriptor));
    g_autofree gchar* instance_argument =
        g_strdup_printf("--renderer-instance-id=%" G_GUINT64_FORMAT, instance_id);
    gchar* child_argv[] = {
        (gchar*)vivid_renderer_descriptor_executable(descriptor),
        ipc_argument,
        id_argument,
        instance_argument,
        NULL,
    };
    g_auto(GStrv) child_environment = g_get_environ();
    const gchar* preload = vivid_renderer_descriptor_preload(descriptor);
    if (preload) {
        const gchar* inherited =
            g_environ_getenv(child_environment, "LD_PRELOAD");
        g_autofree gchar* preload_value = inherited && *inherited
            ? g_strdup_printf("%s:%s", preload, inherited)
            : g_strdup(preload);
        child_environment = g_environ_setenv(child_environment,
                                             "LD_PRELOAD",
                                             preload_value,
                                             TRUE);
    }

    posix_spawn_file_actions_t actions;
    gboolean actions_initialized = FALSE;
    result = configure_spawn_actions(&actions, sockets[1], log_pipe[1]);
    if (result == 0)
        actions_initialized = TRUE;
    posix_spawnattr_t attributes;
    gboolean attributes_initialized = FALSE;
    if (result == 0) {
        result = configure_spawn_attributes(&attributes);
        if (result == 0)
            attributes_initialized = TRUE;
    }

    pid_t pid = -1;
    if (result == 0) {
        result = posix_spawn(&pid,
                             vivid_renderer_descriptor_executable(descriptor),
                             &actions,
                             &attributes,
                             child_argv,
                             child_environment);
    }
    if (attributes_initialized)
        posix_spawnattr_destroy(&attributes);
    if (actions_initialized)
        posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    close(log_pipe[1]);
    sockets[1] = -1;
    log_pipe[1] = -1;

    if (result != 0) {
        close(sockets[0]);
        close(log_pipe[0]);
        g_set_error(error,
                    VIVID_RENDERER_MANAGER_ERROR,
                    VIVID_RENDERER_MANAGER_ERROR_SPAWN,
                    "failed to spawn renderer '%s' executable=%s: %s",
                    vivid_renderer_descriptor_id(descriptor),
                    vivid_renderer_descriptor_executable(descriptor),
                    g_strerror(result));
        return NULL;
    }

    gint pidfd = open_pidfd(pid);
    if (pidfd < 0) {
        kill_and_reap_spawn_failure(pid);
        close(sockets[0]);
        close(log_pipe[0]);
        g_set_error(error,
                    VIVID_RENDERER_MANAGER_ERROR,
                    VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
                    "pidfd_open failed for renderer '%s' pid=%d: %s",
                    vivid_renderer_descriptor_id(descriptor),
                    pid,
                    g_strerror(-pidfd));
        return NULL;
    }

    result = vivid_renderer_transport_set_nonblocking(sockets[0], TRUE);
    if (result == 0) {
        const gint old_flags = fcntl(log_pipe[0], F_GETFL);
        if (old_flags < 0 || fcntl(log_pipe[0], F_SETFL, old_flags | O_NONBLOCK) < 0)
            result = -errno;
    }
    if (result < 0) {
        kill_and_reap_spawn_failure(pid);
        close(pidfd);
        close(sockets[0]);
        close(log_pipe[0]);
        g_set_error(error,
                    VIVID_RENDERER_MANAGER_ERROR,
                    VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
                    "failed to configure renderer parent descriptors pid=%d: %s",
                    pid,
                    g_strerror(-result));
        return NULL;
    }

    VividRendererProcess* process = vivid_renderer_process_new_spawned(
        descriptor,
        route_id,
        identity_hash,
        instance_id,
        pid,
        pidfd,
        pid,
        sockets[0],
        log_pipe[0],
        manager->context,
        &manager->policy,
        &manager->observer,
        manager->observer_data);
    if (!process) {
        kill_and_reap_spawn_failure(pid);
        close(pidfd);
        close(sockets[0]);
        close(log_pipe[0]);
        g_set_error_literal(error,
                            VIVID_RENDERER_MANAGER_ERROR,
                            VIVID_RENDERER_MANAGER_ERROR_SYSTEM,
                            "failed to allocate renderer process state");
        return NULL;
    }

    g_ptr_array_add(manager->processes, process);
    g_message("VividRendererManager: route=%s instance=%" G_GUINT64_FORMAT
              " renderer=%s kind=%u pid=%d pgid=%d identity=%s executable=%s",
              route_id,
              instance_id,
              vivid_renderer_descriptor_id(descriptor),
              vivid_renderer_descriptor_kind(descriptor),
              pid,
              pid,
              identity_hash,
              vivid_renderer_descriptor_executable(descriptor));
    return process;
}

void
vivid_renderer_manager_collect_reaped(VividRendererManager* manager)
{
    if (!manager || !manager->processes)
        return;

    /*
     * Process observers execute from inside the pidfd/group-reap source, so a
     * reaped object must not be released by the observer itself. Route owners
     * call this method from a later main-context dispatch after every reap;
     * descending removal keeps all unreaped process pointers stable.
     */
    for (guint i = manager->processes->len; i > 0; i--) {
        VividRendererProcess* process =
            g_ptr_array_index(manager->processes, i - 1);
        if (vivid_renderer_process_is_reaped(process))
            g_ptr_array_remove_index(manager->processes, i - 1);
    }
}

guint
vivid_renderer_manager_process_count(const VividRendererManager* manager)
{
    return manager && manager->processes ? manager->processes->len : 0;
}

VividRendererProcess*
vivid_renderer_manager_process_at(const VividRendererManager* manager,
                                  guint index)
{
    if (!manager || !manager->processes || index >= manager->processes->len)
        return NULL;
    return g_ptr_array_index(manager->processes, index);
}
