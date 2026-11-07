#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "usb/ehci-vbus-cycle.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        return EXIT_FAILURE; \
    } \
} while (0)

int main(void)
{
    /* Only current-connect-status suppresses the boot VBUS cycle. */
    CHECK(zzusb_vbus_cycle_needed(0U));
    CHECK(zzusb_vbus_cycle_needed(0x8c000000U));
    CHECK(zzusb_vbus_cycle_needed(0x88000206U));
    CHECK(!zzusb_vbus_cycle_needed(ZZUSB_PORTSC_CCS));
    CHECK(!zzusb_vbus_cycle_needed(0x88000207U));

    puts("EHCI boot VBUS cycle decision satisfied");
    return EXIT_SUCCESS;
}
