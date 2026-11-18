/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The lifecycle of a claim around resets, cancellations and races, through
 * the real runtime. A client that disappears (lease), an Amiga reset, a stop
 * that arrives before, during or after a START, a START that fails: each ends
 * the claim once, with its reason in the control block, leaves nothing held
 * but the shell the client has not released, and does not disturb a claim
 * another client made meanwhile. Core 0 and core 1 are interleaved at the
 * points where hardware can: the poll boundary of a deferred operation, the
 * START handoff and the admission points of the teardown.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);
uint8_t *zzapp_test_stack_canary(void);
void zzapp_test_set_service_hook(void (*fn)(void));
void zzapp_test_set_fail_start(int fail);
void zzapp_test_set_start_race_hook(void (*fn)(void));
void zzapp_test_set_admission_hook(void (*fn)(void));

/* What the app of the case does when it runs. */
enum entry_mode {
    ENTRY_RETURN,        /* exits by returning */
    ENTRY_LEASE_EXIT,    /* the lease runs out, then it leaves by itself */
    ENTRY_STOP_EXIT,     /* the client stops it, it leaves by itself */
    ENTRY_DIRECT_STOP,   /* the client writes the stop word, it never leaves */
    ENTRY_CANARY,        /* damages its stack canary */
    ENTRY_RESET,         /* holds a shared block while the card resets */
};
static enum entry_mode mode;
static const aar_api *app_api;
static unsigned direct_stop_seen;
static struct rt_counts g_start;

static void app_entry(const aar_api *api)
{
    app_api = api;
    switch (mode) {
    case ENTRY_RETURN:
        zzapp_host_exit(0);
        break;
    case ENTRY_LEASE_EXIT:
        zzapp_host_tick_at(ZZAPP_LEASE_US - 1U);
        zzapp_test_set_xtime(ZZAPP_LEASE_US);
        zzapp_core0_poll();
        assert(api->should_stop() == 1);
        zzapp_host_exit(0);
        break;
    case ENTRY_STOP_EXIT:
        assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_OK);
        assert(api->should_stop() == 1);
        zzapp_host_exit(0);
        break;
    case ENTRY_DIRECT_STOP:
        mock_client_request_stop(&rt_cl[0]);
        zzapp_host_tick_at(ZZAPP_TICK_US);
        direct_stop_seen = (unsigned)api->should_stop();
        assert(direct_stop_seen == 1U);
        zzapp_host_tick_at(ZZAPP_TICK_US + ZZAPP_STOP_GRACE_US);
        zzapp_worker_hook();
        break;
    case ENTRY_CANARY: {
        uint8_t *canary = zzapp_test_stack_canary();

        assert(canary != NULL);
        canary[0] ^= 0xA5u;
        zzapp_host_yield();
        break;
    }
    case ENTRY_RESET: {
        aar_shared shared;
        aar_info info;

        memset(&shared, 0, sizeof(shared));
        assert(api->shared_alloc(4096u, 256u, 0u, &shared) == AAR_OK);
        assert(shared.ptr != NULL);
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        assert(api->info(&info) == AAR_OK);
        assert(info.shared_used == 4096u + zzapp_xport_shared_used(rt_rec(0U)));
        zzapp_on_amiga_reset();
        zzapp_host_yield();
        break;
    }
    }
}

static void setup(void)
{
    rt_boot(RT_MB(256), 8U);
    g_start = rt_counts();
    rt_entry = app_entry;
    mode = ENTRY_RETURN;
}

/* The client of claim `n` lets go of whatever it holds, and everything is
 * back. A revoked shell answers its old handle with BAD_HANDLE. */
static void release_all(uint32_t n, uint16_t want)
{
    assert(rt_simple(n, ZZAPP_OP_RELEASE) == want);
    rt_settle(6U);
}

/* ---- Lease expiry of a claim that is only loading ------------------------- */

/* Core 0 expires a LOADING claim before core 1 sees the stop: the shell reads
 * STOPPING until core 1's pass retires it, and its handle is revoked. */
static void expire_loading_claim(uint32_t *stale_handle, uint32_t *current_handle)
{
    setup();
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    assert(RT_STATE(0U) == AAR_STATE_LOADING);
    *stale_handle = rt_cl[0].handle;
    zzapp_services_test_set_core(0);
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
    assert(RT_STATE(0U) == AAR_STATE_STOPPING);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_runtime_state() == ZZAPP_ST_LOADING);
    zzapp_worker_hook();
    /* The reclaim returned the record's resources (host view IDLE) and the
     * shell reads IDLE with the end reason. */
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_test_arena_card() == 0U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == ZZAPP_END_QUARANTINED);
    *current_handle = rt_listed_handle(rt_pid[0]);
    assert(*current_handle != 0U && *current_handle != *stale_handle);
}

/* The old client's next request is stale and releases the shell; the claim
 * after it loads normally. */
static void test_stale_contact_releases_expired_load(void)
{
    uint32_t stale, current;

    expire_loading_claim(&stale, &current);
    assert(rt_simple(0U, ZZAPP_OP_LOAD_CHUNK) == SDK_STATUS_BAD_HANDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* An Amiga reset drops the quarantine: the shell is released with it. */
static void test_reset_releases_expired_load(void)
{
    uint32_t stale, current;

    expire_loading_claim(&stale, &current);
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_handle(current) == NULL);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* An administrator releases the shell under the handle LIST reports. */
static void test_administrative_release_of_expired_load(void)
{
    uint32_t stale, current;

    expire_loading_claim(&stale, &current);
    rt_cl[0].handle = current;
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    /* The old handle names nothing, and neither does the released one. */
    rt_cl[0].handle = stale;
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_BAD_HANDLE);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A LOAD_BEGIN that core 1 has taken but not yet run (the poll boundary) is
 * cancelled by the expiry; core 1 completes the cancellation with no loader
 * arena, and the shell can still be released. */
static void test_cancel_deferred_load_before_loader_begin(void)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint32_t stale, current;
    zzapp_rec_t *rec;

    setup();
    rt_claim(0U);
    rec = rt_rec(0U);
    stale = rt_cl[0].handle;
    zzapp_build_load_begin_request(req, stale, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), reply,
                                  &bytes, 1, 1U, 0U) == SDK_STATUS_QUEUED);
    assert(runtime_of(rec)->defer.state == DEFER_POSTED);
    /* The production poll selects the request; core 0 expires the claim
     * before run_deferred enters zzapp_loader_begin(). */
    runtime_of(rec)->defer.state = DEFER_RUNNING;
    zzapp_services_expire_claims(ZZAPP_CLAIM_LEASE_US);
    assert(RT_STATE(0U) == AAR_STATE_STOPPING);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    run_deferred(rec);
    current = rt_listed_handle(rt_pid[0]);
    assert(current != stale);
    /* The cancellation reclaimed the quarantine with no loader arena, so the
     * shell can be released explicitly. */
    rt_cl[0].handle = current;
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* ---- START racing a cancellation ------------------------------------------ */

static int race_hook_fired;

/* Core 0, between START's preparation and its completion: the claim expires
 * and the old client sends one more request with its (now stale) handle. */
static void expire_and_record_intent(void)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    int saved = defer_test_core;

    zzapp_test_set_start_race_hook(NULL);
    zzapp_test_set_service_hook(NULL);
    race_hook_fired = 1;
    zzapp_services_test_set_core(0);
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
    zzapp_build_load_chunk_request(req, rt_cl[0].handle, 0U, 1U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, req, sizeof(req), reply,
                                  &bytes, 1, 77U, 0U) == SDK_STATUS_BAD_HANDLE);
    defer_test_core = saved;
}

/* Core 0 cancels the running START the way STOP cancels a claim: no
 * quarantine, so start_entered completes the record. */
static void cancel_start_without_quarantine(void)
{
    zzapp_test_set_service_hook(NULL);
    race_hook_fired = 1;
    slot_state_lock_acquire();
    assert(stop_current_deferred(rt_rec(0U)) == 0);
    begin_claim_cancellation(rt_rec(0U), ZZAPP_END_FORCED);
    slot_state_lock_release();
}

static void start_racing_expiry_with_release_intent(void)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U, status;

    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    zzapp_build_start_app_request(req, rt_cl[0].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, req, sizeof(req), reply,
                                  &bytes, 1, 5U, 0U) == SDK_STATUS_QUEUED);
    race_hook_fired = 0;
    zzapp_test_set_start_race_hook(expire_and_record_intent);
    rt_core1_pass();
    zzapp_test_set_start_race_hook(NULL);
    assert(race_hook_fired == 1);
    while (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, NULL))
        zzapp_mailbox_harvest_ack();
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    /* The old client's request recorded its release intent while START was
     * in flight. The rejection teardown is the final writer: it completes the
     * release, so the shell is gone with its pair, nothing was re-published
     * as STOPPING (the freed record keeps its last words) and the app never
     * ran. */
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_pid(rt_pid[0])->claim.state == ZZAPP_ST_IDLE);
    assert(rt_entries == 0U);
    rt_settle(4U);
    rt_assert_back(g_start);
}

static uint32_t admitted_handle;
static mock_client late_client;

/* Core 0 makes a claim at every admission boundary of the START teardown:
 * the first time it can, it holds a record of its own (the cancelled app's
 * is still the shell the old client has not released). */
static void claim_at_admission_point(void)
{
    uint16_t status;

    if (admitted_handle != 0U)
        return;
    mock_client_init(&late_client);
    late_client.mailbox = rt_mailbox;
    status = mock_client_claim(&late_client);
    if (status != SDK_STATUS_OK)
        return;
    admitted_handle = late_client.handle;
    zzapp_test_set_admission_hook(NULL);
}

/*
 * A START that core 0 cancels mid-flight is torn down by core 1. A claim core
 * 0 makes at any admission boundary of that teardown must keep CLAIMED, its
 * handle and its record, and its client loads normally afterwards.
 */
static void claim_survives_start_teardown(void (*race)(void), int fail_start)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint32_t stale_handle;
    zzapp_rec_t *late;

    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    stale_handle = rt_cl[0].handle;
    zzapp_build_start_app_request(req, stale_handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, req, sizeof(req), reply,
                                  &bytes, 1, 6U, 0U) == SDK_STATUS_QUEUED);
    race_hook_fired = 0;
    admitted_handle = 0U;
    zzapp_test_set_start_race_hook(race);
    zzapp_test_set_admission_hook(claim_at_admission_point);
    zzapp_test_set_fail_start(fail_start);
    rt_core1_pass();
    zzapp_test_set_start_race_hook(NULL);
    zzapp_test_set_admission_hook(NULL);
    while (zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL))
        zzapp_mailbox_harvest_ack();
    assert(race_hook_fired == 1);
    assert(admitted_handle != 0U && admitted_handle != stale_handle);
    /* The claim made meanwhile has its own record and was not touched. */
    late = zzapp_rec_by_handle(admitted_handle);
    assert(late != NULL && late->hdr.pid != rt_pid[0]);
    assert(zzapp_xport_cb_get(late, AAR_CB_STATE) == AAR_STATE_CLAIMED);
    assert(zzapp_xport_cb_get(late, AAR_CB_HANDLE) == admitted_handle);
    assert(late->claim.state == ZZAPP_ST_CLAIMED);
    /* The cancelled app's claim ended; core 1 left the host IDLE. A shell
     * a stale request released is gone with its pair. */
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    if (zzapp_rec_state(rt_pid[0]) != ZZAPP_REC_FREE)
        assert(RT_STATE(0U) == AAR_STATE_IDLE);
    /* The new owner can load, and everything goes back at the end. */
    rt_cl[1] = late_client;
    rt_pid[1] = late->hdr.pid;
    (void)mock_client_register(&rt_cl[1]);
    assert(rt_load(1U, 1U) == SDK_STATUS_OK);
    assert(RT_STATE(1U) == AAR_STATE_LOADING);
    /* The cancelled claim's shell, if it is still held, goes under whichever
     * handle it has now. */
    if (zzapp_rec_state(rt_pid[0]) != ZZAPP_REC_FREE) {
        uint32_t listed = rt_listed_handle(rt_pid[0]);

        if (zzapp_rec_by_handle(stale_handle) == NULL)
            rt_cl[0].handle = listed;
        assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    }
    release_all(1U, SDK_STATUS_OK);
    rt_settle(4U);
    rt_assert_back(g_start);
}

/* ---- Reset ----------------------------------------------------------------- */

/*
 * An Amiga reset while core 1 is in the middle of a LOAD_END (the loader
 * works through a big image a pass at a time): the reset ends the claim and
 * cancels the op, and nothing of it is held afterwards. The allocator is reset
 * next, so the shell is released with the reset, not left for its client.
 * Core 1's later passes find the cancelled op harmlessly, and a claim made
 * after the reset loads normally.
 */
static void test_reset_with_a_load_in_flight(void)
{
    uint8_t image[RT_IMAGE_BYTES];
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint32_t handle;
    zzapp_rec_t *rec;

    setup();
    rt_claim(0U);
    rec = rt_rec(0U);
    handle = rt_cl[0].handle;
    rt_image(image, 30U);
    assert(rt_load_image(0U, image) == SDK_STATUS_OK);
    zzapp_build_handle(req, handle);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_END, req, sizeof(req), reply,
                                  &bytes, 1, 1U, 0U) == SDK_STATUS_QUEUED);
    /* One pass of core 1: the load is under way, not finished. */
    zzapp_services_test_set_core(1);
    zzapp_kernel_set_current_thread(-1);
    zzapp_services_poll();
    assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL));
    assert(runtime_of(rec)->defer.state == DEFER_RUNNING);
    zzapp_services_test_set_core(0);
    zzapp_on_amiga_reset();
    assert(runtime_of(rec)->defer.cancelled);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_handle(handle) == NULL);
    /* The loader holds nothing for the cancelled op. */
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    rt_settle(3U);
    assert(zzapp_test_arena_card() == 0U);
    while (zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL))
        zzapp_mailbox_harvest_ack();
    rt_assert_back(g_start);
    /* A claim after it is admitted and loads. */
    rt_claim(0U);
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A reset during a partial upload abandons it exactly once. */
static void test_reset_during_a_partial_upload(void)
{
    setup();
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    zzapp_on_amiga_reset();
    rt_settle(2U);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    rt_settle(2U);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A running app sees a reset as an immediate forced stop; its host-visible
 * block goes with the record, and the card claims and runs again. */
static void test_reset_of_a_running_app(void)
{
    uint32_t pid;
    uint16_t status;

    setup();
    mode = ENTRY_RESET;
    rt_claim(0U);
    pid = rt_pid[0];
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    rt_may_lose_reply = 1;
    rt_pass_limit = 4U;
    status = rt_start(0U);
    /* The reset took START's reply with it. */
    assert(status == SDK_STATUS_QUEUED || status == SDK_STATUS_OK);
    rt_may_lose_reply = 0;
    rt_pass_limit = 20000U;
    /* The app held 4 KiB of host-visible memory when reset hit; its record is
     * released and the bytes are gone with it (records are type stable, so
     * the freed one is still readable). */
    assert(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_pid(pid)->visible.used == 0U);
    assert(zzapp_rec_by_pid(pid)->claim.terminal_reason == ZZAPP_END_AMIGA_RESET);
    rt_assert_back(g_start);
    mode = ENTRY_RETURN;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* ---- START ------------------------------------------------------------------ */

/* START could not complete: the claim ends and the record is retired, with the
 * host back to IDLE; the next claim loads and runs. */
static void test_start_failure_releases_host_state(void)
{
    unsigned runs;

    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    runs = rt_entries;
    zzapp_test_set_fail_start(1);
    /* The worker publishes STARTING before run_app; its failure path must
     * still normalize the host to IDLE, or the next LOAD_BEGIN sees EBUSY. */
    assert(rt_start(0U) == SDK_STATUS_IO_ERROR);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(rt_entries == runs);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(rt_entries == runs + 1U);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A STOP before the launch is latched; the real START finds it and cancels the
 * launch, so the app never runs. */
static void test_stop_before_start_cancels_the_launch(void)
{
    unsigned before;

    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_OK);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_host_stop_requested() == 1);
    before = rt_entries;
    (void)rt_start(0U);
    assert(rt_entries == before);
    rt_settle(4U);
    /* The claim ended without the app ever running: nothing of it is left
     * but the shell, which reads IDLE. */
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(rt_entries == before + 1U);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

static int service_hook_fired;

/* The client writes the stop word after core 1's watch of the loading record
 * but before the pending START is consumed. */
static void write_stop_word_during_start(void)
{
    assert(!service_hook_fired);
    service_hook_fired = 1;
    mock_client_request_stop(&rt_cl[0]);
    zzapp_test_set_service_hook(NULL);
}

static void test_stop_word_written_before_start_is_consumed(void)
{
    unsigned before;

    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    before = rt_entries;
    service_hook_fired = 0;
    zzapp_test_set_service_hook(write_stop_word_during_start);
    assert(rt_start(0U) == SDK_STATUS_IO_ERROR);
    assert(service_hook_fired);
    assert(rt_entries == before);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* ---- How a running app ends --------------------------------------------------- */

/* A stop word the client writes itself, without a STOP op, is observed on the
 * next timer tick and the app is forced out after the normal grace. */
static void test_direct_stop_word_is_forced_after_grace(void)
{
    setup();
    mode = ENTRY_DIRECT_STOP;
    direct_stop_seen = 0U;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(4U);
    assert(direct_stop_seen == 1U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_FORCED);
    /* The forced stop is recorded as a fault the client can read. */
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_FAULT_CLASS) != AAR_FAULT_NONE);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A damaged stack canary force-stops with the documented data fault. */
static void test_damaged_canary_is_a_fault(void)
{
    setup();
    mode = ENTRY_CANARY;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(4U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_USER_FAULT);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_FAULT_CLASS) == AAR_FAULT_DATA);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
    /* The fault does not block the next claim: the host's FAULTED word of
     * the old record is that record's alone. */
    mode = ENTRY_RETURN;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(4U);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A fault leaves its record for the client; the next claim starts without
 * it (a new control block), and the app that follows ends cleanly. */
static void test_fault_does_not_follow_to_the_next_claim(void)
{
    setup();
    rt_entry = NULL;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
    zzapp_worker_hook();
    assert(RT_REASON(0U) == AAR_END_USER_FAULT);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_FAULT_CLASS) == AAR_FAULT_DATA);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
    rt_entry = app_entry;
    rt_claim(0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_FAULT_CLASS) == AAR_FAULT_NONE);
    assert(RT_REASON(0U) == AAR_END_NONE);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_FAULT_CLASS) == AAR_FAULT_NONE);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* A normal exit remains EXIT. A lease loss the firmware latched survives a
 * cooperative exit, and a client's STOP does not supply a reason. */
static void test_end_reasons_of_cooperative_exits(void)
{
    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    assert(zzapp_rec_by_pid(rt_pid[0])->claim.terminal_reason == ZZAPP_END_EXIT);
    release_all(0U, SDK_STATUS_OK);

    /* The lease ran out while the app ran; it still leaves by itself. The
     * client is presumed gone: the shell is quarantined and its handle
     * revoked, so only the client's stale request (or a reset) frees it. */
    mode = ENTRY_LEASE_EXIT;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_REASON(0U) == AAR_END_LEASE);
    assert(zzapp_rec_by_handle(rt_cl[0].handle) == NULL);
    assert(zzapp_xport_flag_test(rt_rec(0U), ZZAPP_XPORT_F_QUARANTINED));
    release_all(0U, SDK_STATUS_BAD_HANDLE);

    /* A STOP from the client is not a reason: the app that leaves is EXIT. */
    zzapp_test_set_xtime(0U);
    mode = ENTRY_STOP_EXIT;
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

/* ---- A new claim starts clean ---------------------------------------------- */

/* The heap hands back a block that held another claim's pair (the test heap
 * fills what it frees); a new claim's control block, queues and areas start
 * from zero, with only the words the platform sets at admission. */
static void test_new_claim_starts_clean(void)
{
    zzapp_xport_buf_t pair;
    volatile uint8_t *cb;
    uint32_t off;

    setup();
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    /* The client's last words: a lease tick and a dropped-event count. */
    mock_client_renew_lease(&rt_cl[0]);
    mock_client_cb_put(&rt_cl[0], AAR_CB_EVENTS_DROPPED, 5U);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);

    rt_claim(0U);
    assert(zzapp_xport_block_get(rt_rec(0U), ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
    cb = pair.ptr;
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
    assert(RT_REASON(0U) == AAR_END_NONE);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_LEASE_COUNTER) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_EVENTS_DROPPED) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_FAULT_CLASS) == AAR_FAULT_NONE);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_EXIT_CODE) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_STOP_REQUEST) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_SQ_TAIL) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_SQ_HEAD) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_CQ_TAIL) == 0U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_CQ_HEAD) == 0U);
    /* Both rings are zero: no entry of the previous claim is left. */
    for (off = AAR_CB_BYTES; off < AAR_PAIR_BYTES; off++)
        assert(cb[off] == 0U);
    release_all(0U, SDK_STATUS_OK);
    rt_assert_back(g_start);
}

int main(void)
{
    test_stale_contact_releases_expired_load();
    test_reset_releases_expired_load();
    test_administrative_release_of_expired_load();
    test_cancel_deferred_load_before_loader_begin();
    start_racing_expiry_with_release_intent();
    claim_survives_start_teardown(cancel_start_without_quarantine, 0);
    claim_survives_start_teardown(expire_and_record_intent, 0);
    claim_survives_start_teardown(expire_and_record_intent, 1);
    test_reset_with_a_load_in_flight();
    test_reset_during_a_partial_upload();
    test_reset_of_a_running_app();
    test_start_failure_releases_host_state();
    test_stop_before_start_cancels_the_launch();
    test_stop_word_written_before_start_is_consumed();
    test_direct_stop_word_is_forced_after_grace();
    test_damaged_canary_is_a_fault();
    test_fault_does_not_follow_to_the_next_claim();
    test_end_reasons_of_cooperative_exits();
    test_new_claim_starts_clean();
    puts("reset_lifecycle_test: ok");
    return 0;
}
