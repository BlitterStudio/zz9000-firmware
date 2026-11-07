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
#define TIMER_HZ 333333343ULL /* COUNTS_PER_SECOND on the ZZ9000 */

int main(void)
{
    struct zzusb_root_reset reset;
    enum zzusb_root_reset_action action;
    uint64_t base;

    /* Normal reset. Each phase runs from its arm() call, not from the
     * decision: 300 us of PHY setup before the PR write must not shorten
     * the 50 ms hold, and the 100 us PHY speed switch must not shorten
     * the 10 ms recovery. */
    zzusb_root_reset_init(&reset);
    action = zzusb_root_reset_start(&reset, 1000U, 0U);
    CHECK(action == ZZUSB_ROOT_RESET_ASSERT);
    CHECK(zzusb_root_reset_active(&reset));
    zzusb_root_reset_arm(&reset, 1300U);
    CHECK(zzusb_root_reset_step(&reset, 51299U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, 51300U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_DEASSERT);
    zzusb_root_reset_arm(&reset, 51400U);
    CHECK(zzusb_root_reset_step(&reset, 52000U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    action = zzusb_root_reset_step(&reset, 52100U, 0U);
    CHECK(action == ZZUSB_ROOT_RESET_PORT_READY);
    CHECK(reset.observed_portsc == 0U);
    zzusb_root_reset_arm(&reset, 52200U);
    CHECK(zzusb_root_reset_step(&reset, 62199U, 0U) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, 62200U, 0U) ==
          ZZUSB_ROOT_RESET_COMPLETE);
    CHECK(!zzusb_root_reset_active(&reset));

    /* PR stuck after de-assert: the 4 ms budget runs from the write. */
    zzusb_root_reset_init(&reset);
    CHECK(zzusb_root_reset_start(&reset, 0U, 0U) == ZZUSB_ROOT_RESET_ASSERT);
    zzusb_root_reset_arm(&reset, 0U);
    CHECK(zzusb_root_reset_step(&reset, 50000U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_DEASSERT);
    zzusb_root_reset_arm(&reset, 50200U);
    CHECK(zzusb_root_reset_step(&reset, 54199U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, 54200U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_TIMEOUT);
    CHECK(reset.observed_portsc == EHCI_PS_PR);
    CHECK(!zzusb_root_reset_active(&reset));

    /* Stale PR from an earlier attempt: drop it, then a full hold. */
    zzusb_root_reset_init(&reset);
    action = zzusb_root_reset_start(&reset, 0U, EHCI_PS_PR);
    CHECK(action == ZZUSB_ROOT_RESET_DROP_STALE);
    zzusb_root_reset_arm(&reset, 200U);
    CHECK(zzusb_root_reset_step(&reset, 4100U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, 4150U, 0U) ==
          ZZUSB_ROOT_RESET_ASSERT);
    zzusb_root_reset_arm(&reset, 4300U);
    CHECK(zzusb_root_reset_step(&reset, 54299U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, 54300U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_DEASSERT);

    zzusb_root_reset_init(&reset);
    CHECK(zzusb_root_reset_start(&reset, 0U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_DROP_STALE);
    zzusb_root_reset_arm(&reset, 200U);
    CHECK(zzusb_root_reset_step(&reset, 4200U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_TIMEOUT);
    CHECK(reset.observed_portsc == EHCI_PS_PR);
    CHECK(!zzusb_root_reset_active(&reset));

    /* Elapsed time stays correct across a clock wrap. */
    base = UINT64_MAX - 10000U;
    zzusb_root_reset_init(&reset);
    CHECK(zzusb_root_reset_start(&reset, base, 0U) == ZZUSB_ROOT_RESET_ASSERT);
    zzusb_root_reset_arm(&reset, base);
    CHECK(zzusb_root_reset_step(&reset, base + 49999U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_NONE);
    CHECK(zzusb_root_reset_step(&reset, base + 50000U, EHCI_PS_PR) ==
          ZZUSB_ROOT_RESET_DEASSERT);

    /* The tick conversion stays exact past the 15.37 h point where
     * ticks * 1000000 overflows uint64_t, and keeps advancing. */
    {
        uint64_t hours = 16U;
        uint64_t ticks = hours * 3600U * TIMER_HZ;
        uint64_t us = zzusb_ticks_to_us(ticks, TIMER_HZ);

        CHECK(us == hours * 3600U * 1000000U);
        CHECK(zzusb_ticks_to_us(ticks + TIMER_HZ / 20U, TIMER_HZ) ==
              us + 49999U ||
              zzusb_ticks_to_us(ticks + TIMER_HZ / 20U, TIMER_HZ) ==
              us + 50000U);
        CHECK(zzusb_ticks_to_us(TIMER_HZ - 1U, TIMER_HZ) == 999999U);
    }

    /* SET_ADDRESS recovery is measured from the completed transfer. */
    {
        struct zzusb_set_address_recovery recovery;
        uint64_t recovery_ticks =
            ((uint64_t)ZZUSB_SET_ADDRESS_RECOVERY_US * TIMER_HZ + 999999U) /
            1000000U;

        zzusb_set_address_recovery_init(&recovery);
        zzusb_set_address_recovery_arm(&recovery, 1000U);
        CHECK(zzusb_set_address_recovery_active(&recovery));
        CHECK(!zzusb_set_address_recovery_elapsed(
            &recovery, 1000U + recovery_ticks - 1U, TIMER_HZ));
        CHECK(zzusb_set_address_recovery_elapsed(
            &recovery, 1000U + recovery_ticks, TIMER_HZ));

        /* Tick subtraction, rather than absolute ticks, survives wrap. */
        zzusb_set_address_recovery_arm(
            &recovery, UINT64_MAX - recovery_ticks + 1U);
        CHECK(zzusb_set_address_recovery_elapsed(
            &recovery, 0U, TIMER_HZ));

        zzusb_set_address_recovery_abort(&recovery);
        CHECK(!zzusb_set_address_recovery_active(&recovery));
        CHECK(!zzusb_set_address_recovery_elapsed(
            &recovery, 1000U + recovery_ticks, TIMER_HZ));
    }

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
