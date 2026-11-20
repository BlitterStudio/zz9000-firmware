/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp_rec.h"
#include "zzapp_runtime.h"

/* More live records than the old fixed four: core 1 dies with all of them
 * running and every one is torn down on core 0. */
#define LIVE_RECORDS 8U

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

uint32_t zzapp_services_current_pid(void)
{
    return selected_slot;
}

int zzapp_services_select_pid(uint32_t pid)
{
    selected_slot = pid;
    return 1;
}

void __real_zzapp_loader_retire_resources(void);

/* The first resource a retire returns: by then no context of any record may
 * be left to run, or a mapping would be released under a live thread. Every
 * retire is checked, not just one of them. */
void __wrap_zzapp_loader_retire_resources(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();
    uint32_t pid;
    int retired = 1;

    release_calls++;
    for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
        zzapp_thread_t *thread = zzapp_kernel_thread(kernel, pid);

        if (thread->state != ZZAPP_THREAD_STOPPED ||
            thread->context.frame.exception_lr != 0U)
            retired = 0;
    }
    if (retired && zzapp_kernel_current_thread() < 0)
        retired_before_release++;
    __real_zzapp_loader_retire_resources();
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

static void boot_with_records(uint32_t count)
{
    uint32_t pid;

    zzapp_boot_init();
    for (pid = 0U; pid < count; pid++)
        assert(zzapp_test_admit() == pid);
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
    zzapp_rec_stats_t stats;
    uint32_t slot;

    boot_with_records(LIVE_RECORDS);
    zzapp_test_set_xtime(0);
    for (slot = 0U; slot < LIVE_RECORDS; slot++)
        mark_stale(slot);
    zzapp_rec_stats(&stats);
    assert(stats.live == LIVE_RECORDS);
    kernel = zzapp_runtime_kernel();
    zzapp_kernel_set_current_thread(1);
    zzapp_test_set_xtime(ZZAPP_WATCHDOG_US + 1U);
    zzapp_core0_poll();

    assert(zzapp_test_cold_restarts() == 1);
    assert(release_calls == (int)LIVE_RECORDS);
    assert(retired_before_release == release_calls);
    assert(zzapp_kernel_current_thread() < 0);
    for (slot = 0U; slot < LIVE_RECORDS; slot++) {
        zzapp_thread_t *thread = zzapp_kernel_thread(kernel, slot);

        selected_slot = slot;
        /* The fault is what the launcher reads; the record itself is gone. */
        assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
        assert(zzapp_rec_state(slot) == ZZAPP_REC_FREE);
        assert(thread->state == ZZAPP_THREAD_STOPPED);
        assert(thread->context.frame.exception_lr == 0U);
    }
    /* Every record is back: nothing live, the whole queue free again. */
    zzapp_rec_stats(&stats);
    assert(stats.live == 0U && stats.claimed == 0U && stats.retiring == 0U);
    assert(stats.free == stats.capacity);
    zzapp_core0_poll();
    assert(zzapp_test_cold_restarts() == 1);

    /* The single-core fallback calls this helper. An orphaned module heap
     * lock must be free before GL teardown tries to acquire it. */
    boot_with_records(1U);
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
