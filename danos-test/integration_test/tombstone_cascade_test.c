/*
 * Test: NHGroup / next-hop removal cascades to dependent routes.
 *
 * A route whose desired object still exists but whose NHGroup or member
 * next-hop disappeared must be withdrawn from the backend.  The tombstone
 * sweep must do this even when the programming pass deferred the route, and
 * must be idempotent: a second sweep issues no duplicate withdraw.  Route
 * withdrawals are ordered before other tombstones (for example interface
 * down), and restoring the dependency replays the route.
 */
#include <danos/core/backend_ops.h>
#include <danos/core/object_registry.h>
#include <danos/core/reconciler.h>
#include <danos/dpa.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_adds, g_dels;
static char g_order[128];
static size_t g_order_len;

static void note(char c)
{
    if (g_order_len + 1 < sizeof(g_order))
        g_order[g_order_len++] = c;
}

static danos_status_t fake_iface_up(danos_ifindex_t i, bool up, void *user)
{
    (void)i; (void)up; (void)user;
    note('I');
    return DANOS_OK;
}

static danos_status_t fake_route_add(const danos_resolved_route_t *rr,
                                     void *user)
{
    (void)rr; (void)user;
    g_adds++;
    note('A');
    return DANOS_OK;
}

static danos_status_t fake_route_del(const danos_resolved_route_t *rr,
                                     void *user)
{
    (void)rr; (void)user;
    g_dels++;
    note('D');
    return DANOS_OK;
}

static void seed_iface(void)
{
    danos_tx_t tx;
    danos_iface_t iface;
    memset(&iface, 0, sizeof(iface));
    iface.ifindex = 9;
    snprintf(iface.name, sizeof(iface.name), "tc0");
    iface.mtu = 1500;
    iface.admin_up = true;
    assert(danos_tx_begin(&tx, "seed-if", NULL) == DANOS_OK);
    assert(danos_iface_create(&tx, &iface) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
}

static void seed_nh(void)
{
    danos_tx_t tx;
    danos_nexthop_t nh;
    memset(&nh, 0, sizeof(nh));
    nh.id = 21;
    nh.gateway.af = DANOS_AF_IPV4;
    nh.gateway.addr[0] = 10;
    nh.gateway.addr[3] = 1;
    nh.ifindex = 9;
    assert(danos_tx_begin(&tx, "seed-nh", NULL) == DANOS_OK);
    assert(danos_nh_create(&tx, &nh) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
}

static danos_nhgroup_t group31(void)
{
    danos_nhgroup_t grp;
    memset(&grp, 0, sizeof(grp));
    grp.id = 31;
    grp.nh_count = 1;
    grp.nh_ids[0] = 21;
    return grp;
}

static void seed_group(void)
{
    danos_tx_t tx;
    danos_nhgroup_t grp = group31();
    assert(danos_tx_begin(&tx, "seed-grp", NULL) == DANOS_OK);
    assert(danos_nhgroup_create(&tx, &grp) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
}

static danos_route_t route(void)
{
    danos_route_t r;
    memset(&r, 0, sizeof(r));
    r.vrf_id = 0;
    r.prefix.addr.af = DANOS_AF_IPV4;
    r.prefix.addr.addr[0] = 10;
    r.prefix.addr.addr[1] = 77;
    r.prefix.prefix_len = 24;
    r.protocol = DANOS_ROUTE_PROTO_STATIC;
    r.metric = 0;
    r.nhgroup_id = 31;
    return r;
}

static void delete_group(void)
{
    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "del-grp", NULL) == DANOS_OK);
    assert(danos_nhgroup_delete(&tx, 31) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
}

static void delete_nh(void)
{
    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "del-nh", NULL) == DANOS_OK);
    assert(danos_nh_delete(&tx, 21) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
}

static void seed_route(void)
{
    danos_tx_t tx;
    danos_route_t r = route();
    assert(danos_tx_begin(&tx, "seed-route", NULL) == DANOS_OK);
    assert(danos_route_create(&tx, &r) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
}

int main(void)
{
    if (!g_default_store) g_default_store = danos_object_store_create(256);
    assert(g_default_store);

    seed_iface();
    seed_nh();
    seed_group();
    seed_route();

    danos_backend_ops_t ops = {
        .name = "fake-cascade",
        .iface_up = fake_iface_up,
        .route_add = fake_route_add,
        .route_del = fake_route_del,
    };
    danos_backend_ops_set(&ops);

    uint64_t attempted = 0, failed = 0;
    assert(danos_programming_run(&attempted, &failed) == 2);
    assert(g_adds == 1);
    assert(danos_programming_programmed_count(DANOS_OBJ_ROUTE) == 1);
    assert(danos_reconciler_init(NULL, NULL) == 0);

    /* NHGroup removed while the route remains desired: the reconciler pass
     * withdraws it, and a second pass is idempotent. */
    delete_group();
    (void)danos_reconciler_run_once();
    assert(g_dels == 1);
    assert(danos_programming_programmed_count(DANOS_OBJ_ROUTE) == 0);
    bool desired = false, recorded = false;
    assert(danos_programming_object_presence(DANOS_OBJ_ROUTE, 1,
                                             &desired, &recorded) == DANOS_OK);
    /* Desired route remains for replay once the dependency returns. */
    assert(danos_object_count(g_default_store, DANOS_OBJ_ROUTE) == 1);
    (void)danos_reconciler_run_once();
    assert(g_dels == 1);

    /* Restoring the group replays the route. */
    seed_group();
    (void)danos_reconciler_run_once();
    assert(g_adds == 2);
    assert(danos_programming_programmed_count(DANOS_OBJ_ROUTE) == 1);

    /* Removing the member NH cascades through the surviving group. */
    delete_nh();
    assert(danos_programming_sweep(&failed) == 1);
    assert(g_dels == 2);
    assert(danos_programming_programmed_count(DANOS_OBJ_ROUTE) == 0);

    seed_nh();
    assert(danos_programming_run(&attempted, &failed) == 1);
    assert(g_adds == 3);

    /* Removal ordering: route withdraw precedes interface-down/other
     * tombstones in the same sweep. */
    assert(danos_object_delete(g_default_store, DANOS_OBJ_IFACE, 9)
           == DANOS_OK);
    delete_group();
    g_order_len = 0;
    assert(danos_programming_sweep(&failed) == 2);
    assert(g_dels == 3);
    assert(g_order_len >= 2);
    assert(g_order[0] == 'D');
    assert(g_order[1] == 'I');
    assert(danos_programming_sweep(&failed) == 0);

    danos_reconciler_fini();
    danos_backend_ops_set(NULL);
    danos_tx_t tx;
    danos_route_t r = route();
    assert(danos_tx_begin(&tx, "cleanup", NULL) == DANOS_OK);
    (void)danos_route_delete(&tx, r.vrf_id, r.prefix, r.protocol);
    (void)danos_nh_delete(&tx, 21);
    (void)danos_nhgroup_delete(&tx, 31);
    (void)danos_iface_delete(&tx, 9);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);

    printf("[PASS] tombstone cascade: NHGroup/NH removal withdraws dependent "
           "routes, routes-first ordering, idempotent replay\n");
    return 0;
}
