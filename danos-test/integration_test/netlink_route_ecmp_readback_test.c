/*
 * Live ECMP route readback in an isolated network namespace.
 *
 * A plain RTM_GETROUTE lookup cannot expose the full multipath set or the
 * route metric; this test pins the FIB_MATCH/dump behaviour against a real
 * kernel.  It needs root, CAP_SYS_ADMIN and iproute2, and prints [SKIP] when
 * the environment cannot provide them (non-root CI, missing dummy module).
 */
#define _GNU_SOURCE 1

#include <danos/core/backend_ops.h>
#include "../../danos-netlink/danos_netlink.h"

#include <assert.h>
#include <net/if.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(const char *cmd)
{
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void fill(danos_resolved_route_t *r, uint32_t metric,
                 uint32_t nh_count)
{
    memset(r, 0, sizeof(*r));
    r->route.prefix.addr.af = DANOS_AF_IPV4;
    r->route.prefix.addr.addr[0] = 10;
    r->route.prefix.addr.addr[1] = 77;
    r->route.prefix.addr.addr[2] = 0;
    r->route.prefix.addr.addr[3] = 0;
    r->route.prefix.prefix_len = 24;
    r->route.protocol = DANOS_ROUTE_PROTO_STATIC;
    r->route.metric = metric;
    r->nh_count = nh_count;
    r->nh_oif[0] = if_nametoindex("d0");
    r->nh_oif[1] = if_nametoindex("d1");
    r->nh_gw[0][0] = r->nh_gw[1][0] = 0;
}

int main(void)
{
    if (geteuid() != 0) {
        puts("[SKIP] netns ECMP readback needs root");
        return 0;
    }
    if (unshare(CLONE_NEWNET) != 0) {
        puts("[SKIP] unshare(CLONE_NEWNET) unavailable");
        return 0;
    }
    if (run("ip link set lo up") != 0 ||
        run("ip link add d0 type dummy && ip link set d0 up && "
            "ip link add d1 type dummy && ip link set d1 up") != 0) {
        puts("[SKIP] dummy interface/iproute2 unavailable");
        return 0;
    }
    if (run("ip route add 10.77.0.0/24 metric 7 "
            "nexthop dev d0 nexthop dev d1") != 0) {
        puts("[SKIP] multipath route add unavailable");
        return 0;
    }

    if (danos_netlink_init(true) != 0 || !danos_netlink_is_real()) {
        puts("[SKIP] rtnetlink socket unavailable");
        return 0;
    }
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->route_readback);

    danos_resolved_route_t route;
    danos_route_t oper;

    /* The full two-member set and the metric must verify. */
    fill(&route, 7, 2);
    assert(route.nh_oif[0] != 0 && route.nh_oif[1] != 0);
    assert(ops->route_readback(&route, &oper, ops->user) == DANOS_OK);

    /* A metric mismatch must be reported as a present-but-different route. */
    fill(&route, 8, 2);
    assert(ops->route_readback(&route, &oper, ops->user)
           == DANOS_ERR_VERIFY_FAIL);

    /* A partially withdrawn ECMP group (one member) must not verify. */
    assert(run("ip route replace 10.77.0.0/24 metric 7 "
               "nexthop dev d0") == 0);
    fill(&route, 7, 2);
    assert(ops->route_readback(&route, &oper, ops->user)
           == DANOS_ERR_VERIFY_FAIL);

    /* Restore both members, then remove the route: absence is NOT_FOUND. */
    assert(run("ip route replace 10.77.0.0/24 metric 7 "
               "nexthop dev d0 nexthop dev d1") == 0);
    fill(&route, 7, 2);
    assert(ops->route_readback(&route, &oper, ops->user) == DANOS_OK);
    assert(run("ip route del 10.77.0.0/24") == 0);
    assert(ops->route_readback(&route, &oper, ops->user)
           == DANOS_ERR_NOT_FOUND);

    danos_netlink_shutdown();
    puts("[PASS] netns ECMP readback: members, metric, partial withdrawal, "
         "absence");
    return 0;
}
