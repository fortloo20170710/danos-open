/*
 * Test: VPP multipart dump decoders and OPER readback
 *
 * The detail bodies are encoded here from the FDio .api field order, then
 * injected into the mock dump store so the real decoder and adapter readback
 * paths run without a VPP instance.
 */
#include "../src/api/vpp_api.h"
#include "../src/api/vpp_dump.h"
#include "../src/api/vpp_wire.h"
#include "../src/vpp_adapter.h"
#include <danos/core/backend_ops.h>
#include <danos/dpa.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_fib_path(vpp_buf_t *b, uint32_t sw_if_index, uint32_t table,
                         const uint8_t gw[4], uint32_t type)
{
    vpp_buf_put_u32(b, sw_if_index);
    vpp_buf_put_u32(b, table);
    vpp_buf_put_u32(b, 0);              /* rpf_id */
    vpp_buf_put_u8(b, 1);               /* weight */
    vpp_buf_put_u8(b, 0);               /* preference */
    vpp_buf_put_u32(b, type);
    vpp_buf_put_u32(b, 0);              /* flags */
    vpp_buf_put_u32(b, 0);              /* proto IP4 */
    uint8_t nh[16] = {0};
    memcpy(nh, gw, 4);
    vpp_buf_put_bytes(b, nh, sizeof(nh));
    vpp_buf_put_u32(b, 0);              /* via_label */
    vpp_buf_put_u32(b, 0);              /* obj_id */
    vpp_buf_put_u32(b, 0);              /* classify_table_index */
    vpp_buf_put_u8(b, 0);               /* n_labels */
    uint8_t labels[16 * 7] = {0};
    vpp_buf_put_bytes(b, labels, sizeof(labels));
}

static void make_iface(vpp_buf_t *b)
{
    vpp_buf_put_u32(b, 7);              /* context */
    vpp_buf_put_u32(b, 3);              /* sw_if_index */
    vpp_buf_put_u32(b, 3);              /* sup */
    uint8_t mac[6] = {0};
    vpp_buf_put_bytes(b, mac, sizeof(mac));
    vpp_buf_put_u32(b, 3);              /* ADMIN_UP|LINK_UP */
    vpp_buf_put_u32(b, 0);              /* type */
    vpp_buf_put_u32(b, 2);              /* link_duplex */
    vpp_buf_put_u32(b, 1000000);        /* link_speed */
    vpp_buf_put_u16(b, 1500);           /* link_mtu */
    uint32_t mtu[4] = {1500, 1500, 1500, 0};
    vpp_buf_put_bytes(b, mtu, sizeof(mtu));
    vpp_buf_put_u32(b, 0);              /* sub_id */
    vpp_buf_put_u8(b, 0);               /* sub tags */
    vpp_buf_put_u16(b, 0);
    vpp_buf_put_u16(b, 0);
    vpp_buf_put_u32(b, 0);              /* sub_if_flags */
    vpp_buf_put_u32(b, 0);              /* vtr_op */
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u16(b, 0);              /* outer_tag */
    uint8_t dmac[6] = {0}, smac[6] = {0};
    vpp_buf_put_bytes(b, dmac, sizeof(dmac));
    vpp_buf_put_bytes(b, smac, sizeof(smac));
    vpp_buf_put_u16(b, 0);              /* b_vlanid */
    vpp_buf_put_u32(b, 0);              /* i_sid */
    char name[64] = {0};
    snprintf(name, sizeof(name), "GigabitEthernet0/8/0");
    vpp_buf_put_bytes(b, name, sizeof(name));
}

static void make_table(vpp_buf_t *b)
{
    vpp_buf_put_u32(b, 8);              /* context */
    vpp_buf_put_u32(b, 777);            /* table_id */
    vpp_buf_put_u8(b, 0);               /* is_ip6 */
    char name[64] = {0};
    snprintf(name, sizeof(name), "danos-live");
    vpp_buf_put_bytes(b, name, sizeof(name));
}

static void make_route6(vpp_buf_t *b)
{
    vpp_buf_put_u32(b, 9);
    vpp_buf_put_u32(b, 777);
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u8(b, 1);               /* ADDRESS_IP6 */
    uint8_t addr[16] = {0x20, 0x01, 0x0d, 0xb8};
    vpp_buf_put_bytes(b, addr, sizeof(addr));
    vpp_buf_put_u8(b, 64);
    vpp_buf_put_u8(b, 1);
    vpp_buf_put_u32(b, 1);              /* sw_if_index */
    vpp_buf_put_u32(b, 777);
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u8(b, 1);
    vpp_buf_put_u8(b, 0);
    vpp_buf_put_u32(b, 0);              /* normal */
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u32(b, 1);              /* proto IPv6 */
    uint8_t nh[16] = {0};
    nh[0] = 0x20; nh[1] = 0x01; nh[15] = 1;
    vpp_buf_put_bytes(b, nh, sizeof(nh));
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u32(b, 0);
    vpp_buf_put_u8(b, 0);
    uint8_t labels[16 * 7] = {0};
    vpp_buf_put_bytes(b, labels, sizeof(labels));
}

static void make_route(vpp_buf_t *b, const uint8_t gw2[4])
{
    vpp_buf_put_u32(b, 9);              /* context */
    vpp_buf_put_u32(b, 777);            /* table */
    vpp_buf_put_u32(b, 0);              /* stats_index */
    vpp_buf_put_u8(b, 0);               /* ADDRESS_IP4 */
    uint8_t addr[16] = {203, 0, 113, 0};
    vpp_buf_put_bytes(b, addr, sizeof(addr));
    vpp_buf_put_u8(b, 24);              /* prefix len */
    vpp_buf_put_u8(b, 2);               /* n_paths */
    uint8_t gw1[4] = {10, 0, 0, 1};
    put_fib_path(b, 1, 777, gw1, 0);
    put_fib_path(b, 2, 777, gw2, 0);
}

int main(void)
{
    danos_vpp_adapter_install(false, NULL);
    const danos_backend_ops_t *ops = danos_backend_ops_get();
    assert(ops && ops->readback && ops->route_readback);

    /* No injected dumps: no readback capability, not "object absent". */
    danos_iface_t empty_iface;
    memset(&empty_iface, 0, sizeof(empty_iface));
    empty_iface.ifindex = 1;
    danos_iface_t empty_oper;
    assert(ops->readback(DANOS_OBJ_IFACE, 1, &empty_iface,
                         sizeof(empty_iface), &empty_oper, ops->user)
           == DANOS_ERR_NOT_SUPPORTED);
    assert(danos_vpp_api_mock_dump_total() == 0);

    vpp_buf_t body;
    vpp_buf_init(&body, 512);
    make_iface(&body);
    danos_vpp_api_mock_dump_add("sw_interface_details", body.data, body.len);
    vpp_buf_reset(&body);
    make_table(&body);
    danos_vpp_api_mock_dump_add("ip_table_details", body.data, body.len);
    uint8_t gw2[4] = {10, 0, 0, 2};
    vpp_buf_reset(&body);
    make_route(&body, gw2);
    danos_vpp_api_mock_dump_add("ip_route_details", body.data, body.len);
    vpp_buf_free(&body);

    /* Interface: name, MTU and carrier come from the dump. */
    danos_iface_t iface;
    memset(&iface, 0, sizeof(iface));
    iface.ifindex = 3;
    danos_iface_t oper_iface;
    assert(ops->readback(DANOS_OBJ_IFACE, 3, &iface, sizeof(iface),
                         &oper_iface, ops->user) == DANOS_OK);
    assert(strcmp(oper_iface.name, "GigabitEthernet0/8/0") == 0);
    assert(oper_iface.mtu == 1500);
    assert(oper_iface.admin_up && oper_iface.link_up);

    /* VRF: the VPP table name and family populate OPER. */
    danos_vrf_t vrf;
    memset(&vrf, 0, sizeof(vrf));
    vrf.vrf_id = 777;
    snprintf(vrf.name, sizeof(vrf.name), "desired");
    danos_vrf_t oper_vrf;
    assert(ops->readback(DANOS_OBJ_VRF, 777, &vrf, sizeof(vrf),
                         &oper_vrf, ops->user) == DANOS_OK);
    assert(strcmp(oper_vrf.name, "danos-live") == 0);
    assert(oper_vrf.ipv4_active);

    /* Route/ECMP: both members must match as a set. */
    danos_resolved_route_t rr;
    memset(&rr, 0, sizeof(rr));
    rr.route.vrf_id = 777;
    rr.route.prefix.addr.af = DANOS_AF_IPV4;
    rr.route.prefix.addr.addr[0] = 203;
    rr.route.prefix.addr.addr[2] = 113;
    rr.route.prefix.prefix_len = 24;
    rr.nh_count = 2;
    rr.nh_oif[0] = 1;
    rr.nh_oif[1] = 2;
    memcpy(rr.nh_gw[0], "\x0a\x00\x00\x01", 4);
    memcpy(rr.nh_gw[1], gw2, 4);
    danos_route_t oper_route;
    assert(ops->route_readback(&rr, &oper_route, ops->user) == DANOS_OK);

    /* VPP dumps do not expose the metric: a metric-bearing route must produce
     * an explicit unsupported result, never a false OK. */
    rr.route.metric = 42;
    assert(ops->route_readback(&rr, &oper_route, ops->user)
           == DANOS_ERR_NOT_SUPPORTED);
    rr.route.metric = 0;

    /* A different second gateway is a verification failure, not absence. */
    rr.nh_gw[1][3] = 99;
    assert(ops->route_readback(&rr, &oper_route, ops->user)
           == DANOS_ERR_VERIFY_FAIL);

    /* An unknown table is absent. */
    rr.route.vrf_id = 778;
    assert(ops->route_readback(&rr, &oper_route, ops->user)
           == DANOS_ERR_NOT_FOUND);

    /* Decoder error paths: NULL, every truncated prefix, malformed counts. */
    vpp_iface_dump_t iface_dump;
    vpp_table_dump_t table_dump;
    vpp_route_dump_t route_dump;
    assert(vpp_dump_decode_iface(NULL, 0, &iface_dump) == -1);
    assert(vpp_dump_decode_iface(NULL, 1, NULL) == -1);
    assert(vpp_dump_decode_table(NULL, 0, &table_dump) == -1);
    assert(vpp_dump_decode_table(NULL, 1, NULL) == -1);
    assert(vpp_dump_decode_route(NULL, 0, &route_dump) == -1);
    assert(vpp_dump_decode_route(NULL, 1, NULL) == -1);

    vpp_buf_t edge;
    vpp_buf_init(&edge, 512);
    make_iface(&edge);
    for (uint32_t n = 0; n < edge.len; n++)
        assert(vpp_dump_decode_iface(edge.data, n, &iface_dump) == -1);
    /* A full fixed-width name without a NUL is truncated safely. */
    vpp_buf_reset(&edge);
    make_iface(&edge);
    memset(edge.data + edge.len - 64, 'A', 64);
    assert(vpp_dump_decode_iface(edge.data, edge.len, &iface_dump) == 0);
    assert(strlen(iface_dump.name) == 63);

    vpp_buf_reset(&edge);
    make_table(&edge);
    for (uint32_t n = 0; n < edge.len; n++)
        assert(vpp_dump_decode_table(edge.data, n, &table_dump) == -1);

    uint8_t gw[4] = {10, 0, 0, 2};
    vpp_buf_reset(&edge);
    make_route(&edge, gw);
    for (uint32_t n = 0; n < edge.len; n++)
        assert(vpp_dump_decode_route(edge.data, n, &route_dump) == -1);
    /* n_paths is the byte just after prefix. */
    uint32_t n_paths_off = 4 + 4 + 4 + 1 + 16 + 1;
    edge.data[n_paths_off] = 65;
    assert(vpp_dump_decode_route(edge.data, edge.len, &route_dump) == -1);
    vpp_buf_reset(&edge);
    make_route(&edge, gw);
    /* n_labels of the first path. */
    uint32_t path_off = n_paths_off + 1;
    edge.data[path_off + 54] = 17;
    assert(vpp_dump_decode_route(edge.data, edge.len, &route_dump) == -1);
    vpp_buf_reset(&edge);
    make_route6(&edge);
    assert(vpp_dump_decode_route(edge.data, edge.len, &route_dump) == 0);
    assert(route_dump.is_ipv6);
    assert(route_dump.prefix_len == 64);
    assert(route_dump.n_paths == 1);
    assert(route_dump.paths[0].gw_is_ipv6);
    vpp_buf_free(&edge);

    danos_vpp_api_mock_dump_clear();
    printf("[PASS] test_vpp_dump: interface/table/route decoding and OPER "
           "readback\n");
    return 0;
}
