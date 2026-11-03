/*
 * DANOS-Open Backend: Linux kernel netlink adapter implementation.
 */

#include "danos_netlink.h"
#include <danos/dpa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

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

#define MOCK_ROUTE_MAX_NH 64

typedef struct {
    uint32_t table_id;
    uint8_t  addr[4];
    uint8_t  len;
    bool     used;
    bool     blackhole;
    uint32_t metric;
    uint32_t nh_count;
    uint8_t  nh_gw[MOCK_ROUTE_MAX_NH][4];
    uint32_t nh_oif[MOCK_ROUTE_MAX_NH];
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

static int nl_open(void)
{
    if (nl_fd >= 0) return 0;
    nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    return nl_fd < 0 ? -1 : 0;
}

/* Read and validate one netlink ACK. The kernel replies with NLMSG_ERROR
 * carrying error 0 for success; anything else is a programming failure.
 * The length/type checks keep a malformed or truncated reply from being
 * read past its header. */
static int nl_read_ack(void)
{
    char resp[1024];
    ssize_t n = recv(nl_fd, resp, sizeof(resp), 0);
    if (n < (ssize_t)sizeof(struct nlmsghdr)) return -1;
    struct nlmsghdr *nh = (struct nlmsghdr *)resp;
    if (nh->nlmsg_len < sizeof(*nh) || nh->nlmsg_len > (size_t)n) return -1;
    if (nh->nlmsg_type != NLMSG_ERROR) return -1;
    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -1;
    struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nh);
    danos_netlink_last_kernel_error = err->error;
    return err->error == 0 ? 0 : -1;
}

static bool gateway_is_set(const uint8_t gw[4])
{
    return gw[0] || gw[1] || gw[2] || gw[3];
}

/* Build and send RTM_NEWROUTE for an IPv4 route (kernel table_id). */
static int nl_route_msg(const danos_resolved_route_t *res, int add)
{
    if (res->nh_count > DANOS_NHGROUP_MAX_NH) return -1;
    struct {
        struct nlmsghdr nh;
        struct rtmsg    rt;
        char            attrbuf[2048];
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
    msg.rt.rtm_table = (r->vrf_id == 0 || r->vrf_id >= 256) ? RT_TABLE_MAIN : r->vrf_id;
    msg.rt.rtm_type = (r->flags & DANOS_ROUTE_FLAG_BLACKHOLE)
                          ? RTN_BLACKHOLE : RTN_UNICAST;
    msg.rt.rtm_protocol = RTPROT_STATIC;

    struct rtattr *rta = (struct rtattr *)msg.attrbuf;
    /* RTA_DST */
    rta->rta_type = RTA_DST;
    rta->rta_len = RTA_LENGTH(4);
    memcpy(RTA_DATA(rta), r->prefix.addr.addr, 4);
    msg.nh.nlmsg_len += RTA_LENGTH(4);

    /* RTA_PRIORITY carries the route metric so readback can compare the
     * attribute instead of only the prefix. */
    if (add && r->metric) {
        rta = (struct rtattr *)((char *)rta + RTA_LENGTH(4));
        rta->rta_type = RTA_PRIORITY;
        rta->rta_len = RTA_LENGTH(4);
        memcpy(RTA_DATA(rta), &r->metric, 4);
        msg.nh.nlmsg_len += RTA_LENGTH(4);
    }

    /* ECMP: install every resolved next hop as an RTA_MULTIPATH member.
     * A single next hop keeps the simple RTA_GATEWAY/RTA_OIF form. */
    uint32_t nh_count = res->nh_count;
    if (add && !(r->flags & DANOS_ROUTE_FLAG_BLACKHOLE)) {
        if (nh_count > 1) {
            struct rtattr *mp = (struct rtattr *)((char *)rta + RTA_LENGTH(4));
            char *pos = (char *)RTA_DATA(mp);
            for (uint32_t i = 0; i < nh_count; i++) {
                struct rtnexthop *nhx = (struct rtnexthop *)pos;
                bool has_gw = gateway_is_set(res->nh_gw[i]);
                nhx->rtnh_len = (unsigned short)(sizeof(*nhx) +
                    (has_gw ? RTA_LENGTH(4) : 0));
                nhx->rtnh_flags = 0;
                nhx->rtnh_hops = 0;
                nhx->rtnh_ifindex = (int)res->nh_oif[i];
                if (has_gw) {
                    struct rtattr *gw = RTNH_DATA(nhx);
                    gw->rta_type = RTA_GATEWAY;
                    gw->rta_len = RTA_LENGTH(4);
                    memcpy(RTA_DATA(gw), res->nh_gw[i], 4);
                }
                pos += NLMSG_ALIGN(nhx->rtnh_len);
            }
            mp->rta_type = RTA_MULTIPATH;
            mp->rta_len = (unsigned short)(pos - (char *)mp);
            msg.nh.nlmsg_len += mp->rta_len;
        } else if (res->has_gw) {
            rta = (struct rtattr *)((char *)rta + RTA_LENGTH(4));
            rta->rta_type = RTA_GATEWAY;
            rta->rta_len = RTA_LENGTH(4);
            memcpy(RTA_DATA(rta), res->gw, 4);
            msg.nh.nlmsg_len += RTA_LENGTH(4);

            if (res->oif) {
                rta = (struct rtattr *)((char *)rta + RTA_LENGTH(4));
                rta->rta_type = RTA_OIF;
                rta->rta_len = RTA_LENGTH(4);
                uint32_t oif = res->oif;
                memcpy(RTA_DATA(rta), &oif, 4);
                msg.nh.nlmsg_len += RTA_LENGTH(4);
            }
        } else if (res->oif) {
            /* Interface-only next hop (no gateway): RTA_OIF is the path. */
            rta = (struct rtattr *)((char *)rta + RTA_LENGTH(4));
            rta->rta_type = RTA_OIF;
            rta->rta_len = RTA_LENGTH(4);
            uint32_t oif = res->oif;
            memcpy(RTA_DATA(rta), &oif, 4);
            msg.nh.nlmsg_len += RTA_LENGTH(4);
        }
    }

    if (send(nl_fd, &msg, msg.nh.nlmsg_len, 0) < 0) return -1;

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

    if (send(nl_fd, &msg, msg.nh.nlmsg_len, 0) < 0) return -1;
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
    if (send(nl_fd, &req, req.nh.nlmsg_len, 0) < 0) return -1;

    char resp[4096];
    ssize_t n = recv(nl_fd, resp, sizeof(resp), 0);
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
        out->ifindex = (danos_ifindex_t)ifi->ifi_index;
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

/* Confirm an exact IPv4 prefix is present in the expected kernel table and
 * has the expected unicast/blackhole disposition. This intentionally does
 * not claim next-hop-group readback: the DPA NH identity is not encoded in
 * rtnetlink, and the adapter's current resolved-route callback is one-way. */
static danos_status_t nl_route_read(const danos_route_t *desired)
{
    /* RTM_F_FIB_MATCH asks for the FIB entry with its real prefix length and
     * attributes.  A plain lookup reply reports dst_len=32 and one selected
     * next hop, which cannot answer an attribute/presence comparison.  The
     * default prefix is not reachable through FIB_MATCH on current kernels,
     * so report it unsupported rather than absent. */
    if (desired->prefix.prefix_len == 0) return DANOS_ERR_NOT_SUPPORTED;
    struct {
        struct nlmsghdr nh;
        struct rtmsg rt;
        char attrbuf[RTA_SPACE(4)];
    } req;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_flags = NLM_F_REQUEST;
    req.nh.nlmsg_type = RTM_GETROUTE;
    req.nh.nlmsg_seq = 3;
    req.rt.rtm_family = AF_INET;
    req.rt.rtm_flags = RTM_F_FIB_MATCH;
    req.rt.rtm_dst_len = desired->prefix.prefix_len;
    req.rt.rtm_table = (desired->vrf_id == 0 || desired->vrf_id >= 256)
                           ? RT_TABLE_MAIN : (uint8_t)desired->vrf_id;
    if (desired->prefix.prefix_len != 0) {
        struct rtattr *dst = (struct rtattr *)req.attrbuf;
        dst->rta_type = RTA_DST;
        dst->rta_len = RTA_LENGTH(4);
        memcpy(RTA_DATA(dst), desired->prefix.addr.addr, 4);
        req.nh.nlmsg_len += RTA_LENGTH(4);
    }
    if (send(nl_fd, &req, req.nh.nlmsg_len, 0) < 0)
        return DANOS_ERR_BACKEND_IO;

    char response[8192];
    ssize_t n = recv(nl_fd, response, sizeof(response), 0);
    if (n < (ssize_t)sizeof(struct nlmsghdr))
        return DANOS_ERR_BACKEND_IO;
    int rem = (int)n;
    for (struct nlmsghdr *nh = (struct nlmsghdr *)response;
         NLMSG_OK(nh, rem); nh = NLMSG_NEXT(nh, rem)) {
        if (nh->nlmsg_seq != req.nh.nlmsg_seq) continue;
        if (nh->nlmsg_type == NLMSG_ERROR) {
            if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
                return DANOS_ERR_BACKEND_IO;
            const struct nlmsgerr *err = NLMSG_DATA(nh);
            danos_netlink_last_kernel_error = err->error;
            if (err->error == -ESRCH || err->error == -ENOENT ||
                err->error == -ENETUNREACH || err->error == -EHOSTUNREACH)
                return DANOS_ERR_NOT_FOUND;
            return err->error == 0 ? DANOS_ERR_NOT_FOUND
                                   : DANOS_ERR_BACKEND_IO;
        }
        if (nh->nlmsg_type != RTM_NEWROUTE ||
            nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg)))
            continue;

        const struct rtmsg *rt = NLMSG_DATA(nh);
        if (rt->rtm_family != AF_INET ||
            rt->rtm_dst_len != desired->prefix.prefix_len)
            continue;
        uint32_t table = rt->rtm_table;
        uint8_t dst_addr[4] = {0};
        int alen = (int)nh->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
        for (struct rtattr *rta = RTM_RTA(rt); RTA_OK(rta, alen);
             rta = RTA_NEXT(rta, alen)) {
            if (rta->rta_type == RTA_TABLE &&
                RTA_PAYLOAD(rta) >= sizeof(table))
                memcpy(&table, RTA_DATA(rta), sizeof(table));
            else if (rta->rta_type == RTA_DST && RTA_PAYLOAD(rta) >= 4)
                memcpy(dst_addr, RTA_DATA(rta), sizeof(dst_addr));
        }
        uint32_t expected_table =
            (desired->vrf_id == 0 || desired->vrf_id >= 256)
                ? RT_TABLE_MAIN : desired->vrf_id;
        bool expected_blackhole =
            (desired->flags & DANOS_ROUTE_FLAG_BLACKHOLE) != 0;
        bool route_type_matches = expected_blackhole
            ? rt->rtm_type == RTN_BLACKHOLE
            : rt->rtm_type == RTN_UNICAST;
        if (table != expected_table ||
            !ipv4_prefix_equal(dst_addr, desired->prefix.addr.addr,
                               desired->prefix.prefix_len) ||
            !route_type_matches)
            continue;
        return DANOS_OK;
    }
    return DANOS_ERR_NOT_FOUND;
}

/* Route-specific operational verification.  The kernel route lookup returns
 * one entry for the exact prefix/table; compare its disposition, metric and
 * complete next-hop set (single gateway or RTA_MULTIPATH ECMP members) with
 * the resolved route the pipeline programmed.  Presence alone is not a
 * match: a partially withdrawn ECMP group must be reported as drift. */
#define NL_ROUTE_VERIFY_MAX_NH 64

struct nl_verify_expected {
    uint8_t  gw[4];
    uint32_t oif;
    bool     used;
};

/* Match one observed next hop and consume the expected entry, so a
 * duplicated observed member cannot satisfy two different expected members. */
static bool nl_verify_nh(struct nl_verify_expected *exp, uint32_t count,
                         const uint8_t gw[4], uint32_t oif)
{
    for (uint32_t i = 0; i < count; i++) {
        if (exp[i].used) continue;
        if (memcmp(exp[i].gw, gw, 4) != 0) continue;
        /* Kernels may omit RTA_OIF for a gateway route; when present it must
         * match, otherwise the gateway identity is the check. */
        if (oif && exp[i].oif && exp[i].oif != oif) continue;
        exp[i].used = true;
        return true;
    }
    return false;
}

static danos_status_t nl_route_verify(const danos_resolved_route_t *resolved)
{
    const danos_route_t *desired = &resolved->route;
    if (desired->prefix.prefix_len == 0) return DANOS_ERR_NOT_SUPPORTED;
    struct {
        struct nlmsghdr nh;
        struct rtmsg rt;
        char attrbuf[RTA_SPACE(4)];
    } req;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_flags = NLM_F_REQUEST;
    req.nh.nlmsg_type = RTM_GETROUTE;
    req.nh.nlmsg_seq = 4;
    req.rt.rtm_family = AF_INET;
    req.rt.rtm_flags = RTM_F_FIB_MATCH;
    req.rt.rtm_dst_len = desired->prefix.prefix_len;
    req.rt.rtm_table = (desired->vrf_id == 0 || desired->vrf_id >= 256)
                           ? RT_TABLE_MAIN : (uint8_t)desired->vrf_id;
    if (desired->prefix.prefix_len != 0) {
        struct rtattr *dst = (struct rtattr *)req.attrbuf;
        dst->rta_type = RTA_DST;
        dst->rta_len = RTA_LENGTH(4);
        memcpy(RTA_DATA(dst), desired->prefix.addr.addr, 4);
        req.nh.nlmsg_len += RTA_LENGTH(4);
    }
    if (send(nl_fd, &req, req.nh.nlmsg_len, 0) < 0)
        return DANOS_ERR_BACKEND_IO;

    char response[8192];
    ssize_t n = recv(nl_fd, response, sizeof(response), 0);
    if (n < (ssize_t)sizeof(struct nlmsghdr))
        return DANOS_ERR_BACKEND_IO;

    const uint32_t expected_table =
        (desired->vrf_id == 0 || desired->vrf_id >= 256)
            ? RT_TABLE_MAIN : desired->vrf_id;
    const bool expected_blackhole =
        (desired->flags & DANOS_ROUTE_FLAG_BLACKHOLE) != 0;

    int rem = (int)n;
    for (struct nlmsghdr *nh = (struct nlmsghdr *)response;
         NLMSG_OK(nh, rem); nh = NLMSG_NEXT(nh, rem)) {
        if (nh->nlmsg_seq != req.nh.nlmsg_seq) continue;
        if (nh->nlmsg_type == NLMSG_ERROR) {
            if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
                return DANOS_ERR_BACKEND_IO;
            const struct nlmsgerr *err = NLMSG_DATA(nh);
            danos_netlink_last_kernel_error = err->error;
            if (err->error == -ESRCH || err->error == -ENOENT ||
                err->error == -ENETUNREACH || err->error == -EHOSTUNREACH)
                return DANOS_ERR_NOT_FOUND;
            return err->error == 0 ? DANOS_ERR_NOT_FOUND
                                   : DANOS_ERR_BACKEND_IO;
        }
        if (nh->nlmsg_type != RTM_NEWROUTE ||
            nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg)))
            continue;

        const struct rtmsg *rt = NLMSG_DATA(nh);
        if (rt->rtm_family != AF_INET ||
            rt->rtm_dst_len != desired->prefix.prefix_len)
            continue;

        uint32_t table = rt->rtm_table;
        uint32_t metric = 0;
        uint8_t dst_addr[4] = {0};
        bool route_type_matches = expected_blackhole
            ? rt->rtm_type == RTN_BLACKHOLE
            : rt->rtm_type == RTN_UNICAST;

        struct nl_verify_expected expected[NL_ROUTE_VERIFY_MAX_NH];
        uint32_t expected_count = 0;
        if (!expected_blackhole) {
            if (resolved->nh_count > 0) {
                expected_count = resolved->nh_count > NL_ROUTE_VERIFY_MAX_NH
                                     ? NL_ROUTE_VERIFY_MAX_NH
                                     : resolved->nh_count;
                for (uint32_t i = 0; i < expected_count; i++) {
                    memcpy(expected[i].gw, resolved->nh_gw[i], 4);
                    expected[i].oif = resolved->nh_oif[i];
                    expected[i].used = false;
                }
            } else if (resolved->has_gw) {
                expected_count = 1;
                memcpy(expected[0].gw, resolved->gw, 4);
                expected[0].oif = resolved->oif;
                expected[0].used = false;
            }
        }

        uint32_t observed_count = 0;
        uint32_t unmatched = 0;
        bool have_gateway = false;
        bool have_oif = false;
        uint8_t single_gw[4] = {0};
        uint32_t single_oif = 0;

        int alen = (int)nh->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
        for (struct rtattr *rta = RTM_RTA(rt); RTA_OK(rta, alen);
             rta = RTA_NEXT(rta, alen)) {
            if (rta->rta_type == RTA_TABLE &&
                RTA_PAYLOAD(rta) >= sizeof(table))
                memcpy(&table, RTA_DATA(rta), sizeof(table));
            else if (rta->rta_type == RTA_DST && RTA_PAYLOAD(rta) >= 4)
                memcpy(dst_addr, RTA_DATA(rta), sizeof(dst_addr));
            else if (rta->rta_type == RTA_PRIORITY &&
                     RTA_PAYLOAD(rta) >= sizeof(metric))
                memcpy(&metric, RTA_DATA(rta), sizeof(metric));
            else if (rta->rta_type == RTA_GATEWAY && RTA_PAYLOAD(rta) >= 4) {
                memcpy(single_gw, RTA_DATA(rta), 4);
                have_gateway = true;
            } else if (rta->rta_type == RTA_OIF &&
                       RTA_PAYLOAD(rta) >= sizeof(uint32_t)) {
                memcpy(&single_oif, RTA_DATA(rta), sizeof(single_oif));
                have_oif = true;
            }
            else if (rta->rta_type == RTA_MULTIPATH) {
                int mlen = (int)RTA_PAYLOAD(rta);
                for (struct rtnexthop *nhx = (struct rtnexthop *)RTA_DATA(rta);
                     RTNH_OK(nhx, mlen); nhx = RTNH_NEXT(nhx)) {
                    uint8_t gw[4] = {0};
                    uint32_t oif = (uint32_t)nhx->rtnh_ifindex;
                    int nlen = (int)nhx->rtnh_len - (int)sizeof(*nhx);
                    for (struct rtattr *na = RTNH_DATA(nhx); RTA_OK(na, nlen);
                         na = RTA_NEXT(na, nlen)) {
                        if (na->rta_type == RTA_GATEWAY &&
                            RTA_PAYLOAD(na) >= 4)
                            memcpy(gw, RTA_DATA(na), 4);
                    }
                    observed_count++;
                    if (!nl_verify_nh(expected, expected_count, gw, oif))
                        unmatched++;
                }
            }
        }

        if (!expected_blackhole && !have_gateway && !have_oif &&
            observed_count == 0 && expected_count > 0)
            return DANOS_ERR_VERIFY_FAIL;   /* unicast route without a path */

        /* A single-path route is reported with RTA_GATEWAY/RTA_OIF rather
         * than RTA_MULTIPATH. */
        if (observed_count == 0 && have_gateway) {
            observed_count = 1;
            if (!nl_verify_nh(expected, expected_count, single_gw, single_oif))
                unmatched++;
        } else if (observed_count == 0 && have_oif && expected_count > 0) {
            /* Interface-only next hop: the kernel reports RTA_OIF without a
             * gateway, which is still an observed path to match. */
            static const uint8_t zero_gw[4] = {0, 0, 0, 0};
            observed_count = 1;
            if (!nl_verify_nh(expected, expected_count, zero_gw, single_oif))
                unmatched++;
        }

        if (table != expected_table ||
            !ipv4_prefix_equal(dst_addr, desired->prefix.addr.addr,
                               desired->prefix.prefix_len) ||
            !route_type_matches || metric != desired->metric ||
            observed_count != expected_count || unmatched != 0)
            return DANOS_ERR_VERIFY_FAIL;
        return DANOS_OK;
    }
    return DANOS_ERR_NOT_FOUND;
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
    if (send(nl_fd, &msg, msg.nh.nlmsg_len, 0) < 0) return -1;
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
    const danos_route_t *route = &res->route;
    if (!route || route->prefix.addr.af != DANOS_AF_IPV4)
        return DANOS_ERR_INVALID_ARG;
    if (route->prefix.prefix_len > 32) return DANOS_ERR_INVALID_ARG;

    if (res->nh_count > DANOS_NHGROUP_MAX_NH)
        return DANOS_ERR_INVALID_ARG;
    if (!g_nl.real) {
        mock_route_t *r = mock_find(route->prefix.addr.addr,
                                    route->prefix.prefix_len,
                                    route->vrf_id);
        bool is_new = false;
        if (!r) {
            r = mock_alloc();
            if (!r) return DANOS_ERR_NO_MEMORY;
            is_new = true;
        }
        r->used = true;
        r->table_id = route->vrf_id;
        r->len = route->prefix.prefix_len;
        memcpy(r->addr, route->prefix.addr.addr, 4);
        r->blackhole = (route->flags & DANOS_ROUTE_FLAG_BLACKHOLE) != 0;
        r->metric = route->metric;
        r->nh_count = 0;
        memset(r->nh_gw, 0, sizeof(r->nh_gw));
        memset(r->nh_oif, 0, sizeof(r->nh_oif));
        if (!r->blackhole && res->nh_count > 0) {
            r->nh_count = res->nh_count > MOCK_ROUTE_MAX_NH
                              ? MOCK_ROUTE_MAX_NH : res->nh_count;
            for (uint32_t i = 0; i < r->nh_count; i++) {
                memcpy(r->nh_gw[i], res->nh_gw[i], 4);
                r->nh_oif[i] = res->nh_oif[i];
            }
        } else if (!r->blackhole && res->has_gw) {
            r->nh_count = 1;
            memcpy(r->nh_gw[0], res->gw, 4);
            r->nh_oif[0] = res->oif;
        }
        if (is_new) g_mock.route_count++;
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
    const danos_route_t *route = &res->route;
    if (!route || route->prefix.addr.af != DANOS_AF_IPV4)
        return DANOS_ERR_INVALID_ARG;
    if (res->nh_count > DANOS_NHGROUP_MAX_NH)
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
    /* kernel VRF devices are a platform concern; v0.9 accepts and
     * tracks only */
    return DANOS_OK;
}

static danos_status_t ops_vrf_del(danos_vrf_id_t vrf_id, void *user)
{
    (void)user; (void)vrf_id;
    return DANOS_OK;
}

/* Route-specific OPER readback: full attribute/ECMP equality. */
static danos_status_t ops_route_readback(
    const danos_resolved_route_t *resolved, void *out, void *user)
{
    (void)user;
    if (!resolved || !out) return DANOS_ERR_INVALID_ARG;
    const danos_route_t *route = &resolved->route;
    if (route->prefix.addr.af != DANOS_AF_IPV4 ||
        route->prefix.prefix_len > 32)
        return DANOS_ERR_NOT_SUPPORTED;

    if (g_nl.real && g_nl.initialized) {
        danos_status_t st = nl_route_verify(resolved);
        if (st != DANOS_OK) return st;
        memcpy(out, route, sizeof(*route));
        return DANOS_OK;
    }

    mock_route_t *r = mock_find(route->prefix.addr.addr,
                                route->prefix.prefix_len, route->vrf_id);
    if (!r) return DANOS_ERR_NOT_FOUND;

    bool expected_blackhole =
        (route->flags & DANOS_ROUTE_FLAG_BLACKHOLE) != 0;
    uint32_t expected_count = 0;
    if (!expected_blackhole) {
        if (resolved->nh_count > 0)
            expected_count = resolved->nh_count > MOCK_ROUTE_MAX_NH
                                 ? MOCK_ROUTE_MAX_NH : resolved->nh_count;
        else if (resolved->has_gw)
            expected_count = 1;
    }
    if (r->blackhole != expected_blackhole ||
        r->metric != route->metric || r->nh_count != expected_count)
        return DANOS_ERR_VERIFY_FAIL;

    for (uint32_t i = 0; i < expected_count; i++) {
        const uint8_t *gw = resolved->nh_count > 0
                                ? resolved->nh_gw[i] : resolved->gw;
        uint32_t oif = resolved->nh_count > 0
                           ? resolved->nh_oif[i] : resolved->oif;
        if (memcmp(r->nh_gw[i], gw, 4) != 0)
            return DANOS_ERR_VERIFY_FAIL;
        if (oif && r->nh_oif[i] && oif != r->nh_oif[i])
            return DANOS_ERR_VERIFY_FAIL;
    }
    memcpy(out, route, sizeof(*route));
    return DANOS_OK;
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
            danos_status_t st = nl_route_read(route);
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
        o->admin_up = d->admin_up;
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
    o->link_up = g_mock.iface_link_known[id] ? g_mock.iface_link[id]
                                             : g_mock.iface_up[id];
    return DANOS_OK;
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
    .route_readback = ops_route_readback,
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
