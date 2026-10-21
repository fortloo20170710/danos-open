#include <danos/core/backend_ops.h>
#include "../../danos-netlink/danos_netlink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    if (danos_netlink_init(true) != 0 || !danos_netlink_is_real()) {
        puts("[SKIP] rtnetlink route readback socket unavailable");
        return 0;
    }
    danos_netlink_register_backend();
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->readback);

    /* The host's default route is a stable positive read-only probe when
     * configured. Environments without one still exercise a missing route. */
    danos_route_t route;
    memset(&route, 0, sizeof(route));
    route.prefix.addr.af = DANOS_AF_IPV4;
    route.prefix.prefix_len = 0;
    danos_route_t oper;
    danos_status_t st = ops->readback(DANOS_OBJ_ROUTE, 1, &route,
                                      sizeof(route), &oper, ops->user);
    assert(st == DANOS_OK || st == DANOS_ERR_NOT_FOUND);

    memset(&route, 0, sizeof(route));
    route.prefix.addr.af = DANOS_AF_IPV4;
    route.prefix.addr.addr[0] = 203;
    route.prefix.addr.addr[1] = 0;
    route.prefix.addr.addr[2] = 113;
    route.prefix.addr.addr[3] = 192;
    route.prefix.prefix_len = 26;
    st = ops->readback(DANOS_OBJ_ROUTE, 2, &route, sizeof(route),
                       &oper, ops->user);
    if (st == DANOS_OK) {
        puts("[SKIP] documentation prefix is routed in this environment");
    } else {
        assert(st == DANOS_ERR_NOT_FOUND);
        puts("[PASS] rtnetlink reports absent exact IPv4 prefix");
    }

    danos_netlink_shutdown();
    return 0;
}
