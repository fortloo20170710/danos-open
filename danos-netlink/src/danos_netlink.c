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

/* Build and send RTM_NEWROUTE for an IPv4 route (kernel table_id). */
static int nl_route_msg(const danos_resolved_route_t *res, int add)
{
    struct {
        struct nlmsghdr nh;
        struct rtmsg    rt;
        char            attrbuf[256];
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

    /* RTA_GW + RTA_OIF when the pipeline resolved a next hop */
    if (add && res->has_gw) {
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
    const danos_route_t *route = &res->route;
    if (!route || route->prefix.addr.af != DANOS_AF_IPV4)
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

/* Operational readback: interfaces only. Routes and VRFs have no query
 * binding in this adapter, so they report NOT_SUPPORTED and contribute no
 * OPER copy. */
static danos_status_t ops_readback(danos_obj_type_t type, danos_obj_id_t id,
                                   const void *desired, size_t size,
                                   void *out, void *user)
{
    (void)user;
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
