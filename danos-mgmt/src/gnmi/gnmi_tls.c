#include "gnmi_tls.h"
#include "../authz/authz.h"
#include <openssl/err.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_IDENTITIES 256
#define MAX_ROLE_MAP_BYTES (64 * 1024)

struct tls_identity {
    unsigned char digest[32];
    danos_sec_role_t role;
};

struct danos_gnmi_tls {
    SSL_CTX *ctx;
    size_t count;
    struct tls_identity identities[MAX_IDENTITIES];
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

/* Parse one role map from memory. Lines are consumed in the same 255-byte
 * windows that fgets(3) with a 256-byte buffer produced, so the accepted
 * language does not change. Returns the identity count, or 0 when the map is
 * empty, malformed, names an unknown role, or repeats a fingerprint. */
static size_t parse_role_map(const char *buf, size_t len,
                             struct tls_identity *out, size_t max)
{
    size_t count = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t window_end = pos + 255 < len ? pos + 255 : len;
        size_t end = pos;
        while (end < window_end && buf[end] != '\n') end++;
        bool newline = end < window_end && buf[end] == '\n';
        size_t line_len = end - pos;
        if (line_len && buf[pos + line_len - 1] == '\r') line_len--;

        char line[256];
        memcpy(line, buf + pos, line_len);
        line[line_len] = '\0';
        char *p = line + strspn(line, " \t\r\n");
        if (*p && *p != '#') {
            char hex[65], role[32], extra;
            if (count == max ||
                sscanf(p, "%64s %31s %c", hex, role, &extra) != 2 ||
                strlen(hex) != 64) return 0;
            if (strcmp(role, "admin") && strcmp(role, "operator") &&
                strcmp(role, "viewer")) return 0;
            danos_sec_role_t r;
            if (danos_rbac_role_by_name(role, &r)) return 0;

            struct tls_identity id;
            memset(&id, 0, sizeof(id));
            for (size_t i = 0; i < 32; i++) {
                int a = hex_digit(hex[2 * i]), b = hex_digit(hex[2 * i + 1]);
                if (a < 0 || b < 0) return 0;
                id.digest[i] = (unsigned char)((a << 4) | b);
            }
            for (size_t i = 0; i < count; i++)
                if (!memcmp(out[i].digest, id.digest, 32)) return 0;
            id.role = r;
            out[count++] = id;
        }
        pos = newline ? end + 1 : end;
        if (!newline && end == len) break;
    }
    return count;
}

int danos_gnmi_tls_role_map_valid(const char *text, size_t len)
{
    struct tls_identity ids[MAX_IDENTITIES];
    return parse_role_map(text, len, ids, MAX_IDENTITIES) > 0;
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
    char *text = malloc(MAX_ROLE_MAP_BYTES + 1);
    if (!text) { fclose(f); goto fail; }
    size_t text_len = fread(text, 1, MAX_ROLE_MAP_BYTES + 1, f);
    bool overflow = ferror(f) || text_len > MAX_ROLE_MAP_BYTES;
    fclose(f);
    if (overflow) { free(text); goto fail; }
    t->count = parse_role_map(text, text_len, t->identities, MAX_IDENTITIES);
    free(text);
    if (!t->count) goto fail;
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

SSL *danos_gnmi_tls_accept(danos_gnmi_tls_t *t, int fd, danos_sec_role_t *role, char identity[65])
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
