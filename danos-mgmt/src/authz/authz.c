#include "authz.h"
#include <danos/security/audit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool g_ready;

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
    /* No identity source exists yet, so the role cannot be derived from a
     * credential. DANOS_AUTHZ_DEFAULT_ROLE lets an operator lock the daemon
     * down to a lesser role before TLS or tokens land. */
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