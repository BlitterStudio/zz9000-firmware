/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A record freed by the other core while a pass of the system thread visits
 * it, through the real runtime.
 *
 * The worker pass (core 1) and the core 0 poll walk every record index. Each
 * reads the record's state, selects the record as the core's service context
 * and works on it. The other core frees a record without that pass holding
 * any lock: core 0 frees a claim that RELEASE ends, core 1 frees the shell of
 * an app whose release was requested. A pass that read "not free" and then
 * found the record gone must skip it: selecting a freed record clears the
 * context, and a context-less pass used to stop the core on its undefined
 * instruction (a card fault, code 4, in zzapp_worker_hook).
 *
 * The interleaving is placed exactly: zzapp_rec_state and
 * zzapp_rec_next_allocated are wrapped, and the Nth read of the record's
 * state by the visiting core returns the live state
 * it had and then lets the other core free the record, before the pass goes
 * on. N is swept over every read the pass makes. A pass that stops the core
 * aborts the test.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

#include <signal.h>
#include <unistd.h>

zzapp_rec_state_t __real_zzapp_rec_state(uint32_t pid);
uint32_t __real_zzapp_rec_next_allocated(uint32_t from);

/* One injection: the `target`th lock-free read of record `pid`'s state by
 * `core` (counting reads that find the record live) is followed by `action`.
 * A read is a zzapp_rec_state() call or a record that zzapp_rec_next_allocated()
 * hands to a walk. A read under the slot-state lock cannot race a free, which
 * needs the lock: it is not counted, and the action could not take the lock
 * either. */
static struct {
    int armed;
    int busy;
    int fired;
    int core;
    uint32_t pid;
    unsigned target;
    unsigned seen;
    void (*action)(void);
} race;

static void fire_on_live_read(uint32_t pid)
{
    if (race.armed && !race.busy && pid == race.pid &&
        defer_test_core == race.core && !g_slot_state_test_lock &&
        ++race.seen == race.target) {
        race.busy = 1;
        race.armed = 0;
        race.fired = 1;
        race.action();
        race.busy = 0;
    }
}

zzapp_rec_state_t __wrap_zzapp_rec_state(uint32_t pid)
{
    zzapp_rec_state_t state = __real_zzapp_rec_state(pid);

    if (state != ZZAPP_REC_FREE)
        fire_on_live_read(pid);
    return state;
}

uint32_t __wrap_zzapp_rec_next_allocated(uint32_t from)
{
    uint32_t pid = __real_zzapp_rec_next_allocated(from);

    if (pid < zzapp_rec_capacity())
        fire_on_live_read(pid);
    return pid;
}

static void arm(int core, uint32_t pid, unsigned target, void (*action)(void))
{
    memset(&race, 0, sizeof(race));
    race.armed = 1;
    race.core = core;
    race.pid = pid;
    race.target = target;
    race.action = action;
}

static void disarm(void)
{
    memset(&race, 0, sizeof(race));
}

/* The firmware's way to stop a core is an undefined instruction, the
 * harness's is abort(), as a failed assert is: say where the injection was. */
static void stopped_core(int sig)
{
    char msg[96];
    int n = snprintf(msg, sizeof(msg),
                     "record_race_test: aborted on core %d, read %u of pid %u\n",
                     race.core, race.seen, (unsigned)race.pid);

    (void)sig;
    if (n > 0 && write(2, msg, (size_t)n) < 0)
        _exit(2);
    _exit(134);
}

static struct rt_counts g_start;

static void setup(void)
{
    rt_boot(RT_MB(256), 8U);
    g_start = rt_counts();
}

/* Core 0 ends client 0's claim between two steps of a worker pass. */
static void release_client0(void)
{
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
}

/* Reads of the record a worker pass makes: the sweep covers them all, and
 * the pass finishes with the record gone and the card whole. */
static void test_worker_pass_meets_a_claim_released_by_core0(void)
{
    unsigned target, fired = 0U;

    for (target = 1U; target <= 64U; target++) {
        setup();
        rt_claim(0U);
        rt_claim(1U);
        arm(1, rt_pid[0], target, release_client0);
        rt_core1_pass();
        if (!race.fired) {
            /* The pass reads the record fewer times than this. */
            disarm();
            assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
            rt_settle(4U);
            assert(RT_STATE(1) == AAR_STATE_CLAIMED);
            assert(rt_simple(1U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
            rt_assert_back(g_start);
            break;
        }
        fired++;
        disarm();
        /* The neighbour was not disturbed, and the passes that follow are
         * as good as any. */
        assert(RT_STATE(1) == AAR_STATE_CLAIMED);
        rt_settle(4U);
        assert(RT_STATE(1) == AAR_STATE_CLAIMED);
        assert(rt_simple(1U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        rt_settle(4U);
        rt_assert_back(g_start);
    }
    /* The worker reads a live record's state in each of its walks. */
    assert(fired >= 4U);
}

/* Claim and release back to back, a worker pass between every step, the
 * release landing at a different point of the pass each round. The card
 * keeps handing out claims and its counts return to where they started. */
static void test_claim_release_churn_with_worker_passes(void)
{
    unsigned i, landed = 0U;

    setup();
    for (i = 0U; i < 500U; i++) {
        rt_claim(0U);
        arm(1, rt_pid[0], 1U + i % 6U, release_client0);
        rt_core1_pass();
        if (race.fired) {
            landed++;
        } else {
            assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        }
        disarm();
        rt_core1_pass();
        mock_client_serve_all();
        assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    }
    assert(landed >= 250U);
    rt_settle(4U);
    rt_assert_back(g_start);
    /* The worker is still alive: a claim loads and runs. */
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    assert(RT_STATE(0) == AAR_STATE_RUNNING);
}

/* The app of client 0 runs and its client releases it: the stop is latched
 * and the forced stop is pending on core 1. */
static void release_running_app(void)
{
    static uint64_t when = 20000000U;

    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    assert(RT_STATE(0) == AAR_STATE_RUNNING);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    zzapp_kernel_set_current_thread(-1);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_host_tick_at(when);
    zzapp_host_tick_at(when + ZZAPP_STOP_GRACE_US);
    when += 10000000U;
    zzapp_services_test_set_core(0);
}

/* Core 1 retires the app and frees its shell. */
static void core1_retires_the_app(void)
{
    rt_settle(8U);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
}

/* The other direction: the core 0 poll reads the state of a record whose
 * shell core 1 then frees. */
static void test_core0_poll_meets_a_shell_freed_by_core1(void)
{
    unsigned target, fired = 0U;

    for (target = 1U; target <= 64U; target++) {
        setup();
        release_running_app();
        arm(0, rt_pid[0], target, core1_retires_the_app);
        zzapp_core0_poll();
        if (!race.fired) {
            disarm();
            rt_settle(8U);
            rt_assert_back(g_start);
            break;
        }
        fired++;
        disarm();
        rt_settle(4U);
        zzapp_core0_poll();
        rt_assert_back(g_start);
    }
    assert(fired >= 1U);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } tests[] = {
        { "worker", test_worker_pass_meets_a_claim_released_by_core0 },
        { "churn", test_claim_release_churn_with_worker_passes },
        { "poll", test_core0_poll_meets_a_shell_freed_by_core1 },
    };
    unsigned i;

    signal(SIGABRT, stopped_core);
    /* One suite by name, to see each fail on its own. */
    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i++)
        if (argc < 2 || strcmp(argv[1], tests[i].name) == 0)
            tests[i].run();
    puts("record_race_test: ok");
    return 0;
}
