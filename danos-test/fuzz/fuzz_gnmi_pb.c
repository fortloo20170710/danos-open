/*
 * libFuzzer target: hand-written gNMI protobuf decoders.
 *
 * Coverage-guided counterpart to the deterministic mutational harness in
 * fuzz_decoders.c. Build with -DDANOS_LIBFUZZER=ON and clang.
 */

#include "../src/gnmi/gnmi_proto.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return 0;

    gnmi_get_request_t gr;
    gnmi_set_request_t sr;
    gnmi_subscribe_request_t sub;
    gnmi_path_t p;
    gnmi_typed_value_t v;
    memset(&gr, 0, sizeof(gr));
    memset(&sr, 0, sizeof(sr));
    memset(&sub, 0, sizeof(sub));
    memset(&p, 0, sizeof(p));
    memset(&v, 0, sizeof(v));

    (void)gnmi_decode_get_request(data, size, &gr);
    (void)gnmi_decode_set_request(data, size, &sr);
    (void)gnmi_decode_subscribe_request(data, size, &sub);
    (void)gnmi_decode_path(data, size, &p);
    (void)gnmi_decode_typed_value(data, size, &v);
    return 0;
}
