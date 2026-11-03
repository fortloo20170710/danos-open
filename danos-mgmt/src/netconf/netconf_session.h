/*
 * DANOS-Open Management: NETCONF session engine
 *
 * A transport-independent NETCONF session: server <hello>, client <hello>,
 * RFC 6242 framing and RPC dispatch over a pair of file descriptors.  The
 * per-session SSH subsystem uses stdin/stdout; the shared NETCONF daemon uses
 * an accepted Unix-socket descriptor.  Keeping the engine here means the
 * single-daemon deployment and the legacy per-session deployment run exactly
 * the same protocol code.
 */

#ifndef DANOS_NETCONF_SESSION_H__
#define DANOS_NETCONF_SESSION_H__

#include "netconf.h"

#include <danos/security/rbac.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETCONF_SESSION_IDLE_TIMEOUT_DEFAULT 300u

typedef struct netconf_session netconf_session_t;

/* Post-hello idle timeout in seconds; 0 disables it.  Set before accepting
 * sessions.  The default is NETCONF_SESSION_IDLE_TIMEOUT_DEFAULT (300 s). */
void netconf_session_set_idle_timeout(unsigned seconds);
unsigned netconf_session_idle_timeout(void);

/* Establish a session, send the server <hello> and read the client <hello>.
 * `identity` is the transport-authenticated user (may be empty); `role` is
 * the mapped RBAC role (0 selects the deployment default).  Returns NULL on
 * an I/O or protocol failure after the peer connected. */
netconf_session_t *netconf_session_new(int rfd, int wfd,
                                       const char *identity,
                                       danos_sec_role_t role);

/* Session id advertised in the server hello. */
uint64_t netconf_session_id(const netconf_session_t *s);

/* Dispatch RPCs until close-session, EOF, an authenticated idle timeout or a
 * framing error.  Returns 0 for a clean end, 1 when the idle timeout fired,
 * -1 on failure. */
int netconf_session_run(netconf_session_t *s);

/* True after netconf_session_run returned because of the idle timeout. */
bool netconf_session_timed_out(const netconf_session_t *s);

/* True after the peer sent <close-session/>. */
bool netconf_session_close_requested(const netconf_session_t *s);

/* Release all session resources.  Does not release datastore locks; callers
 * that own a transport disconnect must call netconf_ds_release_session(). */
void netconf_session_free(netconf_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_NETCONF_SESSION_H__ */
