#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "fuzz_mock.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    uint32_t width = 2U + (data[0] % 63U);
    uint32_t height = 2U + (data[1] % 63U);

#if defined(FUZZ_RASTERIX)
    enable_rasterix_mock();
#else
    disable_rasterix_mock();
#endif

    mw_ctx *c = mw_host_create(width, height);
    if (!c) {
        disable_rasterix_mock();
        return 0;
    }

    const uint8_t *p = data + 2;
    size_t rem = size - 2;
    int max_packets = 16;

    while (rem > 0 && max_packets-- > 0) {
        size_t pkt_len = rem;
        if (rem >= 16) {
            uint32_t header_len = mw_be32(p + 8);
            if (header_len >= 16 && header_len < rem) {
                pkt_len = header_len;
            }
        }
        (void)fuzz_submit_fixed(c, p, pkt_len);
        p += pkt_len;
        rem -= pkt_len;
    }

    mw_host_destroy(c);
    disable_rasterix_mock();
    minigl_raster_reset();
    return 0;
}
