/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Per-app host-visible limit, as the client writes it into the control block
 * before LOAD_BEGIN (AAR_CB_SHARED_MIB): the default, a raised and a lowered
 * request, the refusal of a request the card could never hold, a single block
 * larger than 16 MiB, and the accounting. The limit covers the runtime's own
 * blocks as well as the app's: the pair is counted from the claim on and the
 * load buffer until START, so what an app can still allocate is the limit
 * less what the runtime holds (zzapp_xport_shared_used).
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

void zzapp_test_set_host_visible_heap(uint32_t bytes);

#define MIB (1024U * 1024U)
#define CEILING (78U * MIB)

static struct rt_counts g_start;

/* A fresh runtime with one claim; nothing is loaded yet. */
static void claimed(void)
{
    rt_boot(RT_MB(256), 8U);
    zzapp_test_set_host_visible_heap(CEILING);
    g_start = rt_counts();
    rt_claim(0U);
}

/* LOAD_BEGIN with the limits the client wrote, then the record selected for
 * the host calls below. */
static uint16_t load_begin(uint32_t private_mib, uint32_t shared_mib)
{
    uint16_t status;

    mock_client_set_limits(&rt_cl[0], private_mib, shared_mib, 0U);
    status = mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    return status;
}

static uint64_t shared_used(void)
{
    aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK);
    return info.shared_used;
}

static uint64_t host_limit(void)
{
    aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK);
    return info.shared_limit;
}

/* Bytes the app can still allocate under the limit: it counts what the
 * runtime holds and what the app already has. */
static uint64_t room(void)
{
    return host_limit() - shared_used();
}

/* The claim ends: the client releases it, core 1 retires it and everything
 * is back, the app's blocks included. */
static void finish(void)
{
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(4U);
    rt_assert_back(g_start);
    assert(zzapp_rec_by_handle(rt_cl[0].handle) == NULL);
}

static void begin_with(uint32_t shared_mib, uint32_t expected_status)
{
    claimed();
    assert(load_begin(0U, shared_mib) == expected_status);
}

static void test_default_is_16_mib_without_a_request(void)
{
    void *block;
    uint64_t free_room;

    begin_with(0U, SDK_STATUS_OK);
    assert(host_limit() == 16U * MIB);
    /* The pair and the load buffer are inside the 16 MiB. */
    assert(shared_used() == zzapp_xport_shared_used(rt_rec(0U)));
    free_room = room();
    assert(free_room < 16U * MIB && free_room > 15U * MIB);
    assert((free_room & 4095U) == 0U);
    block = zzapp_host_alloc((uint32_t)free_room, 4096U);
    assert(block != NULL);
    assert(room() == 0U);
    assert(zzapp_host_alloc(4096U, 4096U) == NULL);
    assert(zzapp_host_free(block) == AAR_OK);
    /* 16 MiB alone would fit the old whole-app limit, but not with the
     * runtime's own blocks counted. */
    assert(zzapp_host_alloc(16U * MIB, 4096U) == NULL);
    assert(zzapp_host_alloc(16U * MIB + 4096U, 4096U) == NULL);
    finish();

    /* The memory cap alone: the host-visible limit stays the default. */
    claimed();
    assert(load_begin(32U, 0U) == SDK_STATUS_OK);
    assert(zzapp_services_memory_limit() == 32U * MIB);
    assert(host_limit() == 16U * MIB);
    finish();
}

static void test_request_raises_the_limit(void)
{
    void *block;
    uint64_t rest;

    begin_with(40U, SDK_STATUS_OK);
    assert(host_limit() == 40U * MIB);
    assert(zzapp_services_memory_limit() == 0U);
    /* One block of 34 MiB, larger than the old whole-app limit. */
    block = zzapp_host_alloc(34U * MIB, 4096U);
    assert(block != NULL);
    assert(shared_used() == 34U * MIB + zzapp_xport_shared_used(rt_rec(0U)));
    /* The control block counts it the same way. */
    assert(zzapp_xport_cb_get64(rt_rec(0U), AAR_CB_SHARED_USED_HI) == shared_used());
    assert(host_limit() == 40U * MIB);
    /* The cap counts every block of the app. */
    rest = room();
    assert(rest > 5U * MIB && rest < 6U * MIB);
    assert(zzapp_host_alloc((uint32_t)rest + 4096U, 4096U) == NULL);
    assert(zzapp_host_alloc((uint32_t)rest, 4096U) != NULL);
    assert(zzapp_host_free(block) == AAR_OK);
    finish();
}

static void test_request_can_lower_the_limit(void)
{
    begin_with(4U, SDK_STATUS_OK);
    assert(host_limit() == 4U * MIB);
    assert(zzapp_host_alloc(5U * MIB, 4096U) == NULL);
    assert(zzapp_host_alloc(4U * MIB, 4096U) == NULL);
    assert(zzapp_host_alloc((uint32_t)room(), 4096U) != NULL);
    finish();
}

static void test_request_beyond_the_card_is_refused(void)
{
    /* Far more than the heap and one app's alias window can hold. */
    begin_with(200U, SDK_STATUS_NO_MEMORY);
    assert(host_limit() == 16U * MIB);
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);

    /* The refusal leaves the claim usable: a smaller request then works. */
    assert(load_begin(0U, 24U) == SDK_STATUS_OK);
    assert(host_limit() == 24U * MIB);
    finish();

    /* The ceiling follows the heap the allocator manages. */
    claimed();
    zzapp_test_set_host_visible_heap(20U * MIB);
    assert(load_begin(0U, 24U) == SDK_STATUS_NO_MEMORY);
    assert(load_begin(0U, 20U) == SDK_STATUS_OK);
    assert(host_limit() == 20U * MIB);
    zzapp_test_set_host_visible_heap(CEILING);
    finish();

    /* A word no limit can name is a malformed request, not a refusal. */
    claimed();
    assert(load_begin(0U, 4096U) == SDK_STATUS_BAD_REQUEST);
    assert(load_begin(4096U, 0U) == SDK_STATUS_BAD_REQUEST);
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
    assert(load_begin(0U, 8U) == SDK_STATUS_OK);
    assert(host_limit() == 8U * MIB);
    finish();
}

/* A LOAD_BEGIN names its claim and carries a full payload: the shorter forms
 * the window protocol accepted are gone. */
static void test_short_load_begin_is_refused(void)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint16_t bytes = 0U;

    claimed();
    zzapp_build_load_begin_request(req, rt_cl[0].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0U, reply, &bytes,
                                  1, 1U, 0U) == SDK_STATUS_BAD_REQUEST);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, 8U, reply, &bytes,
                                  1, 2U, 0U) == SDK_STATUS_BAD_REQUEST);
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
    finish();
}

static void test_limit_does_not_outlive_the_claim(void)
{
    begin_with(40U, SDK_STATUS_OK);
    assert(host_limit() == 40U * MIB);
    finish();
    /* The next claim starts from the default again. */
    begin_with(0U, SDK_STATUS_OK);
    assert(host_limit() == 16U * MIB);
    finish();
}

/* A second LOAD_BEGIN that the claim refuses (the first is still queued) must
 * not change the limits the first one set, though the client has written new
 * words into the control block by then. */
static void test_rejected_load_begin_keeps_the_first_limits(void)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint16_t status = SDK_STATUS_BUSY;
    uint32_t pass;

    claimed();
    mock_client_set_limits(&rt_cl[0], 8U, 40U, 0U);
    zzapp_build_load_begin_request(req, rt_cl[0].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), reply,
                                  &bytes, 1, 1U, 0U) == SDK_STATUS_QUEUED);
    mock_client_set_limits(&rt_cl[0], 2U, 4U, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), reply,
                                  &bytes, 1, 2U, 0U) == SDK_STATUS_BUSY);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_services_memory_limit() == 8U * MIB);
    assert(host_limit() == 40U * MIB);
    for (pass = 0U; pass < 16U; pass++) {
        rt_core1_pass();
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes)) {
            zzapp_mailbox_harvest_ack();
            break;
        }
    }
    assert(status == SDK_STATUS_OK);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_services_memory_limit() == 8U * MIB);
    assert(host_limit() == 40U * MIB);
    finish();
}

/* Card sequence: a client's LOAD_BEGIN is refused (host-visible limit too
 * large), it closes its claim with STOP, and the next client's ordinary
 * LOAD_BEGIN must still be accepted, plain and with a limit. */
static void test_load_after_a_refused_claim_was_cancelled(void)
{
    uint32_t pass;

    claimed();
    assert(load_begin(0U, 63U) == SDK_STATUS_NO_MEMORY);
    assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_OK);
    rt_settle(8U);
    /* The stop of a claim that never loaded ends it through retire; the
     * shell is the client's to release. */
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    /* A STOP the client asked for ends the claim FORCED, not NONE. */
    assert(RT_REASON(0U) == AAR_END_FORCED);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(4U);
    rt_assert_back(g_start);

    for (pass = 0U; pass < 2U; pass++) {
        rt_claim(0U);
        assert(load_begin(0U, pass == 1U ? 8U : 0U) == SDK_STATUS_OK);
        assert(host_limit() == (pass == 1U ? 8U : 16U) * MIB);
        finish();
    }
}

/* AAR_CB_PRIVATE_COMMITTED follows the record's committed private bytes: the
 * loaded image, a heap grow and a heap shrink. */
static void test_private_committed_is_published(void)
{
    zzapp_rec_t *rec;
    void *old_break;
    uint32_t loaded;

    claimed();
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    rec = rt_rec(0U);
    loaded = rec->proc.memory.committed;
    assert(loaded != 0U);
    assert(zzapp_xport_cb_get64(rec, AAR_CB_PRIVATE_COMMITTED_HI) == loaded);
    assert(zzapp_proc_heap_grow(MIB, &old_break) == AAR_OK);
    assert(rec->proc.memory.committed == loaded + MIB);
    assert(zzapp_xport_cb_get64(rec, AAR_CB_PRIVATE_COMMITTED_HI) ==
           loaded + MIB);
    assert(zzapp_proc_heap_shrink(MIB, &old_break) == AAR_OK);
    assert(zzapp_xport_cb_get64(rec, AAR_CB_PRIVATE_COMMITTED_HI) == loaded);
    finish();
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        { "default", test_default_is_16_mib_without_a_request },
        { "raise", test_request_raises_the_limit },
        { "lower", test_request_can_lower_the_limit },
        { "refuse", test_request_beyond_the_card_is_refused },
        { "short", test_short_load_begin_is_refused },
        { "reclaim", test_limit_does_not_outlive_the_claim },
        { "rejected", test_rejected_load_begin_keeps_the_first_limits },
        { "cancel", test_load_after_a_refused_claim_was_cancelled },
        { "private", test_private_committed_is_published },
    };
    unsigned i;

    /* One argument names a single case, so a failing case is reported on
     * its own; no argument runs them all. */
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc < 2 || strcmp(argv[1], cases[i].name) == 0)
            cases[i].run();
    }
    puts("host_visible_limit_test: ok");
    return 0;
}
