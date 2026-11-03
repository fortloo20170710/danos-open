/*
 * DANOS-Open Management: NETCONF peer-to-RBAC role policy
 *
 * The NETCONF daemon must not take the authorization role from the client.
 * The SSH subsystem sends the deployment-requested role as audit context, but
 * the daemon derives the effective role from the Unix-socket peer uid.
 *
 * Default policy: root is ADMIN, every other uid is VIEWER.  A deployment can
 * override this with a file (DANOS_NETCONF_ROLE_MAP, default
 * /etc/danos/netconf-roles) containing one "<user-or-uid> <role>" or
 * "@<group> <role>" mapping per line.  Unknown users remain VIEWER and unknown
 * role names are ignored.
 */

#ifndef DANOS_NETCONF_ROLE_H__
#define DANOS_NETCONF_ROLE_H__

#include <danos/security/rbac.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve the effective role for an authenticated local peer.  `user` is the
 * NSS name for `uid`; it may be NULL for a numeric-uid mapping. */
danos_sec_role_t netconf_role_resolve(const char *user, uid_t uid);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_NETCONF_ROLE_H__ */
