/* Manual isolated netns fixture; drift0/drift1 are disposable dummy links. */
#include <danos/dpa.h>
#include <danos/core/backend_ops.h>
#include <danos/core/object_registry.h>
#include "../../danos-netlink/danos_netlink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <net/if.h>
#include <unistd.h>

int main(void)
{
    if (!getenv("DANOS_READBACK_TEST_ISOLATED")) return 2;
    uint32_t indices[2] = {if_nametoindex("drift0"), if_nametoindex("drift1")};
    assert(indices[0] && indices[1]);
    assert(danos_netlink_init(true) == 0);
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    g_default_store = danos_object_store_create(256);
    assert(g_default_store);
    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "netlink-readback", NULL) == DANOS_OK);
    for (unsigned i = 0; i < 2; i++) {
        danos_iface_t iface = {.ifindex = indices[i], .admin_up = true};
        assert(danos_iface_create(&tx, &iface) == DANOS_OK);
        danos_nexthop_t nh = {.id = 201+i, .ifindex = indices[i]};
        nh.gateway.af = DANOS_AF_IPV4;
        nh.gateway.addr[0] = 10; nh.gateway.addr[1] = 231;
        nh.gateway.addr[2] = (uint8_t)(1+i); nh.gateway.addr[3] = 2;
        assert(danos_nh_create(&tx, &nh) == DANOS_OK);
    }
    danos_nhgroup_t group = {.id = 301, .nh_count = 2, .nh_ids = {201,202}};
    assert(danos_nhgroup_create(&tx, &group) == DANOS_OK);
    danos_route_t route = {.vrf_id = 257, .nhgroup_id = 301, .protocol = DANOS_ROUTE_PROTO_STATIC};
    route.prefix.addr.af = DANOS_AF_IPV4; route.prefix.prefix_len = 24;
    route.prefix.addr.addr[0] = 198; route.prefix.addr.addr[1] = 51; route.prefix.addr.addr[2] = 100;
    assert(danos_route_create(&tx, &route) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
    uint64_t attempts, failed;
    assert(danos_programming_run(&attempts, &failed) == 3 && failed == 0);
    assert(danos_oper_count(DANOS_OBJ_ROUTE) == 1);
    assert(danos_programming_run(&attempts, &failed) == 0 && failed == 0);
    danos_resolved_route_t changed = {.route = route, .has_gw = true, .nh_count = 2};
    for (unsigned i = 0; i < 2; i++) {
        changed.nh_gw[i][0] = 10; changed.nh_gw[i][1] = 231;
        changed.nh_gw[i][2] = (uint8_t)(1+i); changed.nh_gw[i][3] = 2;
        changed.nh_oif[i] = indices[i];
    }
    changed.gw[0] = 10; changed.gw[1] = 231; changed.gw[2] = 1; changed.gw[3] = 2;
    changed.oif = indices[0];
    assert(ops->route_readback(&changed, &(danos_route_t){0}, NULL) == DANOS_OK);
    puts("[PASS] actual Linux table 257 and complete ECMP readback");
    changed.nh_count = 1;
    assert(ops->route_add(&changed, NULL) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failed) == 1 && failed == 0);
    puts("[PASS] actual Linux missing ECMP member repaired");
    changed.nh_count = 2; changed.nh_gw[0][3] = 99;
    assert(ops->route_add(&changed, NULL) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failed) == 1 && failed == 0);
    puts("[PASS] actual Linux wrong gateway repaired");
    assert(ops->route_del(&changed, NULL) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failed) == 1 && failed == 0);
    puts("[PASS] actual Linux external route deletion repaired");
    assert(ops->iface_up(indices[0], false, NULL) == DANOS_OK);
    /* Store iteration order is not dependency order: an interface may be
     * restored after the route was attempted. Verify bounded convergence. */
    danos_programming_set_policy(5, 1, 4, 5000, 0);
    bool converged = false;
    for (unsigned i = 0; i < 20; i++) {
        (void)danos_programming_run(&attempts, &failed);
        if (failed == 0 && danos_oper_count(DANOS_OBJ_ROUTE) == 1) { converged = true; break; }
        usleep(10000);
    }
    assert(converged);
    danos_iface_t oper;
    size_t size = sizeof(oper);
    assert(danos_oper_read(DANOS_OBJ_IFACE, indices[0], &oper, &size) == DANOS_OK && oper.admin_up);
    puts("[PASS] actual Linux admin drift repaired");
    danos_netlink_shutdown();
    return 0;
}
