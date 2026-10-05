#ifndef DANOS_GNMI_TLS_H
#define DANOS_GNMI_TLS_H
#include <openssl/ssl.h>
#include <danos/security/rbac.h>

typedef struct danos_gnmi_tls danos_gnmi_tls_t;
/* Immutable configuration; caller frees after all sessions have stopped.
 * roles: one SHA256 hex fingerprint + role (admin/operator/viewer) per line.
 * No default identity/role; unknown certificates fail closed. */
danos_gnmi_tls_t *danos_gnmi_tls_new(const char *cert, const char *key,
                                   const char *ca, const char *roles);
void danos_gnmi_tls_free(danos_gnmi_tls_t *tls);
SSL *danos_gnmi_tls_accept(danos_gnmi_tls_t *tls, int fd,
                           danos_sec_role_t *role);
#endif
