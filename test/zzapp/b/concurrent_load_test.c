/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Several claims load at the same time over the real runtime. Each load holds
 * a 16 MiB staging block of the card pool class APP (limit 96 MiB) from
 * LOAD_BEGIN to LOAD_END. When the staging of other claims alone keeps a load
 * from getting its block, the claim must not be refused: LOAD_BEGIN answers
 * BUSY, the claim stays CLAIMED, and the client retries once another load has
 * finished. Only a load that fails with no other staging to wait for is a
 * MEMORY refusal. */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

#define CLAIMS 10U

/* A client's retry loop: LOAD_BEGIN until it is not BUSY. The mock does not
 * retry, so the test is the retrying client. */
static uint16_t begin_and_chunk(uint32_t n)
{
    uint8_t image[RT_IMAGE_BYTES];

    rt_image(image, 1);
    return rt_load_image(n, image);
}

static void finish(uint32_t n)
{
    assert(rt_simple(n, ZZAPP_OP_LOAD_END) == SDK_STATUS_OK);
    assert(rt_start(n) == SDK_STATUS_OK);
}

static void back_to_back(void)
{
    struct rt_counts start;
    uint32_t loading[CLAIMS], waiting[CLAIMS], l = 0U, w = 0U, i;

    rt_boot(RT_MB(512), RT_MAX_APPS);
    start = rt_counts();
    for (i = 0U; i < CLAIMS; i++)
        rt_claim(i);
    /* All claims send LOAD_BEGIN, LOAD_CHUNK close together, before any of
     * them reaches LOAD_END. */
    for (i = 0U; i < CLAIMS; i++) {
        uint16_t status = begin_and_chunk(i);

        if (status == SDK_STATUS_OK) {
            loading[l++] = i;
        } else {
            /* A collision is BUSY, never the end of the claim. */
            assert(status == SDK_STATUS_BUSY);
            assert(rt_last_reply_len == 0U);
            waiting[w++] = i;
        }
    }
    assert(l >= 2U && w >= 1U);
    for (i = 0U; i < w; i++) {
        zzapp_rec_t *rec = rt_rec(waiting[i]);

        assert(rec && rec->claim.state == ZZAPP_ST_CLAIMED);
        assert(RT_STATE(waiting[i]) == AAR_STATE_CLAIMED);
        assert(rec->claim.refusal_reason == AAR_REASON_NONE);
    }
    /* The claims that hold staging go on, interleaved with the retries. */
    for (i = 0U; i < w; i++) {
        /* Still no room while every staging block is held. */
        assert(begin_and_chunk(waiting[i]) == SDK_STATUS_BUSY);
    }
    for (i = 0U; i < l; i++) {
        uint32_t done = loading[i];

        finish(done);
        /* The staging block this claim gave back lets one waiting claim in. */
        if (i < w) {
            assert(begin_and_chunk(waiting[i]) == SDK_STATUS_OK);
            finish(waiting[i]);
        }
    }
    for (i = l; i < w; i++) {
        assert(begin_and_chunk(waiting[i]) == SDK_STATUS_OK);
        finish(waiting[i]);
    }
    for (i = 0U; i < CLAIMS; i++) {
        zzapp_rec_t *rec = rt_rec(i);

        assert(rec && rec->coh.running);
        assert(RT_STATE(i) == AAR_STATE_RUNNING);
    }
    for (i = 0U; i < CLAIMS; i++)
        assert(rt_simple(i, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    zzapp_test_set_xtime(1);
    rt_settle(CLAIMS);
    zzapp_test_set_xtime(10000000);
    rt_settle(CLAIMS);
    rt_assert_back(start);
    zzapp_on_amiga_reset();
}

/* A claim that waits on BUSY can be released without leaving anything. */
static void release_while_busy(void)
{
    struct rt_counts start;
    uint32_t i, first_busy = CLAIMS;

    rt_boot(RT_MB(512), RT_MAX_APPS);
    start = rt_counts();
    for (i = 0U; i < CLAIMS; i++) {
        rt_claim(i);
        if (begin_and_chunk(i) != SDK_STATUS_OK) {
            first_busy = i;
            break;
        }
    }
    assert(first_busy < CLAIMS);
    assert(rt_simple(first_busy, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    for (i = 0U; i < first_busy; i++)
        assert(rt_simple(i, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(2U * CLAIMS);
    rt_assert_back(start);
    zzapp_on_amiga_reset();
}

int main(void)
{
    back_to_back();
    release_while_busy();
    puts("concurrent_load_test: ok (staging collisions answer BUSY, then load)");
    return 0;
}
