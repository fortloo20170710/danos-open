/*
 * Test: netlink OPER readback and repair paths against the mock kernel.
 *
 * Covers interface link-state readback, route presence, ECMP/interface-only
 * and blackhole route verification, metric mismatch, invalid arguments and
 * withdrawal.  Real-kernel FIB_MATCH paths are covered by the live test.
 */
#include <danos/core/backend_ops.h>
#include "../../danos-netlink/danos_netlink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static danos_resolved_route_t make_route(uint8_t third, uint8_t nh_count)
{
    danos_resolved_route_t rr;
    memset(&rr, 0, sizeof(rr));
    rr.route.vrf_id = 0;
    rr.route.prefix.addr.af = DANOS_AF_IPV4;
    rr.route.prefix.addr.addr[0] = 10;
    rr.route.prefix.addr.addr[1] = 9;
    rr.route.prefix.addr.addr[2] = third;
    rr.route.prefix.prefix_len = 24;
    rr.route.protocol = DANOS_ROUTE_PROTO_STATIC;
    rr.nh_count = nh_count;
    for (uint32_t i = 0; i < nh_count; i++) {
        rr.nh_gw[i][0] = 10;
        rr.nh_gw[i][3] = (uint8_t)(1 + i);
        rr.nh_oif[i] = 5 + i;
    }
    return rr;
}

int main(void)
{
    assert(danos_netlink_init(false) == 0);
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->readback && ops->route_readback);

    /* Interface OPER state, including carrier changes and unknown ids. */
    danos_iface_t desired;
    memset(&desired, 0, sizeof(desired));
    desired.ifindex = 5;
    desired.mtu = 1500;
    desired.admin_up = true;
    snprintf(desired.name, sizeof(desired.name), "wan0");
    desired.ipv4_address.addr.af = DANOS_AF_IPV4;
    desired.ipv4_address.addr.addr[0] = 192;
    desired.ipv4_address.addr.addr[1] = 0;
    desired.ipv4_address.addr.addr[2] = 2;
    desired.ipv4_address.addr.addr[3] = 1;
    desired.ipv4_address.prefix_len = 24;
    assert(ops->iface_up(5, true, ops->user) == DANOS_OK);
    assert(ops->iface_up(5, true, ops->user) == DANOS_OK);   /* idempotent */
    assert(ops->iface_up(0, true, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->iface_up(9999, true, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(!danos_netlink_is_real());
    assert(!danos_netlink_mock_iface_up(0));
    danos_netlink_mock_set_link(0, true);     /* ignored, must not crash */
    danos_netlink_mock_set_link(9999, true);
    danos_netlink_mock_set_link(5, true);
    danos_iface_t oper;
    assert(ops->readback(DANOS_OBJ_IFACE, 5, &desired, sizeof(desired),
                         &oper, ops->user) == DANOS_OK);
    assert(oper.link_up && oper.mtu == 1500);
    danos_netlink_mock_set_link(5, false);
    assert(ops->readback(DANOS_OBJ_IFACE, 5, &desired, sizeof(desired),
                         &oper, ops->user) == DANOS_OK);
    assert(!oper.link_up);
    assert(ops->readback(DANOS_OBJ_IFACE, 99, &desired, sizeof(desired),
                         &oper, ops->user) == DANOS_ERR_NOT_FOUND);
    assert(ops->readback(DANOS_OBJ_IFACE, 0, &desired, sizeof(desired),
                         &oper, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->readback(DANOS_OBJ_IFACE, 5, NULL, sizeof(desired),
                         &oper, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->readback(DANOS_OBJ_IFACE, 5, &desired, 1, &oper,
                         ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->readback(DANOS_OBJ_ACL, 5, &desired, sizeof(desired),
                         &oper, ops->user) == DANOS_ERR_NOT_SUPPORTED);

    /* Address programming paths and their argument/unsupported branches. */
    assert(ops->iface_addr_set(&desired, ops->user) == DANOS_OK);
    assert(ops->iface_addr_del(&desired, ops->user) == DANOS_OK);
    danos_iface_t bad = desired;
    bad.ifindex = 0;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    bad = desired;
    bad.ipv4_address.addr.af = DANOS_AF_IPV6;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_NOT_SUPPORTED);
    assert(ops->iface_addr_del(&bad, ops->user) == DANOS_OK);
    assert(ops->iface_addr_set(NULL, ops->user) == DANOS_ERR_INVALID_ARG);

    danos_vrf_t vrf;
    memset(&vrf, 0, sizeof(vrf));
    vrf.vrf_id = 777;
    snprintf(vrf.name, sizeof(vrf.name), "blue");
    assert(ops->vrf_add(&vrf, ops->user) == DANOS_OK);
    assert(ops->vrf_del(777, ops->user) == DANOS_OK);

    /* ECMP route: presence, metric and member mismatches. */
    danos_resolved_route_t rr = make_route(0, 2);
    danos_route_t oper_route;
    assert(ops->route_add(&rr, ops->user) == DANOS_OK);
    assert(ops->route_add(&rr, ops->user) == DANOS_OK);   /* idempotent */
    assert(ops->route_readback(&rr, &oper_route, ops->user) == DANOS_OK);
    assert(ops->route_readback(NULL, &oper_route, ops->user)
           == DANOS_ERR_INVALID_ARG);
    assert(ops->route_readback(&rr, NULL, ops->user)
           == DANOS_ERR_INVALID_ARG);
    assert(ops->readback(DANOS_OBJ_ROUTE, 1, &rr.route, sizeof(rr.route),
                         &oper_route, ops->user) == DANOS_OK);

    rr.route.metric = 5;
    assert(ops->route_readback(&rr, &oper_route, ops->user)
           == DANOS_ERR_VERIFY_FAIL);
    rr.route.metric = 0;
    rr.nh_gw[1][3] = 99;
    assert(ops->route_readback(&rr, &oper_route, ops->user)
           == DANOS_ERR_VERIFY_FAIL);
    rr = make_route(0, 2);

    /* Interface-only next hop (no gateway) is a valid path. */
    danos_resolved_route_t ifonly = make_route(1, 1);
    memset(ifonly.nh_gw[0], 0, sizeof(ifonly.nh_gw[0]));
    assert(ops->route_add(&ifonly, ops->user) == DANOS_OK);
    assert(ops->route_readback(&ifonly, &oper_route, ops->user) == DANOS_OK);

    /* Blackhole routes carry no next hop and still verify. */
    danos_resolved_route_t bh = make_route(2, 0);
    bh.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(ops->route_add(&bh, ops->user) == DANOS_OK);
    assert(ops->route_readback(&bh, &oper_route, ops->user) == DANOS_OK);

    /* Invalid route arguments and over-large next-hop sets are refused. */
    danos_resolved_route_t bad_route = make_route(3, 1);
    bad_route.route.prefix.addr.af = DANOS_AF_IPV6;
    assert(ops->route_add(&bad_route, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->route_readback(&bad_route, &oper_route, ops->user)
           == DANOS_ERR_NOT_SUPPORTED);
    bad_route = make_route(3, 65);
    assert(ops->route_add(&bad_route, ops->user) == DANOS_ERR_INVALID_ARG);

    /* Withdrawal removes presence and the specific readback. */
    assert(ops->route_del(&rr, ops->user) == DANOS_OK);
    assert(ops->route_readback(&rr, &oper_route, ops->user)
           == DANOS_ERR_NOT_FOUND);
    assert(ops->readback(DANOS_OBJ_ROUTE, 2, &rr.route, sizeof(rr.route),
                         &oper_route, ops->user) == DANOS_ERR_NOT_FOUND);
    assert(ops->route_del(&rr, ops->user) == DANOS_ERR_NOT_FOUND);

    danos_netlink_shutdown();
    printf("[PASS] netlink readback: interface, VRF, ECMP, interface-only, "
           "blackhole, metric/member mismatch, withdrawal\n");
    return 0;
}
