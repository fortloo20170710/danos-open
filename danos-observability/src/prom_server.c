/*
 * DANOS-Open Observability: /metrics HTTP server (v0.6)
 *
 * Minimal HTTP/1.1 server: accepts GET (any path), answers 200 with
 * the Prometheus text exposition body. One accept loop on a detached
 * thread; per-connection handling is inline (metrics scraping is
 * low-rate).
 */

#include <danos/observability/prometheus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdatomic.h>

/* Scrapes are infrequent, so a small worker cap is enough. The cap turns
 * an unbounded thread-per-connection design into a bounded one. */
#define PROM_MAX_CONNS 32
#define PROM_RECV_TIMEOUT_SEC 5

static struct {
    int listen_fd;
    uint16_t port;
    char bind_addr[64];
    _Atomic bool running;
    _Atomic int active_conns;
    pthread_t thread;
} g_prom_srv;

static void *prom_conn(void *arg)
{
    int fd = (int)(long)arg;

    struct timeval tv = { .tv_sec = PROM_RECV_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Read until the end of the headers. A single recv() would treat a
     * fragmented or trickled request as a 404, and would let a client
     * hold the worker open indefinitely without a timeout. */
    char req[2048];
    size_t used = 0;
    bool complete = false;
    while (used < sizeof(req) - 1) {
        ssize_t n = recv(fd, req + used, sizeof(req) - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        req[used] = '\0';
        if (strstr(req, "\r\n\r\n")) { complete = true; break; }
    }
    req[used] = '\0';

    if (!complete) {
        close(fd);
        atomic_fetch_sub(&g_prom_srv.active_conns, 1);
        return NULL;
    }

    if (strstr(req, "/metrics")) {
        char body[16384];
        int blen = danos_prom_render(body, sizeof(body) - 1);
        if (blen < 0) blen = 0;
        body[blen] = '\0';

        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/plain; version=0.0.4\r\n"
                            "Content-Length: %d\r\n"
                            "Connection: close\r\n\r\n", blen);
        (void)send(fd, hdr, (size_t)hlen, MSG_NOSIGNAL);
        if (blen > 0) (void)send(fd, body, (size_t)blen, MSG_NOSIGNAL);
    } else {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n";
        (void)send(fd, nf, strlen(nf), MSG_NOSIGNAL);
    }
    close(fd);
    atomic_fetch_sub(&g_prom_srv.active_conns, 1);
    return NULL;
}

static void *prom_accept_loop(void *arg)
{
    (void)arg;
    while (atomic_load(&g_prom_srv.running)) {
        int fd = accept(g_prom_srv.listen_fd, NULL, NULL);
        if (fd < 0) break;
        if (atomic_load(&g_prom_srv.active_conns) >= PROM_MAX_CONNS) {
            const char *busy =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            (void)send(fd, busy, strlen(busy), MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        atomic_fetch_add(&g_prom_srv.active_conns, 1);
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, prom_conn, (void *)(long)fd) != 0) {
            atomic_fetch_sub(&g_prom_srv.active_conns, 1);
            close(fd);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

int danos_prom_start_server(uint16_t port)
{
    if (atomic_load(&g_prom_srv.running)) return 0;
    signal(SIGPIPE, SIG_IGN);

    /* Loopback by default: the exposition reveals topology and counters
     * and has no authentication. A remote scrape must opt in explicitly
     * with DANOS_METRICS_BIND=0.0.0.0 (or a specific address). */
    const char *bind_addr = getenv("DANOS_METRICS_BIND");
    if (!bind_addr || !bind_addr[0]) bind_addr = "127.0.0.1";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 32) < 0) {
        close(fd);
        return -1;
    }
    g_prom_srv.listen_fd = fd;
    g_prom_srv.port = port;
    snprintf(g_prom_srv.bind_addr, sizeof(g_prom_srv.bind_addr), "%s", bind_addr);
    atomic_store(&g_prom_srv.active_conns, 0);
    atomic_store(&g_prom_srv.running, true);

    if (pthread_create(&g_prom_srv.thread, NULL, prom_accept_loop, NULL) != 0) {
        close(fd);
        atomic_store(&g_prom_srv.running, false);
        return -1;
    }
    return 0;
}

void danos_prom_stop_server(void)
{
    if (!atomic_load(&g_prom_srv.running)) return;
    atomic_store(&g_prom_srv.running, false);
    shutdown(g_prom_srv.listen_fd, SHUT_RDWR);
    close(g_prom_srv.listen_fd);
    pthread_join(g_prom_srv.thread, NULL);
    g_prom_srv.listen_fd = -1;
}
