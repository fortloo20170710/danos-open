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
    for (uint32_t i = 0; i < nh_count && i < 64; i++) {
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

    /* IPv4 address programming, duplicate add and readback. */
    assert(ops->iface_addr_set(&desired, ops->user) == DANOS_OK);
    assert(ops->iface_addr_set(&desired, ops->user) == DANOS_OK);
    assert(ops->readback(DANOS_OBJ_IFACE, 5, &desired, sizeof(desired),
                         &oper, ops->user) == DANOS_OK);
    assert(oper.ipv4_address.addr.af == DANOS_AF_IPV4);
    assert(oper.ipv4_address.prefix_len == 24);
    assert(memcmp(oper.ipv4_address.addr.addr, desired.ipv4_address.addr.addr,
                  4) == 0);

    /* IPv6 add/readback/duplicate/remove, including idempotent delete. */
    assert(ops->iface_addr_del(&desired, ops->user) == DANOS_OK);
    danos_iface_t v6 = desired;
    v6.ipv4_address.addr.af = DANOS_AF_UNSPEC;
    v6.ipv6_address.addr.af = DANOS_AF_IPV6;
    v6.ipv6_address.prefix_len = 64;
    v6.ipv6_address.addr.addr[0] = 0x20;
    v6.ipv6_address.addr.addr[1] = 0x01;
    v6.ipv6_address.addr.addr[2] = 0x0d;
    v6.ipv6_address.addr.addr[3] = 0xb8;
    v6.ipv6_address.addr.addr[15] = 1;
    assert(ops->iface_addr_set(&v6, ops->user) == DANOS_OK);
    assert(ops->iface_addr_set(&v6, ops->user) == DANOS_OK);  /* duplicate */
    assert(ops->readback(DANOS_OBJ_IFACE, 5, &v6, sizeof(v6),
                         &oper, ops->user) == DANOS_OK);
    assert(oper.ipv6_address.addr.af == DANOS_AF_IPV6);
    assert(oper.ipv6_address.prefix_len == 64);
    assert(memcmp(oper.ipv6_address.addr.addr, v6.ipv6_address.addr.addr,
                  16) == 0);
    assert(oper.ipv4_address.addr.af == DANOS_AF_UNSPEC);
    assert(ops->iface_addr_del(&v6, ops->user) == DANOS_OK);
    assert(ops->readback(DANOS_OBJ_IFACE, 5, &v6, sizeof(v6),
                         &oper, ops->user) == DANOS_OK);
    assert(oper.ipv6_address.addr.af == DANOS_AF_UNSPEC);
    assert(ops->iface_addr_del(&v6, ops->user) == DANOS_OK);  /* idempotent */

    /* Invalid prefix lengths and families are refused. */
    danos_iface_t bad = v6;
    bad.ipv6_address.prefix_len = 129;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    bad = desired;
    bad.ipv4_address.prefix_len = 33;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    bad = desired;
    bad.ifindex = 0;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    bad = desired;
    bad.ipv4_address.addr.af = DANOS_AF_IPV6;
    assert(ops->iface_addr_set(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->iface_addr_del(&bad, ops->user) == DANOS_ERR_INVALID_ARG);
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

    /* Single gateway supplied through the resolved fields (nh_count=0). */
    danos_resolved_route_t single = make_route(9, 0);
    single.has_gw = true;
    single.gw[0] = 10; single.gw[1] = 9; single.gw[2] = 9; single.gw[3] = 1;
    single.oif = 5;
    assert(ops->route_add(&single, ops->user) == DANOS_OK);
    assert(ops->route_readback(&single, &oper_route, ops->user) == DANOS_OK);
    single.gw[3] = 2;
    assert(ops->route_readback(&single, &oper_route, ops->user)
           == DANOS_ERR_VERIFY_FAIL);
    (void)ops->route_del(&single, ops->user);

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

    /* Route readback argument/type/length branches. */
    assert(ops->readback(DANOS_OBJ_ROUTE, 1, NULL, sizeof(rr.route),
                         &oper_route, ops->user) == DANOS_ERR_INVALID_ARG);
    assert(ops->readback(DANOS_OBJ_ROUTE, 1, &rr.route, 1,
                         &oper_route, ops->user) == DANOS_ERR_INVALID_ARG);
    danos_route_t v6_route = rr.route;
    v6_route.prefix.addr.af = DANOS_AF_IPV6;
    assert(ops->readback(DANOS_OBJ_ROUTE, 1, &v6_route, sizeof(v6_route),
                         &oper_route, ops->user) == DANOS_ERR_NOT_SUPPORTED);
    danos_route_t default_route = rr.route;
    default_route.prefix.prefix_len = 0;
    assert(ops->readback(DANOS_OBJ_ROUTE, 1, &default_route,
                         sizeof(default_route), &oper_route, ops->user)
           == DANOS_ERR_NOT_FOUND);
    danos_resolved_route_t long_prefix = rr;
    long_prefix.route.prefix.prefix_len = 33;
    assert(ops->route_readback(&long_prefix, &oper_route, ops->user)
           == DANOS_ERR_NOT_SUPPORTED);
    long_prefix.route.prefix.addr.af = DANOS_AF_IPV6;
    assert(ops->route_readback(&long_prefix, &oper_route, ops->user)
           == DANOS_ERR_NOT_SUPPORTED);

    /* A desired blackhole that is actually programmed as unicast (and vice
     * versa) is a verification failure, not absence. */
    danos_resolved_route_t bh_missing = make_route(5, 0);
    bh_missing.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(ops->route_readback(&bh_missing, &oper_route, ops->user)
           == DANOS_ERR_NOT_FOUND);
    danos_resolved_route_t unicast = make_route(6, 0);
    assert(ops->route_add(&unicast, ops->user) == DANOS_OK);
    danos_resolved_route_t want_bh = unicast;
    want_bh.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(ops->route_readback(&want_bh, &oper_route, ops->user)
           == DANOS_ERR_VERIFY_FAIL);
    danos_resolved_route_t want_unicast = unicast;
    want_unicast.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    (void)ops->route_del(&want_unicast, ops->user);
    danos_resolved_route_t bh2 = make_route(7, 0);
    bh2.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(ops->route_add(&bh2, ops->user) == DANOS_OK);
    danos_resolved_route_t as_unicast = bh2;
    as_unicast.route.flags = 0;
    assert(ops->route_readback(&as_unicast, &oper_route, ops->user)
           == DANOS_ERR_VERIFY_FAIL);
    (void)ops->route_del(&bh2, ops->user);

    /* Deleting a prefix that was never installed is NOT_FOUND. */
    danos_resolved_route_t absent = make_route(8, 1);
    assert(ops->route_del(&absent, ops->user) == DANOS_ERR_NOT_FOUND);

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
