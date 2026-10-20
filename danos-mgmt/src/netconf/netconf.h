/*
 * DANOS-Open Management: NETCONF Server Skeleton (E3)
 *
 * Implements NETCONF protocol message parsing and dispatch.
 * NETCONF runs over SSH (RFC 6242), but this skeleton implements
 * the protocol layer (XML message parsing, rpc dispatch, get-config,
 * edit-config) without requiring libssh.
 */

#ifndef DANOS_NETCONF_H__
#define DANOS_NETCONF_H__

#include <stdint.h>
#include <stdbool.h>
#include <danos/security/rbac.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NETCONF context */
typedef struct {
    bool running;
    uint16_t port;          /* SSH port (default 830) */
    uint64_t rpc_count;
    uint64_t error_count;
    /* Credential presented in <hello>; NETCONF authenticates per session
     * rather than per request. */
    char    *authorization;
    uint64_t session_id;    /* non-zero once logged in */
    /* Identity established by the transport (the SSH user) and the role the
     * deployment maps that identity to. The SSH server authenticates the
     * user; this context carries the result into RBAC and the audit log. */
    danos_sec_role_t role;
    char identity[64];
} netconf_ctx_t;

/* NETCONF RPC types */
typedef enum {
    NETCONF_RPC_GET_CONFIG    = 1,
    NETCONF_RPC_EDIT_CONFIG   = 2,
    NETCONF_RPC_GET           = 3,
    NETCONF_RPC_GET_OPERATIONS= 4,
    NETCONF_RPC_CLOSE_SESSION = 5,
    NETCONF_RPC_KILL_SESSION  = 6,
    NETCONF_RPC_COMMIT        = 7,
    NETCONF_RPC_DISCARD       = 8,
    NETCONF_RPC_LOCK          = 9,
    NETCONF_RPC_UNLOCK        = 10,
    NETCONF_RPC_HELLO         = 11,
    NETCONF_RPC_COPY_CONFIG   = 12,
    NETCONF_RPC_DELETE_CONFIG = 13,
    NETCONF_RPC_VALIDATE      = 14,
    NETCONF_RPC_UNKNOWN       = 0,
} netconf_rpc_type_t;

/* Initialize NETCONF server */
void netconf_init(netconf_ctx_t *ctx, uint16_t port);

/* Release per-session resources (the credential captured at login). */
void netconf_fini(netconf_ctx_t *ctx);

/* Parse a NETCONF RPC XML message and identify the RPC type.
 * Returns RPC type, or NETCONF_RPC_UNKNOWN on parse failure. */
netconf_rpc_type_t netconf_parse_rpc(const char *xml);

/* Handle a NETCONF RPC.
 * Returns XML response string (caller must free).
 * Returns NULL on error. */
char *netconf_handle_rpc(netconf_ctx_t *ctx, const char *xml);

/* Generate NETCONF <hello> message (capabilities advertisement) for a
 * session. session_id must be in 1..2^31-1. */
char *netconf_hello_message(uint64_t session_id);

/* Allocate a session identifier in the RFC 6241 range. */
uint64_t netconf_new_session_id(void);

/* Get statistics */
void netconf_get_stats(netconf_ctx_t *ctx, uint64_t *rpc_count,
                       uint64_t *error_count);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_NETCONF_H__ */
