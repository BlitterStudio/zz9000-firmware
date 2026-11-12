/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzm_minigl_host.h"
#include "zzm_format.h"
#include <stdio.h>

static int failures;
static int doorbells;
static int polls;
static int reset_polls;
static int transport_releases;
static int work;

static void expect_int(const char *name, int actual, int expected)
{
    if (actual != expected) {
        printf("%s: got %d expected %d\n", name, actual, expected);
        failures++;
    }
}

static void doorbell(void *context) { (void)context; doorbells++; }
static int transport_acquire(void *context, uint32_t *generation)
{
    (void)context;
    *generation = 7;
    return 1;
}
static int transport_release(void *context)
{
    (void)context;
    transport_releases++;
    return 1;
}
static int has_work(void *context) { (void)context; return work; }
static int poll(void *context, uint32_t budget)
{
    (void)context;
    expect_int("bounded_budget", (int)budget, 1);
    polls++;
    work = 0;
    return 1;
}
/* What the module's reset reports: begin's result, then each poll's. */
static int begin_result = ZZM_MINIGL_RESET_QUIESCENT;
static int poll_busy_left;
static int begin_calls;
static int begin_reset(void *context)
{
    (void)context;
    begin_calls++;
    return begin_result;
}
static int reset_poll(void *context)
{
    (void)context;
    reset_polls++;
    if (poll_busy_left > 0) {
        poll_busy_left--;
        return ZZM_MINIGL_RESET_BUSY;
    }
    return ZZM_MINIGL_RESET_QUIESCENT;
}

/* Runs core 1's RESET action until it acknowledges, as the worker loop
 * does; returns the result, or BUSY if it never finished. */
static int run_reset(int *steps)
{
    int result = ZZM_MINIGL_RESET_BUSY;

    for (*steps = 0; *steps < 16 && result == ZZM_MINIGL_RESET_BUSY; ++*steps)
        result = zzm_minigl_reset_step();
    return result;
}
static void abort_reset(void *context) { (void)context; }
static int unload(void *context) { (void)context; return 1; }
static void fini(void *context) { (void)context; }
static int app_bind(void *context, uint32_t surface_id, const void **out_gl)
{
    (void)context;
    (void)surface_id;
    *out_gl = 0;
    return 1;
}
static int app_swap(void *context) { (void)context; return 1; }
static void app_unbind(void *context) { (void)context; }
static void app_reclaim(void *context, uint32_t generation)
{
    (void)context;
    (void)generation;
}

int main(void)
{
    zzm_minigl_worker_ops_t worker = {
        .version = ZZM_MINIGL_WORKER_OPS_V1, .size = sizeof(worker),
        .has_work = has_work, .poll = poll
    };
    zzm_minigl_ops_v1_t ops = {
        .version = ZZM_MINIGL_OPS_V1, .size = sizeof(ops),
        .doorbell = doorbell, .has_work = has_work, .poll = poll,
        .transport_acquire = transport_acquire, .transport_release = transport_release,
        .begin_reset = begin_reset, .reset_poll = reset_poll,
        .abort_reset = abort_reset, .begin_unload = unload, .fini = fini,
        .app_bind = app_bind, .app_swap = app_swap, .app_unbind = app_unbind,
        .app_reclaim = app_reclaim,
        .reset_protocol = ZZM_MINIGL_RESET_PROTOCOL_V2
    };
    zzm_minigl_registration_v1_t registration = {
        .version = ZZM_MINIGL_REGISTRATION_V1, .size = sizeof(registration),
        .ops = &ops, .context = 0, .worker_ops = &worker
    };
    {
        zzm_minigl_ops_v1_t old_ops = ops;

        old_ops.size = offsetof(zzm_minigl_ops_v1_t, reset_protocol);
        expect_int("old_reset_protocol_rejected",
                   zzm_minigl_register(&old_ops, 0, &worker), 0);
    }
    expect_int("register", zzm_minigl_register_ops(ZZM_MODULE_MINIGL,
                                                    &registration,
                                                    sizeof(registration)), 1);
    work = 1;
    zzm_minigl_service_tick(1);
    expect_int("in_api_gates_poll", polls, 0);
    zzm_minigl_service_tick(0);
    expect_int("one_core1_poll", polls, 1);
    {
        uint32_t generation = 0;
        expect_int("transport_acquire", zzm_minigl_transport_acquire(&generation), 1);
        expect_int("transport_generation", (int)generation, 7);
        expect_int("transport_release", zzm_minigl_transport_release(), 1);
        expect_int("transport_release_once", transport_releases, 1);
    }
    {
        int steps;

        expect_int("reset_begin", zzm_minigl_begin_reset(), ZZM_MINIGL_RESET_QUIESCENT);
        expect_int("reset_poll", zzm_minigl_reset_poll(), ZZM_MINIGL_RESET_QUIESCENT);

        /* An abort that fails during reset is acknowledged as failed at
         * once, not retried as BUSY by every worker pass. */
        begin_calls = 0;
        begin_result = ZZM_MINIGL_RESET_FAILED;
        expect_int("failed_abort_acks", run_reset(&steps), ZZM_MINIGL_RESET_FAILED);
        expect_int("failed_abort_steps", steps, 1);
        /* The next request (the second Amiga reset) runs the module again. */
        begin_result = ZZM_MINIGL_RESET_QUIESCENT;
        expect_int("after_failed_reset", run_reset(&steps), ZZM_MINIGL_RESET_QUIESCENT);
        expect_int("after_failed_begins", begin_calls, 2);

        /* A module still draining is polled until it is quiescent. */
        begin_result = ZZM_MINIGL_RESET_BUSY;
        poll_busy_left = 2;
        reset_polls = 0;
        expect_int("draining_reset_completes", run_reset(&steps), ZZM_MINIGL_RESET_QUIESCENT);
        expect_int("draining_reset_steps", steps, 3);
        expect_int("draining_reset_polls", reset_polls, 3);

        /* Kickstart's second reset arrives while the first still drains:
         * that request continues the first instead of restarting it, and a
         * later request starts a fresh one. */
        begin_calls = 0;
        poll_busy_left = 3;
        expect_int("first_reset_in_progress", zzm_minigl_reset_step(), ZZM_MINIGL_RESET_BUSY);
        expect_int("second_request_completes", run_reset(&steps), ZZM_MINIGL_RESET_QUIESCENT);
        expect_int("second_request_no_restart", begin_calls, 1);
        begin_result = ZZM_MINIGL_RESET_QUIESCENT;
        expect_int("third_request_runs", run_reset(&steps), ZZM_MINIGL_RESET_QUIESCENT);
        expect_int("third_request_begins", begin_calls, 2);
    }
    zzm_minigl_on_core1_fault();
    expect_int("fault_unregisters", zzm_minigl_worker_registered(), 0);
    zzm_minigl_boot_caps_init(ZZM_MINIGL_IMAGE_STANDARD);
    expect_int("standard_has_no_ef", (int)zzm_minigl_boot_caps()->has_rasterix_ef, 0);
    zzm_minigl_unregister();
    return failures != 0;
}
