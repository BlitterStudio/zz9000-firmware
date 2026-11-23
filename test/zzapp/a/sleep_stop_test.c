/*
 * aar_sleep_us and a stop request (spec 3.7: sleep is one of the calls that
 * return on stop): a stop requested during a long sleep wakes it on the next
 * system pass, well inside the stop grace, and should_stop reads true when
 * the call returns; a sleep begun while a stop is latched returns at once; a
 * pass with no stop leaves a sleeper parked. The suite drives the production
 * kernel, host runtime and system pass over a microsecond host clock.
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

/* The core stops running the app (it parked) and the kernel looks at the
 * clock, as the timer tick does. */
static void kernel_tick(uint64_t now)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();

    zzapp_test_set_xtime(now);
    zzapp_kernel_idle_begin(kernel);
    zzapp_kernel_idle_end(kernel);
}

/* AARStop from the client: the runtime latches the stop on the record. */
static void test_stop_wakes_sleep(void)
{
    const uint64_t start = 1000000ull;
    uint64_t t;

    start_running_app(start);
    CHECK(zzapp_api_table()->should_stop() == 0);
    zzapp_api_table()->sleep_us(10u * 1000000u);
    CHECK(THREAD0.state == ZZAPP_THREAD_SLEEPING);

    /* No stop: passes of the system thread leave the sleeper parked. */
    zzapp_kernel_set_current_thread(-1);
    for (t = start + 1000u; t < start + 50000u; t += 5000u) {
        zzapp_test_set_xtime(t);
        zzapp_worker_hook();
        kernel_tick(t);
        CHECK(THREAD0.state == ZZAPP_THREAD_SLEEPING);
    }
    CHECK(zzapp_runtime_state() == ZZAPP_ST_RUNNING);

    /* The client asks the app to stop. The next pass wakes the sleeper. */
    t = start + 60000u;
    zzapp_host_latch_stop(0U);
    zzapp_test_set_xtime(t);
    zzapp_worker_hook();
    CHECK(THREAD0.state == ZZAPP_THREAD_RUNNABLE);
    CHECK(THREAD0.wake_reason == ZZAPP_WAKE_CANCELLED);

    /* The sleep returned; the app sees the stop and is not forced out. */
    zzapp_kernel_set_current_thread(0);
    CHECK(zzapp_api_table()->should_stop() == 1);
    CHECK(t - start < ZZAPP_STOP_GRACE_US);
    CHECK(zzapp_runtime_state() == ZZAPP_ST_RUNNING);

    /* The app, still polling, sleeps again: it returns at once. */
    zzapp_api_table()->sleep_us(10u * 1000000u);
    CHECK(THREAD0.state == ZZAPP_THREAD_RUNNABLE);
}

static void test_stop_before_sleep_returns_at_once(void)
{
    start_running_app(5000000ull);
    zzapp_host_latch_stop(0U);
    zzapp_api_table()->sleep_us(10u * 1000000u);
    CHECK(THREAD0.state == ZZAPP_THREAD_RUNNABLE);
    CHECK(zzapp_api_table()->should_stop() == 1);
}

int main(void)
{
    test_stop_wakes_sleep();
    test_stop_before_sleep_returns_at_once();
    printf("sleep_stop_test: %u checks, %u failures\n", checks, failures);
    return failures != 0U;
}
