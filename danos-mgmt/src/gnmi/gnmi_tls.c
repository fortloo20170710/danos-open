#include "gnmi_tls.h"
#include "../authz/authz.h"
#include <openssl/err.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_IDENTITIES 256
struct danos_gnmi_tls {
    SSL_CTX *ctx;
    size_t count;
    struct { unsigned char digest[32]; danos_sec_role_t role; } identities[MAX_IDENTITIES];
};

static int select_h2(SSL *ssl, const unsigned char **out, unsigned char *len,
                     const unsigned char *in, unsigned int n, void *arg)
{
    (void)ssl; (void)arg;
    static const unsigned char h2[] = {2, 'h', '2'};
    unsigned char *selected = NULL;
    if (SSL_select_next_proto(&selected, len, h2, sizeof(h2), in, n)
        != OPENSSL_NPN_NEGOTIATED) return SSL_TLSEXT_ERR_ALERT_FATAL;
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

danos_gnmi_tls_t *danos_gnmi_tls_new(const char *cert, const char *key,
                                   const char *ca, const char *roles)
{
    if (!cert || !key || !ca || !roles) return NULL;
    struct stat st;
    if (stat(key, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0077)) {
        fprintf(stderr, "gNMI TLS: private key must be a private regular file\n");
        return NULL;
    }
    if (stat(roles, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0022)) {
        fprintf(stderr, "gNMI TLS: role map must not be group/other writable\n");
        return NULL;
    }
    danos_gnmi_tls_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    FILE *f = fopen(roles, "r");
    if (!f) goto fail;
    char line[256], hex[65], role[32], extra;
    bool valid = true;
    while (fgets(line, sizeof(line), f)) {
        char *p = line + strspn(line, " \t\r\n");
        if (!*p || *p == '#') continue;
        if (t->count == MAX_IDENTITIES ||
            sscanf(p, "%64s %31s %c", hex, role, &extra) != 2 ||
            strlen(hex) != 64) { valid = false; break; }
        danos_sec_role_t r;
        if (strcmp(role, "admin") && strcmp(role, "operator") && strcmp(role, "viewer")) {
            valid = false; break;
        }
        if (danos_rbac_role_by_name(role, &r)) { valid = false; break; }
        for (size_t i = 0; i < 32; i++) {
            int a = hex_digit(hex[2*i]), b = hex_digit(hex[2*i+1]);
            if (a < 0 || b < 0) { valid = false; break; }
            t->identities[t->count].digest[i] = (unsigned char)((a << 4) | b);
        }
        if (!valid) break;
        for (size_t i = 0; i < t->count; i++) {
            if (!memcmp(t->identities[i].digest, t->identities[t->count].digest, 32))
                valid = false;
        }
        if (!valid) break;
        t->identities[t->count++].role = r;
    }
    if (ferror(f)) valid = false;
    fclose(f);
    if (!valid || !t->count) goto fail;
    t->ctx = SSL_CTX_new(TLS_server_method());
    if (!t->ctx || !SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_cipher_list(t->ctx,
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305") ||
        !SSL_CTX_use_certificate_chain_file(t->ctx, cert) ||
        !SSL_CTX_use_PrivateKey_file(t->ctx, key, SSL_FILETYPE_PEM) ||
        !SSL_CTX_check_private_key(t->ctx) ||
        !SSL_CTX_load_verify_locations(t->ctx, ca, NULL)) goto fail;
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify_depth(t->ctx, 4);
    SSL_CTX_set_options(t->ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_session_cache_mode(t->ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_alpn_select_cb(t->ctx, select_h2, NULL);
    return t;
fail:
    fprintf(stderr, "gNMI TLS: invalid certificate/key/CA/role configuration\n");
    danos_gnmi_tls_free(t);
    return NULL;
}

void danos_gnmi_tls_free(danos_gnmi_tls_t *t)
{
    if (!t) return;
    SSL_CTX_free(t->ctx);
    free(t);
}

SSL *danos_gnmi_tls_accept(danos_gnmi_tls_t *t, int fd, danos_sec_role_t *role)
{
    SSL *s = SSL_new(t->ctx);
    if (!s) return NULL;
    X509 *cert = NULL;
    if (!SSL_set_fd(s, fd) || SSL_accept(s) != 1 ||
        SSL_get_verify_result(s) != X509_V_OK) goto fail;
    const unsigned char *alpn;
    unsigned int alpn_len;
    SSL_get0_alpn_selected(s, &alpn, &alpn_len);
    if (alpn_len != 2 || memcmp(alpn, "h2", 2)) goto fail;
    cert = SSL_get1_peer_certificate(s);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int n;
    if (!cert || !X509_digest(cert, EVP_sha256(), digest, &n) || n != 32) goto fail;
    char identity[65];
    for (size_t i = 0; i < 32; i++) snprintf(identity + 2*i, 3, "%02x", digest[i]);
    for (size_t i = 0; i < t->count; i++) {
        if (!memcmp(digest, t->identities[i].digest, 32)) {
            *role = t->identities[i].role;
            danos_authz_audit_auth(identity, true, "mTLS certificate accepted");
            X509_free(cert);
            return s;
        }
    }
fail:
    danos_authz_audit_auth("mTLS", false, "certificate/identity/ALPN rejected");
    X509_free(cert);
    SSL_free(s);
    return NULL;
}
