/*
 * DANOS-Open VPP Backend: Binary API Wire Codec (v0.3)
 */

#include "vpp_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>

/* =========================================================================
 * Growable buffer
 * ========================================================================= */

void vpp_buf_init(vpp_buf_t *b, uint32_t cap)
{
    if (cap == 0) cap = 128;
    b->data = malloc(cap);
    b->cap = b->data ? cap : 0;
    b->len = 0;
}

void vpp_buf_free(vpp_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void vpp_buf_reset(vpp_buf_t *b)
{
    b->len = 0;
}

static bool buf_reserve(vpp_buf_t *b, uint32_t extra)
{
    if (b->len + extra <= b->cap) return true;
    uint32_t ncap = b->cap ? b->cap : 128;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *nd = realloc(b->data, ncap);
    if (!nd) return false;
    b->data = nd;
    b->cap = ncap;
    return true;
}

void vpp_buf_put_u8(vpp_buf_t *b, uint8_t v)
{
    if (buf_reserve(b, 1)) b->data[b->len++] = v;
}

void vpp_buf_put_u16(vpp_buf_t *b, uint16_t v)
{
    if (buf_reserve(b, 2)) {
        b->data[b->len++] = (uint8_t)(v >> 8);
        b->data[b->len++] = (uint8_t)v;
    }
}

void vpp_buf_put_u32(vpp_buf_t *b, uint32_t v)
{
    if (buf_reserve(b, 4)) {
        b->data[b->len++] = (uint8_t)(v >> 24);
        b->data[b->len++] = (uint8_t)(v >> 16);
        b->data[b->len++] = (uint8_t)(v >> 8);
        b->data[b->len++] = (uint8_t)v;
    }
}

void vpp_buf_put_u64(vpp_buf_t *b, uint64_t v)
{
    vpp_buf_put_u32(b, (uint32_t)(v >> 32));
    vpp_buf_put_u32(b, (uint32_t)v);
}

void vpp_buf_put_bytes(vpp_buf_t *b, const void *p, uint32_t n)
{
    if (!p || n == 0) return;
    if (buf_reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

/*
 * Write a VPP `string`.
 *
 * VPP's memclnt.api declares these as `string name [64]`, where [64] is the
 * string's declared capacity, not a C array. On the wire a string is a u32
 * length followed by exactly that many bytes and *no* padding:
 * vl_api_to_api_string() in api_types.h returns `len + sizeof(u32)`.
 *
 * It is not a u8 length either: encoding it that way shifted every following
 * field and desynchronised the message, so interface and VRF names never
 * arrived intact.
 */
void vpp_buf_put_string(vpp_buf_t *b, const char *s)
{
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    vpp_buf_put_u32(b, n);
    vpp_buf_put_bytes(b, s, n);
}

/* =========================================================================
 * Reader
 * ========================================================================= */

void vpp_reader_init(vpp_reader_t *r, const void *buf, uint32_t len)
{
    r->p = buf;
    r->len = len;
    r->pos = 0;
}

bool vpp_reader_ok(const vpp_reader_t *r)
{
    return r->p != NULL && r->pos <= r->len;
}

static uint32_t rd_take(vpp_reader_t *r, uint32_t n, uint8_t *out)
{
    if (!r->p || r->pos + n > r->len) {
        r->pos = r->len + 1;  /* poison: mark over-run */
        return 0;
    }
    if (out) memcpy(out, r->p + r->pos, n);
    r->pos += n;
    return n;
}

uint8_t vpp_rd_u8(vpp_reader_t *r)
{
    uint8_t v = 0;
    rd_take(r, 1, &v);
    return v;
}

uint16_t vpp_rd_u16(vpp_reader_t *r)
{
    uint8_t t[2] = {0, 0};
    rd_take(r, 2, t);
    return (uint16_t)((t[0] << 8) | t[1]);
}

uint32_t vpp_rd_u32(vpp_reader_t *r)
{
    uint8_t t[4] = {0, 0, 0, 0};
    rd_take(r, 4, t);
    return ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) |
           ((uint32_t)t[2] << 8) | (uint32_t)t[3];
}

uint64_t vpp_rd_u64(vpp_reader_t *r)
{
    return ((uint64_t)vpp_rd_u32(r) << 32) | vpp_rd_u32(r);
}

bool vpp_rd_bytes(vpp_reader_t *r, void *out, uint32_t n)
{
    return rd_take(r, n, out) == n;
}

/* Read a VPP `string`: u32 length, that many bytes, then zero padding to a
 * 4-byte boundary of the field length. Mirrors vpp_buf_put_string. */
/* Read a VPP `string`: u32 length then exactly that many bytes, no padding.
 * Mirrors vpp_buf_put_string and VPP's vl_api_to_api_string(). */
char *vpp_rd_string(vpp_reader_t *r, uint32_t max_len)
{
    uint32_t n = vpp_rd_u32(r);
    if (!vpp_reader_ok(r)) return NULL;
    /* A length beyond what is left means the stream is desynchronised, e.g. a
     * u8 length where a u32 belongs. Fail rather than read past the frame. */
    if ((uint64_t)n > r->len) return NULL;
    uint32_t copy = n;
    if (copy > max_len) copy = max_len;
    char *s = malloc(copy + 1);
    if (!s) return NULL;
    if (!vpp_rd_bytes(r, s, copy)) {
        free(s);
        return NULL;
    }
    s[copy] = '\0';
    /* consume any remainder of the declared length */
    if (n > copy && !vpp_rd_bytes(r, NULL, n - copy)) {
        free(s);
        return NULL;
    }
    return s;
}

/* =========================================================================
 * Socket framing
 * ========================================================================= */

uint64_t vpp_wire_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int vpp_wire_wait(int fd, short events, uint64_t deadline_ms)
{
    for (;;) {
        uint64_t now = vpp_wire_now_ms();
        if (now >= deadline_ms) { errno = ETIMEDOUT; return -1; }
        uint64_t remaining = deadline_ms - now;
        struct pollfd pfd = { .fd = fd, .events = events };
        int rc = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc > 0 && (pfd.revents & events)) return 0;
        if (rc == 0) errno = ETIMEDOUT;
        return -1;
    }
}

/* Nonblocking connect shares the operation deadline with registration. */
int vpp_wire_connect_until(int fd, const struct sockaddr *addr, socklen_t len,
                          uint64_t deadline_ms)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    if (connect(fd, addr, len) == 0) return 0;
    if (errno != EINPROGRESS && errno != EAGAIN) return -1;
    if (vpp_wire_wait(fd, POLLOUT, deadline_ms) != 0) return -1;
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0 || error) {
        if (error) errno = error;
        return -1;
    }
    return 0;
}

static bool transfer_all(int fd, void *p, uint32_t n, bool writing,
                          uint64_t deadline_ms)
{
    uint8_t *b = p;
    while (n > 0) {
        if (vpp_wire_wait(fd, writing ? POLLOUT : POLLIN, deadline_ms) != 0)
            return false;
        ssize_t k = writing ? send(fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT)
                            : recv(fd, b, n, MSG_DONTWAIT);
        if (k <= 0) {
            if (k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            return false;
        }
        b += k;
        n -= (uint32_t)k;
    }
    return true;
}

int vpp_wire_send_until(int fd, uint16_t msg_id, const uint8_t *body,
                        uint32_t body_len, uint64_t deadline_ms)
{
    if (fd < 0 || (!body && body_len) || body_len > UINT32_MAX - 18U) return -1;
    uint32_t data_len = 2 + body_len;
    uint8_t hdr[16] = {0};
    hdr[8] = (uint8_t)(data_len >> 24);
    hdr[9] = (uint8_t)(data_len >> 16);
    hdr[10] = (uint8_t)(data_len >> 8);
    hdr[11] = (uint8_t)data_len;
    uint8_t id[2] = {(uint8_t)(msg_id >> 8), (uint8_t)msg_id};
    if (!transfer_all(fd, hdr, sizeof(hdr), true, deadline_ms) ||
        !transfer_all(fd, id, sizeof(id), true, deadline_ms) ||
        (body_len && !transfer_all(fd, (void *)body, body_len, true, deadline_ms)))
        return -1;
    return 0;
}

int vpp_wire_recv_until(int fd, uint8_t *buf, uint32_t buf_size,
                        uint64_t deadline_ms)
{
    if (fd < 0 || !buf || buf_size < 4) return -1;
    uint8_t hdr[16];
    if (!transfer_all(fd, hdr, sizeof(hdr), false, deadline_ms)) return -1;
    uint32_t data_len = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                        ((uint32_t)hdr[10] << 8) | hdr[11];
    if (data_len < 2 || data_len > buf_size || data_len > INT_MAX) return -1;
    if (!transfer_all(fd, buf, data_len, false, deadline_ms)) return -1;
    return (int)data_len;
}

int vpp_wire_send_fd(int fd, uint16_t msg_id, const uint8_t *body, uint32_t body_len)
{
    return vpp_wire_send_until(fd, msg_id, body, body_len, vpp_wire_now_ms() + 5000);
}

int vpp_wire_recv_fd(int fd, uint8_t *buf, uint32_t buf_size)
{
    return vpp_wire_recv_until(fd, buf, buf_size, vpp_wire_now_ms() + 5000);
}

/* =========================================================================
 * name -> msg_id table
 * ========================================================================= */

static uint32_t name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;  /* FNV-1a */
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    return (uint32_t)(h ^ (h >> 32));
}

void vpp_msg_table_init(vpp_msg_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

bool vpp_msg_table_add(vpp_msg_table_t *t, const char *name, uint16_t msg_id)
{
    if (!t || !name || !name[0]) return false;
    uint32_t mask = VPP_MSG_TABLE_SIZE - 1;
    uint32_t i = name_hash(name) & mask;
    for (uint32_t probe = 0; probe < VPP_MSG_TABLE_SIZE; probe++) {
        vpp_msg_table_entry_t *e = &t->entries[i];
        if (!e->used) {
            snprintf(e->name, sizeof(e->name), "%s", name);
            e->msg_id = msg_id;
            e->used = true;
            t->count++;
            return true;
        }
        if (strcmp(e->name, name) == 0) {
            e->msg_id = msg_id;  /* update */
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

bool vpp_msg_table_lookup(const vpp_msg_table_t *t, const char *name, uint16_t *msg_id)
{
    if (!t || !name) return false;
    uint32_t mask = VPP_MSG_TABLE_SIZE - 1;
    uint32_t i = name_hash(name) & mask;
    for (uint32_t probe = 0; probe < VPP_MSG_TABLE_SIZE; probe++) {
        const vpp_msg_table_entry_t *e = &t->entries[i];
        if (!e->used) return false;
        if (strcmp(e->name, name) == 0) {
            if (msg_id) *msg_id = e->msg_id;
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

uint32_t vpp_msg_table_count(const vpp_msg_table_t *t)
{
    return t ? t->count : 0;
}
