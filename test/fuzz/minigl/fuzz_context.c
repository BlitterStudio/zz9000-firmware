#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "fuzz_mock.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 5)
        return 0;

    uint32_t w1 = 2U + (data[0] % 63U);
    uint32_t h1 = 2U + (data[1] % 63U);
    uint32_t w2 = 2U + (data[2] % 63U);
    uint32_t h2 = 2U + (data[3] % 63U);

#if defined(FUZZ_RASTERIX)
    enable_rasterix_mock();
#else
    disable_rasterix_mock();
#endif

    mw_ctx *c1 = mw_host_create(w1, h1);
    mw_ctx *c2 = mw_host_create(w2, h2);

    if (!c1 || !c2) {
        if (c1) mw_host_destroy(c1);
        if (c2) mw_host_destroy(c2);
        disable_rasterix_mock();
        return 0;
    }

    const uint8_t *p = data + 4;
    size_t rem = size - 4;
    int max_ops = 32;

    while (rem >= 2 && max_ops-- > 0) {
        uint8_t ctrl = p[0];
        size_t chunk_len = (size_t)p[1];
        p += 2;
        rem -= 2;

        if (chunk_len > rem)
            chunk_len = rem;

        mw_ctx *target_ctx = (ctrl & 1) ? c2 : c1;
        int is_request = (ctrl & 2) != 0;

        if (is_request) {
            if (chunk_len >= 4) {
                uint32_t cap = fuzz_size_reply_capacity(p, chunk_len);
                uint8_t *reply = (uint8_t *)malloc(cap);
                if (reply) {
                    uint32_t reply_bytes = 0;
                    (void)mw_host_request(target_ctx, p, (uint32_t)chunk_len,
                                          reply, cap, &reply_bytes);
                    free(reply);
                }
            }
        } else {
            if (chunk_len >= 16) {
                (void)fuzz_submit_fixed(target_ctx, p, chunk_len);
            }
        }

        p += chunk_len;
        rem -= chunk_len;
    }

    mw_host_destroy(c1);
    mw_host_destroy(c2);
    disable_rasterix_mock();
    minigl_raster_reset();
    return 0;
}
