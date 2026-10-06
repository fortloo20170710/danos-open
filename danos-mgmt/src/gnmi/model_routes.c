/*
 * DANOS-Open Management: Composite Route Management implementation (v0.12)
 */

#include "model_routes.h"
#include <danos/core/object_registry.h>
#include <danos/core/transaction.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

danos_status_t gnmi_parse_ipv4(const char *s, danos_ip_addr_t *out)
{
    if (!s || !out) return DANOS_ERR_INVALID_ARG;
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return DANOS_ERR_INVALID_ARG;
    if (a > 255 || b > 255 || c > 255 || d > 255) return DANOS_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->af = DANOS_AF_IPV4;
    out->addr[0] = (uint8_t)a;
    out->addr[1] = (uint8_t)b;
    out->addr[2] = (uint8_t)c;
    out->addr[3] = (uint8_t)d;
    return DANOS_OK;
}

danos_status_t gnmi_parse_prefix(const char *s, danos_ip_prefix_t *out)
{
    if (!s || !out) return DANOS_ERR_INVALID_ARG;
    char ipbuf[64];
    if (strlen(s) >= sizeof(ipbuf)) return DANOS_ERR_INVALID_ARG;
    strncpy(ipbuf, s, sizeof(ipbuf) - 1);
    ipbuf[sizeof(ipbuf) - 1] = '\0';
    char *slash = strchr(ipbuf, '/');
    if (!slash) return DANOS_ERR_INVALID_ARG;
    *slash = '\0';
    char *end;
    unsigned long len = strtoul(slash + 1, &end, 10);
    if (end == slash + 1 || *end || len > 32) return DANOS_ERR_INVALID_ARG;
    danos_status_t st = gnmi_parse_ipv4(ipbuf, &out->addr);
    if (st != DANOS_OK) return st;
    out->prefix_len = (uint8_t)len;
    return DANOS_OK;
}

danos_status_t gnmi_format_prefix(const danos_ip_prefix_t *prefix, char *out, size_t cap)
{
    if (!prefix || !out || !cap) return DANOS_ERR_INVALID_ARG;
    int af = prefix->addr.af == DANOS_AF_IPV4 ? AF_INET :
             prefix->addr.af == DANOS_AF_IPV6 ? AF_INET6 : 0;
    char ip[INET6_ADDRSTRLEN];
    if (!af || !inet_ntop(af, prefix->addr.addr, ip, sizeof(ip))) return DANOS_ERR_INVALID_ARG;
    int n = snprintf(out, cap, "%s/%u", ip, prefix->prefix_len);
    return n >= 0 && (size_t)n < cap ? DANOS_OK : DANOS_ERR_INVALID_ARG;
}

/* Candidate composition: all discovery includes staged additions/deletes. */
static void max_id_iter(danos_object_entry_t *e, void *user)
{
    if (e->id > *(danos_obj_id_t *)user) *(danos_obj_id_t *)user = e->id;
}

static danos_status_t candidate_id(danos_tx_t *tx, danos_obj_type_t type,
                                   danos_obj_id_t *out)
{
    danos_obj_id_t max = 100;
    danos_status_t st = danos_tx_iterate_objects(tx, type, max_id_iter, &max);
    if (st != DANOS_OK) return st;
    if (max == UINT64_MAX) return DANOS_ERR_INVALID_ARG;
    *out = max + 1;
    return DANOS_OK;
}

danos_status_t gnmi_route_stage_set(danos_tx_t *tx, danos_vrf_id_t vrf_id,
    const danos_ip_prefix_t *prefix, const danos_ip_addr_t *gateways,
    const uint32_t *oifs, uint32_t count, bool replace)
{
    if (!tx || !prefix || !gateways || !oifs || !count || count > 64 ||
        !g_default_store) return DANOS_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < count; i++)
        if (gateways[i].af != DANOS_AF_IPV4) return DANOS_ERR_INVALID_ARG;

    danos_route_t old;
    danos_status_t st = danos_route_read(tx, vrf_id, *prefix, DANOS_ROUTE_PROTO_STATIC, &old);
    bool exists = st == DANOS_OK;
    if (!exists && st != DANOS_ERR_NOT_FOUND) return st;
    danos_nhgroup_t before = {0}, grp = {0};
    if (exists) {
        st = danos_nhgroup_read(tx, old.nhgroup_id, &before);
        if (st != DANOS_OK || before.nh_count > 64) return DANOS_ERR_INVALID_ARG;
        grp.id = before.id;
    } else {
        st = candidate_id(tx, DANOS_OBJ_NHGROUP, &grp.id);
        if (st != DANOS_OK) return st;
    }
    grp.nh_count = count;
    for (uint32_t i = 0; i < count; i++) {
        danos_nexthop_t nh = {0};
        bool reuse = exists && i < before.nh_count;
        if (reuse) nh.id = before.nh_ids[i];
        else {
            st = candidate_id(tx, DANOS_OBJ_NEXTHOP, &nh.id);
            if (st != DANOS_OK) return st;
        }
        nh.gateway = gateways[i];
        nh.ifindex = oifs[i];
        nh.weight = count > 1 ? 1 : 0;
        nh.flags = count > 1 ? DANOS_NH_FLAG_ECMP : 0;
        st = reuse ? danos_nh_update(tx, &nh) : danos_nh_create(tx, &nh);
        if (st != DANOS_OK) return st;
        grp.nh_ids[i] = nh.id;
    }
    for (uint32_t i = count; i < before.nh_count; i++) {
        st = danos_nh_delete(tx, before.nh_ids[i]);
        if (st != DANOS_OK) return st;
    }
    st = exists ? danos_nhgroup_update(tx, &grp) : danos_nhgroup_create(tx, &grp);
    if (st != DANOS_OK) return st;
    danos_route_t route = {0};
    if (exists && !replace) route = old;
    else route.admin_distance = 1;
    route.vrf_id = vrf_id;
    route.prefix = *prefix;
    route.protocol = DANOS_ROUTE_PROTO_STATIC;
    route.nhgroup_id = grp.id;
    return exists ? danos_route_update(tx, &route) : danos_route_create(tx, &route);
}

danos_status_t gnmi_route_stage_delete(danos_tx_t *tx, danos_vrf_id_t vrf_id,
                                       const danos_ip_prefix_t *prefix)
{
    if (!tx || !prefix || !g_default_store) return DANOS_ERR_INVALID_ARG;
    danos_route_t route;
    danos_status_t st = danos_route_read(tx, vrf_id, *prefix, DANOS_ROUTE_PROTO_STATIC, &route);
    if (st == DANOS_ERR_NOT_FOUND) return DANOS_OK;
    if (st != DANOS_OK) return st;
    danos_nhgroup_t grp;
    st = danos_nhgroup_read(tx, route.nhgroup_id, &grp);
    if (st != DANOS_OK || grp.nh_count > 64) return DANOS_ERR_INVALID_ARG;
    st = danos_route_delete(tx, vrf_id, *prefix, DANOS_ROUTE_PROTO_STATIC);
    if (st == DANOS_OK) st = danos_nhgroup_delete(tx, grp.id);
    for (uint32_t i = 0; i < grp.nh_count && st == DANOS_OK; i++)
        st = danos_nh_delete(tx, grp.nh_ids[i]);
    return st;
}

static danos_status_t finish_route_tx(danos_tx_t *tx, danos_status_t st)
{
    if (st == DANOS_OK) st = danos_tx_prepare(tx);
    if (st == DANOS_OK) st = danos_tx_validate(tx);
    if (st == DANOS_OK) st = danos_tx_commit(tx);
    if (st != DANOS_OK) danos_tx_abort(tx);
    return st;
}

danos_status_t gnmi_route_set(danos_vrf_id_t vrf_id,
    const danos_ip_prefix_t *prefix, const danos_ip_addr_t *gateway, uint32_t oif)
{
    danos_tx_t tx;
    danos_status_t st = danos_tx_begin(&tx, "gnmi-route", NULL);
    if (st != DANOS_OK) return st;
    return finish_route_tx(&tx, gnmi_route_stage_set(&tx, vrf_id, prefix, gateway, &oif, 1, false));
}

danos_status_t gnmi_route_set_ecmp(danos_vrf_id_t vrf_id,
    const danos_ip_prefix_t *prefix, const danos_ip_addr_t *gateways,
    const uint32_t *oifs, uint32_t count)
{
    danos_tx_t tx;
    danos_status_t st = danos_tx_begin(&tx, "gnmi-route-ecmp", NULL);
    if (st != DANOS_OK) return st;
    return finish_route_tx(&tx, gnmi_route_stage_set(&tx, vrf_id, prefix, gateways, oifs, count, false));
}

danos_status_t gnmi_route_delete(danos_vrf_id_t vrf_id, const danos_ip_prefix_t *prefix)
{
    danos_tx_t tx;
    danos_status_t st = danos_tx_begin(&tx, "gnmi-route-del", NULL);
    if (st != DANOS_OK) return st;
    return finish_route_tx(&tx, gnmi_route_stage_delete(&tx, vrf_id, prefix));
}

/* ---- render ---------------------------------------------------------------- */

danos_status_t gnmi_route_to_json_store(danos_object_store_t *store,
    const danos_route_t *route, char *out, size_t cap)
{
    if (!store || !route || !out || !cap) return DANOS_ERR_INVALID_ARG;
    char pfx[64];
    if (gnmi_format_prefix(&route->prefix, pfx, sizeof(pfx)) != DANOS_OK)
        return DANOS_ERR_INVALID_ARG;

    /* resolve gateway/oif via the group's first NH */
    char gw[64] = "none";
    uint32_t oif = 0;
    if (route->nhgroup_id) {
        danos_nhgroup_t grp;
        size_t sz = sizeof(grp);
        if (route->nhgroup_id &&
            danos_object_read(store, DANOS_OBJ_NHGROUP,
                              route->nhgroup_id, &grp, &sz) == DANOS_OK &&
            grp.nh_count > 0) {
            danos_nexthop_t nh;
            sz = sizeof(nh);
            if (danos_object_read(store, DANOS_OBJ_NEXTHOP,
                                  grp.nh_ids[0], &nh, &sz) == DANOS_OK &&
                nh.gateway.af == DANOS_AF_IPV4) {
                snprintf(gw, sizeof(gw), "%u.%u.%u.%u",
                         nh.gateway.addr[0], nh.gateway.addr[1],
                         nh.gateway.addr[2], nh.gateway.addr[3]);
                oif = nh.ifindex;
            }
        }
    }

    snprintf(out, cap,
             "{\"prefix\":\"%s\",\"vrf\":%u,\"gateway\":\"%s\",\"oif\":%u}",
             pfx, route->vrf_id, gw, oif);
    return DANOS_OK;
}

danos_status_t gnmi_route_to_json(const danos_route_t *route, char *out, size_t cap)
{
    return gnmi_route_to_json_store(g_default_store, route, out, cap);
}
