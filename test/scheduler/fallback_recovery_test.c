/* Drives the production single-core fallback recovery seam
 * (scheduler_core1_fallback_recover_with in scheduler.c) with ordered
 * step stubs: the recovery ORDER is firmware logic, so this fails if a
 * step is dropped or reordered there. */
#include <assert.h>
#include <stdint.h>

#include "scheduler.h"

static uint32_t stage;
static uint32_t recovery_calls;
static taskq_t *seen_queue;
static uint32_t seen_module;
static volatile uint32_t started;

static void step_halt(void)
{
    assert(stage == 0U);
    stage = 1U;
}

static void step_recover(void)
{
    assert(stage == 1U);
    recovery_calls++;
    stage = 2U;
}

static void step_reclaim_queue(taskq_t *queue)
{
    assert(stage == 2U);
    assert(recovery_calls == 1U);
    seen_queue = queue;
    stage = 3U;
}

static void step_reclaim_audio(void)
{
    assert(stage == 3U);
    stage = 4U;
}

static void step_quarantine(uint32_t module)
{
    assert(stage == 4U);
    seen_module = module;
    stage = 5U;
}

int main(void)
{
    static taskq_t queue;

    taskq_init(&queue);
    started = 1U;

    scheduler_core1_fallback_recover_with(
        step_halt, step_recover, step_reclaim_queue, &queue,
        step_reclaim_audio, step_quarantine, 0x1234U, &started);

    /* Every step ran exactly once, in the production order, and the seam
     * cleared the core-1 liveness flag last. */
    assert(recovery_calls == 1U);
    assert(seen_queue == &queue);
    assert(seen_module == 0x1234U);
    assert(stage == 5U);
    assert(started == 0U);
    return 0;
}
