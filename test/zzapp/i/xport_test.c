/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The AAR transport core (zzapp_xport.[ch]) on the host: admission and
 * release of the pair and the service buffers through an injected allocator,
 * the request table, the pump and its dispatch rules, message credits, ids,
 * corrupt indices, the event ring, logs, the doorbell, handle staleness and
 * the barrier hooks. The client side is the reference model of
 * aar/transport.h, bound from the control block descriptors the way the
 * Amiga library binds it, so the firmware code and the model meet in memory.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"
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

/* ---- Record arena -------------------------------------------------------- */

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

/* ---- The injected host-visible heap --------------------------------------- */

#define HEAP_BLOCKS 256U
#define TOKEN_ADJUST 0x08000000U

typedef struct {
    void *ptr;
    uint32_t card;
    uint32_t bytes;
    uint32_t align;
} heap_block_t;

static struct {
    heap_block_t block[HEAP_BLOCKS];
    uint32_t next_card;
    uint32_t allocs, frees, bad_frees;
    uint32_t live, live_bytes;
    uint32_t last_lo, last_hi, last_align, last_bytes;
    int32_t fail_after;      /* allocations that still succeed; -1 never fail */
    int refuse_release;      /* release() answers -1 */
    int trace;               /* 1: log every call into `log` */
    char log[256];
} heap;

static void heap_reset(void)
{
    uint32_t i;

    for (i = 0U; i < HEAP_BLOCKS; i++)
        if (heap.block[i].ptr)
            free(heap.block[i].ptr);
    memset(&heap, 0, sizeof(heap));
    heap.next_card = 0x08200000U;
    heap.fail_after = -1;
}

static int heap_alloc(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
                      uint32_t hi, void **ptr, uint32_t *card)
{
    uint32_t i;
    void *p;

    (void)ctx;
    heap.last_lo = lo;
    heap.last_hi = hi;
    heap.last_align = align;
    heap.last_bytes = bytes;
    if (heap.fail_after == 0)
        return -1;
    if (heap.fail_after > 0)
        heap.fail_after--;
    for (i = 0U; i < HEAP_BLOCKS && heap.block[i].ptr; i++)
        ;
    if (i == HEAP_BLOCKS)
        return -1;
    p = aligned_alloc(align, bytes);
    if (!p)
        return -1;
    memset(p, 0xA5, bytes); /* the heap does not zero: the code under test must */
    heap.next_card = (heap.next_card + align - 1U) & ~(align - 1U);
    heap.block[i].ptr = p;
    heap.block[i].card = heap.next_card;
    heap.block[i].bytes = bytes;
    heap.block[i].align = align;
    heap.next_card += bytes + 0x2000U;
    heap.allocs++;
    heap.live++;
    heap.live_bytes += bytes;
    *ptr = p;
    *card = heap.block[i].card;
    return 0;
}

static int heap_release(void *ctx, void *ptr, uint32_t card)
{
    uint32_t i;

    (void)ctx;
    if (heap.refuse_release)
        return -1;
    for (i = 0U; i < HEAP_BLOCKS; i++) {
        if (heap.block[i].ptr == ptr && heap.block[i].card == card) {
            heap.live--;
            heap.live_bytes -= heap.block[i].bytes;
            heap.frees++;
            free(ptr);
            memset(&heap.block[i], 0, sizeof(heap.block[i]));
            return 0;
        }
    }
    heap.bad_frees++;
    return -1;
}

/* What the 68k does with a token: board base plus token is the card address. */
static void *map_token(uint32_t token)
{
    uint32_t card = token + TOKEN_ADJUST;
    uint32_t i;

    for (i = 0U; i < HEAP_BLOCKS; i++)
        if (heap.block[i].ptr && card >= heap.block[i].card &&
            card < heap.block[i].card + heap.block[i].bytes)
            return (uint8_t *)heap.block[i].ptr + (card - heap.block[i].card);
    return NULL;
}

/* ---- Barrier hooks that record what they saw ------------------------------- */

static unsigned acquire_calls, release_calls;
static void (*release_probe)(void);

static void hook_acquire(void) { acquire_calls++; }
static void hook_release(void)
{
    release_calls++;
    if (release_probe)
        release_probe();
}

static void env_set(void)
{
    zzapp_xport_env_t e;

    memset(&e, 0, sizeof(e));
    e.alloc = heap_alloc;
    e.release = heap_release;
    e.lo = HOST_VISIBLE_HEAP_ADDRESS;
    e.hi = 0x08100000U;
    e.token_adjust = TOKEN_ADJUST;
    e.acquire = hook_acquire;
    e.release_barrier = hook_release;
    zzapp_xport_init(&e);
}

/* ---- The client side: the reference model, bound from the descriptors ------ */

typedef struct {
    volatile uint8_t *cb;
    aar_queue sq, cq;
    uint32_t posted;  /* messages posted, the client's own count */
} client_t;

static int client_attach(client_t *c, uint32_t cb_token)
{
    volatile uint8_t *cb = map_token(cb_token);
    uint32_t t;

    memset(c, 0, sizeof(*c));
    if (!cb || aar_cb_check(cb) != AAR_OK)
        return -1;
    c->cb = cb;
    /* Ring location from the descriptor: token and offset. */
    t = aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_TOKEN);
    if (aar_cb_queue_bind(&c->sq, (volatile void *)cb,
                          (uint8_t *)map_token(t) +
                          aar_get_be32(cb + AAR_CB_SQ_DESC + AAR_QD_OFFSET),
                          AAR_QUEUE_SQ) != AAR_OK)
        return -1;
    t = aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_TOKEN);
    if (aar_cb_queue_bind(&c->cq, (volatile void *)cb,
                          (uint8_t *)map_token(t) +
                          aar_get_be32(cb + AAR_CB_CQ_DESC + AAR_QD_OFFSET),
                          AAR_QUEUE_CQ) != AAR_OK)
        return -1;
    return 0;
}

static uint32_t cb_token_of(zzapp_rec_t *rec)
{
    zzapp_xport_buf_t b;

    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &b) == 1);
    return b.token;
}

/* A claim: a CLAIMED record and its pair. */
static zzapp_rec_t *claim(client_t *c)
{
    zzapp_rec_t *rec = NULL;
    uint32_t handle = 0U;

    CHECK(zzapp_rec_admit(&handle, &rec) == 0U);
    CHECK(rec && handle != 0U);
    CHECK(zzapp_xport_admit(rec) == AAR_REASON_NONE);
    if (c)
        CHECK(client_attach(c, cb_token_of(rec)) == 0);
    return rec;
}

/* Everything the client may rely on after the claim ends: return it all. */
static void unclaim(zzapp_rec_t *rec)
{
    uint32_t handle = rec->hdr.handle;

    CHECK(zzapp_xport_release(rec) == 0);
    CHECK(zzapp_rec_free(handle) == 0);
}

static void set_indices(client_t *c, uint32_t v)
{
    aar_put_be32(c->sq.head, v);
    aar_put_be32(c->sq.tail, v);
    aar_put_be32(c->cq.head, v);
    aar_put_be32(c->cq.tail, v);
}

static uint32_t word(client_t *c, uint32_t off) { return aar_get_be32(c->cb + off); }

static void reset_all(void)
{
    heap_reset();
    env_set();
    acquire_calls = release_calls = 0U;
    release_probe = NULL;
    setup_arena(16U);
}

/* ---- Admission and release ---------------------------------------------------- */

static void test_admission(void)
{
    client_t c;
    zzapp_rec_t *rec;
    zzapp_xport_buf_t pair;
    uint32_t before_free, handle;
    zzapp_rec_stats_t st0, st1;

    reset_all();
    zzapp_rec_stats(&st0);
    rec = claim(&c);
    CHECK(heap.allocs == 1U && heap.live == 1U);
    CHECK(heap.last_bytes == 0x4000U && heap.last_align == 0x2000U);
    CHECK(heap.last_lo == HOST_VISIBLE_HEAP_ADDRESS && heap.last_hi == 0x08100000U);
    CHECK(zzapp_xport_live(rec));
    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
    CHECK(pair.bytes == 0x4000U && pair.off == 0U);
    CHECK((heap.block[0].card & 0x1fffU) == 0U);
    CHECK(pair.token == heap.block[0].card - TOKEN_ADJUST);
    CHECK(((uintptr_t)pair.ptr & 0x1fffU) == 0U);

    /* The control block is complete and describes the pair. */
    CHECK(aar_cb_check(c.cb) == AAR_OK);
    CHECK(word(&c, AAR_CB_HANDLE) == rec->hdr.handle);
    CHECK(word(&c, AAR_CB_PROFILE) == AAR_PROFILE_A32);
    CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_CLAIMED);
    CHECK(word(&c, AAR_CB_SIZE) == AAR_CB_BYTES);
    CHECK(word(&c, AAR_CB_SQ_DESC + AAR_QD_TOKEN) == pair.token);
    CHECK(word(&c, AAR_CB_SQ_DESC + AAR_QD_OFFSET) == AAR_PAIR_SQ_OFFSET);
    CHECK(word(&c, AAR_CB_CQ_DESC + AAR_QD_TOKEN) == pair.token);
    CHECK(word(&c, AAR_CB_CQ_DESC + AAR_QD_OFFSET) == aar_pair_cq_offset(64U));
    CHECK(word(&c, AAR_CB_SQ_DESC + AAR_QD_DEPTH) == 64U);
    CHECK(word(&c, AAR_CB_CQ_DESC + AAR_QD_DEPTH) == 64U);
    CHECK(c.sq.depth == 64U && c.cq.depth == 64U);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x4000U &&
          word(&c, AAR_CB_SHARED_USED_HI) == 0U);
    CHECK(zzapp_xport_shared_used(rec) == 0x4000U);
    /* The heap's 0xA5 garbage is gone from the rings and the rest of the CB. */
    {
        const uint8_t *p = pair.ptr;
        uint32_t i;
        int dirty = 0;

        for (i = 0U; i < 0x4000U; i++)
            if (p[i] == 0xA5U)
                dirty = 1;
        CHECK(!dirty);
    }
    CHECK(word(&c, AAR_CB_SQ_TAIL) == 0U && word(&c, AAR_CB_CQ_HEAD) == 0U);
    CHECK(release_calls >= 1U);  /* a release barrier before the record is published */
    CHECK(zzapp_xport_admit(rec) == AAR_REASON_NONE && heap.allocs == 1U);

    /* Nothing is allocated for the service buffers until they are used. */
    CHECK(!zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_SERVICE, NULL));
    CHECK(!zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, NULL));

    handle = rec->hdr.handle;
    CHECK(zzapp_xport_release(rec) == 0);
    CHECK(heap.live == 0U && heap.frees == 1U && heap.bad_frees == 0U);
    CHECK(!zzapp_xport_live(rec));
    CHECK(zzapp_xport_release(rec) == 0 && heap.frees == 1U);   /* idempotent */
    CHECK(zzapp_rec_free(handle) == 0);
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free);

    /* A refusal leaves nothing behind. */
    heap.fail_after = 0;
    rec = NULL;
    before_free = 0U;
    CHECK(zzapp_rec_admit(&handle, &rec) == 0U);
    CHECK(zzapp_xport_admit(rec) == AAR_REASON_HOSTHEAP);
    CHECK(!zzapp_xport_live(rec) && heap.live == 0U);
    CHECK(zzapp_xport_shared_used(rec) == 0U);
    CHECK(zzapp_xport_cb_get(rec, AAR_CB_MAGIC) == 0U);
    CHECK(zzapp_rec_unadmit(handle) == 0);
    (void)before_free;
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free);
}

static void test_service_and_blocks(void)
{
    client_t c;
    zzapp_rec_t *rec;
    zzapp_xport_buf_t a, b, blk;
    unsigned i;
    const uint8_t *p;

    reset_all();
    rec = claim(&c);

    /* The service block is allocated on first use, once, and zeroed. */
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_LOG, &a) == AAR_OK);
    CHECK(heap.allocs == 2U && heap.last_bytes == ZZAPP_XPORT_SERVICE_BYTES &&
          heap.last_align == 0x2000U);
    CHECK(a.off == ZZAPP_XPORT_LOG_OFF && a.bytes == ZZAPP_XPORT_LOG_BYTES);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &b) == AAR_OK);
    CHECK(heap.allocs == 2U);
    CHECK(b.off == 0U && b.bytes == 0x0400U && b.token == a.token);
    CHECK(map_token(b.token) == b.ptr);
    CHECK((uint8_t *)a.ptr - (uint8_t *)b.ptr == (ptrdiff_t)ZZAPP_XPORT_LOG_OFF);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &b) == AAR_OK &&
          b.off == 0x1000U && b.bytes == 0x1000U);
    /* The palette area: shadow, then the stage, clear of every other area. */
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_PALETTE, &b) == AAR_OK &&
          b.off == 0x0400U && b.bytes == 0x0800U &&
          b.off + b.bytes <= ZZAPP_XPORT_CONSOLE_IN_OFF &&
          ZZAPP_XPORT_PALETTE_STAGE + ZZAPP_XPORT_PALETTE_ENTRIES * 4U <= b.bytes);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_OUT, &b) == AAR_OK &&
          b.off == 0x4000U && b.bytes == 0x8000U);
    CHECK(b.off + b.bytes == ZZAPP_XPORT_SERVICE_BYTES);
    CHECK(zzapp_xport_service_area(rec, (zzapp_xport_area_t)9, &b) == AAR_E_INVAL);
    zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_SERVICE, &blk);
    for (p = blk.ptr, i = 0U; i < ZZAPP_XPORT_SERVICE_BYTES; i++)
        if (p[i] != 0U) {
            CHECK(0 && "service block zeroed");
            break;
        }
    CHECK(zzapp_xport_shared_used(rec) == 0x4000U + 0xC000U);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x10000U);

    /* The data block grows in steps and loses its contents when it does. */
    CHECK(zzapp_xport_data_reserve(rec, 100U, &a) == AAR_OK && a.bytes == 0x2000U);
    memset(a.ptr, 0x11, a.bytes);
    CHECK(zzapp_xport_data_reserve(rec, 0x1000U, &b) == AAR_OK && b.ptr == a.ptr);
    CHECK(zzapp_xport_data_reserve(rec, 0x5000U, &b) == AAR_OK && b.bytes == 0x6000U);
    CHECK(((uint8_t *)b.ptr)[0] == 0U);                     /* zeroed again */
    CHECK(zzapp_xport_data_reserve(rec, ZZAPP_XPORT_DATA_MAX, &b) == AAR_OK &&
          b.bytes == ZZAPP_XPORT_DATA_MAX);
    CHECK(zzapp_xport_data_reserve(rec, ZZAPP_XPORT_DATA_MAX + 1U, &b) == AAR_E_INVAL);
    CHECK(zzapp_xport_data_reserve(rec, 0U, &b) == AAR_E_INVAL);
    CHECK(zzapp_xport_shared_used(rec) == 0x10000U + ZZAPP_XPORT_DATA_MAX);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x10000U + ZZAPP_XPORT_DATA_MAX);

    /* Audio ring and load buffer, rounded to the step. */
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_AUDIO,
                                  AAR_AUDIO_RING_HDR_BYTES + 8192U * 4U, &a) == AAR_OK);
    CHECK(a.bytes == ZZAPP_XPORT_AUDIO_BYTES(8192U) && a.bytes == 0xA000U);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_AUDIO, 1U, &a) == AAR_E_BUSY);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD,
                                  ZZAPP_LOAD_BUFFER_BYTES, &b) == AAR_OK);
    CHECK(b.bytes == ZZAPP_LOAD_BUFFER_BYTES);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_PAIR, 8192U, &a) == AAR_E_INVAL);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0U, &a) == AAR_E_INVAL);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_LOAD) == AAR_OK);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_LOAD) == AAR_OK);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_PAIR) == AAR_E_INVAL);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO) == AAR_OK);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_DATA) == AAR_OK);
    CHECK(zzapp_xport_shared_used(rec) == 0x10000U);

    /* The limit counts everything, the pair included, but never refuses the
     * pair (it is allocated before any limit exists). */
    zzapp_xport_set_shared_limit(rec, 0x10000U + 0x2000U);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x4000U, &a) ==
          AAR_E_NOMEM);
    CHECK(!zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, NULL));
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x2000U, &a) == AAR_OK);
    CHECK(zzapp_xport_shared_used(rec) == 0x12000U);
    zzapp_xport_set_shared_limit(rec, 0U);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_LOAD) == AAR_OK);

    /* A heap that says no. */
    heap.fail_after = 0;
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x2000U, &a) ==
          AAR_E_NOMEM);
    CHECK(zzapp_xport_shared_used(rec) == 0x10000U);
    heap.fail_after = -1;

    /* Release returns every block, the pair last, and the heap is empty. */
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_DATA, 0x2000U, &a) == AAR_OK);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_AUDIO, 0x2000U, &a) == AAR_OK);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x2000U, &a) == AAR_OK);
    CHECK(heap.live == 5U);
    unclaim(rec);
    CHECK(heap.live == 0U && heap.live_bytes == 0U && heap.bad_frees == 0U);
    CHECK(heap.frees == heap.allocs);
}

/* AAR_CB_SHARED_USED is the runtime's blocks plus the app's own shared
 * allocations, and the limit counts both. */
static void test_shared_total(void)
{
    client_t c;
    zzapp_rec_t *rec;
    zzapp_xport_buf_t a;

    reset_all();
    rec = claim(&c);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x4000U);

    /* zzapp_host.c keeps visible.used and publishes after each app alloc. */
    rec->visible.used = 0x6000U;
    zzapp_xport_publish_shared(rec);
    CHECK(word(&c, AAR_CB_SHARED_USED_HI) == 0U && word(&c, AAR_CB_SHARED_USED_LO) == 0xA000U);
    CHECK(zzapp_xport_shared_used(rec) == 0x4000U);           /* runtime blocks only */
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_LOG, &a) == AAR_OK);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x4000U + 0xC000U + 0x6000U);
    CHECK(zzapp_xport_data_reserve(rec, 0x2000U, &a) == AAR_OK);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x4000U + 0xC000U + 0x2000U + 0x6000U);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_DATA) == AAR_OK);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x4000U + 0xC000U + 0x6000U);
    rec->visible.used = 0U;
    zzapp_xport_publish_shared(rec);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x10000U);

    /* The limit counts the app's blocks too. */
    rec->visible.used = 0x6000U;
    zzapp_xport_publish_shared(rec);
    zzapp_xport_set_shared_limit(rec, 0x10000U + 0x6000U + 0x2000U - 0x2000U);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x2000U, &a) == AAR_E_NOMEM);
    zzapp_xport_set_shared_limit(rec, 0x10000U + 0x6000U + 0x2000U);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD, 0x2000U, &a) == AAR_OK);
    CHECK(word(&c, AAR_CB_SHARED_USED_LO) == 0x18000U);
    rec->visible.used = 0U;
    unclaim(rec);
    zzapp_xport_publish_shared(rec);               /* no pair: harmless */
}

static void test_release_failure_and_replay(void)
{
    zzapp_rec_t *rec;
    zzapp_xport_buf_t a;

    reset_all();
    rec = claim(NULL);
    CHECK(zzapp_xport_data_reserve(rec, 0x2000U, &a) == AAR_OK);
    heap.refuse_release = 1;
    /* The allocator knows neither block: both leak, the record forgets them. */
    CHECK(zzapp_xport_release(rec) == -2);
    CHECK(!zzapp_xport_live(rec) && !zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, NULL));
    heap.refuse_release = 0;
    CHECK(zzapp_xport_release(rec) == 0);   /* a replay frees nothing twice */
    CHECK(heap.frees == 0U && heap.bad_frees == 0U);
    CHECK(zzapp_rec_free(rec->hdr.handle) == 0);

    /* A fresh claim of the same index starts clean. */
    rec = claim(NULL);
    CHECK(zzapp_xport_shared_used(rec) == 0x4000U);
    CHECK(zzapp_xport_msg_pending(rec) == 0U && zzapp_xport_event_pending(rec) == 0U);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
    CHECK(zzapp_xport_stats(rec)->pumped == 0U);
    unclaim(rec);
    /* Leaks of the refused run stay in the heap, not in the record. */
    heap_reset();
    env_set();
}

/* ---- Producer and consumer with the model on both sides ---------------------- */

static void fill_request(aar_request *rq, uint32_t i)
{
    uint32_t k;

    memset(rq, 0, sizeof(*rq));
    rq->op = (uint16_t)(AAR_OP_FILE_OPEN + (i % 6U));
    rq->flags = (uint16_t)(i * 7U);
    for (k = 0U; k < AAR_ENTRY_ARGS; k++)
        rq->arg[k] = i * 100U + k;
    rq->buf = 0x00300000U + i;
    rq->buf_off = i * 3U;
    rq->buf_len = i * 5U + 1U;
}

static void complete(client_t *c, const aar_request *rq, int32_t result, uint32_t salt)
{
    aar_completion done;
    uint32_t k;

    memset(&done, 0, sizeof(done));
    done.op = rq->op;
    done.flags = (uint16_t)(0x1200U + salt);
    done.id = rq->id;
    done.result = result;
    for (k = 0U; k < AAR_ENTRY_OUTS; k++)
        done.out[k] = salt * 10U + k;
    CHECK(aar_queue_post_completion(&c->cq, &done, hook_acquire, hook_release) == AAR_OK);
}

static void test_request_roundtrip_near_wrap(void)
{
    static const uint32_t starts[] = { 0U, 0xFFFFFFF0U, 0xFFFFFFFFU, 0x7FFFFFF0U };
    unsigned s;

    for (s = 0U; s < sizeof(starts) / sizeof(starts[0]); s++) {
        client_t c;
        zzapp_rec_t *rec;
        uint32_t i;

        reset_all();
        rec = claim(&c);
        set_indices(&c, starts[s]);
        for (i = 0U; i < 200U; i++) {
            zzapp_xport_req_kind_t kind = (zzapp_xport_req_kind_t)(i % 3U);
            aar_request rq, seen;
            aar_completion got;
            const volatile uint8_t *slot = NULL;
            uint32_t id = 0U;

            fill_request(&rq, i);
            CHECK(zzapp_xport_post_request(rec, kind, &rq, 1000U + i, &id) == AAR_OK);
            CHECK(id == i + 1U);                       /* from 1, in order */
            CHECK(zzapp_xport_req_id(rec, kind) == id);
            CHECK(zzapp_xport_req_deadline(rec, kind) == 1000U + i);
            CHECK(zzapp_xport_req_state(rec, kind) == ZZAPP_XPORT_REQ_POSTED);
            CHECK(zzapp_xport_post_request(rec, kind, &rq, 0U, NULL) == AAR_E_BUSY);
            CHECK(zzapp_xport_take_completion(rec, kind, &got) == 0);

            /* The client consumes the entry exactly as posted. */
            CHECK(aar_queue_peek(&c.sq, hook_acquire, &slot) == 1);
            aar_entry_get_request(slot, &seen);
            aar_queue_pop(&c.sq, hook_release);
            CHECK(seen.op == rq.op && seen.flags == rq.flags && seen.id == id);
            CHECK(memcmp(seen.arg, rq.arg, sizeof(rq.arg)) == 0);
            CHECK(seen.buf == rq.buf && seen.buf_off == rq.buf_off &&
                  seen.buf_len == rq.buf_len);
            CHECK(aar_get_be32(slot + 12) == 0U);       /* reserved bytes stay zero */
            CHECK(aar_get_be32(slot + 52) == 0U && aar_get_be32(slot + 60) == 0U);

            complete(&c, &seen, (int32_t)i - 100, i);
            CHECK(zzapp_xport_cq_pending(rec) == 1);
            CHECK(zzapp_xport_pump(rec) == 1);
            CHECK(zzapp_xport_take_completion(rec, kind, &got) == 1);
            CHECK(got.id == id && got.op == rq.op && got.result == (int32_t)i - 100);
            CHECK(got.flags == 0x1200U + i);
            {
                uint32_t k;

                for (k = 0U; k < AAR_ENTRY_OUTS; k++)
                    CHECK(got.out[k] == i * 10U + k);
            }
            CHECK(zzapp_xport_req_state(rec, kind) == ZZAPP_XPORT_REQ_FREE);
            CHECK(zzapp_xport_take_completion(rec, kind, &got) == AAR_E_INVAL);
        }
        CHECK(word(&c, AAR_CB_SQ_TAIL) == starts[s] + 200U);
        CHECK(word(&c, AAR_CB_SQ_HEAD) == starts[s] + 200U);
        CHECK(word(&c, AAR_CB_CQ_TAIL) == starts[s] + 200U);
        CHECK(word(&c, AAR_CB_CQ_HEAD) == starts[s] + 200U);
        CHECK(zzapp_xport_stats(rec)->pumped == 200U);
        CHECK(zzapp_xport_stats(rec)->unknown_dropped == 0U);
        unclaim(rec);
    }
}

static void test_three_entries_are_independent(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request rq[3], seen[3];
    aar_completion got;
    uint32_t id[3];
    const volatile uint8_t *slot = NULL;
    int k;

    reset_all();
    rec = claim(&c);
    for (k = 0; k < 3; k++) {
        fill_request(&rq[k], (uint32_t)k);
        CHECK(zzapp_xport_post_request(rec, (zzapp_xport_req_kind_t)k, &rq[k], 0U,
                                       &id[k]) == AAR_OK);
    }
    CHECK(id[0] == 1U && id[1] == 2U && id[2] == 3U);
    CHECK(zzapp_xport_post_request(rec, (zzapp_xport_req_kind_t)ZZAPP_XPORT_REQ_COUNT, &rq[0], 0U, NULL) ==
          AAR_E_INVAL);
    for (k = 0; k < 3; k++) {
        CHECK(aar_queue_peek(&c.sq, hook_acquire, &slot) == 1);
        aar_entry_get_request(slot, &seen[k]);
        aar_queue_pop(&c.sq, hook_release);
    }
    /* Completions out of order: each lands in its own entry. */
    complete(&c, &seen[2], 22, 2U);
    complete(&c, &seen[0], 20, 0U);
    CHECK(zzapp_xport_pump(rec) == 2);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_DONE);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) == ZZAPP_XPORT_REQ_POSTED);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT) == ZZAPP_XPORT_REQ_DONE);
    CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT, &got) == 1 &&
          got.result == 22 && got.id == 3U);
    CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &got) == 1 &&
          got.result == 20 && got.id == 1U);
    CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_FLIP, &got) == 0);
    unclaim(rec);
}

static void test_sq_full_empty_and_message_reserve(void)
{
    client_t c;
    zzapp_rec_t *rec;
    uint8_t msg[AAR_MSG_BYTES];
    aar_request rq;
    uint32_t i, id;
    const volatile uint8_t *slot = NULL;

    reset_all();
    rec = claim(&c);
    set_indices(&c, 0xFFFFFFF0U);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 0);     /* empty */
    CHECK(zzapp_xport_sq_space(rec) == 1);
    memset(msg, 0x5A, sizeof(msg));
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        msg[0] = (uint8_t)i;
        CHECK(zzapp_xport_post_msg(rec, msg) == AAR_OK);
    }
    CHECK(zzapp_xport_post_msg(rec, msg) == AAR_E_BUSY);   /* one entry stays free */
    CHECK(zzapp_xport_sq_space(rec) == 1);
    fill_request(&rq, 1U);
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_OK);
    CHECK(zzapp_xport_sq_space(rec) == 0);                 /* now full */
    CHECK(word(&c, AAR_CB_SQ_FULL) == 0U);
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_FLIP, &rq, 0U, &id) == AAR_E_BUSY);
    CHECK(word(&c, AAR_CB_SQ_FULL) == 1U);
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_FLIP, &rq, 0U, &id) == AAR_E_BUSY);
    CHECK(word(&c, AAR_CB_SQ_FULL) == 2U);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) == ZZAPP_XPORT_REQ_FREE);
    CHECK(zzapp_xport_post_msg(rec, msg) == AAR_E_BUSY);
    /* Entries are in order, messages byte for byte. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        uint8_t got[AAR_MSG_BYTES];

        CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
        CHECK(aar_get_be16(slot + AAR_ENTRY_OP) == AAR_OP_MSG);
        aar_entry_get_msg(slot, got);
        CHECK(got[0] == (uint8_t)i && got[1] == 0x5AU && got[31] == 0x5AU);
        aar_queue_pop(&c.sq, NULL);
    }
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    CHECK(aar_get_be16(slot + AAR_ENTRY_OP) != AAR_OP_MSG);
    aar_queue_pop(&c.sq, NULL);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 0);
    CHECK(zzapp_xport_sq_space(rec) == 1);
    CHECK(word(&c, AAR_CB_SQ_TAIL) == 0xFFFFFFF0U + 64U);   /* wrapped past 2^32 */
    unclaim(rec);
}

/* ---- Barrier hooks -------------------------------------------------------------- */

static client_t *probe_client;
static uint32_t probe_head_seen, probe_tail_seen;
static unsigned probe_calls;

static void probe_cq_head(void)
{
    probe_head_seen = aar_get_be32(probe_client->cq.head);
    probe_calls++;
}

static void probe_sq_tail(void)
{
    probe_tail_seen = aar_get_be32(probe_client->sq.tail);
    probe_calls++;
}

static uint32_t probe_reason[2], probe_state[2];
static unsigned probe_n;

static void probe_lifecycle(void)
{
    if (probe_n < 2U) {
        probe_reason[probe_n] = aar_get_be32(probe_client->cb + AAR_CB_END_REASON);
        probe_state[probe_n] = aar_get_be32(probe_client->cb + AAR_CB_STATE);
    }
    probe_n++;
}

static void test_barrier_order(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request rq;
    aar_completion done;
    uint32_t id;

    reset_all();
    rec = claim(&c);
    probe_client = &c;
    set_indices(&c, 100U);

    /* Producer: the release barrier runs before the tail is stored. */
    fill_request(&rq, 3U);
    release_probe = probe_sq_tail;
    probe_calls = 0U;
    acquire_calls = 0U;
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_OK);
    CHECK(probe_calls == 1U && probe_tail_seen == 100U);
    CHECK(word(&c, AAR_CB_SQ_TAIL) == 101U);
    CHECK(acquire_calls >= 1U);                       /* head read, then acquire */

    /* Consumer: the stricter release barrier runs before the head is stored. */
    memset(&done, 0, sizeof(done));
    done.op = rq.op;
    done.id = id;
    CHECK(aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK);
    release_probe = probe_cq_head;
    probe_calls = 0U;
    acquire_calls = 0U;
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(probe_calls == 1U && probe_head_seen == 100U);
    CHECK(word(&c, AAR_CB_CQ_HEAD) == 101U);
    CHECK(acquire_calls >= 1U);                       /* tail read, then acquire */

    /* The lifecycle: end reason, then state, each after a release barrier, so
     * a client that sees the state sees the reason and the fault record. */
    zzapp_xport_cb_put(rec, AAR_CB_STATE, AAR_STATE_RUNNING);
    zzapp_xport_cb_put(rec, AAR_CB_END_REASON, 0U);
    release_probe = probe_lifecycle;
    probe_n = 0U;
    zzapp_xport_cb_set_state(rec, AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    release_probe = NULL;
    CHECK(probe_n == 2U);
    CHECK(probe_reason[0] == 0U && probe_state[0] == AAR_STATE_RUNNING);
    CHECK(probe_reason[1] == AAR_END_USER_FAULT && probe_state[1] == AAR_STATE_RUNNING);
    CHECK(word(&c, AAR_CB_END_REASON) == AAR_END_USER_FAULT);
    CHECK(word(&c, AAR_CB_STATE) == AAR_STATE_FAULTED);
    unclaim(rec);
}

static void test_control_block_words(void)
{
    client_t c;
    zzapp_rec_t *rec;

    reset_all();
    rec = claim(&c);
    zzapp_xport_cb_set_fault(rec, AAR_FAULT_DATA, 0x805U, 0x60000010U, 0x1000U,
                             0x2000U, 0x3000U, 0xDEADBEEFU);
    CHECK(word(&c, AAR_CB_FAULT_CLASS) == AAR_FAULT_DATA);
    CHECK(word(&c, AAR_CB_FAULT_SYNDROME) == 0x805U);
    CHECK(word(&c, AAR_CB_FAULT_PSTATE) == 0x60000010U);
    CHECK(word(&c, AAR_CB_FAULT_PC_HI) == 0U && word(&c, AAR_CB_FAULT_PC_LO) == 0x1000U);
    CHECK(word(&c, AAR_CB_FAULT_SP_LO) == 0x2000U && word(&c, AAR_CB_FAULT_LR_LO) == 0x3000U);
    CHECK(word(&c, AAR_CB_FAULT_ADDR_HI) == 0U && word(&c, AAR_CB_FAULT_ADDR_LO) == 0xDEADBEEFU);
    zzapp_xport_cb_put64(rec, AAR_CB_PROGRESS_HI, 0x123456789ABCDEF0ULL);
    CHECK(word(&c, AAR_CB_PROGRESS_HI) == 0x12345678U && word(&c, AAR_CB_PROGRESS_LO) == 0x9ABCDEF0U);
    CHECK(zzapp_xport_cb_get64(rec, AAR_CB_PROGRESS_HI) == 0x123456789ABCDEF0ULL);
    /* Client-written words are read after a load and an acquire barrier. */
    aar_put_be32(c.cb + AAR_CB_LEASE_COUNTER, 77U);
    acquire_calls = 0U;
    CHECK(zzapp_xport_cb_read_client(rec, AAR_CB_LEASE_COUNTER) == 77U);
    CHECK(acquire_calls == 1U);
    unclaim(rec);
    /* Without a pair every word reads 0 and a write is ignored. */
    CHECK(zzapp_xport_cb_get(rec, AAR_CB_MAGIC) == 0U);
    zzapp_xport_cb_put(rec, AAR_CB_STATE, 1U);
}

/* ---- Messages, credits, events ----------------------------------------------------- */

static void test_credits(void)
{
    client_t c;
    zzapp_rec_t *rec;
    uint8_t msg[AAR_MSG_BYTES], got[AAR_MSG_BYTES];
    aar_request rq;
    aar_completion done;
    uint32_t i, id;
    const volatile uint8_t *slot = NULL;

    reset_all();
    rec = claim(&c);
    set_indices(&c, 0xFFFFFFE0U);
    memset(msg, 0, sizeof(msg));

    /* A request is in flight; the client sends all 63 messages its credits
     * allow, then the completion behind them. */
    fill_request(&rq, 9U);
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_OK);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    aar_entry_get_request(slot, &rq);
    aar_queue_pop(&c.sq, NULL);
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        msg[0] = (uint8_t)i;
        msg[1] = (uint8_t)(i >> 8);
        CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.posted, msg, NULL, NULL) == AAR_OK);
    }
    CHECK(c.posted == 63U);
    CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.posted, msg, NULL, NULL) == AAR_E_BUSY);
    memset(&done, 0, sizeof(done));
    done.op = rq.op;
    done.id = rq.id;
    done.result = 4321;
    CHECK(aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK);   /* the 64th */
    CHECK(zzapp_xport_cq_pending(rec) == 64);

    /* One pump reaches the completion behind 63 messages and leaves nothing. */
    CHECK(zzapp_xport_pump(rec) == 64);
    CHECK(zzapp_xport_cq_pending(rec) == 0);
    CHECK(zzapp_xport_msg_pending(rec) == 63U);
    CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &done) == 1 &&
          done.result == 4321);
    CHECK(zzapp_xport_stats(rec)->msgs_overflow == 0U);

    /* The credit comes back when the app takes the message, not when the pump
     * moves it. */
    CHECK(word(&c, AAR_CB_MSG_CONSUMED) == 0U);
    CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.posted, msg, NULL, NULL) == AAR_E_BUSY);
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        CHECK(zzapp_xport_take_msg(rec, got) == 1);
        CHECK(got[0] == (uint8_t)i && got[1] == (uint8_t)(i >> 8));   /* in order */
        CHECK(word(&c, AAR_CB_MSG_CONSUMED) == i + 1U);
        msg[0] = (uint8_t)(200U + i);
        msg[1] = 0xEEU;
        CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.posted, msg, NULL, NULL) == AAR_OK);
        CHECK(aar_cb_post_msg(&c.cq, c.cb, &c.posted, msg, NULL, NULL) == AAR_E_BUSY);
        CHECK(zzapp_xport_pump(rec) == 1);
        CHECK(zzapp_xport_msg_pending(rec) == AAR_MSG_CAPACITY);
    }
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        CHECK(zzapp_xport_take_msg(rec, got) == 1);
        CHECK(got[0] == (uint8_t)(200U + i) && got[1] == 0xEEU);
    }
    CHECK(word(&c, AAR_CB_MSG_CONSUMED) == 2U * AAR_MSG_CAPACITY);
    CHECK(c.posted == 2U * AAR_MSG_CAPACITY);
    CHECK(zzapp_xport_take_msg(rec, got) == 0);
    unclaim(rec);
}

static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state >> 8;
}

/* A client that respects credits never blocks a completion: random traffic of
 * messages, events and completions against a pump and takes that run at their
 * own pace, around the 2^32 wrap. Every completion is delivered. */
static void test_credit_stress(void)
{
    client_t c;
    zzapp_rec_t *rec;
    uint32_t steps, sent_msgs = 0U, got_msgs = 0U, sent_events = 0U, got_events = 0U;
    uint32_t completions = 0U, delivered = 0U, next_msg = 0U, expect_msg = 0U;
    uint32_t pending_id = 0U;
    aar_request pending;
    int have_pending = 0;

    reset_all();
    rec = claim(&c);
    set_indices(&c, 0xFFFFF000U);
    rng_state = 12345U;

    for (steps = 0U; steps < 60000U; steps++) {
        uint32_t r = rng() % 8U;
        const volatile uint8_t *slot = NULL;

        if (!have_pending && r == 0U) {
            aar_request rq;

            fill_request(&rq, steps);
            if (zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U,
                                         &pending_id) == AAR_OK) {
                CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
                aar_entry_get_request(slot, &pending);
                aar_queue_pop(&c.sq, NULL);
                have_pending = 1;
            }
        } else if (r <= 2U) {
            uint8_t msg[AAR_MSG_BYTES] = { 0 };

            msg[0] = (uint8_t)next_msg;
            msg[1] = (uint8_t)(next_msg >> 8);
            msg[2] = (uint8_t)(next_msg >> 16);
            if (aar_cb_post_msg(&c.cq, c.cb, &c.posted, msg, NULL, NULL) == AAR_OK) {
                next_msg++;
                sent_msgs++;
            }
        } else if (r == 3U) {
            aar_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.type = 3U;
            ev.time_ms = sent_events;
            if (aar_queue_post_event(&c.cq, &ev, NULL, NULL) == AAR_OK)
                sent_events++;
        } else if (r == 4U && have_pending == 1) {
            aar_completion done;

            memset(&done, 0, sizeof(done));
            done.op = pending.op;
            done.id = pending.id;
            done.result = (int32_t)pending_id;
            if (aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK) {
                completions++;
                have_pending = 2;           /* posted, not yet seen */
            }
        } else if (r == 5U) {
            (void)zzapp_xport_pump(rec);
            CHECK(zzapp_xport_cq_pending(rec) == 0 ||
                  zzapp_xport_cq_pending(rec) <= 64);
            if (have_pending == 2) {
                aar_completion got;

                if (zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING,
                                                &got) == 1) {
                    CHECK(got.id == pending_id && got.result == (int32_t)pending_id);
                    delivered++;
                    have_pending = 0;
                }
            }
        } else if (r == 6U) {
            uint8_t got[AAR_MSG_BYTES];

            if (zzapp_xport_take_msg(rec, got) == 1) {
                CHECK(got[0] == (uint8_t)expect_msg &&
                      got[1] == (uint8_t)(expect_msg >> 8) &&
                      got[2] == (uint8_t)(expect_msg >> 16));
                expect_msg++;
                got_msgs++;
            }
        } else if (r == 7U) {
            aar_event ev;

            while (zzapp_xport_take_event(rec, &ev) == 1)
                got_events++;
        }
    }
    /* Drain. */
    for (;;) {
        aar_event ev;
        uint8_t got[AAR_MSG_BYTES];
        aar_completion done;

        (void)zzapp_xport_pump(rec);
        while (zzapp_xport_take_event(rec, &ev) == 1)
            got_events++;
        while (zzapp_xport_take_msg(rec, got) == 1) {
            CHECK(got[0] == (uint8_t)expect_msg);
            expect_msg++;
            got_msgs++;
        }
        if (have_pending == 2 &&
            zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &done) == 1) {
            delivered++;
            have_pending = 0;
        }
        if (zzapp_xport_cq_pending(rec) == 0)
            break;
    }
    CHECK(sent_msgs > 1000U && sent_events > 1000U && completions > 100U);
    CHECK(got_msgs == sent_msgs);                 /* credits: nothing lost */
    CHECK(zzapp_xport_stats(rec)->msgs_overflow == 0U);
    CHECK(delivered == completions);              /* no completion blocked */
    CHECK(got_events + zzapp_xport_stats(rec)->events_dropped == sent_events);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == 0U);
    unclaim(rec);
}

static void test_inbox_overflow_by_a_client_that_ignores_credits(void)
{
    client_t c;
    zzapp_rec_t *rec;
    uint8_t msg[AAR_MSG_BYTES], got[AAR_MSG_BYTES];
    uint32_t i;

    reset_all();
    rec = claim(&c);
    memset(msg, 0, sizeof(msg));
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        msg[0] = (uint8_t)i;
        CHECK(aar_queue_post_msg(&c.cq, msg, NULL, NULL) == AAR_OK);
    }
    CHECK(zzapp_xport_pump(rec) == 63);
    for (i = 0U; i < 5U; i++) {
        msg[0] = (uint8_t)(100U + i);
        CHECK(aar_queue_post_msg(&c.cq, msg, NULL, NULL) == AAR_OK);
    }
    CHECK(zzapp_xport_pump(rec) == 5);          /* taken out of the CQ anyway */
    CHECK(zzapp_xport_cq_pending(rec) == 0);
    CHECK(zzapp_xport_stats(rec)->msgs_overflow == 5U);
    CHECK(zzapp_xport_msg_pending(rec) == 63U);
    CHECK(word(&c, AAR_CB_MSG_CONSUMED) == 0U);  /* a drop is not a consumption */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        CHECK(zzapp_xport_take_msg(rec, got) == 1 && got[0] == (uint8_t)i);
    }
    CHECK(zzapp_xport_take_msg(rec, got) == 0);
    unclaim(rec);
}

static void test_event_ring(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_event ev, got;
    uint32_t i;

    reset_all();
    rec = claim(&c);
    set_indices(&c, 0xFFFFFFF8U);
    for (i = 0U; i < 63U; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = (uint16_t)(1U + i % 7U);
        ev.code = (uint16_t)(0x40U + i);
        ev.dx = (int16_t)(i - 30);
        ev.dy = (int16_t)(-(int)i);
        ev.buttons = (uint16_t)(i * 3U);
        ev.time_ms = 1000U + i;
        CHECK(aar_queue_post_event(&c.cq, &ev, NULL, NULL) == AAR_OK);
    }
    CHECK(zzapp_xport_pump(rec) == 63);
    CHECK(zzapp_xport_event_pending(rec) == 63U);
    /* The ring holds 64: one more fits, the next ones are dropped and counted. */
    for (i = 63U; i < 70U; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = (uint16_t)(1U + i % 7U);
        ev.code = (uint16_t)(0x40U + i);
        ev.dx = (int16_t)(i - 30);
        ev.dy = (int16_t)(-(int)i);
        ev.buttons = (uint16_t)(i * 3U);
        ev.time_ms = 1000U + i;
        CHECK(aar_queue_post_event(&c.cq, &ev, NULL, NULL) == AAR_OK);
    }
    CHECK(zzapp_xport_pump(rec) == 7);
    CHECK(zzapp_xport_event_pending(rec) == 64U);
    CHECK(zzapp_xport_stats(rec)->events_dropped == 6U);
    CHECK(zzapp_xport_cq_pending(rec) == 0);     /* a dropped event is still consumed */
    for (i = 0U; i < 64U; i++) {            /* the oldest 64, in order, intact */
        CHECK(zzapp_xport_take_event(rec, &got) == 1);
        CHECK(got.type == 1U + i % 7U && got.code == 0x40U + i);
        CHECK(got.dx == (int16_t)(i - 30) && got.dy == (int16_t)(-(int)i));
        CHECK(got.buttons == (uint16_t)(i * 3U) && got.time_ms == 1000U + i);
    }
    CHECK(zzapp_xport_take_event(rec, &got) == 0);
    /* The free-running event counters pass 2^32 without trouble. */
    rec->xport.ev_head = rec->xport.ev_tail = 0xFFFFFFFEU;
    for (i = 0U; i < 5U; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = 3U;
        ev.time_ms = i;
        CHECK(aar_queue_post_event(&c.cq, &ev, NULL, NULL) == AAR_OK);
    }
    CHECK(zzapp_xport_pump(rec) == 5);
    for (i = 0U; i < 5U; i++)
        CHECK(zzapp_xport_take_event(rec, &got) == 1 && got.time_ms == i);
    unclaim(rec);
}

/* ---- Ids, unknown and abandoned ------------------------------------------------------ */

static void test_ids(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request rq, seen, seen2;
    aar_completion done, got;
    const volatile uint8_t *slot = NULL;
    uint32_t id1, id2, id3;

    reset_all();
    rec = claim(&c);
    fill_request(&rq, 4U);

    /* An id that matches nothing is dropped and counted. */
    memset(&done, 0, sizeof(done));
    done.op = rq.op;
    done.id = 0x5555U;
    CHECK(aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK);
    done.id = 0U;
    CHECK(aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK);
    CHECK(zzapp_xport_pump(rec) == 2);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == 2U);
    CHECK(zzapp_xport_stats(rec)->late_dropped == 0U);

    /* A request, an echo with the wrong op: dropped, the request stays. */
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id1) == AAR_OK);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    aar_entry_get_request(slot, &seen);
    aar_queue_pop(&c.sq, NULL);
    done.id = id1;
    done.op = (uint16_t)(rq.op + 1U);
    CHECK(aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == 3U);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_POSTED);

    /* The wait times out: the owner abandons the id, and posts the next call. */
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);          /* harmless */
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id2) == AAR_OK);
    CHECK(id2 != id1 && id2 > id1);                              /* never reused */
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    aar_entry_get_request(slot, &seen2);
    aar_queue_pop(&c.sq, NULL);

    /* The late completion of the old id arrives first, then the new one. */
    complete(&c, &seen, 111, 1U);
    complete(&c, &seen2, 222, 2U);
    CHECK(zzapp_xport_pump(rec) == 2);
    CHECK(zzapp_xport_stats(rec)->late_dropped == 1U);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == 3U);
    CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &got) == 1);
    CHECK(got.id == id2 && got.result == 222);                   /* not the old one */

    /* A duplicate of a completion already taken matches nothing. */
    complete(&c, &seen2, 222, 2U);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == 4U);

    /* A DONE entry that is abandoned also forgets its result. */
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id3) == AAR_OK);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    aar_entry_get_request(slot, &seen);
    aar_queue_pop(&c.sq, NULL);
    complete(&c, &seen, 5, 3U);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_DONE);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
    CHECK(zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &got) == AAR_E_INVAL);
    unclaim(rec);
}

static void test_id_exhaustion(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request rq;
    uint32_t id = 0U;
    const volatile uint8_t *slot = NULL;

    reset_all();
    rec = claim(&c);
    fill_request(&rq, 1U);
    rec->xport.next_id = 0xFFFFFFFEU;
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_OK &&
          id == 0xFFFFFFFEU);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_OK &&
          id == 0xFFFFFFFFU);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    /* Never zero, never reused: the counter does not wrap, the post is refused. */
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_E_NOSPC);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    unclaim(rec);
}

/* A buffer a request names stays allocated and unchanged until the client has
 * completed that request. A request the owner abandons while it is still
 * POSTED keeps its buffer held in its entry (abandoned_names) until the late
 * completion with its id and op arrives; meanwhile nothing frees, replaces,
 * reuses or rewrites the buffer and the entry takes no new request. A request
 * that completed or never named a tracked buffer holds nothing. */
static void post_named(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind,
                       uint16_t op, const zzapp_xport_buf_t *buf)
{
    aar_request rq;
    uint32_t id = 0U;

    memset(&rq, 0, sizeof(rq));
    rq.op = op;
    if (buf) {
        rq.buf = buf->token;
        rq.buf_off = buf->off;
        rq.buf_len = 16U;
    }
    CHECK(zzapp_xport_post_request(rec, kind, &rq, 0U, &id) == AAR_OK);
}

/* The client takes the oldest request off the SQ. */
static void take_request(client_t *c, aar_request *seen)
{
    const volatile uint8_t *slot = NULL;

    CHECK(aar_queue_peek(&c->sq, NULL, &slot) == 1);
    aar_entry_get_request(slot, seen);
    aar_queue_pop(&c->sq, NULL);
}

static void test_abandoned_request_holds_data_block(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request first, second, wrong;
    zzapp_xport_buf_t a, ring, now;
    uint32_t late0, unknown0;
    uint8_t *p;
    uint32_t i;

    reset_all();
    rec = claim(&c);
    CHECK(zzapp_xport_data_reserve(rec, 0x2000U, &a) == AAR_OK);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_AUDIO, 0x2000U, &ring) == AAR_OK);

    /* Completed, then dropped: nothing is held. */
    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_READ, &a);
    take_request(&c, &first);
    complete(&c, &first, 0, 1U);
    CHECK(zzapp_xport_pump(rec) == 1);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(!zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));
    CHECK(zzapp_xport_data_reserve(rec, 0x4000U, &a) == AAR_OK && a.bytes == 0x4000U);

    /* Posted but naming no tracked buffer (a seek): nothing is held, and the
     * entry takes the next request. */
    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_SEEK, NULL);
    take_request(&c, &first);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(!zzapp_xport_buffer_held(rec, 0xffU));
    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_SEEK, NULL);
    take_request(&c, &first);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);

    /* Posted and naming the data block: held, immutable under every use. */
    p = (uint8_t *)a.ptr;
    for (i = 0U; i < 64U; i++)
        p[i] = (uint8_t)(0x40U + i);
    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_WRITE, &a);
    take_request(&c, &first);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));
    CHECK(!zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AUDIO));
    CHECK(zzapp_xport_data_reserve(rec, 0x1000U, &now) == AAR_E_BUSY);   /* smaller */
    CHECK(zzapp_xport_data_reserve(rec, 0x4000U, &now) == AAR_E_BUSY);   /* same */
    CHECK(zzapp_xport_data_reserve(rec, 0x6000U, &now) == AAR_E_BUSY);   /* larger */
    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &now) == 1 &&
          now.ptr == a.ptr && now.bytes == 0x4000U);
    for (i = 0U; i < 64U; i++)
        CHECK(p[i] == (uint8_t)(0x40U + i));
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_DATA) == AAR_OK);
    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, NULL) == 1);
    /* The entry is unavailable while the buffer is borrowed. */
    {
        aar_request rq;
        uint32_t id = 0U;

        memset(&rq, 0, sizeof(rq));
        rq.op = AAR_OP_FILE_SEEK;
        CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_E_BUSY);
    }
    /* The ring is not named: it still goes. */
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO) == AAR_OK);
    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);

    /* A completion with the wrong op, the wrong id, or one for nothing the
     * entry remembers does not release the buffer. */
    late0 = zzapp_xport_stats(rec)->late_dropped;
    unknown0 = zzapp_xport_stats(rec)->unknown_dropped;
    wrong = first;
    wrong.op = (uint16_t)(first.op + 1U);
    complete(&c, &wrong, 0, 2U);
    wrong = first;
    wrong.id = first.id + 0x100U;
    complete(&c, &wrong, 0, 3U);
    CHECK(zzapp_xport_pump(rec) == 2);
    CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));
    CHECK(zzapp_xport_stats(rec)->late_dropped == late0);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == unknown0 + 2U);
    /* The right completion arrives and nothing else pumps: the next use of
     * the buffer takes it. */
    complete(&c, &first, 0, 4U);
    CHECK(zzapp_xport_data_reserve(rec, 0x2000U, &now) == AAR_OK && now.ptr == a.ptr);
    CHECK(!zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));
    CHECK(zzapp_xport_stats(rec)->late_dropped == late0 + 1U);
    CHECK(zzapp_xport_data_reserve(rec, 0x6000U, &now) == AAR_OK && now.bytes == 0x6000U);

    /* A duplicate of that completion cannot release the next borrow. */
    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_READ, &now);
    take_request(&c, &second);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(second.id != first.id && zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));
    complete(&c, &first, 0, 5U);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));
    complete(&c, &second, 0, 6U);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(!zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA));

    /* A post that fails (SQ full) leaves nothing held. */
    {
        aar_request rq;
        uint32_t id = 0U;
        unsigned n;

        fill_request(&rq, 1U);
        for (n = 0U; n < 64U; n++)
            if (aar_queue_post_request(&rec->xport.sq, &rq, NULL, NULL) != AAR_OK)
                break;
        memset(&rq, 0, sizeof(rq));
        rq.op = AAR_OP_FILE_READ;
        rq.buf = now.token;
        CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) != AAR_OK);
        zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
        CHECK(!zzapp_xport_buffer_held(rec, 0xffU));
    }
    /* The claim's release returns everything. */
    unclaim(rec);
    CHECK(heap.live == 0U && heap.bad_frees == 0U);
}

/* The paths and the console-read room of a service block are held the same
 * way, area by area: the other areas, which keep their own in-flight rules,
 * are not. Two entries borrow two areas at once. */
static void test_abandoned_request_holds_service_areas(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request first, second;
    zzapp_xport_buf_t scratch, in, other;
    uint8_t *p;

    reset_all();
    rec = claim(&c);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &scratch) == AAR_OK);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &in) == AAR_OK);
    p = (uint8_t *)scratch.ptr;
    memcpy(p, "pak0.pak", 9U);

    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_OPEN, &scratch);
    take_request(&c, &first);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AREA(ZZAPP_XPORT_AREA_SCRATCH)));
    CHECK(!zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AREA(ZZAPP_XPORT_AREA_CONSOLE_IN)));
    /* The path of the abandoned open cannot be rewritten for the next one. */
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &other) == AAR_E_BUSY);
    CHECK(memcmp(p, "pak0.pak", 9U) == 0);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &other) == AAR_OK);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_PALETTE, &other) == AAR_OK);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_OUT, &other) == AAR_OK);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_LOG, &other) == AAR_OK);
    /* A second entry borrows the console-read room. */
    post_named(rec, ZZAPP_XPORT_REQ_FLIP, AAR_OP_CONSOLE_READ, &in);
    take_request(&c, &second);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FLIP);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &other) == AAR_E_BUSY);
    /* The scratch completion arrives: only that area is free again. */
    complete(&c, &first, 0, 1U);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &other) == AAR_OK);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &other) == AAR_E_BUSY);
    complete(&c, &second, 0, 2U);
    CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &other) == AAR_OK);
    unclaim(rec);
    CHECK(heap.live == 0U && heap.bad_frees == 0U);
}

/* AUDIO_CLOSE names no buffer but tells the client to stop using the ring:
 * until the client has answered it, the ring is not returned. */
static void test_abandoned_audio_close_holds_ring(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request close_req;
    zzapp_xport_buf_t ring;

    reset_all();
    rec = claim(&c);
    CHECK(zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_AUDIO, 0x2000U, &ring) == AAR_OK);
    post_named(rec, ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_AUDIO_CLOSE, NULL);
    take_request(&c, &close_req);
    zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
    CHECK(zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AUDIO));
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO) == AAR_OK);
    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 1);
    complete(&c, &close_req, 0, 1U);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO) == AAR_OK);
    CHECK(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, NULL) == 0);
    unclaim(rec);
    CHECK(heap.live == 0U && heap.bad_frees == 0U);
}

/* ---- Corrupt indices ---------------------------------------------------------------------- */

static void test_corrupt_indices(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_request rq;
    uint8_t msg[AAR_MSG_BYTES] = { 0 };
    uint32_t id;
    uint32_t used_head;

    reset_all();
    rec = claim(&c);
    fill_request(&rq, 1U);

    /* CQ: tail more than depth ahead of head, and head ahead of tail. */
    aar_put_be32(c.cq.head, 10U);
    aar_put_be32(c.cq.tail, 10U + 65U);
    CHECK(zzapp_xport_pump(rec) == AAR_E_IO);
    CHECK(zzapp_xport_cq_pending(rec) == AAR_E_IO);
    CHECK(zzapp_xport_needs_pump(rec) == AAR_E_IO);
    CHECK(zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_CORRUPT));
    CHECK(zzapp_xport_stats(rec)->io_errors == 3U);
    CHECK(word(&c, AAR_CB_CQ_HEAD) == 10U);            /* nothing was consumed */
    aar_put_be32(c.cq.tail, 9U);                       /* head ahead of tail */
    CHECK(zzapp_xport_pump(rec) == AAR_E_IO);
    CHECK(word(&c, AAR_CB_CQ_HEAD) == 10U);
    aar_put_be32(c.cq.tail, 10U);                      /* repaired: empty again */
    CHECK(zzapp_xport_pump(rec) == 0);

    /* SQ: every producer path reports it and writes nothing. */
    used_head = 500U;
    aar_put_be32(c.sq.head, used_head);
    aar_put_be32(c.sq.tail, used_head + 1000U);
    CHECK(zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) == AAR_E_IO);
    CHECK(zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) == ZZAPP_XPORT_REQ_FREE);
    CHECK(zzapp_xport_post_msg(rec, msg) == AAR_E_IO);
    CHECK(zzapp_xport_post_log(rec, "x", 1U) == AAR_E_IO);
    CHECK(zzapp_xport_sq_space(rec) == AAR_E_IO);
    CHECK(word(&c, AAR_CB_SQ_TAIL) == used_head + 1000U);
    CHECK(word(&c, AAR_CB_SQ_FULL) == 0U);             /* corruption is not "full" */
    unclaim(rec);
}

/* ---- Log ----------------------------------------------------------------------------------- */

static void test_log(void)
{
    client_t c;
    zzapp_rec_t *rec;
    const volatile uint8_t *slot = NULL;
    aar_request lr;
    char text[AAR_LOG_MAX + 100];
    uint32_t i;

    reset_all();
    rec = claim(&c);
    set_indices(&c, 0xFFFFFFFCU);
    memset(text, 'a', sizeof(text));
    CHECK(zzapp_xport_post_log(rec, text, 0U) == AAR_E_INVAL);
    CHECK(heap.allocs == 1U);                          /* no block for nothing */

    /* Eight outstanding records, a ninth is dropped and counted. */
    for (i = 0U; i < 8U; i++) {
        text[0] = (char)('0' + i);
        CHECK(zzapp_xport_post_log(rec, text, 10U) == AAR_OK);
    }
    CHECK(heap.allocs == 2U);                          /* service block on first use */
    CHECK(zzapp_xport_post_log(rec, text, 10U) == AAR_E_BUSY);
    CHECK(zzapp_xport_stats(rec)->log_dropped == 1U);
    CHECK(word(&c, AAR_CB_LOG_DROPPED) == 1U);
    /* The client reads each record from the shared buffer the entry names. */
    for (i = 0U; i < 8U; i++) {
        const char *p;

        CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
        aar_entry_get_request(slot, &lr);
        CHECK(lr.op == AAR_OP_LOG && lr.buf_len == 10U && lr.id == 0U);
        p = (const char *)map_token(lr.buf) + lr.buf_off;
        CHECK(p[0] == (char)('0' + i) && p[1] == 'a' && p[9] == 'a');
        if (i < 3U)
            aar_queue_pop(&c.sq, NULL);               /* consumed: 0, 1 and 2 */
        else
            break;
    }
    /* A slot is reusable only once the client has passed its entry. */
    text[0] = 'X';
    CHECK(zzapp_xport_post_log(rec, text, 5U) == AAR_OK);
    text[0] = 'Y';
    CHECK(zzapp_xport_post_log(rec, text, 5U) == AAR_OK);
    text[0] = 'Z';
    CHECK(zzapp_xport_post_log(rec, text, 5U) == AAR_OK);
    CHECK(zzapp_xport_post_log(rec, text, 5U) == AAR_E_BUSY);
    CHECK(zzapp_xport_stats(rec)->log_dropped == 2U);
    /* The three new ones went to the freed slots, the others are untouched. */
    {
        const char *base = map_token(cb_token_of(rec)); /* unused: pair token */
        zzapp_xport_buf_t area;

        (void)base;
        CHECK(zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_LOG, &area) == AAR_OK);
        CHECK(((char *)area.ptr)[0] == 'X' && ((char *)area.ptr)[1024] == 'Y' &&
              ((char *)area.ptr)[2048] == 'Z');
        CHECK(((char *)area.ptr)[3 * 1024] == '3');
    }
    /* Too long: truncated to AAR_LOG_MAX. */
    while (aar_queue_peek(&c.sq, NULL, &slot) == 1)
        aar_queue_pop(&c.sq, NULL);
    CHECK(zzapp_xport_post_log(rec, text, sizeof(text)) == AAR_OK);
    CHECK(aar_queue_peek(&c.sq, NULL, &slot) == 1);
    aar_entry_get_request(slot, &lr);
    CHECK(lr.buf_len == AAR_LOG_MAX);
    aar_queue_pop(&c.sq, NULL);
    unclaim(rec);
}

static void test_log_drops_on_full_sq_and_heap(void)
{
    client_t c;
    zzapp_rec_t *rec;
    uint8_t msg[AAR_MSG_BYTES] = { 0 };
    uint32_t i;

    reset_all();
    rec = claim(&c);
    /* A full SQ drops the record and counts both words. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++)
        CHECK(zzapp_xport_post_msg(rec, msg) == AAR_OK);
    CHECK(zzapp_xport_post_log(rec, "abc", 3U) == AAR_OK);   /* the last free entry */
    CHECK(zzapp_xport_post_log(rec, "abc", 3U) == AAR_E_BUSY);
    CHECK(zzapp_xport_stats(rec)->log_dropped == 1U && zzapp_xport_stats(rec)->sq_full == 1U);
    CHECK(word(&c, AAR_CB_LOG_DROPPED) == 1U && word(&c, AAR_CB_SQ_FULL) == 1U);
    unclaim(rec);

    /* No service block to be had: dropped, counted, the app goes on. */
    reset_all();
    rec = claim(&c);
    heap.fail_after = 0;
    CHECK(zzapp_xport_post_log(rec, "abc", 3U) == AAR_E_NOMEM);
    CHECK(zzapp_xport_stats(rec)->log_dropped == 1U && word(&c, AAR_CB_LOG_DROPPED) == 1U);
    CHECK(aar_get_be32(c.sq.tail) == 0U);
    heap.fail_after = -1;
    unclaim(rec);
}

/* ---- Doorbell and handle staleness ---------------------------------------------------------- */

static void test_kick_and_pump_need(void)
{
    client_t c;
    zzapp_rec_t *rec;
    aar_completion done;

    reset_all();
    rec = claim(&c);
    CHECK(zzapp_xport_needs_pump(rec) == 0);
    CHECK(zzapp_xport_kick_take(rec) == 0U);
    CHECK(zzapp_xport_kick(rec->hdr.handle, ZZAPP_KICK_CQ_POSTED) == AAR_OK);
    CHECK(zzapp_xport_needs_pump(rec) == 1);
    CHECK(zzapp_xport_kick(rec->hdr.handle, ZZAPP_KICK_SQ_SPACE) == AAR_OK);
    CHECK(zzapp_xport_kick_take(rec) == (ZZAPP_KICK_CQ_POSTED | ZZAPP_KICK_SQ_SPACE));
    CHECK(zzapp_xport_kick_take(rec) == 0U);                 /* cleared */
    CHECK(zzapp_xport_needs_pump(rec) == 0);
    CHECK(zzapp_xport_kick(rec->hdr.handle, 0x80U) == AAR_E_INVAL);   /* unknown flag */
    CHECK(zzapp_xport_kick_take(rec) == 0U);
    /* No kick, but the queue holds an entry: the indices are authoritative. */
    memset(&done, 0, sizeof(done));
    done.op = AAR_OP_FILE_OPEN;
    CHECK(aar_queue_post_completion(&c.cq, &done, NULL, NULL) == AAR_OK);
    CHECK(zzapp_xport_needs_pump(rec) == 1);
    CHECK(zzapp_xport_pump(rec) == 1);
    CHECK(zzapp_xport_needs_pump(rec) == 0);
    unclaim(rec);
}

typedef int (*handle_op_t)(uint32_t handle);

static int op_resolve(uint32_t handle)
{
    int status;

    return zzapp_xport_resolve(handle, &status) ? AAR_OK : status;
}

static int op_kick(uint32_t handle)
{
    return zzapp_xport_kick(handle, ZZAPP_KICK_CQ_POSTED);
}

static void test_handle_staleness(void)
{
    static const handle_op_t ops[] = { op_resolve, op_kick };
    zzapp_rec_t *a, *b;
    client_t c;
    uint32_t old, idx, k;
    unsigned o;
    int status;

    reset_all();
    a = claim(&c);
    old = a->hdr.handle;
    idx = aar_handle_index(old);
    for (o = 0U; o < sizeof(ops) / sizeof(ops[0]); o++) {
        CHECK(ops[o](old) == AAR_OK);
        CHECK(ops[o](0U) == AAR_E_INVAL);                          /* handle 0 */
        CHECK(ops[o](aar_handle_make(idx, 0U)) == AAR_E_INVAL);    /* generation 0 */
        CHECK(ops[o](aar_handle_make(idx, aar_handle_generation(old) + 1U)) ==
              AAR_E_STALE);                                         /* the next life */
        CHECK(ops[o](aar_handle_make(idx + 1U, aar_handle_generation(old))) ==
              AAR_E_STALE);                                         /* a FREE index */
        CHECK(ops[o](aar_handle_make(60000U, 1U)) == AAR_E_STALE);  /* out of range */
    }
    CHECK(zzapp_xport_resolve(old, &status) == a && status == AAR_OK);
    CHECK(zzapp_xport_resolve(0U, NULL) == NULL);

    /* A record that is admitted but owns no pair is not a claim yet. */
    {
        zzapp_rec_t *half = NULL;
        uint32_t h = 0U;

        CHECK(zzapp_rec_admit(&h, &half) == 0U);
        for (o = 0U; o < sizeof(ops) / sizeof(ops[0]); o++)
            CHECK(ops[o](h) == AAR_E_STALE);
        CHECK(zzapp_rec_unadmit(h) == 0);
    }

    /* Retired but not released (RECLAIMED): the client may still talk. */
    CHECK(zzapp_rec_set_state(a, ZZAPP_REC_LIVE) == 0);
    CHECK(zzapp_rec_set_state(a, ZZAPP_REC_RETIRING) == 0);
    CHECK(zzapp_rec_set_state(a, ZZAPP_REC_RECLAIMED) == 0);
    for (o = 0U; o < sizeof(ops) / sizeof(ops[0]); o++)
        CHECK(ops[o](old) == AAR_OK);

    /* Released and freed: the old handle is stale for every op. */
    CHECK(zzapp_xport_release(a) == 0);
    for (o = 0U; o < sizeof(ops) / sizeof(ops[0]); o++)
        CHECK(ops[o](old) == AAR_E_STALE);          /* pair gone, record not yet free */
    CHECK(zzapp_rec_free(old) == 0);
    for (o = 0U; o < sizeof(ops) / sizeof(ops[0]); o++)
        CHECK(ops[o](old) == AAR_E_STALE);

    /* Walk the index around until it is reused: the generation moved on. */
    for (k = 0U; k < 40U; k++) {
        b = claim(NULL);
        if (aar_handle_index(b->hdr.handle) == idx) {
            CHECK(b->hdr.handle != old);
            for (o = 0U; o < sizeof(ops) / sizeof(ops[0]); o++)
                CHECK(ops[o](old) == AAR_E_STALE && ops[o](b->hdr.handle) == AAR_OK);
            /* A kick to the old life never reaches the new one. */
            (void)zzapp_xport_kick_take(b);
            CHECK(zzapp_xport_kick(old, ZZAPP_KICK_CQ_POSTED) == AAR_E_STALE);
            CHECK(zzapp_xport_kick_take(b) == 0U);
            unclaim(b);
            break;
        }
        unclaim(b);
    }
    CHECK(k < 40U);
    CHECK(heap.live == 0U && heap.bad_frees == 0U);
}

/* ---- Two threads: the firmware side and the client, real memory ordering ------------------- */

#define THREAD_ROUNDS 20000U

typedef struct {
    client_t c;
    volatile int stop;
    uint32_t requests, logs, msgs_seen;
    uint32_t events_posted_ok;
    uint32_t bad;
} xthread_t;

static void *client_thread(void *arg)
{
    xthread_t *t = arg;
    client_t *c = &t->c;
    uint32_t next_event = 0U, next_msg = 0U;

    while (!t->stop) {
        const volatile uint8_t *slot = NULL;
        int rc = aar_queue_peek(&c->sq, zzapp_xport_dmb, &slot);

        if (rc == 1) {
            uint16_t op = aar_get_be16(slot + AAR_ENTRY_OP);

            if (op == AAR_OP_LOG) {
                aar_request lr;
                const char *p;

                aar_entry_get_request(slot, &lr);
                p = (const char *)map_token(lr.buf) + lr.buf_off;
                if (lr.buf_len != 16U || memcmp(p, "log-record-00000", 12U) != 0 ||
                    p[15] != 'z')
                    t->bad++;
                t->logs++;
                aar_queue_pop(&c->sq, zzapp_xport_dmb);
            } else if (op == AAR_OP_MSG) {
                uint8_t m[AAR_MSG_BYTES];

                aar_entry_get_msg(slot, m);
                if (m[3] != (uint8_t)t->msgs_seen)
                    t->bad++;
                t->msgs_seen++;
                aar_queue_pop(&c->sq, zzapp_xport_dmb);
            } else {
                aar_request rq;
                aar_completion done;
                uint8_t m[AAR_MSG_BYTES] = { 0 };
                aar_event ev;

                aar_entry_get_request(slot, &rq);
                aar_queue_pop(&c->sq, zzapp_xport_dmb);
                t->requests++;
                /* Traffic ahead of the completion: messages within credits and
                 * events, so the completion sits behind them. */
                m[0] = (uint8_t)next_msg;
                m[1] = (uint8_t)(next_msg >> 8);
                while (aar_cb_post_msg(&c->cq, c->cb, &c->posted, m, zzapp_xport_dmb,
                                       zzapp_xport_dmb) == AAR_OK) {
                    next_msg++;
                    m[0] = (uint8_t)next_msg;
                    m[1] = (uint8_t)(next_msg >> 8);
                    if ((next_msg & 7U) == 0U)
                        break;
                }
                memset(&ev, 0, sizeof(ev));
                ev.type = 3U;
                ev.time_ms = next_event;
                if (aar_queue_post_event(&c->cq, &ev, zzapp_xport_dmb,
                                         zzapp_xport_dmb) == AAR_OK) {
                    next_event++;
                    t->events_posted_ok++;
                }
                memset(&done, 0, sizeof(done));
                done.op = rq.op;
                done.id = rq.id;
                done.result = (int32_t)(rq.arg[0] * 3U + 1U);
                done.out[0] = rq.arg[0] ^ 0xA5A5A5A5U;
                while (aar_queue_post_completion(&c->cq, &done, zzapp_xport_dmb,
                                                 zzapp_xport_dmb) != AAR_OK) {
                    if (t->stop)
                        return NULL;
                }
            }
        } else if (rc < 0) {
            t->bad++;
            return NULL;
        }
    }
    return NULL;
}

static void test_two_threads(void)
{
    static xthread_t t;
    zzapp_xport_env_t e;
    zzapp_rec_t *rec;
    pthread_t th;
    uint32_t i, msgs = 0U, events = 0U, logs_ok = 0U, expect_msg = 0U;

    reset_all();
    e = *zzapp_xport_env();
    e.acquire = zzapp_xport_dmb;
    e.release_barrier = zzapp_xport_dmb;
    zzapp_xport_init(&e);
    memset(&t, 0, sizeof(t));
    rec = claim(&t.c);
    set_indices(&t.c, 0xFFFFFF00U);
    CHECK(pthread_create(&th, NULL, client_thread, &t) == 0);

    for (i = 0U; i < THREAD_ROUNDS; i++) {
        aar_request rq;
        aar_completion got;
        uint32_t id = 0U;
        uint32_t spins = 0U;
        aar_event ev;
        uint8_t m[AAR_MSG_BYTES];
        char log[16];

        fill_request(&rq, i);
        while (zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_BLOCKING, &rq, 0U, &id) !=
               AAR_OK)
            ;
        memcpy(log, "log-record-00000", 16U);
        log[15] = 'z';
        if (zzapp_xport_post_log(rec, log, 16U) == AAR_OK)
            logs_ok++;
        for (;;) {
            int rc = zzapp_xport_pump(rec);

            CHECK(rc >= 0);
            if (rc < 0)
                break;
            if (zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_BLOCKING, &got) == 1)
                break;
            if (++spins > 100000000U) {
                CHECK(0 && "completion never arrived");
                break;
            }
        }
        CHECK(got.id == id && got.result == (int32_t)(rq.arg[0] * 3U + 1U) &&
              got.out[0] == (rq.arg[0] ^ 0xA5A5A5A5U));
        while (zzapp_xport_take_msg(rec, m) == 1) {
            CHECK(m[0] == (uint8_t)expect_msg && m[1] == (uint8_t)(expect_msg >> 8));
            expect_msg++;
            msgs++;
        }
        while (zzapp_xport_take_event(rec, &ev) == 1)
            events++;
    }
    t.stop = 1;
    pthread_join(th, NULL);
    while (zzapp_xport_pump(rec) > 0) {
        uint8_t m[AAR_MSG_BYTES];
        aar_event ev;

        while (zzapp_xport_take_msg(rec, m) == 1) {
            CHECK(m[0] == (uint8_t)expect_msg);
            expect_msg++;
            msgs++;
        }
        while (zzapp_xport_take_event(rec, &ev) == 1)
            events++;
    }
    {
        uint8_t m[AAR_MSG_BYTES];
        aar_event ev;

        while (zzapp_xport_take_msg(rec, m) == 1) {
            expect_msg++;
            msgs++;
        }
        while (zzapp_xport_take_event(rec, &ev) == 1)
            events++;
    }
    CHECK(t.bad == 0U);
    CHECK(t.requests == THREAD_ROUNDS);
    CHECK(t.logs == logs_ok);
    CHECK(msgs == t.c.posted);
    CHECK(events + zzapp_xport_stats(rec)->events_dropped == t.events_posted_ok);
    CHECK(zzapp_xport_stats(rec)->msgs_overflow == 0U);
    CHECK(zzapp_xport_stats(rec)->unknown_dropped == 0U);
    unclaim(rec);
    CHECK(heap.live == 0U && heap.bad_frees == 0U);
}

int main(void)
{
    test_admission();
    test_service_and_blocks();
    test_release_failure_and_replay();
    test_shared_total();
    test_request_roundtrip_near_wrap();
    test_three_entries_are_independent();
    test_sq_full_empty_and_message_reserve();
    test_barrier_order();
    test_control_block_words();
    test_credits();
    test_credit_stress();
    test_inbox_overflow_by_a_client_that_ignores_credits();
    test_event_ring();
    test_ids();
    test_id_exhaustion();
    test_abandoned_request_holds_data_block();
    test_abandoned_request_holds_service_areas();
    test_abandoned_audio_close_holds_ring();
    test_corrupt_indices();
    test_log();
    test_log_drops_on_full_sq_and_heap();
    test_kick_and_pump_need();
    test_handle_staleness();
    test_two_threads();
    printf("zzapp_xport: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
