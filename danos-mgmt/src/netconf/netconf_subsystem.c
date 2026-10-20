/*
 * DANOS-Open Management: NETCONF over SSH subsystem
 *
 * OpenSSH performs transport, authentication and encryption; this program is
 * the `Subsystem netconf` process that sshd spawns with the session's stdin
 * and stdout connected to the SSH channel. It owns the RFC 6242 framing and
 * the NETCONF message layer.
 *
 * The SSH user is the authenticated identity. DANOS_NETCONF_ROLE selects the
 * RBAC role for the session (admin, operator, viewer, security-admin);
 * without it the deployment default applies.
 */

#include "netconf.h"
#include "netconf_datastore.h"
#include "netconf_framing.h"
#include "../authz/authz.h"

#include <danos/security/rbac.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NETCONF_HELLO_MAX 262144u

static int write_all(const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Read the client <hello>, which always uses end-of-message framing. The
 * buffer is NUL-terminated so the delimiter can be located with strstr. */
static int read_client_hello(char *buf, size_t cap, size_t *len_out,
                             size_t *eom_pos)
{
    size_t len = 0;

    for (;;) {
        if (len + 1 >= cap) return -1;
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        len += (size_t)n;
        buf[len] = '\0';

        const char *hit = strstr(buf, "]]>]]>");
        if (hit) {
            *len_out = len;
            *eom_pos = (size_t)(hit - buf);
            return 0;
        }
    }
}

struct session {
    netconf_ctx_t *ctx;
    bool base11;
    bool close_requested;
};

static int on_message(const char *msg, size_t len, void *user)
{
    struct session *s = user;
    (void)len;

    netconf_rpc_type_t type = netconf_parse_rpc(msg);
    char *reply = netconf_handle_rpc(s->ctx, msg);
    if (!reply) return -1;

    size_t frame_len = 0;
    char *frame = netconf_frame_encode(s->base11, reply, strlen(reply),
                                       &frame_len);
    free(reply);
    if (!frame) return -1;

    int rc = write_all(frame, frame_len);
    free(frame);
    if (rc != 0) return -1;

    if (type == NETCONF_RPC_CLOSE_SESSION) s->close_requested = true;
    return 0;
}

static void set_session_identity(netconf_ctx_t *ctx)
{
    const char *user = getenv("LOGNAME");
    if (!user || !*user) user = getenv("USER");
    if (user && *user)
        snprintf(ctx->identity, sizeof(ctx->identity), "%s", user);

    const char *role_name = getenv("DANOS_NETCONF_ROLE");
    if (role_name && *role_name) {
        danos_sec_role_t role;
        if (danos_rbac_role_by_name(role_name, &role) == 0)
            ctx->role = role;
        else
            fprintf(stderr, "netconf: unknown DANOS_NETCONF_ROLE '%s'\n",
                    role_name);
    }
    if (!ctx->role) ctx->role = danos_authz_default_role();
}

int main(int argc, char **argv)
{
    const char *startup_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--startup") == 0 && i + 1 < argc) {
            startup_path = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--startup <path>]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    (void)danos_authz_init(NULL);

    /* An explicit --startup wins over DANOS_NETCONF_STARTUP and the
     * compiled-in default; netconf_init's later init call is a no-op. */
    netconf_ds_init(startup_path);

    netconf_ctx_t ctx;
    netconf_init(&ctx, 830);
    ctx.session_id = netconf_new_session_id();
    set_session_identity(&ctx);

    char *hello = netconf_hello_message(ctx.session_id);
    if (!hello) return 1;
    size_t hello_len = 0;
    char *hello_frame = netconf_frame_encode(false, hello, strlen(hello),
                                             &hello_len);
    free(hello);
    if (!hello_frame || write_all(hello_frame, hello_len) != 0) {
        free(hello_frame);
        return 1;
    }
    free(hello_frame);

    char *buf = malloc(NETCONF_HELLO_MAX);
    if (!buf) return 1;
    size_t len = 0, eom = 0;
    if (read_client_hello(buf, NETCONF_HELLO_MAX, &len, &eom) != 0) {
        fprintf(stderr, "netconf: no client hello\n");
        free(buf);
        return 1;
    }
    buf[eom] = '\0';    /* inspect only the hello body */
    if (!strstr(buf, "urn:ietf:params:netconf:base:1.0")) {
        fprintf(stderr, "netconf: client does not advertise base 1.0\n");
        free(buf);
        return 1;
    }
    bool base11 = netconf_hello_advertises_base11(buf);

    struct session s = { .ctx = &ctx, .base11 = base11,
                         .close_requested = false };
    netconf_frame_decoder_t *dec = netconf_frame_decoder_new(base11);
    if (!dec) {
        free(buf);
        return 1;
    }

    int rc = 0;
    size_t done = eom + 6;
    if (len > done && netconf_frame_feed(dec, buf + done, len - done,
                                         on_message, &s) != 0)
        rc = 1;

    char chunk[4096];
    while (rc == 0 && !s.close_requested) {
        ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = 1;
            break;
        }
        if (n == 0) break;
        if (netconf_frame_feed(dec, chunk, (size_t)n, on_message, &s) != 0)
            rc = 1;
    }

    netconf_frame_decoder_free(dec);
    free(buf);
    netconf_fini(&ctx);
    danos_authz_shutdown();
    return rc;
}
