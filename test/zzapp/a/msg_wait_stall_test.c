/*
 * An app parked in zz_msg_wait is inside an API call that waits on the
 * launcher, so the 30 s stall and 10 s stuck-call watchdogs leave it alone.
 * The same silence without a call stops the slot, so the tick below is able
 * to detect a stall.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"

#define SECOND_US 1000000ull

static uint8_t window[ZZAPP_WINDOW_BYTES];
static uint64_t clock_us;
static unsigned clock_reads;
static unsigned ticks_while_parked;
static int stalled_while_parked;
static int wait_registered_at_tick;

static uint64_t wait_clock(void)
{
    clock_reads++;
    if (clock_reads == 3u) {
        /* The app has been parked for 31 s, past both watchdog limits. */
        clock_us = 31u * SECOND_US;
        zzapp_test_set_xtime(clock_us);
        zzapp_host_tick_at(clock_us);
        ticks_while_parked++;
        wait_registered_at_tick = zzapp_services_wait_registered(0U);
        if (zzapp_runtime_kernel()->threads[0].state ==
            ZZAPP_THREAD_STOPPED || zzapp_runtime_state() != ZZAPP_ST_RUNNING)
            stalled_while_parked = 1;
    } else if (clock_reads >= 4u) {
        clock_us = 41u * SECOND_US;
        zzapp_test_set_xtime(clock_us);
    }
    return clock_us;
}

static void start_running_app(void)
{
    zzapp_boot_init();
    zzapp_services_test_reset();
    memset(window, 0, sizeof(window));
    zzapp_services_test_set_window(window, sizeof(window));
    zzapp_test_set_xtime(0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
}

int main(void)
{
    int rc;

    /* Control: 31 s of silence outside any API call is a stall. */
    start_running_app();
    zzapp_kernel_set_current_thread(-1);
    zzapp_host_tick_at(31u * SECOND_US);
    assert(zzapp_runtime_kernel()->threads[0].state == ZZAPP_THREAD_STOPPED);

    /* Parked in zz_msg_wait for 31 s of a 40 s timeout. */
    start_running_app();
    clock_us = 0u;
    clock_reads = 0u;
    zzapp_services_test_set_time(wait_clock);
    rc = zzapp_api_table()->msg_wait(40u * (uint32_t)SECOND_US);
    zzapp_services_test_set_time(NULL);
    printf("msg_wait rc=%d ticks_while_parked=%u registered=%d stalled=%d\n",
           rc, ticks_while_parked, wait_registered_at_tick,
           stalled_while_parked);
    assert(rc == ZZ_ETIMEDOUT);
    assert(ticks_while_parked == 1u);
    assert(wait_registered_at_tick == 1);
    assert(!stalled_while_parked);
    assert(!zzapp_services_wait_registered(0U));
    return 0;
}
