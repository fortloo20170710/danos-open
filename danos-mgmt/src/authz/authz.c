#include "authz.h"
#include <danos/security/audit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static bool g_ready;

/* ---- credential sources -------------------------------------------------- */

static char    *g_token;          /* bearer token, NUL-terminated */
static size_t   g_token_len;
static bool     g_peercred_enabled;

int danos_authz_configure(const char *token_file, bool allow_peercred)
{
    free(g_token);
    g_token = NULL;
    g_token_len = 0;
    g_peercred_enabled = allow_peercred;

    if (!token_file || !*token_file) return 0;   /* tokens disabled */

    /* A token any local user can read is not a credential. Refuse rather
     * than warn: serving with a readable token is worse than not serving. */
    struct stat st;
    if (stat(token_file, &st) != 0) {
        fprintf(stderr, "authz: token file %s: %s\n", token_file,
                strerror(errno));
        return -1;
    }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        fprintf(stderr,
                "authz: token file %s is group/other accessible (mode %04o); "
                "refusing\n", token_file, (unsigned)(st.st_mode & 07777));
        return -1;
    }

    FILE *f = fopen(token_file, "r");
    if (!f) {
        fprintf(stderr, "authz: cannot open token file %s: %s\n", token_file,
                strerror(errno));
        return -1;
    }
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strcspn(line, "\r\n");
    line[n] = '\0';
    if (n == 0) {
        fprintf(stderr, "authz: token file %s is empty\n", token_file);
        return -1;
    }
    g_token = strdup(line);
    if (!g_token) return -1;
    g_token_len = n;
    return 0;
}

bool danos_authz_configured(void)
{
    return g_token != NULL || g_peercred_enabled;
}

/* Length-independent, content-constant-time compare. */
static bool token_equal(const char *a, size_t alen, const char *b, size_t blen)
{
    /* Compare lengths without an early return, then bytes. */
    unsigned diff = (unsigned)(alen ^ blen);
    size_t n = alen > blen ? alen : blen;
    for (size_t i = 0; i < n; i++) {
        unsigned char x = i < alen ? (unsigned char)a[i] : 0;
        unsigned char y = i < blen  ? (unsigned char)b[i] : 0;
        diff |= (unsigned)(x ^ y);
    }
    return diff == 0;
}

danos_auth_result_t danos_authz_authenticate(const char *authorization,
                                             long peer_uid, long peer_pid,
                                             danos_sec_role_t *role_out)
{
    (void)peer_pid;

    if (authorization && *authorization) {
        /* RFC 6750: "Bearer <token>". Scheme match is case-insensitive. */
        const char *v = authorization;
        while (*v == ' ') v++;
        if (strncasecmp(v, "Bearer ", 7) != 0) return DANOS_AUTH_BAD;
        const char *tok = v + 7;
        while (*tok == ' ') tok++;
        if (!g_token) return DANOS_AUTH_BAD;      /* no token source: reject */
        if (!token_equal(tok, strlen(tok), g_token, g_token_len))
            return DANOS_AUTH_BAD;
        /* A token authenticates the client but carries no role of its own,
         * so it gets the configured default - which may be lowered. */
        if (role_out) *role_out = danos_authz_default_role();
        return DANOS_AUTH_OK;
    }

    if (g_peercred_enabled && peer_uid >= 0) {
        if (role_out) *role_out = (peer_uid == 0)
            ? DANOS_ROLE_ADMIN
            : danos_authz_default_role();
        return DANOS_AUTH_OK;
    }

    return g_token || g_peercred_enabled ? DANOS_AUTH_NONE
                                         : DANOS_AUTH_DISABLED;
}

int danos_authz_init(const char *audit_path)
{
    if (g_ready) return 0;
    if (danos_rbac_init() != 0) return -1;
    /* A failed audit init is not fatal to serving, but it must be visible:
     * silently dropping records would make the log worthless while still
     * looking healthy. */
    if (danos_audit_init(audit_path) != 0) {
        fprintf(stderr,
                "authz: audit log unavailable (%s); authorization enforced "
                "without an audit trail\n",
                audit_path ? audit_path : "default");
    }
    g_ready = true;
    return 0;
}

bool danos_authz_ready(void)
{
    return g_ready;
}

void danos_authz_shutdown(void)
{
    if (!g_ready) return;
    danos_audit_close();
    g_ready = false;
}

danos_sec_role_t danos_authz_default_role(void)
{
    /* Default for laboratory/bearer/local paths only. Certificate identities
     * have explicit roles and never fall back to this setting. */
    const char *env = getenv("DANOS_AUTHZ_DEFAULT_ROLE");
    if (env && *env) {
        danos_sec_role_t r;
        if (danos_rbac_role_by_name(env, &r) == 0) return r;
        fprintf(stderr, "authz: unknown DANOS_AUTHZ_DEFAULT_ROLE '%s'\n", env);
    }
    return DANOS_ROLE_ADMIN;
}

danos_sec_role_t danos_authz_role_for_peer(const char *peer_addr)
{
    (void)peer_addr;   /* no authenticated identity to inspect yet */
    return danos_authz_default_role();
}

const char *danos_authz_obj_name(danos_sec_obj_type_t obj)
{
    switch (obj) {
    case DANOS_SEC_OBJ_INTERFACE: return "interface";
    case DANOS_SEC_OBJ_VRF:       return "vrf";
    case DANOS_SEC_OBJ_ROUTE:     return "route";
    case DANOS_SEC_OBJ_ACL:       return "acl";
    case DANOS_SEC_OBJ_QOS:       return "qos";
    case DANOS_SEC_OBJ_BFD:       return "bfd";
    case DANOS_SEC_OBJ_SECURITY:  return "security";
    case DANOS_SEC_OBJ_ALL:       return "all";
    default:                      return "unknown";
    }
}

static void audit_fill(danos_audit_entry_t *e, uint64_t tx_id,
                       danos_audit_event_t ev, const char *initiator,
                       danos_sec_obj_type_t obj, const char *obj_id,
                       const char *diff)
{
    memset(e, 0, sizeof(*e));
    e->tx_id = tx_id;
    e->event = ev;
    snprintf(e->initiator, sizeof(e->initiator), "%s",
             initiator ? initiator : "unknown");
    snprintf(e->obj_type, sizeof(e->obj_type), "%s", danos_authz_obj_name(obj));
    snprintf(e->obj_id, sizeof(e->obj_id), "%s", obj_id ? obj_id : "");
    if (diff) snprintf(e->diff, sizeof(e->diff), "%s", diff);
    clock_gettime(CLOCK_REALTIME, &e->timestamp);
}

bool danos_authz_check(danos_sec_role_t role, danos_sec_obj_type_t obj,
                       danos_sec_op_t op, const char *detail)
{
    bool allowed = danos_rbac_check(role, obj, op);
    if (!g_ready) return allowed;   /* not initialised: nothing to log to */
    danos_audit_entry_t e;
    audit_fill(&e, 0, allowed ? DANOS_AUDIT_PERMIT : DANOS_AUDIT_DENY,
               danos_rbac_role_name(role), obj, "", detail);
    (void)danos_audit_log(&e);
    return allowed;
}

void danos_authz_audit_tx(uint64_t tx_id, danos_audit_event_t event,
                          const char *initiator, danos_sec_obj_type_t obj,
                          const char *obj_id, const char *diff)
{
    if (!g_ready) return;
    danos_audit_entry_t e;
    audit_fill(&e, tx_id, event, initiator, obj, obj_id, diff);
    (void)danos_audit_log(&e);
}

void danos_authz_audit_auth(const char *peer, bool allowed, const char *detail)
{
    if (!g_ready) return;
    danos_audit_entry_t e;
    audit_fill(&e, 0, allowed ? DANOS_AUDIT_AUTH_OK : DANOS_AUDIT_AUTH_FAIL,
               peer ? peer : "unknown", DANOS_SEC_OBJ_ALL, "", detail);
    if (peer) snprintf(e.source_ip, sizeof(e.source_ip), "%s", peer);
    (void)danos_audit_log(&e);
}
