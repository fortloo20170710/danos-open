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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#define IFINDEX_MAP_MAX 256
static struct { uint32_t from, to; } g_ifindex_map[IFINDEX_MAP_MAX];
static unsigned g_ifindex_count;
static bool g_real;

/* Parse the entire map at install time: never accept a valid prefix followed
 * by junk, conflicting source indices, signs or numeric overflow. Multiple
 * Linux aliases may deliberately address the same VPP interface. */
static int parse_ifindex_map(void)
{
    g_ifindex_count = 0;
    const char *p = getenv("DANOS_VPP_IFINDEX_MAP");
    if (!p || !*p) return g_real ? -1 : 0;
    while (*p) {
        if (g_ifindex_count == IFINDEX_MAP_MAX || !isdigit((unsigned char)*p)) return -1;
        errno = 0;
        char *end;
        unsigned long from = strtoul(p, &end, 10);
        if (errno || !from || from > UINT32_MAX || *end != ':') return -1;
        p = end + 1;
        if (!isdigit((unsigned char)*p)) return -1;
        errno = 0;
        unsigned long to = strtoul(p, &end, 10);
        if (errno || !to || to >= UINT32_MAX || (*end && *end != ',')) return -1;
        for (unsigned i = 0; i < g_ifindex_count; i++)
            if (g_ifindex_map[i].from == from) return -1;
        g_ifindex_map[g_ifindex_count].from = (uint32_t)from;
        g_ifindex_map[g_ifindex_count++].to = (uint32_t)to;
        if (*end == 0) return 0;
        p = end + 1;
        if (!*p) return -1;
    }
    return 0;
}

static danos_status_t map_ifindex(uint32_t from, uint32_t *to)
{
    if (!from) return DANOS_ERR_INVALID_ARG;
    for (unsigned i = 0; i < g_ifindex_count; i++)
        if (g_ifindex_map[i].from == from) {
            *to = g_ifindex_map[i].to;
            return DANOS_OK;
        }
    /* Identity is a mock fixture convenience only, never a real mapping. */
    if (!g_real && !g_ifindex_count) { *to = from; return DANOS_OK; }
    return DANOS_ERR_NOT_FOUND;
}

static danos_status_t vpp_adapter_iface_up(danos_ifindex_t ifindex,
                                           bool up, void *user)
{
    (void)user;
    uint32_t index;
    danos_status_t st = map_ifindex(ifindex, &index);
    if (st != DANOS_OK) return st;
    return vpp_msg_sw_interface_set_flags(index, up);
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
    uint32_t index;
    danos_status_t st = map_ifindex(iface->ifindex, &index);
    if (st != DANOS_OK) return st;
    return vpp_msg_sw_interface_add_del_address(index, is_add, &prefix);
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
    if (!res) return DANOS_ERR_INVALID_ARG;
    const danos_route_t *r = &res->route;
    if (r->prefix.addr.af != DANOS_AF_IPV4 || (r->flags & DANOS_ROUTE_FLAG_BLACKHOLE))
        return DANOS_ERR_NOT_SUPPORTED;
    if (r->prefix.prefix_len > 32) return DANOS_ERR_INVALID_ARG;

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
        danos_status_t st = map_ifindex(res->nh_count ? res->nh_oif[i] : res->oif, &nh_ifs[i]);
        if (st != DANOS_OK) return st;
    }

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
    if (!res) return DANOS_ERR_INVALID_ARG;
    const danos_route_t *r = &res->route;
    if (r->prefix.addr.af != DANOS_AF_IPV4 || (r->flags & DANOS_ROUTE_FLAG_BLACKHOLE))
        return DANOS_ERR_NOT_SUPPORTED;
    if (r->prefix.prefix_len > 32) return DANOS_ERR_INVALID_ARG;

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

static danos_status_t vpp_adapter_readback(danos_obj_type_t type, danos_obj_id_t id,
                                           const void *desired, size_t size,
                                           void *out, void *user)
{
    (void)id; (void)user;
    if (type != DANOS_OBJ_IFACE || !g_real) return DANOS_ERR_NOT_SUPPORTED;
    if (!desired || !out || size != sizeof(danos_iface_t)) return DANOS_ERR_INVALID_ARG;
    const danos_iface_t *iface = desired;
    uint32_t index;
    danos_status_t st = map_ifindex(iface->ifindex, &index);
    if (st != DANOS_OK) return DANOS_ERR_INVALID_ARG; /* configuration, not dataplane loss */
    vpp_iface_state_t state;
    st = vpp_msg_iface_read(index, &state);
    if (st != DANOS_OK) return st;
    danos_iface_t *observed = out;
    *observed = *iface;
    observed->admin_up = state.admin_up;
    observed->link_up = state.link_up;
    observed->mtu = state.mtu;
    memcpy(observed->mac, state.mac, sizeof(observed->mac));
    return DANOS_OK;
}

static bool prefix_matches(const uint8_t *a, const uint8_t *b, uint8_t bits)
{
    for (unsigned i = 0; i < bits; i++)
        if ((a[i / 8] ^ b[i / 8]) & (0x80u >> (i % 8))) return false;
    return true;
}

static danos_status_t vpp_adapter_route_verify(const danos_resolved_route_t *res, void *user)
{
    (void)user;
    if (!g_real) return DANOS_ERR_NOT_SUPPORTED;
    if (!res || res->route.prefix.prefix_len > 32 || res->nh_count > 64)
        return DANOS_ERR_INVALID_ARG;
    if (res->route.prefix.addr.af != DANOS_AF_IPV4 ||
        (res->route.flags & DANOS_ROUTE_FLAG_BLACKHOLE)) return DANOS_ERR_NOT_SUPPORTED;
    uint32_t count = res->nh_count ? res->nh_count : 1;
    uint32_t interfaces[64];
    for (unsigned i = 0; i < count; i++)
        if (map_ifindex(res->nh_count ? res->nh_oif[i] : res->oif, &interfaces[i]) != DANOS_OK)
            return DANOS_ERR_INVALID_ARG;
    vpp_prefix_t prefix = { .len = res->route.prefix.prefix_len };
    memcpy(prefix.addr.addr, res->route.prefix.addr.addr, 4);
    vpp_route_state_t state;
    danos_status_t st = vpp_msg_route_read(res->route.vrf_id, &prefix, &state);
    if (st != DANOS_OK) return st;
    if (state.table_id != res->route.vrf_id || state.prefix.addr.is_ipv6 ||
        state.prefix.len != prefix.len ||
        !prefix_matches(state.prefix.addr.addr, prefix.addr.addr, prefix.len) ||
        state.n_paths != count) return DANOS_ERR_VERIFY_FAIL;
    bool matched[64] = {false};
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *gw = res->nh_count ? res->nh_gw[i] : res->gw;
        bool found = false;
        bool attached = gw[0] == 0 && gw[1] == 0 && gw[2] == 0 && gw[3] == 0;
        for (unsigned j = 0; j < count; j++) {
            if (!matched[j] && state.nh_ifs[j] == interfaces[i] &&
                memcmp(state.nhs[j].addr, gw, 4) == 0 && state.types[j] == 0 &&
                /* VPP normalizes RESOLVE_VIA_ATTACHED to flags=0
                 * for an explicit interface and all-zero gateway. */
                state.protocols[j] == 0 &&
                (state.flags[j] == 0 || (attached && state.flags[j] == 1)) &&
                state.weights[j] == 1 &&
                state.preferences[j] == 0 && state.n_labels[j] == 0 &&
                state.nh_tables[j] == res->route.vrf_id) {
                matched[j] = true; found = true; break;
            }
        }
        if (!found) return DANOS_ERR_VERIFY_FAIL;
    }
    return DANOS_OK;
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
    .readback = vpp_adapter_readback,
    .route_verify = vpp_adapter_route_verify,
    .user      = NULL,
};

/* Connect the VPP binary API (mock or real) and install the ops. */
int danos_vpp_adapter_install(bool real, const char *sock_path)
{
    g_real = real;
    if (parse_ifindex_map() != 0) {
        fprintf(stderr, "vpp: explicit valid DANOS_VPP_IFINDEX_MAP required for real backend\n");
        return -1;
    }
    danos_vpp_api_init();
    if (!real) {
        danos_vpp_api_enable_mock();
    } else {
        if (sock_path) danos_vpp_api_set_sock_path(sock_path);
        if (danos_vpp_api_connect() != 0) return -1;
    }
    danos_backend_ops_set(&g_vpp_ops);
    /* Advertise what this backend can program. Without this the capability
     * registry stays empty and anything doing backend selection has nothing
     * to select on. */
    (void)danos_vpp_capability_register();
    return 0;
}
