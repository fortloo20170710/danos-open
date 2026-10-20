/*
 * libFuzzer target: mTLS certificate-fingerprint role map parser.
 *
 * The handshake and certificate verification are OpenSSL's; the DANOS-owned
 * TLS surface is the role map. Exercise the same parser used by the file
 * loader with arbitrary bytes, including embedded NULs and oversized lines.
 */

#include "../src/gnmi/gnmi_tls.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data) return 0;
    (void)danos_gnmi_tls_role_map_valid((const char *)data, size);
    return 0;
}
