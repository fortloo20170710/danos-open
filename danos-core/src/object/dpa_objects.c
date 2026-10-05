/*
 * DANOS-Open Core: DPA Object CRUD API implementation
 *
 * Implements the public DPA object functions from danos/dpa.h.
 * For v0.1, these operate on the in-memory object store.
 * In production, they dispatch to the appropriate backend.
 */

#include <danos/dpa.h>
#include <danos/core/object_registry.h>
#include <danos/core/transaction.h>
#include <string.h>
#include <stdlib.h>

/* Lazy-init default store */
static danos_object_store_t *get_default_store(void)
{
    if (!g_default_store) {
        g_default_store = danos_object_store_create(1024);
    }
    return g_default_store;
}

static danos_status_t dpa_create(danos_tx_t *tx, danos_object_store_t *store,
                                 danos_obj_type_t type, danos_obj_id_t id,
                                 const void *data, size_t size)
{
    (void)store;
    return danos_tx_stage_object(tx, type, id, data, size, DANOS_TX_OP_CREATE);
}

static danos_status_t dpa_update(danos_tx_t *tx, danos_object_store_t *store,
                                 danos_obj_type_t type, danos_obj_id_t id,
                                 const void *data, size_t size)
{
    (void)store;
    return danos_tx_stage_object(tx, type, id, data, size, DANOS_TX_OP_UPDATE);
}

static danos_status_t dpa_delete(danos_tx_t *tx, danos_object_store_t *store,
                                 danos_obj_type_t type, danos_obj_id_t id)
{
    (void)store;
    return danos_tx_stage_object(tx, type, id, NULL, 0, DANOS_TX_OP_DELETE);
}

static danos_status_t dpa_read(danos_tx_t *tx, danos_object_store_t *store,
                               danos_obj_type_t type, danos_obj_id_t id,
                               void *out, size_t *size)
{
    if (!tx) return DANOS_ERR_TX_INVALID;
    bool handled = false;
    danos_status_t st = danos_tx_read_staged(tx, type, id, out, size,
                                             &handled);
    if (handled || st != DANOS_OK) return st;
    return danos_object_read(store, type, id, out, size);
}

/* All DPA CRUD below is transaction-scoped; the generic object registry
 * remains the immediate-mutation API for recovery and internal tests. */
#define danos_object_create(store, type, id, data, size) \
    dpa_create(tx, store, type, id, data, size)
#define danos_object_update(store, type, id, data, size) \
    dpa_update(tx, store, type, id, data, size)
#define danos_object_delete(store, type, id) dpa_delete(tx, store, type, id)
#define danos_object_read(store, type, id, out, size) \
    dpa_read(tx, store, type, id, out, size)

/* =========================================================================
 * Interface CRUD
 * ========================================================================= */
danos_status_t danos_iface_create(danos_tx_t *tx, const danos_iface_t *iface)
{
    (void)tx;
    if (!iface) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_IFACE,
                               iface->ifindex, iface, sizeof(*iface));
}

danos_status_t danos_iface_update(danos_tx_t *tx, const danos_iface_t *iface)
{
    (void)tx;
    if (!iface) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_IFACE,
                               iface->ifindex, iface, sizeof(*iface));
}

danos_status_t danos_iface_delete(danos_tx_t *tx, danos_ifindex_t ifindex)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_IFACE, ifindex);
}

danos_status_t danos_iface_read(danos_tx_t *tx, danos_ifindex_t ifindex, danos_iface_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_IFACE, ifindex, out, &sz);
}

/* =========================================================================
 * VRF CRUD
 * ========================================================================= */
danos_status_t danos_vrf_create(danos_tx_t *tx, const danos_vrf_t *vrf)
{
    (void)tx;
    if (!vrf) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_VRF,
                               vrf->vrf_id, vrf, sizeof(*vrf));
}

danos_status_t danos_vrf_update(danos_tx_t *tx, const danos_vrf_t *vrf)
{
    (void)tx;
    if (!vrf) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_VRF,
                               vrf->vrf_id, vrf, sizeof(*vrf));
}

danos_status_t danos_vrf_delete(danos_tx_t *tx, danos_vrf_id_t vrf_id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_VRF, vrf_id);
}

danos_status_t danos_vrf_read(danos_tx_t *tx, danos_vrf_id_t vrf_id, danos_vrf_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_VRF, vrf_id, out, &sz);
}

/* =========================================================================
 * NextHop CRUD
 * ========================================================================= */
danos_status_t danos_nh_create(danos_tx_t *tx, const danos_nexthop_t *nh)
{
    (void)tx;
    if (!nh) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_NEXTHOP,
                               nh->id, nh, sizeof(*nh));
}

danos_status_t danos_nh_update(danos_tx_t *tx, const danos_nexthop_t *nh)
{
    (void)tx;
    if (!nh) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_NEXTHOP,
                               nh->id, nh, sizeof(*nh));
}

danos_status_t danos_nh_delete(danos_tx_t *tx, danos_obj_id_t id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_NEXTHOP, id);
}

danos_status_t danos_nh_read(danos_tx_t *tx, danos_obj_id_t id, danos_nexthop_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_NEXTHOP, id, out, &sz);
}

/* =========================================================================
 * NHGroup CRUD
 * ========================================================================= */
danos_status_t danos_nhgroup_create(danos_tx_t *tx, const danos_nhgroup_t *grp)
{
    (void)tx;
    if (!grp) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_NHGROUP,
                               grp->id, grp, sizeof(*grp));
}

danos_status_t danos_nhgroup_update(danos_tx_t *tx, const danos_nhgroup_t *grp)
{
    (void)tx;
    if (!grp) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_NHGROUP,
                               grp->id, grp, sizeof(*grp));
}

danos_status_t danos_nhgroup_delete(danos_tx_t *tx, danos_obj_id_t id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_NHGROUP, id);
}

danos_status_t danos_nhgroup_read(danos_tx_t *tx, danos_obj_id_t id, danos_nhgroup_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_NHGROUP, id, out, &sz);
}

/* =========================================================================
 * Route CRUD
 * ========================================================================= */
/* Route key = (vrf_id, prefix, protocol) → hash to obj_id */
static danos_obj_id_t route_key(danos_vrf_id_t vrf, const danos_ip_prefix_t *p,
                                danos_route_proto_t proto)
{
    /* FNV-1a hash: good distribution for IP prefixes */
    uint64_t h = 14695981039346656037ULL;  /* FNV offset basis */
    h ^= (uint64_t)vrf;
    h *= 1099511628211ULL;  /* FNV prime */
    for (int i = 0; i < 16; i++) {
        h ^= p->addr.addr[i];
        h *= 1099511628211ULL;
    }
    h ^= p->prefix_len;
    h *= 1099511628211ULL;
    h ^= (uint64_t)proto;
    h *= 1099511628211ULL;
    return h;
}

danos_status_t danos_route_create(danos_tx_t *tx, const danos_route_t *route)
{
    (void)tx;
    if (!route) return DANOS_ERR_INVALID_ARG;
    danos_obj_id_t id = route_key(route->vrf_id, &route->prefix, route->protocol);
    return danos_object_create(get_default_store(), DANOS_OBJ_ROUTE,
                               id, route, sizeof(*route));
}

danos_status_t danos_route_update(danos_tx_t *tx, const danos_route_t *route)
{
    (void)tx;
    if (!route) return DANOS_ERR_INVALID_ARG;
    danos_obj_id_t id = route_key(route->vrf_id, &route->prefix, route->protocol);
    return danos_object_update(get_default_store(), DANOS_OBJ_ROUTE,
                               id, route, sizeof(*route));
}

danos_status_t danos_route_delete(danos_tx_t *tx, danos_vrf_id_t vrf_id,
                                  danos_ip_prefix_t prefix,
                                  danos_route_proto_t proto)
{
    (void)tx;
    danos_obj_id_t id = route_key(vrf_id, &prefix, proto);
    return danos_object_delete(get_default_store(), DANOS_OBJ_ROUTE, id);
}

danos_status_t danos_route_read(danos_tx_t *tx, danos_vrf_id_t vrf_id,
                                danos_ip_prefix_t prefix,
                                danos_route_proto_t proto,
                                danos_route_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    danos_obj_id_t id = route_key(vrf_id, &prefix, proto);
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_ROUTE, id, out, &sz);
}

typedef struct {
    danos_tx_t *tx;
    danos_vrf_id_t vrf_id;
    danos_route_cb_t cb;
    void *user;
} route_dump_ctx_t;

static void route_dump_iter(danos_object_entry_t *e, void *user)
{
    route_dump_ctx_t *c = user;
    if (e->type != DANOS_OBJ_ROUTE) return;
    if (e->data_size < sizeof(danos_route_t)) return;
    const danos_route_t *r = e->data;
    danos_route_t overlay;
    size_t overlay_size = sizeof(overlay);
    bool handled = false;
    danos_status_t st = danos_tx_read_staged(c->tx, DANOS_OBJ_ROUTE, e->id,
                                              &overlay, &overlay_size,
                                              &handled);
    if (handled) {
        if (st != DANOS_OK) return;
        r = &overlay;
    }
    if (c->vrf_id != 0 && r->vrf_id != c->vrf_id) return;  /* 0 = all VRFs */
    c->cb(r, c->user);
}

danos_status_t danos_route_dump(danos_tx_t *tx, danos_vrf_id_t vrf_id,
                                danos_route_cb_t cb, void *user)
{
    if (!tx || !cb) return DANOS_ERR_INVALID_ARG;
    route_dump_ctx_t c = { .tx = tx, .vrf_id = vrf_id, .cb = cb, .user = user };
    danos_object_iterate(get_default_store(), route_dump_iter, &c);
    danos_tx_record_t *rec = danos_tx_validate_ptr(tx);
    if (!rec) return DANOS_ERR_TX_INVALID;
    pthread_mutex_lock(&rec->lock);
    danos_route_t *additions = calloc(rec->staged_count, sizeof(*additions));
    if (rec->staged_count && !additions) {
        pthread_mutex_unlock(&rec->lock);
        return DANOS_ERR_NO_MEMORY;
    }
    size_t addition_count = 0;
    for (size_t i = 0; i < rec->staged_count; i++) {
        danos_object_mutation_t *m = &rec->staged[i];
        if (m->type != DANOS_OBJ_ROUTE || m->remove || m->base_exists ||
            m->data_size < sizeof(danos_route_t)) continue;
        const danos_route_t *r = m->data;
        if (vrf_id == 0 || r->vrf_id == vrf_id)
            additions[addition_count++] = *r;
    }
    pthread_mutex_unlock(&rec->lock);
    for (size_t i = 0; i < addition_count; i++) cb(&additions[i], user);
    free(additions);
    return DANOS_OK;
}

/* =========================================================================
 * ACL CRUD
 * ========================================================================= */
danos_status_t danos_acl_table_create(danos_tx_t *tx, const danos_acl_table_t *tbl)
{
    (void)tx;
    if (!tbl) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_ACL,
                               tbl->table_id, tbl, sizeof(*tbl));
}

danos_status_t danos_acl_table_update(danos_tx_t *tx, const danos_acl_table_t *tbl)
{
    (void)tx;
    if (!tbl) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_ACL,
                               tbl->table_id, tbl, sizeof(*tbl));
}

danos_status_t danos_acl_table_delete(danos_tx_t *tx, danos_obj_id_t table_id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_ACL, table_id);
}

danos_status_t danos_acl_table_read(danos_tx_t *tx, danos_obj_id_t table_id,
                                    danos_acl_table_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_ACL,
                             table_id, out, &sz);
}

danos_status_t danos_acl_rule_add(danos_tx_t *tx, danos_obj_id_t table_id,
                                  const danos_acl_rule_t *rule)
{
    (void)tx; (void)table_id;
    if (!rule) return DANOS_ERR_INVALID_ARG;
    /* Store rule with rule_id as key (offset to avoid clash with table) */
    return danos_object_create(get_default_store(), DANOS_OBJ_ACL,
                               rule->rule_id + 1000000, rule, sizeof(*rule));
}

danos_status_t danos_acl_rule_delete(danos_tx_t *tx, danos_obj_id_t table_id,
                                     danos_obj_id_t rule_id)
{
    (void)tx; (void)table_id;
    return danos_object_delete(get_default_store(), DANOS_OBJ_ACL, rule_id + 1000000);
}

danos_status_t danos_acl_rule_read(danos_tx_t *tx, danos_obj_id_t table_id,
                                   danos_obj_id_t rule_id, danos_acl_rule_t *out)
{
    (void)tx; (void)table_id;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_ACL,
                             rule_id + 1000000, out, &sz);
}

/* =========================================================================
 * QoS CRUD
 * ========================================================================= */
danos_status_t danos_qos_policy_create(danos_tx_t *tx, const danos_qos_policy_t *p)
{
    (void)tx;
    if (!p) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_QOS,
                               p->policy_id, p, sizeof(*p));
}

danos_status_t danos_qos_policy_update(danos_tx_t *tx, const danos_qos_policy_t *p)
{
    (void)tx;
    if (!p) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_QOS,
                               p->policy_id, p, sizeof(*p));
}

danos_status_t danos_qos_policy_delete(danos_tx_t *tx, danos_obj_id_t policy_id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_QOS, policy_id);
}

danos_status_t danos_qos_policy_read(danos_tx_t *tx, danos_obj_id_t policy_id,
                                     danos_qos_policy_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_QOS,
                             policy_id, out, &sz);
}

/* Binding stored as its own object type so it persists and can be
 * listed; key derived from (policy, ifindex, direction). */
typedef struct {
    danos_obj_id_t  policy_id;
    danos_ifindex_t ifindex;
    bool            ingress;
} danos_qos_bind_rec_t;

static danos_obj_id_t qos_bind_key(danos_obj_id_t policy_id,
                                   danos_ifindex_t ifindex, bool ingress)
{
    uint64_t h = 0x51505342ULL;  /* "QPSB" */
    h = (h ^ policy_id) * 1099511628211ULL;
    h = (h ^ ifindex) * 1099511628211ULL;
    h = (h ^ (uint64_t)ingress) * 1099511628211ULL;
    h |= 1ULL << 63;  /* keep out of the plain policy-id space */
    return h;
}

danos_status_t danos_qos_policy_bind(danos_tx_t *tx, danos_obj_id_t policy_id,
                                     danos_ifindex_t ifindex, bool ingress)
{
    (void)tx;
    danos_qos_bind_rec_t rec = {
        .policy_id = policy_id, .ifindex = ifindex, .ingress = ingress,
    };
    return danos_object_create(get_default_store(), DANOS_OBJ_QOS_BIND,
                               qos_bind_key(policy_id, ifindex, ingress),
                               &rec, sizeof(rec));
}

/* =========================================================================
 * BFD CRUD
 * ========================================================================= */
danos_status_t danos_bfd_create(danos_tx_t *tx, const danos_bfd_t *bfd)
{
    (void)tx;
    if (!bfd) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_BFD,
                               bfd->id, bfd, sizeof(*bfd));
}

danos_status_t danos_bfd_delete(danos_tx_t *tx, danos_obj_id_t id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_BFD, id);
}

/* =========================================================================
 * MPLS LSP CRUD (v0.2)
 * ========================================================================= */
danos_status_t danos_mpls_lsp_create(danos_tx_t *tx, const danos_mpls_lsp_t *lsp)
{
    (void)tx;
    if (!lsp) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_MPLS_LSP,
                               lsp->in_label, lsp, sizeof(*lsp));
}

danos_status_t danos_mpls_lsp_update(danos_tx_t *tx, const danos_mpls_lsp_t *lsp)
{
    (void)tx;
    if (!lsp) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_MPLS_LSP,
                               lsp->in_label, lsp, sizeof(*lsp));
}

danos_status_t danos_mpls_lsp_delete(danos_tx_t *tx, danos_mpls_label_t in_label)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_MPLS_LSP, in_label);
}

danos_status_t danos_mpls_lsp_read(danos_tx_t *tx, danos_mpls_label_t in_label,
                                   danos_mpls_lsp_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_MPLS_LSP,
                             in_label, out, &sz);
}

/* =========================================================================
 * Tunnel CRUD (v0.2)
 * ========================================================================= */
danos_status_t danos_tunnel_create(danos_tx_t *tx, const danos_tunnel_t *tun)
{
    (void)tx;
    if (!tun) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_TUNNEL,
                               tun->id, tun, sizeof(*tun));
}

danos_status_t danos_tunnel_update(danos_tx_t *tx, const danos_tunnel_t *tun)
{
    (void)tx;
    if (!tun) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_TUNNEL,
                               tun->id, tun, sizeof(*tun));
}

danos_status_t danos_tunnel_delete(danos_tx_t *tx, danos_obj_id_t id)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_TUNNEL, id);
}

danos_status_t danos_tunnel_read(danos_tx_t *tx, danos_obj_id_t id,
                                 danos_tunnel_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_TUNNEL,
                             id, out, &sz);
}

/* =========================================================================
 * EVPN EVI CRUD (v0.2)
 * ========================================================================= */
danos_status_t danos_evpn_evi_create(danos_tx_t *tx, const danos_evpn_evi_t *evi)
{
    (void)tx;
    if (!evi) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_EVPN,
                               evi->evi, evi, sizeof(*evi));
}

danos_status_t danos_evpn_evi_update(danos_tx_t *tx, const danos_evpn_evi_t *evi)
{
    (void)tx;
    if (!evi) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_EVPN,
                               evi->evi, evi, sizeof(*evi));
}

danos_status_t danos_evpn_evi_delete(danos_tx_t *tx, uint32_t evi)
{
    (void)tx;
    return danos_object_delete(get_default_store(), DANOS_OBJ_EVPN, evi);
}

danos_status_t danos_evpn_evi_read(danos_tx_t *tx, uint32_t evi,
                                   danos_evpn_evi_t *out)
{
    (void)tx;
    if (!out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_EVPN,
                             evi, out, &sz);
}

/* =========================================================================
 * Multicast mroute CRUD (v0.2)
 *
 * Composite key: (vrf_id << 32) | group_hash, where group_hash is the
 * first 4 bytes of the group address interpreted as uint32. This is a
 * v0.2 simplification; production would use a proper hash.
 * ========================================================================= */
static danos_obj_id_t mroute_key(danos_vrf_id_t vrf_id,
                                 const danos_ip_addr_t *group)
{
    uint32_t g = 0;
    if (group) {
        g = ((uint32_t)group->addr[0] << 24) |
            ((uint32_t)group->addr[1] << 16) |
            ((uint32_t)group->addr[2] << 8)  |
            ((uint32_t)group->addr[3]);
    }
    return ((danos_obj_id_t)vrf_id << 32) | g;
}

danos_status_t danos_mroute_create(danos_tx_t *tx, const danos_mroute_t *mr)
{
    (void)tx;
    if (!mr) return DANOS_ERR_INVALID_ARG;
    return danos_object_create(get_default_store(), DANOS_OBJ_MULTICAST,
                               mroute_key(mr->vrf_id, &mr->group),
                               mr, sizeof(*mr));
}

danos_status_t danos_mroute_update(danos_tx_t *tx, const danos_mroute_t *mr)
{
    (void)tx;
    if (!mr) return DANOS_ERR_INVALID_ARG;
    return danos_object_update(get_default_store(), DANOS_OBJ_MULTICAST,
                               mroute_key(mr->vrf_id, &mr->group),
                               mr, sizeof(*mr));
}

danos_status_t danos_mroute_delete(danos_tx_t *tx, danos_vrf_id_t vrf_id,
                                   const danos_ip_addr_t *group)
{
    (void)tx;
    if (!group) return DANOS_ERR_INVALID_ARG;
    return danos_object_delete(get_default_store(), DANOS_OBJ_MULTICAST,
                               mroute_key(vrf_id, group));
}

danos_status_t danos_mroute_read(danos_tx_t *tx, danos_vrf_id_t vrf_id,
                                 const danos_ip_addr_t *group,
                                 danos_mroute_t *out)
{
    (void)tx;
    if (!group || !out) return DANOS_ERR_INVALID_ARG;
    size_t sz = sizeof(*out);
    return danos_object_read(get_default_store(), DANOS_OBJ_MULTICAST,
                             mroute_key(vrf_id, group), out, &sz);
}
