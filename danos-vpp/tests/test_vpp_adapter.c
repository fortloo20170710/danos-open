#include "../src/vpp_adapter.h"
#include "../src/api/vpp_api.h"
#include <danos/core/backend_ops.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

int main(void)
{
    const char *invalid[] = {"1:2,junk", "1:2,", "1:2,1:3", "-1:2", "0:1", "1:0",
                            "1:4294967295", "4294967296:1", "1:2z", "1:+2", "1: 2"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        assert(setenv("DANOS_VPP_IFINDEX_MAP", invalid[i], 1) == 0);
        assert(danos_vpp_adapter_install(false, NULL) == -1);
    }
    unsetenv("DANOS_VPP_IFINDEX_MAP");
    assert(danos_vpp_adapter_install(true, NULL) == -1);
    assert(setenv("DANOS_VPP_IFINDEX_MAP", "101:1,102:2,103:1", 1) == 0);
    assert(danos_vpp_adapter_install(false, NULL) == 0);
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    uint64_t sent;
    danos_vpp_api_get_stats(&sent, NULL, NULL, NULL);
    assert(sent == 0);
    assert(ops->iface_up(999, true, NULL) == DANOS_ERR_NOT_FOUND);
    assert(ops->iface_up(0, true, NULL) == DANOS_ERR_INVALID_ARG);
    danos_resolved_route_t res = { .has_gw = true, .oif = 999 };
    res.route.prefix.addr.af = DANOS_AF_IPV4;
    assert(ops->route_add(&res, NULL) == DANOS_ERR_NOT_FOUND);
    danos_vpp_api_get_stats(&sent, NULL, NULL, NULL);
    assert(sent == 0); /* rejected before any API write */
    assert(ops->iface_up(101, true, NULL) == DANOS_OK);
    assert(setenv("DANOS_VPP_IFINDEX_MAP", "999:9", 1) == 0);
    assert(ops->iface_up(101, true, NULL) == DANOS_OK); /* installed snapshot */
    assert(ops->iface_up(999, true, NULL) == DANOS_ERR_NOT_FOUND);
    res.route.flags = DANOS_ROUTE_FLAG_BLACKHOLE;
    assert(ops->route_add(&res, NULL) == DANOS_ERR_NOT_SUPPORTED);
    unsetenv("DANOS_VPP_IFINDEX_MAP");
    danos_vpp_api_disconnect();
    puts("[PASS] strict complete immutable VPP map, no guessed egress");
    return 0;
}
