/*
 * DANOS-Open Backend: VPP adapter for the programming pipeline
 * (v0.11, ADR-0007).
 *
 * Implements danos_backend_ops_t over the existing VPP binary-API
 * message layer (vpp_msgs). Registers itself so the reconciler can
 * drive VPP as a second backend alongside netlink.
 *
 * Proof point: the same pipeline that programs the kernel FIB (K4)
 * can drive VPP with only an adapter — no pipeline changes.
 */

#include <danos/core/backend_ops.h>
#include <danos/dpa.h>
#include "capability/vpp_capability.h"
#include "api/vpp_api.h"
#include "api/vpp_msgs.h"
#include "api/vpp_dump.h"
#include "api/vpp_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t vpp_adapter_map_ifindex(uint32_t ifindex);

static danos_status_t vpp_adapter_iface_up(danos_ifindex_t ifindex,
                                           bool up, void *user)
{
    (void)user;
    return vpp_msg_sw_interface_set_flags(vpp_adapter_map_ifindex(ifindex), up);
}

/* FRR/Linux ifindexes are namespace-local and must not be assumed to equal
 * VPP sw_if_index values.  The integration topology supplies a compact,
 * explicit mapping such as DANOS_VPP_IFINDEX_MAP=2:1,3:2. */
static uint32_t vpp_adapter_map_ifindex(uint32_t ifindex)
{
    const char *map = getenv("DANOS_VPP_IFINDEX_MAP");
    if (!map || !*map) return ifindex;
    const char *p = map;
    while (*p) {
        char *end = NULL;
        unsigned long from = strtoul(p, &end, 10);
        if (end == p || *end != ':') break;
        p = end + 1;
        unsigned long to = strtoul(p, &end, 10);
        if (end == p) break;
        if (from == ifindex && to <= UINT32_MAX) return (uint32_t)to;
        p = (*end == ',') ? end + 1 : end;
    }
    return ifindex;
}

static danos_status_t vpp_adapter_iface_addr_one(const danos_iface_t *iface,
                                                 const danos_ip_prefix_t *p,
                                                 bool is_add)
{
    vpp_prefix_t prefix;
    memset(&prefix, 0, sizeof(prefix));
    prefix.addr.is_ipv6 = p->addr.af == DANOS_AF_IPV6;
    memcpy(prefix.addr.addr, p->addr.addr, prefix.addr.is_ipv6 ? 16 : 4);
    prefix.len = p->prefix_len;
    return vpp_msg_sw_interface_add_del_address(vpp_adapter_map_ifindex(iface->ifindex), is_add, &prefix);
}

static danos_status_t vpp_adapter_iface_addr_set(const danos_iface_t *iface,
                                                 void *user)
{
    (void)user;
    if (iface->ipv4_address.addr.af != DANOS_AF_UNSPEC) {
        danos_status_t st = vpp_adapter_iface_addr_one(iface, &iface->ipv4_address, true);
        if (st != DANOS_OK) return st;
    }
    if (iface->ipv6_address.addr.af != DANOS_AF_UNSPEC)
        return vpp_adapter_iface_addr_one(iface, &iface->ipv6_address, true);
    return DANOS_OK;
}

static danos_status_t vpp_adapter_iface_addr_del(const danos_iface_t *iface,
                                                 void *user)
{
    (void)user;
    if (iface->ipv4_address.addr.af != DANOS_AF_UNSPEC) {
        danos_status_t st = vpp_adapter_iface_addr_one(iface, &iface->ipv4_address, false);
        if (st != DANOS_OK) return st;
    }
    if (iface->ipv6_address.addr.af != DANOS_AF_UNSPEC)
        return vpp_adapter_iface_addr_one(iface, &iface->ipv6_address, false);
    return DANOS_OK;
}

static danos_status_t vpp_adapter_route_add(const danos_resolved_route_t *res,
                                            void *user)
{
    (void)user;
    const danos_route_t *r = &res->route;
    if (r->prefix.addr.af != DANOS_AF_IPV4) return DANOS_ERR_NOT_SUPPORTED;

    vpp_prefix_t prefix;
    memset(&prefix, 0, sizeof(prefix));
    prefix.addr.is_ipv6 = false;
    memcpy(prefix.addr.addr, r->prefix.addr.addr, 4);
    prefix.len = r->prefix.prefix_len;

    vpp_ip_t nhs[64];
    uint32_t nh_ifs[64];
    uint32_t count = res->nh_count ? res->nh_count : 1;
    if (count > 64) return DANOS_ERR_INVALID_ARG;
    memset(nhs, 0, sizeof(nhs));
    for (uint32_t i = 0; i < count; i++) {
        nhs[i].is_ipv6 = false;
        memcpy(nhs[i].addr, res->nh_count ? res->nh_gw[i] : res->gw, 4);
        nh_ifs[i] = vpp_adapter_map_ifindex(res->nh_count ? res->nh_oif[i] : res->oif);
        if (nh_ifs[i] == 0) nh_ifs[i] = 1;
    }

    /* A single zero-gateway path is the connected route for an interface
     * address. VPP creates it when the address is programmed; adding it again
     * as a zero-gateway path fails, especially during ledger replay after a
     * VPP restart. Mark it handled here. */
    if (count == 1 && !nhs[0].addr[0] && !nhs[0].addr[1] &&
        !nhs[0].addr[2] && !nhs[0].addr[3])
        return DANOS_OK;

    const char *debug = getenv("DANOS_VPP_DEBUG");
    if (debug && debug[0] == '1') {
        fprintf(stderr, "vpp route add vrf=%u prefix-len=%u gw=%u.%u.%u.%u if=%u\n",
                r->vrf_id, prefix.len, nhs[0].addr[0], nhs[0].addr[1],
                nhs[0].addr[2], nhs[0].addr[3], nh_ifs[0]);
    }

    return vpp_msg_ip_route_add_del(true, r->vrf_id, &prefix,
                                    count, nhs, nh_ifs);
}

static danos_status_t vpp_adapter_route_del(const danos_resolved_route_t *res,
                                            void *user)
{
    (void)user;
    const danos_route_t *r = &res->route;
    if (r->prefix.addr.af != DANOS_AF_IPV4) return DANOS_ERR_NOT_SUPPORTED;

    vpp_prefix_t prefix;
    memset(&prefix, 0, sizeof(prefix));
    prefix.addr.is_ipv6 = false;
    memcpy(prefix.addr.addr, r->prefix.addr.addr, 4);
    prefix.len = r->prefix.prefix_len;

    vpp_ip_t nh;
    memset(&nh, 0, sizeof(nh));
    uint32_t nh_if = 1;
    return vpp_msg_ip_route_add_del(false, r->vrf_id, &prefix,
                                    1, &nh, &nh_if);
}

static danos_status_t vpp_adapter_vrf_add(const danos_vrf_t *vrf, void *user)
{
    (void)user;
    return vpp_msg_ip_table_add_del(vrf->vrf_id, false, vrf->name, true);
}

static danos_status_t vpp_adapter_vrf_del(danos_vrf_id_t vrf_id, void *user)
{
    (void)user;
    return vpp_msg_ip_table_add_del(vrf_id, false, NULL, false);
}

/* =========================================================================
 * OPER readback from VPP dumps
 * ========================================================================= */

#define VPP_IF_STATUS_ADMIN_UP 0x1u
#define VPP_IF_STATUS_LINK_UP  0x2u
#define VPP_FIB_PATH_NORMAL    0u
#define VPP_FIB_PATH_DROP      2u

struct iface_scan {
    uint32_t want;
    bool     found;
    vpp_iface_dump_t hit;
};

static int iface_scan_cb(const uint8_t *body, uint32_t len, void *user)
{
    struct iface_scan *scan = user;
    vpp_iface_dump_t entry;
    if (vpp_dump_decode_iface(body, len, &entry) != 0) return 0;
    if (entry.sw_if_index == scan->want && !scan->found) {
        scan->hit = entry;
        scan->found = true;
    }
    return 0;
}

static danos_status_t vpp_iface_readback(danos_obj_id_t id,
                                         const danos_iface_t *desired,
                                         danos_iface_t *out)
{
    vpp_buf_t req;
    vpp_buf_init(&req, 16);
    vpp_buf_put_u32(&req, 0xFFFFFFFFu);   /* all interfaces */
    vpp_buf_put_u8(&req, 0);              /* name_filter_valid */
    vpp_buf_put_u32(&req, 0);             /* empty name_filter string */
    struct iface_scan scan = { .want = vpp_adapter_map_ifindex((uint32_t)id) };
    int n = danos_vpp_api_dump("sw_interface_dump", "sw_interface_details",
                               req.data, req.len, iface_scan_cb, &scan);
    vpp_buf_free(&req);
    if (n < 0) return DANOS_ERR_BACKEND_IO;
    if (!scan.found) return DANOS_ERR_NOT_FOUND;

    *out = *desired;
    if (scan.hit.name[0])
        snprintf(out->name, sizeof(out->name), "%s", scan.hit.name);
    if (scan.hit.mtu) out->mtu = scan.hit.mtu;
    out->admin_up = (scan.hit.flags & VPP_IF_STATUS_ADMIN_UP) != 0;
    out->link_up = (scan.hit.flags & (VPP_IF_STATUS_ADMIN_UP |
                                      VPP_IF_STATUS_LINK_UP)) ==
                   (VPP_IF_STATUS_ADMIN_UP | VPP_IF_STATUS_LINK_UP);
    return DANOS_OK;
}

struct table_scan {
    uint32_t want;
    bool     found;
    vpp_table_dump_t hit;
};

static int table_scan_cb(const uint8_t *body, uint32_t len, void *user)
{
    struct table_scan *scan = user;
    vpp_table_dump_t entry;
    if (vpp_dump_decode_table(body, len, &entry) != 0) return 0;
    if (entry.table_id == scan->want && !scan->found) {
        scan->hit = entry;
        scan->found = true;
    }
    return 0;
}

static danos_status_t vpp_vrf_readback(danos_obj_id_t id,
                                       const danos_vrf_t *desired,
                                       danos_vrf_t *out)
{
    struct table_scan scan = { .want = (uint32_t)id };
    int n = danos_vpp_api_dump("ip_table_dump", "ip_table_details",
                               NULL, 0, table_scan_cb, &scan);
    if (n < 0) return DANOS_ERR_BACKEND_IO;
    if (!scan.found) return DANOS_ERR_NOT_FOUND;

    *out = *desired;
    if (scan.hit.name[0])
        snprintf(out->name, sizeof(out->name), "%s", scan.hit.name);
    out->ipv4_active = !scan.hit.is_ip6;
    out->ipv6_active = scan.hit.is_ip6;
    return DANOS_OK;
}

struct route_scan {
    uint32_t table_id;
    bool     have_prefix;
    uint8_t  prefix[16];
    uint8_t  prefix_len;
    vpp_route_dump_t hit;
};

static int route_scan_cb(const uint8_t *body, uint32_t len, void *user)
{
    struct route_scan *scan = user;
    vpp_route_dump_t entry;
    if (vpp_dump_decode_route(body, len, &entry) != 0) return 0;
    if (entry.is_ipv6 || entry.table_id != scan->table_id) return 0;
    if (entry.prefix_len != scan->prefix_len ||
        memcmp(entry.prefix, scan->prefix, 4) != 0)
        return 0;
    if (!scan->have_prefix) {
        scan->hit = entry;
        scan->have_prefix = true;
    }
    return 0;
}

static bool path_matches(const vpp_route_dump_t *route, uint32_t *used,
                         const uint8_t gw[4], uint32_t oif, bool blackhole)
{
    uint32_t want_type = blackhole ? VPP_FIB_PATH_DROP : VPP_FIB_PATH_NORMAL;
    for (uint32_t i = 0; i < route->n_paths; i++) {
        if (used[i]) continue;
        if (route->paths[i].type != want_type) continue;
        if (!blackhole) {
            if (route->paths[i].sw_if_index != oif) continue;
            if (memcmp(route->paths[i].gw, gw, 4) != 0) continue;
        }
        used[i] = 1;
        return true;
    }
    return false;
}

static danos_status_t vpp_route_readback(
    const danos_resolved_route_t *resolved, void *out, void *user)
{
    (void)user;
    if (!resolved || !out) return DANOS_ERR_INVALID_ARG;
    const danos_route_t *route = &resolved->route;
    if (route->prefix.addr.af != DANOS_AF_IPV4 ||
        route->prefix.prefix_len > 32)
        return DANOS_ERR_NOT_SUPPORTED;
    /* VPP's FIB dump exposes paths and prefix, but not a route metric.
     * Claiming OK after path equality would silently report the metric as
     * verified, so non-zero metrics are an explicit unsupported result. */
    if (route->metric != 0)
        return DANOS_ERR_NOT_SUPPORTED;

    uint8_t req[69];                      /* ip_table { u32; bool; name[64] } */
    memset(req, 0, sizeof(req));
    req[0] = (uint8_t)(route->vrf_id >> 24);
    req[1] = (uint8_t)(route->vrf_id >> 16);
    req[2] = (uint8_t)(route->vrf_id >> 8);
    req[3] = (uint8_t)route->vrf_id;
    req[4] = 0;                           /* IPv4 table */

    struct route_scan scan;
    memset(&scan, 0, sizeof(scan));
    scan.table_id = route->vrf_id;
    scan.prefix_len = route->prefix.prefix_len;
    memcpy(scan.prefix, route->prefix.addr.addr, 4);
    int n = danos_vpp_api_dump("ip_route_dump", "ip_route_details",
                               req, sizeof(req), route_scan_cb, &scan);
    if (n < 0) return DANOS_ERR_BACKEND_IO;
    if (!scan.have_prefix) return DANOS_ERR_NOT_FOUND;

    const bool blackhole =
        (route->flags & DANOS_ROUTE_FLAG_BLACKHOLE) != 0;
    uint32_t expected = 0;
    if (!blackhole) {
        expected = resolved->nh_count ? resolved->nh_count : 1;
        if (expected > VPP_DUMP_MAX_PATHS) return DANOS_ERR_INVALID_ARG;
    } else {
        expected = 1;                     /* one DROP path */
    }
    if (scan.hit.n_paths != expected) return DANOS_ERR_VERIFY_FAIL;

    uint32_t used[VPP_DUMP_MAX_PATHS] = {0};
    if (blackhole) {
        if (!path_matches(&scan.hit, used, NULL, 0, true))
            return DANOS_ERR_VERIFY_FAIL;
    } else {
        for (uint32_t i = 0; i < expected; i++) {
            const uint8_t *gw = resolved->nh_count
                                    ? resolved->nh_gw[i] : resolved->gw;
            uint32_t oif = vpp_adapter_map_ifindex(
                resolved->nh_count ? resolved->nh_oif[i] : resolved->oif);
            if (!path_matches(&scan.hit, used, gw, oif, false))
                return DANOS_ERR_VERIFY_FAIL;
        }
    }
    memcpy(out, route, sizeof(*route));
    return DANOS_OK;
}

static danos_status_t vpp_adapter_readback(danos_obj_type_t type,
                                           danos_obj_id_t id,
                                           const void *desired, size_t size,
                                           void *out, void *user)
{
    (void)user;
    /* Without injected mock dumps a mock backend has no dump store; that is
     * "no readback capability", not "object absent". */
    if (danos_vpp_api_is_mock() && danos_vpp_api_mock_dump_total() == 0)
        return DANOS_ERR_NOT_SUPPORTED;
    if (!desired || !out) return DANOS_ERR_INVALID_ARG;
    if (type == DANOS_OBJ_IFACE) {
        if (size < sizeof(danos_iface_t)) return DANOS_ERR_INVALID_ARG;
        return vpp_iface_readback(id, desired, out);
    }
    if (type == DANOS_OBJ_VRF) {
        if (size < sizeof(danos_vrf_t)) return DANOS_ERR_INVALID_ARG;
        return vpp_vrf_readback(id, desired, out);
    }
    return DANOS_ERR_NOT_SUPPORTED;
}

static danos_status_t vpp_adapter_route_readback(
    const danos_resolved_route_t *resolved, void *out, void *user)
{
    (void)user;
    if (danos_vpp_api_is_mock() && danos_vpp_api_mock_dump_total() == 0)
        return DANOS_ERR_NOT_SUPPORTED;
    return vpp_route_readback(resolved, out, NULL);
}

static danos_backend_ops_t g_vpp_ops = {
    .name      = "vpp",
    .iface_up  = vpp_adapter_iface_up,
    .iface_addr_set = vpp_adapter_iface_addr_set,
    .iface_addr_del = vpp_adapter_iface_addr_del,
    .route_add = vpp_adapter_route_add,
    .route_del = vpp_adapter_route_del,
    .vrf_add   = vpp_adapter_vrf_add,
    .vrf_del   = vpp_adapter_vrf_del,
    .readback  = vpp_adapter_readback,
    .route_readback = vpp_adapter_route_readback,
    .user      = NULL,
};

/* Connect the VPP binary API (mock or real) and install the ops. */
int danos_vpp_adapter_install(bool real, const char *sock_path)
{
    /* A caller that already connected (the live conformance test) keeps its
     * connection and stat-socket configuration. */
    if (!danos_vpp_api_is_connected()) {
        danos_vpp_api_init();
        if (!real) {
            danos_vpp_api_enable_mock();
        } else {
            if (sock_path) danos_vpp_api_set_sock_path(sock_path);
            if (danos_vpp_api_connect() != 0) return -1;
        }
    }
    danos_backend_ops_set(&g_vpp_ops);
    /* Advertise what this backend can program. Without this the capability
     * registry stays empty and anything doing backend selection has nothing
     * to select on. */
    (void)danos_vpp_capability_register();
    return 0;
}
