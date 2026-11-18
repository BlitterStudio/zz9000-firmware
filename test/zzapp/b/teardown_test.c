/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Every way a claim's app can end, through the real runtime: exit, stop,
 * fault, stall, stuck API call, lease loss (the short and the extended one),
 * Amiga reset, core 1 restart, and a stop that finds the app parked in a call.
 * Three claims run side by side: app 0 only claimed, app 1 loaded and started
 * (the one that ends), app 2 running (the neighbour). For each way:
 *   - the control block of app 1 reads IDLE with the end reason and the fault
 *     record, the client's way to learn how the app ended;
 *   - app 1's frames, address space and host-visible blocks are back, and its
 *     display is unbound, while the pair stays for the client until RELEASE
 *     (a quarantined shell, after lease loss, also keeps the app's own
 *     host-visible blocks, which a lost client may still write);
 *   - the neighbour is untouched;
 *   - RELEASE, or the stale request of a revoked shell, returns everything.
 * After an Amiga reset the shells are gone with the reset, and the records
 * keep their last words for the test to read.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);

enum teardown_path {
    TEARDOWN_EXIT,
    TEARDOWN_STOP,
    TEARDOWN_FAULT,
    TEARDOWN_STALL,
    TEARDOWN_STUCK_CALL,
    TEARDOWN_LEASE,
    TEARDOWN_ADVERTISED_LEASE,
    TEARDOWN_AMIGA_RESET,
    TEARDOWN_CORE_RESTART,
    TEARDOWN_DIRECT_STOP_PARKED,
};

/* Default note: 1 MiB image, 1 MiB stack, no private heap. */
#define DEFAULT_COMMIT RT_MB(2)
#define FRONT 0x00200000U
#define BACK  0x00220000U

static enum teardown_path path;
static uint32_t live_app1_commit;
static uint32_t live_app2_commit;
static zzapp_rec_t *app1_before;

/* The app: takes 1 MiB more of private heap, a shared block and a display,
 * then ends the way the case says. */
static void app_entry(const aar_api *api)
{
    void *old_break;
    aar_shared shared;
    aar_surface surface;
    uint32_t pid1 = rt_pid[1];

    (void)api;
    assert(zzapp_proc_heap_grow(RT_MB(1), &old_break) == AAR_OK);
    assert(old_break != NULL);
    memset(&shared, 0, sizeof(shared));
    assert(zzapp_host_shared_alloc(4096U, 256U, 0U, &shared) == AAR_OK);
    assert(shared.ptr != NULL);
    rt_cl[1].display_front = ((uint32_t)(FRONT) - MOCK_HEAP_TOKEN_ADJUST);
    rt_cl[1].display_back = ((uint32_t)(BACK) - MOCK_HEAP_TOKEN_ADJUST);
    memset(&surface, 0, sizeof(surface));
    zzapp_services_test_set_yield(mock_client_yield_hook);
    assert(zzapp_svc_display_open(320U, 200U, AAR_FMT_RGB565, 0U, &surface) == AAR_OK);
    zzapp_services_test_set_yield(NULL);
    assert(runtime_of(rt_rec(1U))->display_bound);
    live_app1_commit = rt_rec(1U)->proc.memory.committed;
    live_app2_commit = rt_rec(2U)->proc.memory.committed;
    assert(live_app1_commit == RT_MB(3));
    assert(live_app2_commit == DEFAULT_COMMIT);
    assert(rt_rec(1U)->visible.used == 4096U);

    switch (path) {
    case TEARDOWN_EXIT:
        zzapp_host_exit(0);
        break;
    case TEARDOWN_STOP:
        /* The client asks, and the app does not leave: forced after grace. */
        assert(rt_simple(1U, ZZAPP_OP_STOP) == SDK_STATUS_OK);
        zzapp_host_tick_at(1U);
        zzapp_host_tick_at(ZZAPP_STOP_GRACE_US + 1U);
        break;
    case TEARDOWN_FAULT:
        zzapp_take_app_fault(ZZAPP_FAULT_DATA, 1U, 2U, 3U, 4U, 5U, 0U);
        break;
    case TEARDOWN_STALL:
        zzapp_host_tick_at(ZZAPP_APP_STALL_US);
        break;
    case TEARDOWN_STUCK_CALL:
        zzapp_api_enter();
        zzapp_host_tick_at(1U);
        zzapp_host_tick_at(ZZAPP_API_STUCK_US + 1U);
        break;
    case TEARDOWN_LEASE:
    case TEARDOWN_ADVERTISED_LEASE: {
        uint64_t period = path == TEARDOWN_LEASE ? ZZAPP_LEASE_US :
                                                   ZZAPP_LAUNCHER_LEASE_US;

        /* The neighbour's client renews before every poll; app 1's does not. */
        zzapp_kernel_set_current_thread(-1);
        mock_client_renew_lease(&rt_cl[2]);
        zzapp_host_tick_at(period - 1U);
        zzapp_core0_poll();
        assert(!rt_rec(1U)->host.stop_requested);
        assert(!rt_rec(2U)->host.stop_requested);
        mock_client_renew_lease(&rt_cl[2]);
        zzapp_host_tick_at(period);
        zzapp_core0_poll();
        assert(rt_rec(1U)->host.stop_requested);
        assert(!rt_rec(2U)->host.stop_requested);
        zzapp_services_select_pid(pid1);
        zzapp_host_tick_at(period);
        zzapp_host_tick_at(period + ZZAPP_STOP_GRACE_US);
        break;
    }
    case TEARDOWN_AMIGA_RESET:
        zzapp_kernel_set_current_thread(-1);
        zzapp_on_amiga_reset();
        break;
    case TEARDOWN_CORE_RESTART:
        zzapp_kernel_set_current_thread(-1);
        zzapp_on_core1_restart();
        break;
    case TEARDOWN_DIRECT_STOP_PARKED:
        /* The system thread is active while app 1 waits. The client writes
         * the stop word of app 1's control block directly, without a STOP
         * op. */
        assert(zzapp_kernel_block(zzapp_runtime_kernel(), pid1) == 0);
        zzapp_kernel_set_current_thread(-1);
        zzapp_services_select_pid(rt_pid[2]);
        mock_client_request_stop(&rt_cl[1]);
        zzapp_host_tick_at(ZZAPP_TICK_US);
        zzapp_worker_hook();
        assert(rt_rec(1U)->thread.state == ZZAPP_THREAD_BLOCKED);
        zzapp_host_tick_at(ZZAPP_TICK_US + ZZAPP_STOP_GRACE_US);
        zzapp_worker_hook();
        assert(rt_rec(1U)->thread.state == ZZAPP_THREAD_STOPPED);
        break;
    }
}

static uint32_t expected_reason(enum teardown_path value)
{
    static const uint32_t reasons[] = {
        AAR_END_EXIT,
        AAR_END_FORCED,
        AAR_END_USER_FAULT,
        AAR_END_STALL,
        AAR_END_STUCK_CALL,
        AAR_END_LEASE,
        AAR_END_LEASE,
        ZZAPP_END_AMIGA_RESET,
        ZZAPP_END_CORE_RESTART,
        AAR_END_FORCED,
    };

    return reasons[value];
}

/* The record of app 1 holds nothing of the app any more. */
static void assert_app1_reclaimed(zzapp_rec_t *rec, enum teardown_path value)
{
    assert(rec->proc.memory.committed == 0U);
    assert(rec->proc.memory.frame_count == 0U);
    assert(rec->proc.as.active == 0U);
    assert(rec->proc.as.l1 == NULL);
    /* The display is unbound and its grants are gone. */
    assert(!runtime_of(rec)->display_bound);
    assert(runtime_of(rec)->display_alias[0] == 0U &&
           runtime_of(rec)->display_grant_bytes[0] == 0U);
    /* The app's own host-visible block goes with the app, except in a
     * quarantined shell, whose client may still write through its mapping. */
    if (value == TEARDOWN_LEASE || value == TEARDOWN_ADVERTISED_LEASE)
        assert(rec->visible.used == 4096U);
    else
        assert(rec->visible.used == 0U);
}

/* What the client reads from app 1's control block. */
static void assert_app1_status(uint32_t reason)
{
    assert(RT_STATE(1U) == AAR_STATE_IDLE);
    assert(RT_REASON(1U) == reason);
    if (reason == AAR_END_USER_FAULT) {
        /* The fault record is valid for the client. */
        assert(mock_client_cb_get(&rt_cl[1], AAR_CB_FAULT_CLASS) == AAR_FAULT_DATA);
        assert(mock_client_cb_get(&rt_cl[1], AAR_CB_FAULT_PC_LO) == 1U);
        assert(mock_client_cb_get(&rt_cl[1], AAR_CB_FAULT_SP_LO) == 2U);
        assert(mock_client_cb_get(&rt_cl[1], AAR_CB_FAULT_LR_LO) == 3U);
        assert(mock_client_cb_get(&rt_cl[1], AAR_CB_FAULT_ADDR_LO) == 4U);
        assert(mock_client_cb_get(&rt_cl[1], AAR_CB_FAULT_SYNDROME) == 5U);
    }
    if (reason == AAR_END_EXIT)
        assert(RT_EXIT(1U) == 0U);
}

static void assert_app2_unchanged(void)
{
    zzapp_rec_t *rec = rt_rec(2U);

    assert(rec);
    assert(RT_STATE(2U) == AAR_STATE_RUNNING);
    assert(RT_REASON(2U) == AAR_END_NONE);
    assert(rec->proc.memory.committed == live_app2_commit);
    assert(rec->thread.state != ZZAPP_THREAD_STOPPED);
    assert(!rec->host.stop_requested);
    assert(mock_client_cb_get(&rt_cl[2], AAR_CB_STOP_REQUEST) == 0U);
}

static void run_case(enum teardown_path value)
{
    struct rt_counts start;
    uint32_t pid1;
    int ended_by_reset = value == TEARDOWN_AMIGA_RESET;
    uint32_t old_handle[3];
    unsigned i;

    rt_boot(RT_MB(512), 8U);
    start = rt_counts();
    path = value;
    live_app1_commit = live_app2_commit = 0U;
    if (value == TEARDOWN_ADVERTISED_LEASE)
        rt_client_caps = ZZAPP_CLIENT_LEASE_EXTENDED;

    for (i = 0U; i < 3U; i++)
        rt_claim(i);
    for (i = 0U; i < 3U; i++)
        old_handle[i] = rt_cl[i].handle;
    pid1 = rt_pid[1];
    assert(rt_load(1U, 0U) == SDK_STATUS_OK);
    assert(rt_load(2U, 0U) == SDK_STATUS_OK);
    /* The neighbour runs: its thread is runnable and nothing ends it. */
    assert(rt_start(2U) == SDK_STATUS_OK);
    assert(RT_STATE(2U) == AAR_STATE_RUNNING);
    assert(rt_rec(2U)->proc.memory.committed == DEFAULT_COMMIT);
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
    app1_before = rt_rec(1U);

    rt_entry = app_entry;
    if (ended_by_reset || value == TEARDOWN_CORE_RESTART) {
        /* The reset and the restart take the START reply with them. */
        rt_may_lose_reply = 1;
        rt_pass_limit = 4U;
    }
    {
        uint16_t status = rt_start(1U);

        assert(status == SDK_STATUS_OK ||
               (status == SDK_STATUS_QUEUED && rt_may_lose_reply));
    }
    rt_entry = NULL;
    rt_pass_limit = 20000U;
    rt_may_lose_reply = 0;
    assert(live_app1_commit == RT_MB(3));
    rt_settle(6U);

    if (ended_by_reset) {
        /* The reset ended every record and released every shell: the old
         * handles are stale and nothing is held. The records keep their last
         * words (they are type stable) for the test to read. */
        for (i = 0U; i < 3U; i++)
            assert(zzapp_rec_by_handle(old_handle[i]) == NULL);
        assert(app1_before == zzapp_rec_by_pid(pid1));
        assert(app1_before->claim.terminal_reason == ZZAPP_END_AMIGA_RESET);
        assert_app1_reclaimed(app1_before, value);
        assert(zzapp_rec_by_pid(rt_pid[2])->claim.terminal_reason ==
               ZZAPP_END_AMIGA_RESET);
        assert(zzapp_rec_by_pid(rt_pid[0])->claim.terminal_reason ==
               ZZAPP_END_AMIGA_RESET);
        rt_assert_back(start);
        return;
    }

    {
        zzapp_rec_t *shell = zzapp_rec_by_pid(pid1);

        assert(shell == app1_before);
        assert(zzapp_rec_state(pid1) == ZZAPP_REC_RECLAIMED);
        assert_app1_reclaimed(shell, value);
        assert_app1_status(expected_reason(value));
        /* The shell keeps its pair for the client to drain and read. */
        assert(zzapp_xport_live(shell));
        assert(mock_heap.live >= 3U);
    }
    if (value == TEARDOWN_CORE_RESTART) {
        /* The restart ended the neighbour and the claimed app too: no
         * client is left running, every shell is read from its block. */
        for (i = 0U; i < 3U; i++) {
            assert(RT_STATE(i) == AAR_STATE_IDLE);
            assert(RT_REASON(i) == ZZAPP_END_CORE_RESTART);
            assert(rt_rec(i)->proc.memory.committed == 0U);
        }
    } else {
        assert_app2_unchanged();
        /* Replaying a core 1 restart over the settled records is harmless. */
        zzapp_on_core1_restart();
        assert(RT_STATE(1U) == AAR_STATE_IDLE);
        assert(RT_REASON(1U) == expected_reason(value));
    }

    /* The clients let go. A revoked shell (lease loss) is released by its old
     * client's next, stale, request; the others by RELEASE. */
    for (i = 0U; i < 3U; i++) {
        uint16_t want = (i == 1U && (value == TEARDOWN_LEASE ||
                                     value == TEARDOWN_ADVERTISED_LEASE)) ?
                        SDK_STATUS_BAD_HANDLE : SDK_STATUS_OK;

        if (i == 1U && want == SDK_STATUS_BAD_HANDLE)
            assert(zzapp_rec_by_handle(old_handle[1]) == NULL);
        assert(rt_simple(i, ZZAPP_OP_RELEASE) == want);
    }
    rt_settle(6U);
    rt_assert_back(start);
    for (i = 0U; i < 3U; i++)
        assert(zzapp_rec_by_handle(old_handle[i]) == NULL);
}

static void test_abort_republishes_service_state(void)
{
    struct rt_counts start;
    uint32_t pid;
    uint32_t handle;

    rt_boot(RT_MB(512), 8U);
    start = rt_counts();
    rt_claim(0U);
    pid = rt_pid[0];
    handle = rt_cl[0].handle;
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    assert(RT_STATE(0U) == AAR_STATE_LOADING);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(pid);
    assert(zzapp_loader_abort() == AAR_OK);
    /* The loader's own state republishes at once; the claim itself ends only
     * when its record is retired (the control block reads as before until
     * then). */
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(RT_STATE(0U) == AAR_STATE_LOADING);
    assert(zzapp_rec_retire(pid) == 0);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(zzapp_rec_by_handle(handle) == NULL);
    rt_assert_back(start);
    /* The next claim loads again. */
    rt_claim(0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(4U);
    rt_assert_back(start);
}

static void test_stopped_load_aborts_when_another_app_is_selected(void)
{
    struct rt_counts start;
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;

    rt_boot(RT_MB(512), 8U);
    start = rt_counts();
    rt_claim(0U);
    rt_claim(1U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    /* App 1's LOAD_BEGIN is posted and not yet run. */
    zzapp_build_load_begin_request(req, rt_cl[1].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), reply,
                                  &bytes, 1, 99U, 0U) == SDK_STATUS_QUEUED);
    /* Its client stops app 0 while app 1 is the one selected. */
    mock_client_request_stop(&rt_cl[0]);
    zzapp_host_latch_stop(rt_pid[0]);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[1]);
    zzapp_worker_hook();
    zzapp_worker_hook();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    /* App 1's own load was not taken with it. */
    assert(zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL));
    zzapp_mailbox_harvest_ack();
    assert(RT_STATE(1U) == AAR_STATE_LOADING);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(rt_simple(1U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(6U);
    rt_assert_back(start);
}

static uint32_t list_cpu(uint32_t pid)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

    assert(mock_client_list(&rt_probe, pid, reply) == SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_LIST_INDEX) == pid);
    return zzapp_get_be32(reply + ZZAPP_LIST_CPU_MS);
}

static void test_list_cpu_starts_at_new_claim(void)
{
    uint32_t pid;
    unsigned tries;

    rt_boot(RT_MB(512), 8U);
    rt_claim(0U);
    pid = rt_pid[0];
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    /* A claim that ran: the ticks of the live record are what LIST shows. */
    rt_rec(0U)->thread.user_ticks = 17U;
    rt_rec(0U)->thread.svc_ticks = 23U;
    assert(list_cpu(pid) == 40U);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(zzapp_rec_by_handle(rt_cl[0].handle) == NULL);
    /* The next claim of the same record is a new use of it: its CPU time
     * starts at zero. Claim and release until the arena hands it out again. */
    rt_claim(0U);
    for (tries = 0U; rt_pid[0] != pid && tries < 16U; tries++) {
        assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
        rt_settle(2U);
        rt_claim(0U);
    }
    assert(rt_pid[0] == pid);
    assert(list_cpu(pid) == 0U);
    assert(rt_load(0U, 1U) == SDK_STATUS_OK);
    assert(list_cpu(pid) == 0U);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(6U);
}

static void test_core_restart_aborts_unstarted_load(void)
{
    struct rt_counts start;
    uint32_t card;

    rt_boot(RT_MB(512), 8U);
    start = rt_counts();
    rt_claim(0U);
    rt_claim(1U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    card = zzapp_test_arena_card();
    assert(card != 0U);
    zzapp_services_select_pid(rt_pid[1]);
    zzapp_services_set_state(ZZAPP_ST_RUNNING);
    zzapp_test_mark_running();
    zzapp_on_core1_restart();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == ZZAPP_END_CORE_RESTART);
    assert(rt_rec(0U)->proc.memory.committed == 0U);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    /* The restart ended both claims; the clients release the shells and the
     * next claim loads again. */
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(rt_simple(1U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(4U);
    rt_assert_back(start);
    rt_claim(0U);
    assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) == SDK_STATUS_OK);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() != 0U);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_settle(6U);
    rt_assert_back(start);
}

int main(void)
{
    enum teardown_path value;

    for (value = TEARDOWN_EXIT; value <= TEARDOWN_DIRECT_STOP_PARKED; value++)
        run_case(value);
    test_abort_republishes_service_state();
    test_stopped_load_aborts_when_another_app_is_selected();
    test_core_restart_aborts_unstarted_load();
    test_list_cpu_starts_at_new_claim();
    puts("teardown_test: ok");
    return 0;
}
