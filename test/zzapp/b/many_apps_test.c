/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Many launcher windows over the real host, loader, commit ledger, address
 * spaces and retire path. Only execution of ARM user code is substituted. */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "host_card_pool.h"
#include "aar_note_image.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
void zzapp_host_exit(int code);

#define IMAGE_BYTES 0x200U
#define LOAD_OFF 0x100U
#define MB(n) ((uint32_t)(n) << 20)
static uint8_t win_mem[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];
static uint32_t handles[ZZAPP_SLOT_COUNT], generations[ZZAPP_SLOT_COUNT];
static uint32_t request_id;
static jmp_buf entered;
static unsigned entries;
static int consume_tables;

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

static void request(uint8_t *req, uint32_t win)
{
    memset(req, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_put_be32(req + ZZAPP_SEL_SLOT, win);
    zzapp_put_be32(req + ZZAPP_SEL_GENERATION, generations[win]);
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

static void fill_tables(uint32_t win)
{
    zzapp_rec_t *rec = window_rec(&windows[win]);
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

static uint16_t dispatch(uint16_t op, uint32_t win, uint8_t *req)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0, status;
    unsigned pass;
    zzapp_services_test_set_core(0);
    status = zzapp_mailbox_dispatch(op, req, ZZAPP_MAILBOX_PAYLOAD_BYTES,
                                    reply, &bytes, 1, ++request_id, 0);
    if (status != SDK_STATUS_QUEUED)
        return status;
    for (pass = 0; pass < 20000; pass++) {
        zzapp_services_test_set_core(1);
        if (setjmp(entered) == 0)
            zzapp_worker_hook();
        zzapp_kernel_set_current_thread(-1);
        zzapp_services_test_set_core(0);
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes)) {
            zzapp_mailbox_harvest_ack();
            return status;
        }
        if (consume_tables && window_rec(&windows[win])->proc.as.active)
            fill_tables(win);
    }
    assert(0 && "deferred request did not finish");
    return SDK_STATUS_IO_ERROR;
}

struct counts {
    uint32_t free_records, tables_free, tables_total, commit_free, commit_total;
    uint32_t free_windows, heap_free, pool_used, l2_free, l2_reserved;
};
static struct counts counts(void)
{
    struct counts c;
    zzapp_rec_stats_t r;
    zzapp_as_stats_t a;
    uint8_t req[48] = {0}, reply[48];
    uint16_t bytes = 0;
    uint32_t win, live = 0, committed = 0, ranges = 0;
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, req, sizeof(req), reply,
                                  &bytes, 1, 0, 0) == SDK_STATUS_OK);
    assert(bytes == ZZAPP_QUERY_EXT_REPLY_BYTES);
    zzapp_rec_stats(&r); zzapp_as_stats(&a);
    c.free_records = zzapp_get_be32(reply + ZZAPP_QUERY_RECORDS_FREE);
    c.tables_free = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_FREE);
    c.tables_total = zzapp_get_be32(reply + ZZAPP_QUERY_TABLES_TOTAL);
    c.commit_free = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_FREE);
    c.commit_total = zzapp_get_be32(reply + ZZAPP_QUERY_COMMIT_TOTAL);
    c.free_windows = zzapp_get_be32(reply + ZZAPP_QUERY_FREE_SLOTS);
    c.heap_free = zzapp_get_be32(reply + ZZAPP_QUERY_HOSTHEAP_FREE);
    c.pool_used = host_card_pool_used();
    c.l2_free = a.l2_free; c.l2_reserved = a.l2_reserved;
    for (win = 0; win < ZZAPP_SLOT_COUNT; win++) {
        zzapp_rec_t *rec = window_rec(&windows[win]);
        if (rec) {
            assert(zzapp_rec_by_handle(windows[win].handle) == rec);
            live++;
            committed += rec->proc.memory.committed;
        } else {
            assert(windows[win].handle == 0);
        }
        ranges += windows[win].test_range_held != 0;
    }
    assert(c.free_records == r.free && r.free + live == r.capacity);
    assert(c.tables_free == a.l2_total - a.l2_template - a.l2_reserved);
    assert(c.tables_total == a.l2_total - a.l2_template);
    assert(c.commit_total == c.commit_free + committed);
    assert(c.commit_free == zzapp_loader_pool_free());
    assert(c.heap_free == (ZZAPP_SLOT_COUNT - ranges) * ZZAPP_WINDOW_BYTES);
    return c;
}
static void boot(uint32_t bytes)
{
    host_card_pool_reset(bytes);
    zzapp_test_set_arena_apps(64);
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_test_set_entry(app_entry);
    zzapp_test_set_xtime(0);
    request_id = entries = 0;
    for (uint32_t win = 0; win < ZZAPP_SLOT_COUNT; win++)
        zzapp_services_test_set_window_pool(win, win_mem[win]);
}
static void claim(uint32_t win)
{
    uint8_t req[48] = {0}, reply[48];
    uint16_t bytes = 0;
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, req, sizeof(req),
                                  reply, &bytes, 1, ++request_id, 0) == SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) == win);
    generations[win] = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
    handles[win] = windows[win].handle;
}
static uint16_t load(uint32_t win, uint32_t heap)
{
    uint8_t image[IMAGE_BYTES], req[48];
    image_make(image, heap);
    request(req, win);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, win, req) == SDK_STATUS_OK);
    memcpy(win_mem[win] + ZZAPP_STAGING_OFF, image, sizeof(image));
    request(req, win);
    zzapp_put_be32(req + ZZAPP_CHUNK_LENGTH, sizeof(image));
    zzapp_put_be32(req + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF);
    assert(dispatch(ZZAPP_OP_LOAD_CHUNK, win, req) == SDK_STATUS_OK);
    request(req, win);
    return dispatch(ZZAPP_OP_LOAD_END, win, req);
}
static void all_started(uint32_t n)
{
    for (uint32_t win = 0; win < n; win++) {
        uint8_t req[48];
        claim(win);
        assert(load(win, 1) == SDK_STATUS_OK);
        request(req, win);
        assert(dispatch(ZZAPP_OP_START, win, req) == SDK_STATUS_OK);
        assert(window_rec(&windows[win])->coh.running);
        assert(window_rec(&windows[win])->thread.state == ZZAPP_THREAD_RUNNABLE);
        (void)counts();
    }
    assert(entries == n);
    assert(counts().free_records == 64 - n);
}
static void released(struct counts start, uint32_t n, int reset)
{
    struct counts end = counts();
    assert(end.free_records == start.free_records);
    assert(end.tables_free == start.tables_free && end.l2_free == start.l2_free);
    assert(end.l2_reserved == start.l2_reserved);
    assert(end.commit_free == start.commit_free && end.commit_total == start.commit_total);
    assert(end.pool_used == start.pool_used);
    assert(end.free_windows == ZZAPP_SLOT_COUNT);
    assert(end.heap_free == start.heap_free - (reset ? 0 : n * ZZAPP_WINDOW_BYTES));
    for (uint32_t win = 0; win < n; win++)
        assert(zzapp_rec_by_handle(handles[win]) == NULL);
}
static void many(uint32_t n, int mode)
{
    struct counts start;
    boot(MB(512)); start = counts();
    all_started(n);
    zzapp_test_set_xtime(1);
    if (mode == 2) {
        zzapp_on_amiga_reset();
    } else {
        for (uint32_t win = 0; win < n; win++) {
            zzapp_services_select_pid(window_rec(&windows[win])->hdr.pid);
            zzapp_services_test_set_core(1);
            if (mode == 0) {
                uint8_t req[48];
                request(req, win);
                assert(dispatch(ZZAPP_OP_STOP, win, req) == SDK_STATUS_OK);
            } else {
                zzapp_kernel_set_current_thread((int)window_rec(&windows[win])->hdr.pid);
                zzapp_host_exit(0);
                zzapp_kernel_set_current_thread(-1);
            }
        }
        zzapp_services_test_set_core(1);
        zzapp_worker_hook();
        zzapp_test_set_xtime(10000000);
        zzapp_worker_hook();
    }
    released(start, n, mode == 2);
    zzapp_on_amiga_reset();
}
static void quarantine(void)
{
    struct counts start;
    uint8_t req[48];
    boot(MB(512)); start = counts();
    claim(0); assert(load(0, 1) == SDK_STATUS_OK);
    zzapp_services_test_set_core(0);
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US + 1);
    zzapp_services_test_set_core(1);
    zzapp_worker_hook();
    assert(windows[0].quarantined && windows[0].quarantine_reclaimed);
    assert(zzapp_rec_by_handle(handles[0]) != NULL);
    assert(counts().free_records == 63);
    {
        struct counts reclaimed = counts();
        assert(reclaimed.tables_free == start.tables_free);
        assert(reclaimed.l2_free == start.l2_free && reclaimed.l2_reserved == 0);
        assert(reclaimed.commit_free == start.commit_free);
        assert(reclaimed.pool_used == start.pool_used);
        assert(reclaimed.heap_free == start.heap_free - ZZAPP_WINDOW_BYTES);
    }
    generations[0] = windows[0].claim_generation;
    request(req, 0);
    assert(dispatch(ZZAPP_OP_RELEASE, 0, req) == SDK_STATUS_OK);
    released(start, 1, 0);
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
                                  zzapp_as_pool_bytes(64, 32), 64, 32) == 0);
        zzapp_as_init(bsp);
    }
    start = counts();
    claim(0); consume_tables = tables;
    assert(load(0, tables ? 1 : 2) == SDK_STATUS_NO_MEMORY);
    assert(ZZAPP_LIFE_REFUSAL_OFF == 0xAC);
    assert(zzapp_get_be32(win_mem[0] + ZZAPP_LIFE_OFF + 0xAC) ==
           (tables ? AAR_REASON_TABLES : AAR_REASON_MEMORY));
    assert(!consume_tables);
    released(start, 1, 0);
    zzapp_on_amiga_reset();
}
int main(void)
{
    for (uint32_t n = 8; n <= 64; n *= 8)
        for (int mode = 0; mode < 3; mode++)
            many(n, mode);
    quarantine();
    refusal(0); refusal(1);
    puts("many_apps_test: ok (8/64 stop, exit, reset; quarantine; MEMORY/TABLES)");
    return 0;
}
