/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The CPU share weight a client writes into the control block before
 * LOAD_BEGIN (AAR_CB_WEIGHT): 0 selects 1, 1..ZZAPP_WEIGHT_MAX is taken as
 * written, a larger value is refused with BAD_REQUEST and changes nothing, and
 * the weight reaches the scheduler's thread when the app starts and ends with
 * the claim.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

#define MIB (1024U * 1024U)

static struct rt_counts g_start;

static void claimed(void)
{
    rt_boot(RT_MB(256), 8U);
    g_start = rt_counts();
    rt_claim(0U);
}

static uint16_t load_begin(uint32_t weight)
{
    mock_client_set_limits(&rt_cl[0], 0U, 0U, weight);
    return mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL);
}

static uint32_t thread_weight(void)
{
    return zzapp_kernel_thread(zzapp_runtime_kernel(), rt_pid[0])->weight;
}

static void finish(void)
{
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(4U);
    rt_assert_back(g_start);
    assert(zzapp_rec_by_handle(rt_cl[0].handle) == NULL);
}

/* The same for an app that was started: the client stops it, core 1 ends it
 * and the shell is released. */
static void stop_and_finish(void)
{
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_services_test_set_core(1);
    assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_OK);
    zzapp_test_set_xtime(1);
    zzapp_worker_hook();
    zzapp_test_set_xtime(10000000);
    zzapp_worker_hook();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    finish();
}

/* Claim, load with `weight` in the control block, start: the claim holds the
 * weight and so does the thread the app runs on. */
static void run_with(uint32_t weight, uint32_t expected)
{
    claimed();
    assert(rt_rec(0U)->claim.weight == 1U);
    mock_client_set_limits(&rt_cl[0], 0U, 0U, weight);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_rec(0U)->claim.weight == expected);
    /* The firmware never writes the client's word. */
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_WEIGHT) == weight);
    assert(rt_start(0U) == SDK_STATUS_OK);
    assert(zzapp_kernel_thread(zzapp_runtime_kernel(), rt_pid[0])->state ==
           ZZAPP_THREAD_RUNNABLE);
    assert(thread_weight() == expected);
    stop_and_finish();
}

static void test_zero_selects_one(void)
{
    run_with(0U, 1U);
}

static void test_values_are_taken_as_written(void)
{
    run_with(1U, 1U);
    run_with(3U, 3U);
    run_with(ZZAPP_WEIGHT_MAX - 1U, ZZAPP_WEIGHT_MAX - 1U);
    run_with(ZZAPP_WEIGHT_MAX, ZZAPP_WEIGHT_MAX);
}

/* 257 and anything above: BAD_REQUEST, the claim as it was, and the next
 * LOAD_BEGIN with a good weight goes through. */
static void test_above_the_bound_is_refused(void)
{
    static const uint32_t bad[] = { ZZAPP_WEIGHT_MAX + 1U, 4095U, 0x80000000U,
                                    UINT32_MAX };
    unsigned i;

    claimed();
    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* The limits written with it do not take effect either. */
        mock_client_set_limits(&rt_cl[0], 32U, 24U, bad[i]);
        assert(mock_client_load_begin(&rt_cl[0], ZZAPP_CLIENT_CLUT8, NULL,
                                      NULL) == SDK_STATUS_BAD_REQUEST);
        assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
        assert(rt_rec(0U)->claim.weight == 1U);
        assert(rt_rec(0U)->claim.memory_limit == 0U);
        assert(rt_rec(0U)->claim.host_visible_limit == 0U);
        assert(rt_rec(0U)->claim.client_caps == 0U);
    }
    assert(load_begin(ZZAPP_WEIGHT_MAX) == SDK_STATUS_OK);
    assert(rt_rec(0U)->claim.weight == ZZAPP_WEIGHT_MAX);
    finish();
}

/* A refused LOAD_BEGIN after an accepted one leaves the accepted weight. */
static void test_refusal_keeps_the_accepted_weight(void)
{
    claimed();
    assert(load_begin(5U) == SDK_STATUS_OK);
    assert(rt_rec(0U)->claim.weight == 5U);
    assert(load_begin(ZZAPP_WEIGHT_MAX + 1U) == SDK_STATUS_BAD_REQUEST);
    assert(rt_rec(0U)->claim.weight == 5U);
    finish();
}

/* The weight lasts for the claim: the next one starts from 1 again. */
static void test_weight_does_not_outlive_the_claim(void)
{
    claimed();
    assert(load_begin(9U) == SDK_STATUS_OK);
    assert(rt_rec(0U)->claim.weight == 9U);
    finish();
    rt_claim(0U);
    assert(rt_rec(0U)->claim.weight == 1U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_rec(0U)->claim.weight == 1U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    assert(thread_weight() == 1U);
    stop_and_finish();
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        { "zero", test_zero_selects_one },
        { "written", test_values_are_taken_as_written },
        { "refuse", test_above_the_bound_is_refused },
        { "keep", test_refusal_keeps_the_accepted_weight },
        { "claim", test_weight_does_not_outlive_the_claim },
    };
    unsigned i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc < 2 || strcmp(argv[1], cases[i].name) == 0)
            cases[i].run();
    }
    puts("load_weight_test: ok");
    return 0;
}
