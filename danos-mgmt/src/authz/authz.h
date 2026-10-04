/*
 * Management-plane authorization and audit.
 *
 * This module is the single enforcement point for the northbound
 * interfaces. It exists because danos-security (RBAC, audit, CoPP) was built
 * and unit-tested but never linked into the daemon, so the repository
 * shipped a security module that provided no security.
 *
 * Identity is deliberately pluggable and currently unresolved: there is no
 * TLS, no token and no peer-credential check on any listener, so
 * authz_role_for_peer() cannot derive a role from a credential and returns
 * the configured default. Enforcement and audit are wired now so that adding
 * an identity source is a change to one function rather than to every
 * handler. Until such a source exists, the default role must be ADMIN for
 * behaviour to be unchanged - this is *not* authorization, and the audit log
 * is the only externally visible effect.
 *
 * What this does provide today:
 *   - every mutating RPC is authorized through one check, so the set of
 *     handlers that can be protected is enumerable
 *   - every transaction and every authorization decision is recorded in an
 *     append-only log
 *   - a denied decision is enforced, not merely logged
 */

#ifndef DANOS_AUTHZ_H__
#define DANOS_AUTHZ_H__

#include <danos/security/rbac.h>
#include <danos/security/audit.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise RBAC and the audit log.
 *
 * audit_path may be NULL for the default. If the audit log cannot be opened
 * the module stays uninitialised rather than silently dropping records;
 * authz_ready() then reports false and callers can decide whether to serve.
 * Returns 0 on success. */
int danos_authz_init(const char *audit_path);

/* True once rbac + audit are usable. */
bool danos_authz_ready(void);

/* Release resources (closes the audit log). */
void danos_authz_shutdown(void);

/* Role to apply to a peer.
 *
 * There is no authenticated identity yet, so this ignores the address and
 * returns default_role. It is the single place that must change once TLS,
 * bearer tokens or SO_PEERCRED are introduced. */
danos_sec_role_t danos_authz_role_for_peer(const char *peer_addr);

/* Role assumed for peers with no authenticated identity. Defaults to ADMIN
 * so existing behaviour is unchanged; set DANOS_AUTHZ_DEFAULT_ROLE in the
 * environment to "operator" or "viewer" to lock the daemon down without an
 * identity source. */
danos_sec_role_t danos_authz_default_role(void);

/* Check one operation. Returns true when allowed. A denial is recorded in
 * the audit log, so callers only need to handle the boolean. */
bool danos_authz_check(danos_sec_role_t role, danos_sec_obj_type_t obj,
                       danos_sec_op_t op, const char *detail);

/* Map a gRPC method path to the object it touches, for auditing. */
const char *danos_authz_obj_name(danos_sec_obj_type_t obj);

/* Record a transaction lifecycle event. tx_id 0 is tolerated. */
void danos_authz_audit_tx(uint64_t tx_id, danos_audit_event_t event,
                          const char *initiator, danos_sec_obj_type_t obj,
                          const char *obj_id, const char *diff);

/* Record an authorization decision explicitly (in addition to the decision
 * dano_authz_check already logs) so callers can add context. */
void danos_authz_audit_auth(const char *peer, bool allowed, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_AUTHZ_H__ */