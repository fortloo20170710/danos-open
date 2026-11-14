/*
 * DANOS-Open Backend: Linux kernel netlink adapter implementation.
 */

#include "danos_netlink.h"
#include <danos/dpa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>

/* =========================================================================
 * Shared state
 * ========================================================================= */

static struct {
    bool real;
    bool initialized;
} g_nl;

/* =========================================================================
 * Mock tables (in-memory FIB / iface flags)
 * ========================================================================= */

#define MOCK_MAX_ROUTES 4096
#define MOCK_MAX_IFACES 256

typedef struct {
    uint32_t table_id;
    uint8_t  addr[4];
    uint8_t  len;
    bool     used;
} mock_route_t;

static struct {
    mock_route_t routes[MOCK_MAX_ROUTES];
    unsigned route_count;
    bool iface_up[MOCK_MAX_IFACES];
    bool iface_known[MOCK_MAX_IFACES];
    /* Carrier state, independent of admin state (tests drive this). */
    bool iface_link[MOCK_MAX_IFACES];
    bool iface_link_known[MOCK_MAX_IFACES];
} g_mock;

static mock_route_t *mock_find(const uint8_t addr[4], uint8_t len,
                               danos_vrf_id_t table_id)
{
    for (unsigned i = 0; i < MOCK_MAX_ROUTES; i++) {
        mock_route_t *r = &g_mock.routes[i];
        if (r->used && r->len == len && r->table_id == table_id &&
            memcmp(r->addr, addr, 4) == 0)
            return r;
    }
    return NULL;
}

static mock_route_t *mock_alloc(void)
{
    for (unsigned i = 0; i < MOCK_MAX_ROUTES; i++)
        if (!g_mock.routes[i].used) return &g_mock.routes[i];
    return NULL;
}

unsigned danos_netlink_mock_route_count(void)
{
    return g_mock.route_count;
}

bool danos_netlink_mock_route_exists(const uint8_t addr[4], uint8_t len,
                                     danos_vrf_id_t table_id)
{
    return mock_find(addr, len, table_id) != NULL;
}

bool danos_netlink_mock_iface_up(danos_ifindex_t ifindex)
{
    if (ifindex == 0 || ifindex >= MOCK_MAX_IFACES) return false;
    return g_mock.iface_known[ifindex] && g_mock.iface_up[ifindex];
}

void danos_netlink_mock_set_link(danos_ifindex_t ifindex, bool up)
{
    if (ifindex == 0 || ifindex >= MOCK_MAX_IFACES) return;
    g_mock.iface_link_known[ifindex] = true;
    g_mock.iface_link[ifindex] = up;
}

bool danos_netlink_is_real(void)
{
    return g_nl.real && g_nl.initialized;
}

/* =========================================================================
 * Real rtnetlink
 * ========================================================================= */

static int nl_fd = -1;
static uint32_t nl_sequence;
#define NL_IO_TIMEOUT_MS 1000
int danos_netlink_last_kernel_error = 0;

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>

/* IFLA_OPERSTATE value for an up link (linux/if.h). glibc's net/if.h and
 * linux/if.h cannot be included together, and only the former is used here. */
#ifndef IF_OPER_UP
#define IF_OPER_UP 6
#endif

static uint64_t nl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void nl_reset(void)
{
    if (nl_fd >= 0) close(nl_fd);
    nl_fd = -1;
}
static int nl_open(void)
{
    if (nl_fd >= 0) return 0;
    nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nl_fd < 0) return -1;
    struct timeval timeout = { .tv_sec = NL_IO_TIMEOUT_MS / 1000 };
    int strict = 1;
    if (setsockopt(nl_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(nl_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(nl_fd, SOL_NETLINK, NETLINK_GET_STRICT_CHK, &strict, sizeof(strict)) != 0) {
        nl_reset();
        return -1;
    }
    return 0;
}
static int nl_send(void *buf, size_t size)
{
    if (nl_open() != 0) return -1;
    struct nlmsghdr *nh = buf;
    if (++nl_sequence == 0) ++nl_sequence;
    nh->nlmsg_seq = nl_sequence;
    if (send(nl_fd, buf, size, 0) != (ssize_t)size) { nl_reset(); return -1; }
    return 0;
}
static ssize_t nl_receive(void *buf, size_t size, uint64_t deadline)
{
    for (;;) {
        uint64_t now = nl_now_ms();
        if (now >= deadline) break;
        struct pollfd pfd = { .fd = nl_fd, .events = POLLIN };
        int rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || !(pfd.revents & POLLIN)) break;
        ssize_t n = recv(nl_fd, buf, size, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n > 0 && (size_t)n <= size) return n;
        break;
    }
    /* Discard the whole socket: a late reply must not become the next ACK. */
    nl_reset();
    return -1;
}

/* Read and validate one netlink ACK. The kernel replies with NLMSG_ERROR
 * carrying error 0 for success; anything else is a programming failure.
 * The length/type checks keep a malformed or truncated reply from being
 * read past its header. */
static int nl_read_ack(void)
{
    char resp[1024];
    ssize_t n = nl_receive(resp, sizeof(resp), nl_now_ms() + NL_IO_TIMEOUT_MS);
    if (n < (ssize_t)sizeof(struct nlmsghdr)) return -1;
    struct nlmsghdr *nh = (struct nlmsghdr *)resp;
    if (nh->nlmsg_len < sizeof(*nh) || nh->nlmsg_len > (size_t)n) return -1;
    if (nh->nlmsg_seq != nl_sequence || nh->nlmsg_type != NLMSG_ERROR) { nl_reset(); return -1; }
    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -1;
    struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nh);
    danos_netlink_last_kernel_error = err->error;
    return err->error == 0 ? 0 : -1;
}

/* Build and send RTM_NEWROUTE for an IPv4 route (kernel table_id). */
static int nl_route_msg(const danos_resolved_route_t *res, int add)
{
    struct {
        struct nlmsghdr nh;
        struct rtmsg    rt;
        char            attrbuf[4096];
    } msg;
    memset(&msg, 0, sizeof(msg));

    msg.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    /* REPLACE makes replay idempotent: after a controller restart the
     * same route is re-issued and must update, not fail with EEXIST. */
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
                         (add ? (NLM_F_CREATE | NLM_F_REPLACE) : 0);
    msg.nh.nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
    msg.nh.nlmsg_seq = 1;

    msg.rt.rtm_family = AF_INET;
    const danos_route_t *r = &res->route;
    msg.rt.rtm_dst_len = r->prefix.prefix_len;
    uint32_t table = r->vrf_id == 0 ? RT_TABLE_MAIN : r->vrf_id;
    msg.rt.rtm_table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;
    msg.rt.rtm_type = (r->flags & DANOS_ROUTE_FLAG_BLACKHOLE)
                          ? RTN_BLACKHOLE : RTN_UNICAST;
    msg.rt.rtm_protocol = RTPROT_STATIC;

    struct rtattr *rta = (struct rtattr *)msg.attrbuf;
    /* RTA_DST */
    rta->rta_type = RTA_DST;
    rta->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(rta), r->prefix.addr.addr, 4);
    msg.nh.nlmsg_len += RTA_LENGTH(4);

    if (table >= 256) {
        rta = (struct rtattr *)((char *)&msg + NLMSG_ALIGN(msg.nh.nlmsg_len));
        rta->rta_type = RTA_TABLE;
        rta->rta_len = RTA_LENGTH(sizeof(table));
        memcpy(RTA_DATA(rta), &table, sizeof(table));
        msg.nh.nlmsg_len = NLMSG_ALIGN(msg.nh.nlmsg_len) + RTA_SPACE(sizeof(table));
    }

    if (add && res->nh_count > 1 && !(r->flags & DANOS_ROUTE_FLAG_BLACKHOLE)) {
        if (res->nh_count > 64) return -1;
        rta = (struct rtattr *)((char *)&msg + NLMSG_ALIGN(msg.nh.nlmsg_len));
        rta->rta_type = RTA_MULTIPATH;
        rta->rta_len = RTA_LENGTH(0);
        for (uint32_t i = 0; i < res->nh_count; i++) {
            struct rtnexthop *nh = (struct rtnexthop *)((char *)rta + rta->rta_len);
            nh->rtnh_len = sizeof(*nh);
            nh->rtnh_ifindex = (int)res->nh_oif[i];
            struct rtattr *gw = (struct rtattr *)((char *)nh + sizeof(*nh));
            gw->rta_type = RTA_GATEWAY;
            gw->rta_len = RTA_LENGTH(4);
            memcpy(RTA_DATA(gw), res->nh_gw[i], 4);
            nh->rtnh_len += RTA_SPACE(4);
            rta->rta_len += RTNH_ALIGN(nh->rtnh_len);
        }
        msg.nh.nlmsg_len = NLMSG_ALIGN(msg.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);
    } else if (add && res->has_gw && !(r->flags & DANOS_ROUTE_FLAG_BLACKHOLE)) {
        rta = (struct rtattr *)((char *)&msg + NLMSG_ALIGN(msg.nh.nlmsg_len));
        rta->rta_type = RTA_GATEWAY;
        rta->rta_len = RTA_LENGTH(4);
        memcpy(RTA_DATA(rta), res->gw, 4);
        msg.nh.nlmsg_len = NLMSG_ALIGN(msg.nh.nlmsg_len) + RTA_SPACE(4);
        if (res->oif) {
            rta = (struct rtattr *)((char *)&msg + NLMSG_ALIGN(msg.nh.nlmsg_len));
            rta->rta_type = RTA_OIF;
            rta->rta_len = RTA_LENGTH(4);
            memcpy(RTA_DATA(rta), &res->oif, 4);
            msg.nh.nlmsg_len = NLMSG_ALIGN(msg.nh.nlmsg_len) + RTA_SPACE(4);
        }
    }

    if (nl_send(&msg, msg.nh.nlmsg_len) != 0) return -1;

    /* read ACK (or error) */
    return nl_read_ack();
}

static int nl_iface_up(danos_ifindex_t ifindex, bool up)
{
    struct {
        struct nlmsghdr nh;
        struct ifinfomsg ifi;
    } msg;
    memset(&msg, 0, sizeof(msg));
    msg.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    msg.nh.nlmsg_type = RTM_NEWLINK;
    msg.nh.nlmsg_seq = 1;
    msg.ifi.ifi_index = (int)ifindex;
    msg.ifi.ifi_flags = up ? IFF_UP : 0;
    msg.ifi.ifi_change = IFF_UP;

    if (nl_send(&msg, msg.nh.nlmsg_len) != 0) return -1;
    return nl_read_ack();
}

/* Read one link's operational state. Returns 0 and fills the ifindex, MTU,
 * name (when reported) and link_up; -1 on a kernel error or short reply.
 *
 * link_up follows the kernel semantics operators expect: administratively
 * up AND carrier up. IFLA_OPERSTATE, when present, is authoritative; the
 * IFF_RUNNING flag is the fallback for kernels that omit it. */
static int nl_iface_read(danos_ifindex_t ifindex, danos_iface_t *out)
{
    struct {
        struct nlmsghdr nh;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nh.nlmsg_flags = NLM_F_REQUEST;
    req.nh.nlmsg_type = RTM_GETLINK;
    req.nh.nlmsg_seq = 2;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = (int)ifindex;
    if (nl_send(&req, req.nh.nlmsg_len) != 0) return -1;

    char resp[4096];
    ssize_t n = nl_receive(resp, sizeof(resp), nl_now_ms() + NL_IO_TIMEOUT_MS);
    if (n < (ssize_t)sizeof(struct nlmsghdr)) return -1;

    int rem = (int)n;
    for (struct nlmsghdr *nh = (struct nlmsghdr *)resp; NLMSG_OK(nh, rem);
         nh = NLMSG_NEXT(nh, rem)) {
        if (nh->nlmsg_seq != req.nh.nlmsg_seq) continue;
        if (nh->nlmsg_type == NLMSG_ERROR) {
            if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -1;
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nh);
            if (err->error) {
                danos_netlink_last_kernel_error = err->error;
                return -1;
            }
            continue;
        }
        if (nh->nlmsg_type != RTM_NEWLINK ||
            nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
            continue;

        const struct ifinfomsg *ifi = NLMSG_DATA(nh);
        if ((danos_ifindex_t)ifi->ifi_index != ifindex) return -1;
        out->ifindex = (danos_ifindex_t)ifi->ifi_index;
        out->admin_up = (ifi->ifi_flags & IFF_UP) != 0;
        bool up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
        int alen = (int)nh->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));
        for (struct rtattr *rta = IFLA_RTA(ifi); RTA_OK(rta, alen);
             rta = RTA_NEXT(rta, alen)) {
            if (rta->rta_type == IFLA_OPERSTATE && RTA_PAYLOAD(rta) >= 1) {
                uint8_t state;
                memcpy(&state, RTA_DATA(rta), 1);
                up = (ifi->ifi_flags & IFF_UP) && state == IF_OPER_UP;
            } else if (rta->rta_type == IFLA_MTU &&
                       (size_t)RTA_PAYLOAD(rta) >= sizeof(uint32_t)) {
                uint32_t mtu;
                memcpy(&mtu, RTA_DATA(rta), sizeof(mtu));
                if (mtu <= UINT16_MAX) out->mtu = (uint16_t)mtu;
            } else if (rta->rta_type == IFLA_IFNAME &&
                       RTA_PAYLOAD(rta) >= 1) {
                size_t len = RTA_PAYLOAD(rta);
                if (len >= sizeof(out->name)) len = sizeof(out->name) - 1;
                memcpy(out->name, RTA_DATA(rta), len);
                out->name[len] = '\0';
            }
        }
        out->link_up = up;
        return 0;
    }
    return -1;
}

static bool ipv4_prefix_equal(const uint8_t lhs[4], const uint8_t rhs[4],
                              uint8_t prefix_len)
{
    unsigned full_bytes = prefix_len / 8;
    unsigned remaining_bits = prefix_len % 8;
    if (full_bytes && memcmp(lhs, rhs, full_bytes) != 0) return false;
    if (remaining_bits) {
        uint8_t mask = (uint8_t)(0xffu << (8 - remaining_bits));
        if ((lhs[full_bytes] & mask) != (rhs[full_bytes] & mask))
            return false;
    }
    return true;
}

/* A GETROUTE lookup follows policy rules and can return a covering host route,
 * ignoring the requested table. Use a strict table-filtered multipart dump and
 * compare exact prefixes; consume DONE before publishing any observation. */
static danos_status_t route_observation(const struct nlmsghdr *nh,
                                         const danos_route_t *desired,
                                         const danos_resolved_route_t *resolved)
{
    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg))) return DANOS_ERR_BACKEND_IO;
    const struct rtmsg *rt = NLMSG_DATA(nh);
    if (rt->rtm_family != AF_INET || rt->rtm_dst_len != desired->prefix.prefix_len)
        return DANOS_ERR_NOT_FOUND;
    uint32_t table = rt->rtm_table;
    uint8_t dst[4] = {0}, gateways[64][4] = {{0}};
    uint32_t interfaces[64] = {0};
    unsigned count = 1;
    bool multipath = false, nondefault_weight = false, unsupported = false;
    int remaining = (int)nh->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
    for (struct rtattr *a = RTM_RTA(rt); RTA_OK(a, remaining); a = RTA_NEXT(a, remaining)) {
        switch (a->rta_type) {
        case RTA_TABLE:
            if (RTA_PAYLOAD(a) != 4) return DANOS_ERR_BACKEND_IO;
            memcpy(&table, RTA_DATA(a), 4); break;
        case RTA_DST:
            if (RTA_PAYLOAD(a) != 4) return DANOS_ERR_BACKEND_IO;
            memcpy(dst, RTA_DATA(a), 4); break;
        case RTA_GATEWAY:
            if (RTA_PAYLOAD(a) != 4 || multipath) return DANOS_ERR_BACKEND_IO;
            memcpy(gateways[0], RTA_DATA(a), 4); break;
        case RTA_OIF:
            if (RTA_PAYLOAD(a) != 4 || multipath) return DANOS_ERR_BACKEND_IO;
            memcpy(&interfaces[0], RTA_DATA(a), 4); break;
        case RTA_MULTIPATH: {
            if (multipath) return DANOS_ERR_BACKEND_IO;
            multipath = true; count = 0;
            int left = RTA_PAYLOAD(a);
            struct rtnexthop *path = RTA_DATA(a);
            while (RTNH_OK(path, left)) {
                if (count >= 64) { unsupported = true; break; }
                interfaces[count] = (uint32_t)path->rtnh_ifindex;
                if (path->rtnh_hops != 0) nondefault_weight = true;
                int attrs = path->rtnh_len - sizeof(*path);
                for (struct rtattr *gw = RTNH_DATA(path); RTA_OK(gw, attrs); gw = RTA_NEXT(gw, attrs)) {
                    if (gw->rta_type == RTA_GATEWAY) {
                        if (RTA_PAYLOAD(gw) != 4) return DANOS_ERR_BACKEND_IO;
                        memcpy(gateways[count], RTA_DATA(gw), 4);
                    } else if (gw->rta_type == RTA_VIA || gw->rta_type == RTA_ENCAP)
                        unsupported = true;
                }
                if (attrs) return DANOS_ERR_BACKEND_IO;
                if (path->rtnh_flags & (RTNH_F_DEAD | RTNH_F_LINKDOWN)) unsupported = true;
                count++;
                left -= RTNH_ALIGN(path->rtnh_len);
                path = RTNH_NEXT(path);
            }
            if (left && !unsupported) return DANOS_ERR_BACKEND_IO;
            break;
        }
        case RTA_NH_ID: case RTA_VIA: case RTA_ENCAP:
            unsupported = true; break;
        default: break;
        }
    }
    if (remaining) return DANOS_ERR_BACKEND_IO;
    uint32_t expected = desired->vrf_id ? desired->vrf_id : RT_TABLE_MAIN;
    if (table != expected || !ipv4_prefix_equal(dst, desired->prefix.addr.addr, desired->prefix.prefix_len))
        return DANOS_ERR_NOT_FOUND;
    bool blackhole = (desired->flags & DANOS_ROUTE_FLAG_BLACKHOLE) != 0;
    if (rt->rtm_type != (blackhole ? RTN_BLACKHOLE : RTN_UNICAST)) return DANOS_ERR_VERIFY_FAIL;
    if (!resolved) return DANOS_OK; /* presence-only public compatibility probe */
    if (blackhole) return DANOS_OK;
    unsigned expected_count = resolved->nh_count ? resolved->nh_count : 1;
    if (count != expected_count || nondefault_weight || unsupported) return DANOS_ERR_VERIFY_FAIL;
    bool matched[64] = {false};
    for (unsigned i = 0; i < expected_count; i++) {
        const uint8_t *gw = resolved->nh_count ? resolved->nh_gw[i] : resolved->gw;
        uint32_t oif = resolved->nh_count ? resolved->nh_oif[i] : resolved->oif;
        bool found = false;
        for (unsigned j = 0; j < count; j++)
            if (!matched[j] && memcmp(gateways[j], gw, 4) == 0 && (!oif || interfaces[j] == oif)) {
                matched[j] = true; found = true; break;
            }
        if (!found) return DANOS_ERR_VERIFY_FAIL;
    }
    return DANOS_OK;
}

static danos_status_t nl_route_read(const danos_route_t *desired,
                                     const danos_resolved_route_t *resolved)
{
    struct {
        struct nlmsghdr nh;
        struct rtmsg rt;
        char attrbuf[RTA_SPACE(4)];
    } req;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.rt));
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nh.nlmsg_type = RTM_GETROUTE;
    req.rt.rtm_family = AF_INET;
    uint32_t table = desired->vrf_id ? desired->vrf_id : RT_TABLE_MAIN;
    req.rt.rtm_table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;
    if (table >= 256) {
        struct rtattr *a = (struct rtattr *)req.attrbuf;
        a->rta_type = RTA_TABLE; a->rta_len = RTA_LENGTH(4);
        memcpy(RTA_DATA(a), &table, 4);
        req.nh.nlmsg_len += RTA_SPACE(4);
    }
    if (nl_send(&req, req.nh.nlmsg_len) != 0) return DANOS_ERR_BACKEND_IO;
    uint64_t deadline = nl_now_ms() + NL_IO_TIMEOUT_MS;
    danos_status_t observation = DANOS_ERR_NOT_FOUND;
    bool found = false;
    _Alignas(struct nlmsghdr) char response[32768];
    for (unsigned frame = 0; frame < 65536; frame++) {
        ssize_t n = nl_receive(response, sizeof(response), deadline);
        if (n < (ssize_t)sizeof(struct nlmsghdr)) break;
        int remaining = (int)n;
        for (struct nlmsghdr *nh = (struct nlmsghdr *)response; NLMSG_OK(nh, remaining);
             nh = NLMSG_NEXT(nh, remaining)) {
            if (nh->nlmsg_seq != req.nh.nlmsg_seq) continue;
            if (nh->nlmsg_flags & NLM_F_DUMP_INTR) goto incomplete;
            if (nh->nlmsg_type == NLMSG_DONE) {
                if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(int))) goto incomplete;
                int error;
                memcpy(&error, NLMSG_DATA(nh), sizeof(error));
                if (error) goto incomplete;
                return observation;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) goto incomplete;
                const struct nlmsgerr *err = NLMSG_DATA(nh);
                danos_netlink_last_kernel_error = err->error;
                if (err->error == -ENOENT) return DANOS_ERR_NOT_FOUND;
                goto incomplete;
            }
            if (nh->nlmsg_type != RTM_NEWROUTE) continue;
            danos_status_t st = route_observation(nh, desired, resolved);
            if (st == DANOS_ERR_BACKEND_IO) goto incomplete;
            if (st != DANOS_ERR_NOT_FOUND) {
                if (found) observation = DANOS_ERR_VERIFY_FAIL; /* ambiguous same-prefix routes */
                else observation = st;
                found = true;
            }
        }
        if (remaining) break;
    }
incomplete:
    nl_reset();
    return DANOS_ERR_BACKEND_IO;
}

static int nl_iface_addr_msg(const danos_iface_t *iface, int add)
{
    const danos_ip_prefix_t *p = &iface->ipv4_address;
    if (p->addr.af != DANOS_AF_IPV4) return -1;
    struct {
        struct nlmsghdr nh;
        struct ifaddrmsg ifa;
        char attrbuf[64];
    } msg;
    memset(&msg, 0, sizeof(msg));
    msg.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (add) msg.nh.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    msg.nh.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
    msg.ifa.ifa_family = AF_INET;
    msg.ifa.ifa_prefixlen = p->prefix_len;
    msg.ifa.ifa_index = iface->ifindex;
    struct rtattr *rta = (struct rtattr *)msg.attrbuf;
    rta->rta_type = IFA_LOCAL;
    rta->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(rta), p->addr.addr, 4);
    msg.nh.nlmsg_len += RTA_LENGTH(4);
    if (nl_send(&msg, msg.nh.nlmsg_len) != 0) return -1;
    return nl_read_ack();
}

/* =========================================================================
 * danos_backend_ops_t implementation
 * ========================================================================= */

static danos_status_t ops_iface_up(danos_ifindex_t ifindex, bool up, void *user)
{
    (void)user;
    if (ifindex == 0) return DANOS_ERR_INVALID_ARG;
    if (!g_nl.real) {
        if (ifindex >= MOCK_MAX_IFACES) return DANOS_ERR_INVALID_ARG;
        g_mock.iface_known[ifindex] = true;
        g_mock.iface_up[ifindex] = up;
        return DANOS_OK;
    }
    return nl_iface_up(ifindex, up) == 0 ? DANOS_OK : DANOS_ERR_BACKEND_IO;
}

static danos_status_t ops_iface_addr_set(const danos_iface_t *iface, void *user)
{
    (void)user;
    if (!iface || iface->ifindex == 0) return DANOS_ERR_INVALID_ARG;
    if (iface->ipv4_address.addr.af != DANOS_AF_IPV4)
        return DANOS_ERR_NOT_SUPPORTED;
    if (!g_nl.real) return DANOS_OK;
    return nl_iface_addr_msg(iface, 1) == 0 ? DANOS_OK : DANOS_ERR_BACKEND_IO;
}

static danos_status_t ops_iface_addr_del(const danos_iface_t *iface, void *user)
{
    (void)user;
    if (!iface || iface->ifindex == 0) return DANOS_ERR_INVALID_ARG;
    if (!g_nl.real) return DANOS_OK;
    if (iface->ipv4_address.addr.af != DANOS_AF_IPV4)
        return DANOS_ERR_NOT_SUPPORTED;
    return nl_iface_addr_msg(iface, 0) == 0 ? DANOS_OK : DANOS_ERR_BACKEND_IO;
}

static danos_status_t ops_route_add(const danos_resolved_route_t *res,
                                    void *user)
{
    (void)user;
    if (!res) return DANOS_ERR_INVALID_ARG;
    const danos_route_t *route = &res->route;
    if (route->prefix.addr.af != DANOS_AF_IPV4)
        return DANOS_ERR_INVALID_ARG;
    if (route->prefix.prefix_len > 32 || res->nh_count > 64)
        return DANOS_ERR_INVALID_ARG;

    if (!g_nl.real) {
        if (mock_find(route->prefix.addr.addr, route->prefix.prefix_len,
                      route->vrf_id)) {
            mock_route_t *r = mock_find(route->prefix.addr.addr,
                                        route->prefix.prefix_len,
                                        route->vrf_id);
            (void)r;   /* idempotent re-add */
            return DANOS_OK;
        }
        mock_route_t *r = mock_alloc();
        if (!r) return DANOS_ERR_NO_MEMORY;
        r->used = true;
        r->table_id = route->vrf_id;
        r->len = route->prefix.prefix_len;
        memcpy(r->addr, route->prefix.addr.addr, 4);
        g_mock.route_count++;
        return DANOS_OK;
    }
    if (nl_route_msg(res, 1) == 0) return DANOS_OK;
    fprintf(stderr, "netlink: route add kernel error %d (%s)\n",
            danos_netlink_last_kernel_error,
            strerror(-danos_netlink_last_kernel_error));
    return DANOS_ERR_BACKEND_IO;
}

static danos_status_t ops_route_del(const danos_resolved_route_t *res,
                                    void *user)
{
    (void)user;
    if (!res) return DANOS_ERR_INVALID_ARG;
    const danos_route_t *route = &res->route;
    if (route->prefix.addr.af != DANOS_AF_IPV4)
        return DANOS_ERR_INVALID_ARG;
    if (!g_nl.real) {
        mock_route_t *r = mock_find(route->prefix.addr.addr,
                                    route->prefix.prefix_len, route->vrf_id);
        if (!r) return DANOS_ERR_NOT_FOUND;
        r->used = false;
        if (g_mock.route_count) g_mock.route_count--;
        return DANOS_OK;
    }
    return nl_route_msg(res, 0) == 0 ? DANOS_OK : DANOS_ERR_BACKEND_IO;
}

static danos_status_t ops_vrf_add(const danos_vrf_t *vrf, void *user)
{
    (void)user; (void)vrf;
    /* No kernel VRF device lifecycle is implemented. Never claim success. */
    return g_nl.real ? DANOS_ERR_NOT_SUPPORTED : DANOS_OK;
}

static danos_status_t ops_vrf_del(danos_vrf_id_t vrf_id, void *user)
{
    (void)user; (void)vrf_id;
    return g_nl.real ? DANOS_ERR_NOT_SUPPORTED : DANOS_OK;
}

/* Operational readback: link state and exact IPv4 route presence. */
static danos_status_t ops_readback(danos_obj_type_t type, danos_obj_id_t id,
                                   const void *desired, size_t size,
                                   void *out, void *user)
{
    (void)user;
    if (type == DANOS_OBJ_ROUTE) {
        if (!desired || !out || size < sizeof(danos_route_t))
            return DANOS_ERR_INVALID_ARG;
        const danos_route_t *route = desired;
        if (route->prefix.addr.af != DANOS_AF_IPV4 ||
            route->prefix.prefix_len > 32)
            return DANOS_ERR_NOT_SUPPORTED;
        if (g_nl.real && g_nl.initialized) {
            danos_status_t st = nl_route_read(route, NULL);
            if (st != DANOS_OK) return st;
        } else if (!mock_find(route->prefix.addr.addr,
                              route->prefix.prefix_len, route->vrf_id)) {
            return DANOS_ERR_NOT_FOUND;
        }
        memcpy(out, desired, sizeof(danos_route_t));
        return DANOS_OK;
    }
    if (type != DANOS_OBJ_IFACE) return DANOS_ERR_NOT_SUPPORTED;
    if (!desired || !out || size < sizeof(danos_iface_t) || id == 0)
        return DANOS_ERR_INVALID_ARG;

    const danos_iface_t *d = desired;
    danos_iface_t *o = out;
    memset(o, 0, sizeof(*o));
    if (g_nl.real && g_nl.initialized) {
        if (nl_iface_read((danos_ifindex_t)id, o) != 0)
            return DANOS_ERR_BACKEND_IO;
        /* The kernel carries identity and state; keep the configured
         * attributes it does not report from the desired copy. */
        if (!o->name[0]) snprintf(o->name, sizeof(o->name), "%s", d->name);
        o->type = d->type;
        memcpy(o->mac, d->mac, sizeof(o->mac));
        o->parent_ifindex = d->parent_ifindex;
        o->outer_vlan = d->outer_vlan;
        o->inner_vlan = d->inner_vlan;
        o->ipv4_address = d->ipv4_address;
        o->ipv6_address = d->ipv6_address;
        return DANOS_OK;
    }

    if (id >= MOCK_MAX_IFACES) return DANOS_ERR_NOT_FOUND;
    if (!g_mock.iface_known[id]) return DANOS_ERR_NOT_FOUND;
    *o = *d;
    o->admin_up = g_mock.iface_up[id];
    o->link_up = g_mock.iface_link_known[id] ? g_mock.iface_link[id]
                                             : g_mock.iface_up[id];
    return DANOS_OK;
}

static danos_status_t ops_route_verify(const danos_resolved_route_t *res, void *user)
{
    (void)user;
    if (!res || res->route.prefix.prefix_len > 32 || res->nh_count > 64)
        return DANOS_ERR_INVALID_ARG;
    if (res->route.prefix.addr.af != DANOS_AF_IPV4) return DANOS_ERR_NOT_SUPPORTED;
    if (!g_nl.real)
        return mock_find(res->route.prefix.addr.addr, res->route.prefix.prefix_len,
                         res->route.vrf_id) ? DANOS_OK : DANOS_ERR_NOT_FOUND;
    return nl_route_read(&res->route, res);
}

static danos_backend_ops_t g_ops = {
    .name      = "netlink",
    .iface_up  = ops_iface_up,
    .iface_addr_set = ops_iface_addr_set,
    .iface_addr_del = ops_iface_addr_del,
    .route_add = ops_route_add,
    .route_del = ops_route_del,
    .vrf_add   = ops_vrf_add,
    .vrf_del   = ops_vrf_del,
    .readback  = ops_readback,
    .route_verify = ops_route_verify,
    .user      = NULL,
};

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

int danos_netlink_init(bool real)
{
    memset(&g_mock, 0, sizeof(g_mock));
    g_nl.real = real;
    g_nl.initialized = true;
    if (real) {
        if (nl_open() != 0) {
            /* fall back to mock when rtnetlink is unavailable */
            g_nl.real = false;
            return -1;
        }
    }
    return 0;
}

void danos_netlink_register_backend(void)
{
    danos_backend_ops_set(&g_ops);
}

void danos_netlink_shutdown(void)
{
    if (nl_fd >= 0) {
        close(nl_fd);
        nl_fd = -1;
    }
    g_nl.initialized = false;
}
