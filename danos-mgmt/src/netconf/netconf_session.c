/*
 * DANOS-Open Management: transport-independent NETCONF session engine
 */

#include "netconf_session.h"
#include "netconf_datastore.h"
#include "netconf_framing.h"
#include "../authz/authz.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <limits.h>
#include <unistd.h>

/* A client <hello> is small; RFC 6242 does not bound it, so keep a generous
 * local ceiling consistent with the framing limits. */
#define NETCONF_HELLO_MAX 262144u

static unsigned g_idle_timeout = NETCONF_SESSION_IDLE_TIMEOUT_DEFAULT;

void netconf_session_set_idle_timeout(unsigned seconds)
{
    g_idle_timeout = seconds;
}

unsigned netconf_session_idle_timeout(void)
{
    return g_idle_timeout;
}

struct netconf_session {
    int                       rfd;
    int                       wfd;
    netconf_ctx_t             ctx;
    bool                      base11;
    bool                      close_requested;
    bool                      timed_out;
    netconf_frame_decoder_t  *dec;
};

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

/* Read the client <hello>, which always uses end-of-message framing.  The
 * buffer is NUL-terminated so the delimiter can be located with strstr. */
static int read_client_hello(int fd, char *buf, size_t cap, size_t *len_out,
                             size_t *eom_pos)
{
    size_t len = 0;

    for (;;) {
        if (len + 1 >= cap) return -1;
        ssize_t n = read(fd, buf + len, cap - len - 1);
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

static int on_message(const char *msg, size_t len, void *user)
{
    netconf_session_t *s = user;
    (void)len;

    netconf_rpc_type_t type = netconf_parse_rpc(msg);
    char *reply = netconf_handle_rpc(&s->ctx, msg);
    if (!reply) return -1;

    size_t frame_len = 0;
    char *frame = netconf_frame_encode(s->base11, reply, strlen(reply),
                                       &frame_len);
    free(reply);
    if (!frame) return -1;

    int rc = write_all(s->wfd, frame, frame_len);
    free(frame);
    if (rc != 0) return -1;

    if (type == NETCONF_RPC_CLOSE_SESSION) s->close_requested = true;
    return 0;
}

netconf_session_t *netconf_session_new(int rfd, int wfd,
                                       const char *identity,
                                       danos_sec_role_t role)
{
    netconf_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->rfd = rfd;
    s->wfd = wfd;

    netconf_init(&s->ctx, 830);
    if (!netconf_ds_ready()) {
        netconf_fini(&s->ctx);
        free(s);
        return NULL;
    }
    s->ctx.session_id = netconf_new_session_id();
    if (!s->ctx.session_id) s->ctx.session_id = 1;
    if (identity && *identity)
        snprintf(s->ctx.identity, sizeof(s->ctx.identity), "%s", identity);
    s->ctx.role = role ? role : danos_authz_default_role();

    char *hello = netconf_hello_message(s->ctx.session_id);
    if (!hello) { fprintf(stderr, "netconf: cannot build server hello\n"); goto fail; }

    size_t hello_len = 0;
    char *hello_frame = netconf_frame_encode(false, hello, strlen(hello),
                                             &hello_len);
    free(hello);
    if (!hello_frame) { fprintf(stderr, "netconf: cannot frame server hello\n"); goto fail; }
    int hrc = write_all(s->wfd, hello_frame, hello_len);
    free(hello_frame);
    if (hrc != 0) { fprintf(stderr, "netconf: cannot send server hello\n"); goto fail; }

    char *buf = malloc(NETCONF_HELLO_MAX);
    if (!buf) goto fail;
    size_t len = 0, eom = 0;
    if (read_client_hello(s->rfd, buf, NETCONF_HELLO_MAX, &len, &eom) != 0) {
        fprintf(stderr, "netconf: client hello read failed\n");
        free(buf);
        goto fail;
    }
    buf[eom] = '\0';    /* inspect only the hello body */
    if (!strstr(buf, "urn:ietf:params:netconf:base:1.0")) {
        fprintf(stderr, "netconf: client hello lacks base 1.0\n");
        free(buf);
        goto fail;
    }
    s->base11 = netconf_hello_advertises_base11(buf);

    s->dec = netconf_frame_decoder_new(s->base11);
    if (!s->dec) {
        fprintf(stderr, "netconf: frame decoder allocation failed\n");
        free(buf);
        goto fail;
    }

    /* Bytes sent in the same segment after the client hello delimiter may
     * already contain RPCs; feed them before blocking on the socket. */
    size_t done = eom + 6;
    if (len > done &&
        netconf_frame_feed(s->dec, buf + done, len - done, on_message, s) != 0) {
        fprintf(stderr, "netconf: pipelined frame rejected\n");
        free(buf);
        goto fail;
    }
    free(buf);
    return s;

fail:
    netconf_frame_decoder_free(s->dec);
    netconf_fini(&s->ctx);
    free(s);
    return NULL;
}

uint64_t netconf_session_id(const netconf_session_t *s)
{
    return s ? s->ctx.session_id : 0;
}

bool netconf_session_close_requested(const netconf_session_t *s)
{
    return s && s->close_requested;
}

bool netconf_session_timed_out(const netconf_session_t *s)
{
    return s && s->timed_out;
}

int netconf_session_run(netconf_session_t *s)
{
    if (!s || !s->dec) return -1;

    char chunk[4096];
    while (!s->close_requested) {
        if (g_idle_timeout) {
            long timeout_ms = g_idle_timeout > (unsigned)(INT_MAX / 1000)
                ? INT_MAX : (long)g_idle_timeout * 1000L;
            struct pollfd p = { .fd = s->rfd, .events = POLLIN };
            int pr = poll(&p, 1, (int)timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (pr == 0) {
                s->timed_out = true;
                return 1;
            }
        }
        ssize_t n = read(s->rfd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        if (netconf_frame_feed(s->dec, chunk, (size_t)n, on_message, s) != 0)
            return -1;
    }
    return 0;
}

void netconf_session_free(netconf_session_t *s)
{
    if (!s) return;
    netconf_frame_decoder_free(s->dec);
    netconf_fini(&s->ctx);
    free(s);
}
