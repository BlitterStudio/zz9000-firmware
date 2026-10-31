/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzm_minigl_host.h"
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
static int begin_reset(void *context) { (void)context; return 1; }
static int reset_poll(void *context)
{
    (void)context;
    reset_polls++;
    return ZZM_MINIGL_RESET_QUIESCENT;
}
static void abort_reset(void *context) { (void)context; }
static int unload(void *context) { (void)context; return 1; }
static void fini(void *context) { (void)context; }

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
        .abort_reset = abort_reset, .begin_unload = unload, .fini = fini
    };

    expect_int("register", zzm_minigl_register(&ops, 0, &worker), 1);
    expect_int("registered", zzm_minigl_worker_registered(), 1);
    zzm_minigl_doorbell();
    expect_int("doorbell_no_parser", doorbells, 1);
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
    expect_int("reset_begin", zzm_minigl_begin_reset(), 1);
    expect_int("reset_poll", zzm_minigl_reset_poll(), ZZM_MINIGL_RESET_QUIESCENT);
    expect_int("reset_called", reset_polls, 1);
    zzm_minigl_on_core1_fault();
    expect_int("fault_unregisters", zzm_minigl_worker_registered(), 0);
    zzm_minigl_boot_caps_init(ZZM_MINIGL_IMAGE_STANDARD);
    expect_int("standard_has_no_ef", (int)zzm_minigl_boot_caps()->has_rasterix_ef, 0);
    zzm_minigl_unregister();
    return failures != 0;
}
