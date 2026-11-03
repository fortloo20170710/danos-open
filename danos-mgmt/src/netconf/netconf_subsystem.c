/*
 * DANOS-Open Management: NETCONF over SSH subsystem
 *
 * OpenSSH performs transport, authentication and encryption; sshd spawns this
 * program as the `Subsystem netconf` process with the session's stdin and
 * stdout connected to the SSH channel.
 *
 * When a shared `danos-netconfd` is reachable, this process is a byte relay:
 * it tells the daemon which SSH identity/role the session carries and then
 * forwards the RFC 6242 stream unchanged.  That makes candidate state, locks
 * and confirmed-commit persist tokens shared across SSH sessions.
 *
 * Without a daemon it runs the session engine in-process, preserving the
 * previous laboratory/single-session behaviour.
 */

#include "netconf.h"
#include "netconf_datastore.h"
#include "netconf_session.h"
#include "../authz/authz.h"

#include <danos/security/rbac.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NETCONF_DEFAULT_SOCKET  "/run/danos/netconf.sock"
#define NETCONF_HANDSHAKE_MAX   512
#define NETCONF_HANDSHAKE_MAGIC "DANOS-NETCONF/1"

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void resolve_identity(char *identity, size_t identity_cap,
                             danos_sec_role_t *role)
{
    const char *user = getenv("LOGNAME");
    if (!user || !*user) user = getenv("USER");
    identity[0] = '\0';
    if (user && *user) snprintf(identity, identity_cap, "%s", user);

    *role = 0;
    const char *role_name = getenv("DANOS_NETCONF_ROLE");
    if (role_name && *role_name) {
        if (danos_rbac_role_by_name(role_name, role) != 0) {
            fprintf(stderr, "netconf: unknown DANOS_NETCONF_ROLE '%s'\n",
                    role_name);
            *role = 0;
        }
    }
    if (!*role) *role = danos_authz_default_role();
}

static int connect_daemon(const char *path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) return -1;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Relay the SSH stdio channel and the daemon socket in both directions.
 * EOF on stdin is half-closed to the daemon so the final replies can still
 * be delivered. */
static int proxy_loop(int sock)
{
    bool stdin_open = true;
    char buf[4096];

    for (;;) {
        struct pollfd p[2] = {
            { .fd = STDIN_FILENO, .events = stdin_open ? POLLIN : 0 },
            { .fd = sock,         .events = POLLIN },
        };
        int rc = poll(p, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) return 0;   /* daemon ended the session */
            if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) return -1;
        }

        if (stdin_open && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) {
                stdin_open = false;
                (void)shutdown(sock, SHUT_WR);
                continue;
            }
            if (write_all(sock, buf, (size_t)n) != 0) return -1;
        }
    }
}

static int run_proxy(int sock, const char *identity, danos_sec_role_t role)
{
    const char *role_name = danos_rbac_role_name(role);
    char line[NETCONF_HANDSHAKE_MAX];
    int n = snprintf(line, sizeof(line), NETCONF_HANDSHAKE_MAGIC " %s %s\n",
                     identity[0] ? identity : "-",
                     (role_name && *role_name) ? role_name : "-");
    if (n < 0 || (size_t)n >= sizeof(line)) return -1;
    if (write_all(sock, line, (size_t)n) != 0) return -1;
    return proxy_loop(sock);
}

static int run_in_process(const char *startup_path, const char *identity,
                          danos_sec_role_t role)
{
    /* An explicit --startup wins over DANOS_NETCONF_STARTUP and the
     * compiled-in default; netconf_init's later init call is a no-op. */
    netconf_ds_init(startup_path);

    netconf_session_t *s = netconf_session_new(STDIN_FILENO, STDOUT_FILENO,
                                               identity, role);
    if (!s) return 1;
    int rc = netconf_session_run(s);
    netconf_ds_release_session(netconf_session_id(s));
    netconf_session_free(s);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *startup_path = NULL;
    const char *daemon_path = NULL;
    bool no_daemon = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--startup") == 0 && i + 1 < argc) {
            startup_path = argv[++i];
        } else if (strcmp(argv[i], "--daemon") == 0 && i + 1 < argc) {
            daemon_path = argv[++i];
        } else if (strcmp(argv[i], "--no-daemon") == 0) {
            no_daemon = true;
        } else {
            fprintf(stderr,
                    "usage: %s [--startup <path>] [--daemon <socket>]"
                    " [--no-daemon]\n", argv[0]);
            return 2;
        }
    }

    const char *idle_env = getenv("DANOS_NETCONF_IDLE_TIMEOUT");
    if (idle_env && *idle_env) {
        char *end = NULL;
        unsigned long idle = strtoul(idle_env, &end, 10);
        if (!end || *end != '\0' || idle > 604800UL) {
            fprintf(stderr, "netconf: invalid DANOS_NETCONF_IDLE_TIMEOUT '%s'\n",
                    idle_env);
            return 2;
        }
        netconf_session_set_idle_timeout((unsigned)idle);
    }

    signal(SIGPIPE, SIG_IGN);
    (void)danos_authz_init(NULL);

    char identity[64];
    danos_sec_role_t role = 0;
    resolve_identity(identity, sizeof(identity), &role);

    if (!no_daemon) {
        bool explicit = true;
        const char *path = daemon_path;
        if (!path || !*path) {
            path = getenv("DANOS_NETCONF_SOCKET");
            if (!path || !*path) {
                path = NETCONF_DEFAULT_SOCKET;
                explicit = false;   /* opportunistic default */
            }
        }
        int sock = connect_daemon(path);
        if (sock >= 0) {
            int rc = run_proxy(sock, identity, role);
            close(sock);
            danos_authz_shutdown();
            return rc == 0 ? 0 : 1;
        }
        if (explicit) {
            fprintf(stderr, "netconf: cannot connect to daemon socket %s: %s\n",
                    path, strerror(errno));
            danos_authz_shutdown();
            return 1;
        }
    }

    int rc = run_in_process(startup_path, identity, role);
    danos_authz_shutdown();
    return rc;
}
