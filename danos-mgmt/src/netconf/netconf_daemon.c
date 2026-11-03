/*
 * DANOS-Open Management: shared NETCONF daemon
 *
 * The SSH subsystem historically ran one process per NETCONF session, so the
 * candidate datastore, lock table and pending confirmed commit lived inside
 * that process.  RFC 6241 candidate locking and confirmed-commit persist
 * tokens are defined across sessions, so they must be hosted by one process.
 *
 * `danos-netconfd` owns the datastores and runs one session engine thread per
 * accepted Unix-socket connection.  `danos-netconf-subsystem` connects to it,
 * sends a one-line identity preamble and then relays the SSH channel bytes
 * unchanged.  The daemon keeps the same wire protocol as the in-process
 * subsystem, including the server hello.
 */

#define _GNU_SOURCE 1

#include "netconf.h"
#include "netconf_datastore.h"
#include "netconf_session.h"
#include "netconf_role.h"
#include "../authz/authz.h"

#include <danos/security/rbac.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>

#define NETCONF_DEFAULT_SOCKET "/run/danos/netconf.sock"
#define NETCONF_HANDSHAKE_MAX  512
#define NETCONF_HANDSHAKE_MAGIC "DANOS-NETCONF/1"
#define NETCONF_MAX_CLIENTS    128
#define NETCONF_HELLO_TIMEOUT_S 10

struct client_conn {
    int                 fd;
    struct client_conn *next;
};

static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_clients_cv = PTHREAD_COND_INITIALIZER;
static struct client_conn *g_clients;
static size_t          g_client_count;
static bool            g_stopping;

static volatile sig_atomic_t g_signal_stop;
static int              g_lock_fd = -1;
static dev_t            g_socket_dev;
static ino_t            g_socket_ino;

static void on_signal(int sig)
{
    (void)sig;
    g_signal_stop = 1;
}


/* Read the one-line preamble from the subsystem proxy.  Read a byte at a time
 * so no NETCONF hello byte is consumed with it. */
static int read_handshake(int fd, char *line, size_t cap)
{
    size_t len = 0;
    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        if (c == '\n') break;
        if (len + 1 >= cap) return -1;
        line[len++] = c;
    }
    line[len] = '\0';
    return 0;
}

/* Authenticate the claimed user against the kernel-supplied peer uid.  Both
 * the credential lookup and the NSS lookup fail closed: an unresolvable user
 * or an unavailable SO_PEERCRED is a rejected connection, never an implicit
 * trust decision. */
static bool peer_identity_ok(int fd, const char *user, uid_t *uid_out)
{
#ifdef SO_PEERCRED
    if (!user || !*user) return false;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;

    struct passwd *pw = getpwnam(user);
    if (!pw) return false;
    if (pw->pw_uid != cred.uid) return false;
    if (uid_out) *uid_out = cred.uid;
    return true;
#else
    (void)fd;
    (void)user;
    (void)uid_out;
    return false;   /* no peer credential support: refuse instead of guessing */
#endif
}

static bool client_register(int fd)
{
    struct client_conn *c = malloc(sizeof(*c));
    if (!c) return false;
    c->fd = fd;
    pthread_mutex_lock(&g_clients_lock);
    c->next = g_clients;
    g_clients = c;
    g_client_count++;
    pthread_mutex_unlock(&g_clients_lock);
    return true;
}

static void client_unregister(int fd)
{
    pthread_mutex_lock(&g_clients_lock);
    struct client_conn **pp = &g_clients;
    while (*pp) {
        if ((*pp)->fd == fd) {
            struct client_conn *dead = *pp;
            *pp = dead->next;
            free(dead);
            if (g_client_count) g_client_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    if (g_client_count == 0) pthread_cond_broadcast(&g_clients_cv);
    pthread_mutex_unlock(&g_clients_lock);
}

struct client_arg {
    int               fd;
    char              user[64];
    char              requested_role[64];   /* audit context only */
    danos_sec_role_t  role;                 /* derived from the peer uid */
};

static void *client_thread(void *arg)
{
    struct client_arg *ca = arg;
    int fd = ca->fd;
    char user[64];
    danos_sec_role_t role = ca->role;
    char requested_role[64];
    snprintf(user, sizeof(user), "%s", ca->user);
    snprintf(requested_role, sizeof(requested_role), "%s", ca->requested_role);
    free(ca);

    const char *effective = danos_rbac_role_name(role);
    if (requested_role[0] &&
        (!effective || strcmp(requested_role, effective) != 0)) {
        fprintf(stderr,
                "netconfd: user '%s' requested role '%s'; using '%s' from policy\n",
                user, requested_role, effective ? effective : "none");
    }

    netconf_session_t *s = netconf_session_new(fd, fd, user, role);
    if (!s) {
        fprintf(stderr, "netconfd: session handshake failed\n");
        goto out;
    }

    struct timeval no_to = { .tv_sec = 0, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_to, sizeof(no_to));

    int rc = netconf_session_run(s);
    if (rc == 1 && netconf_session_timed_out(s))
        fprintf(stderr, "netconfd: closing idle session %llu after %u s\n",
                (unsigned long long)netconf_session_id(s),
                netconf_session_idle_timeout());
    else if (rc != 0)
        fprintf(stderr, "netconfd: session ended on a protocol error\n");

    /* A disconnect must release locks and roll back a non-persisted
     * confirmed commit, exactly as <close-session> would. */
    netconf_ds_release_session(netconf_session_id(s));
    netconf_session_free(s);

out:
    client_unregister(fd);
    close(fd);
    return NULL;
}

static int listen_socket(const char *path, gid_t gid, mode_t mode)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "netconfd: socket path too long: %s\n", path);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    /* A single-instance lock stops a second daemon from unlinking the live
     * socket.  The lock file is separate from the socket so the inode check
     * below still identifies our own bind. */
    char lock_path[sizeof(addr.sun_path) + 8];
    int n = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    if (n < 0 || (size_t)n >= sizeof(lock_path)) return -1;
    g_lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_lock_fd < 0) {
        perror("netconfd: lock file");
        return -1;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "netconfd: another instance owns %s\n", path);
        close(g_lock_fd);
        g_lock_fd = -1;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("netconfd: socket");
        return -1;
    }

    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("netconfd: bind");
        close(fd);
        return -1;
    }
    /* The SSH subsystem connects as the authenticated user.  The socket is
     * group-owned by the configured service group; the directory is the
     * outer boundary.  A world-writable socket is never a good default. */
    if (gid != (gid_t)-1 && chown(path, (uid_t)-1, gid) != 0) {
        perror("netconfd: chown socket");
        close(fd);
        unlink(path);
        return -1;
    }
    if (chmod(path, mode) != 0) {
        perror("netconfd: chmod socket");
        close(fd);
        unlink(path);
        return -1;
    }
    if (mode & 0002)
        fprintf(stderr,
                "netconfd: warning: world-writable socket mode 0%o\n", mode);

    if (listen(fd, 64) != 0) {
        perror("netconfd: listen");
        close(fd);
        unlink(path);
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        perror("netconfd: stat socket");
        close(fd);
        unlink(path);
        return -1;
    }
    g_socket_dev = st.st_dev;
    g_socket_ino = st.st_ino;
    return fd;
}

static void shutdown_clients(void)
{
    pthread_mutex_lock(&g_clients_lock);
    g_stopping = true;
    for (struct client_conn *c = g_clients; c; c = c->next)
        (void)shutdown(c->fd, SHUT_RDWR);
    pthread_cond_broadcast(&g_clients_cv);
    while (g_client_count > 0)
        pthread_cond_wait(&g_clients_cv, &g_clients_lock);
    pthread_mutex_unlock(&g_clients_lock);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--socket <path>] [--startup <path>]\n"
            "          [--socket-group <name>] [--socket-mode <octal>]\n"
            "          [--idle-timeout <seconds>]\n"
            "  socket defaults to $DANOS_NETCONF_SOCKET or %s\n"
            "  mode defaults to 0660; group defaults to the process egid\n"
            "  idle timeout defaults to %u s (0 disables); env "
            "DANOS_NETCONF_IDLE_TIMEOUT is also accepted\n",
            argv0, NETCONF_DEFAULT_SOCKET,
            NETCONF_SESSION_IDLE_TIMEOUT_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *socket_path = NULL;
    const char *startup_path = NULL;
    const char *socket_group = NULL;
    unsigned long socket_mode = 0660;
    unsigned long idle_timeout = NETCONF_SESSION_IDLE_TIMEOUT_DEFAULT;

    const char *idle_env = getenv("DANOS_NETCONF_IDLE_TIMEOUT");
    if (idle_env && *idle_env) {
        char *end = NULL;
        idle_timeout = strtoul(idle_env, &end, 10);
        if (!end || *end != '\0' || idle_timeout > 604800UL) {
            fprintf(stderr, "netconfd: invalid DANOS_NETCONF_IDLE_TIMEOUT '%s'\n",
                    idle_env);
            return 2;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--startup") == 0 && i + 1 < argc) {
            startup_path = argv[++i];
        } else if (strcmp(argv[i], "--socket-group") == 0 && i + 1 < argc) {
            socket_group = argv[++i];
        } else if (strcmp(argv[i], "--socket-mode") == 0 && i + 1 < argc) {
            char *end = NULL;
            socket_mode = strtoul(argv[++i], &end, 8);
            if (!end || *end != '\0' || socket_mode > 0777) {
                fprintf(stderr, "netconfd: invalid --socket-mode '%s'\n",
                        argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--idle-timeout") == 0 && i + 1 < argc) {
            char *end = NULL;
            idle_timeout = strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || idle_timeout > 604800UL) {
                fprintf(stderr, "netconfd: invalid --idle-timeout '%s'\n",
                        argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!socket_path || !*socket_path)
        socket_path = getenv("DANOS_NETCONF_SOCKET");
    if (!socket_path || !*socket_path)
        socket_path = NETCONF_DEFAULT_SOCKET;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    gid_t socket_gid = (gid_t)-1;
    if (socket_group && *socket_group) {
        struct group *gr = getgrnam(socket_group);
        if (!gr) {
            fprintf(stderr, "netconfd: unknown socket group '%s'\n",
                    socket_group);
            return 2;
        }
        socket_gid = gr->gr_gid;
    }

    (void)danos_authz_init(NULL);
    netconf_session_set_idle_timeout((unsigned)idle_timeout);
    netconf_ds_init(startup_path);
    if (!netconf_ds_ready()) {
        fprintf(stderr, "netconfd: refusing to start with an unreadable running "
                "datastore\n");
        netconf_ds_fini();
        danos_authz_shutdown();
        return 1;
    }

    int lfd = listen_socket(socket_path, socket_gid, (mode_t)socket_mode);
    if (lfd < 0) {
        netconf_ds_fini();
        danos_authz_shutdown();
        return 1;
    }

    fprintf(stderr, "netconfd: listening on %s (idle timeout %lu s)\n",
            socket_path, idle_timeout);

    while (!g_signal_stop) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("netconfd: accept");
            break;
        }

        /* Bound the preamble read: this runs on the accept loop, so a local
         * client that connects and stays silent would otherwise stall every
         * later session.  The hello timeout is applied after the preamble and
         * cleared by the session thread once the client hello is parsed. */
        struct timeval hs_to = { .tv_sec = 5, .tv_usec = 0 };
        (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &hs_to, sizeof(hs_to));

        char line[NETCONF_HANDSHAKE_MAX];
        if (read_handshake(cfd, line, sizeof(line)) != 0) {
            fprintf(stderr, "netconfd: connection without identity preamble\n");
            close(cfd);
            continue;
        }
        char magic[32] = {0}, user[64] = {0}, role[64] = {0};
        int fields = sscanf(line, "%31s %63s %63s", magic, user, role);
        if (fields < 2 ||
            strcmp(magic, NETCONF_HANDSHAKE_MAGIC) != 0) {
            fprintf(stderr, "netconfd: malformed identity preamble\n");
            close(cfd);
            continue;
        }
        if (strcmp(user, "-") == 0) user[0] = '\0';
        if (fields < 3 || strcmp(role, "-") == 0) role[0] = '\0';

        uid_t peer_uid = 0;
        if (!peer_identity_ok(cfd, user, &peer_uid)) {
            fprintf(stderr,
                    "netconfd: refusing session claiming '%s' without a matching peer uid\n",
                    user);
            close(cfd);
            continue;
        }

        struct client_arg *ca = calloc(1, sizeof(*ca));
        if (!ca) {
            close(cfd);
            continue;
        }
        ca->fd = cfd;
        ca->role = netconf_role_resolve(user, peer_uid);
        snprintf(ca->user, sizeof(ca->user), "%s", user);
        snprintf(ca->requested_role, sizeof(ca->requested_role), "%s", role);

        /* Register before starting the thread so shutdown_clients() cannot
         * observe zero clients while a session thread is still starting. */
        if (!client_register(cfd)) {
            fputs("netconfd: connection limit reached\n", stderr);
            free(ca);
            close(cfd);
            continue;
        }

        /* Bound the NETCONF <hello> wait so a client that sends a valid
         * preamble and then stalls cannot pin a session thread forever.  The
         * timeout is cleared by the session thread once the hello is read. */
        struct timeval hello_to = { .tv_sec = NETCONF_HELLO_TIMEOUT_S,
                                    .tv_usec = 0 };
        (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &hello_to,
                         sizeof(hello_to));

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ca) != 0) {
            fputs("netconfd: cannot start session thread\n", stderr);
            client_unregister(cfd);
            free(ca);
            close(cfd);
            continue;
        }
        pthread_detach(th);
    }

    close(lfd);
    struct stat st;
    if (lstat(socket_path, &st) == 0 &&
        st.st_dev == g_socket_dev && st.st_ino == g_socket_ino)
        unlink(socket_path);
    shutdown_clients();
    netconf_ds_fini();
    danos_authz_shutdown();
    return 0;
}
