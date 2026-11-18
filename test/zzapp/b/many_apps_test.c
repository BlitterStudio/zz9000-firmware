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
#include "host_card_pool.h"
#include "aar_note_image.h"
#include "mock_client.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
void zzapp_host_exit(int code);

#define IMAGE_BYTES 0x200U
#define LOAD_OFF 0x100U
#define MB(n) ((uint32_t)(n) << 20)
#define MAX_APPS 64U
static uint32_t handles[MAX_APPS];
static uint32_t request_id;
static jmp_buf entered;
static unsigned entries;
static int consume_tables;
static uint8_t last_reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
static uint16_t last_reply_len;

static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
}
static void put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static void image_make(uint8_t image[IMAGE_BYTES], uint32_t heap)
{
    uint8_t *ph;
    memset(image, 0, IMAGE_BYTES);
    memcpy(image, "\177ELF\1\1\1", 7);
    put16(image + 16, 3); put16(image + 18, 40);
    put32(image + 24, LOAD_OFF); put32(image + 28, 52);
    put32(image + 32, 0x180); put32(image + 36, 0x400);
    put16(image + 40, 52); put16(image + 42, 32);
    put16(image + 44, 2); put16(image + 46, 40); put16(image + 48, 1);
    ph = image + 52;
    put32(ph, 1); put32(ph + 4, LOAD_OFF); put32(ph + 8, LOAD_OFF);
    put32(ph + 16, 4); put32(ph + 20, 8); put32(ph + 24, 1);
    ph += 32;
    put32(ph, 4); put32(ph + 4, TEST_NOTE_OFF);
    put32(ph + 16, TEST_NOTE_BYTES);
    test_note_init(image + TEST_NOTE_OFF);
    test_note_set(image + TEST_NOTE_OFF, TEST_NOTE_W_MIN_HEAP_MIB, heap);
    put32(image + 0xd0, LOAD_OFF); put32(image + 0xd4, 23);
    put32(image + 0xd8, LOAD_OFF);
    put32(image + 0x184, 4); put32(image + 0x190, 0xd0);
    put32(image + 0x194, 12); put32(image + 0x1a4, 12);
}

/* Returning from the injected entry means exit in the host harness. Jumping
 * back instead models the CPU returning to its system thread with the app
 * still runnable. START preparation and publication have already run. */
static void app_entry(const void *api, int argc, char **argv)
{
    assert(api && argc == 0 && argv);
    entries++;
    longjmp(entered, 1);
}

static void fill_tables(uint32_t handle)
{
    zzapp_rec_t *rec = zzapp_rec_by_handle(handle);
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

/* One mailbox op: dispatched from core 0 and, when deferred, run by core 1
 * until core 0 harvests its completion. The reply payload is kept. */
static uint16_t dispatch(uint16_t op, uint32_t handle, uint8_t *req)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0, status;
    unsigned pass;
    zzapp_services_test_set_core(0);
    status = zzapp_mailbox_dispatch(op, req, ZZAPP_MAILBOX_PAYLOAD_BYTES,
                                    reply, &bytes, 1, ++request_id, 0);
    if (status != SDK_STATUS_QUEUED) {
        memcpy(last_reply, reply, sizeof(reply));
        last_reply_len = bytes;
        return status;
    }
    for (pass = 0; pass < 20000; pass++) {
        zzapp_services_test_set_core(1);
        if (setjmp(entered) == 0)
            zzapp_worker_hook();
        zzapp_kernel_set_current_thread(-1);
        zzapp_services_test_set_core(0);
        bytes = 0;
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, last_reply, &bytes)) {
            zzapp_mailbox_harvest_ack();
            last_reply_len = bytes;
            return status;
        }
        if (consume_tables && zzapp_rec_by_handle(handle) &&
            zzapp_rec_by_handle(handle)->proc.as.active)
            fill_tables(handle);
    }
    assert(0 && "deferred request did not finish");
    return SDK_STATUS_IO_ERROR;
}

static uint16_t simple(uint16_t op, uint32_t handle)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = {0};
    zzapp_build_handle(req, handle);
    return dispatch(op, handle, req);
}

struct counts {
    uint32_t free_records, tables_free, tables_total, commit_free, commit_total;
    uint32_t heap_blocks, heap_bytes, pool_used, l2_free, l2_reserved;
};
static struct counts counts(void)
{
    struct counts c;
    zzapp_rec_stats_t r;
    zzapp_as_stats_t a;
    uint8_t reply[48];
    uint16_t bytes = 0;
    uint32_t pid, live = 0, committed = 0;
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0, reply,
                                  &bytes, 1, 0, 0) == SDK_STATUS_OK);
    assert(bytes == ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_rec_stats(&r); zzapp_as_stats(&a);
    c.free_records = zzapp_get_be32(reply + ZZAPP_QUERY_RECORDS_FREE);
    c.tables_free = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_FREE);
    c.tables_total = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_TOTAL);
    c.commit_free = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_FREE);
    c.commit_total = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_TOTAL);
    c.heap_blocks = mock_heap.live;
    c.heap_bytes = mock_heap.live_bytes;
    c.pool_used = host_card_pool_used();
    c.l2_free = a.l2_free; c.l2_reserved = a.l2_reserved;
    for (pid = 0; pid < zzapp_rec_capacity(); pid++) {
        zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
        if (rec && zzapp_rec_state(pid) != ZZAPP_REC_FREE) {
            live++;
            committed += rec->proc.memory.committed;
        }
    }
    assert(c.free_records == r.free && r.free + live == r.capacity);
    assert(c.tables_free == a.l2_total - a.l2_template - a.l2_reserved);
    assert(c.tables_total == a.l2_total - a.l2_template);
    assert(c.commit_total == c.commit_free + committed);
    assert(c.commit_free == zzapp_loader_pool_free());
    return c;
}
static void boot(uint32_t bytes)
{
    zzapp_xport_env_t env;
    host_card_pool_reset(bytes);
    zzapp_test_set_arena_apps(MAX_APPS);
    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    env = mock_heap_env();
    zzapp_xport_init(&env);
    zzapp_test_set_entry(app_entry);
    zzapp_test_set_xtime(0);
    request_id = entries = 0;
}
static void claim(uint32_t n)
{
    uint8_t reply[48];
    uint16_t bytes = 0;
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_CLAIM, NULL, 0, reply, &bytes, 1,
                                  ++request_id, 0) == SDK_STATUS_OK);
    assert(bytes == ZZAPP_CLAIM_REPLY_BYTES);
    handles[n] = zzapp_get_be32(reply + ZZAPP_CLAIM_HANDLE);
    assert(zzapp_rec_by_handle(handles[n]) != NULL);
}
/* LOAD_BEGIN, the image in the load buffer the reply names, LOAD_CHUNK and
 * LOAD_END; the status of the last op that ran. */
static uint16_t load(uint32_t n, uint32_t heap)
{
    uint8_t image[IMAGE_BYTES], req[48] = {0};
    uint16_t status;
    uint8_t *buf;
    zzapp_build_load_begin_request(req, handles[n], 0);
    status = dispatch(ZZAPP_OP_LOAD_BEGIN, handles[n], req);
    if (status != SDK_STATUS_OK)
        return status;
    assert(last_reply_len == ZZAPP_LOAD_BEGIN_REPLY_BYTES);
    buf = mock_heap_map(zzapp_get_be32(last_reply + ZZAPP_LOAD_REPLY_TOKEN),
                        ZZAPP_LOAD_BUFFER_BYTES);
    assert(buf);
    image_make(image, heap);
    memcpy(buf, image, sizeof(image));
    memset(req, 0, sizeof(req));
    zzapp_build_load_chunk_request(req, handles[n], 0, sizeof(image));
    assert(dispatch(ZZAPP_OP_LOAD_CHUNK, handles[n], req) == SDK_STATUS_OK);
    return simple(ZZAPP_OP_LOAD_END, handles[n]);
}
static void all_started(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t req[48] = {0};
        zzapp_rec_t *rec;
        claim(i);
        assert(load(i, 1) == SDK_STATUS_OK);
        zzapp_build_start_app_request(req, handles[i], 0);
        assert(dispatch(ZZAPP_OP_START, handles[i], req) == SDK_STATUS_OK);
        rec = zzapp_rec_by_handle(handles[i]);
        assert(rec && rec->coh.running);
        assert(rec->thread.state == ZZAPP_THREAD_RUNNABLE);
        assert(zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_RUNNING);
        (void)counts();
    }
    assert(entries == n);
    assert(counts().free_records == MAX_APPS - n);
}
/* The claims are over: every record, table, commit and block is back. */
static void released(struct counts start, uint32_t n)
{
    struct counts end = counts();
    assert(end.free_records == start.free_records);
    assert(end.tables_free == start.tables_free && end.l2_free == start.l2_free);
    assert(end.l2_reserved == start.l2_reserved);
    assert(end.commit_free == start.commit_free && end.commit_total == start.commit_total);
    assert(end.pool_used == start.pool_used);
    assert(end.heap_blocks == start.heap_blocks && end.heap_bytes == start.heap_bytes);
    for (uint32_t i = 0; i < n; i++)
        assert(zzapp_rec_by_handle(handles[i]) == NULL);
}
/* mode 0: the client stops each app; 1: each app exits; 2: an Amiga reset. */
static void many(uint32_t n, int mode)
{
    struct counts start;
    boot(MB(512)); start = counts();
    all_started(n);
    zzapp_test_set_xtime(1);
    if (mode == 2) {
        zzapp_on_amiga_reset();
    } else {
        for (uint32_t i = 0; i < n; i++) {
            zzapp_rec_t *rec = zzapp_rec_by_handle(handles[i]);
            assert(rec);
            zzapp_services_select_pid(rec->hdr.pid);
            zzapp_services_test_set_core(1);
            if (mode == 0) {
                assert(simple(ZZAPP_OP_STOP, handles[i]) == SDK_STATUS_OK);
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
            zzapp_rec_t *rec = zzapp_rec_by_handle(handles[i]);
            assert(rec);
            assert(zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_IDLE);
            assert(zzapp_xport_cb_get(rec, AAR_CB_END_REASON) ==
                   (mode == 0 ? AAR_END_FORCED : AAR_END_EXIT));
        }
        assert(counts().free_records == start.free_records - n);
        for (uint32_t i = 0; i < n; i++)
            assert(simple(ZZAPP_OP_RELEASE, handles[i]) == SDK_STATUS_OK);
    }
    released(start, n);
    zzapp_on_amiga_reset();
}
static void quarantine(void)
{
    struct counts start;
    boot(MB(512)); start = counts();
    claim(0); assert(load(0, 1) == SDK_STATUS_OK);
    zzapp_services_test_set_core(0);
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US + 1);
    zzapp_services_test_set_core(1);
    zzapp_worker_hook();
    {
        zzapp_rec_t *shell = zzapp_rec_by_pid(aar_handle_index(handles[0]));
        assert(shell && zzapp_xport_flag_test(shell, ZZAPP_XPORT_F_QUARANTINED));
        /* The old handle is revoked: the shell has a new one. */
        assert(zzapp_rec_by_handle(handles[0]) == NULL);
        assert(zzapp_xport_cb_get(shell, AAR_CB_END_REASON) == AAR_END_QUARANTINED ||
               zzapp_xport_cb_get(shell, AAR_CB_END_REASON) == AAR_END_LEASE);
    }
    assert(counts().free_records == MAX_APPS - 1);
    {
        struct counts reclaimed = counts();
        assert(reclaimed.tables_free == start.tables_free);
        assert(reclaimed.l2_free == start.l2_free && reclaimed.l2_reserved == 0);
        assert(reclaimed.commit_free == start.commit_free);
        assert(reclaimed.pool_used == start.pool_used);
        /* The shell keeps its pair (and any service block) until released. */
        assert(reclaimed.heap_blocks >= start.heap_blocks + 1);
    }
    /* The client's next contact with its old handle is stale and releases it. */
    assert(simple(ZZAPP_OP_RELEASE, handles[0]) == SDK_STATUS_BAD_HANDLE);
    released(start, 1);
    zzapp_on_amiga_reset();
}
static void refusal(int tables)
{
    struct counts start;
    boot(tables ? MB(512) : MB(128 + 16 + 3));
    if (tables) {
        uint32_t bsp[4096];
        memcpy(bsp, zzapp_as_template(), sizeof(bsp));
        assert(zzapp_as_pool_init((void *)(uintptr_t)zzapp_rec_arena_base(),
                                  zzapp_as_pool_bytes(MAX_APPS, 32), MAX_APPS, 32) == 0);
        zzapp_as_init(bsp);
    }
    start = counts();
    claim(0); consume_tables = tables;
    assert(load(0, tables ? 1 : 2) == SDK_STATUS_NO_MEMORY);
    /* The refusal is the reply of the failing op: the reason alone. */
    assert(last_reply_len == ZZAPP_REFUSAL_REPLY_BYTES);
    assert(zzapp_get_be32(last_reply + ZZAPP_REFUSAL_REASON) ==
           (tables ? AAR_REASON_TABLES : AAR_REASON_MEMORY));
    assert(!consume_tables);
    assert(simple(ZZAPP_OP_RELEASE, handles[0]) == SDK_STATUS_OK);
    released(start, 1);
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
