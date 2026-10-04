/*
 * Test: ZAPI Parser (C1)
 * Verify ZAPI message parse/serialize round-trip.
 */
#include "../src/zapi/zapi.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int test_zapi_parse_serialize(void)
{
    /* Build a ZAPI message: ROUTE_ADD with small payload */
    uint8_t payload[] = {0x00, 0x00, 0x00, 0x64,  /* vrf_id=100 */
                         0x04,                      /* family=IPv4 */
                         0x18,                      /* prefix_len=24 */
                         0x0A, 0x00, 0x00, 0x00,   /* 10.0.0.0 */
                         0x02,                      /* proto=static */
                         0x01,                      /* admin_dist=1 */
                         0x00, 0x00, 0x00, 0x00,   /* metric=0 */
                         0x00};                     /* nh_count=0 */

    zapi_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.command = ZEBRA_ROUTE_ADD;
    msg.payload = payload;
    msg.payload_size = sizeof(payload);

    /* Serialize */
    uint8_t buf[256];
    int written = zapi_serialize(&msg, buf, sizeof(buf));
    assert(written > 0);
    assert(written == (int)(ZAPI_HEADER_SIZE + sizeof(payload)));

    /* Parse back */
    zapi_message_t parsed;
    int rc = zapi_parse(buf, (size_t)written, &parsed);
    assert(rc == 0);
    assert(parsed.header.length == (uint32_t)written);
    assert(parsed.header.marker == ZAPI_HEADER_MARKER);
    assert(parsed.header.version == ZAPI_VERSION);
    assert(parsed.header.command == ZEBRA_ROUTE_ADD);
    assert(parsed.payload_size == sizeof(payload));
    assert(memcmp(parsed.payload, payload, sizeof(payload)) == 0);

    printf("[PASS] test_zapi_parse_serialize: round-trip works\n");
    return 0;
}

int test_zapi_decoder(void)
{
    /* Test decoder helpers */
    uint8_t data[] = {0x01,                       /* u8 = 1 */
                      0x00, 0x64,                 /* u16 = 100 */
                      0x00, 0x00, 0x00, 0xC8,    /* u32 = 200 */
                      0x0A, 0x00, 0x00, 0x01};   /* IPv4 10.0.0.1 */

    zapi_decoder_t d;
    zapi_decoder_init(&d, data, sizeof(data));

    uint8_t u8;
    assert(zapi_decode_u8(&d, &u8) == 0);
    assert(u8 == 1);

    uint16_t u16;
    assert(zapi_decode_u16(&d, &u16) == 0);
    assert(u16 == 100);

    uint32_t u32;
    assert(zapi_decode_u32(&d, &u32) == 0);
    assert(u32 == 200);

    uint8_t ip[4];
    assert(zapi_decode_in_addr(&d, ip) == 0);
    assert(ip[0] == 10 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1);

    /* Past end */
    uint8_t extra;
    assert(zapi_decode_u8(&d, &extra) != 0);

    printf("[PASS] test_zapi_decoder: decode primitives work\n");
    return 0;
}

int test_frr_v6_header(void)
{
    /* FRR zserv v6: u16 length, marker, version, u32 VRF, u16 command. */
    uint8_t buf[] = { 0x00, 0x0B, 0xFE, 0x06, 0, 0, 0, 0,
                      0x00, ZEBRA_ROUTE_ADD, 0xAA };
    zapi_message_t msg;
    assert(zapi_parse_frr(buf, sizeof(buf), &msg) == 0);
    assert(msg.header.length == sizeof(buf));
    assert(msg.header.marker == 0xFE);
    assert(msg.header.version == ZAPI_VERSION);
    assert(msg.header.command == ZEBRA_ROUTE_ADD);
    assert(msg.payload_size == 1 && msg.payload[0] == 0xAA);
    printf("[PASS] test_frr_v6_header: real zserv header parsed\n");
    return 0;
}

int test_frr_route_decode(void)
{
    /* type=2, instance=0, flags=0, message=NEXTHOP, SAFI=1,
     * AF_INET=2, 10.0.0.0/24, one IPv4 nexthop 192.0.2.1. */
    uint8_t payload[] = {2, 0, 0, 0,0,0,0, 0,0,0,1, 1, 2,24,
                         10,0,0, 0,1, 0,0,0,0, 2,0, 192,0,2,1, 0,0,0,2};
    zapi_message_t msg = { .header = {0, 0xFE, 6, 7},
                           .payload = payload, .payload_size = sizeof(payload) };
    zapi_frr_route_t route;
    assert(zapi_decode_frr_route(&msg, &route) == 0);
    assert(route.family == 2 && route.prefix_len == 24);
    assert(route.nexthop_count == 1 && route.nexthops[0].has_gateway);
    assert(route.nexthops[0].gateway[0] == 192);
    printf("[PASS] test_frr_route_decode: native route fields decoded\n");
    return 0;
}

/* Two IPv4 nexthops must both decode.
 *
 * The decoder previously read the trailing ifindex only for nexthop type 3,
 * while zserv_encode_nexthop() writes it for type 2 as well. The first
 * nexthop therefore left four bytes unread and the second was decoded from
 * the wrong offset - silently, because the payload still had bytes left.
 * This test passes a type-2 nexthop followed by another, which is the
 * smallest case that exposes the desynchronisation.
 */
int test_frr_route_two_ipv4_nexthops(void)
{
    /* prefix 10.0.0.0/24, nexthop_count = 2
     * nh1: vrf 0, type 2 (IPV4), flags 0, gw 192.0.2.1, ifindex 2
     * nh2: vrf 0, type 2 (IPV4), flags 0, gw 192.0.2.2, ifindex 3
     * Correct order is vrf, type, flags, gateway, ifindex - the ifindex is
     * present for type 2 as well as type 3. */
    /* Header: type=2, instance=0, flags=0, message=NEXTHOP(1), safi=1,
     * family=AF_INET(2), prefix_len=24, prefix=10.0.0. */
    uint8_t payload[] = {
        2, 0,0, 0,0,0,0, 0,0,0,1, 1, 2, 24, 10,0,0,
        0, 2,                                  /* nexthop_count = 2 */
        0,0,0,0,  2, 0,  192,0,2,1,  0,0,0,2,
        0,0,0,0,  2, 0,  192,0,2,2,  0,0,0,3,
    };
    zapi_message_t msg = { .header = {0, 0xFE, 6, 7},
                           .payload = payload, .payload_size = sizeof(payload) };
    zapi_frr_route_t route;
    assert(zapi_decode_frr_route(&msg, &route) == 0);
    assert(route.nexthop_count == 2);
    assert(route.nexthops[0].has_gateway && route.nexthops[1].has_gateway);
    assert(route.nexthops[0].gateway[0] == 192 &&
           route.nexthops[0].gateway[3] == 1);
    assert(route.nexthops[1].gateway[3] == 2);   /* would be 0 if desynced */
    assert(route.nexthops[0].ifindex == 2);
    assert(route.nexthops[1].ifindex == 3);
    printf("[PASS] test_frr_route_two_ipv4_nexthops: ifindex consumed for "
           "type 2, second nexthop intact\n");
    return 0;
}

/* IPv6 and blackhole nexthops, previously skipped without consuming their
 * payload. Type 4 is a 16-byte address plus ifindex; type 6 carries none. */
int test_frr_route_ipv6_and_blackhole(void)
{
    /* Header: family=AF_INET6(10), prefix_len=64, prefix=2001:db8::/64. */
    uint8_t payload[] = {
        2, 0,0, 0,0,0,0, 0,0,0,1, 1, 10, 64,
        0x20,0x01,0x0d,0xb8, 0,0,0,0,         /* prefix: (64+7)/8 = 8 bytes */
        0, 2,                                  /* nexthop_count = 2 */
        0,0,0,0,  4, 0,                        /* type 4 = IPV6 */
        0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1,   0,0,0,7,
        0,0,0,0,  6, 0,                        /* type 6 = BLACKHOLE, no payload */
    };
    zapi_message_t msg = { .header = {0, 0xFE, 6, 7},
                           .payload = payload, .payload_size = sizeof(payload) };
    zapi_frr_route_t route;
    assert(zapi_decode_frr_route(&msg, &route) == 0);
    assert(route.nexthop_count == 2);
    assert(route.nexthops[0].type == ZAPI_NH_IPV6 && route.nexthops[0].has_gateway);
    assert(route.nexthops[0].gateway[0] == 0x20 && route.nexthops[0].gateway[15] == 1);
    assert(route.nexthops[0].ifindex == 7);
    assert(route.nexthops[1].type == ZAPI_NH_BLACKHOLE);
    assert(route.nexthops[1].blackhole);
    assert(!route.nexthops[1].has_gateway);

    /* An unknown type must be rejected: its length is unknowable, so
     * continuing would decode the rest of the message as garbage. */
    uint8_t bad[] = {
        2, 0,0, 0,0,0,0, 0,0,0,1, 1, 2, 24, 10,0,0,
        0, 1,                                  /* nexthop_count = 1 */
        0,0,0,0,  9, 0,                        /* type 9 does not exist */
    };
    zapi_message_t bmsg = { .header = {0, 0xFE, 6, 7},
                            .payload = bad, .payload_size = sizeof(bad) };
    zapi_frr_route_t broute;
    assert(zapi_decode_frr_route(&bmsg, &broute) != 0);

    printf("[PASS] test_frr_route_ipv6_and_blackhole: v6/blackhole decoded, "
           "unknown type rejected\n");
    return 0;
}

int test_zapi_invalid(void)
{
    /* Too short */
    uint8_t short_buf[4] = {0};
    zapi_message_t msg;
    assert(zapi_parse(short_buf, sizeof(short_buf), &msg) != 0);

    /* Bad marker */
    uint8_t bad_marker[16] = {0};
    bad_marker[4] = 0x00; bad_marker[5] = 0x00;  /* marker != 0xFFFF */
    assert(zapi_parse(bad_marker, sizeof(bad_marker), &msg) != 0);

    printf("[PASS] test_zapi_invalid: rejects bad input\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_zapi_parse_serialize() != 0) failed++;
    if (test_frr_v6_header() != 0) failed++;
    if (test_frr_route_decode() != 0) failed++;
    if (test_frr_route_two_ipv4_nexthops() != 0) failed++;
    if (test_frr_route_ipv6_and_blackhole() != 0) failed++;
    if (test_zapi_decoder() != 0) failed++;
    if (test_zapi_invalid() != 0) failed++;
    printf("=== fib_test (zapi_parse): %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}
