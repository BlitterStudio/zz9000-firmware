#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "usb/ehci_lifecycle.h"

#define CHECK(expr) do {     if (!(expr)) {         fprintf(stderr, "%s:%d: check failed: %s\n",                 __FILE__, __LINE__, #expr);         return EXIT_FAILURE;     } } while (0)

static int descriptors_are_freed(int unlink_acknowledged,
                                 int schedule_stopped,
                                 int controller_reset)
{
    return ehci_dma_reclaimable(unlink_acknowledged, schedule_stopped,
                                controller_reset);
}

int main(void)
{
    uint32_t start = UINT32_MAX - 7u;

    CHECK(!descriptors_are_freed(0, 0, 0));
    CHECK(descriptors_are_freed(1, 0, 0));
    CHECK(descriptors_are_freed(0, 1, 0));
    CHECK(descriptors_are_freed(0, 0, 1));

    CHECK(!ehci_deadline_expired_u32(start + 4u, start, 20u));
    CHECK(!ehci_deadline_expired_u32(3u, start, 20u));
    CHECK(ehci_deadline_expired_u32(12u, start, 20u));
    CHECK(ehci_deadline_expired_u32(200u, start, 20u));

    puts("EHCI lifetime and wrap-safe deadline contract satisfied");
    return EXIT_SUCCESS;
}
