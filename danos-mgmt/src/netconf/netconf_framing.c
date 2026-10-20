/*
 * DANOS-Open Management: NETCONF message framing (RFC 6242)
 */

#include "netconf_framing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETCONF_EOM "]]>]]>"
#define NETCONF_EOM_LEN 6

/* Server replies are framed in chunks of at most this size. */
#define NETCONF_ENCODE_CHUNK (1024u * 1024u)

enum nc_frame_state {
    NC_ST_EOM = 0,      /* base 1.0: scan for the end-of-message delimiter */
    NC_ST_CHUNK_LF,     /* base 1.1: LF at the start of a chunk */
    NC_ST_CHUNK_HASH,   /* '#' that introduces a chunk or end-of-chunks */
    NC_ST_CHUNK_KIND,   /* '#' (end-of-chunks) or the first size digit */
    NC_ST_CHUNK_SIZE,   /* remaining size digits, then LF */
    NC_ST_CHUNK_DATA,
    NC_ST_END_LF        /* LF after the end-of-chunks '#' */
};

struct netconf_frame_decoder {
    bool base11;
    enum nc_frame_state state;

    char  *msg;
    size_t len;
    size_t cap;

    size_t scan_pos;    /* base 1.0 delimiter scan offset */
    size_t size_val;    /* base 1.1 chunk size being parsed */
    size_t chunk_need;
    size_t chunk_have;

    bool failed;
};

static void decoder_reset(struct netconf_frame_decoder *d)
{
    d->state = d->base11 ? NC_ST_CHUNK_LF : NC_ST_EOM;
    d->len = 0;
    d->scan_pos = 0;
    d->size_val = 0;
    d->chunk_need = 0;
    d->chunk_have = 0;
}

static bool reserve(struct netconf_frame_decoder *d, size_t extra)
{
    if (d->cap - d->len >= extra) return true;

    size_t want = d->len + extra;
    size_t cap = d->cap ? d->cap : 1024;
    while (cap < want) {
        if (cap > (size_t)-1 / 2) return false;
        cap *= 2;
    }
    char *nb = realloc(d->msg, cap);
    if (!nb) return false;
    d->msg = nb;
    d->cap = cap;
    return true;
}

static bool append(struct netconf_frame_decoder *d, const char *p, size_t n)
{
    if (n == 0) return true;
    if (n > NETCONF_MAX_MESSAGE_BYTES || d->len > NETCONF_MAX_MESSAGE_BYTES - n)
        return false;
    if (!reserve(d, n + 1)) return false;
    memcpy(d->msg + d->len, p, n);
    d->len += n;
    return true;
}

static int emit(const char *msg, size_t len, netconf_frame_cb_t cb, void *user)
{
    if (!cb) return 0;
    return cb(msg, len, user) == 0 ? 0 : -1;
}

static int end_message(struct netconf_frame_decoder *d, netconf_frame_cb_t cb,
                       void *user)
{
    if (!reserve(d, 1)) return -1;
    d->msg[d->len] = '\0';
    int rc = emit(d->msg, d->len, cb, user);
    d->len = 0;
    d->scan_pos = 0;
    return rc;
}

/* Base 1.0: retain bytes until "]]>]]>" appears. The delimiter is removed
 * from the retained buffer, and any bytes that followed it stay queued for
 * the next message. */
static int feed_eom(struct netconf_frame_decoder *d, const char *data,
                    size_t n, netconf_frame_cb_t cb, void *user)
{
    if (!append(d, data, n)) return -1;

    while (d->scan_pos + NETCONF_EOM_LEN <= d->len) {
        const char *p = d->msg + d->scan_pos;
        size_t avail = d->len - d->scan_pos - NETCONF_EOM_LEN + 1;
        const char *hit = memchr(p, ']', avail);
        size_t at;

        if (!hit) {
            d->scan_pos = d->len >= NETCONF_EOM_LEN - 1
                          ? d->len - (NETCONF_EOM_LEN - 1) : 0;
            return 0;
        }
        at = (size_t)(hit - d->msg);
        if (d->len - at >= NETCONF_EOM_LEN &&
            memcmp(d->msg + at, NETCONF_EOM, NETCONF_EOM_LEN) == 0) {
            size_t rest = d->len - (at + NETCONF_EOM_LEN);
            if (!reserve(d, 1)) return -1;
            d->msg[at] = '\0';
            if (emit(d->msg, at, cb, user) != 0) return -1;
            if (rest) memmove(d->msg, d->msg + at + NETCONF_EOM_LEN, rest);
            d->len = rest;
            d->scan_pos = 0;
        } else {
            d->scan_pos = at + 1;
        }
    }
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Base 1.1: chunked framing. */
static int feed_chunked(struct netconf_frame_decoder *d, char c,
                        netconf_frame_cb_t cb, void *user)
{
    switch (d->state) {
    case NC_ST_CHUNK_LF:
        if (c != '\n') return -1;
        d->state = NC_ST_CHUNK_HASH;
        return 0;

    case NC_ST_CHUNK_HASH:
        if (c != '#') return -1;
        d->state = NC_ST_CHUNK_KIND;
        return 0;

    case NC_ST_CHUNK_KIND:
        if (c == '#') {
            d->state = NC_ST_END_LF;
            return 0;
        } else {
            int v = hex_digit(c);
            if (v < 0) return -1;
            d->size_val = (size_t)v;
            d->state = NC_ST_CHUNK_SIZE;
            return 0;
        }

    case NC_ST_CHUNK_SIZE: {
        int v = hex_digit(c);
        if (v >= 0) {
            if (d->size_val > (NETCONF_MAX_CHUNK_BYTES - (size_t)v) / 16)
                return -1;
            d->size_val = d->size_val * 16 + (size_t)v;
            return 0;
        }
        if (c != '\n' || d->size_val == 0) return -1;
        if (d->size_val > NETCONF_MAX_CHUNK_BYTES) return -1;
        d->chunk_need = d->size_val;
        d->chunk_have = 0;
        d->state = NC_ST_CHUNK_DATA;
        return 0;
    }

    case NC_ST_CHUNK_DATA:
        if (!append(d, &c, 1)) return -1;
        if (++d->chunk_have == d->chunk_need) d->state = NC_ST_CHUNK_LF;
        return 0;

    case NC_ST_END_LF:
        if (c != '\n') return -1;
        if (end_message(d, cb, user) != 0) return -1;
        d->state = NC_ST_CHUNK_LF;
        return 0;

    case NC_ST_EOM:
    default:
        return -1;
    }
}

netconf_frame_decoder_t *netconf_frame_decoder_new(bool base11)
{
    netconf_frame_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->base11 = base11;
    decoder_reset(d);
    return d;
}

void netconf_frame_decoder_free(netconf_frame_decoder_t *dec)
{
    if (!dec) return;
    free(dec->msg);
    free(dec);
}

int netconf_frame_feed(netconf_frame_decoder_t *dec, const char *data,
                       size_t n, netconf_frame_cb_t cb, void *user)
{
    if (!dec || dec->failed) return -1;
    if (!data && n) return -1;

    int rc;
    if (!dec->base11) {
        rc = feed_eom(dec, data, n, cb, user);
    } else {
        rc = 0;
        for (size_t i = 0; i < n && rc == 0; i++)
            rc = feed_chunked(dec, data[i], cb, user);
    }
    if (rc != 0) dec->failed = true;
    return rc;
}

char *netconf_frame_encode(bool base11, const char *msg, size_t len,
                           size_t *out_len)
{
    if (!msg && len) return NULL;
    if (len > NETCONF_MAX_MESSAGE_BYTES) return NULL;

    size_t total;
    if (base11) {
        size_t chunks = len ? (len + NETCONF_ENCODE_CHUNK - 1) /
                              NETCONF_ENCODE_CHUNK : 0;
        total = 4;   /* "\n##\n" */
        if (chunks && total > (size_t)-1 - chunks * 32) return NULL;
        total += chunks * 32;   /* LF '#' digits LF, upper bound */
        if (total > (size_t)-1 - len) return NULL;
        total += len;
    } else {
        if (len > (size_t)-1 - NETCONF_EOM_LEN) return NULL;
        total = len + NETCONF_EOM_LEN;
    }

    char *out = malloc(total + 1);
    if (!out) return NULL;

    size_t off = 0;
    if (!base11) {
        memcpy(out, msg, len);
        memcpy(out + len, NETCONF_EOM, NETCONF_EOM_LEN);
        off = len + NETCONF_EOM_LEN;
    } else {
        size_t pos = 0;
        while (pos < len) {
            size_t take = len - pos;
            if (take > NETCONF_ENCODE_CHUNK) take = NETCONF_ENCODE_CHUNK;
            int n = snprintf(out + off, total + 1 - off, "\n#%zx\n", take);
            if (n < 0 || (size_t)n >= total + 1 - off) { free(out); return NULL; }
            off += (size_t)n;
            memcpy(out + off, msg + pos, take);
            off += take;
            pos += take;
        }
        memcpy(out + off, "\n##\n", 4);
        off += 4;
    }
    out[off] = '\0';
    if (out_len) *out_len = off;
    return out;
}

bool netconf_hello_advertises_base11(const char *hello)
{
    if (!hello) return false;
    /* Both the capability URN and the hello namespace moved, so look for the
     * base:1.1 URN only. The 1.0 URN is a prefix of nothing, so a plain
     * substring test cannot confuse the two. */
    return strstr(hello, "urn:ietf:params:netconf:base:1.1") != NULL;
}
