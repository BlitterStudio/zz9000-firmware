/* Quiesce-timeout completion posting: a task that outlived the quiesce
 * wait is FAILED by the halt-path reclaim, and its client must receive an
 * INTERNAL_ERROR completion carrying its original request id and cookie
 * BEFORE the queue is reinitialised. Drives the production seams in
 * scheduler.c (scheduler_quiesce_finish + scheduler_harvest_post_with),
 * composed exactly as the ARM glue composes them. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "scheduler.h"

#define FAIL_STATUS_SENTINEL 0x5A5AU

struct posted {
    int slot;
    uint32_t request_id;
    uint32_t user_cookie;
    uint16_t opcode;
    uint16_t status;
    uint16_t payload_len;
    uint8_t payload[TASKQ_RESULT_PAYLOAD];
};
static struct posted posts[TASKQ_CAPACITY];
static uint32_t n_posts;
static uint32_t forget_calls;
static int stale_slot = -1;        /* slot failing the generation check */
static int full_ring_slot = -1;    /* slot whose post reports a full ring */
static uint32_t nonfree_seen_at_post;
static int order_next;
static int restart_order = -1;
static int first_post_order = -1;
static uint32_t restart_calls;
static taskq_shared_t *active_shared;

static int gen_ok(int slot)
{
    return slot != stale_slot;
}

static int post(int slot, uint32_t request_id, uint32_t user_cookie,
                uint16_t opcode, uint16_t status, const uint8_t *payload,
                uint16_t payload_len)
{
    struct posted *p;
    uint32_t i;

    if (slot == full_ring_slot)
        return 0;                    /* completion ring full */

    if (first_post_order < 0)
        first_post_order = order_next++;
    /* The queue must still hold the harvested descriptors: posting happens
     * before taskq_init wipes them. */
    for (i = 0; i < TASKQ_CAPACITY; i++)
        if (active_shared->queue.descs[i].state != TASK_FREE &&
            i != (uint32_t)slot)
            nonfree_seen_at_post++;

    assert(n_posts < TASKQ_CAPACITY);
    p = &posts[n_posts++];
    p->slot = slot;
    p->request_id = request_id;
    p->user_cookie = user_cookie;
    p->opcode = opcode;
    p->status = status;
    p->payload_len = payload_len;
    memcpy(p->payload, payload, payload_len);
    return 1;
}

static void forget(int slot)
{
    (void)slot;
    forget_calls++;
}

static void restart_core1(void)
{
    restart_calls++;
    restart_order = order_next++;
    /* core1_cold_restart's halt-path reclaim fails every active task. */
    taskq_abort_active(&active_shared->queue, 0U);
}

static void post_completions(void)
{
    scheduler_harvest_post_with(&active_shared->queue, NULL,
                                FAIL_STATUS_SENTINEL, gen_ok, post, forget);
}


int main(void)
{
    static taskq_shared_t sh;
    int a, b, c;
    uint32_t i;

    active_shared = &sh;

    /* --- timeout teardown: restart, post failures, then re-init --- */
    taskq_shared_boot_init(&sh);
    a = taskq_enqueue(&sh.queue, TASKQ_OP_AUDIO_STREAM_FEED, TASK_LONG,
                      0x1000U, 64U, 0x2000U, 64U, 0xA1B2C3D4U, 0x13572468U,
                      NULL, 0U);
    b = taskq_enqueue(&sh.queue, TASKQ_OP_DECODE_MP3, TASK_LONG,
                      0x1100U, 32U, 0x2100U, 32U, 0xB2C3D4E5U, 0x2468ACE0U,
                      NULL, 0U);
    assert(a >= 0 && b >= 0);
    /* One task already claimed by the worker; both are "active". */
    assert(taskq_claim_any(&sh.queue) >= 0);
    sh.core1_restart_request = 1U;
    sh.core1_current_slot = 0;

    scheduler_quiesce_finish(&sh, 1 /* timed out */, restart_core1,
                             post_completions);

    /* Cold restart ran first, before any completion was posted. */
    assert(restart_calls == 1U);
    assert(first_post_order > restart_order);
    /* Both active tasks were failed and posted to their clients with the
     * failure status, keeping their original request ids and cookies. */
    assert(n_posts == 2U);
    assert(nonfree_seen_at_post > 0U);  /* posted BEFORE taskq_init */
    for (i = 0; i < n_posts; i++) {
        const struct posted *p = &posts[i];
        assert(p->status == FAIL_STATUS_SENTINEL);
        assert(p->payload_len == 0U);
        assert((p->request_id == 0xA1B2C3D4U &&
                p->user_cookie == 0x13572468U &&
                p->opcode == TASKQ_OP_AUDIO_STREAM_FEED) ||
               (p->request_id == 0xB2C3D4E5U &&
                p->user_cookie == 0x2468ACE0U &&
                p->opcode == TASKQ_OP_DECODE_MP3));
    }
    /* The queue was reinitialised afterwards: no descriptor survives. */
    for (i = 0; i < TASKQ_CAPACITY; i++)
        assert(sh.queue.descs[i].state == TASK_FREE);
    assert(sh.core1_current_slot == -1);
    assert(sh.core1_restart_request == 0U);

    /* --- generation check: a task from a previous mailbox lifetime is
     * forgotten, never posted --- */
    taskq_shared_boot_init(&sh);
    c = taskq_enqueue(&sh.queue, TASKQ_OP_SCALE_IMAGE, TASK_LONG,
                      0x3000U, 16U, 0x4000U, 16U, 0xDEADBEEFU, 0xCAFEBABEU,
                      NULL, 0U);
    assert(c >= 0);
    stale_slot = c;
    taskq_abort_active(&sh.queue, 0U);
    n_posts = 0U;
    scheduler_harvest_post_with(&sh.queue, NULL, FAIL_STATUS_SENTINEL,
                                gen_ok, post, forget);
    assert(n_posts == 0U);
    assert(forget_calls == 1U);
    assert(sh.queue.descs[c].state == TASK_FREE);  /* dropped, not stranded */

    /* --- full completion ring: the task stays harvested for retry, and a
     * later poll posts its DONE result unchanged --- */
    taskq_shared_boot_init(&sh);
    stale_slot = -1;  /* generation tags were reset by boot init */
    a = taskq_enqueue(&sh.queue, TASKQ_OP_DECODE_JPEG, TASK_LONG,
                      0x1000U, 64U, 0x2000U, 64U, 0x0BADF00DU, 0x0FEE1D00U,
                      NULL, 0U);
    assert(a >= 0);
    {
        const uint8_t done_payload[3] = {0x11U, 0x22U, 0x33U};

        taskq_complete(&sh.queue, a, 0x4321U, done_payload,
                       sizeof(done_payload));
    }
    full_ring_slot = a;
    scheduler_harvest_post_with(&sh.queue, NULL, FAIL_STATUS_SENTINEL,
                                gen_ok, post, forget);
    assert(n_posts == 0U);
    assert(sh.queue.descs[a].state == TASK_DONE);  /* kept for the retry */

    full_ring_slot = -1;
    scheduler_harvest_post_with(&sh.queue, NULL, FAIL_STATUS_SENTINEL,
                                gen_ok, post, forget);
    assert(n_posts == 1U);
    assert(posts[0].status == 0x4321U);            /* result, not failure */
    assert(posts[0].request_id == 0x0BADF00DU);
    assert(posts[0].user_cookie == 0x0FEE1D00U);
    assert(posts[0].payload_len == 3U);
    assert(posts[0].payload[0] == 0x11U && posts[0].payload[2] == 0x33U);
    assert(sh.queue.descs[a].state == TASK_FREE);

    return 0;
}
