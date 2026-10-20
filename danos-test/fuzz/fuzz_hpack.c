/*
 * libFuzzer target: HPACK header-block decoder, including Huffman.
 */

#include "../src/gnmi/hpack.h"
#include "../src/gnmi/hpack_huffman.h"
#include <stddef.h>
#include <stdint.h>

static bool sink(const char *name, const char *value, void *user)
{
    (void)user; (void)name; (void)value;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return 0;
    hpack_dyn_table_t dyn;
    hpack_dyn_init(&dyn);
    (void)hpack_decode(&dyn, data, size, sink, NULL);
    hpack_dyn_free(&dyn);
    return 0;
}
