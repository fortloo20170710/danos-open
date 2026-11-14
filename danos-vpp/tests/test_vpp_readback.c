/* Independent wire fixtures for VPP 26.10 readback, no desired-state echo. */
#include "../src/api/vpp_api.h"
#include "../src/api/vpp_msgs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int mode;
static bool schema_ok = true, connected = true;
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v>>24); p[1] = (uint8_t)(v>>16); p[2] = (uint8_t)(v>>8); p[3] = (uint8_t)v; }
bool __wrap_danos_vpp_api_is_mock(void) { return false; }
bool __wrap_danos_vpp_api_is_connected(void) { return connected; }
int __wrap_danos_vpp_api_query(const char *request, const char *response,
                              const uint8_t *body, uint32_t size,
                              uint8_t *reply, uint32_t capacity)
{
    assert(strcmp(request, "ip_route_lookup_710d6471") == 0);
    assert(strcmp(response, "ip_route_lookup_reply_5d8febcb") == 0);
    if (!schema_ok) return -2;
    assert(size == 23 && body[4] == 1);
    assert(body[3] == 7 && capacity >= 204);
    memset(reply, 0, 204);
    reply[1] = 102;
    if (mode == 1) { put32(reply+6, (uint32_t)-6); return 10; }
    if (mode == 2) return -1;
    if (mode == 3) return -1; /* unexpected reply rejected by query */
    put32(reply+10, 7); /* table, stats, prefix */
    reply[18] = 0; reply[19] = 198; reply[20] = 51; reply[21] = 100;
    reply[35] = 24; reply[36] = mode == 4 ? 2 : 1;
    put32(reply+37, 9); put32(reply+41, 7);
    reply[49] = 1; /* path weight */
    reply[63] = 192; reply[64] = 0; reply[65] = 2; reply[66] = 1;
    return 204;
}
int __wrap_danos_vpp_api_dump_query(const char *request, const char *detail,
                                   const uint8_t *body, uint32_t size,
                                   danos_vpp_dump_cb cb, void *user)
{
    assert(strcmp(request, "sw_interface_dump_aa610c27") == 0);
    assert(strcmp(detail, "sw_interface_details_6c221fc7") == 0);
    if (!schema_ok) return -2;
    assert(size == 9 && body[3] == 9);
    if (mode == 1) return 0; /* complete empty dump */
    uint8_t bytes[295] = {0};
    bytes[1] = 102; put32(bytes+6, 9); put32(bytes+20, 3);
    bytes[36] = 5; bytes[37] = 220; /* mtu 1500 */
    if (cb(bytes, mode == 3 ? 38 : sizeof(bytes), user) != 0) return -1;
    return mode == 2 ? -1 : 0; /* detail received but barrier timed out */
}
int main(void)
{
    vpp_prefix_t prefix = {.len = 24};
    vpp_route_state_t route;
    assert(vpp_msg_route_read(7, &prefix, &route) == DANOS_OK);
    assert(route.table_id == 7 && route.n_paths == 1 && route.nh_ifs[0] == 9);
    assert(route.nhs[0].addr[0] == 192 && route.nhs[0].addr[3] == 1 && route.weights[0] == 1);
    mode = 1; assert(vpp_msg_route_read(7, &prefix, &route) == DANOS_ERR_NOT_FOUND);
    for (mode = 2; mode <= 4; mode++)
        assert(vpp_msg_route_read(7, &prefix, &route) == DANOS_ERR_BACKEND_IO);
    mode = 0;
    vpp_iface_state_t iface;
    assert(vpp_msg_iface_read(9, &iface) == DANOS_OK);
    assert(iface.admin_up && iface.link_up && iface.mtu == 1500);
    mode = 1; assert(vpp_msg_iface_read(9, &iface) == DANOS_ERR_NOT_FOUND);
    mode = 2; assert(vpp_msg_iface_read(9, &iface) == DANOS_ERR_BACKEND_IO);
    mode = 3; assert(vpp_msg_iface_read(9, &iface) == DANOS_ERR_BACKEND_IO);
    mode = 0; schema_ok = false;
    assert(vpp_msg_iface_read(9, &iface) == DANOS_ERR_NOT_SUPPORTED);
    assert(vpp_msg_route_read(7, &prefix, &route) == DANOS_ERR_NOT_SUPPORTED);
    connected = false;
    assert(vpp_msg_route_read(7, &prefix, &route) == DANOS_ERR_BACKEND_DOWN);
    assert(vpp_msg_iface_read(9, &iface) == DANOS_ERR_BACKEND_DOWN);
    puts("[PASS] actual field decoding, incomplete/error replies and schema mismatch");
    return 0;
}
