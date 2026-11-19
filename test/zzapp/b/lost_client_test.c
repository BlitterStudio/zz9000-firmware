/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A client that disappears, through the real runtime. The claim's control
 * block, queues and buffers outlive the app and live until the client
 * releases the claim, because a client that lost its lease may still write
 * through its mapping:
 *   - the lease runs out (a running app, a claim that never loaded);
 *   - the app ends, the shell is quarantined under a new handle, and the
 *     app's own host-visible blocks are kept;
 *   - the old client's next request is stale and releases the shell, which
 *     returns every block; so does an explicit RELEASE under the handle LIST
 *     reports, and so does an Amiga reset;
 *   - a live neighbour is not disturbed by any of it.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

static struct rt_counts g_start;

/* The app takes a host-visible block and runs on. */
static void app_entry(const aar_api *api)
{
    aar_shared shared;

    memset(&shared, 0, sizeof(shared));
    assert(api->shared_alloc(4096u, 256u, 0u, &shared) == AAR_OK);
    longjmp(rt_entered, 1);
}

/* RELEASE of a running app: the stop is latched, and the app is forced out
 * after the grace on the following ticks. */
static void release_running(uint32_t n)
{
    static uint64_t when = 20000000U;

    assert(rt_simple(n, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    zzapp_kernel_set_current_thread(-1);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[n]);
    zzapp_host_tick_at(when);
    zzapp_host_tick_at(when + ZZAPP_STOP_GRACE_US);
    when += 10000000U;
    rt_settle(8U);
}

static void setup(void)
{
    rt_boot(RT_MB(256), 8U);
    g_start = rt_counts();
    rt_entry = app_entry;
}

/* Client `n` runs an app that holds a host-visible block. */
static void run_app(uint32_t n)
{
    rt_claim(n);
    assert(rt_load(n, 1U) == SDK_STATUS_OK);
    assert(rt_start(n) == SDK_STATUS_OK);
    assert(RT_STATE(n) == AAR_STATE_RUNNING);
    assert(rt_rec(n)->visible.used == 4096U);
}

/* The lease of app `lost` runs out while the client of `live` keeps renewing.
 * The app is stopped after the grace and its claim ends. */
static void lose_the_lease(uint32_t lost, int live)
{
    mock_client_renew_lease(&rt_cl[live]);
    zzapp_kernel_set_current_thread(-1);
    zzapp_host_tick_at(ZZAPP_LEASE_US - 1U);
    zzapp_core0_poll();
    assert(!rt_rec(lost)->host.stop_requested);
    mock_client_renew_lease(&rt_cl[live]);
    zzapp_host_tick_at(ZZAPP_LEASE_US);
    zzapp_core0_poll();
    assert(rt_rec(lost)->host.stop_requested);
    assert(!rt_rec(live)->host.stop_requested);
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[lost]);
    zzapp_host_tick_at(ZZAPP_LEASE_US);
    zzapp_host_tick_at(ZZAPP_LEASE_US + ZZAPP_STOP_GRACE_US);
    rt_settle(6U);
}

/* The lease lost: the shell is quarantined under a new handle, keeps the
 * app's block and reads IDLE with the reason; the neighbour runs on. */
static uint32_t assert_quarantined(uint32_t lost, uint32_t old_handle)
{
    zzapp_rec_t *shell = rt_rec(lost);
    uint32_t current;

    assert(zzapp_rec_state(rt_pid[lost]) == ZZAPP_REC_RECLAIMED);
    assert(zzapp_xport_flag_test(shell, ZZAPP_XPORT_F_QUARANTINED));
    assert(RT_STATE(lost) == AAR_STATE_IDLE);
    assert(RT_REASON(lost) == AAR_END_LEASE);
    assert(zzapp_rec_by_handle(old_handle) == NULL);
    current = rt_listed_handle(rt_pid[lost]);
    assert(current != 0U && current != old_handle);
    /* The app's own resources went, except the block the lost client could
     * still write through. */
    assert(shell->proc.memory.committed == 0U);
    assert(shell->visible.used == 4096U);
    return current;
}

/* The old client's next request, with the handle it knows, is stale and
 * releases the shell and every block of it. */
static void test_stale_request_releases_a_lost_app(void)
{
    uint32_t old_handle;

    setup();
    run_app(0U);
    run_app(1U);
    old_handle = rt_cl[0].handle;
    lose_the_lease(0U, 1);
    assert_quarantined(0U, old_handle);
    assert(RT_STATE(1U) == AAR_STATE_RUNNING);
    assert(RT_REASON(1U) == AAR_END_NONE);
    /* A doorbell is posted without its status being read: a stale KICK tells
     * the client nothing and leaves the shell. */
    assert(rt_simple(0U, ZZAPP_OP_KICK) == SDK_STATUS_BAD_HANDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_RECLAIMED);
    assert(zzapp_xport_flag_test(rt_rec(0U), ZZAPP_XPORT_F_QUARANTINED));
    assert(rt_rec(0U)->visible.used == 4096U);
    /* A request the client is answered releases the shell and every block. */
    assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_BAD_HANDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_pid(rt_pid[0])->visible.used == 0U);
    /* The neighbour is untouched and goes at its own release. */
    assert(RT_STATE(1U) == AAR_STATE_RUNNING);
    release_running(1U);
    rt_settle(8U);
    rt_assert_back(g_start);
}

/* An administrator releases the shell under the handle LIST reports; the
 * handle the client knows stays stale. */
static void test_release_of_a_quarantined_handle(void)
{
    uint32_t old_handle, current;

    setup();
    run_app(0U);
    run_app(1U);
    old_handle = rt_cl[0].handle;
    lose_the_lease(0U, 1);
    current = assert_quarantined(0U, old_handle);
    rt_cl[0].handle = current;
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_pid(rt_pid[0])->visible.used == 0U);
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_BAD_HANDLE);
    rt_cl[0].handle = old_handle;
    assert(rt_simple(0U, ZZAPP_OP_RELEASE) == SDK_STATUS_BAD_HANDLE);
    release_running(1U);
    rt_settle(8U);
    rt_assert_back(g_start);
}

/* A claim that never loaded loses its 30 s lease: no app to stop, the claim
 * ends at once and the shell waits for its client. */
static void test_a_claim_that_never_loaded_is_quarantined(void)
{
    uint32_t old_handle;

    setup();
    rt_claim(0U);
    old_handle = rt_cl[0].handle;
    zzapp_services_test_set_core(0);
    zzapp_services_expire_claims(ZZAPP_CLAIM_LEASE_US - 1U);
    assert(zzapp_rec_by_handle(old_handle) != NULL);
    zzapp_services_expire_claims(ZZAPP_CLAIM_LEASE_US);
    assert(zzapp_rec_by_handle(old_handle) == NULL);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_BAD_HANDLE);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    rt_assert_back(g_start);
}

/* An Amiga reset with held shells and live apps: every record, quarantined
 * or not, is released, every block goes back before the allocator is reset
 * and every old handle stays stale. */
static void test_reset_with_held_shells(void)
{
    uint32_t handles[4];
    uint32_t n;

    setup();
    run_app(0U);
    run_app(1U);
    run_app(2U);
    rt_claim(3U);
    for (n = 0U; n < 4U; n++)
        handles[n] = rt_cl[n].handle;
    lose_the_lease(0U, 1);
    assert_quarantined(0U, handles[0]);
    /* One shell the client has not released, one lost shell, two live. */
    zzapp_on_amiga_reset();
    for (n = 0U; n < 4U; n++) {
        assert(zzapp_rec_state(rt_pid[n]) == ZZAPP_REC_FREE);
        assert(zzapp_rec_by_handle(handles[n]) == NULL);
        assert(zzapp_rec_by_pid(rt_pid[n])->visible.used == 0U);
    }
    assert(mock_heap.live == 0U);
    assert(mock_heap.bad_frees == 0U);
    rt_assert_back(g_start);
    /* The card claims and runs again. */
    run_app(0U);
    release_running(0U);
    rt_settle(8U);
    rt_assert_back(g_start);
}

int main(void)
{
    test_stale_request_releases_a_lost_app();
    test_release_of_a_quarantined_handle();
    test_a_claim_that_never_loaded_is_quarantined();
    test_reset_with_held_shells();
    puts("lost_client_test: ok");
    return 0;
}
