/*
 * Host glue for the suites that link the process-record layer (zzapp_rec.c,
 * zzapp_as.c, mmu_page.c) through zzapp_host.c without owning the platform
 * stubs themselves: a single core's lock primitives and the cache mock's
 * state. Every definition is weak, so a suite that defines its own (to count
 * calls or to model two cores) keeps winning.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>

#include "xil_cache.h"
#include "sdk_smp_lock.h"

__attribute__((weak)) struct xil_cache_mock_state g_xil_cache_mock;

__attribute__((weak)) int smp_cpu_id(void)
{
    return 0;
}

__attribute__((weak)) void smp_raw_spin_lock(volatile uint32_t *word)
{
    *word = 1U;
}

__attribute__((weak)) int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}

__attribute__((weak)) void smp_raw_spin_unlock(volatile uint32_t *word)
{
    *word = 0U;
}
