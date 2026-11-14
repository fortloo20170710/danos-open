/* Isolated live fixture only. Never run against a production backend.
 * The shell runner supplies VPP with loopbacks 1/2 and map 101:1,102:2. */
#include <danos/dpa.h>
#include <danos/core/backend_ops.h>
#include <danos/core/object_registry.h>
#include "../../danos-vpp/src/vpp_adapter.h"
#include "../../danos-vpp/src/api/vpp_api.h"
#include "../../danos-vpp/src/api/vpp_msgs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *socket = getenv("DANOS_READBACK_TEST_VPP_SOCK");
    if (!socket) { fputs("isolated fixture socket required\n", stderr); return 2; }
    assert(danos_vpp_adapter_install(true, socket) == 0);
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->route_verify && ops->readback);
    if (!g_default_store) g_default_store = danos_object_store_create(256);
    assert(g_default_store);
    danos_tx_t tx;
    assert(danos_tx_begin(&tx, "readback-live", NULL) == DANOS_OK);
    for (unsigned i = 0; i < 2; i++) {
        danos_iface_t iface = { .ifindex = 101+i, .admin_up = true };
        snprintf(iface.name, sizeof(iface.name), "test%u", i);
        assert(danos_iface_create(&tx, &iface) == DANOS_OK);
        danos_nexthop_t nh = { .id = 201+i, .ifindex = 101+i };
        nh.gateway.af = DANOS_AF_IPV4;
        assert(danos_nh_create(&tx, &nh) == DANOS_OK);
    }
    danos_nhgroup_t group = { .id = 301, .nh_count = 2, .nh_ids = {201, 202} };
    assert(danos_nhgroup_create(&tx, &group) == DANOS_OK);
    danos_route_t route = { .nhgroup_id = 301, .protocol = DANOS_ROUTE_PROTO_STATIC };
    route.prefix.addr.af = DANOS_AF_IPV4;
    route.prefix.addr.addr[0] = 198; route.prefix.addr.addr[1] = 51; route.prefix.addr.addr[2] = 100;
    route.prefix.prefix_len = 24;
    assert(danos_route_create(&tx, &route) == DANOS_OK);
    assert(danos_tx_commit_atomic(&tx) == DANOS_OK);
    uint64_t attempts = 0, failures = 0;
    assert(danos_programming_run(&attempts, &failures) == 3 && failures == 0);
    assert(danos_oper_count(DANOS_OBJ_IFACE) == 2);
    vpp_prefix_t initial_prefix = {.len = 24};
    memcpy(initial_prefix.addr.addr, route.prefix.addr.addr, 4);
    vpp_route_state_t initial_state;
    assert(vpp_msg_route_read(0, &initial_prefix, &initial_state) == DANOS_OK);
    for (unsigned i = 0; i < initial_state.n_paths; i++)
        printf("observed path if=%u flags=%u type=%u proto=%u table=%u weight=%u\n",
               initial_state.nh_ifs[i], initial_state.flags[i], initial_state.types[i],
               initial_state.protocols[i], initial_state.nh_tables[i], initial_state.weights[i]);
    assert(danos_oper_count(DANOS_OBJ_ROUTE) == 1);
    assert(danos_programming_run(&attempts, &failures) == 0 && failures == 0);

    vpp_prefix_t prefix = {.len = 24};
    memcpy(prefix.addr.addr, route.prefix.addr.addr, 4);
    vpp_ip_t nhs[2] = {0};
    uint32_t interfaces[2] = {1, 2};
    /* Remove one member without changing desired state or ledger. */
    assert(vpp_msg_ip_route_add_del(true, 0, &prefix, 1, nhs, interfaces) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failures) == 1 && failures == 0);
    vpp_route_state_t actual;
    assert(vpp_msg_route_read(0, &prefix, &actual) == DANOS_OK && actual.n_paths == 2);
    puts("[PASS] real VPP ECMP member drift repaired");
    /* Wrong gateway with an otherwise identical prefix and two paths. */
    nhs[0].addr[0] = 192; nhs[0].addr[2] = 2; nhs[0].addr[3] = 99;
    assert(vpp_msg_ip_route_add_del(true, 0, &prefix, 2, nhs, interfaces) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failures) == 1 && failures == 0);
    puts("[PASS] real VPP wrong gateway drift repaired");
    memset(nhs, 0, sizeof(nhs));
    /* Reordering an equal-cost set is not drift. */
    interfaces[0] = 2; interfaces[1] = 1;
    assert(vpp_msg_ip_route_add_del(true, 0, &prefix, 2, nhs, interfaces) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failures) == 0 && failures == 0);
    puts("[PASS] real VPP reordered ECMP set is equivalent");
    assert(vpp_msg_ip_route_add_del(false, 0, &prefix, 2, nhs, interfaces) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failures) == 1 && failures == 0);
    puts("[PASS] real VPP external route deletion repaired");
    assert(vpp_msg_sw_interface_set_flags(1, false) == DANOS_OK);
    assert(danos_programming_run(&attempts, &failures) == 1 && failures == 0);
    vpp_iface_state_t state;
    assert(vpp_msg_iface_read(1, &state) == DANOS_OK && state.admin_up);
    puts("[PASS] real VPP interface admin drift repaired and observed");
    assert(ops->iface_up(999, true, NULL) == DANOS_ERR_NOT_FOUND);
    assert(ops->iface_up(0, true, NULL) == DANOS_ERR_INVALID_ARG);
    puts("[PASS] real VPP missing mapping rejects programming");
    danos_vpp_api_disconnect();
    return 0;
}
