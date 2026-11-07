#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "usb_port_reset_sm.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        return EXIT_FAILURE; \
    } \
} while (0)

#define EHCI_PS_PR (1U << 8)

int main(void)
{
    struct zzusb_root_reset reset;
    enum zzusb_root_reset_action action;

    zzusb_root_reset_init(&reset);
    action = zzusb_root_reset_start(&reset, 1000U, 0U);
    CHECK(action == ZZUSB_ROOT_RESET_ASSERT);
    CHECK(zzusb_root_reset_active(&reset));
    CHECK(zzusb_root_reset_step(&reset, 50999U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    action = zzusb_root_reset_step(&reset, 51000U, EHCI_PS_PR);
    CHECK(action == ZZUSB_ROOT_RESET_DEASSERT);
    CHECK(zzusb_root_reset_step(&reset, 51999U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    action = zzusb_root_reset_step(&reset, 52000U, 0U);
    CHECK(action == ZZUSB_ROOT_RESET_PORT_READY);
    CHECK(reset.observed_portsc == 0U);
    CHECK(zzusb_root_reset_step(&reset, 61999U, 0U) ==
          ZZUSB_ROOT_RESET_NONE);
    action = zzusb_root_reset_step(&reset, 62000U, 0U);
    CHECK(action == ZZUSB_ROOT_RESET_COMPLETE);
    CHECK(!zzusb_root_reset_active(&reset));

    zzusb_root_reset_init(&reset);
    action = zzusb_root_reset_start(&reset, 0U, EHCI_PS_PR);
    CHECK(action == ZZUSB_ROOT_RESET_DROP_STALE);
    CHECK(zzusb_root_reset_step(&reset, 3999U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, 4000U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_TIMEOUT);
    CHECK(reset.observed_portsc == EHCI_PS_PR);
    CHECK(!zzusb_root_reset_active(&reset));

    zzusb_root_reset_init(&reset);
    CHECK(zzusb_root_reset_start(&reset, 0U, 0U) ==
          ZZUSB_ROOT_RESET_ASSERT);
    zzusb_root_reset_abort(&reset);
    CHECK(!zzusb_root_reset_active(&reset));
    CHECK(zzusb_root_reset_step(&reset, 100000U, 0U) ==
          ZZUSB_ROOT_RESET_NONE);

    puts("USB root reset state machine satisfied");
    return EXIT_SUCCESS;
}
