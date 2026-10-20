/*
 * libFuzzer target: HTTP/2 frame header validation.
 */

#include "../src/gnmi/gnmi_grpc.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size < 9) return 0;
    danos_h2_frame_header_t out;
    (void)danos_h2_parse_frame_header(data, &out);
    return 0;
}
