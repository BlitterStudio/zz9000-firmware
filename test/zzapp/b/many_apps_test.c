/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Many apps over the real host, loader, commit ledger, address spaces,
 * transport and retire path. Only execution of ARM user code is substituted.
 * There is no ceiling of launcher windows any more: the apps are bounded by
 * the record arena alone. Each client claims through the mailbox, loads
 * through the load buffer it is given, and lets go with RELEASE. */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"

#define MAX_APPS RT_MAX_APPS
#define MB RT_MB
static int consume_tables;

static void fill_tables(uint32_t n)
{
    zzapp_rec_t *rec = rt_rec(n);
    zzapp_as_stats_t before, after;
    uint32_t va;
    assert(rec && rec->proc.as.active);
    zzapp_as_stats(&before);
    /* ZZAPP_MAX_LOAD is four and complete uniform sections use L1 entries,
     * so an image alone cannot exceed an intact sixteen-table reserve.
     * Consume that reserve and the shrunken shared remainder after AS setup,
     * before image mapping, to exercise the loader's real TABLES refusal. */
    assert(zzapp_as_alias_map(&rec->proc.as, 0x10000000U,
                              before.l2_free << 20, 1, &va) == 0);
    zzapp_as_stats(&after);
    assert(after.l2_free == 0 && after.l2_reserved_unused == 0);
    consume_tables = 0;
}

static void table_hook(unsigned pass)
{
    (void)pass;
    if (consume_tables && rt_rec(0U) && rt_rec(0U)->proc.as.active)
        fill_tables(0U);
}

static void all_started(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        zzapp_rec_t *rec;
        rt_claim(i);
        assert(rt_load(i, 1) == SDK_STATUS_OK);
        assert(rt_start(i) == SDK_STATUS_OK);
        rec = rt_rec(i);
        assert(rec && rec->coh.running);
        assert(rec->thread.state == ZZAPP_THREAD_RUNNABLE);
        assert(zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_RUNNING);
        (void)rt_counts();
    }
    assert(rt_entries == n);
    assert(rt_counts().free_records == MAX_APPS - n);
}
/* mode 0: the client stops each app; 1: each app exits; 2: an Amiga reset. */
static void many(uint32_t n, int mode)
{
    struct rt_counts start;
    rt_boot(MB(512), MAX_APPS); start = rt_counts();
    all_started(n);
    zzapp_test_set_xtime(1);
    if (mode == 2) {
        zzapp_on_amiga_reset();
    } else {
        for (uint32_t i = 0; i < n; i++) {
            zzapp_rec_t *rec = rt_rec(i);
            assert(rec);
            zzapp_services_select_pid(rec->hdr.pid);
            zzapp_services_test_set_core(1);
            if (mode == 0) {
                assert(rt_simple(i, ZZAPP_OP_STOP) == SDK_STATUS_OK);
            } else {
                zzapp_kernel_set_current_thread((int)rec->hdr.pid);
                zzapp_host_exit(0);
                zzapp_kernel_set_current_thread(-1);
            }
        }
        zzapp_services_test_set_core(1);
        zzapp_worker_hook();
        zzapp_test_set_xtime(10000000);
        zzapp_worker_hook();
        /* The shells hold their records and pairs for the client, which reads
         * the end from its control block and then releases. */
        for (uint32_t i = 0; i < n; i++) {
            assert(rt_rec(i));
            assert(RT_STATE(i) == AAR_STATE_IDLE);
            assert(RT_REASON(i) == (mode == 0 ? AAR_END_FORCED : AAR_END_EXIT));
        }
        assert(rt_counts().free_records == start.free_records - n);
        for (uint32_t i = 0; i < n; i++)
            assert(rt_simple(i, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    }
    rt_assert_back(start);
    for (uint32_t i = 0; i < n; i++)
        assert(zzapp_rec_by_handle(rt_cl[i].handle) == NULL);
    zzapp_on_amiga_reset();
}
static void quarantine(void)
{
    struct rt_counts start;
    rt_boot(MB(512), MAX_APPS); start = rt_counts();
    rt_claim(0); assert(rt_load(0, 1) == SDK_STATUS_OK);
    zzapp_services_test_set_core(0);
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US + 1);
    zzapp_services_test_set_core(1);
    zzapp_worker_hook();
    {
        zzapp_rec_t *shell = zzapp_rec_by_pid(rt_pid[0]);
        assert(shell && zzapp_xport_flag_test(shell, ZZAPP_XPORT_F_QUARANTINED));
        /* The old handle is revoked: the shell has a new one. */
        assert(zzapp_rec_by_handle(rt_cl[0].handle) == NULL);
        assert(zzapp_xport_cb_get(shell, AAR_CB_END_REASON) == AAR_END_QUARANTINED ||
               zzapp_xport_cb_get(shell, AAR_CB_END_REASON) == AAR_END_LEASE);
    }
    assert(rt_counts().free_records == MAX_APPS - 1);
    {
        struct rt_counts reclaimed = rt_counts();
        assert(reclaimed.tables_free == start.tables_free);
        assert(reclaimed.l2_free == start.l2_free && reclaimed.l2_reserved == 0);
        assert(reclaimed.commit_free == start.commit_free);
        assert(reclaimed.pool_used == start.pool_used);
        /* The shell keeps its pair (and any service block) until released. */
        assert(reclaimed.heap_blocks >= start.heap_blocks + 1);
    }
    /* The client's next contact with its old handle is stale and releases it. */
    assert(rt_simple(0, ZZAPP_OP_RELEASE) == SDK_STATUS_BAD_HANDLE);
    rt_assert_back(start);
    zzapp_on_amiga_reset();
}
static void refusal(int tables)
{
    struct rt_counts start;
    rt_boot(tables ? MB(512) : MB(128 + 16 + 3), MAX_APPS);
    if (tables) {
        uint32_t bsp[4096];
        memcpy(bsp, zzapp_as_template(), sizeof(bsp));
        assert(zzapp_as_pool_init((void *)(uintptr_t)zzapp_rec_arena_base(),
                                  zzapp_as_pool_bytes(MAX_APPS, 32), MAX_APPS, 32) == 0);
        zzapp_as_init(bsp);
    }
    start = rt_counts();
    rt_claim(0); consume_tables = tables;
    rt_pass_hook = table_hook;
    assert(rt_load(0, tables ? 1 : 2) == SDK_STATUS_NO_MEMORY);
    /* The refusal is the reply of the failing op: the reason, then the
     * AAR_E_NOMEM code. */
    assert(rt_last_reply_len == ZZAPP_REFUSAL_CODE_REPLY_BYTES);
    assert(zzapp_get_be32(rt_last_reply + ZZAPP_REFUSAL_CODE) ==
           (uint32_t)-AAR_E_NOMEM);
    assert(zzapp_get_be32(rt_last_reply + ZZAPP_REFUSAL_REASON) ==
           (tables ? AAR_REASON_TABLES : AAR_REASON_MEMORY));
    assert(!consume_tables);
    assert(rt_simple(0, ZZAPP_OP_RELEASE) == SDK_STATUS_OK);
    rt_assert_back(start);
    zzapp_on_amiga_reset();
}
int main(void)
{
    for (uint32_t n = 8; n <= MAX_APPS; n *= 8)
        for (int mode = 0; mode < 3; mode++)
            many(n, mode);
    quarantine();
    refusal(0); refusal(1);
    puts("many_apps_test: ok (8/64 stop, exit, reset; quarantine; MEMORY/TABLES)");
    return 0;
}
