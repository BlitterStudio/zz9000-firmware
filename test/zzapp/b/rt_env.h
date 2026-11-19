/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The real runtime for the lifecycle suites: the host, loader, address
 * spaces, commit ledger, kernel and retire path are the production ones, and
 * only the execution of ARM user code is substituted. Every client is a
 * mock_client that claims, loads, starts, stops and releases through the
 * mailbox, and sees the control block and the queues the way the 68k does.
 *
 * Include after zzapp_services.c (with ZZAPP_HOST_DEFERRED_TEST defined) in a
 * suite linked with LOADER_REAL_AS_HOST_OBJS.
 *
 * The mailbox is the production dispatcher on core 0. A deferred operation
 * (the load and start ops) is run on core 1 by worker passes until core 0
 * harvests its completion, exactly as hardware orders the two. The client
 * model answers the app's requests between the passes.
 */
#ifndef B_RT_ENV_H
#define B_RT_ENV_H

#include "host_card_pool.h"
#include "aar_note_image.h"
#include "mock_client.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

void zzapp_host_exit(int code);
void zzapp_test_set_time_hook(uint64_t (*hook)(void));

#define RT_IMAGE_BYTES 0x200U
#define RT_LOAD_OFF 0x100U
#define RT_MB(n) ((uint32_t)(n) << 20)
#define RT_MAX_APPS 64U

/* The synthetic image commits one image frame, one stack frame and the heap
 * the note asks for, in MiB. */
static void rt_put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
}
static void rt_put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}

/* A minimal ELF32 ARM image with a valid note and one relocation; a test
 * changes a note word with test_note_set(image + TEST_NOTE_OFF, ...). */
static void rt_image(uint8_t image[RT_IMAGE_BYTES], uint32_t heap_mib)
{
    uint8_t *ph;

    memset(image, 0, RT_IMAGE_BYTES);
    memcpy(image, "\177ELF\1\1\1", 7);
    rt_put16(image + 16, 3); rt_put16(image + 18, 40);
    rt_put32(image + 24, RT_LOAD_OFF); rt_put32(image + 28, 52);
    rt_put32(image + 32, 0x180); rt_put32(image + 36, 0x400);
    rt_put16(image + 40, 52); rt_put16(image + 42, 32);
    rt_put16(image + 44, 2); rt_put16(image + 46, 40); rt_put16(image + 48, 1);
    ph = image + 52;
    rt_put32(ph, 1); rt_put32(ph + 4, RT_LOAD_OFF); rt_put32(ph + 8, RT_LOAD_OFF);
    rt_put32(ph + 16, 4); rt_put32(ph + 20, 8); rt_put32(ph + 24, 1);
    ph += 32;
    rt_put32(ph, 4); rt_put32(ph + 4, TEST_NOTE_OFF);
    rt_put32(ph + 16, TEST_NOTE_BYTES);
    test_note_init(image + TEST_NOTE_OFF);
    test_note_set(image + TEST_NOTE_OFF, TEST_NOTE_W_MIN_HEAP_MIB, heap_mib);
    rt_put32(image + 0xd0, RT_LOAD_OFF); rt_put32(image + 0xd4, 23);
    rt_put32(image + 0xd8, RT_LOAD_OFF);
    rt_put32(image + 0x184, 4); rt_put32(image + 0x190, 0xd0);
    rt_put32(image + 0x194, 12); rt_put32(image + 0x1a4, 12);
}

/* ---- The clients and the mailbox ------------------------------------------ */

static mock_client rt_cl[RT_MAX_APPS];
/* A client that holds no claim: QUERY and LIST take no handle. */
static mock_client rt_probe;
static uint32_t rt_pid[RT_MAX_APPS];
static uint32_t rt_request_id;
static jmp_buf rt_entered;
/* Times the app entry ran, and what it does: NULL returns to the system
 * thread with the app still runnable (the host harness reads a return from
 * the entry as an exit, so the default jumps back instead). */
static unsigned rt_entries;
static void (*rt_entry)(const aar_api *api);
/* Runs after every core 1 pass of a deferred operation, on core 0's side. */
static void (*rt_pass_hook)(unsigned pass);
/* A deferred operation that does not complete within this many passes: 0 is
 * a failure, 1 returns SDK_STATUS_QUEUED (a reset took the reply with it). */
static unsigned rt_pass_limit = 20000U;
static int rt_may_lose_reply;
/* The client capabilities word of LOAD_BEGIN (ZZAPP_CLIENT_*). */
static uint32_t rt_client_caps;
/* The reply of the last operation that produced one. */
static uint8_t rt_last_reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
static uint16_t rt_last_reply_len;

static void rt_app_entry(const void *api, int argc, char **argv)
{
    assert(api && argc >= 0 && argv);
    rt_entries++;
    if (rt_entry) {
        rt_entry((const aar_api *)api);
        return;
    }
    longjmp(rt_entered, 1);
}

/* One pass of the system thread on core 1. An entry that jumps back leaves
 * the app runnable and the pass ends here. */
static void rt_core1_pass(void)
{
    zzapp_services_test_set_core(1);
    if (setjmp(rt_entered) == 0)
        zzapp_worker_hook();
    zzapp_kernel_set_current_thread(-1);
    zzapp_services_test_set_core(0);
}

static uint16_t rt_mailbox_run(uint16_t op, const uint8_t *req, uint16_t req_len,
                               uint8_t *reply, uint16_t *reply_len)
{
    uint8_t scratch[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U, status;
    unsigned pass;

    if (!reply)
        reply = scratch;
    zzapp_services_test_set_core(0);
    status = zzapp_mailbox_dispatch(op, req, req_len, reply, &bytes, 1,
                                    ++rt_request_id, 0);
    if (status != SDK_STATUS_QUEUED) {
        memcpy(rt_last_reply, reply, bytes);
        rt_last_reply_len = bytes;
        if (reply_len)
            *reply_len = bytes;
        return status;
    }
    for (pass = 0U; pass < rt_pass_limit; pass++) {
        rt_core1_pass();
        mock_client_serve_all();
        if (rt_pass_hook)
            rt_pass_hook(pass);
        bytes = 0U;
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, reply, &bytes)) {
            zzapp_mailbox_harvest_ack();
            memcpy(rt_last_reply, reply, bytes);
            rt_last_reply_len = bytes;
            if (reply_len)
                *reply_len = bytes;
            return status;
        }
    }
    assert(rt_may_lose_reply && "deferred request did not finish");
    if (reply_len)
        *reply_len = 0U;
    return SDK_STATUS_QUEUED;
}

/* The mailbox as a client calls it. The core the caller was on is restored:
 * an app that makes a mailbox call from its own thread (a test entry) stays
 * on core 1. */
static uint16_t rt_mailbox(uint16_t op, const uint8_t *req, uint16_t req_len,
                           uint8_t *reply, uint16_t *reply_len)
{
    int saved_core = defer_test_core;
    uint16_t status = rt_mailbox_run(op, req, req_len, reply, reply_len);

    defer_test_core = saved_core;
    return status;
}

/* ---- Counts a claim, a load and a release must return to ------------------ */

struct rt_counts {
    uint32_t free_records, tables_free, tables_total, commit_free, commit_total;
    uint32_t heap_blocks, heap_bytes, pool_used, l2_free, l2_reserved;
};

static struct rt_counts rt_counts(void)
{
    struct rt_counts c;
    zzapp_rec_stats_t r;
    zzapp_as_stats_t a;
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0;
    uint32_t pid, live = 0, committed = 0;
    int saved_core = defer_test_core;

    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_QUERY, NULL, 0, reply, &bytes, 1,
                                  0, 0) == SDK_STATUS_OK);
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
    defer_test_core = saved_core;
    return c;
}

/* Every record, table, commit frame and block is back where it was. */
static void rt_assert_back(struct rt_counts start)
{
    struct rt_counts end = rt_counts();

    assert(end.free_records == start.free_records);
    assert(end.tables_free == start.tables_free && end.l2_free == start.l2_free);
    assert(end.l2_reserved == start.l2_reserved);
    assert(end.commit_free == start.commit_free &&
           end.commit_total == start.commit_total);
    assert(end.pool_used == start.pool_used);
    assert(end.heap_blocks == start.heap_blocks &&
           end.heap_bytes == start.heap_bytes);
}

/* ---- Boot, claim, load ----------------------------------------------------- */

/* A fresh card pool of `bytes`, a record arena of `apps` records and a clean
 * heap, client set and services. */
static void rt_boot(uint32_t bytes, uint32_t apps)
{
    zzapp_xport_env_t env;

    mock_client_registry_clear();
    mock_client_init(&rt_probe);
    rt_probe.mailbox = rt_mailbox;
    memset(rt_cl, 0, sizeof(rt_cl));
    host_card_pool_reset(bytes);
    zzapp_test_set_arena_apps(apps);
    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    env = mock_heap_env();
    zzapp_xport_init(&env);
    zzapp_test_set_entry(rt_app_entry);
    zzapp_test_set_xtime(0);
    zzapp_test_set_time_hook(NULL);
    rt_request_id = 0U;
    rt_entries = 0U;
    rt_entry = NULL;
    rt_pass_hook = NULL;
    rt_pass_limit = 20000U;
    rt_may_lose_reply = 0;
    rt_client_caps = 0U;
}

/* Client `n` claims through the mailbox. */
static void rt_claim(uint32_t n)
{
    mock_client *c = &rt_cl[n];

    mock_client_init(c);
    c->mailbox = rt_mailbox;
    c->kick = zzapp_xport_kick;
    assert(mock_client_claim(c) == SDK_STATUS_OK);
    assert(zzapp_rec_by_handle(c->handle) != NULL);
    rt_pid[n] = aar_handle_index(c->handle);
    (void)mock_client_register(c);
}

static zzapp_rec_t *rt_rec(uint32_t n)
{
    return zzapp_rec_by_pid(rt_pid[n]);
}

/* A mailbox op that names only the handle. */
static uint16_t rt_simple(uint32_t n, uint16_t op)
{
    return mock_client_call(&rt_cl[n], op, NULL, 0U, NULL, NULL);
}

/* LOAD_BEGIN, then the image in the load buffer the reply names and one
 * LOAD_CHUNK. The status of the last op that ran. */
static uint16_t rt_load_image(uint32_t n, const uint8_t image[RT_IMAGE_BYTES])
{
    mock_client *c = &rt_cl[n];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES], req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = {0};
    uint16_t len = 0U, status;
    uint8_t *buf;

    status = mock_client_load_begin(c, rt_client_caps, reply, &len);
    if (status != SDK_STATUS_OK)
        return status;
    assert(len == ZZAPP_LOAD_BEGIN_REPLY_BYTES);
    buf = mock_heap_map(zzapp_get_be32(reply + ZZAPP_LOAD_REPLY_TOKEN),
                        ZZAPP_LOAD_BUFFER_BYTES);
    assert(buf);
    memcpy(buf, image, RT_IMAGE_BYTES);
    zzapp_build_load_chunk_request(req, c->handle, 0, RT_IMAGE_BYTES);
    return mock_client_call(c, ZZAPP_OP_LOAD_CHUNK, req,
                            ZZAPP_MAILBOX_PAYLOAD_BYTES, NULL, NULL);
}

/* Claim-side load of the default image with `heap_mib` of private heap: all
 * of LOAD_BEGIN, LOAD_CHUNK and LOAD_END. */
static uint16_t rt_load(uint32_t n, uint32_t heap_mib)
{
    uint8_t image[RT_IMAGE_BYTES];
    uint16_t status;

    rt_image(image, heap_mib);
    status = rt_load_image(n, image);
    if (status != SDK_STATUS_OK)
        return status;
    return rt_simple(n, ZZAPP_OP_LOAD_END);
}

static uint16_t rt_start(uint32_t n)
{
    return mock_client_start(&rt_cl[n], 0U);
}

/* Claim, load and start client `n`; the app is runnable on return. */
static void __attribute__((unused)) rt_run(uint32_t n, uint32_t heap_mib)
{
    zzapp_rec_t *rec;

    rt_claim(n);
    assert(rt_load(n, heap_mib) == SDK_STATUS_OK);
    assert(rt_start(n) == SDK_STATUS_OK);
    rec = rt_rec(n);
    assert(rec);
    assert(zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_RUNNING ||
           zzapp_xport_cb_get(rec, AAR_CB_STATE) == AAR_STATE_IDLE);
}

/* The state, end reason and exit code the client reads. */
#define RT_STATE(n)  mock_client_cb_get(&rt_cl[n], AAR_CB_STATE)
#define RT_REASON(n) mock_client_cb_get(&rt_cl[n], AAR_CB_END_REASON)
#define RT_EXIT(n)   mock_client_cb_get(&rt_cl[n], AAR_CB_EXIT_CODE)

/* The next handle of a shell: LIST reports the current one of the record at
 * `pid` (a quarantined shell has a handle its client does not know). */
static uint32_t __attribute__((unused)) rt_listed_handle(uint32_t pid)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };

    assert(mock_client_list(&rt_probe, pid, reply) == SDK_STATUS_OK);
    if (zzapp_get_be32(reply + ZZAPP_LIST_INDEX) != pid)
        return 0U;
    return zzapp_get_be32(reply + ZZAPP_LIST_HANDLE);
}

/* Core 1 runs `passes` passes of its worker, the clients answering between
 * them: a stop latched on core 0 takes effect and a retire finishes. */
static void __attribute__((unused)) rt_settle(unsigned passes)
{
    while (passes--) {
        rt_core1_pass();
        mock_client_serve_all();
    }
}

#endif
