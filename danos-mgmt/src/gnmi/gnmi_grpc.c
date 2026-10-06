/*
 * DANOS-Open Management: gNMI gRPC Server implementation (v0.3)
 *
 * HTTP/2 (mTLS or laboratory h2c) server with HPACK, gRPC message framing
 * ([1B compressed][4B BE length][protobuf]), and gNMI method dispatch.
 */

#include "gnmi_grpc.h"
#include "../authz/authz.h"
#include "gnmi_proto.h"
#include "model_paths.h"
#include "model_routes.h"
#include "hpack.h"
#include "gnmi.h"          /* existing DPA-backed set helpers reuse */
#include <danos/dpa.h>
#include <danos/core/object_registry.h>
#include <danos/core/transaction.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <poll.h>

#define H2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_PREFACE_LEN 24

enum {
    H2_F_DATA = 0, H2_F_HEADERS = 1, H2_F_PRIORITY = 2, H2_F_RST = 3,
    H2_F_SETTINGS = 4, H2_F_PUSH = 5, H2_F_PING = 6, H2_F_GOAWAY = 7,
    H2_F_WINDOW = 8, H2_F_CONT = 9,
};

#define H2_FLAG_END_STREAM  0x1
#define H2_FLAG_ACK         0x1
#define H2_FLAG_END_HEADERS 0x4

#define MAX_FRAME 16384
#define MAX_HEADERS 64
#define MAX_MSG (256 * 1024)

/* Frames read out of order (e.g. another stream's DATA while we are
 * window-blocked on a response) are parked here.
 *
 * The queue is FIFO. It used to push at the head and pop from the head,
 * which reversed frames belonging to the same stream, so a gRPC message
 * split across several DATA frames was reassembled in the wrong order. */
typedef struct pending_frame {
    uint8_t type, flags;
    uint32_t stream;
    uint8_t *payload;
    uint32_t len;
    struct pending_frame *next;
} pending_frame_t;

/* Per-stream flow-control state.
 *
 * Windows were kept in `stream_window[stream % 64]`, so stream 1 and stream
 * 65 shared one budget and could starve each other. HTTP/2 stream
 * identifiers increase monotonically, so any connection running more than a
 * few dozen concurrent RPCs aliased. Keyed by the real identifier here. */
typedef struct {
    uint32_t id;
    bool     used;
    int64_t  window;        /* how much the peer still allows us to send */
} h2_stream_t;

#define H2_MAX_TRACKED_STREAMS 256
/* Bound on parked frames: a peer that floods frames while we are
 * window-blocked must not be able to grow this without limit. */
#define H2_MAX_PENDING 64

typedef struct {
    int fd;
    SSL *ssl;
    hpack_dyn_table_t dyn_dec;   /* decoder-side dynamic table */
    /* HTTP/2 flow control (our send side) */
    int64_t conn_window;         /* stream-0 budget */
    h2_stream_t streams[H2_MAX_TRACKED_STREAMS];
    uint32_t peer_initial_window;
    pending_frame_t *pending, *pending_tail;
    uint32_t pending_count;
} h2_conn_t;

#define H2_DEFAULT_WINDOW 65535
#define H2_MAX_FRAME_LEN  16384

/* =========================================================================
 * Frame I/O
 * ========================================================================= */

static int read_full(h2_conn_t *c, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t k = c->ssl ? SSL_read(c->ssl, p, (int)n) : recv(c->fd, p, n, 0);
        if (k <= 0) {
            if (!c->ssl && k < 0 && errno == EINTR) continue;
            return -1;
        }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int write_full(h2_conn_t *c, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = c->ssl ? SSL_write(c->ssl, p, (int)n) : send(c->fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) {
            if (!c->ssl && k < 0 && errno == EINTR) continue;
            return -1;
        }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int write_frame(h2_conn_t *c, uint8_t type, uint8_t flags, uint32_t stream,
                       const void *payload, uint32_t len)
{
    uint8_t hdr[9];
    hdr[0] = (uint8_t)(len >> 16);
    hdr[1] = (uint8_t)(len >> 8);
    hdr[2] = (uint8_t)len;
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = (uint8_t)(stream >> 24);
    hdr[6] = (uint8_t)(stream >> 16);
    hdr[7] = (uint8_t)(stream >> 8);
    hdr[8] = (uint8_t)stream;
    if (write_full(c, hdr, 9) < 0) return -1;
    if (len && write_full(c, payload, len) < 0) return -1;
    return 0;
}

/* =========================================================================
 * Frame receive with pending queue + flow-control state machine
 * ========================================================================= */

static h2_stream_t *stream_find(h2_conn_t *c, uint32_t id)
{
    for (int i = 0; i < H2_MAX_TRACKED_STREAMS; i++)
        if (c->streams[i].used && c->streams[i].id == id)
            return &c->streams[i];
    return NULL;
}

/* Window for `id`, creating the entry on first use. Returns NULL only if
 * every slot is taken, in which case the caller treats the stream as
 * unusable rather than aliasing someone else's budget. */
static h2_stream_t *stream_get(h2_conn_t *c, uint32_t id)
{
    h2_stream_t *s = stream_find(c, id);
    if (s) return s;
    for (int i = 0; i < H2_MAX_TRACKED_STREAMS; i++) {
        if (c->streams[i].used) continue;
        c->streams[i].used = true;
        c->streams[i].id = id;
        c->streams[i].window = c->peer_initial_window;
        return &c->streams[i];
    }
    return NULL;
}

static void stream_forget(h2_conn_t *c, uint32_t id)
{
    h2_stream_t *s = stream_find(c, id);
    if (s) s->used = false;
}

static void pending_push(h2_conn_t *c, uint8_t type, uint8_t flags,
                         uint32_t stream, const uint8_t *payload, uint32_t len)
{
    if (c->pending_count >= H2_MAX_PENDING) return;   /* bounded */
    pending_frame_t *pf = malloc(sizeof(*pf));
    if (!pf) return;
    pf->type = type;
    pf->flags = flags;
    pf->stream = stream;
    pf->len = len;
    pf->payload = len ? malloc(len) : NULL;
    if (len && !pf->payload) {
        free(pf);
        return;
    }
    if (len) memcpy(pf->payload, payload, len);
    pf->next = NULL;
    /* FIFO: append at the tail so frames on one stream stay in order. */
    if (c->pending_tail) c->pending_tail->next = pf;
    else c->pending = pf;
    c->pending_tail = pf;
    c->pending_count++;
}

static bool pending_pop(h2_conn_t *c, uint8_t *type, uint8_t *flags,
                        uint32_t *stream, uint8_t *payload, uint32_t *len)
{
    pending_frame_t *pf = c->pending;
    if (!pf) return false;
    c->pending = pf->next;
    if (!c->pending) c->pending_tail = NULL;
    if (c->pending_count) c->pending_count--;
    *type = pf->type;
    *flags = pf->flags;
    *stream = pf->stream;
    *len = pf->len < *len ? pf->len : *len;
    if (pf->len && payload) memcpy(payload, pf->payload, *len);
    free(pf->payload);
    free(pf);
    return true;
}

static void pending_free_all(h2_conn_t *c)
{
    while (c->pending) {
        pending_frame_t *pf = c->pending;
        c->pending = pf->next;
        free(pf->payload);
        free(pf);
    }
    c->pending_tail = NULL;
    c->pending_count = 0;
}

/* Read one frame from the wire, handling connection-level housekeeping
 * (SETTINGS ack + window updates, PING ack). Frames for other streams
 * encountered while the caller waits are returned to it via the queue;
 * this function only returns frames that matter to the caller. */
/* Returns 0 = frame delivered, 1 = housekeeping processed (flow-control
 * windows may have changed; caller should re-check budget), -1 = error. */
static int recv_frame(h2_conn_t *c, uint8_t *type, uint8_t *flags,
                      uint32_t *stream, uint8_t *payload, uint32_t *len)
{
    for (;;) {
        if (pending_pop(c, type, flags, stream, payload, len)) return 0;

        uint8_t fh[9];
        if (read_full(c, fh, 9) < 0) return -1;
        uint32_t flen = ((uint32_t)fh[0] << 16) | ((uint32_t)fh[1] << 8) | fh[2];
        uint8_t ftype = fh[3], fflags = fh[4];
        uint32_t fstream = ((uint32_t)fh[5] << 24) | ((uint32_t)fh[6] << 16) |
                           ((uint32_t)fh[7] << 8) | fh[8];
        if (flen > H2_MAX_FRAME_LEN) return -1;
        uint8_t fbuf[H2_MAX_FRAME_LEN];
        if (flen && read_full(c, fbuf, flen) < 0) return -1;

        switch (ftype) {
        case H2_F_SETTINGS: {
            if (!(fflags & H2_FLAG_ACK)) {
                /* parse peer INITIAL_WINDOW_SIZE (id 4) and MAX_FRAME_SIZE (5) */
                if (flen % 6 == 0) {
                    for (uint32_t i = 0; i + 6 <= flen; i += 6) {
                        uint16_t id = (uint16_t)((fbuf[i] << 8) | fbuf[i + 1]);
                        uint32_t v = ((uint32_t)fbuf[i + 2] << 24) |
                                     ((uint32_t)fbuf[i + 3] << 16) |
                                     ((uint32_t)fbuf[i + 4] << 8) | fbuf[i + 5];
                        if (id == 0x4) {
                            /* RFC 7540 6.9.2: a change to
                             * INITIAL_WINDOW_SIZE is a *delta* applied to
                             * every stream's current window, not an
                             * absolute reset. Assigning it overwrote live
                             * windows and desynchronised the accounting. */
                            int64_t delta = (int64_t)v -
                                             (int64_t)c->peer_initial_window;
                            c->peer_initial_window = v;
                            for (int w = 0; w < H2_MAX_TRACKED_STREAMS; w++)
                                if (c->streams[w].used)
                                    c->streams[w].window += delta;
                        }
                    }
                }
                write_frame(c, H2_F_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
            }
            continue;   /* housekeeping: caller never sees SETTINGS */
        }
        case H2_F_PING:
            if (!(fflags & H2_FLAG_ACK))
                write_frame(c, H2_F_PING, H2_FLAG_ACK, 0, fbuf, flen);
            continue;
        case H2_F_WINDOW: {
            if (flen < 4) continue;
            uint32_t inc = ((uint32_t)fbuf[0] << 24) | ((uint32_t)fbuf[1] << 16) |
                           ((uint32_t)fbuf[2] << 8) | fbuf[3];
            if (fstream == 0) {
                c->conn_window += inc;
            } else {
                h2_stream_t *s = stream_get(c, fstream);
                if (s) s->window += inc;
            }
            return 1;   /* caller may be unblocked */
        }
        default:
            *type = ftype;
            *flags = fflags;
            *stream = fstream;
            *len = flen;
            if (flen && payload) memcpy(payload, fbuf, flen);
            return 0;
        }
    }
}

/* Replenish our receive window after consuming `n` bytes of DATA.
 *
 * Without this the peer exhausts its 65535-byte send window and blocks
 * forever: it will not send a WINDOW_UPDATE until we have consumed data and
 * told it there is room again. Any request body over 64 KiB deadlocked the
 * connection. Emitted once the consumed amount reaches half a window rather
 * than per frame. */
static void send_window_update(h2_conn_t *c, uint32_t stream, uint32_t n)
{
    uint32_t inc = (n + 3) & ~3u;   /* WINDOW_UPDATE increment must be >0 */
    uint8_t p[4];
    p[0] = (uint8_t)(inc >> 24); p[1] = (uint8_t)(inc >> 16);
    p[2] = (uint8_t)(inc >> 8);  p[3] = (uint8_t)inc;
    (void)write_frame(c, H2_F_WINDOW, 0, 0, p, 4);
    if (stream) (void)write_frame(c, H2_F_WINDOW, 0, stream, p, 4);
}

/* Send `len` bytes as DATA frames respecting peer flow-control windows.
 * While blocked on window budget, incoming frames are queued. */
static int send_data_windowed(h2_conn_t *c, uint32_t stream,
                              const uint8_t *data, size_t len)
{
    size_t sent = 0;
    h2_stream_t *s = stream_get(c, stream);
    if (!s) return -1;
    while (sent < len) {
        int64_t budget = s->window;
        if (budget > c->conn_window) budget = c->conn_window;
        if (budget > H2_MAX_FRAME_LEN) budget = H2_MAX_FRAME_LEN;

        if (budget <= 0) {
            /* wait for WINDOW_UPDATE; park unrelated frames */
            uint8_t t, fl;
            uint32_t st;
            uint32_t plen = H2_MAX_FRAME_LEN;
            uint8_t pl[H2_MAX_FRAME_LEN];
            int rr = recv_frame(c, &t, &fl, &st, pl, &plen);
            if (rr < 0) return -1;
            if (rr == 1) continue;              /* window refilled */
            if ((t == H2_F_DATA || t == H2_F_HEADERS) && st != stream)
                pending_push(c, t, fl, st, pl, plen);
            continue;
        }

        size_t chunk = (size_t)budget < len - sent ? (size_t)budget : len - sent;
        if (write_frame(c, H2_F_DATA, 0, stream,
                        data + sent, (uint32_t)chunk) < 0) return -1;
        sent += chunk;
        s->window -= (int64_t)chunk;
        c->conn_window -= (int64_t)chunk;
    }
    return 0;
}

/* =========================================================================
 * gRPC response helpers
 * ========================================================================= */

static int send_grpc_response(h2_conn_t *c, uint32_t stream,
                              const uint8_t *msg, size_t len)
{
    /* HEADERS: :status 200 + content-type application/grpc */
    uint8_t hbuf[128];
    size_t hlen = 0;
    int k = hpack_encode_literal(hbuf + hlen, sizeof(hbuf) - hlen,
                                 ":status", "200");
    if (k < 0) return -1;
    hlen += (size_t)k;
    k = hpack_encode_literal(hbuf + hlen, sizeof(hbuf) - hlen,
                             "content-type", "application/grpc");
    if (k < 0) return -1;
    hlen += (size_t)k;

    if (write_frame(c, H2_F_HEADERS, 0x4 /* END_HEADERS */, stream,
                    hbuf, (uint32_t)hlen) < 0)
        return -1;

    /* DATA: gRPC frame header + windowed payload */
    uint8_t gbuf[5];
    gbuf[0] = 0;  /* not compressed */
    gbuf[1] = (uint8_t)(len >> 24);
    gbuf[2] = (uint8_t)(len >> 16);
    gbuf[3] = (uint8_t)(len >> 8);
    gbuf[4] = (uint8_t)len;
    if (write_frame(c, H2_F_DATA, 0, stream, gbuf, 5) < 0) return -1;
    if (send_data_windowed(c, stream, msg, len) < 0) return -1;
    if (len == 0) {
        if (write_frame(c, H2_F_DATA, H2_FLAG_END_STREAM, stream, NULL, 0) < 0)
            return -1;
    }

    /* Trailers: grpc-status 0, END_STREAM */
    uint8_t tbuf[64];
    size_t tlen = 0;
    k = hpack_encode_literal(tbuf, sizeof(tbuf), "grpc-status", "0");
    if (k < 0) return -1;
    tlen = (size_t)k;
    return write_frame(c, H2_F_HEADERS, H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS,
                       stream, tbuf, (uint32_t)tlen);
}

/* =========================================================================
 * gNMI handlers (protobuf level)
 * ========================================================================= */

static danos_state_store_t *g_store;

/* store-mutation notification for STREAM subscriptions (v0.13):
 * object-registry events bump the epoch; waiting loops use the cond */
static pthread_mutex_t g_store_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_store_ev_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_store_epoch = 0;
static pthread_once_t g_event_subscribe_once = PTHREAD_ONCE_INIT;

static void store_event_cb(const danos_event_t *ev, void *user)
{
    (void)ev; (void)user;
    pthread_mutex_lock(&g_store_ev_lock);
    g_store_epoch++;
    pthread_cond_broadcast(&g_store_ev_cond);
    pthread_mutex_unlock(&g_store_ev_lock);
}

static void subscribe_store_events_once(void)
{
    danos_event_subscribe((danos_event_type_t)
        (DANOS_EVENT_OBJ_CREATED | DANOS_EVENT_OBJ_UPDATED |
         DANOS_EVENT_OBJ_DELETED), store_event_cb, NULL);
}

_Atomic uint64_t g_rpcs_total = 0;

static danos_state_store_t *store_or_default(void)
{
    return g_store ? g_store : NULL;
}

/*
 * Resolve the desired-state store the handlers read and mutate.
 *
 * The update paths wrote g_default_store directly while the delete paths
 * used ss->desired, so a create-then-delete inside one SetRequest operated
 * on two different stores. Worse, store_or_default() returns NULL when no
 * state store is configured - the default in a daemon that never calls the
 * setter - and the delete paths dereferenced it directly, killing the
 * connection thread on any Set carrying a delete.
 *
 * One resolver, used by every path: the state store's desired set when one
 * is configured, otherwise the global default store.
 */
static danos_object_store_t *desired_store(danos_state_store_t *ss)
{
    if (ss && ss->desired) return ss->desired;
    return g_default_store;
}

/* Serialize one object as a JSON-ish value (we store DPA structs; emit
 * a compact JSON encoding per object type). */
static int obj_to_json(danos_obj_type_t type, const void *data, size_t size,
                       char *out, size_t cap)
{
    if (type == DANOS_OBJ_IFACE && size >= sizeof(danos_iface_t)) {
        const danos_iface_t *i = data;
        snprintf(out, cap,
                 "{\"ifindex\":%u,\"name\":\"%s\",\"mtu\":%u,\"admin_up\":%s}",
                 i->ifindex, i->name, i->mtu, i->admin_up ? "true" : "false");
        return 0;
    }
    if (type == DANOS_OBJ_ROUTE && size >= sizeof(danos_route_t)) {
        const danos_route_t *r = data;
        char pfx[64];
        if (r->prefix.addr.af == DANOS_AF_IPV4) {
            snprintf(pfx, sizeof(pfx), "%u.%u.%u.%u/%u",
                     r->prefix.addr.addr[0], r->prefix.addr.addr[1],
                     r->prefix.addr.addr[2], r->prefix.addr.addr[3],
                     r->prefix.prefix_len);
        } else {
            snprintf(pfx, sizeof(pfx), "<ipv6>/%u", r->prefix.prefix_len);
        }
        snprintf(out, cap, "{\"prefix\":\"%s\",\"vrf\":%u}",
                 pfx, r->vrf_id);
        return 0;
    }
    if (type == DANOS_OBJ_VRF && size >= sizeof(danos_vrf_t)) {
        const danos_vrf_t *v = data;
        snprintf(out, cap, "{\"vrf_id\":%u,\"name\":\"%s\"}",
                 v->vrf_id, v->name);
        return 0;
    }
    snprintf(out, cap, "{}");
    return -1;
}

/* Collect objects of one type (iterate desired store buckets) */
typedef struct {
    danos_obj_type_t type;
    void *out;
    size_t out_size;     /* per-element size */
    size_t max;
    size_t count;
} collect_ctx_t;

static void collect_cb(danos_object_entry_t *e, void *user)
{
    collect_ctx_t *c = user;
    if (e->type != c->type || c->count >= c->max) return;
    char *dst = (char *)c->out + c->count * c->out_size;
    size_t n = e->data_size < c->out_size ? e->data_size : c->out_size;
    memcpy(dst, e->data, n);
    c->count++;
}

static uint32_t collect_objects(danos_state_store_t *ss, danos_obj_type_t type,
                                void *out, size_t elem_size, size_t max)
{
    /* No explicit store: read the DPA default store (the same store
     * danos_tx_commit writes to). */
    danos_object_store_t *src = desired_store(ss);
    if (!src) return 0;
    collect_ctx_t c = { .type = type, .out = out, .out_size = elem_size,
                        .max = max, .count = 0 };
    danos_object_iterate(src, collect_cb, &c);
    return (uint32_t)c.count;
}

/*
 * Look up one interface by name.
 *
 * The callers used to copy every interface of a type into a fixed array
 * (16 or 64 entries) and then scan it by name. Once a store held more
 * interfaces than the array, the wanted one was simply absent from the
 * copy and the lookup reported NOT_FOUND even though the object existed -
 * which aborted the whole SetRequest. Searching the store directly has no
 * capacity limit and allocates nothing per call.
 */
typedef struct {
    const char      *name;
    danos_iface_t   *out;
    bool             found;
} find_iface_ctx_t;

static void find_iface_cb(danos_object_entry_t *e, void *user)
{
    find_iface_ctx_t *c = user;
    if (c->found) return;
    if (e->type != DANOS_OBJ_IFACE || e->data_size < sizeof(danos_iface_t))
        return;
    const danos_iface_t *i = e->data;
    if (strcmp(i->name, c->name) == 0) {
        memcpy(c->out, i, sizeof(*c->out));
        c->found = true;
    }
}

static bool find_iface_by_name(danos_state_store_t *ss, const char *name,
                               danos_iface_t *out)
{
    danos_object_store_t *src = desired_store(ss);
    if (!src || !name || !out) return false;
    find_iface_ctx_t c = { .name = name, .out = out, .found = false };
    danos_object_iterate(src, find_iface_cb, &c);
    return c.found;
}

/* Send response HEADERS for a streaming RPC (no END_STREAM) */
static int send_stream_headers(h2_conn_t *c, uint32_t stream)
{
    uint8_t hbuf[128];
    size_t hlen = 0;
    int k = hpack_encode_literal(hbuf + hlen, sizeof(hbuf) - hlen,
                                 ":status", "200");
    if (k < 0) return -1;
    hlen += (size_t)k;
    k = hpack_encode_literal(hbuf + hlen, sizeof(hbuf) - hlen,
                             "content-type", "application/grpc");
    if (k < 0) return -1;
    hlen += (size_t)k;
    return write_frame(c, H2_F_HEADERS, 0x4, stream, hbuf, (uint32_t)hlen);
}

/* Send one gRPC message on an open stream (DATA frames only) */
static int send_stream_message(h2_conn_t *c, uint32_t stream,
                               const uint8_t *msg, size_t len)
{
    uint8_t gbuf[5] = { 0, (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                        (uint8_t)(len >> 8), (uint8_t)len };
    if (write_frame(c, H2_F_DATA, 0, stream, gbuf, 5) < 0) return -1;
    return send_data_windowed(c, stream, msg, len);
}

static int send_stream_trailers(h2_conn_t *c, uint32_t stream)
{
    uint8_t tbuf[64];
    int k = hpack_encode_literal(tbuf, sizeof(tbuf), "grpc-status", "0");
    if (k < 0) return -1;
    return write_frame(c, H2_F_HEADERS,
                       H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS,
                       stream, tbuf, (size_t)k);
}

/* Build a notification containing every object matching one path */
static int build_notification_for_paths(const gnmi_subscribe_list_t *sl,
                                        const gnmi_path_t *paths,
                                        uint32_t path_count,
                                        uint8_t *resp, size_t resp_cap)
{
    (void)sl;
    /* Emits the BARE Notification message body (no field tag): the
     * caller wraps it as SubscribeResponse.update. */
    gnmi_pb_t w;
    gnmi_pb_init(&w, resp, resp_cap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    gnmi_pb_put_uint64(&w, 1, (uint64_t)ts.tv_sec * 1000000000ULL +
                                  (uint64_t)ts.tv_nsec);

    for (uint32_t i = 0; i < path_count; i++) {
        const gnmi_path_t *p = &paths[i];
        if (p->elem_count == 0) continue;
        const char *top = p->elems[0].name;

        if (strcmp(top, "interfaces") == 0) {
            danos_iface_t ifaces[64];
            uint32_t n = collect_objects(NULL, DANOS_OBJ_IFACE, ifaces,
                                         sizeof(ifaces[0]), 64);
            for (uint32_t j = 0; j < n; j++) {
                if (p->elem_count >= 2 && p->elems[1].has_key &&
                    strcmp(p->elems[1].key_value, ifaces[j].name) != 0)
                    continue;
                char json[256];
                obj_to_json(DANOS_OBJ_IFACE, &ifaces[j], sizeof(ifaces[j]),
                            json, sizeof(json));
                size_t us = gnmi_pb_begin_nested(&w, 4);
                gnmi_encode_path(&w, 1, p);
                gnmi_typed_value_t v = { .kind = GNMI_VAL_JSON_IETF };
                snprintf(v.s, sizeof(v.s), "%s", json);
                gnmi_encode_typed_value(&w, 3, &v);
                gnmi_pb_end_nested(&w, us);
            }
        } else if (strcmp(top, "vrfs") == 0) {
            danos_vrf_t vrfs[16];
            uint32_t n = collect_objects(NULL, DANOS_OBJ_VRF, vrfs,
                                         sizeof(vrfs[0]), 16);
            for (uint32_t j = 0; j < n; j++) {
                char json[256];
                obj_to_json(DANOS_OBJ_VRF, &vrfs[j], sizeof(vrfs[j]),
                            json, sizeof(json));
                size_t us = gnmi_pb_begin_nested(&w, 4);
                gnmi_encode_path(&w, 1, p);
                gnmi_typed_value_t v = { .kind = GNMI_VAL_JSON_IETF };
                snprintf(v.s, sizeof(v.s), "%s", json);
                gnmi_encode_typed_value(&w, 3, &v);
                gnmi_pb_end_nested(&w, us);
            }
        } else if (strcmp(top, "routes") == 0) {
            danos_route_t routes[32];
            uint32_t n = collect_objects(NULL, DANOS_OBJ_ROUTE, routes,
                                         sizeof(routes[0]), 32);
            for (uint32_t j = 0; j < n; j++) {
                char json[256];
                obj_to_json(DANOS_OBJ_ROUTE, &routes[j], sizeof(routes[j]),
                            json, sizeof(json));
                size_t us = gnmi_pb_begin_nested(&w, 4);
                gnmi_encode_path(&w, 1, p);
                gnmi_typed_value_t v = { .kind = GNMI_VAL_JSON_IETF };
                snprintf(v.s, sizeof(v.s), "%s", json);
                gnmi_encode_typed_value(&w, 3, &v);
                gnmi_pb_end_nested(&w, us);
            }
        }
    }
    return w.overflow ? -1 : (int)w.len;
}

/* STREAM mode: push notifications when the store changes. Poll every
 * 100 ms; detect client GOAWAY/RST/close via recv with MSG_PEEK. */
/* Order-sensitive hash over every object's bytes. FNV-1a over the payload,
 * folded with the type and id so a change of either registers. Streams the
 * store rather than copying into a fixed array, so correctness does not
 * depend on a capacity that a large config can exceed. */
static void hash_entry(danos_object_entry_t *e, void *user)
{
    uint64_t *h = user;
    uint64_t v = *h;
    uint8_t hdr[16];
    uint32_t t = (uint32_t)e->type, id32 = (uint32_t)e->id;
    uint64_t id = e->id;
    memcpy(hdr, &t, 4);
    memcpy(hdr + 4, &id, 8);
    memcpy(hdr + 12, &id32, 4);
    for (size_t i = 0; i < sizeof(hdr); i++) {
        v ^= hdr[i];
        v *= 1099511628211ULL;
    }
    for (size_t i = 0; i < e->data_size; i++) {
        v ^= ((const uint8_t *)e->data)[i];
        v *= 1099511628211ULL;
    }
    *h = v;
}

/* Exposed for tests: the STREAM change detector's input hash. */
uint64_t stream_state_hash_for_test(void);

static uint64_t stream_state_hash(void)
{
    uint64_t h = 14695981039346656037ULL;
    if (g_default_store) danos_object_iterate(g_default_store, hash_entry, &h);
    return h;
}

uint64_t stream_state_hash_for_test(void)
{
    return stream_state_hash();
}

static int subscribe_stream_loop(h2_conn_t *c, uint32_t stream,
                                 const gnmi_subscribe_list_t *sl)
{
    uint64_t last_hash = 0;
    bool first = true;

    for (;;) {
        /* check peer liveness + consume control frames */
        uint8_t probe[16];
        ssize_t n = -1;
        if (c->ssl) {
            struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
            if (SSL_pending(c->ssl) || poll(&pfd, 1, 0) > 0) {
                n = SSL_peek(c->ssl, probe, sizeof(probe));
                if (n <= 0) return 0;
            }
        } else {
            n = recv(c->fd, probe, sizeof(probe), MSG_PEEK | MSG_DONTWAIT);
        }
        if (n == 0) return 0;   /* client closed */

        /* Change detector.
         *
         * The previous version copied each type into a fixed array
         * (ifaces[64], vrfs[16], routes[32]) and hashed only interface mtu
         * and ifindex. Two problems: a store holding more interfaces than the
         * array made the scan silently miss objects, and any change to an
         * interface name, admin state or address - or to a VRF name or a
         * route prefix, metric or next hop - did not alter the hash at all,
         * so a subscriber was never told.
         *
         * Hash the raw object bytes instead, streaming through the store so
         * there is no capacity limit. */
        uint64_t hash = stream_state_hash();

        /* wait for a store mutation (or 200 ms tick) before re-hashing */
        pthread_mutex_lock(&g_store_ev_lock);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_store_ev_cond, &g_store_ev_lock, &ts);
        pthread_mutex_unlock(&g_store_ev_lock);

        if (first || hash != last_hash) {
            gnmi_path_t paths[GNMI_MAX_ELEMS];
            uint32_t pc = 0;
            for (uint32_t i = 0; i < sl->sub_count && pc < GNMI_MAX_ELEMS; i++)
                paths[pc++] = sl->subs[i].path;

            uint8_t msg[8192];
            int mlen = build_notification_for_paths(sl, paths, pc,
                                                    msg, sizeof(msg));
            if (mlen < 0) return -1;

            if (first) {
                if (send_stream_headers(c, stream) < 0) return -1;
                first = false;
            }
            uint8_t resp[16384];
            gnmi_pb_t w;
            gnmi_pb_init(&w, resp, sizeof(resp));
            /* SubscribeResponse: field1 = Notification bytes */
            gnmi_pb_put_len_delim(&w, 1, msg, (size_t)mlen);
            if (send_stream_message(c, stream, resp, w.len) < 0) return -1;
            last_hash = hash;
        }
        usleep(100000);
    }
}

/*
 * POLL subscription state, keyed by stream.
 *
 * gNMI POLL is request/response: the client opens a stream with a Subscribe,
 * receives the initial set plus a sync_response, then sends a Poll on the same
 * stream for each later sample. This used to ignore that - every message on
 * the stream was handled as a fresh Subscribe, so each Poll re-sent the whole
 * initial set instead of a delta, and the notification and sync_response were
 * emitted in one message even though SubscribeResponse is a oneof.
 */
#define POLL_MAX_STREAMS 32

typedef struct {
    bool          used;
    uint32_t      stream;
    gnmi_path_t   paths[GNMI_MAX_ELEMS];
    uint32_t      path_count;
} poll_stream_t;

static poll_stream_t g_poll_streams[POLL_MAX_STREAMS];
static pthread_mutex_t g_poll_lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds g_poll_lock. */
static poll_stream_t *poll_stream_get(uint32_t stream, bool create)
{
    poll_stream_t *spare = NULL;
    for (int i = 0; i < POLL_MAX_STREAMS; i++) {
        if (g_poll_streams[i].used && g_poll_streams[i].stream == stream)
            return &g_poll_streams[i];
        if (!g_poll_streams[i].used && !spare) spare = &g_poll_streams[i];
    }
    if (!create || !spare) return NULL;
    memset(spare, 0, sizeof(*spare));
    spare->used = true;
    spare->stream = stream;
    return spare;
}

static void poll_stream_drop(uint32_t stream)
{
    pthread_mutex_lock(&g_poll_lock);
    poll_stream_t *ps = poll_stream_get(stream, false);
    if (ps) memset(ps, 0, sizeof(*ps));
    pthread_mutex_unlock(&g_poll_lock);
}

int gnmi_handle_subscribe(void *opaque, uint32_t stream,
                          const uint8_t *req, size_t req_len)
{
    h2_conn_t *c = (h2_conn_t *)opaque;
    gnmi_subscribe_request_t sr;
    if (!gnmi_decode_subscribe_request(req, req_len, &sr)) return -1;

    const gnmi_subscribe_list_t *sl = &sr.subscribe;

    if (sl->mode == GNMI_SUB_MODE_ONCE) {
        poll_stream_drop(stream);
        gnmi_path_t paths[GNMI_MAX_ELEMS];
        uint32_t pc = 0;
        for (uint32_t i = 0; i < sl->sub_count && pc < GNMI_MAX_ELEMS; i++)
            paths[pc++] = sl->subs[i].path;

        uint8_t notif[8192];
        int mlen = build_notification_for_paths(sl, paths, pc,
                                                notif, sizeof(notif));
        if (mlen < 0) return -1;

        uint8_t resp[16384];
        gnmi_pb_t w;
        gnmi_pb_init(&w, resp, sizeof(resp));
        gnmi_pb_put_len_delim(&w, 1, notif, (size_t)mlen);

        if (send_stream_headers(c, stream) < 0) return -1;
        /* update first, then a separate sync_response message: clients
         * (gnmic) exit on sync and would drop a same-message update */
        if (send_stream_message(c, stream, resp, w.len) < 0) return -1;
        gnmi_pb_t sw;
        uint8_t sresp[16];
        gnmi_pb_init(&sw, sresp, sizeof(sresp));
        gnmi_encode_subscribe_sync(&sw);
        if (send_stream_message(c, stream, sresp, sw.len) < 0) return -1;
        if (send_stream_trailers(c, stream) < 0) return -1;
        return 0;
    }

    if (sl->mode == GNMI_SUB_MODE_POLL) {
        gnmi_path_t paths[GNMI_MAX_ELEMS];
        uint32_t pc = 0;
        for (uint32_t i = 0; i < sl->sub_count && pc < GNMI_MAX_ELEMS; i++)
            paths[pc++] = sl->subs[i].path;

        /* A Poll on an established stream answers with a notification only;
         * sync_response is sent once, after the initial set. */
        if (sr.is_poll) {
            pthread_mutex_lock(&g_poll_lock);
            poll_stream_t *ps = poll_stream_get(stream, false);
            bool known = (ps != NULL);
            if (known) {
                memcpy(paths, ps->paths, sizeof(gnmi_path_t) * ps->path_count);
                pc = ps->path_count;
            }
            pthread_mutex_unlock(&g_poll_lock);
            if (!known) return -1;   /* Poll before Subscribe */

            uint8_t notif[8192];
            int mlen = build_notification_for_paths(sl, paths, pc,
                                                    notif, sizeof(notif));
            if (mlen < 0) return -1;
            uint8_t resp[16384];
            gnmi_pb_t w;
            gnmi_pb_init(&w, resp, sizeof(resp));
            gnmi_pb_put_len_delim(&w, 1, notif, (size_t)mlen);
            return send_stream_message(c, stream, resp, w.len) < 0 ? -1 : 0;
        }

        /* Initial Subscribe: register the stream, then send the set and the
         * sync_response as *separate* gRPC messages. SubscribeResponse is a
         * oneof (notification = 1, sync_response = 2), so a message carrying
         * both is invalid; the previous code emitted them together. */
        pthread_mutex_lock(&g_poll_lock);
        poll_stream_t *ps = poll_stream_get(stream, true);
        if (ps) {
            memcpy(ps->paths, paths, sizeof(gnmi_path_t) * pc);
            ps->path_count = pc;
        }
        pthread_mutex_unlock(&g_poll_lock);
        if (!ps) return -1;

        uint8_t notif[8192];
        int mlen = build_notification_for_paths(sl, paths, pc,
                                                notif, sizeof(notif));
        if (mlen < 0) return -1;
        uint8_t resp[16384];
        gnmi_pb_t w;
        gnmi_pb_init(&w, resp, sizeof(resp));
        gnmi_pb_put_len_delim(&w, 1, notif, (size_t)mlen);
        if (send_stream_headers(c, stream) < 0) return -1;
        if (send_stream_message(c, stream, resp, w.len) < 0) return -1;

        uint8_t sresp[16];
        gnmi_pb_t sw;
        gnmi_pb_init(&sw, sresp, sizeof(sresp));
        gnmi_encode_subscribe_sync(&sw);
        if (send_stream_message(c, stream, sresp, sw.len) < 0) return -1;
        return 0;   /* stream stays open for Poll messages */
    }
    return subscribe_stream_loop(c, stream, sl);
}

int gnmi_handle_capabilities(const uint8_t *req, size_t req_len,
                             uint8_t *resp, size_t resp_cap)
{
    (void)req; (void)req_len;   /* CapabilityRequest has only extensions */
    gnmi_pb_t w;
    gnmi_pb_init(&w, resp, resp_cap);

    danos_version_t ver = danos_dpa_get_version();
    char vstr[48];
    snprintf(vstr, sizeof(vstr), "DANOS-Open DPA %u.%u.%u",
             ver.major, ver.minor, ver.patch);

    /* model list sourced from the model registry (single truth) */
    const gnmi_model_data_t *models;
    uint32_t model_count = 0;
    gnmi_model_supported_models(&models, &model_count);
    gnmi_encoding_t encs[] = { GNMI_ENC_JSON, GNMI_ENC_JSON_IETF };
    gnmi_encode_capabilities_response(&w, models, model_count, encs, 2, vstr);
    return w.overflow ? -1 : (int)w.len;
}

static danos_status_t resolve_rpc_path(const gnmi_path_t *, const gnmi_path_t *, gnmi_path_t *);
static danos_status_t path_uint(const char *text, uint32_t *out)
{
    if (!text || !*text || !isdigit((unsigned char)*text)) return DANOS_ERR_INVALID_ARG;
    char *end; errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n > UINT32_MAX) return DANOS_ERR_INVALID_ARG;
    *out = (uint32_t)n;
    return DANOS_OK;
}

typedef struct {
    danos_obj_type_t type;
    size_t elem_size, count, capacity;
    void *data;
    danos_status_t status;
} get_objects_t;

static void get_collect(danos_object_entry_t *e, void *user)
{
    get_objects_t *c = user;
    if (e->type != c->type || c->status != DANOS_OK) return;
    if (e->data_size != c->elem_size) { c->status = DANOS_ERR_INTERNAL; return; }
    if (c->count == c->capacity) {
        size_t cap = c->capacity ? c->capacity * 2 : 16;
        if (cap < c->capacity || cap > SIZE_MAX / c->elem_size) {
            c->status = DANOS_ERR_NO_MEMORY; return;
        }
        void *p = realloc(c->data, cap * c->elem_size);
        if (!p) { c->status = DANOS_ERR_NO_MEMORY; return; }
        c->data = p; c->capacity = cap;
    }
    memcpy((char *)c->data + c->count++ * c->elem_size, e->data, c->elem_size);
}

int gnmi_handle_get(danos_state_store_t *store, const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t resp_cap)
{
    gnmi_get_request_t gr;
    if (!gnmi_decode_get_request(req, req_len, &gr)) return -(int)DANOS_ERR_INVALID_ARG;
    danos_state_store_t *ss = store ? store : store_or_default();
    danos_object_store_t *src = desired_store(ss);
    gnmi_pb_t w; gnmi_pb_init(&w, resp, resp_cap);
    size_t saved = gnmi_pb_begin_nested(&w, 1);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    gnmi_pb_put_uint64(&w, 1, (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
    for (uint32_t i = 0; i < gr.path_count; i++) {
        gnmi_path_t path;
        danos_status_t st = resolve_rpc_path(&gr.prefix, &gr.paths[i], &path);
        if (st != DANOS_OK) return -(int)st;
        const gnmi_path_t *p = &path;
        gnmi_model_binding_t mb;
        gnmi_path_t lookup = path;
        if (lookup.elem_count == 2 && !lookup.elems[1].has_key &&
            ((!strcmp(lookup.elems[0].name, "interfaces") && !strcmp(lookup.elems[1].name, "interface")) ||
             (!strcmp(lookup.elems[0].name, "vrfs") && !strcmp(lookup.elems[1].name, "vrf")) ||
             (!strcmp(lookup.elems[0].name, "routes") && !strcmp(lookup.elems[1].name, "route"))))
            lookup.elem_count = 1;
        if (gnmi_model_resolve(&lookup, &mb) != DANOS_OK) return -(int)DANOS_ERR_NOT_FOUND;
        if (p->elem_count > 1 && p->elems[1].has_key &&
            ((mb.obj_type == DANOS_OBJ_IFACE && (strcmp(p->elems[1].key_name, "name") || p->elems[1].has_extra_key)) ||
             (mb.obj_type == DANOS_OBJ_VRF && (strcmp(p->elems[1].key_name, "id") || p->elems[1].has_extra_key)) ||
             (mb.obj_type == DANOS_OBJ_ROUTE && (strcmp(p->elems[1].key_name, "prefix") ||
               (p->elems[1].has_extra_key && strcmp(p->elems[1].extra_key_name, "vrf"))))))
            return -(int)DANOS_ERR_INVALID_ARG;
        if (p->elem_count > 1 && p->elems[1].has_extra_key) {
            uint32_t vrf;
            if (path_uint(p->elems[1].extra_key_value, &vrf) != DANOS_OK)
                return -(int)DANOS_ERR_INVALID_ARG;
        }
        if (mb.kind == GNMI_MODEL_LEAF) {
            danos_iface_t iface; danos_vrf_t vrf;
            const void *obj = NULL; size_t size = 0;
            if (mb.obj_type == DANOS_OBJ_IFACE && find_iface_by_name(ss, p->elems[1].key_value, &iface)) {
                obj = &iface; size = sizeof(iface);
            } else if (mb.obj_type == DANOS_OBJ_VRF) {
                uint32_t id; size_t n = sizeof(vrf);
                if (path_uint(p->elems[1].key_value, &id) != DANOS_OK) return -(int)DANOS_ERR_INVALID_ARG;
                if (src && danos_object_read(src, DANOS_OBJ_VRF, id, &vrf, &n) == DANOS_OK) {
                    obj = &vrf; size = sizeof(vrf);
                }
            }
            if (!obj) return -(int)DANOS_ERR_NOT_FOUND;
            gnmi_typed_value_t val;
            st = gnmi_model_read_leaf(mb.obj_type, 0, mb.field, obj, size, &val);
            if (st != DANOS_OK) return -(int)st;
            size_t us = gnmi_pb_begin_nested(&w, 4);
            gnmi_encode_path(&w, 1, p); gnmi_encode_typed_value(&w, 3, &val);
            gnmi_pb_end_nested(&w, us);
            continue;
        }
        size_t elem_size = mb.obj_type == DANOS_OBJ_IFACE ? sizeof(danos_iface_t) :
                           mb.obj_type == DANOS_OBJ_VRF ? sizeof(danos_vrf_t) : sizeof(danos_route_t);
        get_objects_t objects = { .type = mb.obj_type, .elem_size = elem_size, .status = DANOS_OK };
        if (src) danos_object_iterate(src, get_collect, &objects);
        if (objects.status != DANOS_OK) { free(objects.data); return -(int)objects.status; }
        for (size_t j = 0; j < objects.count && !w.overflow; j++) {
            const void *obj = (char *)objects.data + j * elem_size;
            char key[64], json[256];
            uint32_t route_vrf = 0;
            if (mb.obj_type == DANOS_OBJ_IFACE) {
                snprintf(key, sizeof(key), "%s", ((const danos_iface_t *)obj)->name);
                obj_to_json(mb.obj_type, obj, elem_size, json, sizeof(json));
            } else if (mb.obj_type == DANOS_OBJ_VRF) {
                snprintf(key, sizeof(key), "%u", ((const danos_vrf_t *)obj)->vrf_id);
                obj_to_json(mb.obj_type, obj, elem_size, json, sizeof(json));
            } else {
                const danos_route_t *r = obj;
                route_vrf = r->vrf_id;
                st = gnmi_format_prefix(&r->prefix, key, sizeof(key));
                if (st == DANOS_OK) st = gnmi_route_to_json_store(src, r, json, sizeof(json));
                if (st != DANOS_OK) { free(objects.data); return -(int)st; }
            }
            if (p->elem_count > 1 && p->elems[1].has_key && strcmp(p->elems[1].key_value, key)) continue;
            if (mb.obj_type == DANOS_OBJ_ROUTE && p->elem_count > 1 && p->elems[1].has_extra_key) {
                uint32_t want;
                if (path_uint(p->elems[1].extra_key_value, &want) != DANOS_OK) {
                    free(objects.data); return -(int)DANOS_ERR_INVALID_ARG;
                }
                if (want != route_vrf) continue;
            }
            gnmi_path_t concrete = *p;
            if (concrete.elem_count == 1) {
                concrete.elem_count = 2;
                snprintf(concrete.elems[1].name, sizeof(concrete.elems[1].name), "%s",
                         mb.obj_type == DANOS_OBJ_IFACE ? "interface" : mb.obj_type == DANOS_OBJ_VRF ? "vrf" : "route");
            }
            gnmi_path_elem_t *entry = &concrete.elems[1];
            entry->has_key = true;
            snprintf(entry->key_name, sizeof(entry->key_name), "%s",
                     mb.obj_type == DANOS_OBJ_IFACE ? "name" : mb.obj_type == DANOS_OBJ_VRF ? "id" : "prefix");
            snprintf(entry->key_value, sizeof(entry->key_value), "%s", key);
            if (mb.obj_type == DANOS_OBJ_ROUTE) {
                entry->has_extra_key = true;
                snprintf(entry->extra_key_name, sizeof(entry->extra_key_name), "vrf");
                snprintf(entry->extra_key_value, sizeof(entry->extra_key_value), "%u", route_vrf);
            }
            size_t us = gnmi_pb_begin_nested(&w, 4);
            gnmi_encode_path(&w, 1, &concrete);
            gnmi_typed_value_t val = { .kind = GNMI_VAL_JSON_IETF };
            snprintf(val.s, sizeof(val.s), "%s", json);
            gnmi_encode_typed_value(&w, 3, &val);
            gnmi_pb_end_nested(&w, us);
        }
        free(objects.data);
    }
    gnmi_pb_end_nested(&w, saved);
    return w.overflow ? -(int)DANOS_ERR_NO_MEMORY : (int)w.len;
}

/* Extract an unsigned field from a flat JSON body */
static const char *json_find(const char *body, const char *key)
{
    char pattern[80];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return NULL;
    const char *p = strstr(body, pattern);
    if (!p) return NULL;
    p += n;
    while (isspace((unsigned char)*p)) p++;
    return *p == ':' ? p + 1 : NULL;
}

static int json_get_str(const char *body, const char *key,
                        char *out, size_t cap)
{
    const char *p = json_find(body, key);
    if (!p) return -1;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '"' || !cap) return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return *p == '"' ? 0 : -1;
}

static int json_get_uint(const char *body, const char *key, unsigned *out)
{
    const char *p = json_find(body, key);
    if (!p) return -1;
    while (isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return -1;
    char *end;
    errno = 0;
    unsigned long value = strtoul(p, &end, 10);
    if (errno || value > UINT_MAX) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != ',' && *end != '}') return -1;
    *out = (unsigned)value;
    return 0;
}

static int json_get_ipv4_array(const char *body, const char *key,
                               danos_ip_addr_t *out, uint32_t *count)
{
    const char *p = json_find(body, key);
    if (!p) return -1;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '[') return -1;
    p++;
    uint32_t n = 0;
    while (*p && *p != ']' && n < 64) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') return -1;
        char ip[64]; size_t i = 0;
        for (p++; *p && *p != '"' && i < sizeof(ip) - 1; p++) ip[i++] = *p;
        ip[i] = '\0';
        if (*p != '"' || gnmi_parse_ipv4(ip, &out[n]) != DANOS_OK) return -1;
        n++; p++;
    }
    if (*p != ']' || n == 0) return -1;
    *count = n;
    return 0;
}

/* Apply one SetRequest to a private transaction in specification order. */
static pthread_mutex_t g_set_lock = PTHREAD_MUTEX_INITIALIZER;

static danos_status_t resolve_rpc_path(const gnmi_path_t *prefix,
                                        const gnmi_path_t *path, gnmi_path_t *out)
{
    if (prefix->elem_count + path->elem_count > GNMI_MAX_ELEMS ||
        (prefix->origin[0] && path->origin[0] && strcmp(prefix->origin, path->origin)))
        return DANOS_ERR_INVALID_ARG;
    *out = *prefix;
    if (path->origin[0]) snprintf(out->origin, sizeof(out->origin), "%s", path->origin);
    for (uint32_t i = 0; i < path->elem_count; i++) out->elems[out->elem_count++] = path->elems[i];
    for (uint32_t i = 0; i < out->elem_count; i++) {
        gnmi_path_elem_t *e = &out->elems[i];
        if (e->has_extra_key && !strcmp(e->name, "route") &&
            !strcmp(e->key_name, "vrf") && !strcmp(e->extra_key_name, "prefix")) {
            char name[64], value[64];
            memcpy(name, e->key_name, sizeof(name));
            memcpy(value, e->key_value, sizeof(value));
            memcpy(e->key_name, e->extra_key_name, sizeof(name));
            memcpy(e->key_value, e->extra_key_value, sizeof(value));
            memcpy(e->extra_key_name, name, sizeof(name));
            memcpy(e->extra_key_value, value, sizeof(value));
        }
    }
    return DANOS_OK;
}

/* Accepted entry JSON is flat and typed. Never silently accept unknown fields,
 * malformed scalars or duplicate keys, especially in a destructive replace. */
static bool flat_json_valid(const char *body, const char *const *keys)
{
    const char *p = body;
    char seen[16][64]; size_t count = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '{') return false;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '}') { p++; break; }
        if (*p++ != '"' || count == 16) return false;
        size_t n = 0;
        while (*p && *p != '"') {
            if (*p == '\\' || (unsigned char)*p < 32 || n == 63) return false;
            seen[count][n++] = *p++;
        }
        if (*p++ != '"') return false;
        seen[count][n] = 0;
        bool known = false;
        for (size_t i = 0; keys[i]; i++) if (!strcmp(keys[i], seen[count])) known = true;
        for (size_t i = 0; i < count; i++) if (!strcmp(seen[i], seen[count])) return false;
        if (!known) return false;
        count++;
        while (isspace((unsigned char)*p)) p++;
        if (*p++ != ':') return false;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (*p == '\\' || (unsigned char)*p < 32) return false; p++; }
            if (*p++ != '"') return false;
        } else if (*p == '[') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            if (*p != ']') for (;;) {
                if (*p++ != '"') return false;
                while (*p && *p != '"') { if (*p == '\\' || (unsigned char)*p < 32) return false; p++; }
                if (*p++ != '"') return false;
                while (isspace((unsigned char)*p)) p++;
                if (*p != ',') break;
                p++;
                while (isspace((unsigned char)*p)) p++;
            }
            if (*p++ != ']') return false;
        } else if (!strncmp(p, "true", 4)) p += 4;
        else if (!strncmp(p, "false", 5)) p += 5;
        else {
            if (!isdigit((unsigned char)*p)) return false;
            if (*p == '0' && isdigit((unsigned char)p[1])) return false;
            while (isdigit((unsigned char)*p)) p++;
        }
        while (isspace((unsigned char)*p)) p++;
        if (*p == '}') { p++; break; }
        if (*p++ != ',') return false;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '}') return false;
    }
    while (isspace((unsigned char)*p)) p++;
    return !*p;
}

static int json_get_bool(const char *body, const char *key, bool *out)
{
    const char *p = json_find(body, key);
    if (!p) return -1;
    while (isspace((unsigned char)*p)) p++;
    size_t n;
    if (!strncmp(p, "true", 4)) { *out = true; n = 4; }
    else if (!strncmp(p, "false", 5)) { *out = false; n = 5; }
    else if (*p == '0' || *p == '1') { *out = *p == '1'; n = 1; }
    else return -1;
    p += n; while (isspace((unsigned char)*p)) p++;
    return *p == ',' || *p == '}' ? 0 : -1;
}

typedef struct { const char *name; danos_iface_t iface; bool found; danos_obj_id_t max; } candidate_iface_t;
static void candidate_iface_cb(danos_object_entry_t *e, void *user)
{
    candidate_iface_t *c = user;
    if (e->id > c->max) c->max = e->id;
    if (e->data_size == sizeof(c->iface) && !strcmp(((danos_iface_t *)e->data)->name, c->name)) {
        c->iface = *(danos_iface_t *)e->data;
        c->found = true;
    }
}
static void reset_iface_config(danos_iface_t *i)
{
    i->mtu = 1500; i->admin_up = true;
    memset(&i->ipv4_address, 0, sizeof(i->ipv4_address));
    memset(&i->ipv6_address, 0, sizeof(i->ipv6_address));
}

static danos_status_t stage_iface_write(danos_tx_t *tx, const gnmi_update_t *u, bool replace)
{
    const gnmi_path_t *p = &u->path;
    gnmi_model_binding_t mb;
    if (gnmi_model_resolve(p, &mb) != DANOS_OK || mb.obj_type != DANOS_OBJ_IFACE ||
        p->elem_count < 2 || p->elem_count > 4 || !p->elems[1].has_key ||
        strcmp(p->elems[1].key_name, "name") || p->elems[1].has_extra_key ||
        (p->elem_count > 2 && !mb.config_tree)) return DANOS_ERR_INVALID_ARG;
    const char *name = p->elems[1].key_value;
    if (!*name || strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != strlen(name))
        return DANOS_ERR_INVALID_ARG;
    candidate_iface_t c = { .name = name };
    danos_status_t st = danos_tx_iterate_objects(tx, DANOS_OBJ_IFACE, candidate_iface_cb, &c);
    if (st != DANOS_OK) return st;
    if (!c.found) {
        if (c.max >= UINT32_MAX) return DANOS_ERR_INVALID_ARG;
        c.iface.ifindex = (danos_ifindex_t)(c.max + 1);
        char *end;
        unsigned long id = strtoul(name, &end, 10);
        if (end != name && !*end) {
            if (!id || id > UINT32_MAX) return DANOS_ERR_INVALID_ARG;
            c.iface.ifindex = (danos_ifindex_t)id;
        }
        snprintf(c.iface.name, sizeof(c.iface.name), "%s", name);
        reset_iface_config(&c.iface);
    }
    if (mb.kind == GNMI_MODEL_LEAF) {
        st = gnmi_model_apply_leaf(DANOS_OBJ_IFACE, mb.field, &c.iface, sizeof(c.iface), &u->val);
        if (st != DANOS_OK) return st;
    } else {
        if (u->val.kind != GNMI_VAL_JSON && u->val.kind != GNMI_VAL_JSON_IETF) return DANOS_ERR_INVALID_ARG;
        static const char *const keys[] = {"name", "ifindex", "mtu", "admin_up", "enabled", "ipv4-address", "ipv6-address", NULL};
        if (!flat_json_valid(u->val.s, keys)) return DANOS_ERR_INVALID_ARG;
        if (replace) reset_iface_config(&c.iface);
        char value[80];
        if (json_find(u->val.s, "name") &&
            (json_get_str(u->val.s, "name", value, sizeof(value)) || strcmp(value, name)))
            return DANOS_ERR_INVALID_ARG;
        unsigned n;
        if (json_find(u->val.s, "ifindex") &&
            (json_get_uint(u->val.s, "ifindex", &n) || n != c.iface.ifindex)) return DANOS_ERR_INVALID_ARG;
        if (json_find(u->val.s, "mtu")) {
            if (json_get_uint(u->val.s, "mtu", &n) || n < 68 || n > 9216) return DANOS_ERR_INVALID_ARG;
            c.iface.mtu = (uint16_t)n;
        }
        if (json_find(u->val.s, "enabled") && json_find(u->val.s, "admin_up")) return DANOS_ERR_INVALID_ARG;
        const char *enabled = json_find(u->val.s, "enabled") ? "enabled" : "admin_up";
        if (json_find(u->val.s, enabled) && json_get_bool(u->val.s, enabled, &c.iface.admin_up)) return DANOS_ERR_INVALID_ARG;
        const char *addresses[] = {"ipv4-address", "ipv6-address"};
        for (size_t i = 0; i < 2; i++) if (json_find(u->val.s, addresses[i])) {
            gnmi_typed_value_t val = { .kind = GNMI_VAL_STRING };
            if (json_get_str(u->val.s, addresses[i], val.s, sizeof(val.s))) return DANOS_ERR_INVALID_ARG;
            st = gnmi_model_apply_leaf(DANOS_OBJ_IFACE, i ? GNMI_FIELD_IPV6_ADDRESS : GNMI_FIELD_IPV4_ADDRESS,
                                       &c.iface, sizeof(c.iface), &val);
            if (st != DANOS_OK) return st;
        }
    }
    return c.found ? danos_iface_update(tx, &c.iface) : danos_iface_create(tx, &c.iface);
}

static danos_status_t stage_route_write(danos_tx_t *tx, const gnmi_update_t *u, bool replace)
{
    if (u->path.elem_count != 2 || !u->path.elems[1].has_key ||
        strcmp(u->path.elems[1].key_name, "prefix") ||
        (u->val.kind != GNMI_VAL_JSON && u->val.kind != GNMI_VAL_JSON_IETF))
        return DANOS_ERR_INVALID_ARG;
    static const char *const keys[] = {"gateway", "gateways", "oif", "vrf", "prefix", NULL};
    if (!flat_json_valid(u->val.s, keys)) return DANOS_ERR_INVALID_ARG;
    danos_ip_prefix_t prefix;
    danos_status_t st = gnmi_parse_prefix(u->path.elems[1].key_value, &prefix);
    if (st != DANOS_OK) return st;
    char value[64];
    if (json_find(u->val.s, "prefix") &&
        (json_get_str(u->val.s, "prefix", value, sizeof(value)) ||
         strcmp(value, u->path.elems[1].key_value))) return DANOS_ERR_INVALID_ARG;
    unsigned vrf = 0, oif = 0;
    const gnmi_path_elem_t *entry = &u->path.elems[1];
    if (entry->has_extra_key &&
        (strcmp(entry->extra_key_name, "vrf") ||
         path_uint(entry->extra_key_value, &vrf) != DANOS_OK)) return DANOS_ERR_INVALID_ARG;
    unsigned keyed_vrf = vrf;
    if ((json_find(u->val.s, "vrf") && json_get_uint(u->val.s, "vrf", &vrf)) ||
        (json_find(u->val.s, "oif") && json_get_uint(u->val.s, "oif", &oif))) return DANOS_ERR_INVALID_ARG;
    if (entry->has_extra_key && vrf != keyed_vrf) return DANOS_ERR_INVALID_ARG;
    bool gateway = json_find(u->val.s, "gateway") != NULL;
    bool gateways = json_find(u->val.s, "gateways") != NULL;
    if (gateway && gateways) return DANOS_ERR_INVALID_ARG;
    danos_ip_addr_t gws[64];
    uint32_t oifs[64], count = 0;
    if (gateway) {
        if (json_get_str(u->val.s, "gateway", value, sizeof(value)) ||
            gnmi_parse_ipv4(value, &gws[0]) != DANOS_OK) return DANOS_ERR_INVALID_ARG;
        count = 1;
    } else if (gateways) {
        if (json_get_ipv4_array(u->val.s, "gateways", gws, &count)) return DANOS_ERR_INVALID_ARG;
    }
    danos_route_t old;
    danos_nhgroup_t grp = {0};
    bool exists = danos_route_read(tx, vrf, prefix, DANOS_ROUTE_PROTO_STATIC, &old) == DANOS_OK;
    if (!replace && exists && danos_nhgroup_read(tx, old.nhgroup_id, &grp) != DANOS_OK)
        return DANOS_ERR_INVALID_ARG;
    if (!count) {
        if (replace || !exists || !grp.nh_count || grp.nh_count > 64) return DANOS_ERR_INVALID_ARG;
        count = grp.nh_count;
        for (uint32_t i = 0; i < count; i++) {
            danos_nexthop_t nh;
            st = danos_nh_read(tx, grp.nh_ids[i], &nh);
            if (st != DANOS_OK) return st;
            gws[i] = nh.gateway;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        oifs[i] = oif;
        if (!replace && !json_find(u->val.s, "oif") && grp.nh_count) {
            danos_nexthop_t nh;
            st = danos_nh_read(tx, grp.nh_ids[i < grp.nh_count ? i : 0], &nh);
            if (st != DANOS_OK) return st;
            oifs[i] = nh.ifindex;
        }
    }
    return gnmi_route_stage_set(tx, vrf, &prefix, gws, oifs, count, replace);
}

static danos_status_t stage_delete(danos_tx_t *tx, const gnmi_path_t *p)
{
    gnmi_model_binding_t mb;
    if (gnmi_model_resolve(p, &mb) != DANOS_OK || p->elem_count < 2) return DANOS_ERR_INVALID_ARG;
    if (mb.obj_type == DANOS_OBJ_ROUTE) {
        if (p->elem_count != 2 || !p->elems[1].has_key ||
            strcmp(p->elems[1].key_name, "prefix")) return DANOS_ERR_INVALID_ARG;
        uint32_t vrf = 0;
        if (p->elems[1].has_extra_key &&
            (strcmp(p->elems[1].extra_key_name, "vrf") ||
             path_uint(p->elems[1].extra_key_value, &vrf) != DANOS_OK)) return DANOS_ERR_INVALID_ARG;
        danos_ip_prefix_t prefix;
        danos_status_t st = gnmi_parse_prefix(p->elems[1].key_value, &prefix);
        return st == DANOS_OK ? gnmi_route_stage_delete(tx, vrf, &prefix) : st;
    }
    if (mb.obj_type != DANOS_OBJ_IFACE || !p->elems[1].has_key ||
        p->elems[1].has_extra_key || strcmp(p->elems[1].key_name, "name") ||
        (p->elem_count > 2 && !mb.config_tree)) return DANOS_ERR_INVALID_ARG;
    candidate_iface_t c = { .name = p->elems[1].key_value };
    danos_status_t st = danos_tx_iterate_objects(tx, DANOS_OBJ_IFACE, candidate_iface_cb, &c);
    if (st != DANOS_OK || !c.found) return st; /* idempotent */
    if (p->elem_count == 2) return danos_iface_delete(tx, c.iface.ifindex);
    if (mb.kind == GNMI_MODEL_ENTRY) reset_iface_config(&c.iface);
    else if (mb.field == GNMI_FIELD_MTU) c.iface.mtu = 1500;
    else if (mb.field == GNMI_FIELD_ENABLED) c.iface.admin_up = true;
    else if (mb.field == GNMI_FIELD_IPV4_ADDRESS) memset(&c.iface.ipv4_address, 0, sizeof(c.iface.ipv4_address));
    else if (mb.field == GNMI_FIELD_IPV6_ADDRESS) memset(&c.iface.ipv6_address, 0, sizeof(c.iface.ipv6_address));
    else return DANOS_ERR_INVALID_ARG;
    return danos_iface_update(tx, &c.iface);
}

int gnmi_handle_set(danos_state_store_t *store, const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t resp_cap)
{
    gnmi_set_request_t *sr = calloc(1, sizeof(*sr));
    gnmi_set_response_t *out = calloc(1, sizeof(*out));
    if (!sr || !out) { free(sr); free(out); return -(int)DANOS_ERR_NO_MEMORY; }
    if (!gnmi_decode_set_request(req, req_len, sr)) {
        free(sr); free(out); return -(int)DANOS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&g_set_lock);
    if (!g_default_store) g_default_store = danos_object_store_create(1024);
    danos_status_t status = g_default_store ? DANOS_OK : DANOS_ERR_NO_MEMORY;
    /* The current DPA transaction engine binds the default store. Never
     * silently mutate it when a caller requested a different desired store. */
    if (status == DANOS_OK && desired_store(store ? store : store_or_default()) != g_default_store)
        status = DANOS_ERR_INVALID_ARG;
    danos_tx_t tx = {0};
    if (status == DANOS_OK) status = danos_tx_begin(&tx, "gnmi-set", NULL);
    for (uint32_t i = 0; i < sr->delete_count && status == DANOS_OK; i++) {
        gnmi_path_t p;
        status = resolve_rpc_path(&sr->prefix, &sr->deletes[i], &p);
        if (status == DANOS_OK) status = stage_delete(&tx, &p);
        if (status == DANOS_OK) gnmi_set_response_add(out, &p, GNMI_OP_DELETE);
    }
    for (unsigned phase = 0; phase < 2 && status == DANOS_OK; phase++) {
        uint32_t count = phase ? sr->update_count : sr->replace_count;
        gnmi_update_t *updates = phase ? sr->updates : sr->replaces;
        for (uint32_t i = 0; i < count && status == DANOS_OK; i++) {
            gnmi_update_t u = updates[i];
            status = resolve_rpc_path(&sr->prefix, &updates[i].path, &u.path);
            gnmi_model_binding_t mb;
            if (status == DANOS_OK && gnmi_model_resolve(&u.path, &mb) != DANOS_OK)
                status = DANOS_ERR_INVALID_ARG;
            if (status == DANOS_OK)
                status = mb.obj_type == DANOS_OBJ_ROUTE ? stage_route_write(&tx, &u, phase == 0)
                                                       : stage_iface_write(&tx, &u, phase == 0);
            if (status == DANOS_OK) gnmi_set_response_add(out, &u.path, phase ? GNMI_OP_UPDATE : GNMI_OP_REPLACE);
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    out->timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    gnmi_pb_t w; gnmi_pb_init(&w, resp, resp_cap);
    /* Response capacity failure must not commit unseen writes. */
    if (status == DANOS_OK && !gnmi_encode_set_response(&w, out)) status = DANOS_ERR_NO_MEMORY;
    if (status == DANOS_OK) status = danos_tx_prepare(&tx);
    if (status == DANOS_OK) status = danos_tx_validate(&tx);
    if (status == DANOS_OK) status = danos_tx_commit(&tx);
    if (status != DANOS_OK && tx.id) danos_tx_abort(&tx);
    int rc = status == DANOS_OK ? (int)w.len : -(int)status;
    pthread_mutex_unlock(&g_set_lock);
    free(sr); free(out);
    return rc;
}

/* =========================================================================
 * HTTP/2 connection handling
 * ========================================================================= */

typedef struct {
    char *path;
    char *method;
    char *content_type;
    char *te;
    char *authorization;
} req_headers_t;

static bool header_cb(const char *name, const char *value, void *user)
{
    req_headers_t *h = user;
    if (strcmp(name, ":path") == 0 && !h->path) h->path = strdup(value);
    else if (strcmp(name, ":method") == 0 && !h->method) h->method = strdup(value);
    else if (strcmp(name, "content-type") == 0 && !h->content_type)
        h->content_type = strdup(value);
    else if (strcmp(name, "te") == 0 && !h->te) h->te = strdup(value);
    else if (strcmp(name, "authorization") == 0 && !h->authorization)
        h->authorization = strdup(value);
    return true;
}

static void headers_free(req_headers_t *h)
{
    free(h->path); free(h->method); free(h->content_type); free(h->te);
    free(h->authorization);
    memset(h, 0, sizeof(*h));
}

/* Read a gRPC length-prefixed message from DATA frames of a stream.
 * Returns message length (>=0, may be 0 for an empty message),
 * -2 if the stream ended before any gRPC frame arrived,
 * -1 on error. */
#define GRPC_READ_NO_MSG (-2)
static int read_grpc_message(h2_conn_t *c, uint32_t stream,
                             uint8_t *msg, size_t cap)
{
    size_t have = 0;      /* bytes of gRPC frame header collected */
    uint8_t ghdr[5];
    bool hdr_done = false;
    size_t need = 0;
    size_t got = 0;
    /* Bytes consumed since the last WINDOW_UPDATE, so we return the credit
     * in batches instead of a frame per DATA frame. */
    uint32_t consumed = 0;

    for (;;) {
        uint8_t ftype, fflags;
        uint32_t fstream;
        uint8_t frame[H2_MAX_FRAME_LEN];
        uint32_t flen = sizeof(frame);
        int rr = recv_frame(c, &ftype, &fflags, &fstream, frame, &flen);
        if (rr < 0) return -1;
        if (rr == 1) continue;
        uint32_t sid = fstream;

        if (ftype == H2_F_HEADERS) {
            if (fflags & H2_FLAG_END_STREAM)
                return hdr_done ? (int)got : GRPC_READ_NO_MSG;
            continue;
        }
        if (ftype == H2_F_GOAWAY || ftype == H2_F_RST) return -1;
        if (ftype != H2_F_DATA || sid != stream) {
            pending_push(c, ftype, fflags, sid, frame, flen);
            continue;
        }

        const uint8_t *p = frame;
        uint32_t n = flen;
        /* Return receive credit for this frame's payload. */
        if (flen) consumed += flen;
        if (consumed >= H2_DEFAULT_WINDOW / 2) {
            send_window_update(c, stream, consumed);
            consumed = 0;
        }
        while (n > 0 || (hdr_done && got == need)) {
            if (!hdr_done) {
                if (n == 0) break;
                ghdr[have++] = *p++;
                n--;
                if (have < 5) continue;
                if (ghdr[0] != 0) return -1;  /* compressed flag */
                need = ((size_t)ghdr[1] << 24) | ((size_t)ghdr[2] << 16) |
                       ((size_t)ghdr[3] << 8) | ghdr[4];
                if (need > cap) return -1;
                hdr_done = true;
                got = 0;
                if (need == 0) return 0;   /* empty message */
                continue;
            }
            if (got == need) {
                if (consumed) { send_window_update(c, stream, consumed); }
                return (int)got;
            }
            if (n == 0) break;
            size_t take = need - got;
            if (take > n) take = n;
            memcpy(msg + got, p, take);
            got += take;
            p += take;
            n -= take;
        }
        if (fflags & H2_FLAG_END_STREAM) {
            if (consumed) send_window_update(c, stream, consumed);
            return hdr_done ? (int)got : GRPC_READ_NO_MSG;
        }
    }
}

static int serve_connection(int fd, SSL *ssl, danos_sec_role_t peer_role)
{
    h2_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = fd;
    c.ssl = ssl;
    c.conn_window = H2_DEFAULT_WINDOW;
    c.peer_initial_window = H2_DEFAULT_WINDOW;
    /* Stream windows are created on demand at peer_initial_window. */
    hpack_dyn_init(&c.dyn_dec);

    /* 1. client preface */
    char preface[H2_PREFACE_LEN];
    if (read_full(&c, preface, H2_PREFACE_LEN) < 0 ||
        memcmp(preface, H2_PREFACE, H2_PREFACE_LEN) != 0) {
        hpack_dyn_free(&c.dyn_dec);
        return -1;
    }

    /* 2. our SETTINGS */
    write_frame(&c, H2_F_SETTINGS, 0, 0, NULL, 0);

    /* subscribe to store mutations (once) for STREAM subscriptions */
    pthread_once(&g_event_subscribe_once, subscribe_store_events_once);

    int rc = 0;
    uint8_t msg[MAX_MSG];

    for (;;) {
        uint8_t ftype, fflags;
        uint32_t fstream;
        uint8_t frame[H2_MAX_FRAME_LEN];
        uint32_t flen = sizeof(frame);
        int rr = recv_frame(&c, &ftype, &fflags, &fstream, frame, &flen);
        if (rr < 0) break;
        if (rr == 1) continue;
        if (ftype == H2_F_GOAWAY) break;
        if (ftype != H2_F_HEADERS) continue;

        if (!(fflags & H2_FLAG_END_HEADERS)) {
            /* CONTINUATION frames follow; fold them in */
            for (;;) {
                uint8_t ctype, cflags;
                uint32_t cstream;
                uint8_t cframe[H2_MAX_FRAME_LEN];
                uint32_t clen = sizeof(cframe);
                int cr = recv_frame(&c, &ctype, &cflags, &cstream, cframe, &clen);
                if (cr < 0) {
                    rc = -1;
                    goto out;
                }
                if (cr == 1) continue;
                if (ctype != H2_F_CONT) {
                    pending_push(&c, ctype, cflags, cstream, cframe, clen);
                    continue;
                }
                if (flen + clen <= sizeof(frame)) {
                    memcpy(frame + flen, cframe, clen);
                    flen += clen;
                }
                if (cflags & H2_FLAG_END_HEADERS) break;
            }
        }

        req_headers_t h;
        memset(&h, 0, sizeof(h));
        if (!hpack_decode(&c.dyn_dec, frame, flen, header_cb, &h)) {
            headers_free(&h);
            rc = -1;
            break;
        }

        atomic_fetch_add_explicit(&g_rpcs_total, 1, memory_order_relaxed);

        bool end_stream = (fflags & H2_FLAG_END_STREAM) != 0;
        size_t msg_len = 0;
        if (!end_stream) {
            int n = read_grpc_message(&c, fstream, msg, sizeof(msg));
            if (n == -1) { headers_free(&h); rc = -1; break; }
            if (n == GRPC_READ_NO_MSG) { headers_free(&h); continue; }
            msg_len = (size_t)n;
        }

        /* Authenticate, then authorize.
         *
         * Once a credential source is configured (bearer token, or
         * SO_PEERCRED on a unix socket) an unauthenticated request is
         * rejected outright. With no source configured the daemon is open,
         * which is only acceptable because configure() then reports false
         * and the startup path is expected to refuse serving. */
        bool rpc_open = (h.path && strcmp(h.path, "/gnmi.gNMI/Capabilities") == 0);
        danos_sec_role_t role = ssl ? peer_role : danos_authz_default_role();
        bool authed = true;
        if (!ssl && !rpc_open && danos_authz_configured()) {
            long uid = -1;
            authed = (danos_authz_authenticate(h.authorization, uid, -1,
                                               &role) == DANOS_AUTH_OK);
            if (!authed) {
                /* grpc-status 16 UNAUTHENTICATED */
                headers_free(&h);
                uint8_t tbuf[96];
                size_t tlen = 0;
                int k = hpack_encode_literal(tbuf, sizeof(tbuf) - tlen,
                                             ":status", "200");
                tlen += k;
                k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                         "content-type", "application/grpc");
                tlen += k;
                k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                         "grpc-status", "16");
                tlen += k;
                write_frame(&c, H2_F_HEADERS,
                            H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS,
                            fstream, tbuf, tlen);
                danos_authz_audit_auth("gRPC", false, "unauthenticated RPC");
                continue;
            }
        }

        /* Authorize before dispatch. Read paths map to READ, everything
         * that can change state to UPDATE; Capabilities is a static
         * description and needs no check. */
        if (!rpc_open &&
            !danos_authz_check(role, DANOS_SEC_OBJ_ALL,
                               (h.path && (strcmp(h.path, "/gnmi.gNMI/Get") == 0 ||
                                           strcmp(h.path, "/gnmi.gNMI/Subscribe") == 0))
                                   ? DANOS_SEC_OP_READ : DANOS_SEC_OP_UPDATE,
                               h.path ? h.path : "")) {
            /* grpc-status 7 PERMISSION_DENIED (DANOS_ERR_PERMISSION) */
            headers_free(&h);
            uint8_t tbuf[96];
            size_t tlen = 0;
            int k = hpack_encode_literal(tbuf, sizeof(tbuf) - tlen,
                                         ":status", "200");
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "content-type", "application/grpc");
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "grpc-status", "7");
            tlen += k;
            write_frame(&c, H2_F_HEADERS,
                        H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS,
                        fstream, tbuf, tlen);
            continue;
        }

        /* dispatch on :path */
        uint8_t resp[MAX_MSG];
        int rlen = -1;
        if (h.path && strcmp(h.path, "/gnmi.gNMI/Capabilities") == 0) {
            rlen = gnmi_handle_capabilities(msg, msg_len, resp, sizeof(resp));
        } else if (h.path && strcmp(h.path, "/gnmi.gNMI/Get") == 0) {
            rlen = end_stream ? -1
                              : gnmi_handle_get(store_or_default(), msg,
                                                msg_len, resp, sizeof(resp));
        } else if (h.path && strcmp(h.path, "/gnmi.gNMI/Set") == 0) {
            rlen = end_stream ? -1
                              : gnmi_handle_set(store_or_default(), msg,
                                                msg_len, resp, sizeof(resp));
        } else if (h.path && strcmp(h.path, "/gnmi.gNMI/Subscribe") == 0) {
            int sret = end_stream ? -1
                       : gnmi_handle_subscribe(&c, fstream, msg, msg_len);
            if (sret == 0) { headers_free(&h); continue; }  /* stream done */
            headers_free(&h);
            rc = -1;
            break;
        } else {
            /* unknown method: grpc-status 12 (unimplemented);
             * trailers-only responses must still carry :status */
            headers_free(&h);
            uint8_t tbuf[96];
            size_t tlen = 0;
            int k = hpack_encode_literal(tbuf, sizeof(tbuf) - tlen,
                                         ":status", "200");
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "content-type", "application/grpc");
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "grpc-status", "12");
            tlen += k;
            write_frame(&c, H2_F_HEADERS,
                        H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS,
                        fstream, tbuf, tlen);
            continue;
        }

        if (rlen >= 0) {
            send_grpc_response(&c, fstream, resp, (size_t)rlen);
        } else {
            /* map DPA status -> gRPC status: NOT_FOUND(2) -> 5,
             * INVALID_ARG(1) -> 3, everything else -> 2 (Unknown) */
            const char *gs = "2";
            const char *gmsg = "internal error";
            if (rlen == -(int)DANOS_ERR_NOT_FOUND) {
                gs = "5"; gmsg = "path not found";
            } else if (rlen == -(int)DANOS_ERR_INVALID_ARG) {
                gs = "3"; gmsg = "invalid argument";
            } else if (rlen == -(int)DANOS_ERR_EXISTS) {
                gs = "6"; gmsg = "already exists";
            }
            uint8_t tbuf[192];
            size_t tlen = 0;
            int k = hpack_encode_literal(tbuf, sizeof(tbuf) - tlen,
                                         ":status", "200");
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "content-type", "application/grpc");
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "grpc-status", gs);
            tlen += k;
            k = hpack_encode_literal(tbuf + tlen, sizeof(tbuf) - tlen,
                                     "grpc-message", gmsg);
            tlen += k;
            write_frame(&c, H2_F_HEADERS,
                        H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS,
                        fstream, tbuf, tlen);
        }
        headers_free(&h);
        /* The response always carries END_STREAM, so the stream is done:
         * release its flow-control slot so a long-lived connection running
         * many sequential RPCs cannot exhaust the table. */
        if (end_stream) stream_forget(&c, fstream);
    }
out:
    pending_free_all(&c);
    hpack_dyn_free(&c.dyn_dec);
    return rc;
}

int danos_gnmi_grpc_serve_fd(int fd)
{
    return serve_connection(fd, NULL, DANOS_ROLE_VIEWER);
}

/* =========================================================================
 * Server lifecycle
 * ========================================================================= */

static struct {
    danos_gnmi_grpc_ctx_t *ctx;
    pthread_t thread;
} g_grpc;

typedef struct grpc_conn_arg {
    int fd;
    danos_gnmi_grpc_ctx_t *ctx;
    struct grpc_conn_arg *next;
} grpc_conn_arg_t;

static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_conn_done = PTHREAD_COND_INITIALIZER;
static grpc_conn_arg_t *g_connections;

static void *grpc_conn_thread(void *argp)
{
    grpc_conn_arg_t *a = argp;
    danos_sec_role_t role = DANOS_ROLE_VIEWER;
    SSL *ssl = a->ctx->tls ? danos_gnmi_tls_accept(a->ctx->tls, a->fd, &role) : NULL;
    if (!a->ctx->tls || ssl) serve_connection(a->fd, ssl, role);
    SSL_free(ssl);
    pthread_mutex_lock(&g_conn_lock);
    grpc_conn_arg_t **p = &g_connections;
    while (*p != a) p = &(*p)->next;
    *p = a->next;
    close(a->fd);
    atomic_fetch_add_explicit(&a->ctx->rpcs_served, 1, memory_order_relaxed);
    pthread_cond_broadcast(&g_conn_done);
    pthread_mutex_unlock(&g_conn_lock);
    free(a);
    return NULL;
}

static void *grpc_accept_loop(void *arg)
{
    danos_gnmi_grpc_ctx_t *ctx = arg;
    while (ctx->running) {
        int fd = accept(ctx->listen_fd, NULL, NULL);
        if (fd < 0) break;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        /* Bound an unauthenticated peer's handshake and idle reads. */
        struct timeval timeout = { .tv_sec = 10 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        grpc_conn_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            close(fd);
            continue;
        }
        a->fd = fd;
        a->ctx = ctx;
        pthread_mutex_lock(&g_conn_lock);
        size_t connections = 0;
        for (grpc_conn_arg_t *p = g_connections; p; p = p->next) connections++;
        if (connections >= 128) {
            pthread_mutex_unlock(&g_conn_lock);
            close(fd);
            free(a);
            continue;
        }
        a->next = g_connections;
        g_connections = a;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, grpc_conn_thread, a) != 0) {
            g_connections = a->next;
            close(fd);
            free(a);
        }
        pthread_mutex_unlock(&g_conn_lock);
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

void danos_gnmi_grpc_init(danos_gnmi_grpc_ctx_t *ctx, uint16_t port)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    ctx->port = port;
}

void danos_gnmi_grpc_set_store(danos_gnmi_grpc_ctx_t *ctx,
                               danos_state_store_t *store)
{
    ctx->store = store;
    g_store = store;
}

void danos_gnmi_grpc_set_tls(danos_gnmi_grpc_ctx_t *ctx, danos_gnmi_tls_t *tls)
{
    ctx->tls = tls;
}

int danos_gnmi_grpc_start(danos_gnmi_grpc_ctx_t *ctx)
{
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ctx->port);
    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(ctx->listen_fd, 64) < 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        return -1;
    }
    ctx->running = true;
    g_grpc.ctx = ctx;
    if (pthread_create(&g_grpc.thread, NULL, grpc_accept_loop, ctx) != 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        ctx->running = false;
        return -1;
    }
    return 0;
}

void danos_gnmi_grpc_stop(danos_gnmi_grpc_ctx_t *ctx)
{
    if (!ctx->running) return;
    ctx->running = false;
    shutdown(ctx->listen_fd, SHUT_RDWR);
    close(ctx->listen_fd);
    pthread_join(g_grpc.thread, NULL);
    pthread_mutex_lock(&g_conn_lock);
    for (grpc_conn_arg_t *a = g_connections; a; a = a->next)
        shutdown(a->fd, SHUT_RDWR);
    while (g_connections) pthread_cond_wait(&g_conn_done, &g_conn_lock);
    pthread_mutex_unlock(&g_conn_lock);
    ctx->listen_fd = -1;
}
