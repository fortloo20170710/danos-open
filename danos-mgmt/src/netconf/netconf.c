/*
 * DANOS-Open Management: NETCONF Server Implementation (E3)
 */

#include "netconf.h"
#include "../gnmi/model_paths.h"
#include "../authz/authz.h"
#include <danos/core/object_registry.h>
#include <danos/dpa.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void netconf_init(netconf_ctx_t *ctx, uint16_t port)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->port = port ? port : 830;
}

void netconf_fini(netconf_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->authorization);
    ctx->authorization = NULL;
}

/* Simple XML tag extraction. Matches the element name exactly, so "<get"
 * does not also select "<get-config". */
static bool has_element(const char *xml, const char *tag)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "<%s", tag);
    size_t plen = strlen(pattern);

    for (const char *p = xml; (p = strstr(p, pattern)) != NULL; p += plen) {
        switch (p[plen]) {
        case '>': case '/': case ' ': case '\t': case '\n': case '\r':
            return true;
        default:
            break;
        }
    }
    return false;
}

netconf_rpc_type_t netconf_parse_rpc(const char *xml)
{
    if (!xml) return NETCONF_RPC_UNKNOWN;

    if (has_element(xml, "hello"))          return NETCONF_RPC_HELLO;
    if (has_element(xml, "get-config"))     return NETCONF_RPC_GET_CONFIG;
    if (has_element(xml, "edit-config"))    return NETCONF_RPC_EDIT_CONFIG;
    if (has_element(xml, "get"))            return NETCONF_RPC_GET;
    if (has_element(xml, "close-session"))  return NETCONF_RPC_CLOSE_SESSION;
    if (has_element(xml, "kill-session"))   return NETCONF_RPC_KILL_SESSION;
    if (has_element(xml, "commit"))         return NETCONF_RPC_COMMIT;
    if (has_element(xml, "discard-changes"))return NETCONF_RPC_DISCARD;
    if (has_element(xml, "lock"))           return NETCONF_RPC_LOCK;
    if (has_element(xml, "unlock"))         return NETCONF_RPC_UNLOCK;

    return NETCONF_RPC_UNKNOWN;
}

uint64_t netconf_new_session_id(void)
{
    static unsigned counter;
    struct timespec ts = {0, 0};
    uint64_t mixed;

    (void)clock_gettime(CLOCK_REALTIME, &ts);
    mixed = ((uint64_t)ts.tv_sec << 20) ^ ((uint64_t)ts.tv_nsec >> 10) ^
            ((uint64_t)getpid() << 32) ^ (uint64_t)++counter;
    mixed &= 0x7fffffffULL;     /* RFC 6241: 1 .. 2^31-1 */
    return mixed ? mixed : 1;
}

char *netconf_hello_message(uint64_t session_id)
{
    static const char *const caps[] = {
        "urn:ietf:params:netconf:base:1.0",
        "urn:ietf:params:netconf:base:1.1",
        "urn:ietf:params:netconf:capability:writable-running:1.0",
        "urn:danos:yang:1.0",
    };

    if (session_id == 0 || session_id > 0x7fffffffULL) session_id = 1;

    size_t cap = 512;
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
        cap += strlen(caps[i]) + 32;

    char *hello = malloc(cap);
    if (!hello) return NULL;

    size_t off = (size_t)snprintf(hello, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
        "  <capabilities>\n");
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
        off += (size_t)snprintf(hello + off, cap - off,
            "    <capability>%s</capability>\n", caps[i]);
    off += (size_t)snprintf(hello + off, cap - off,
        "  </capabilities>\n"
        "  <session-id>%llu</session-id>\n"
        "</hello>\n",
        (unsigned long long)session_id);
    if (off >= cap) {
        free(hello);
        return NULL;
    }
    return hello;
}

static char *ok_reply(void)
{
    return strdup(
        "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
        "  <ok/>\n"
        "</rpc-reply>\n"
    );
}

static char *error_reply_tag(const char *tag, const char *msg)
{
    char *resp = malloc(512);
    if (resp) {
        snprintf(resp, 512,
            "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
            "  <rpc-error>\n"
            "    <error-type>application</error-type>\n"
            "    <error-tag>%s</error-tag>\n"
            "    <error-severity>error</error-severity>\n"
            "    <error-message>%s</error-message>\n"
            "  </rpc-error>\n"
            "</rpc-reply>\n",
            tag ? tag : "operation-failed",
            msg ? msg : "unknown error");
    }
    return resp;
}

static char *error_reply(const char *msg)
{
    return error_reply_tag("operation-failed", msg);
}

/* RPCs this server advertises no capability for. RFC 6241 requires
 * operation-not-supported instead of a fabricated success. */
static char *unsupported_reply(const char *operation)
{
    return error_reply_tag("operation-not-supported", operation);
}

/* ---- model-layer wired config view + edit ------------------------------- */

/*
 * Growable response builder.
 *
 * The previous implementation accumulated into a fixed 8 KiB file-scope
 * buffer with `g_nc_off += snprintf(buf + g_nc_off, sizeof - g_nc_off, ...)`.
 * Once the offset passed the capacity, snprintf reports the length it
 * *would* have written while `sizeof - g_nc_off` underflowed to a huge
 * size_t, so a store holding a few hundred interfaces wrote past the end
 * of the buffer. Being file-scope it was also shared across concurrent
 * sessions, so two overlapping get-configs interleaved into one buffer.
 *
 * A per-call heap builder removes both problems: the offset can never
 * exceed the allocation, and nothing is shared between sessions.
 */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
} nc_buf_t;

static void nc_buf_init(nc_buf_t *b, size_t cap)
{
    b->buf = malloc(cap);
    b->len = 0;
    b->cap = b->buf ? cap : 0;
    b->oom = (b->buf == NULL);
    if (b->buf) b->buf[0] = '\0';
}

static bool nc_buf_reserve(nc_buf_t *b, size_t extra)
{
    if (b->oom) return false;
    if (b->cap - b->len > extra) return true;   /* room for NUL included */

    size_t want = b->len + extra + 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < want) {
        if (cap > (size_t)-1 / 2) { b->oom = true; return false; }
        cap *= 2;
    }
    char *nb = realloc(b->buf, cap);
    if (!nb) { b->oom = true; return false; }
    b->buf = nb;
    b->cap = cap;
    return true;
}

static void nc_buf_addf(nc_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void nc_buf_addf(nc_buf_t *b, const char *fmt, ...)
{
    if (b->oom) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = true; return; }

    if (!nc_buf_reserve(b, (size_t)n)) return;

    va_start(ap, fmt);
    vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* XML-escape the five predefined entities so interface names cannot
 * inject markup into the reply (RFC 6241 well-formedness). */
static void nc_buf_add_escaped(nc_buf_t *b, const char *s)
{
    for (const char *p = s; *p && !b->oom; p++) {
        switch (*p) {
        case '&':  nc_buf_addf(b, "&amp;");  break;
        case '<':  nc_buf_addf(b, "&lt;");   break;
        case '>':  nc_buf_addf(b, "&gt;");   break;
        case '"':  nc_buf_addf(b, "&quot;"); break;
        case '\'': nc_buf_addf(b, "&apos;"); break;
        default: {
            char one[2] = { *p, '\0' };
            nc_buf_addf(b, "%s", one);
        }
        }
    }
}

static void nc_iface_xml_iter(danos_object_entry_t *e, void *user)
{
    nc_buf_t *b = user;
    if (b->oom) return;
    if (e->type != DANOS_OBJ_IFACE || e->data_size < sizeof(danos_iface_t))
        return;
    const danos_iface_t *i = e->data;

    nc_buf_addf(b,
        "      <interface>\n"
        "        <name>");
    nc_buf_add_escaped(b, i->name);
    nc_buf_addf(b,
        "</name>\n"
        "        <mtu>%u</mtu>\n"
        "        <enabled>%s</enabled>\n"
        "      </interface>\n",
        i->mtu, i->admin_up ? "true" : "false");
}

static char *get_config_reply(void)
{
    nc_buf_t b;
    nc_buf_init(&b, 4096);

    nc_buf_addf(&b,
        "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
        "  <data>\n"
        "    <interfaces xmlns=\"urn:danos:yang:danos-iface\">\n");
    danos_object_iterate(g_default_store, nc_iface_xml_iter, &b);
    nc_buf_addf(&b,
        "    </interfaces>\n"
        "  </data>\n"
        "</rpc-reply>\n");

    if (b.oom) { free(b.buf); return error_reply("response too large"); }
    return b.buf;   /* ownership transfers to the caller */
}

/* Extract the first <tag>...</tag> that ends before `limit`. */
static void extract_text_n(const char *xml, const char *limit, const char *tag,
                           char *out, size_t cap)
{
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *p = strstr(xml, open);
    if (!p || (limit && p >= limit)) return;
    p += strlen(open);
    const char *e = strstr(p, close);
    if (!e || (limit && e > limit)) return;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

struct nc_lookup { const char *want; danos_iface_t *out; bool found; };

static void nc_lookup_iface_iter(danos_object_entry_t *e, void *user)
{
    struct nc_lookup *lu = user;
    if (lu->found) return;
    if (e->type != DANOS_OBJ_IFACE || e->data_size < sizeof(danos_iface_t))
        return;
    const danos_iface_t *i = e->data;
    if (strcmp(i->name, lu->want) == 0) {
        memcpy(lu->out, i, sizeof(*i));
        lu->found = true;
    }
}

#define NETCONF_MAX_EDIT_BLOCKS 1024

/*
 * Apply every <interface> block in the edit-config as one transaction.
 * The earlier implementation called strstr once, so a client that changed
 * several interfaces silently had all but the first discarded. Validation
 * happens before the transaction starts; any failure leaves the store
 * untouched.
 */
static char *edit_config_apply(const char *xml)
{
    const char *p = strstr(xml, "<config");
    if (!p) p = xml;

    danos_iface_t *targets = NULL;
    size_t count = 0, cap = 0;

    const char *cur = p;
    while ((cur = strstr(cur, "<interface")) != NULL) {
        /* "<interface" is 10 bytes; reject the <interfaces> container. */
        char next = cur[10];
        if (next != '>' && next != '/' && next != ' ' &&
            next != '\t' && next != '\n' && next != '\r') {
            cur += 10;
            continue;
        }
        if (next == '/') {      /* <interface/> carries no name */
            free(targets);
            return error_reply("interface name missing");
        }
        const char *body = strchr(cur, '>');
        if (!body) break;
        body++;
        const char *end = strstr(body, "</interface>");
        if (!end) {
            free(targets);
            return error_reply("unterminated interface element");
        }

        char name[64] = {0}, mtu[32] = {0}, enabled[16] = {0};
        extract_text_n(body, end, "name", name, sizeof(name));
        extract_text_n(body, end, "mtu", mtu, sizeof(mtu));
        extract_text_n(body, end, "enabled", enabled, sizeof(enabled));
        if (!name[0]) {
            free(targets);
            return error_reply("interface name missing");
        }
        if (count == NETCONF_MAX_EDIT_BLOCKS) {
            free(targets);
            return error_reply("too many interfaces in one edit-config");
        }
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            danos_iface_t *nt = realloc(targets, ncap * sizeof(*nt));
            if (!nt) {
                free(targets);
                return error_reply("out of memory");
            }
            targets = nt;
            cap = ncap;
        }

        danos_iface_t *target = &targets[count];
        struct nc_lookup lu = { .want = name, .out = target, .found = false };
        danos_object_iterate(g_default_store, nc_lookup_iface_iter, &lu);
        if (!lu.found) {
            free(targets);
            return error_reply("interface not found");
        }

        if (mtu[0]) {
            gnmi_typed_value_t v = { .kind = GNMI_VAL_JSON_IETF };
            snprintf(v.s, sizeof(v.s), "%s", mtu);
            danos_status_t st = gnmi_model_apply_leaf(DANOS_OBJ_IFACE,
                                                      GNMI_FIELD_MTU,
                                                      target, sizeof(*target), &v);
            if (st != DANOS_OK) {
                free(targets);
                return error_reply(danos_status_str(st));
            }
        }
        if (enabled[0]) {
            gnmi_typed_value_t v = { .kind = GNMI_VAL_JSON_IETF };
            snprintf(v.s, sizeof(v.s), "%s", enabled);
            danos_status_t st = gnmi_model_apply_leaf(DANOS_OBJ_IFACE,
                                                      GNMI_FIELD_ENABLED,
                                                      target, sizeof(*target), &v);
            if (st != DANOS_OK) {
                free(targets);
                return error_reply(danos_status_str(st));
            }
        }
        count++;
        cur = end + strlen("</interface>");
    }

    if (count == 0) {
        free(targets);
        return ok_reply();
    }

    danos_tx_t tx = {0};
    danos_status_t st = danos_tx_begin(&tx, "netconf-edit-config", NULL);
    for (size_t i = 0; st == DANOS_OK && i < count; i++)
        st = danos_iface_update(&tx, &targets[i]);
    if (st == DANOS_OK) st = danos_tx_commit_atomic(&tx);
    if (st != DANOS_OK && tx._internal) (void)danos_tx_abort(&tx);
    free(targets);
    if (st != DANOS_OK) return error_reply(danos_status_str(st));
    return ok_reply();
}

/* edit-config mutates state, everything else is read-only. The role comes
 * from the transport when one established an identity (SSH), otherwise from
 * the deployment default. */
static bool netconf_authorized(const netconf_ctx_t *ctx, netconf_rpc_type_t t)
{
    bool mutating = (t == NETCONF_RPC_EDIT_CONFIG);
    danos_sec_role_t role = ctx->role ? ctx->role
                                      : danos_authz_role_for_peer(NULL);
    const char *identity = ctx->identity[0] ? ctx->identity
                                            : danos_rbac_role_name(role);
    return danos_authz_check_identity(role, DANOS_SEC_OBJ_ALL,
                                      mutating ? DANOS_SEC_OP_UPDATE
                                               : DANOS_SEC_OP_READ,
                                      "netconf", identity);
}

/* NETCONF carries its credential in the <hello> message, not a per-request
 * header, so a session authenticates once at login. A session whose
 * transport already authenticated a user (the SSH subsystem) is past that
 * step; otherwise the <hello> bearer token is checked. */
static bool netconf_authenticate(netconf_ctx_t *ctx)
{
    if (ctx->identity[0]) return true;       /* authenticated by the transport */
    if (!danos_authz_configured()) return true;
    if (ctx->session_id != 0) return true;   /* already logged in */
    long uid = -1;
    return danos_authz_authenticate(ctx->authorization, uid, -1, NULL)
           == DANOS_AUTH_OK;
}

char *netconf_handle_rpc(netconf_ctx_t *ctx, const char *xml)
{
    if (!xml) return error_reply("null input");

    netconf_rpc_type_t rpc_type = netconf_parse_rpc(xml);

    /* Capture the credential from <hello> and remember the session. */
    if (rpc_type == NETCONF_RPC_HELLO && ctx->authorization == NULL) {
        const char *m = strstr(xml, "authorization");
        if (m) {
            const char *open = strchr(m, '>');
            const char *close = open ? strchr(open, '<') : NULL;
            if (open && close && close > open + 1) {
                size_t n = (size_t)(close - open - 1);
                char *tok = malloc(n + 1);
                if (tok) {
                    memcpy(tok, open + 1, n);
                    tok[n] = '\0';
                    /* store as an RFC 6750 credential so one code path
                     * serves both NETCONF and gRPC */
                    size_t pre = strlen("Bearer ");
                    char *hdr = malloc(pre + n + 1);
                    if (hdr) {
                        memcpy(hdr, "Bearer ", pre);
                        memcpy(hdr + pre, tok, n + 1);
                        free(tok);
                        ctx->authorization = hdr;
                    } else {
                        free(tok);
                    }
                }
            }
        }
    }

    if (!netconf_authenticate(ctx)) {
        ctx->error_count++;
        return strdup(
            "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
            "  <rpc-error>\n"
            "    <error-type>protocol</error-type>\n"
            "    <error-tag>access-denied</error-tag>\n"
            "    <error-severity>error</error-severity>\n"
            "    <error-message>authentication required</error-message>\n"
            "  </rpc-error>\n"
            "</rpc-reply>\n");
    }

    if (!netconf_authorized(ctx, rpc_type)) {
        ctx->error_count++;
        /* RFC 6241 access-denied, reported as rpc-error. */
        return strdup(
            "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
            "  <rpc-error>\n"
            "    <error-type>protocol</error-type>\n"
            "    <error-tag>access-denied</error-tag>\n"
            "    <error-severity>error</error-severity>\n"
            "    <error-message>authorization denied</error-message>\n"
            "  </rpc-error>\n"
            "</rpc-reply>\n");
    }
    ctx->rpc_count++;

    switch (rpc_type) {
    case NETCONF_RPC_GET_CONFIG:
        return get_config_reply();

    case NETCONF_RPC_EDIT_CONFIG:
        return edit_config_apply(xml);

    case NETCONF_RPC_GET:
        return get_config_reply();

    case NETCONF_RPC_COMMIT:
        ctx->error_count++;
        return unsupported_reply("commit");

    case NETCONF_RPC_DISCARD:
        ctx->error_count++;
        return unsupported_reply("discard-changes");

    case NETCONF_RPC_LOCK:
        ctx->error_count++;
        return unsupported_reply("lock");

    case NETCONF_RPC_UNLOCK:
        ctx->error_count++;
        return unsupported_reply("unlock");

    case NETCONF_RPC_KILL_SESSION:
        ctx->error_count++;
        return unsupported_reply("kill-session");

    case NETCONF_RPC_CLOSE_SESSION:
        return ok_reply();

    case NETCONF_RPC_UNKNOWN:
    default:
        ctx->error_count++;
        return error_reply("unknown RPC");
    }
}

void netconf_get_stats(netconf_ctx_t *ctx, uint64_t *rpc_count,
                       uint64_t *error_count)
{
    if (rpc_count)   *rpc_count   = ctx->rpc_count;
    if (error_count) *error_count = ctx->error_count;
}
