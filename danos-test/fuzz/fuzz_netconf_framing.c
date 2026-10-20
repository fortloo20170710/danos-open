/*
 * Coverage-guided fuzz target for the NETCONF RFC 6242 framing decoder.
 *
 * The decoder is fed attacker-controlled bytes from the SSH channel. It must
 * reject malformed chunks and unfinished messages without over-reading,
 * overflowing the message bound or leaking the assembled buffer.
 */

#include "netconf/netconf_framing.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static int sink(const char *msg, size_t len, void *user)
{
    (void)msg;
    (void)len;
    (void)user;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    for (int mode = 0; mode < 2; mode++) {
        netconf_frame_decoder_t *d = netconf_frame_decoder_new(mode == 1);
        if (!d) return 0;
        (void)netconf_frame_feed(d, (const char *)data, size, sink, NULL);
        /* Feed the same bytes again: exercises the carry-over path after a
         * complete message and any partially parsed state. */
        (void)netconf_frame_feed(d, (const char *)data, size, sink, NULL);
        netconf_frame_decoder_free(d);
    }

    size_t out_len = 0;
    char *out = netconf_frame_encode(true, (const char *)data, size, &out_len);
    free(out);
    return 0;
}
