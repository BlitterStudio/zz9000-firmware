/*
 * Unit tests for the Wire-v9 transport guard rails: a freshly bound window
 * is quiescent, and garbage control memory can neither wedge the worker
 * inside mw_service_poll() nor flood the reject log.
 *
 * These reproduce the A4000 boot failure: the transport window is a raw
 * host-visible heap allocation (all ones at power-on), and the worker read
 * producer_seq/op_seq 0xffffffff from it, drained a phantom backlog with
 * one unthrottled reject print per packet, flooded the UART, starved
 * core 0's console, and the Amiga never booted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "mw_types.h"
#include "mw_service.h"

static uint8_t g_window[MW_WINDOW_SIZE_Z3];

static volatile ZZ9KMiniGLControlBlockV9 *bind_all_ones(void)
{
    memset(g_window, 0xff, sizeof(g_window));
    assert(mw_service_bind_region(g_window, sizeof(g_window)) == 1);
    return (volatile ZZ9KMiniGLControlBlockV9 *)(void *)g_window;
}

/* The host-owned control fields are declared const (firmware never writes
 * them); tests play the 68k host, so write through an explicit cast. */
static void set_host_u32(const volatile uint32_t *field, uint32_t value)
{
    *(volatile uint32_t *)(void *)field = value;
}
static void test_bind_zeroes_control_memory(void)
{
    volatile ZZ9KMiniGLControlBlockV9 *ctrl = bind_all_ones();
    volatile ZZ9KMiniGLStatusV9 *status =
        (volatile ZZ9KMiniGLStatusV9 *)(void *)(g_window + MW_WINDOW_STATUS_OFFSET);

    assert(ctrl->magic == MW_SERVICE_MAGIC);
    assert(ctrl->consumer_seq == 0);
    assert(ctrl->completion_fence == 0);
    assert(ctrl->op_done_seq == 0);
    assert(ctrl->slot_capacity == MW_DEFAULT_SLOT_CAPACITY_Z3);
    assert(ctrl->slot_count == MW_DEFAULT_SLOT_COUNT_Z3);

    /* Host ownership half: no host has opened the transport yet. */
    assert(ctrl->producer_seq == 0);
    assert(ctrl->active_slot == 0);
    assert(ctrl->op_code == 0);
    assert(ctrl->op_handle == 0);
    assert(ctrl->op_seq == 0);

    assert(status->capabilities == (1U << 18));
    assert(mw_service_has_work() == 0);
    printf("PASS: test_bind_zeroes_control_memory\n");
}

/* Core-1 (re)start re-runs mw_service_init on a live window: retired ring
 * packets must not come back as a phantom backlog, and a pending sync op
 * must complete with CONTEXT_LOST instead of re-executing stale fields. */
static void test_reinit_absorbs_host_progress(void)
{
    volatile ZZ9KMiniGLControlBlockV9 *ctrl = bind_all_ones();

    set_host_u32((volatile uint32_t *)&ctrl->producer_seq, 47U);
    set_host_u32((volatile uint32_t *)&ctrl->op_seq, 5U);
    set_host_u32((volatile uint32_t *)&ctrl->op_code, 1U);
    set_host_u32((volatile uint32_t *)&ctrl->op_handle, 0xdeadbeefU);
    assert(mw_service_has_work());

    mw_service_init();

    assert(ctrl->consumer_seq == 47U);
    assert(ctrl->completion_fence == 47U);
    assert(ctrl->op_done_seq == 5U);
    assert(ctrl->op_status == MW_STATUS_CONTEXT_LOST);
    assert(mw_service_has_work() == 0);
    printf("PASS: test_reinit_absorbs_host_progress\n");
}

/* The exact boot-loop shape: all-ones producer_seq (uninitialized or
 * destroyed control memory). One poll call must retire it. */
static void test_poll_garbage_producer_resyncs_once(void)
{
    volatile ZZ9KMiniGLControlBlockV9 *ctrl = bind_all_ones();

    set_host_u32((volatile uint32_t *)&ctrl->producer_seq, 0xffffffffU);
    assert(mw_service_has_work());

    int done = mw_service_poll();

    /* The old firmware drained the phantom backlog one rejected packet at
     * a time (here: 4294967295 iterations); the resync retires it at once. */
    assert(done >= 1);
    assert(done <= (int)MW_POLL_MAX_RING_BATCH + 1);
    assert(ctrl->consumer_seq == 0xffffffffU);
    assert(ctrl->completion_fence == 0xffffffffU);
    assert(mw_service_has_work() == 0);
    printf("PASS: test_poll_garbage_producer_resyncs_once\n");
}

/* A plausible-but-bogus backlog under the resync threshold still drains in
 * bounded batches, so the worker always returns to its scheduler loop. */
static void test_poll_ring_drain_is_batched(void)
{
    volatile ZZ9KMiniGLControlBlockV9 *ctrl = bind_all_ones();

    set_host_u32((volatile uint32_t *)&ctrl->producer_seq,
                 ctrl->consumer_seq + 100U);
    assert(mw_service_poll() == (int)MW_POLL_MAX_RING_BATCH);
    assert(ctrl->consumer_seq == MW_POLL_MAX_RING_BATCH);
    assert(mw_service_has_work());

    assert(mw_service_poll() == (int)(100U - MW_POLL_MAX_RING_BATCH));
    assert(ctrl->consumer_seq == 100U);
    assert(ctrl->completion_fence == 100U);
    assert(mw_service_has_work() == 0);
    printf("PASS: test_poll_ring_drain_is_batched\n");
}

/* A sustained reject storm must produce a bounded number of log lines, not
 * one per packet: the UART flood is what starved core 0's console. */
static void test_reject_log_is_throttled(void)
{
    volatile ZZ9KMiniGLControlBlockV9 *ctrl = bind_all_ones();
    uint32_t events_before = mw_service_reject_log_events();
    uint32_t rejects = 4U * MW_REJECT_LOG_INTERVAL;
    uint32_t events_after;

    while (rejects) {
        uint32_t round = rejects < MW_POLL_MAX_RING_BATCH ? rejects
                                                          : MW_POLL_MAX_RING_BATCH;
        set_host_u32((volatile uint32_t *)&ctrl->producer_seq,
                     ctrl->consumer_seq + round);
        (void)mw_service_poll();
        rejects -= round;
    }
    assert(ctrl->consumer_seq == ctrl->producer_seq);

    events_after = mw_service_reject_log_events();
    assert(events_after > events_before);
    assert(events_after - events_before <=
           4U * MW_REJECT_LOG_INTERVAL / MW_REJECT_LOG_INTERVAL + 1U);
    printf("PASS: test_reject_log_is_throttled (%u events for %u rejects)\n",
           (unsigned)(events_after - events_before),
           (unsigned)(4U * MW_REJECT_LOG_INTERVAL));
}

int main(void)
{
    printf("Starting ZZ9000 MiniGL Wire-v9 Transport Guard Tests...\n");
    test_bind_zeroes_control_memory();
    test_reinit_absorbs_host_progress();
    test_poll_garbage_producer_resyncs_once();
    test_poll_ring_drain_is_batched();
    test_reject_log_is_throttled();
    printf("All transport guard tests passed.\n");
    return 0;
}
