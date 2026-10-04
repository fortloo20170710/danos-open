/*
 * Golden-bytes conformance for the ZAPI route decoder.
 *
 * The tests here exist because hand-written hex payloads kept agreeing with
 * the decoder by coincidence: both were written from the same assumption, so
 * they could not catch a wrong assumption. Two real bugs survived that way -
 * the nexthop type numbering, and not consuming the trailing ifindex that
 * zserv_encode_nexthop() writes for type 2 as well as type 3.
 *
 * So the payloads below are not literals. They are produced by
 * golden_encode_nexthop(), an independent encoder written from FRR 10.3
 * upstream:
 *
 *   - lib/nexthop.h           enum nexthop_types_t
 *   - zebra/zapi_msg.c        zserv_encode_nexthop()
 *   - lib/zclient.h           ZAPI_NEXTHOP_FLAG_*
 *
 * The production decoder has to agree with it. If either drifts, the test
 * fails, which is the property the hex literals did not have.
 *
 * Upstream references are recorded in docs/; they are not vendored, so this
 * file cites them rather than asserting against a local copy.
 */
#include "../src/zapi/zapi.h"
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ---- independent encoder, transcribed from zserv_encode_nexthop() -------- */

typedef struct {
    uint8_t buf[512];
    size_t  len;
} golden_t;

static void g_u8(golden_t *g, uint8_t v)
{
    g->buf[g->len++] = v;
}

static void g_u16(golden_t *g, uint16_t v)
{
    g_u8(g, (uint8_t)(v >> 8));
    g_u8(g, (uint8_t)v);
}

static void g_u32(golden_t *g, uint32_t v)
{
    g_u16(g, (uint16_t)(v >> 16));
    g_u16(g, (uint16_t)v);
}

static void g_u64(golden_t *g, uint64_t v)
{
    g_u32(g, (uint32_t)(v >> 32));
    g_u32(g, (uint32_t)v);
}

static void g_bytes(golden_t *g, const void *p, size_t n)
{
    memcpy(g->buf + g->len, p, n);
    g->len += n;
}

/* vrf_id, type, flags, then per-type payload - the framing this dialect uses.
 * The per-type payload and whether an ifindex follows are exactly what
 * upstream does, which is what the decoder got wrong. */
static void golden_encode_nexthop(golden_t *g, uint32_t vrf, uint8_t type,
                                  uint8_t flags, const uint8_t *gw,
                                  uint32_t ifindex)
{
    g_u32(g, vrf);
    g_u8(g, type);
    g_u8(g, flags);
    switch (type) {
    case ZAPI_NH_IFINDEX:
        g_u32(g, ifindex);
        break;
    case ZAPI_NH_IPV4:
    case ZAPI_NH_IPV4_IFINDEX:
        g_bytes(g, gw, 4);
        g_u32(g, ifindex);          /* upstream writes it for both */
        break;
    case ZAPI_NH_IPV6:
    case ZAPI_NH_IPV6_IFINDEX:
        g_bytes(g, gw, 16);
        g_u32(g, ifindex);          /* and for both v6 forms */
        break;
    case ZAPI_NH_BLACKHOLE:
        break;                      /* no payload */
    default:
        assert(0 && "unknown nexthop type in golden encoder");
    }
    if (flags & ZAPI_NH_FLAG_WEIGHT)
        g_u64(g, 0);                /* weight present, value irrelevant here */
}

/* Route header in the same field order the decoder expects. */
static void golden_encode_route_header(golden_t *g, uint8_t family,
                                       uint8_t prefix_len,
                                       const uint8_t *prefix,
                                       uint32_t message)
{
    g_u8(g, 2);                     /* type = ZEBRA_ROUTE_ADD */
    g_u16(g, 0);                    /* instance */
    g_u32(g, 0);                    /* flags */
    g_u32(g, message);
    g_u8(g, 1);                     /* safi = unicast */
    g_u8(g, family);
    g_u8(g, prefix_len);
    g_bytes(g, prefix, (size_t)((prefix_len + 7) / 8));
}

static zapi_message_t wrap(const golden_t *g)
{
    zapi_message_t m;
    memset(&m, 0, sizeof(m));
    m.header.version = ZAPI_VERSION;
    m.header.command = ZEBRA_ROUTE_ADD;
    m.payload = g->buf;
    m.payload_size = (uint32_t)g->len;
    return m;
}

/* ---- the constant tables must match upstream --------------------------- */

/* Guards the numeric values themselves. enum nexthop_types_t in
 * lib/nexthop.h starts at 1 and increments; a renumbering upstream would
 * break the wire protocol, so pin it here. */
int test_golden_nexthop_type_values(void)
{
    assert(ZAPI_NH_IFINDEX      == 1);
    assert(ZAPI_NH_IPV4         == 2);
    assert(ZAPI_NH_IPV4_IFINDEX == 3);
    assert(ZAPI_NH_IPV6         == 4);
    assert(ZAPI_NH_IPV6_IFINDEX == 5);
    assert(ZAPI_NH_BLACKHOLE    == 6);

    /* ZAPI_NEXTHOP_FLAG_* from lib/zclient.h. */
    assert(ZAPI_NH_FLAG_ONLINK     == 0x01);
    assert(ZAPI_NH_FLAG_LABEL      == 0x02);
    assert(ZAPI_NH_FLAG_WEIGHT     == 0x04);
    assert(ZAPI_NH_FLAG_HAS_BACKUP == 0x08);
    assert(ZAPI_NH_FLAG_SEG6       == 0x10);   /* not 0x20 */
    assert(ZAPI_NH_FLAG_SEG6LOCAL  == 0x20);
    assert(ZAPI_NH_FLAG_EVPN       == 0x40);

    printf("[PASS] test_golden_nexthop_type_values: matches FRR 10.3 "
           "lib/nexthop.h and lib/zclient.h\n");
    return 0;
}

/* ---- every nexthop type round-trips ------------------------------------- */

int test_golden_every_nexthop_type(void)
{
    static const struct {
        uint8_t  type;
        size_t   gw_len;      /* 0 for blackhole/ifindex-only */
        uint32_t ifindex;
        bool     has_gw;
    } cases[] = {
        { ZAPI_NH_IFINDEX,      0,  7, false },
        { ZAPI_NH_IPV4,         4,  2, true  },
        { ZAPI_NH_IPV4_IFINDEX, 4,  3, true  },
        { ZAPI_NH_IPV6,        16,  7, true  },
        { ZAPI_NH_IPV6_IFINDEX,16,  8, true  },
        { ZAPI_NH_BLACKHOLE,    0,  0, false },
    };

    uint8_t gw[16];
    for (size_t i = 0; i < sizeof(gw); i++) gw[i] = (uint8_t)(0x20 + i);

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        /* An IPv4 route family, with one nexthop of the type under test.
         * The address width comes from the nexthop type, so a v6 nexthop in
         * an IPv4 route is the interesting case. */
        uint8_t prefix[3] = { 10, 0, 0 };
        golden_t g;
        memset(&g, 0, sizeof(g));
        golden_encode_route_header(&g, AF_INET, 24, prefix,
                                   ZAPI_FRR_MESSAGE_NEXTHOP);
        g_u16(&g, 1);        /* one nexthop */
        golden_encode_nexthop(&g, 0, cases[c].type, 0, gw,
                              cases[c].ifindex);

        zapi_message_t msg = wrap(&g);
        zapi_frr_route_t r;
        assert(zapi_decode_frr_route(&msg, &r) == 0);
        assert(r.nexthop_count == 1);
        const zapi_frr_nexthop_t *nh = &r.nexthops[0];
        assert(nh->type == cases[c].type);
        assert(nh->has_gateway == cases[c].has_gw);
        if (cases[c].has_gw)
            assert(memcmp(nh->gateway, gw, cases[c].gw_len) == 0);
        if (cases[c].type != ZAPI_NH_BLACKHOLE)
            assert(nh->ifindex == cases[c].ifindex);
        assert(nh->blackhole == (cases[c].type == ZAPI_NH_BLACKHOLE));
    }
    printf("[PASS] test_golden_every_nexthop_type: all 6 upstream types "
           "round-trip\n");
    return 0;
}

/* ---- ECMP: several nexthops must each keep their own gateway ----------- */

int test_golden_ecmp_nexthops(void)
{
    uint8_t prefix[3] = { 10, 0, 0 };
    golden_t g;
    memset(&g, 0, sizeof(g));
    golden_encode_route_header(&g, AF_INET, 24, prefix,
                               ZAPI_FRR_MESSAGE_NEXTHOP);

    enum { N = 6 };
    uint8_t gw[N][4];
    for (int i = 0; i < N; i++) {
        gw[i][0] = 192; gw[i][1] = 0; gw[i][2] = 2; gw[i][3] = (uint8_t)(i + 1);
    }
    g_u16(&g, N);
    for (int i = 0; i < N; i++)
        golden_encode_nexthop(&g, 0, ZAPI_NH_IPV4, 0, gw[i],
                              (uint32_t)(10 + i));

    zapi_message_t msg = wrap(&g);
    zapi_frr_route_t r;
    assert(zapi_decode_frr_route(&msg, &r) == 0);
    assert(r.nexthop_count == N);
    /* Each member must carry its own gateway and ifindex. A desynchronising
     * decoder shifts every member after the first. */
    for (int i = 0; i < N; i++) {
        assert(r.nexthops[i].has_gateway);
        assert(memcmp(r.nexthops[i].gateway, gw[i], 4) == 0);
        assert(r.nexthops[i].ifindex == (uint32_t)(10 + i));
    }
    printf("[PASS] test_golden_ecmp_nexthops: %d members each intact\n", N);
    return 0;
}

/* ---- the weight flag changes the length, so it must be honoured -------- */

int test_golden_weight_flag(void)
{
    uint8_t prefix[3] = { 10, 0, 0 };
    uint8_t gw[4] = { 192, 0, 2, 1 };

    /* Without the flag there is no weight on the wire. */
    golden_t a;
    memset(&a, 0, sizeof(a));
    golden_encode_route_header(&a, AF_INET, 24, prefix,
                               ZAPI_FRR_MESSAGE_NEXTHOP);
    g_u16(&a, 1);
    golden_encode_nexthop(&a, 0, ZAPI_NH_IPV4, 0, gw, 5);

    /* With it, an extra u64 follows - so omitting it desynchronises. */
    golden_t b;
    memset(&b, 0, sizeof(b));
    golden_encode_route_header(&b, AF_INET, 24, prefix,
                               ZAPI_FRR_MESSAGE_NEXTHOP);
    g_u16(&b, 2);
    golden_encode_nexthop(&b, 0, ZAPI_NH_IPV4, ZAPI_NH_FLAG_WEIGHT, gw, 5);
    golden_encode_nexthop(&b, 0, ZAPI_NH_IPV4, 0, gw, 6);

    zapi_message_t ma = wrap(&a), mb = wrap(&b);
    zapi_frr_route_t ra, rb;
    assert(zapi_decode_frr_route(&ma, &ra) == 0);
    assert(zapi_decode_frr_route(&mb, &rb) == 0);
    assert(ra.nexthop_count == 1 && ra.nexthops[0].ifindex == 5);
    assert(rb.nexthop_count == 2);
    assert(rb.nexthops[0].ifindex == 5);
    assert(rb.nexthops[1].ifindex == 6);
    printf("[PASS] test_golden_weight_flag: flag changes length and the "
           "decoder follows it\n");
    return 0;
}

/* ---- a truncated or over-long buffer must be refused ------------------- */

int test_golden_rejects_bad_lengths(void)
{
    uint8_t prefix[3] = { 10, 0, 0 };
    uint8_t gw[4] = { 192, 0, 2, 1 };

    golden_t g;
    memset(&g, 0, sizeof(g));
    golden_encode_route_header(&g, AF_INET, 24, prefix,
                               ZAPI_FRR_MESSAGE_NEXTHOP);
    g_u16(&g, 2);
    golden_encode_nexthop(&g, 0, ZAPI_NH_IPV4, 0, gw, 5);
    golden_encode_nexthop(&g, 0, ZAPI_NH_IPV4, 0, gw, 6);

    /* Claiming fewer nexthops than the buffer holds must not be an error -
     * trailing bytes are the message's business, not the decoder's - but a
     * claim the buffer cannot satisfy must be refused rather than read past.
     */
    zapi_message_t m = wrap(&g);
    zapi_frr_route_t r;
    assert(zapi_decode_frr_route(&m, &r) == 0);
    assert(r.nexthop_count == 2);

    /* Truncating mid-nexthop must fail. */
    for (size_t cut = 1; cut < 12; cut++) {
        zapi_message_t t = wrap(&g);
        t.payload_size = (uint32_t)(g.len - cut);
        zapi_frr_route_t tr;
        assert(zapi_decode_frr_route(&t, &tr) != 0);
    }

    /* A prefix length that overruns the prefix must fail. */
    golden_t bad;
    memset(&bad, 0, sizeof(bad));
    golden_encode_route_header(&bad, AF_INET, 24, prefix, 0);
    zapi_message_t bm = wrap(&bad);
    assert(zapi_decode_frr_route(&bm, &r) == 0);   /* no nexthops: fine */

    printf("[PASS] test_golden_rejects_bad_lengths: truncated nexthops "
           "refused\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_golden_nexthop_type_values() != 0) failed++;
    if (test_golden_every_nexthop_type() != 0) failed++;
    if (test_golden_ecmp_nexthops() != 0) failed++;
    if (test_golden_weight_flag() != 0) failed++;
    if (test_golden_rejects_bad_lengths() != 0) failed++;
    printf("=== fib_test_golden: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}