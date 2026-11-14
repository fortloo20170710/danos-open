/*
 * DANOS-Open VPP Backend: Binary API Client (v0.3)
 *
 * Real implementation of the VPP binary API socket client:
 *   - Connect to VPP's AF_UNIX SOCK_STREAM binary API socket
 *     (default /run/vpp/api.sock).
 *   - Perform the legacy socket handshake: send sockclnt_create
 *     (fixed msg_id 0xF), receive sockclnt_create_reply which carries
 *     the client index and the full name->msg_id message table.
 *   - Send/receive length-framed big-endian messages with context
 *     correlation (danos_vpp_api_transact).
 *
 * Mock mode (danos_vpp_api_enable_mock) is retained for unit tests
 * and environments without VPP.
 *
 * Wire format reference: FDio VPP src/vlibmemory/memclnt.api and
 * src/vlibmemory/socket.c (see also vpp_wire.h).
 */

#include "vpp_wire.h"
#include "vpp_api.h"
#include <danos/dpa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>

#define VPP_API_SOCK_PATH "/run/vpp/api.sock"
#define VPP_STAT_SOCK_PATH "/run/vpp/stats.sock"
#define VPP_RECONNECT_INITIAL_MS 500
#define VPP_RECONNECT_MAX_MS    30000
#define VPP_RECV_TIMEOUT_MS     5000
#define VPP_CLIENT_NAME "danos-open"

typedef enum {
    VPP_API_DISCONNECTED = 0,
    VPP_API_CONNECTING   = 1,
    VPP_API_CONNECTED    = 2,
    VPP_API_MOCK         = 3,  /* mock mode (no VPP) */
} vpp_api_state_t;

typedef struct {
    int msg_fd;          /* binary API socket */
    int stat_fd;         /* stat segment socket (SOCK_SEQPACKET) */
    vpp_api_state_t state;
    char api_sock_path[256];
    char stat_sock_path[256];
    uint64_t last_connect_ns;
    uint32_t reconnect_delay_ms;
    uint64_t msgs_sent;
    uint64_t msgs_received;
    uint64_t connect_count;
    uint64_t reconnect_count;
    uint32_t context;        /* monotonically increasing request context */
    uint32_t client_index;   /* assigned by VPP in sockclnt_create_reply */
    vpp_msg_table_t msg_table;
    vpp_msg_table_t schema_table;
    /* Mock mode: record last message */
    uint16_t last_msg_id;
    uint32_t last_msg_size;
} vpp_api_ctx_impl_t;

/* Mock stat segment: named counters */
#define VPP_MOCK_STAT_MAX 64
typedef struct {
    char    name[128];
    uint64_t value;
    bool    used;
} vpp_mock_stat_t;

static vpp_api_ctx_impl_t g_ctx;
static vpp_mock_stat_t    g_mock_stats[VPP_MOCK_STAT_MAX];
#define VPP_MOCK_DUMP_MAX 16
typedef struct {
    char     detail_name[64];
    uint8_t *body;
    uint32_t len;
} vpp_mock_dump_t;
static vpp_mock_dump_t g_mock_dumps[VPP_MOCK_DUMP_MAX];
static uint32_t g_mock_dump_count;

static bool g_initialized = false;
static uint32_t g_io_timeout_ms = VPP_RECV_TIMEOUT_MS;

/* The binary API is one shared socket with one request context counter, and
 * the daemon queries it from several threads (backend worker, health and CLI
 * paths). Serialise every entry point on one mutex. It is recursive because
 * the public calls nest: transact -> send/recv, reconnect -> connect. */
static pthread_mutex_t g_ctx_lock;
static pthread_once_t g_ctx_lock_once = PTHREAD_ONCE_INIT;

static void vpp_ctx_lock_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return;
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&g_ctx_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void vpp_ctx_lock(void)
{
    pthread_once(&g_ctx_lock_once, vpp_ctx_lock_init);
    pthread_mutex_lock(&g_ctx_lock);
}

static void vpp_ctx_unlock(void)
{
    pthread_mutex_unlock(&g_ctx_lock);
}

static void vpp_strip_crc_suffix(char *name)
{
    size_t n = strlen(name);
    if (n < 9 || name[n - 9] != '_') return;
    for (size_t i = n - 8; i < n; i++)
        if (!isxdigit((unsigned char)name[i])) return;
    name[n - 9] = 0;
}


static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int danos_vpp_api_set_io_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms == 0 || timeout_ms > 60000) return -1;
    vpp_ctx_lock();
    g_io_timeout_ms = timeout_ms;
    vpp_ctx_unlock();
    return 0;
}

/* =========================================================================
 * Initialization and connection
 * ========================================================================= */

int danos_vpp_api_init(void)
{
    vpp_ctx_lock();
    if (g_initialized) danos_vpp_api_disconnect();
    g_io_timeout_ms = VPP_RECV_TIMEOUT_MS;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.msg_fd = -1;
    g_ctx.stat_fd = -1;
    g_ctx.state = VPP_API_DISCONNECTED;
    snprintf(g_ctx.api_sock_path, sizeof(g_ctx.api_sock_path), "%s", VPP_API_SOCK_PATH);
    snprintf(g_ctx.stat_sock_path, sizeof(g_ctx.stat_sock_path), "%s", VPP_STAT_SOCK_PATH);
    const char *api_env = getenv("VPP_API_SOCK");
    const char *stat_env = getenv("VPP_STAT_SOCK");
    if (api_env && *api_env)
        snprintf(g_ctx.api_sock_path, sizeof(g_ctx.api_sock_path), "%s", api_env);
    if (stat_env && *stat_env)
        snprintf(g_ctx.stat_sock_path, sizeof(g_ctx.stat_sock_path), "%s", stat_env);
    g_ctx.reconnect_delay_ms = VPP_RECONNECT_INITIAL_MS;
    vpp_msg_table_init(&g_ctx.msg_table);
    vpp_msg_table_init(&g_ctx.schema_table);
    danos_vpp_api_mock_dump_clear();
    g_initialized = true;
    vpp_ctx_unlock();
    return 0;
}

void danos_vpp_api_enable_mock(void)
{
    vpp_ctx_lock();
    if (!g_initialized) danos_vpp_api_init();
    g_ctx.state = VPP_API_MOCK;
    g_ctx.msg_fd = -1;
    g_ctx.stat_fd = -1;
    vpp_ctx_unlock();
}

void danos_vpp_api_mock_dump_add(const char *detail_name,
                                 const void *body, uint32_t len)
{
    if (!detail_name || !body || len == 0) return;
    vpp_ctx_lock();
    if (g_mock_dump_count >= VPP_MOCK_DUMP_MAX) {
        vpp_ctx_unlock();
        return;
    }
    vpp_mock_dump_t *d = &g_mock_dumps[g_mock_dump_count];
    snprintf(d->detail_name, sizeof(d->detail_name), "%s", detail_name);
    d->body = malloc(len);
    if (!d->body) {
        vpp_ctx_unlock();
        return;
    }
    memcpy(d->body, body, len);
    d->len = len;
    g_mock_dump_count++;
    vpp_ctx_unlock();
}

void danos_vpp_api_mock_dump_clear(void)
{
    vpp_ctx_lock();
    for (uint32_t i = 0; i < g_mock_dump_count; i++)
        free(g_mock_dumps[i].body);
    memset(g_mock_dumps, 0, sizeof(g_mock_dumps));
    g_mock_dump_count = 0;
    vpp_ctx_unlock();
}

uint32_t danos_vpp_api_mock_dump_total(void)
{
    vpp_ctx_lock();
    uint32_t n = g_mock_dump_count;
    vpp_ctx_unlock();
    return n;
}

static int connect_unix_stream(const char *path, uint64_t deadline_ms)
{
    if (strncmp(path, "tcp://", 6) == 0) {
        const char *host = path + 6, *colon = strrchr(host, ':');
        if (!colon || colon == host || !colon[1]) return -1;
        char name[128], service[16];
        size_t n = (size_t)(colon - host);
        if (n >= sizeof(name) || strlen(colon + 1) >= sizeof(service)) return -1;
        memcpy(name, host, n); name[n] = 0;
        snprintf(service, sizeof(service), "%s", colon + 1);
        /* Numeric relay endpoints avoid unbounded resolver calls under the
         * API mutex. Configure a Unix socket or tcp://IP:port. */
        struct addrinfo hints = { .ai_socktype = SOCK_STREAM,
                                  .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV }, *res = NULL;
        if (getaddrinfo(name, service, &hints, &res) != 0) return -1;
        int fd = -1;
        for (struct addrinfo *it = res; it; it = it->ai_next) {
            fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
            if (fd >= 0 && vpp_wire_connect_until(fd, it->ai_addr, it->ai_addrlen, deadline_ms) == 0) break;
            if (fd >= 0) { close(fd); fd = -1; }
        }
        freeaddrinfo(res);
        return fd;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (vpp_wire_connect_until(fd, (struct sockaddr *)&addr, sizeof(addr), deadline_ms) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Perform the legacy socket handshake:
 * send sockclnt_create, parse sockclnt_create_reply, build the
 * name->msg_id table. Returns 0 on success. */
static int vpp_handshake(uint64_t deadline_ms)
{
    vpp_buf_t body;
    vpp_buf_init(&body, 96);
    /* VPP's socket registration schema is context(u32), name[64]. Unlike
     * regular API `string` fields, this legacy control message uses a fixed
     * 64-byte name array. A length-prefixed string produces a 22-byte frame,
     * while VPP's generated size check requires 70 bytes (2-byte msg id +
     * 4-byte context + 64-byte name). */
    vpp_buf_put_u32(&body, 0xCEFAEDFEu);
    uint8_t client_name[64] = {0};
    size_t client_name_len = strlen(VPP_CLIENT_NAME);
    if (client_name_len >= sizeof(client_name)) {
        vpp_buf_free(&body);
        return -1;
    }
    memcpy(client_name, VPP_CLIENT_NAME, client_name_len);
    vpp_buf_put_bytes(&body, client_name, sizeof(client_name));

    int rc = vpp_wire_send_until(g_ctx.msg_fd, VPP_MSG_ID_SOCKCLNT_CREATE,
                                 body.data, body.len, deadline_ms);
    vpp_buf_free(&body);
    if (rc != 0) return -1;

    /* VPP 26.10 advertises ~2000 messages; the table is >128 KiB. */
    uint8_t rbuf[512 * 1024];
    int n = vpp_wire_recv_until(g_ctx.msg_fd, rbuf, sizeof(rbuf), deadline_ms);
    if (n <= 2) return -1;

    /* rbuf[0..1] is the 16-bit frame length prefix; the reader must stop
     * at n-2, exactly like the transact path below. */
    uint32_t frame_len = (uint32_t)n - 2;
    uint16_t reply_id = ((uint16_t)rbuf[0] << 8) | rbuf[1];
    /* VPP assigns socket-control reply IDs from the runtime API table; they
     * are not necessarily request_id + 1 (26.10 returns a dynamic ID). */
    if (reply_id == 0) return -1;

    /* sockclnt_create_reply:
     *   u32 client_index; u32 context; i32 response; u32 index;
     *   u16 count; message_table[count] { u16 msg_id; string name; } */
    vpp_reader_t r;
    vpp_reader_init(&r, rbuf + 2, frame_len);
    (void)vpp_rd_u32(&r);             /* client_index is unused for sockets */
    (void)vpp_rd_u32(&r);   /* context */
    int32_t  response = (int32_t)vpp_rd_u32(&r);
    uint32_t index = vpp_rd_u32(&r); /* socket registration handle */
    uint16_t count = vpp_rd_u16(&r);
    if (!vpp_reader_ok(&r) || response != 0) return -1;

    vpp_msg_table_init(&g_ctx.msg_table);
    vpp_msg_table_init(&g_ctx.schema_table);
    for (uint16_t i = 0; i < count; i++) {
        uint16_t msg_id = vpp_rd_u16(&r);
        /* sockclnt_create_reply's message_table_entry is { u16 index;
         * u8 name[64]; }. It is fixed-width, unlike ordinary API strings. */
        char name[65] = {0};
        if (!vpp_rd_bytes(&r, name, 64) || !vpp_reader_ok(&r)) return -1;
        name[64] = '\0';
        if (name[0] && !vpp_msg_table_add(&g_ctx.schema_table, name, msg_id)) return -1;
        vpp_strip_crc_suffix(name);
        if (name[0] != '\0') vpp_msg_table_add(&g_ctx.msg_table, name, msg_id);
    }

    g_ctx.client_index = index;
    return 0;
}

int danos_vpp_api_connect(void)
{
    vpp_ctx_lock();
    if (!g_initialized) danos_vpp_api_init();
    if (g_ctx.state == VPP_API_MOCK) {
        vpp_ctx_unlock();
        return 0;
    }

    if (g_ctx.msg_fd >= 0) danos_vpp_api_disconnect();
    uint64_t deadline_ms = vpp_wire_now_ms() + g_io_timeout_ms;
    g_ctx.msg_fd = connect_unix_stream(g_ctx.api_sock_path, deadline_ms);
    if (g_ctx.msg_fd < 0) {
        g_ctx.state = VPP_API_DISCONNECTED;
        vpp_ctx_unlock();
        return -1;
    }

    if (vpp_handshake(deadline_ms) != 0) {
        close(g_ctx.msg_fd);
        g_ctx.msg_fd = -1;
        g_ctx.state = VPP_API_DISCONNECTED;
        vpp_ctx_unlock();
        return -1;
    }

    g_ctx.state = VPP_API_CONNECTED;
    g_ctx.last_connect_ns = now_ns();
    g_ctx.reconnect_delay_ms = VPP_RECONNECT_INITIAL_MS;
    g_ctx.connect_count++;
    vpp_ctx_unlock();
    return 0;
}

void danos_vpp_api_disconnect(void)
{
    vpp_ctx_lock();
    if (g_ctx.msg_fd >= 0) {
        close(g_ctx.msg_fd);
        g_ctx.msg_fd = -1;
    }
    if (g_ctx.stat_fd >= 0) {
        close(g_ctx.stat_fd);
        g_ctx.stat_fd = -1;
    }
    g_ctx.state = VPP_API_DISCONNECTED;
    vpp_ctx_unlock();
}

int danos_vpp_api_reconnect(void)
{
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        vpp_ctx_unlock();
        return 0;
    }

    uint64_t now = now_ns();
    uint64_t elapsed = (now - g_ctx.last_connect_ns) / 1000000ULL;
    if (elapsed < g_ctx.reconnect_delay_ms) {
        vpp_ctx_unlock();
        return -1;
    }

    g_ctx.reconnect_count++;
    danos_vpp_api_disconnect();
    if (danos_vpp_api_connect() == 0) {
        vpp_ctx_unlock();
        return 0;
    }

    g_ctx.reconnect_delay_ms *= 2;
    if (g_ctx.reconnect_delay_ms > VPP_RECONNECT_MAX_MS)
        g_ctx.reconnect_delay_ms = VPP_RECONNECT_MAX_MS;
    g_ctx.last_connect_ns = now;
    vpp_ctx_unlock();
    return -1;
}

/* =========================================================================
 * Message send/receive and transactions
 * ========================================================================= */

static int api_send_until(uint16_t msg_id, const void *payload, uint32_t payload_size,
                           uint64_t deadline_ms)
{
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        g_ctx.last_msg_id = msg_id;
        g_ctx.last_msg_size = payload_size;
        g_ctx.msgs_sent++;
        vpp_ctx_unlock();
        return 0;
    }

    if (g_ctx.state != VPP_API_CONNECTED || g_ctx.msg_fd < 0 ||
        (!payload && payload_size > 0)) {
        vpp_ctx_unlock();
        return -1;
    }

    /* Every request starts with client_index + context (big-endian),
     * followed by message-specific fields. The caller-supplied payload
     * must NOT include them; we prepend them here. */
    vpp_buf_t body;
    vpp_buf_init(&body, payload_size + 8);
    vpp_buf_put_u32(&body, g_ctx.client_index);
    g_ctx.context++;
    vpp_buf_put_u32(&body, g_ctx.context);
    vpp_buf_put_bytes(&body, payload, payload_size);

    int rc = vpp_wire_send_until(g_ctx.msg_fd, msg_id, body.data, body.len, deadline_ms);
    vpp_buf_free(&body);
    if (rc != 0) {
        danos_vpp_api_disconnect();
        vpp_ctx_unlock();
        return -1;
    }
    g_ctx.msgs_sent++;
    vpp_ctx_unlock();
    return 0;
}

static int api_recv_until(uint8_t *buf, uint32_t buf_size, uint64_t deadline_ms)
{
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        if (buf_size < 6) {
            vpp_ctx_unlock();
            return -1;
        }
        memset(buf, 0, 6);
        g_ctx.msgs_received++;
        vpp_ctx_unlock();
        return 6;
    }

    if (g_ctx.state != VPP_API_CONNECTED || g_ctx.msg_fd < 0) {
        vpp_ctx_unlock();
        return -1;
    }

    /* Return the API payload (msg_id + struct); the msgbuf header is consumed. */
    int n = vpp_wire_recv_until(g_ctx.msg_fd, buf, buf_size, deadline_ms);
    if (n <= 2) {
        danos_vpp_api_disconnect();
        vpp_ctx_unlock();
        return -1;
    }
    g_ctx.msgs_received++;
    vpp_ctx_unlock();
    return n;
}

int danos_vpp_api_transact(uint16_t msg_id, const uint8_t *payload,
                           uint32_t payload_size, uint8_t *reply,
                           uint32_t reply_size)
{
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        g_ctx.last_msg_id = msg_id;
        g_ctx.last_msg_size = payload_size;
        g_ctx.msgs_sent++;
        if (reply && reply_size >= 10) {
            /* well-formed reply: [msg_id BE][context BE][retval 0] */
            g_ctx.context++;
            uint32_t ctx = g_ctx.context;
            reply[0] = (uint8_t)(msg_id >> 8);
            reply[1] = (uint8_t)msg_id;
            reply[2] = (uint8_t)(ctx >> 24); reply[3] = (uint8_t)(ctx >> 16);
            reply[4] = (uint8_t)(ctx >> 8);  reply[5] = (uint8_t)ctx;
            reply[6] = 0; reply[7] = 0; reply[8] = 0; reply[9] = 0;
            vpp_ctx_unlock();
            return 10;
        }
        vpp_ctx_unlock();
        return 0;
    }
    uint64_t deadline_ms = vpp_wire_now_ms() + g_io_timeout_ms;
    if (api_send_until(msg_id, payload, payload_size, deadline_ms) != 0) {
        vpp_ctx_unlock();
        return -1;
    }
    uint8_t rbuf[64 * 1024];
    for (int attempt = 0; attempt < 64; attempt++) {
        int n = api_recv_until(rbuf, sizeof(rbuf), deadline_ms);
        if (n < 6) break;
        vpp_reader_t r;
        vpp_reader_init(&r, rbuf + 2, (uint32_t)n - 2);
        if (vpp_rd_u32(&r) != g_ctx.context) continue;
        if (reply && (uint32_t)n > reply_size) break; /* never return a truncated frame */
        if (reply) memcpy(reply, rbuf, (size_t)n);
        vpp_ctx_unlock();
        return n;
    }
    danos_vpp_api_disconnect();
    vpp_ctx_unlock();
    return -1;
}

int danos_vpp_api_send(uint16_t msg_id, const void *payload, uint32_t size)
{
    vpp_ctx_lock();
    int rc = api_send_until(msg_id, payload, size, vpp_wire_now_ms() + g_io_timeout_ms);
    vpp_ctx_unlock();
    return rc;
}

int danos_vpp_api_recv(uint8_t *buf, uint32_t size)
{
    vpp_ctx_lock();
    int rc = api_recv_until(buf, size, vpp_wire_now_ms() + g_io_timeout_ms);
    vpp_ctx_unlock();
    return rc;
}

static int api_dump_ids(uint16_t msg_id, const uint8_t *payload, uint32_t size,
                       uint16_t details_id, danos_vpp_dump_cb cb, void *user)
{
    vpp_ctx_lock();
    uint16_t ping, ping_reply;
    if (g_ctx.state == VPP_API_MOCK || !cb ||
        !danos_vpp_api_lookup_msg_id("control_ping", &ping) ||
        !danos_vpp_api_lookup_msg_id("control_ping_reply", &ping_reply)) {
        vpp_ctx_unlock();
        return -1;
    }
    uint64_t deadline = vpp_wire_now_ms() + g_io_timeout_ms;
    if (api_send_until(msg_id, payload, size, deadline) != 0) goto fail;
    uint32_t dump_ctx = g_ctx.context;
    if (api_send_until(ping, NULL, 0, deadline) != 0) goto fail;
    uint32_t ping_ctx = g_ctx.context;
    uint8_t buf[64 * 1024];
    for (unsigned i = 0; i < 4096; i++) {
        int n = api_recv_until(buf, sizeof(buf), deadline);
        if (n < 6) goto fail;
        uint16_t id = ((uint16_t)buf[0] << 8) | buf[1];
        vpp_reader_t r;
        vpp_reader_init(&r, buf + 2, (uint32_t)n - 2);
        uint32_t ctx = vpp_rd_u32(&r);
        if (ctx == ping_ctx && id == ping_reply) {
            if (n < 10 || (int32_t)vpp_rd_u32(&r) != 0) goto fail;
            vpp_ctx_unlock();
            return 0;
        }
        if (ctx == dump_ctx && id == details_id && cb(buf, (uint32_t)n, user) != 0)
            goto fail;
    }
fail:
    danos_vpp_api_disconnect();
    vpp_ctx_unlock();
    return -1;
}

/* Keep the mainline body-callback API while sharing the bounded transport. */
struct body_dump_bridge { danos_vpp_dump_cb cb; void *user; int entries; };
static int body_dump_frame(const uint8_t *frame, uint32_t size, void *user)
{
    struct body_dump_bridge *bridge = user;
    if (size < 6 || bridge->cb(frame + 2, size - 2, bridge->user) != 0) return -1;
    bridge->entries++;
    return 0;
}
int danos_vpp_api_dump(const char *request_name, const char *detail_name,
                       const uint8_t *payload, uint32_t size,
                       danos_vpp_dump_cb cb, void *user)
{
    if (!request_name || !detail_name || !cb) return -1;
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        int entries = 0;
        for (uint32_t i = 0; i < g_mock_dump_count; i++) {
            if (strcmp(g_mock_dumps[i].detail_name, detail_name)) continue;
            if (cb(g_mock_dumps[i].body, g_mock_dumps[i].len, user) != 0) {
                vpp_ctx_unlock(); return -1;
            }
            entries++;
        }
        vpp_ctx_unlock(); return entries;
    }
    uint16_t request, detail;
    if (!danos_vpp_api_lookup_msg_id(request_name, &request) ||
        !danos_vpp_api_lookup_msg_id(detail_name, &detail)) {
        vpp_ctx_unlock(); return -1;
    }
    struct body_dump_bridge bridge = { .cb = cb, .user = user };
    int rc = api_dump_ids(request, payload, size, detail, body_dump_frame, &bridge);
    vpp_ctx_unlock();
    return rc == 0 ? bridge.entries : -1;
}

int danos_vpp_api_query(const char *request_schema, const char *reply_schema,
                        const uint8_t *payload, uint32_t size,
                        uint8_t *reply, uint32_t capacity)
{
    vpp_ctx_lock();
    uint16_t request, expected;
    int n;
    if (g_ctx.state != VPP_API_CONNECTED) n = -1;
    else if (!danos_vpp_api_lookup_schema(request_schema, &request) ||
             !danos_vpp_api_lookup_schema(reply_schema, &expected)) n = -2;
    else {
        n = danos_vpp_api_transact(request, payload, size, reply, capacity);
        if (n < 2 || !reply || (((uint16_t)reply[0] << 8) | reply[1]) != expected) {
            danos_vpp_api_disconnect();
            n = -1;
        }
    }
    vpp_ctx_unlock();
    return n;
}
int danos_vpp_api_dump_query(const char *request_schema, const char *detail_schema,
                             const uint8_t *payload, uint32_t size,
                             danos_vpp_dump_cb cb, void *user)
{
    vpp_ctx_lock();
    uint16_t request, detail;
    int rc;
    if (g_ctx.state != VPP_API_CONNECTED) rc = -1;
    else if (!danos_vpp_api_lookup_schema(request_schema, &request) ||
             !danos_vpp_api_lookup_schema(detail_schema, &detail)) rc = -2;
    else rc = api_dump_ids(request, payload, size, detail, cb, user);
    vpp_ctx_unlock();
    return rc;
}

/* Resolve a message name to its dynamically assigned msg_id using the
 * table returned during the handshake. */
bool danos_vpp_api_lookup_msg_id(const char *name, uint16_t *msg_id)
{
    vpp_ctx_lock();
    bool found;
    if (g_ctx.state == VPP_API_MOCK) {
        if (msg_id) *msg_id = 0;
        found = true;
    } else {
        found = vpp_msg_table_lookup(&g_ctx.msg_table, name, msg_id);
    }
    vpp_ctx_unlock();
    return found;
}

bool danos_vpp_api_lookup_schema(const char *name_crc, uint16_t *msg_id)
{
    vpp_ctx_lock();
    bool found = g_ctx.state == VPP_API_CONNECTED &&
                 vpp_msg_table_lookup(&g_ctx.schema_table, name_crc, msg_id);
    vpp_ctx_unlock();
    return found;
}

uint32_t danos_vpp_api_client_index(void)
{
    vpp_ctx_lock();
    uint32_t index = g_ctx.client_index;
    vpp_ctx_unlock();
    return index;
}

uint32_t danos_vpp_api_msg_table_count(void)
{
    vpp_ctx_lock();
    uint32_t count = vpp_msg_table_count(&g_ctx.msg_table);
    vpp_ctx_unlock();
    return count;
}

const char *danos_vpp_api_sock_path(void)
{
    vpp_ctx_lock();
    const char *path = g_ctx.api_sock_path;
    vpp_ctx_unlock();
    return path;
}

void danos_vpp_api_set_sock_path(const char *path)
{
    if (!path) return;
    vpp_ctx_lock();
    snprintf(g_ctx.api_sock_path, sizeof(g_ctx.api_sock_path), "%s", path);
    vpp_ctx_unlock();
}

void danos_vpp_api_set_stat_sock_path(const char *path)
{
    if (!path) return;
    vpp_ctx_lock();
    snprintf(g_ctx.stat_sock_path, sizeof(g_ctx.stat_sock_path), "%s", path);
    vpp_ctx_unlock();
    /* keep the stat client in sync */
    extern void danos_vpp_stat_set_sock_path(const char *path);
    danos_vpp_stat_set_sock_path(path);
}

/* =========================================================================
 * Stat segment
 * ========================================================================= */

/* The real stat segment client (SOCK_SEQPACKET + SCM_RIGHTS + mmap)
 * lives in vpp_stat.c; the query entry point keeps this name for
 * compatibility. */
int danos_vpp_stat_connect(void);
void danos_vpp_stat_disconnect(void);
uint64_t danos_vpp_stat_query_real(const char *name);
int danos_vpp_stat_list_real(const char **names, uint64_t *values, int max_entries);

int danos_vpp_api_connect_stat(void)
{
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        vpp_ctx_unlock();
        return 0;
    }
    /* The stat segment is independent of the binary API channel. */
    int rc = danos_vpp_stat_connect();
    vpp_ctx_unlock();
    return rc;
}

uint64_t danos_vpp_api_stat_query(const char *name)
{
    if (!name) return 0;
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        for (int i = 0; i < VPP_MOCK_STAT_MAX; i++) {
            if (g_mock_stats[i].used &&
                strcmp(g_mock_stats[i].name, name) == 0) {
                uint64_t v = g_mock_stats[i].value;
                vpp_ctx_unlock();
                return v;
            }
        }
        uint64_t h = 0;
        for (const char *p = name; *p; p++) {
            h = h * 31 + (uint64_t)(unsigned char)*p;
        }
        vpp_ctx_unlock();
        return h;
    }
    uint64_t value = danos_vpp_stat_query_real(name);
    vpp_ctx_unlock();
    return value;
}

int danos_vpp_api_stat_list(const char **names, uint64_t *values, int max_entries)
{
    vpp_ctx_lock();
    if (g_ctx.state == VPP_API_MOCK) {
        int count = 0;
        for (int i = 0; i < VPP_MOCK_STAT_MAX && count < max_entries; i++) {
            if (g_mock_stats[i].used) {
                if (names)  names[count] = g_mock_stats[i].name;
                if (values) values[count] = g_mock_stats[i].value;
                count++;
            }
        }
        vpp_ctx_unlock();
        return count;
    }
    int count = danos_vpp_stat_list_real(names, values, max_entries);
    vpp_ctx_unlock();
    return count;
}

void danos_vpp_api_stat_set(const char *name, uint64_t value)
{
    if (!name) return;
    vpp_ctx_lock();
    for (int i = 0; i < VPP_MOCK_STAT_MAX; i++) {
        if (g_mock_stats[i].used &&
            strcmp(g_mock_stats[i].name, name) == 0) {
            g_mock_stats[i].value = value;
            vpp_ctx_unlock();
            return;
        }
    }
    for (int i = 0; i < VPP_MOCK_STAT_MAX; i++) {
        if (!g_mock_stats[i].used) {
            snprintf(g_mock_stats[i].name, sizeof(g_mock_stats[i].name), "%s", name);
            g_mock_stats[i].value = value;
            g_mock_stats[i].used = true;
            break;
        }
    }
    vpp_ctx_unlock();
}

void danos_vpp_api_stat_reset(void)
{
    vpp_ctx_lock();
    memset(g_mock_stats, 0, sizeof(g_mock_stats));
    vpp_ctx_unlock();
}

/* =========================================================================
 * Status and diagnostics
 * ========================================================================= */

vpp_api_state_t danos_vpp_api_get_state(void)
{
    vpp_ctx_lock();
    vpp_api_state_t state = g_ctx.state;
    vpp_ctx_unlock();
    return state;
}

bool danos_vpp_api_is_connected(void)
{
    vpp_ctx_lock();
    bool up = g_ctx.state == VPP_API_CONNECTED || g_ctx.state == VPP_API_MOCK;
    vpp_ctx_unlock();
    return up;
}

bool danos_vpp_api_is_mock(void)
{
    vpp_ctx_lock();
    bool mock = g_ctx.state == VPP_API_MOCK;
    vpp_ctx_unlock();
    return mock;
}

void danos_vpp_api_get_stats(uint64_t *msgs_sent, uint64_t *msgs_received,
                              uint64_t *connect_count, uint64_t *reconnect_count)
{
    vpp_ctx_lock();
    if (msgs_sent)        *msgs_sent = g_ctx.msgs_sent;
    if (msgs_received)    *msgs_received = g_ctx.msgs_received;
    if (connect_count)    *connect_count = g_ctx.connect_count;
    if (reconnect_count)  *reconnect_count = g_ctx.reconnect_count;
    vpp_ctx_unlock();
}

void danos_vpp_api_get_last_mock_msg(uint16_t *msg_id, uint32_t *msg_size)
{
    vpp_ctx_lock();
    if (msg_id)   *msg_id = g_ctx.last_msg_id;
    if (msg_size) *msg_size = g_ctx.last_msg_size;
    vpp_ctx_unlock();
}
