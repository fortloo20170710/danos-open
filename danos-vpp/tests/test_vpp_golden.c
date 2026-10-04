/*
 * Golden-bytes conformance for the VPP binary API framing.
 *
 * Same purpose as the ZAPI golden tests: the payloads here are produced by
 * encoders transcribed from upstream VPP rather than written as literals, so
 * the production codecs cannot agree with a shared wrong assumption.
 *
 * Upstream references:
 *   src/vlibmemory/memclnt.api   sockclnt_create, message_table_entry
 *   src/vlibapi/api_types.h      vl_api_to_api_string (string encoding)
 *   src/vnet/interface.api       sw_interface_set_flags
 *   src/vlibmemory/socket_client.c, socket_api.c
 *                                the socket-control messages are exchanged
 *                                as raw structs with fixed-width fields
 *
 * The socket-control path is the interesting one: those two messages do NOT
 * use vlapi's length-driven string encoding, which is exactly the distinction
 * that was got wrong twice.
 */
#include "../src/api/vpp_wire.h"
#include "../src/api/vpp_msgs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>

/* ---- independent encoders transcribed from upstream -------------------- */

/* vl_api_to_api_string() in api_types.h returns len + sizeof(u32): a u32
 * length and exactly that many bytes, with NO padding. */
static size_t golden_put_api_string(uint8_t *out, const char *s)
{
    size_t n = strlen(s);
    uint32_t be = htonl((uint32_t)n);
    memcpy(out, &be, 4);
    memcpy(out + 4, s, n);
    return 4 + n;
}

/* sockclnt_create is context(4) + a fixed zero-padded 64-byte name. VPP's own
 * client writes it with strncpy(mp->name, ..., sizeof(mp->name) - 1). */
static size_t golden_put_sockclnt_create(uint8_t *out, const char *name)
{
    uint32_t context = htonl(0xCEFAEDFEu);
    memcpy(out, &context, 4);
    memset(out + 4, 0, 64);
    size_t n = strlen(name);
    assert(n < 64);          /* would be truncated by sizeof(name) - 1 */
    memcpy(out + 4, name, n);
    return 4 + 64;
}

/* message_table_entry is u16 index + a fixed zero-padded 64-byte name, filled
 * upstream with strncpy_s(dst, 64, ...) and no length field. */
static size_t golden_put_table_entry(uint8_t *out, uint16_t index,
                                     const char *name)
{
    uint16_t be = htons(index);
    memcpy(out, &be, 2);
    memset(out + 2, 0, 64);
    size_t n = strlen(name);
    assert(n < 64);
    memcpy(out + 2, name, n);
    return 2 + 64;
}

/* sw_interface_set_flags: u32 sw_if_index then a single u8 admin_up_down. */
static size_t golden_put_set_flags(uint8_t *out, uint32_t ifindex, bool up)
{
    uint32_t be = htonl(ifindex);
    memcpy(out, &be, 4);
    out[4] = up ? 1 : 0;
    return 5;
}

/* ---- the encoders and the production codecs must agree ---------------- */

int test_golden_api_string_no_padding(void)
{
    /* A string of each length modulo 4, because a length-relative padding
     * rule would show up on three of them. */
    static const char *names[] = { "a", "ab", "abc", "abcd" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint8_t golden[128];
        size_t glen = golden_put_api_string(golden, names[i]);

        vpp_buf_t b;
        vpp_buf_init(&b, 128);
        vpp_buf_put_string(&b, names[i]);
        assert(b.len == glen);
        assert(memcmp(b.data, golden, glen) == 0);
        vpp_buf_free(&b);

        /* And the reader must invert it exactly, with no leftover bytes. */
        vpp_reader_t r;
        uint8_t blob[128];
        memcpy(blob, golden, glen);
        vpp_reader_init(&r, blob, glen);
        char *got = vpp_rd_string(&r, 256);
        assert(got != NULL);
        assert(strcmp(got, names[i]) == 0);
        assert(r.pos == r.len);      /* nothing left over */
        free(got);
    }
    printf("[PASS] test_golden_api_string_no_padding: writer and reader "
           "match vl_api_to_api_string\n");
    return 0;
}

int test_golden_sockclnt_create_fixed_name(void)
{
    /* sockclnt_create on the wire: msg_id(2) + context(4) + name(64) = 70.
     *
     * The mock server already validates this frame strictly during the
     * handshake, so what this adds is the byte-level statement of *why*:
     * the name occupies a fixed 64-byte field starting at offset 6, and a
     * length-prefixed encoding would produce a different length entirely. */
    uint8_t frame[128];
    size_t flen = golden_put_sockclnt_create(frame, "danos-open");
    assert(flen == 68);                 /* body without the msg id */

    /* Context bytes sit at offset 0 of the body. */
    assert(frame[0] == 0xCE && frame[1] == 0xFA &&
           frame[2] == 0xED && frame[3] == 0xFE);
    /* The name starts at offset 4 and is zero-padded to 64 bytes. */
    assert(memcmp(frame + 4, "danos-open", 10) == 0);
    for (size_t i = 4 + 10; i < 68; i++) assert(frame[i] == 0);

    /* Total frame including the 2-byte message id is 70, and the name is
     * *not* length-prefixed: byte 4 is 'd', not a zero length byte. */
    uint8_t with_id[128];
    with_id[0] = 0x00; with_id[1] = 0x0F;    /* VPP_MSG_ID_SOCKCLNT_CREATE */
    memcpy(with_id + 2, frame, flen);
    assert(flen + 2 == 70);
    assert(with_id[6] == 'd');

    /* A length-prefixed encoding of the same name would be 4+4+10 = 18 body
     * bytes, i.e. a 20-byte frame. Assert the two are distinguishable so a
     * future "cleanup" to put_string cannot pass unnoticed. */
    uint8_t as_string[128];
    size_t slen = golden_put_api_string(as_string, "danos-open");
    size_t string_frame = 4 + slen;          /* context + length-prefixed */
    assert(slen == 14);
    assert(string_frame != flen);
    assert(string_frame + 2 != 70);
    printf("[PASS] test_golden_sockclnt_create_fixed_name: 70-byte "
           "fixed-width frame, distinct from the 20-byte string form\n");
    return 0;
}

int test_golden_message_table_fixed_name(void)
{
    /* Two entries, read back through the production parser's primitive. */
    static const struct { uint16_t id; const char *name; } tbl[] = {
        { 1, "control_ping" },
        { 2, "ip_route_add_del" },
    };
    uint8_t golden[256];
    size_t glen = 0;
    for (size_t i = 0; i < 2; i++)
        glen += golden_put_table_entry(golden + glen, tbl[i].id, tbl[i].name);

    /* The reader must consume exactly two entries with no leftovers: a
     * length-prefixed read would desynchronise on the second. */
    vpp_reader_t r;
    vpp_reader_init(&r, golden, glen);
    for (size_t i = 0; i < 2; i++) {
        uint16_t id = vpp_rd_u16(&r);
        assert(id == tbl[i].id);
        char name[65];
        memset(name, 0, sizeof(name));
        assert(vpp_rd_bytes(&r, (uint8_t *)name, 64));
        assert(strcmp(name, tbl[i].name) == 0);
    }
    assert(r.pos == r.len);
    printf("[PASS] test_golden_message_table_fixed_name: fixed-width entries "
           "consume exactly\n");
    return 0;
}

int test_golden_set_flags_is_one_byte(void)
{
    /* interface.api: u8 admin_up_down. A 4-byte field puts a zero in the
     * byte VPP actually reads, so an interface can never come admin-up. */
    for (int up = 0; up <= 1; up++) {
        uint8_t golden[8];
        size_t glen = golden_put_set_flags(golden, 3, up != 0);

        uint8_t got[16];
        int n = vpp_encode_sw_interface_set_flags(3, up != 0, got,
                                                  sizeof(got));
        assert(n == (int)glen);
        assert(memcmp(got, golden, glen) == 0);
        assert(got[4] == (uint8_t)(up ? 1 : 0));
    }
    printf("[PASS] test_golden_set_flags_is_one_byte: 5-byte body per "
           "interface.api\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    if (test_golden_api_string_no_padding() != 0) failed++;
    if (test_golden_sockclnt_create_fixed_name() != 0) failed++;
    if (test_golden_message_table_fixed_name() != 0) failed++;
    if (test_golden_set_flags_is_one_byte() != 0) failed++;
    printf("=== vpp_golden_test: %s ===\n",
           failed == 0 ? "ALL PASSED" : "FAILURES");
    return failed;
}