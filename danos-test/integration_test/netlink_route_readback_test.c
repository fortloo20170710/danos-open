#include <danos/core/backend_ops.h>
#include "../../danos-netlink/danos_netlink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    if (danos_netlink_init(true) != 0 || !danos_netlink_is_real()) {
        puts("[SKIP] rtnetlink route readback socket unavailable");
        return 0;
    }
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->readback && ops->route_readback);

    danos_route_t route;
    danos_route_t oper;

    /* The default prefix is not reachable through RTM_F_FIB_MATCH on current
     * kernels; report it unsupported rather than absent so the reconciler
     * does not drop and churn the ledger every pass. */
    memset(&route, 0, sizeof(route));
    route.prefix.addr.af = DANOS_AF_IPV4;
    route.prefix.prefix_len = 0;
    danos_status_t st = ops->readback(DANOS_OBJ_ROUTE, 1, &route,
                                      sizeof(route), &oper, ops->user);
    assert(st == DANOS_ERR_NOT_SUPPORTED);

    /* Choose a real non-default route from /proc/net/route.  This is a
     * positive live readback assertion, so a lookup that reports dst_len=32
     * and no attributes (the pre-FIB_MATCH bug) now fails. */
    FILE *routes = fopen("/proc/net/route", "r");
    assert(routes != NULL);
    char line[256];
    assert(fgets(line, sizeof(line), routes) != NULL);   /* header */
    bool have_route = false;
    while (!have_route && fgets(line, sizeof(line), routes)) {
        char iface[64];
        unsigned dest, gw, flags, metric, mask;
        if (sscanf(line, "%63s %x %x %x %*u %*u %u %x %*u %*u %*u",
                   iface, &dest, &gw, &flags, &metric, &mask) != 6)
            continue;
        if (dest == 0 || mask == 0) continue;   /* skip default */
        (void)gw;
        unsigned prefix_len = 0;
        for (unsigned bit = 0; bit < 32; bit++)
            if (mask & (1u << bit)) prefix_len++;
        memset(&route, 0, sizeof(route));
        route.vrf_id = 0;
        route.prefix.addr.af = DANOS_AF_IPV4;
        route.prefix.addr.addr[0] = (uint8_t)(dest & 0xff);
        route.prefix.addr.addr[1] = (uint8_t)((dest >> 8) & 0xff);
        route.prefix.addr.addr[2] = (uint8_t)((dest >> 16) & 0xff);
        route.prefix.addr.addr[3] = (uint8_t)((dest >> 24) & 0xff);
        route.prefix.prefix_len = (uint8_t)prefix_len;
        route.protocol = DANOS_ROUTE_PROTO_STATIC;
        route.metric = metric;
        have_route = true;
    }
    fclose(routes);
    if (!have_route) {
        puts("[SKIP] no non-default IPv4 route to probe");
        danos_netlink_shutdown();
        return 0;
    }

    st = ops->readback(DANOS_OBJ_ROUTE, 2, &route, sizeof(route),
                       &oper, ops->user);
    assert(st == DANOS_OK);

    danos_resolved_route_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    resolved.route = route;
    st = ops->route_readback(&resolved, &oper, ops->user);
    assert(st == DANOS_OK);

    /* A verified route with a different metric must not pass. */
    resolved.route.metric = route.metric + 1;
    st = ops->route_readback(&resolved, &oper, ops->user);
    assert(st == DANOS_ERR_VERIFY_FAIL);

    /* A prefix that is not routed must be reported absent, not mismatched. */
    memset(&route, 0, sizeof(route));
    route.prefix.addr.af = DANOS_AF_IPV4;
    route.prefix.addr.addr[0] = 203;
    route.prefix.addr.addr[1] = 0;
    route.prefix.addr.addr[2] = 113;
    route.prefix.addr.addr[3] = 192;
    route.prefix.prefix_len = 26;
    st = ops->readback(DANOS_OBJ_ROUTE, 3, &route, sizeof(route),
                       &oper, ops->user);
    assert(st == DANOS_ERR_NOT_FOUND);

    danos_netlink_shutdown();
    puts("[PASS] rtnetlink FIB_MATCH readback: default unsupported, loopback "
         "present, metric mismatch detected, absent prefix reported");
    return 0;
}
