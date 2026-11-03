/*
 * DANOS-Open VPP Backend: multipart dump decoders
 *
 * VPP detail messages carry the request context as their first field and use
 * packed, big-endian fields.  These decoders are pure functions over a detail
 * message body so the wire layout can be tested without a live VPP.
 */

#ifndef DANOS_VPP_DUMP_H__
#define DANOS_VPP_DUMP_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VPP_DUMP_MAX_PATHS 64

typedef struct {
    uint32_t sw_if_index;
    uint32_t flags;                 /* IF_STATUS_API_FLAG_* */
    uint16_t mtu;
    char     name[64];
} vpp_iface_dump_t;

typedef struct {
    uint32_t table_id;
    bool     is_ip6;
    char     name[64];
} vpp_table_dump_t;

typedef struct {
    uint8_t  gw[16];
    bool     gw_is_ipv6;
    uint32_t sw_if_index;
    uint32_t type;                  /* FIB_API_PATH_TYPE_* */
} vpp_path_dump_t;

typedef struct {
    uint32_t table_id;
    bool     is_ipv6;
    uint8_t  prefix[16];
    uint8_t  prefix_len;
    uint8_t  n_paths;
    vpp_path_dump_t paths[VPP_DUMP_MAX_PATHS];
} vpp_route_dump_t;

/* Each decoder expects the struct body starting at the context field. */
int vpp_dump_decode_iface(const uint8_t *body, uint32_t len,
                          vpp_iface_dump_t *out);
int vpp_dump_decode_table(const uint8_t *body, uint32_t len,
                          vpp_table_dump_t *out);
int vpp_dump_decode_route(const uint8_t *body, uint32_t len,
                          vpp_route_dump_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_VPP_DUMP_H__ */
