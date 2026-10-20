/*
 * DANOS-Open Management: NETCONF message framing (RFC 6242)
 *
 * The base 1.0 end-of-message delimiter is `]]>]]>`. Base 1.1 replaces it
 * with chunked framing:
 *
 *   chunked-message = 1*chunk end-of-chunks
 *   chunk           = LF "#" chunk-size LF chunk-data
 *   chunk-size      = 1*HEXDIG
 *   chunk-data      = OCTET{chunk-size}
 *   end-of-chunks   = LF "##" LF
 *
 * The decoder is incremental: callers hand it whatever `read(2)` returned
 * and it invokes a callback once per complete message. Bytes that follow a
 * delimiter are kept for the next message, which is required after the
 * <hello> exchange, where a client may pipeline its first RPC.
 */

#ifndef DANOS_NETCONF_FRAMING_H__
#define DANOS_NETCONF_FRAMING_H__

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds that keep a hostile peer from growing the receive buffer without
 * limit. RFC 6242 imposes none; these are local policy. */
#define NETCONF_MAX_MESSAGE_BYTES (8u * 1024u * 1024u)
#define NETCONF_MAX_CHUNK_BYTES   (4u * 1024u * 1024u)

/* Receives one complete message. `len` excludes the terminating NUL that the
 * decoder appends. Returning non-zero stops the feed with that value. */
typedef int (*netconf_frame_cb_t)(const char *msg, size_t len, void *user);

typedef struct netconf_frame_decoder netconf_frame_decoder_t;

/* base11 selects chunked (true) or end-of-message (false) framing. */
netconf_frame_decoder_t *netconf_frame_decoder_new(bool base11);
void netconf_frame_decoder_free(netconf_frame_decoder_t *dec);

/* Consume `n` bytes. Returns 0 on success, -1 on a framing error or when a
 * callback returned non-zero. */
int netconf_frame_feed(netconf_frame_decoder_t *dec, const char *data,
                       size_t n, netconf_frame_cb_t cb, void *user);

/* Encode one message. Returns a NUL-terminated heap buffer and stores its
 * length in *out_len, or NULL on allocation failure/oversized input. */
char *netconf_frame_encode(bool base11, const char *msg, size_t len,
                           size_t *out_len);

/* True when a peer <hello> advertises NETCONF base 1.1. */
bool netconf_hello_advertises_base11(const char *hello);

#ifdef __cplusplus
}
#endif

#endif /* DANOS_NETCONF_FRAMING_H__ */
