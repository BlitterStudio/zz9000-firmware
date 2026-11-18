/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the production park handshake with cache and event hardware hooks.
 * A host cannot prove SCU snooping or DSB completion on a physical Cortex-A9. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "scheduler.h"
#include "zzapp_rec.h"

struct cache_model {
    taskq_shared_t shared;
    zzapp_rec_t dirty;
    zzapp_rec_t poc;
    unsigned cleans;
    unsigned events;
};

static void clean_local_l1(void *context)
{
    struct cache_model *model = context;

    /* No observer may reset core 1 until its last updates are durable. */
    assert(model->shared.core1_parked == 0U);
    assert(model->events == 0U);
    memcpy(&model->poc, &model->dirty, sizeof(model->poc));
    model->cleans++;
}

static void publish_park(void *context)
{
    struct cache_model *model = context;

    assert(model->cleans == 1U);
    assert(__atomic_load_n(&model->shared.core1_parked,
                           __ATOMIC_ACQUIRE) == 1U);
    assert(memcmp(&model->dirty, &model->poc, sizeof(model->poc)) == 0);
    model->events++;
}

int main(void)
{
    static struct cache_model model;

    /* No request means no cache maintenance and no acknowledgement. */
    assert(scheduler_core1_service_park_request_with(
        &model.shared, clean_local_l1, publish_park, &model) == 0);
    assert(model.cleans == 0U && model.events == 0U);
    assert(model.shared.core1_parked == 0U);

    /* These are the actual record words recovery consumes, not a second
     * implementation of the park or retirement routines. A detach completed
     * since the previous PoC image must survive a cooperative reset too. */
    model.poc.visible.block[0].ptr = &model;
    model.dirty.coh.running = 0U;
    model.dirty.coh.reset_request = 1U;
    model.dirty.coh.reset_ack = 1U;
    model.dirty.host.exit_state = 1U;
    model.dirty.host.stop_requested = 1U;
    model.dirty.hdr.retire_stage = ZZAPP_RETIRE_RESOURCES;
    model.dirty.kernel.api_wait_sequence = 0x1234U;
    model.dirty.kernel.api_waiting = 1U;
    model.dirty.visible.block[0].ptr = NULL;
    __atomic_store_n(&model.shared.core1_park_request, 1U, __ATOMIC_RELEASE);

    assert(scheduler_core1_service_park_request_with(
        &model.shared, clean_local_l1, publish_park, &model) == 1);
    assert(model.cleans == 1U && model.events == 1U);
    assert(__atomic_load_n(&model.shared.core1_parked, __ATOMIC_ACQUIRE));
    /* Hardware reset discards local L1 but not memory at PoC. */
    memset(&model.dirty, 0, sizeof(model.dirty));
    assert(model.poc.coh.reset_ack == 1U);
    assert(model.poc.host.exit_state == 1U);
    assert(model.poc.host.stop_requested == 1U);
    assert(model.poc.hdr.retire_stage == ZZAPP_RETIRE_RESOURCES);
    assert(model.poc.kernel.api_wait_sequence == 0x1234U);
    assert(model.poc.kernel.api_waiting == 1U);
    assert(model.poc.visible.block[0].ptr == NULL);
    puts("coherent_test: clean precedes park acknowledgement");
    return 0;
}
