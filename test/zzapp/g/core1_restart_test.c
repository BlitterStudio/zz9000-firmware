/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp_runtime.h"

static uint32_t selected_slot;
static int retired_before_release;
static int release_calls;
static int heap_orphaned;
static int heap_reclaim_ran;
static int check_heap;

void zzapp_recover_external_locks(void)
{
    heap_orphaned = 0;
}

uint32_t zzapp_services_current_slot(void)
{
    return selected_slot;
}

void zzapp_services_select_slot(uint32_t slot)
{
    selected_slot = slot;
}

void __real_zzapp_loader_release(void);

void __wrap_zzapp_loader_release(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();
    uint32_t slot;
    int retired = 1;

    release_calls++;
    for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++) {
        if (kernel->threads[slot].state != ZZAPP_THREAD_STOPPED ||
            kernel->threads[slot].context.r[15] != 0U)
            retired = 0;
    }
    if (retired && zzapp_kernel_current_thread() < 0)
        retired_before_release = 1;
    __real_zzapp_loader_release();
}

void __real_zzapp_minigl_reclaim(uint32_t generation);

void __wrap_zzapp_minigl_reclaim(uint32_t generation)
{
    if (check_heap) {
        assert(!heap_orphaned);
        heap_reclaim_ran = 1;
        return;
    }
    __real_zzapp_minigl_reclaim(generation);
}

static void mark_stale(uint32_t slot)
{
    selected_slot = slot;
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    assert(zzapp_kernel_prepare_app(zzapp_runtime_kernel(), slot,
                                    0x1000u + slot, 1u, 0u, 0u,
                                    0x8000u) == 0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), slot) == 0);
}

int main(void)
{
    zzapp_kernel_t *kernel;
    uint32_t slot;

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    for (slot = 0U; slot < 3U; slot++)
        mark_stale(slot);
    kernel = zzapp_runtime_kernel();
    zzapp_kernel_set_current_thread(1);
    zzapp_test_set_xtime(ZZAPP_WATCHDOG_US + 1U);
    zzapp_core0_poll();

    assert(zzapp_test_cold_restarts() == 1);
    assert(retired_before_release);
    assert(release_calls == 3);
    assert(zzapp_kernel_current_thread() < 0);
    for (slot = 0U; slot < 3U; slot++) {
        selected_slot = slot;
        assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
        assert(kernel->threads[slot].state == ZZAPP_THREAD_STOPPED);
        assert(kernel->threads[slot].context.r[15] == 0U);
    }
    zzapp_core0_poll();
    assert(zzapp_test_cold_restarts() == 1);

    /* The single-core fallback calls this helper. An orphaned module heap
     * lock must be free before GL teardown tries to acquire it. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    mark_stale(0);
    heap_orphaned = 1;
    check_heap = 1;
    zzapp_recover_halted_core1();
    assert(heap_reclaim_ran);
    assert(!heap_orphaned);
    printf("core1_restart_test: ok\n");
    return 0;
}
