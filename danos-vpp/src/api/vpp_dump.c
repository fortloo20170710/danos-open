/*
 * DANOS-Open VPP Backend: multipart dump decoders
 *
 * Field order follows the FDio VPP .api definitions:
 *   interface.api   sw_interface_details
 *   ip.api          ip_table_details, ip_route_details
 *   ip_types.api    address/prefix
 *   fib_types.api   fib_path
 * All structs are packed and multi-byte fields are big-endian.
 */

#include "vpp_dump.h"
#include "vpp_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fib_path: 4+4+4+1+1+4+4+4 + address_union(16) + 4+4+4 + 1 +
 *           16 * fib_mpls_label(7) */
#define FIB_PATH_WIRE_SIZE 167u
#define FIB_MPLS_LABEL_SIZE 7u
#define FIB_PATH_MAX_LABELS 16u

/* FIB_API_PATH_TYPE_* */
#define FIB_PATH_TYPE_NORMAL 0u
#define FIB_PATH_TYPE_DROP   2u

static void copy_fixed_string(char *dst, size_t cap, const uint8_t *src,
                              size_t src_len)
{
    size_t n = 0;
    while (n < src_len && n + 1 < cap && src[n] != '\0') {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

int vpp_dump_decode_iface(const uint8_t *body, uint32_t len,
                          vpp_iface_dump_t *out)
{
    if (!body || !out) return -1;
    vpp_reader_t r;
    vpp_reader_init(&r, body, len);
    memset(out, 0, sizeof(*out));

    (void)vpp_rd_u32(&r);                 /* context */
    out->sw_if_index = vpp_rd_u32(&r);
    (void)vpp_rd_u32(&r);                 /* sup_sw_if_index */
    uint8_t mac[6];
    (void)vpp_rd_bytes(&r, mac, sizeof(mac));
    out->flags = vpp_rd_u32(&r);
    (void)vpp_rd_u32(&r);                 /* type */
    (void)vpp_rd_u32(&r);                 /* link_duplex */
    (void)vpp_rd_u32(&r);                 /* link_speed */
    out->mtu = vpp_rd_u16(&r);
    uint32_t mtu4[4];
    (void)vpp_rd_bytes(&r, mtu4, sizeof(mtu4));
    (void)vpp_rd_u32(&r);                 /* sub_id */
    (void)vpp_rd_u8(&r);                  /* sub_number_of_tags */
    (void)vpp_rd_u16(&r);                 /* sub_outer_vlan_id */
    (void)vpp_rd_u16(&r);                 /* sub_inner_vlan_id */
    (void)vpp_rd_u32(&r);                 /* sub_if_flags */
    (void)vpp_rd_u32(&r);                 /* vtr_op */
    (void)vpp_rd_u32(&r);                 /* vtr_push_dot1q */
    (void)vpp_rd_u32(&r);                 /* vtr_tag1 */
    (void)vpp_rd_u32(&r);                 /* vtr_tag2 */
    (void)vpp_rd_u16(&r);                 /* outer_tag */
    uint8_t dmac[6], smac[6];
    (void)vpp_rd_bytes(&r, dmac, sizeof(dmac));
    (void)vpp_rd_bytes(&r, smac, sizeof(smac));
    (void)vpp_rd_u16(&r);                 /* b_vlanid */
    (void)vpp_rd_u32(&r);                 /* i_sid */

    char *name = vpp_rd_string(&r, 63);
    if (!vpp_reader_ok(&r)) {
        free(name);
        return -1;
    }
    if (name) {
        snprintf(out->name, sizeof(out->name), "%s", name);
        free(name);
    }
    return 0;
}

int vpp_dump_decode_table(const uint8_t *body, uint32_t len,
                          vpp_table_dump_t *out)
{
    if (!body || !out) return -1;
    vpp_reader_t r;
    vpp_reader_init(&r, body, len);
    memset(out, 0, sizeof(*out));

    (void)vpp_rd_u32(&r);                 /* context */
    out->table_id = vpp_rd_u32(&r);
    out->is_ip6 = vpp_rd_u8(&r) != 0;
    /* VPP 26.10 ip_table.name is a fixed 64-byte array. */
    uint8_t name[64];
    if (!vpp_rd_bytes(&r, name, sizeof(name)) || !vpp_reader_ok(&r))
        return -1;
    copy_fixed_string(out->name, sizeof(out->name), name, sizeof(name));
    return 0;
}

static int decode_path(vpp_reader_t *r, vpp_path_dump_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sw_if_index = vpp_rd_u32(r);
    (void)vpp_rd_u32(r);                  /* table_id */
    (void)vpp_rd_u32(r);                  /* rpf_id */
    (void)vpp_rd_u8(r);                   /* weight */
    (void)vpp_rd_u8(r);                   /* preference */
    out->type = vpp_rd_u32(r);
    (void)vpp_rd_u32(r);                  /* flags */
    uint32_t proto = vpp_rd_u32(r);
    out->gw_is_ipv6 = proto == 1;         /* FIB_PATH_NH_PROTO_API_IP6 */
    if (!vpp_rd_bytes(r, out->gw, sizeof(out->gw))) return -1;
    (void)vpp_rd_u32(r);                  /* via_label */
    (void)vpp_rd_u32(r);                  /* obj_id */
    (void)vpp_rd_u32(r);                  /* classify_table_index */
    uint8_t n_labels = vpp_rd_u8(r);
    if (n_labels > FIB_PATH_MAX_LABELS) return -1;
    uint8_t labels[FIB_PATH_MAX_LABELS * FIB_MPLS_LABEL_SIZE];
    if (!vpp_rd_bytes(r, labels, sizeof(labels))) return -1;
    return vpp_reader_ok(r) ? 0 : -1;
}

int vpp_dump_decode_route(const uint8_t *body, uint32_t len,
                          vpp_route_dump_t *out)
{
    if (!body || !out) return -1;
    vpp_reader_t r;
    vpp_reader_init(&r, body, len);
    memset(out, 0, sizeof(*out));

    (void)vpp_rd_u32(&r);                 /* context */
    out->table_id = vpp_rd_u32(&r);
    (void)vpp_rd_u32(&r);                 /* stats_index */
    uint8_t af = vpp_rd_u8(&r);
    out->is_ipv6 = af == 1;               /* ADDRESS_IP6 */
    if (!vpp_rd_bytes(&r, out->prefix, sizeof(out->prefix))) return -1;
    out->prefix_len = vpp_rd_u8(&r);
    out->n_paths = vpp_rd_u8(&r);
    if (out->n_paths > VPP_DUMP_MAX_PATHS) return -1;
    for (uint32_t i = 0; i < out->n_paths; i++)
        if (decode_path(&r, &out->paths[i]) != 0) return -1;
    return vpp_reader_ok(&r) ? 0 : -1;
}

/* Keep the compiler honest about the wire size we transcribed. */
_Static_assert(FIB_PATH_WIRE_SIZE == 167u, "fib_path wire size");
