#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "fuzz_mock.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4)
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

    size_t total = size - 3;
    size_t submit_len = (size_t)data[2];
    if (submit_len > total)
        submit_len = total;
    size_t req_len = total - submit_len;

    const uint8_t *submit_data = data + 3;
    const uint8_t *req_data = data + 3 + submit_len;

    if (submit_len >= 16) {
        (void)mw_host_submit(c, submit_data, (uint32_t)submit_len);
    }

    if (req_len >= 4) {
        uint32_t cap = fuzz_size_reply_capacity(req_data, req_len);
        uint8_t *reply = (uint8_t *)malloc(cap);
        if (reply) {
            uint32_t reply_bytes = 0;
            (void)mw_host_request(c, req_data, (uint32_t)req_len,
                                  reply, cap, &reply_bytes);
            free(reply);
        }
    }

    mw_host_destroy(c);
    disable_rasterix_mock();
    minigl_raster_reset();
    return 0;
}
