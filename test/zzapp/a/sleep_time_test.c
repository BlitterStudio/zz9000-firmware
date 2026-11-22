/*
 * aar_sleep_us (spec 7.3): the call parks for at least the requested
 * microseconds of the clock aar_time_us reads. The kernel steps its
 * millisecond clock from that microsecond clock; the suite drives the
 * production kernel and host runtime over a microsecond host clock and checks
 * the observable contract:
 *
 *  - a sleeper never runs before start + us, whatever the phase within the
 *    millisecond at which it began and whatever the duration (around the
 *    5000 us scheduler granule included), and it runs at the deadline, not
 *    at the next millisecond boundary: the overshoot is bounded by the
 *    interval between two looks at the clock and reaches none over the
 *    phases (a coarse tick sees it within one tick);
 *  - a sleep without a stop request lasts at least its time, whatever the
 *    system passes in between do.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"
#include "zzapp_rec.h"
#include "zzapp_kernel.h"

#define THREAD0 (*zzapp_kernel_thread(zzapp_runtime_kernel(), 0U))

static unsigned checks;
static unsigned failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* An app running since `t0`: the first (and only) slot, on the core. */
static void start_running_app(uint64_t t0)
{
    zzapp_boot_init();
    zzapp_services_test_reset();
    assert(zzapp_test_admit() == 0U);
    zzapp_test_set_xtime(t0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
}

/* The ways the kernel looks at the clock while the app sleeps. */
enum looker {
    LOOK_IDLE_TICK,     /* idle_begin/idle_end around a WFE, or a switch */
    LOOK_SYSTEM_POLL    /* the system thread's loop: zzapp_kernel_sleep_poll */
};

/* The core stops running the app (it parked) and the kernel looks at the
 * clock, as the timer tick or the system loop does. */
static void kernel_look(enum looker how, uint64_t now)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();

    zzapp_test_set_xtime(now);
    if (how == LOOK_SYSTEM_POLL) {
        (void)zzapp_kernel_sleep_poll(kernel, ZZAPP_TICK_US);
        return;
    }
    zzapp_kernel_idle_begin(kernel);
    zzapp_kernel_idle_end(kernel);
}

static void kernel_tick(uint64_t now)
{
    kernel_look(LOOK_IDLE_TICK, now);
}

/* The microsecond at which a sleeper of `us`, begun at `start`, runs again,
 * looking at the kernel every `step` microseconds; 0 if it never does. */
static uint64_t wake_time(enum looker how, uint64_t start, uint32_t us,
                          uint32_t step)
{
    uint64_t t;
    uint64_t limit = start + (uint64_t)us + 3U * ZZAPP_TICK_US;

    zzapp_kernel_set_current_thread(-1);
    for (t = start; t <= limit; t += step) {
        kernel_look(how, t);
        if (THREAD0.state == ZZAPP_THREAD_RUNNABLE)
            return t;
    }
    return 0U;
}

/* Looks every 10 us (the finest the model sees) or every scheduler tick. */
static const struct {
    enum looker how;
    uint32_t step;
} lookers[] = {
    { LOOK_IDLE_TICK, 10u },
    { LOOK_SYSTEM_POLL, 10u },
    { LOOK_IDLE_TICK, ZZAPP_TICK_US },
};

static void test_never_wakes_early(void)
{
    static const uint32_t durations[] = { 1000u, 4999u, 5000u, 5001u,
                                          10000u, 50000u };
    /* Where in the millisecond the app calls sleep, and which millisecond. */
    static const uint32_t phases[] = { 0u, 1u, 113u, 500u, 887u, 998u, 999u };
    size_t d;
    size_t p;
    size_t l;
    unsigned run;
    uint32_t min_over[sizeof(lookers) / sizeof(lookers[0])];

    for (l = 0U; l < sizeof(lookers) / sizeof(lookers[0]); l++)
        min_over[l] = UINT32_MAX;
    for (d = 0U; d < sizeof(durations) / sizeof(durations[0]); d++) {
        for (p = 0U; p < sizeof(phases) / sizeof(phases[0]); p++) {
            for (run = 0U; run < 3U; run++) {
                for (l = 0U; l < sizeof(lookers) / sizeof(lookers[0]); l++) {
                    uint64_t t0 = 7000000ull + 1000ull * run * 137u;
                    uint64_t start = t0 + 2000u + phases[p] + 1000u * run;
                    uint64_t woke;
                    uint32_t over;

                    start_running_app(t0);
                    zzapp_test_set_xtime(start);
                    zzapp_api_table()->sleep_us(durations[d]);
                    CHECK(THREAD0.state == ZZAPP_THREAD_SLEEPING);
                    /* The system loop keeps looking (no WFE) when the
                     * deadline is within a tick, and idles beyond. */
                    CHECK((zzapp_kernel_sleep_poll(zzapp_runtime_kernel(),
                                                   ZZAPP_TICK_US) != 0) ==
                          (durations[d] <= ZZAPP_TICK_US));
                    woke = wake_time(lookers[l].how, start, durations[d],
                                     lookers[l].step);
                    if (woke == 0U) {
                        printf("never woke: us=%u phase=%u\n",
                               (unsigned)durations[d], (unsigned)phases[p]);
                        CHECK(woke != 0U);
                        continue;
                    }
                    if (woke < start + durations[d])
                        printf("EARLY: us=%u phase=%u slept=%u\n",
                               (unsigned)durations[d], (unsigned)phases[p],
                               (unsigned)(woke - start));
                    CHECK(woke >= start + durations[d]);
                    over = (uint32_t)(woke - (start + durations[d]));
                    /* Not late by more than the interval between two looks
                     * at the clock: the sleep ends at the deadline, not at
                     * the next millisecond boundary. */
                    CHECK(over < lookers[l].step + 1u);
                    if (over < min_over[l])
                        min_over[l] = over;
                }
            }
        }
    }
    /* Over the phases the overshoot reaches (close to) none. */
    CHECK(min_over[0] == 0u);
    CHECK(min_over[1] == 0u);
}

static void test_sleep_without_stop_lasts_its_time(void)
{
    const uint64_t start = 2000000ull + 321u;
    const uint32_t us = 250000u;
    uint64_t woke;
    uint64_t t;

    start_running_app(2000000ull);
    zzapp_test_set_xtime(start);
    zzapp_api_table()->sleep_us(us);
    CHECK(THREAD0.state == ZZAPP_THREAD_SLEEPING);
    /* System passes along the way (no stop) do not end it. */
    zzapp_kernel_set_current_thread(-1);
    for (t = start; t < start + us; t += 5000u) {
        zzapp_test_set_xtime(t);
        zzapp_worker_hook();
        kernel_tick(t);
        if (t < start + us - 1000u)
            CHECK(THREAD0.state == ZZAPP_THREAD_SLEEPING);
    }
    woke = wake_time(LOOK_IDLE_TICK, start + us - 1000u, 1000u, 10u);
    CHECK(woke >= start + us);
    CHECK(zzapp_api_table()->should_stop() == 0);
}

int main(void)
{
    test_never_wakes_early();
    test_sleep_without_stop_lasts_its_time();
    printf("sleep_time_test: %u checks, %u failures\n", checks, failures);
    return failures != 0U;
}
