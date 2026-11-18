/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The client side of the AAR transport (sdk/amiga/src/aar_xport_zz9000.c)
 * against the firmware side (ZZ9000OS/src/zzapp_xport.c): the same queue code
 * at both ends in opposite roles, in one process, over one memory.
 *
 * The client is the module under test, built for the host with a fake board.
 * The "card" is this file's mailbox: it answers QUERY, CLAIM, LOAD_*, START,
 * STOP, LIST, RELEASE and KICK with the real firmware records and the real
 * zzapp_xport_*() calls (admission, blocks, requests, the pump, the doorbell,
 * handle resolution), over a host-visible heap that hands out 8 KiB aligned
 * blocks at card addresses. Tokens are board offsets, as on the card; the
 * fake board maps a token by finding the block that holds it.
 *
 * Covered: claim, refusal and an unusable control block, the queue ops end to
 * end (requests, completions with payload buffers, messages with credits,
 * events with coalescing and drop counting, logs, the doorbell), load and
 * start, status, lease, handle staleness (release, revocation) with a check
 * that the client touches no memory afterwards, teardown, the audio ring, and
 * a two-thread run.
 */
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_xport_zz9000.h"
#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"
#include "zz9k/abi.h"
#include "zzapp_xport.h"

struct xil_cache_mock_state g_xil_cache_mock;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

#define DDR_WB 0x15de6U
#define HEAP_78M (78U << 20)

static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

/* ---- Record arena ---------------------------------------------------------- */

static uint32_t bsp[4096];

static void setup_arena(uint32_t apps)
{
    zzapp_rec_layout_t lay;
    uint32_t addr;
    uint32_t i;

    host_card_pool_reset(128U << 20);
    zzapp_rec_layout_for(apps, HEAP_78M, &lay);
    addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
        CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
    CHECK(addr != 0U);
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
    for (i = 0U; i < 0x400U; i++)
        bsp[i] = (i << 20) | DDR_WB;
    zzapp_as_init(bsp);
}

/* ---- The host-visible heap -------------------------------------------------- */

#define HEAP_BLOCKS 256U
#define TOKEN_ADJUST 0x08000000U

typedef struct {
    void *ptr;
    uint32_t card;
    uint32_t bytes;
} heap_block_t;

static struct {
    heap_block_t block[HEAP_BLOCKS];
    uint32_t next_card;
    uint32_t allocs, frees, bad_frees;
    uint32_t live;
    int keep_freed;                 /* poison and keep a freed block */
    heap_block_t dead[HEAP_BLOCKS];
    unsigned dead_count;
} heap;

static void heap_reset(void)
{
    uint32_t i;

    for (i = 0U; i < HEAP_BLOCKS; i++) {
        if (heap.block[i].ptr)
            free(heap.block[i].ptr);
        if (heap.dead[i].ptr)
            free(heap.dead[i].ptr);
    }
    memset(&heap, 0, sizeof(heap));
    heap.next_card = 0x08200000U;
}

static int heap_alloc(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
                      uint32_t hi, void **ptr, uint32_t *card)
{
    uint32_t i;
    void *p;

    (void)ctx; (void)lo; (void)hi;
    for (i = 0U; i < HEAP_BLOCKS && heap.block[i].ptr; i++)
        ;
    if (i == HEAP_BLOCKS)
        return -1;
    p = aligned_alloc(align, bytes);
    if (!p)
        return -1;
    memset(p, 0xA5, bytes);
    heap.next_card = (heap.next_card + align - 1U) & ~(align - 1U);
    heap.block[i].ptr = p;
    heap.block[i].card = heap.next_card;
    heap.block[i].bytes = bytes;
    heap.next_card += bytes + 0x2000U;
    heap.allocs++;
    heap.live++;
    *ptr = p;
    *card = heap.block[i].card;
    return 0;
}

static int heap_release(void *ctx, void *ptr, uint32_t card)
{
    uint32_t i;

    (void)ctx;
    for (i = 0U; i < HEAP_BLOCKS; i++) {
        if (heap.block[i].ptr == ptr && heap.block[i].card == card) {
            heap.live--;
            heap.frees++;
            if (heap.keep_freed) {
                memset(ptr, 0xDD, heap.block[i].bytes);
                heap.dead[heap.dead_count++] = heap.block[i];
            } else {
                free(ptr);
            }
            memset(&heap.block[i], 0, sizeof(heap.block[i]));
            return 0;
        }
    }
    heap.bad_frees++;
    return -1;
}

/* The poison of every freed block is still there. */
static int graveyard_intact(void)
{
    unsigned i;
    uint32_t b;

    for (i = 0U; i < heap.dead_count; i++)
        for (b = 0U; b < heap.dead[i].bytes; b++)
            if (((uint8_t *)heap.dead[i].ptr)[b] != 0xDDU)
                return 0;
    return 1;
}

static volatile uint8_t *token_ptr(uint32_t token, uint32_t bytes, int *fits)
{
    uint32_t card = token + TOKEN_ADJUST;
    uint32_t i;

    for (i = 0U; i < HEAP_BLOCKS; i++)
        if (heap.block[i].ptr && card >= heap.block[i].card &&
            card < heap.block[i].card + heap.block[i].bytes) {
            if (fits)
                *fits = card - heap.block[i].card + bytes <=
                        heap.block[i].bytes;
            return (volatile uint8_t *)heap.block[i].ptr +
                   (card - heap.block[i].card);
        }
    return NULL;
}

/* ---- Barrier hooks ----------------------------------------------------------- */

static unsigned fw_acquire, fw_release, cl_acquire, cl_release;

static void fw_acq(void) { fw_acquire++; }
static void fw_rel(void) { fw_release++; }
static void cl_acq(void) { cl_acquire++; }
static void cl_rel(void) { cl_release++; }

static void env_set(void)
{
    zzapp_xport_env_t e;

    memset(&e, 0, sizeof(e));
    e.alloc = heap_alloc;
    e.release = heap_release;
    e.lo = HOST_VISIBLE_HEAP_ADDRESS;
    e.hi = 0x08100000U;
    e.token_adjust = TOKEN_ADJUST;
    e.acquire = fw_acq;
    e.release_barrier = fw_rel;
    zzapp_xport_init(&e);
}

/* ---- The card: a mailbox over the real firmware calls ------------------------ */

#define MAX_MAPS 64U

static struct {
    zzapp_rec_t *rec;
    uint32_t handle;
    uint32_t revoked;               /* handle the card has revoked, 0: none */
    uint32_t refuse_reason;         /* CLAIM refusal, 0: none */
    int corrupt_magic;              /* CLAIM yields a block that does not check */
    int bad_cb_token;               /* CLAIM names memory the board lacks */
    unsigned calls, long_calls, ticks;
    uint16_t last_op;
    unsigned posts, post_busy;      /* post_busy: answer BUSY this many times */
    unsigned kicks;
    uint32_t kick_flags_or, last_kick_flags;
    uint32_t limits[3];
    uint8_t image[512U * 1024U];
    uint32_t image_len, chunks;
    uint32_t caps_seen;
    uint32_t argc;
    char argv[ZZAPP_ARGV_MAX][ZZAPP_ARGV_STR_MAX];
    uint32_t argv_off[ZZAPP_ARGV_MAX];
    int started, stopped, released;
    unsigned map_calls, map_violations;
    struct { uint32_t token, bytes; } maps[MAX_MAPS];
} card;

static void reply_status(aar_xport_reply *r, int status)
{
    r->status = status;
    r->len = 0U;
}

static int card_release_claim(void)
{
    uint32_t handle = card.rec->hdr.handle;

    CHECK(zzapp_xport_release(card.rec) == 0);
    CHECK(zzapp_rec_free(handle) == 0);
    card.released = 1;
    card.rec = NULL;
    return 0;
}

static int fb_call(void *user, uint16_t op, const uint8_t *payload,
                   uint32_t len, int long_op, aar_xport_tick_fn tick,
                   void *tick_user, aar_xport_reply *reply)
{
    uint32_t handle = 0U;
    zzapp_rec_t *rec = NULL;
    int st = 0;

    (void)user;
    card.calls++;
    card.last_op = op;
    memset(reply, 0, sizeof(*reply));
    if (long_op) {
        card.long_calls++;
        if (tick && tick(tick_user))
            return ZZ9K_STATUS_BAD_HANDLE;
        card.ticks++;
    }
    if (op != ZZAPP_OP_QUERY && op != ZZAPP_OP_CLAIM && op != ZZAPP_OP_LIST) {
        if (len != ZZAPP_MAILBOX_PAYLOAD_BYTES)
            return ZZ9K_STATUS_BAD_REQUEST;
        handle = zzapp_get_be32(payload + ZZAPP_SEL_HANDLE);
        if (handle != 0U && handle == card.revoked) {
            /* The stale path of a revoked claim: the shell is released. */
            if (card.rec)
                card_release_claim();
            reply_status(reply, ZZAPP_ERR_STALE);
            return ZZAPP_ERR_STALE;
        }
        rec = zzapp_xport_resolve(handle, &st);
        if (!rec) {
            reply_status(reply, ZZAPP_ERR_STALE);
            return ZZAPP_ERR_STALE;
        }
    }
    switch (op) {
    case ZZAPP_OP_QUERY:
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_ABI_MAJOR, ZZAPP_ABI_MAJOR);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_ABI_MINOR, ZZAPP_ABI_MINOR);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_CAPS,
                       ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_QUEUE_DEPTH, 64U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_RECORDS_FREE, 15U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_HOSTHEAP_FREE, 1234U);
        reply->len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
        return 0;
    case ZZAPP_OP_CLAIM: {
        zzapp_xport_buf_t pair;
        uint32_t h = 0U;

        if (card.refuse_reason) {
            zzapp_put_be32(reply->payload, card.refuse_reason);
            reply->len = ZZAPP_REFUSAL_REPLY_BYTES;
            reply->status = ZZ9K_STATUS_NO_MEMORY;
            return ZZ9K_STATUS_NO_MEMORY;
        }
        CHECK(zzapp_rec_admit(&h, &card.rec) == 0U);
        CHECK(zzapp_xport_admit(card.rec) == AAR_REASON_NONE);
        card.handle = h;
        card.revoked = 0U;
        card.released = 0;
        card.started = card.stopped = 0;
        card.image_len = card.chunks = 0U;
        CHECK(zzapp_xport_block_get(card.rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
        if (card.corrupt_magic)
            zzapp_xport_cb_put(card.rec, AAR_CB_MAGIC, 0U);
        zzapp_put_be32(reply->payload + ZZAPP_CLAIM_HANDLE, h);
        zzapp_put_be32(reply->payload + ZZAPP_CLAIM_CB_TOKEN,
                       card.bad_cb_token ? 0x7ff00000U : pair.token);
        zzapp_put_be32(reply->payload + ZZAPP_CLAIM_CB_BYTES, AAR_PAIR_BYTES);
        reply->len = ZZAPP_CLAIM_REPLY_BYTES;
        return 0;
    }
    case ZZAPP_OP_LOAD_BEGIN: {
        zzapp_xport_buf_t load;

        card.caps_seen = zzapp_get_be32(payload + ZZAPP_LOAD_CLIENT_CAPS);
        card.limits[0] = zzapp_xport_cb_read_client(rec, AAR_CB_PRIVATE_MIB);
        card.limits[1] = zzapp_xport_cb_read_client(rec, AAR_CB_SHARED_MIB);
        card.limits[2] = zzapp_xport_cb_read_client(rec, AAR_CB_WEIGHT);
        if (zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD,
                                    ZZAPP_LOAD_BUFFER_BYTES, &load) != AAR_OK) {
            zzapp_put_be32(reply->payload, AAR_REASON_HOSTHEAP);
            reply->len = ZZAPP_REFUSAL_REPLY_BYTES;
            reply->status = ZZ9K_STATUS_NO_MEMORY;
            return ZZ9K_STATUS_NO_MEMORY;
        }
        zzapp_xport_cb_set_state(rec, AAR_STATE_LOADING, AAR_END_NONE);
        zzapp_put_be32(reply->payload + ZZAPP_LOAD_REPLY_TOKEN, load.token);
        zzapp_put_be32(reply->payload + ZZAPP_LOAD_REPLY_SIZE, load.bytes);
        zzapp_put_be32(reply->payload + ZZAPP_LOAD_REPLY_CHUNK_MAX,
                       ZZAPP_CHUNK_MAX);
        zzapp_put_be32(reply->payload + ZZAPP_LOAD_REPLY_ARGV_OFF,
                       ZZAPP_LOAD_ARGV_OFF);
        reply->len = ZZAPP_LOAD_BEGIN_REPLY_BYTES;
        return 0;
    }
    case ZZAPP_OP_LOAD_CHUNK: {
        zzapp_xport_buf_t load;
        uint32_t off = zzapp_get_be32(payload + ZZAPP_CHUNK_FILE_OFF);
        uint32_t n = zzapp_get_be32(payload + ZZAPP_CHUNK_LENGTH);

        if (!zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load) ||
            off + n > sizeof(card.image))
            return ZZ9K_STATUS_BAD_REQUEST;
        memcpy(card.image + off, load.ptr, n);
        if (off + n > card.image_len)
            card.image_len = off + n;
        card.chunks++;
        return 0;
    }
    case ZZAPP_OP_LOAD_END:
        return 0;
    case ZZAPP_OP_START: {
        zzapp_xport_buf_t load;
        const uint8_t *area;
        uint32_t i;

        CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load) == 1);
        area = (const uint8_t *)load.ptr + ZZAPP_LOAD_ARGV_OFF;
        card.argc = zzapp_get_be32(payload + ZZAPP_START_ARGC);
        CHECK(card.argc == zzapp_get_be32(area + ZZAPP_ARGV_ARGC_OFF));
        for (i = 0U; i < card.argc && i < ZZAPP_ARGV_MAX; i++) {
            card.argv_off[i] = zzapp_get_be32(area + ZZAPP_ARGV_OFFS_OFF + i * 4U);
            snprintf(card.argv[i], sizeof(card.argv[i]), "%s",
                     (const char *)load.ptr + card.argv_off[i]);
        }
        CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_LOAD) == AAR_OK);
        zzapp_xport_cb_set_state(rec, AAR_STATE_RUNNING, AAR_END_NONE);
        card.started = 1;
        return 0;
    }
    case ZZAPP_OP_STOP:
        zzapp_xport_cb_set_state(rec, AAR_STATE_IDLE, AAR_END_FORCED);
        card.stopped = 1;
        return 0;
    case ZZAPP_OP_RELEASE:
        card_release_claim();
        return 0;
    case ZZAPP_OP_LIST:
        reply->len = ZZAPP_LIST_REPLY_BYTES;
        if (card.rec && zzapp_get_be32(payload) == 0U) {
            zzapp_put_be32(reply->payload + ZZAPP_LIST_HANDLE, card.handle);
            zzapp_put_be32(reply->payload + ZZAPP_LIST_STATE, AAR_STATE_RUNNING);
            zzapp_put_be32(reply->payload + ZZAPP_LIST_INDEX,
                           aar_handle_index(card.handle));
            zzapp_put_be32(reply->payload + ZZAPP_LIST_REASONS,
                           (ZZAPP_BLOCK_FILE << 24) | (AAR_END_NONE << 16));
            memcpy(reply->payload + ZZAPP_LIST_NAME, "swquake", 7);
        }
        return 0;
    default:
        return ZZ9K_STATUS_UNSUPPORTED;
    }
}

static int fb_post(void *user, uint16_t op, const uint8_t *payload, uint32_t len)
{
    uint32_t handle, flags;

    (void)user;
    card.posts++;
    if (card.post_busy) {
        card.post_busy--;
        return AAR_E_BUSY;
    }
    CHECK(op == ZZAPP_OP_KICK && len == ZZAPP_MAILBOX_PAYLOAD_BYTES);
    handle = zzapp_get_be32(payload + ZZAPP_SEL_HANDLE);
    flags = zzapp_get_be32(payload + ZZAPP_KICK_FLAGS);
    card.kicks++;
    card.kick_flags_or |= flags;
    card.last_kick_flags = flags;
    return zzapp_xport_kick(handle, flags) == AAR_OK ? AAR_OK : AAR_E_STALE;
}

static volatile uint8_t *fb_map(void *user, uint32_t token, uint32_t bytes)
{
    int fits = 0;
    volatile uint8_t *p;

    (void)user;
    card.map_calls++;
    if (card.map_calls <= MAX_MAPS) {
        card.maps[card.map_calls - 1U].token = token;
        card.maps[card.map_calls - 1U].bytes = bytes;
    }
    p = token_ptr(token, bytes, &fits);
    if (!p)
        return NULL;
    /* A mapped range lies in one block and is whole 8 KiB steps. */
    if (!fits || (bytes & 0x1fffU))
        card.map_violations++;
    return p;
}

static aar_xport_board board;

static void card_reset(void)
{
    memset(&card, 0, sizeof(card));
}

static void reset_all(void)
{
    heap_reset();
    env_set();
    fw_acquire = fw_release = cl_acquire = cl_release = 0U;
    card_reset();
    setup_arena(16U);
    memset(&board, 0, sizeof(board));
    board.call = fb_call;
    board.post = fb_post;
    board.map = fb_map;
    board.acquire = cl_acq;
    board.release = cl_rel;
}

static void claim(aar_xport *x)
{
    uint32_t reason = 99U;

    CHECK(aar_xport_open(x, &board, &reason) == AAR_OK);
    CHECK(reason == AAR_REASON_NONE);
}

static uint32_t cbw(uint32_t off)
{
    return zzapp_xport_cb_get(card.rec, off);
}

/* ---- Firmware helpers --------------------------------------------------------- */

static uint32_t fw_request(zzapp_xport_req_kind_t kind, uint16_t op, uint32_t a0,
                           uint32_t a1, uint32_t buf, uint32_t off, uint32_t len)
{
    aar_request r;
    uint32_t id = 0U;

    memset(&r, 0, sizeof(r));
    r.op = op;
    r.arg[0] = a0;
    r.arg[1] = a1;
    r.buf = buf;
    r.buf_off = off;
    r.buf_len = len;
    CHECK(zzapp_xport_post_request(card.rec, kind, &r, 0U, &id) == AAR_OK);
    return id;
}

/* The pump of the app side, then the completion of an entry. */
static int fw_completion(zzapp_xport_req_kind_t kind, aar_completion *c)
{
    CHECK(zzapp_xport_pump(card.rec) >= 0);
    return zzapp_xport_take_completion(card.rec, kind, c);
}

/* ---- Tests ---------------------------------------------------------------------- */

static void test_open_and_refusal(void)
{
    aar_xport x;
    uint32_t reason = 0U;
    zzapp_rec_stats_t st0, st1;
    zzapp_xport_buf_t pair;

    reset_all();
    zzapp_rec_stats(&st0);
    claim(&x);
    CHECK(card.rec && x.handle == card.handle && aar_xport_live(&x));
    CHECK(aar_xport_handle(&x) == card.rec->hdr.handle);
    CHECK(card.calls == 2U);                  /* QUERY, CLAIM */
    CHECK(zzapp_xport_block_get(card.rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
    /* One mapping: the pair block, whole, and both queues inside it. */
    CHECK(card.map_calls == 1U && card.maps[0].token == pair.token &&
          card.maps[0].bytes == 0x4000U && card.map_violations == 0U);
    CHECK(x.sq.depth == 64U && x.cq.depth == 64U);
    CHECK(cbw(AAR_CB_CLIENT_PRESENT) == 1U);
    CHECK(aar_xport_lease_renew(&x) == AAR_OK && cbw(AAR_CB_LEASE_COUNTER) == 1U);

    /* Close it: everything comes back. */
    CHECK(aar_xport_release(&x) == AAR_OK);
    CHECK(!aar_xport_live(&x) && heap.live == 0U && card.released);
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free);

    /* The card refuses: the reason comes back, nothing is held. */
    card.refuse_reason = AAR_REASON_HOSTHEAP;
    CHECK(aar_xport_open(&x, &board, &reason) == AAR_E_NORESOURCE);
    CHECK(reason == AAR_REASON_HOSTHEAP && aar_xport_reason(&x) == reason);
    CHECK(!aar_xport_live(&x) && heap.live == 0U);
    card.refuse_reason = 0U;

    /* A control block that does not check: the module gives the claim back. */
    card.corrupt_magic = 1;
    CHECK(aar_xport_open(&x, &board, &reason) == AAR_E_IO);
    CHECK(!aar_xport_live(&x) && heap.live == 0U && card.released);
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free);
    card.corrupt_magic = 0;

    /* A token the board does not have: the same. */
    card.bad_cb_token = 1;
    CHECK(aar_xport_open(&x, &board, &reason) == AAR_E_IO);
    CHECK(!aar_xport_live(&x) && heap.live == 0U && card.released);
    card.bad_cb_token = 0;

    /* Without a board nothing works, and nothing crashes. */
    CHECK(aar_xport_open(&x, NULL, NULL) == AAR_E_INVAL);
    CHECK(aar_xport_lease_renew(&x) == AAR_E_STALE);
    CHECK(aar_xport_service_next(&x, &(aar_xport_req){ 0 }) == AAR_E_STALE);
    CHECK(aar_xport_send_message(&x, "x") == AAR_E_STALE);
    CHECK(aar_xport_map(&x, 1U, 0U, 1U) == NULL);
}

static void test_query_and_list(void)
{
    aar_xport_query_info qi;
    aar_xport_list_info li;
    aar_xport x;

    reset_all();
    CHECK(aar_xport_query(&board, &qi) == AAR_OK);
    CHECK(qi.abi_major == 0U && qi.abi_minor == ZZAPP_ABI_MINOR &&
          (qi.caps & ZZAPP_CAP_Z3) && qi.queue_depth == 64U &&
          qi.records_free == 15U && qi.hostheap_free == 1234U);
    CHECK(aar_xport_list(&board, 0U, &li) == 0 && li.handle == 0U);
    claim(&x);
    CHECK(aar_xport_list(&board, 0U, &li) == 1);
    CHECK(li.handle == x.handle && li.state == AAR_STATE_RUNNING &&
          li.block_reason == ZZAPP_BLOCK_FILE && !strcmp(li.name, "swquake"));
    /* The administrative release of a claim by its handle. */
    CHECK(aar_xport_release_handle(&board, 0x12345678U) == AAR_E_STALE);
    CHECK(aar_xport_release_handle(&board, x.handle) == AAR_OK);
    CHECK(card.released && heap.live == 0U);
}

static void test_requests_end_to_end(void)
{
    aar_xport x;
    aar_xport_req req;
    aar_completion c;
    zzapp_xport_buf_t scratch, data;
    uint32_t id, i;
    uint8_t *bytes;

    reset_all();
    claim(&x);
    CHECK(aar_xport_service_next(&x, &req) == 0);

    /* FILE_OPEN: the path is in the scratch area. */
    CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_SCRATCH,
                                   &scratch) == AAR_OK);
    strcpy((char *)scratch.ptr, "PROGDIR:data/pak0.pak");
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_OPEN, 3U, AAR_FILE_READ,
                    scratch.token, scratch.off, 22U);
    CHECK(zzapp_xport_req_state(card.rec, ZZAPP_XPORT_REQ_BLOCKING) ==
          ZZAPP_XPORT_REQ_POSTED);
    CHECK(aar_xport_service_next(&x, &req) == 1);
    CHECK(req.op == AAR_OP_FILE_OPEN && req.id == id && req.arg[0] == 3U &&
          req.arg[1] == AAR_FILE_READ && req.buf == scratch.token &&
          req.buf_len == 22U && req.result == 0 && req.out[0] == 0U);
    CHECK(req.ptr && !strcmp((const char *)req.ptr, "PROGDIR:data/pak0.pak"));
    CHECK(aar_xport_service_next(&x, &req) == 0);   /* the SQ is empty again */
    /* Not complete yet: the firmware sees nothing. */
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 0);
    {
        aar_xport_req done = { 0 };

        done.op = AAR_OP_FILE_OPEN;
        done.id = id;
        done.result = AAR_OK;
        done.out[0] = 3U;
        done.out[5] = 0xdeadbeefU;
        done.reply_flags = 0x0102U;
        CHECK(aar_xport_service_reply(&x, &done) == AAR_OK);
    }
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1);
    CHECK(c.op == AAR_OP_FILE_OPEN && c.id == id && c.result == AAR_OK &&
          c.out[0] == 3U && c.out[5] == 0xdeadbeefU && c.flags == 0x0102U);
    /* The doorbell went off for the completion. */
    CHECK(card.kicks == 1U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    CHECK(zzapp_xport_kick_take(card.rec) == ZZAPP_KICK_CQ_POSTED);
    CHECK(aar_xport_stats_get(&x)->requests == 1U &&
          aar_xport_stats_get(&x)->replies == 1U);

    /* FILE_READ: the client writes the bytes, then completes; the firmware
     * reads them from the data block it named. */
    CHECK(zzapp_xport_data_reserve(card.rec, 40000U, &data) == AAR_OK);
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_READ, 3U, 40000U,
                    data.token, 0U, 40000U);
    CHECK(aar_xport_service_next(&x, &req) == 1 && req.op == AAR_OP_FILE_READ);
    CHECK(req.ptr == (volatile uint8_t *)data.ptr);
    for (i = 0U; i < 40000U; i++)
        req.ptr[i] = (uint8_t)(i * 7U + 1U);
    req.result = AAR_OK;
    req.out[AAR_R_FILE_COUNT] = 40000U;
    CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1 &&
          c.out[AAR_R_FILE_COUNT] == 40000U && c.id == id);
    bytes = data.ptr;
    for (i = 0U; i < 40000U; i++)
        if (bytes[i] != (uint8_t)(i * 7U + 1U)) {
            CHECK(0 && "file payload");
            break;
        }
    /* The data block was mapped once for both requests of its range. */
    CHECK(card.map_violations == 0U);

    /* A request with a payload offset and the barrier hooks on both ends. */
    CHECK(fw_acquire > 0U && fw_release > 0U && cl_acquire > 0U && cl_release > 0U);
    CHECK(aar_xport_release(&x) == AAR_OK && heap.live == 0U);
}

static void test_mapping_cache(void)
{
    aar_xport x;
    aar_xport_req req;
    zzapp_xport_buf_t sc, data;
    unsigned before;
    uint32_t i;

    reset_all();
    claim(&x);
    CHECK(card.map_calls == 1U);
    CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_SCRATCH, &sc) == AAR_OK);
    /* Many requests into the one service block: one mapping of 8 KiB. */
    for (i = 0U; i < 10U; i++) {
        fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, sc.token,
                   sc.off, 16U);
        CHECK(aar_xport_service_next(&x, &req) == 1);
        req.result = 0;
        CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    }
    CHECK(card.map_calls == 2U && card.maps[1].token == sc.token &&
          card.maps[1].bytes == 0x2000U);
    /* A later request into the same block that reaches further extends it. */
    before = card.map_calls;
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_CONSOLE_WRITE, 0U, 0U, sc.token,
               ZZAPP_XPORT_CONSOLE_OUT_OFF, 0x2000U);
    CHECK(aar_xport_service_next(&x, &req) == 1 && req.ptr);
    CHECK(card.map_calls == before + 1U && card.maps[before].token == sc.token &&
          card.maps[before].bytes == 0x6000U);
    req.result = 0;
    CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    /* A different block maps separately. */
    CHECK(zzapp_xport_data_reserve(card.rec, 0x4000U, &data) == AAR_OK);
    before = card.map_calls;
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_WRITE, 0U, 0U, data.token, 0U,
               0x4000U);
    CHECK(aar_xport_service_next(&x, &req) == 1);
    CHECK(card.map_calls == before + 1U && card.maps[before].bytes == 0x4000U);
    CHECK(card.map_violations == 0U);
    /* The public map: same cache. */
    before = card.map_calls;
    CHECK(aar_xport_map(&x, data.token, 0x100U, 0x100U) == (volatile uint8_t *)data.ptr + 0x100U);
    CHECK(card.map_calls == before);
    CHECK(aar_xport_map(&x, 0x7ff00000U, 0U, 16U) == NULL);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_bad_buffer_is_refused(void)
{
    aar_xport x;
    aar_xport_req req;
    aar_completion c;
    uint32_t id;

    reset_all();
    claim(&x);
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_OPEN, 0U, 0U, 0x7ff00000U,
                    0U, 8U);
    /* The client answers an unmappable buffer itself, so the app is not left
     * waiting: the request is not handed out. */
    CHECK(aar_xport_service_next(&x, &req) == 0);
    CHECK(aar_xport_stats_get(&x)->bad_buf == 1U);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1);
    CHECK(c.id == id && c.result == AAR_E_INVAL && c.op == AAR_OP_FILE_OPEN);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_messages_and_credits(void)
{
    aar_xport x;
    uint8_t m[AAR_MSG_BYTES], got[AAR_MSG_BYTES];
    uint32_t i, n;

    reset_all();
    claim(&x);
    memset(m, 0, sizeof(m));

    /* App to client: in order, and none stuck behind a request. */
    for (i = 0U; i < 5U; i++) {
        m[0] = (uint8_t)(i + 1U);
        CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    }
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
    m[0] = 99U;
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);   /* behind the request */
    {
        aar_xport_req req;

        CHECK(aar_xport_service_next(&x, &req) == 1 && req.op == AAR_OP_FILE_STAT);
        CHECK(aar_xport_message_count(&x) == 5U);
        for (i = 0U; i < 5U; i++) {
            CHECK(aar_xport_get_message(&x, got) == 1 && got[0] == i + 1U);
        }
        CHECK(aar_xport_get_message(&x, got) == 1 && got[0] == 99U);  /* pumped now */
        CHECK(aar_xport_get_message(&x, got) == 0);
        req.result = 0;
        CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    }
    CHECK(aar_xport_stats_get(&x)->msgs_taken == 6U);

    /* The credit: 63 messages and no more until the app takes some. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        m[0] = (uint8_t)i; m[1] = 0x5a;
        CHECK(aar_xport_send_message(&x, m) == AAR_OK);
    }
    CHECK(aar_xport_send_message(&x, m) == AAR_E_BUSY);
    CHECK(aar_xport_stats_get(&x)->msgs_sent == AAR_MSG_CAPACITY);
    /* A request completion still fits behind them: the CQ has room for it. */
    {
        uint32_t id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U,
                                 0U, 0U, 0U);
        aar_xport_req req;
        aar_completion c;

        CHECK(aar_xport_service_next(&x, &req) == 1);
        req.result = 0;
        CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
        /* The pump moves everything out of the CQ; the completion is reached
         * although 63 messages came first. */
        CHECK(zzapp_xport_pump(card.rec) == (int)AAR_MSG_CAPACITY + 1);
        CHECK(zzapp_xport_take_completion(card.rec, ZZAPP_XPORT_REQ_BLOCKING, &c) == 1 &&
              c.id == id);
    }
    /* Nothing consumed yet: still no credit, though the CQ itself is empty. */
    CHECK(aar_xport_send_message(&x, m) == AAR_E_BUSY);
    CHECK(zzapp_xport_msg_pending(card.rec) == AAR_MSG_CAPACITY);
    for (n = 0U; n < 3U; n++) {
        CHECK(zzapp_xport_take_msg(card.rec, got) == 1 && got[0] == n && got[1] == 0x5a);
    }
    CHECK(cbw(AAR_CB_MSG_CONSUMED) == 3U);
    for (n = 0U; n < 3U; n++) {
        m[0] = 0xc0 + (uint8_t)n;
        CHECK(aar_xport_send_message(&x, m) == AAR_OK);
    }
    CHECK(aar_xport_send_message(&x, m) == AAR_E_BUSY);
    CHECK(zzapp_xport_pump(card.rec) == 3);
    CHECK(zzapp_xport_stats(card.rec)->msgs_overflow == 0U);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_inbox_overflow_is_counted(void)
{
    aar_xport x;
    aar_xport_req req;
    uint8_t m[AAR_MSG_BYTES] = { 0 }, got[AAR_MSG_BYTES];
    uint32_t i, taken = 0U;

    reset_all();
    claim(&x);
    /* The app sends 63; the client moves them aside, reads none. The app sends
     * 63 more; the inbox holds 64 in all, the rest are lost and counted. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        m[0] = (uint8_t)i;
        CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    }
    CHECK(aar_xport_service_next(&x, &req) == 0);
    CHECK(aar_xport_message_count(&x) == AAR_MSG_CAPACITY);
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        m[0] = (uint8_t)(100U + i);
        CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    }
    CHECK(aar_xport_service_next(&x, &req) == 0);
    CHECK(aar_xport_message_count(&x) == AAR_XPORT_INBOX);
    CHECK(aar_xport_stats_get(&x)->msgs_overflow == 2U * AAR_MSG_CAPACITY - AAR_XPORT_INBOX);
    while (aar_xport_get_message(&x, got) == 1) {
        if (taken < AAR_MSG_CAPACITY)
            CHECK(got[0] == taken);
        else
            CHECK(got[0] == 100U);    /* the first of the second batch */
        taken++;
    }
    CHECK(taken == AAR_XPORT_INBOX);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_events_coalescing_and_drops(void)
{
    aar_xport x;
    aar_event ev, got;
    uint32_t i;
    zzapp_xport_buf_t none;

    (void)none;
    reset_all();
    claim(&x);

    /* Keys are delivered one by one, in order. */
    for (i = 0U; i < 10U; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = AAR_EV_KEY_DOWN; ev.code = (uint16_t)(0x20U + i); ev.time_ms = i;
        CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    }
    CHECK(zzapp_xport_pump(card.rec) == 10);
    for (i = 0U; i < 10U; i++) {
        CHECK(zzapp_xport_take_event(card.rec, &got) == 1);
        CHECK(got.type == AAR_EV_KEY_DOWN && got.code == 0x20U + i && got.time_ms == i);
    }

    /* A mouse flood: the client posts one entry, not 1000. The first event
     * goes out at once; the pump runs rarely, so the rest ride on the CQ
     * only as far as it has room. Fill the CQ first, so nothing is posted. */
    for (i = 0U; i < 64U; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = AAR_EV_KEY_UP; ev.code = (uint16_t)i;
        CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    }
    /* Full now: another key is dropped and counted in the control block. */
    CHECK(aar_xport_post_event(&x, &ev) == AAR_E_BUSY);
    CHECK(aar_get_be32(x.cb + AAR_CB_EVENTS_DROPPED) == 1U);
    CHECK(aar_get_be32(x.cb + AAR_CB_CQ_FULL) == 1U);
    CHECK(aar_xport_stats_get(&x)->events_dropped == 1U);
    /* Mouse motion meanwhile costs no entry: it is held and summed. */
    for (i = 0U; i < 1000U; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = AAR_EV_MOUSE; ev.dx = 3; ev.dy = -1; ev.time_ms = 500U + i;
        CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    }
    CHECK(aar_xport_stats_get(&x)->events_merged == 999U);
    CHECK(aar_get_be32(x.cb + AAR_CB_EVENTS_DROPPED) == 1U);    /* none lost */
    CHECK(zzapp_xport_pump(card.rec) == 64);
    CHECK(zzapp_xport_event_pending(card.rec) == 64U);
    for (i = 0U; i < 64U; i++) {
        CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.code == i);
    }
    /* Room again: the held motion goes out as one event with the sums. */
    CHECK(aar_xport_flush_events(&x) == 1);
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1);
    CHECK(got.type == AAR_EV_MOUSE && got.dx == 3000 && got.dy == -1000 &&
          got.time_ms == 1499U);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 0);
    CHECK(aar_xport_flush_events(&x) == 1);          /* nothing pending */

    /* A key after motion keeps the order: motion first, then the key. */
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_MOUSE; ev.dx = 5; ev.dy = 6;
    /* The CQ is empty, so the motion goes out at once; hold the next. */
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    ev.dx = 1;
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_DOWN; ev.code = 0x45;
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    CHECK(zzapp_xport_pump(card.rec) == 3);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_MOUSE && got.dx == 5);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_MOUSE && got.dx == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_KEY_DOWN);

    /* A button change is not folded into earlier motion. */
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_MOUSE; ev.buttons = 0; ev.dx = 4;
    for (i = 0U; i < 64U; i++) {            /* fill the CQ so events stay held */
        aar_event k;

        memset(&k, 0, sizeof(k));
        k.type = AAR_EV_KEY_UP; k.code = (uint16_t)i;
        CHECK(aar_xport_post_event(&x, &k) == AAR_OK);
    }
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);       /* held */
    ev.buttons = 1; ev.dx = 2;
    CHECK(aar_xport_post_event(&x, &ev) == AAR_E_BUSY);   /* cannot flush: dropped */
    CHECK(aar_xport_stats_get(&x)->events_merged == 999U);   /* the click was not folded in */
    CHECK(zzapp_xport_pump(card.rec) == 64);
    for (i = 0U; i < 64U; i++)
        CHECK(zzapp_xport_take_event(card.rec, &got) == 1);
    CHECK(aar_xport_flush_events(&x) == 1);
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.dx == 4 && got.buttons == 0);
    /* The click, retried, goes out now. */
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.buttons == 1 && got.dx == 2);

    /* Saturation: motion past 16 bits does not wrap. */
    for (i = 0U; i < 64U; i++) {
        aar_event k;

        memset(&k, 0, sizeof(k));
        k.type = AAR_EV_KEY_UP;
        CHECK(aar_xport_post_event(&x, &k) == AAR_OK);
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_MOUSE; ev.dx = 30000;
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    CHECK(aar_xport_post_event(&x, &ev) == AAR_OK);
    CHECK(zzapp_xport_pump(card.rec) == 64);
    for (i = 0U; i < 64U; i++)
        (void)zzapp_xport_take_event(card.rec, &got);
    CHECK(aar_xport_flush_events(&x) == 1 && zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.dx == 32767);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_logs(void)
{
    aar_xport x;
    aar_xport_req req;

    reset_all();
    claim(&x);
    /* Without a sink the records are dropped and counted, not stuck. */
    CHECK(zzapp_xport_post_log(card.rec, "hello", 5U) == AAR_OK);
    CHECK(aar_xport_service_next(&x, &req) == 0);
    CHECK(aar_xport_stats_get(&x)->logs_unclaimed == 1U && aar_xport_stats_get(&x)->logs == 0U);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static char sink_seen[8][AAR_LOG_MAX + 1];
static uint32_t sink_len[8];
static unsigned sink_count;

void log_sink_fn(void *user, const void *data, uint32_t len)
{
    (void)user;
    if (sink_count < 8U) {
        memcpy(sink_seen[sink_count], data, len);
        sink_seen[sink_count][len] = '\0';
        sink_len[sink_count] = len;
    }
    sink_count++;
}

static void test_log_sink(void)
{
    aar_xport x;
    aar_xport_req req;
    char big[2000];

    reset_all();
    claim(&x);
    sink_count = 0U;
    aar_xport_set_log_sink(&x, log_sink_fn, NULL);
    CHECK(zzapp_xport_post_log(card.rec, "first line", 10U) == AAR_OK);
    memset(big, 'z', sizeof(big));
    CHECK(zzapp_xport_post_log(card.rec, big, sizeof(big)) == AAR_OK);   /* truncated to 1024 */
    CHECK(zzapp_xport_post_log(card.rec, "third", 5U) == AAR_OK);
    CHECK(aar_xport_service_next(&x, &req) == 0);
    CHECK(sink_count == 3U);
    CHECK(sink_len[0] == 10U && !strcmp(sink_seen[0], "first line"));
    CHECK(sink_len[1] == AAR_LOG_MAX && sink_seen[1][0] == 'z' &&
          sink_seen[1][AAR_LOG_MAX - 1U] == 'z');
    CHECK(!strcmp(sink_seen[2], "third"));
    CHECK(aar_xport_stats_get(&x)->logs == 3U);
    /* A log record between two requests does not hide the second. */
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
    CHECK(zzapp_xport_post_log(card.rec, "fourth", 6U) == AAR_OK);
    CHECK(aar_xport_service_next(&x, &req) == 1 && req.op == AAR_OP_FILE_STAT);
    CHECK(aar_xport_service_next(&x, &req) == 0 && sink_count == 4U);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_doorbell(void)
{
    aar_xport x;
    aar_xport_req req;
    unsigned i;

    reset_all();
    claim(&x);
    /* A reply rings once. */
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
    CHECK(aar_xport_service_next(&x, &req) == 1);
    req.result = 0;
    CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
    CHECK(card.kicks == 1U && zzapp_xport_needs_pump(card.rec) == 1);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    (void)zzapp_xport_kick_take(card.rec);

    /* The mailbox is busy with the last bell: bits coalesce and go out later,
     * combined, from the next pass. */
    card.post_busy = 1000U;
    for (i = 0U; i < 3U; i++) {
        fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
        CHECK(aar_xport_service_next(&x, &req) == 1);
        req.result = 0;
        CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    }
    CHECK(card.kicks == 1U);                          /* every attempt refused */
    card.post_busy = 0U;
    CHECK(aar_xport_stats_get(&x)->kicks_coalesced >= 2U);
    aar_xport_flush(&x);
    CHECK(card.kicks == 2U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    aar_xport_flush(&x);
    CHECK(card.kicks == 2U);                          /* nothing pending */

    /* A full SQ that the client drains rings SQ_SPACE: the app may be parked
     * for room. 63 messages and a request fill the 64 entries. */
    {
        uint8_t m[AAR_MSG_BYTES] = { 0 };
        uint32_t k;

        (void)zzapp_xport_kick_take(card.rec);
        for (k = 0U; k < AAR_MSG_CAPACITY; k++)
            CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
        fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
        CHECK(zzapp_xport_sq_space(card.rec) == 0);
        card.kick_flags_or = 0U;
        CHECK(aar_xport_service_next(&x, &req) == 1);
        CHECK(zzapp_xport_sq_space(card.rec) == 1);
        aar_xport_flush(&x);
        CHECK(card.kick_flags_or & ZZAPP_KICK_SQ_SPACE);
        req.result = 0;
        CHECK(aar_xport_service_reply(&x, &req) == AAR_OK);
    }
    /* Unknown bits are not sent. */
    {
        unsigned kicks = card.kicks;

        aar_xport_kick(&x, 0x80U);
        CHECK(card.kicks == kicks);
    }
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_load_start_stop_status(void)
{
    aar_xport x;
    aar_xport_status st;
    static uint8_t image[150000];
    const char *argv[3] = { "swquake", "-nosound", "+map demo1" };
    volatile uint8_t *staging;
    uint32_t chunk_max, i;

    reset_all();
    claim(&x);
    CHECK(cbw(AAR_CB_STATE) == AAR_STATE_CLAIMED);
    for (i = 0U; i < sizeof(image); i++)
        image[i] = (uint8_t)(i * 31U + 7U);

    /* Limits go through the control block, before the load. */
    CHECK(aar_xport_set_limits(&x, 64U, 20U, 3U) == AAR_OK);
    CHECK(aar_xport_set_limits(&x, 4096U, 0U, 0U) == AAR_E_INVAL);
    CHECK(aar_xport_load_chunk(&x, 0U, 10U) == AAR_E_INVAL);          /* no begin */
    CHECK(aar_xport_start(&x, argv, 3U) == AAR_E_INVAL);
    CHECK(aar_xport_load_memory(&x, image, sizeof(image),
                                ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_CLUT8) == AAR_OK);
    CHECK(card.limits[0] == 64U && card.limits[1] == 20U && card.limits[2] == 3U);
    CHECK(card.caps_seen == (ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_CLUT8));
    CHECK(card.image_len == sizeof(image) && card.chunks == 3U &&
          !memcmp(card.image, image, sizeof(image)));
    CHECK(card.long_calls == 2U);                    /* begin, end */
    CHECK(card.ticks >= 2U && cbw(AAR_CB_LEASE_COUNTER) >= 2U);   /* lease renewed in them */
    CHECK(aar_xport_set_limits(&x, 1U, 1U, 1U) == AAR_E_BUSY);
    CHECK(aar_xport_load_begin(&x, 0U, &staging, &chunk_max) == AAR_E_BUSY);

    CHECK(aar_xport_start(&x, argv, 3U) == AAR_OK);
    CHECK(card.argc == 3U && !strcmp(card.argv[0], "swquake") &&
          !strcmp(card.argv[1], "-nosound") && !strcmp(card.argv[2], "+map demo1"));
    CHECK(card.argv_off[0] >= ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
    CHECK(card.started && x.load == NULL);
    CHECK(!zzapp_xport_block_get(card.rec, ZZAPP_XPORT_BLOCK_LOAD, NULL));
    CHECK(aar_xport_start(&x, argv, 3U) == AAR_E_INVAL);          /* twice */

    /* Status comes from the control block. */
    zzapp_xport_cb_set_fault(card.rec, AAR_FAULT_DATA, 0x805U, 0x60000010U,
                             0x00401234U, 0x7fff0000U, 0x00400100U, 0xdeadbeefU);
    zzapp_xport_cb_put(card.rec, AAR_CB_EXIT_CODE, (uint32_t)-5);
    zzapp_xport_cb_put(card.rec, AAR_CB_FLAGS, AAR_CBF_DISPLAY | AAR_CBF_CONSOLE);
    zzapp_xport_cb_put64(card.rec, AAR_CB_LOAD_BASE_HI, 0x0000000100400000ULL);
    zzapp_xport_cb_set_state(card.rec, AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    CHECK(aar_xport_status_get(&x, &st) == AAR_OK);
    CHECK(st.handle == x.handle && st.state == AAR_STATE_FAULTED &&
          st.end_reason == AAR_END_USER_FAULT && st.exit_code == -5 &&
          st.flags == (AAR_CBF_DISPLAY | AAR_CBF_CONSOLE) &&
          st.fault_class == AAR_FAULT_DATA && st.fault_syndrome == 0x805U &&
          st.fault_pstate == 0x60000010U && st.fault_pc == 0x00401234U &&
          st.fault_sp == 0x7fff0000U && st.fault_lr == 0x00400100U &&
          st.fault_addr == 0xdeadbeefU && st.load_base == 0x0000000100400000ULL);
    CHECK(st.shared_used == zzapp_xport_shared_used(card.rec));

    /* Stop: the word, and the mailbox op. */
    aar_xport_request_stop(&x);
    CHECK(zzapp_xport_cb_read_client(card.rec, AAR_CB_STOP_REQUEST) != 0U);
    CHECK(aar_xport_stop(&x) == AAR_OK && card.stopped);
    CHECK(aar_xport_status_get(&x, &st) == AAR_OK && st.state == AAR_STATE_IDLE &&
          st.end_reason == AAR_END_FORCED);

    /* Argument limits. */
    CHECK(aar_xport_release(&x) == AAR_OK);
    claim(&x);
    {
        const char *toomany[17] = { "a" };
        char longarg[200];

        memset(longarg, 'x', sizeof(longarg) - 1U);
        longarg[sizeof(longarg) - 1U] = '\0';
        CHECK(aar_xport_load_memory(&x, image, 100U, 0U) == AAR_OK);
        CHECK(aar_xport_start(&x, toomany, 17U) == AAR_E_INVAL);
        toomany[0] = longarg;
        CHECK(aar_xport_start(&x, toomany, 1U) == AAR_E_INVAL);
        CHECK(aar_xport_start(&x, NULL, 0U) == AAR_OK && card.argc == 0U);
    }
    CHECK(aar_xport_release(&x) == AAR_OK && heap.live == 0U);
}

static void test_load_refused_and_unknown_caps(void)
{
    aar_xport x;
    static uint8_t image[1000];
    volatile uint8_t *staging;
    uint32_t chunk_max;

    reset_all();
    claim(&x);
    CHECK(aar_xport_load_begin(&x, 0x80000000U, &staging, &chunk_max) == AAR_E_INVAL);
    /* The card cannot allocate the load buffer: the refusal reason arrives. */
    {
        zzapp_xport_buf_t hog;

        zzapp_xport_set_shared_limit(card.rec, 0x4000U);   /* the pair alone fills it */
        (void)hog;
    }
    CHECK(aar_xport_load_memory(&x, image, sizeof(image), 0U) == AAR_E_NORESOURCE);
    CHECK(aar_xport_reason(&x) == AAR_REASON_HOSTHEAP);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_lease(void)
{
    aar_xport x;
    uint32_t seen[4];

    reset_all();
    claim(&x);
    seen[0] = aar_get_be32(x.cb + AAR_CB_LEASE_COUNTER);
    CHECK(aar_xport_lease_renew(&x) == AAR_OK);
    seen[1] = aar_get_be32(x.cb + AAR_CB_LEASE_COUNTER);
    CHECK(aar_xport_lease_renew(&x) == AAR_OK);
    seen[2] = aar_get_be32(x.cb + AAR_CB_LEASE_COUNTER);
    CHECK(seen[0] == 0U && seen[1] == 1U && seen[2] == 2U);
    /* The word never reads 0 again, also across the wrap. */
    x.lease = 0xffffffffU;
    CHECK(aar_xport_lease_renew(&x) == AAR_OK);
    seen[3] = aar_get_be32(x.cb + AAR_CB_LEASE_COUNTER);
    CHECK(seen[3] == 1U);
    CHECK(zzapp_xport_cb_read_client(card.rec, AAR_CB_LEASE_COUNTER) == 1U);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_staleness_after_release(void)
{
    aar_xport x;
    aar_xport_req req;
    aar_event ev = { .type = AAR_EV_KEY_DOWN };
    uint8_t m[AAR_MSG_BYTES] = { 7 }, got[AAR_MSG_BYTES];
    aar_xport_status st;
    uint32_t old;

    reset_all();
    heap.keep_freed = 1;
    claim(&x);
    old = x.handle;
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    CHECK(aar_xport_service_next(&x, &req) == 0);   /* the message is in the inbox now */

    CHECK(aar_xport_release(&x) == AAR_OK);
    CHECK(card.released && heap.live == 0U && heap.dead_count == 1U);
    /* Everything answers STALE, and nothing writes to the freed block. */
    CHECK(aar_xport_lease_renew(&x) == AAR_E_STALE);
    CHECK(aar_xport_status_get(&x, &st) == AAR_E_STALE);
    CHECK(aar_xport_post_event(&x, &ev) == AAR_E_STALE);
    CHECK(aar_xport_send_message(&x, m) == AAR_E_STALE);
    CHECK(aar_xport_service_next(&x, &req) == AAR_E_STALE);
    CHECK(aar_xport_service_reply(&x, &req) == AAR_E_STALE);
    CHECK(aar_xport_stop(&x) == AAR_E_STALE);
    CHECK(aar_xport_set_limits(&x, 1U, 1U, 1U) == AAR_E_STALE);
    CHECK(aar_xport_load_memory(&x, m, sizeof(m), 0U) == AAR_E_STALE);
    CHECK(aar_xport_map(&x, 1U, 0U, 1U) == NULL);
    aar_xport_request_stop(&x);
    aar_xport_kick(&x, ZZAPP_KICK_CQ_POSTED);
    aar_xport_flush(&x);
    CHECK(graveyard_intact());
    CHECK(card.kicks == 0U);
    /* The message that was already in the inbox can still be read. */
    CHECK(aar_xport_get_message(&x, got) == 1 && got[0] == 7U);
    CHECK(aar_xport_get_message(&x, got) == AAR_E_STALE);
    /* Releasing again is harmless. */
    CHECK(aar_xport_release(&x) == AAR_OK && card.calls > 0U);

    /* The index is reused by the next claim, with a newer generation: the old
     * handle is stale on the card. */
    {
        aar_xport y;
        unsigned n;

        for (n = 0U; n < 40U; n++) {
            claim(&y);
            if (aar_handle_index(y.handle) == aar_handle_index(old))
                break;
            CHECK(aar_xport_release(&y) == AAR_OK);
        }
        CHECK(n < 40U && aar_handle_index(y.handle) == aar_handle_index(old));
        CHECK(aar_handle_generation(y.handle) != aar_handle_generation(old));
        {
            int status = 0;

            CHECK(zzapp_xport_resolve(old, &status) == NULL && status == AAR_E_STALE);
            CHECK(zzapp_xport_resolve(y.handle, &status) == card.rec);
        }
        CHECK(aar_xport_release(&y) == AAR_OK);
    }
}

static void test_staleness_after_revocation(void)
{
    aar_xport x;
    aar_xport_req req;
    aar_xport_status st;
    uint32_t before;
    int rc;

    reset_all();
    heap.keep_freed = 1;
    claim(&x);
    CHECK(aar_xport_lease_renew(&x) == AAR_OK);
    /* The card revoked the handle (lease lost, quarantine) but holds the
     * shell. The client does not know yet: its mapping is still the card's,
     * so renewing is harmless. The first mailbox call it makes answers STALE
     * and the card releases the shell then. */
    card.revoked = x.handle;
    before = aar_get_be32(x.cb + AAR_CB_LEASE_COUNTER);
    CHECK(aar_xport_lease_renew(&x) == AAR_OK);
    CHECK(aar_get_be32(x.cb + AAR_CB_LEASE_COUNTER) == before + 1U);
    CHECK(heap.live == 1U);
    rc = aar_xport_stop(&x);
    CHECK(rc == AAR_E_STALE);
    CHECK(!aar_xport_live(&x) && aar_xport_handle(&x) == 0U);
    CHECK(heap.live == 0U && heap.dead_count == 1U);
    /* After that the module never touches the memory again. */
    CHECK(aar_xport_lease_renew(&x) == AAR_E_STALE);
    CHECK(aar_xport_status_get(&x, &st) == AAR_E_STALE);
    CHECK(aar_xport_service_next(&x, &req) == AAR_E_STALE);
    aar_xport_request_stop(&x);
    CHECK(graveyard_intact());

    /* Revocation seen through a long call: the lease tick notices it. */
    card.revoked = 0U;
    claim(&x);
    card.revoked = x.handle;
    CHECK(aar_xport_load_memory(&x, "abcd", 4U, 0U) == AAR_E_STALE);
    CHECK(!aar_xport_live(&x) && heap.live == 0U && graveyard_intact());

    /* Release of a revoked claim by its owner is fine too. */
    card.revoked = 0U;
    claim(&x);
    card.revoked = x.handle;
    CHECK(aar_xport_release(&x) == AAR_OK);
    CHECK(heap.live == 0U && graveyard_intact());
}

static void test_teardown_returns_everything(void)
{
    aar_xport x;
    zzapp_rec_stats_t st0, st1;
    zzapp_xport_buf_t b;
    static uint8_t image[70000];
    const char *argv[1] = { "app" };
    unsigned i;

    reset_all();
    zzapp_rec_stats(&st0);
    for (i = 0U; i < 5U; i++) {
        claim(&x);
        CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_LOG, &b) == AAR_OK);
        CHECK(zzapp_xport_data_reserve(card.rec, 0x20000U, &b) == AAR_OK);
        CHECK(zzapp_xport_block_alloc(card.rec, ZZAPP_XPORT_BLOCK_AUDIO,
                                      ZZAPP_XPORT_AUDIO_BYTES(8192U), &b) == AAR_OK);
        CHECK(aar_xport_load_memory(&x, image, sizeof(image), 0U) == AAR_OK);
        if (i & 1U)
            CHECK(aar_xport_start(&x, argv, 1U) == AAR_OK);
        CHECK(heap.live >= 4U);
        CHECK(aar_xport_release(&x) == AAR_OK);
        CHECK(heap.live == 0U && heap.bad_frees == 0U);
    }
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free && st1.claimed == st0.claimed);
}

static void write_le32(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t read_le32(const volatile uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_audio_ring(void)
{
    aar_xport x;
    zzapp_xport_buf_t ring;
    volatile uint8_t *r;
    int16_t out[64 * 2];
    uint32_t got, i;
    const uint32_t frames = 8192U;

    reset_all();
    claim(&x);
    CHECK(zzapp_xport_block_alloc(card.rec, ZZAPP_XPORT_BLOCK_AUDIO,
                                  ZZAPP_XPORT_AUDIO_BYTES(frames), &ring) == AAR_OK);
    r = ring.ptr;
    CHECK(aar_xport_audio_read(&x, out, 10U, &got) == AAR_E_INVAL);   /* not attached */
    /* A header that does not say S16LE stereo is refused. */
    CHECK(aar_xport_audio_attach(&x, ring.token, ring.bytes, frames) == AAR_E_IO);
    write_le32(r + AAR_AUDIO_RING_RATE, 44100U);
    write_le32(r + AAR_AUDIO_RING_CHANNELS, 2U);
    write_le32(r + AAR_AUDIO_RING_FORMAT, AAR_AUDIO_FORMAT_S16LE);
    CHECK(aar_xport_audio_attach(&x, ring.token, ring.bytes, 1000U) == AAR_E_INVAL);
    CHECK(aar_xport_audio_attach(&x, ring.token, ring.bytes, frames) == AAR_OK);
    CHECK(aar_xport_audio_available(&x) == 0U);

    /* The app writes 40 frames. The client reads 64: 40 arrive, one underrun. */
    for (i = 0U; i < 40U; i++) {
        int16_t l = (int16_t)(-1000 + (int)i * 7), rr = (int16_t)(i * 300);

        r[0x80 + i * 4 + 0] = (uint8_t)l;           r[0x80 + i * 4 + 1] = (uint8_t)((uint16_t)l >> 8);
        r[0x80 + i * 4 + 2] = (uint8_t)rr;          r[0x80 + i * 4 + 3] = (uint8_t)((uint16_t)rr >> 8);
    }
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 40U);
    CHECK(aar_xport_audio_available(&x) == 40U);
    CHECK(aar_xport_audio_read(&x, out, 64U, &got) == AAR_OK && got == 40U);
    for (i = 0U; i < 40U; i++)
        CHECK(out[i * 2] == (int16_t)(-1000 + (int)i * 7) && out[i * 2 + 1] == (int16_t)(i * 300));
    CHECK(read_le32(r + AAR_AUDIO_RING_READ_FRAME) == 40U);
    CHECK(read_le32(r + AAR_AUDIO_RING_UNDERRUNS) == 1U);
    CHECK(aar_xport_audio_available(&x) == 0U);
    /* An exact read is no underrun. */
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 50U);
    CHECK(aar_xport_audio_read(&x, out, 10U, &got) == AAR_OK && got == 10U);
    CHECK(read_le32(r + AAR_AUDIO_RING_UNDERRUNS) == 1U);

    /* Free-running indices across the wrap of the ring and of 2^32. */
    write_le32(r + AAR_AUDIO_RING_READ_FRAME, 0xfffffff0U);
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 0x00000010U);       /* 32 frames */
    for (i = 0U; i < 32U; i++) {
        uint32_t at = 0x80U + (((0xfffffff0U + i) & (frames - 1U)) * 4U);

        write_le32(r + at, 0x12340000U + i);                       /* l = i, r = 0x1234 */
    }
    CHECK(aar_xport_audio_available(&x) == 32U);
    CHECK(aar_xport_audio_read(&x, out, 32U, &got) == AAR_OK && got == 32U);
    for (i = 0U; i < 32U; i++)
        CHECK(out[i * 2] == (int16_t)i && out[i * 2 + 1] == 0x1234);
    CHECK(read_le32(r + AAR_AUDIO_RING_READ_FRAME) == 0x10U);
    /* A write index that runs further ahead than the ring is corrupt. */
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 0x10U + frames + 1U);
    CHECK(aar_xport_audio_read(&x, out, 4U, &got) == AAR_E_IO && got == 0U);
    aar_xport_audio_detach(&x);
    CHECK(aar_xport_audio_read(&x, out, 4U, &got) == AAR_E_INVAL);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

static void test_corrupt_queue(void)
{
    aar_xport x;
    aar_xport_req req;
    aar_event ev = { .type = AAR_EV_KEY_DOWN };

    reset_all();
    claim(&x);
    aar_put_be32(x.cb + AAR_CB_SQ_TAIL, 1000U);      /* more than the depth */
    CHECK(aar_xport_service_next(&x, &req) == AAR_E_IO);
    CHECK(aar_xport_get_message(&x, &req) == AAR_E_IO);
    aar_put_be32(x.cb + AAR_CB_SQ_TAIL, 0U);
    aar_put_be32(x.cb + AAR_CB_CQ_HEAD, 5000U);      /* the app side's index */
    CHECK(aar_xport_post_event(&x, &ev) == AAR_E_IO);
    CHECK(aar_xport_send_message(&x, "m") == AAR_E_IO);
    aar_put_be32(x.cb + AAR_CB_CQ_HEAD, 0U);
    aar_put_be32(x.cb + AAR_CB_MSG_CONSUMED, 100U);  /* ahead of the posted count */
    CHECK(aar_xport_send_message(&x, "m") == AAR_E_IO);
    CHECK(aar_xport_release(&x) == AAR_OK);
}

/* ---- Two threads, one per role ------------------------------------------------------ */

static void full_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#define ROUNDS 3000U

static aar_xport tx;
static volatile int fw_done, cl_stop;
static uint32_t cl_events_posted, cl_events_dropped_seen, cl_msgs_sent;
static uint32_t fw_events_taken, fw_msgs_taken, fw_requests_done;
static int thread_error;

static void *client_thread(void *arg)
{
    aar_xport_req req;
    uint8_t m[AAR_MSG_BYTES];
    uint8_t got[AAR_MSG_BYTES];
    uint32_t next_in = 0U, seq = 0U, ev_seq = 0U;

    (void)arg;
    memset(m, 0, sizeof(m));
    while (!cl_stop || cl_msgs_sent < ROUNDS) {
        int rc = aar_xport_service_next(&tx, &req);

        if (rc < 0) { thread_error = 1; return NULL; }
        if (rc == 1) {
            req.result = AAR_OK;
            req.out[0] = req.arg[0] * 2U + 1U;
            while (aar_xport_service_reply(&tx, &req) == AAR_E_BUSY)
                ;
        }
        while (aar_xport_get_message(&tx, got) == 1) {
            uint32_t v = (uint32_t)got[0] | ((uint32_t)got[1] << 8) |
                         ((uint32_t)got[2] << 16) | ((uint32_t)got[3] << 24);

            if (v != next_in)
                thread_error = 2;
            next_in++;
        }
        if (cl_msgs_sent < ROUNDS) {
            m[0] = (uint8_t)seq; m[1] = (uint8_t)(seq >> 8);
            m[2] = (uint8_t)(seq >> 16); m[3] = (uint8_t)(seq >> 24);
            if (aar_xport_send_message(&tx, m) == AAR_OK) {
                seq++;
                cl_msgs_sent++;
            }
        }
        {
            aar_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.type = AAR_EV_KEY_DOWN;
            ev.time_ms = ev_seq;
            if (aar_xport_post_event(&tx, &ev) == AAR_OK)
                cl_events_posted++;
            ev_seq++;
        }
        if ((seq & 0x3fU) == 0U)
            (void)aar_xport_lease_renew(&tx);
    }
    cl_events_dropped_seen = aar_get_be32(tx.cb + AAR_CB_EVENTS_DROPPED);
    return NULL;
}

static void test_two_threads(void)
{
    pthread_t t;
    uint32_t round, expect_msg = 0U, last_time = 0U;
    int first_event = 1;
    uint8_t m[AAR_MSG_BYTES] = { 0 }, got[AAR_MSG_BYTES];
    aar_event ev;
    aar_completion c;
    uint32_t out_msgs = 0U;

    reset_all();
    /* Both ends use a real fence, as the card and the 68k's bus do. */
    {
        zzapp_xport_env_t e;

        memset(&e, 0, sizeof(e));
        e.alloc = heap_alloc; e.release = heap_release;
        e.lo = HOST_VISIBLE_HEAP_ADDRESS; e.hi = 0x08100000U;
        e.token_adjust = TOKEN_ADJUST;
        e.acquire = full_fence; e.release_barrier = full_fence;
        zzapp_xport_init(&e);
    }
    board.acquire = full_fence;
    board.release = full_fence;
    claim(&tx);
    fw_done = cl_stop = 0;
    cl_events_posted = cl_events_dropped_seen = cl_msgs_sent = 0U;
    fw_events_taken = fw_msgs_taken = fw_requests_done = 0U;
    thread_error = 0;
    CHECK(pthread_create(&t, NULL, client_thread, NULL) == 0);

    /* The firmware side: one request per round, waiting for its completion
     * while pumping; messages both ways; events taken as they come. */
    for (round = 0U; round < ROUNDS; round++) {
        uint32_t id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, round,
                                 0U, 0U, 0U, 0U);

        for (;;) {
            int pr = zzapp_xport_pump(card.rec);

            if (pr < 0) { thread_error = 3; break; }
            while (zzapp_xport_take_event(card.rec, &ev) == 1) {
                if (!first_event && ev.time_ms <= last_time)
                    thread_error = 4;
                first_event = 0;
                last_time = ev.time_ms;
                fw_events_taken++;
            }
            while (zzapp_xport_take_msg(card.rec, got) == 1) {
                uint32_t v = (uint32_t)got[0] | ((uint32_t)got[1] << 8) |
                             ((uint32_t)got[2] << 16) | ((uint32_t)got[3] << 24);

                if (v != expect_msg)
                    thread_error = 5;
                expect_msg++;
                fw_msgs_taken++;
            }
            if (zzapp_xport_take_completion(card.rec, ZZAPP_XPORT_REQ_BLOCKING, &c) == 1) {
                if (c.id != id || c.out[0] != round * 2U + 1U)
                    thread_error = 6;
                fw_requests_done++;
                break;
            }
            sched_yield();
        }
        if (thread_error)
            break;
        /* A message to the client now and then. */
        m[0] = (uint8_t)out_msgs; m[1] = (uint8_t)(out_msgs >> 8);
        m[2] = (uint8_t)(out_msgs >> 16); m[3] = (uint8_t)(out_msgs >> 24);
        if (zzapp_xport_post_msg(card.rec, m) == AAR_OK)
            out_msgs++;
    }
    cl_stop = 1;
    pthread_join(t, NULL);
    /* Drain what is left. */
    (void)zzapp_xport_pump(card.rec);
    while (zzapp_xport_take_event(card.rec, &ev) == 1)
        fw_events_taken++;
    while (zzapp_xport_take_msg(card.rec, got) == 1) {
        uint32_t v = (uint32_t)got[0] | ((uint32_t)got[1] << 8) |
                     ((uint32_t)got[2] << 16) | ((uint32_t)got[3] << 24);

        if (v != expect_msg)
            thread_error = 5;
        expect_msg++;
        fw_msgs_taken++;
    }
    while (zzapp_xport_pump(card.rec) > 0) {
        while (zzapp_xport_take_event(card.rec, &ev) == 1)
            fw_events_taken++;
        while (zzapp_xport_take_msg(card.rec, got) == 1) {
            expect_msg++;
            fw_msgs_taken++;
        }
    }
    CHECK(thread_error == 0);
    CHECK(fw_requests_done == ROUNDS);
    CHECK(fw_msgs_taken == cl_msgs_sent && cl_msgs_sent == ROUNDS);
    /* Every event is either taken, dropped by the client's count or still in
     * the firmware's ring when the record ring was full (counted). */
    CHECK(fw_events_taken + zzapp_xport_stats(card.rec)->events_dropped <=
          cl_events_posted);
    CHECK(fw_events_taken > 0U);
    CHECK(zzapp_xport_stats(card.rec)->msgs_overflow == 0U);
    CHECK(zzapp_xport_stats(card.rec)->io_errors == 0U);
    CHECK(aar_xport_release(&tx) == AAR_OK);
    (void)cl_events_dropped_seen;
    (void)out_msgs;
}

int main(void)
{
    test_open_and_refusal();
    test_query_and_list();
    test_requests_end_to_end();
    test_mapping_cache();
    test_bad_buffer_is_refused();
    test_messages_and_credits();
    test_inbox_overflow_is_counted();
    test_events_coalescing_and_drops();
    test_logs();
    test_log_sink();
    test_doorbell();
    test_load_start_stop_status();
    test_load_refused_and_unknown_caps();
    test_lease();
    test_staleness_after_release();
    test_staleness_after_revocation();
    test_teardown_returns_everything();
    test_audio_ring();
    test_corrupt_queue();
    test_two_threads();
    printf("aar_xport_zz9000: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
