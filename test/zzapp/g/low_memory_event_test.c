/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The low-memory event: when free memory crosses the low-water mark every
 * running app gets one AAR_EV_LOW_MEMORY through aar_event_poll, and a claim
 * that is only claimed, or an app that ended, gets none. One event per
 * crossing, however often the app polls.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "card_pool.h"
#include "host_card_pool.h"
#include "mock_client.h"
#include "zzapp_mem.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"
#define MB(n) ((uint32_t)(n) << 20)

void zzapp_host_register_low_memory_callback(zzapp_mem_t *memory);

int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0u; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1u; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1u;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0u; }

static zzapp_mem_t memory;
static zzapp_mem_proc_t proc;

/* A claimed record the app of which runs (`running`) or does not. Returns its
 * record index. */
static uint32_t claim(int running)
{
    uint32_t pid = zzapp_test_admit();

    assert(pid != ZZAPP_PID_NONE);
    if (running)
        zzapp_test_mark_running();
    return pid;
}

static void expect_low_memory_event(uint32_t pid)
{
    aar_event event[2];
    uint32_t count;

    zzapp_services_select_pid(pid);
    assert(zzapp_svc_event_poll(event, 2u, &count) == AAR_OK);
    assert(count == 1u);
    assert(event[0].type == AAR_EV_LOW_MEMORY);
    assert(zzapp_svc_event_poll(event, 2u, &count) == AAR_OK);
    assert(count == 0u);
}

static void expect_no_low_memory_event(uint32_t pid)
{
    aar_event event;
    uint32_t count;

    zzapp_services_select_pid(pid);
    assert(zzapp_svc_event_poll(&event, 1u, &count) == AAR_OK);
    assert(count == 0u);
}

int main(void)
{
    uint32_t frames[128];
    uint32_t i;
    uint32_t pid[3];
    uint32_t again;

    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    {
        zzapp_xport_env_t env = mock_heap_env();

        zzapp_xport_init(&env);
    }
    /* Apps 0 and 2 run; claim 1 is only claimed, so it gets no event. */
    pid[0] = claim(1);
    pid[1] = claim(0);
    pid[2] = claim(1);
    host_card_pool_reset(MB(256));
    assert(zzapp_mem_init(&memory, &card_pool) == 0);
    zzapp_mem_proc_init(&proc, MB(128));
    zzapp_host_register_low_memory_callback(&memory);

    assert(zzapp_mem_alloc(&memory, &proc, 97u, frames) == 0);
    /* App 0 ends (its resources go, its shell stays for its client) and a
     * new claim takes its place. */
    zzapp_services_select_pid(pid[0]);
    zzapp_services_on_app_end();
    (void)zzapp_rec_retire(pid[0]);
    again = claim(0);
    expect_no_low_memory_event(pid[1]);
    expect_no_low_memory_event(again);
    expect_low_memory_event(pid[2]);
    zzapp_services_select_pid(again);
    zzapp_test_mark_running();

    /* Already below the mark: no new crossing, no event. */
    assert(zzapp_mem_alloc(&memory, &proc, 1u, &frames[97]) == 0);
    expect_no_low_memory_event(again);
    expect_no_low_memory_event(pid[2]);
    for (i = 0u; i < 35u; i++)
        assert(zzapp_mem_release(&memory, &proc, frames[i]) == 0);
    /* Above the mark again, then below it: a second crossing reaches both
     * running apps, and nothing else. */
    assert(zzapp_mem_alloc(&memory, &proc, 34u, frames) == 0);
    expect_low_memory_event(again);
    expect_low_memory_event(pid[2]);
    expect_no_low_memory_event(pid[1]);
    return 0;
}
