#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "usb_proxy.h"

#ifndef ZZUSB_PROTOCOL_VERSION
#define ZZUSB_PROTOCOL_VERSION 1u
#endif
#ifndef ZZUSB_V2_HEADER_SIZE
#define ZZUSB_V2_HEADER_SIZE ZZUSB_CMD_SIZE
#endif

#define CHECK(expr) do {     if (!(expr)) {         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);         return EXIT_FAILURE;     } } while (0)

static int completion_matches(uint32_t active_id, uint32_t active_epoch,
                              uint32_t completion_id, uint32_t completion_epoch)
{
    return ZZUSB_PROTOCOL_VERSION >= 2u &&
           active_id != 0u &&
           active_id == completion_id &&
           active_epoch == completion_epoch;
}

int main(void)
{
    CHECK(ZZUSB_PROTOCOL_VERSION == 2u);
    CHECK(ZZUSB_V2_HEADER_SIZE == ZZUSB_DATA_OFFSET);

    CHECK(completion_matches(7u, 3u, 7u, 3u));
    CHECK(!completion_matches(7u, 3u, 6u, 3u));
    CHECK(!completion_matches(7u, 3u, 7u, 2u));
    CHECK(!completion_matches(0u, 3u, 0u, 3u));

    puts("USB proxy completion identity contract satisfied");
    return EXIT_SUCCESS;
}
