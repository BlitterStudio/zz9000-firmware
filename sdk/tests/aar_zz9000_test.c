/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * aar.library's portable core (aar repository, client/src/aar_core.c) and the
 * ZZ9000 platform layer (sdk/amiga/src/aar_zz9000.c) against the firmware side
 * (ZZ9000OS/src/zzapp_xport.c): the same queue code at both ends in opposite
 * roles, in one process, over one memory.
 *
 * The client is L1 plus L3 as the library links them, built for the host with
 * a fake board. The "card" is this file's mailbox: it answers QUERY, CLAIM,
 * LOAD_*, START, STOP, LIST, RELEASE and KICK with the real firmware records
 * and the real zzapp_xport_*() calls (admission, blocks, requests, the pump,
 * the doorbell, handle resolution), over a host-visible heap that hands out
 * 8 KiB aligned blocks at card addresses. Tokens are board offsets, as on the
 * card; the fake board maps a token by finding the block that holds it.
 *
 * The library words ULONG and APTR are 32 bits here (the 68k layout), so every
 * block the client reaches through an APTR, and every handle, lives in one
 * static arena below 4 GiB: the test is linked without PIE.
 *
 * Covered: claim, refusal and an unusable control block, the ABI check, query,
 * the queue ops end to end (requests, completions with payload buffers,
 * messages with credits, events with coalescing and drop counting, logs, the
 * doorbell and its per-claim request), load and start with every tag, status,
 * the lease interrupt and its gates, handle ownership and staleness (release,
 * revocation) with a check that the client touches no memory afterwards,
 * teardown, the audio ring, and two-thread runs.
 */
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_core.h"
#include "aar_plat.h"
#include "aar_zz9000.h"
#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"
#include "zz9k/abi.h"
#include "zz9k-app-ringtest.h"
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

/* ---- Memory below 4 GiB ------------------------------------------------------- */

#define LOW_BYTES (24U << 20)
#define LOW_SLOTS 1024U

static uint8_t low[LOW_BYTES] __attribute__((aligned(8192)));
static struct {
    uint32_t off, len;
    int used;
} low_tab[LOW_SLOTS];

static void *low_alloc(uint32_t bytes, uint32_t align)
{
    uint32_t cand, i, j, slot = LOW_SLOTS;

    for (i = 0U; i < LOW_SLOTS; i++)
        if (!low_tab[i].used) {
            slot = i;
            break;
        }
    if (slot == LOW_SLOTS)
        return NULL;
    /* First fit: after the arena start or after any used block. */
    for (i = 0U; i <= LOW_SLOTS; i++) {
        uint32_t base;

        if (i == LOW_SLOTS) {
            base = 0U;
        } else if (low_tab[i].used) {
            base = low_tab[i].off + low_tab[i].len;
        } else {
            continue;
        }
        cand = (base + align - 1U) & ~(align - 1U);
        if (cand + bytes > LOW_BYTES)
            continue;
        for (j = 0U; j < LOW_SLOTS; j++)
            if (low_tab[j].used && cand < low_tab[j].off + low_tab[j].len &&
                low_tab[j].off < cand + bytes)
                break;
        if (j == LOW_SLOTS) {
            low_tab[slot].off = cand;
            low_tab[slot].len = bytes;
            low_tab[slot].used = 1;
            return low + cand;
        }
    }
    return NULL;
}

static void low_free(void *p)
{
    uint32_t i;

    for (i = 0U; i < LOW_SLOTS; i++)
        if (low_tab[i].used && low + low_tab[i].off == p) {
            low_tab[i].used = 0;
            return;
        }
    CHECK(0 && "free of a block the arena does not know");
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
    memset(low_tab, 0, sizeof(low_tab));
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
    p = low_alloc(bytes, align);
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
                low_free(ptr);
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

static int ptr_token(const volatile void *ptr, uint32_t bytes, uint32_t *token)
{
    uint32_t i;

    for (i = 0U; i < HEAP_BLOCKS; i++) {
        const volatile uint8_t *b = heap.block[i].ptr;
        const volatile uint8_t *p = ptr;

        if (b && p >= b && p < b + heap.block[i].bytes) {
            if ((uint32_t)(p - b) + bytes > heap.block[i].bytes)
                return AAR_E_INVAL;
            *token = heap.block[i].card + (uint32_t)(p - b) - TOKEN_ADJUST;
            return AAR_OK;
        }
    }
    return AAR_E_INVAL;
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
    uint32_t abi_major;             /* what QUERY reports */
    uint32_t caps;                  /* what QUERY reports */
    int no_mmu;                     /* the board cannot map (mmu.library missing) */
    int events;                     /* QUERY names the firmware's event block */
    unsigned calls, long_calls, claims;
    uint16_t last_op;
    unsigned posts, post_busy;      /* post_busy: answer BUSY this many times */
    unsigned kicks;
    uint32_t kick_flags_or, last_kick_flags;
    int bell_hold;                  /* a posted bell stays in flight */
    unsigned bell_dones, bell_cancels;
    const aar_zz_bell *last_bell, *cancel_bell;
    int bell_mt;                    /* multi-threaded bell test: count only */
    unsigned mt_posts, mt_dones, mt_double;
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

static void reply_status(aar_zz_reply *r, int status)
{
    r->status = status;
    r->len = 0U;
}

static int card_release_claim(zzapp_rec_t *rec)
{
    uint32_t handle = rec->hdr.handle;

    CHECK(zzapp_xport_release(rec) == 0);
    CHECK(zzapp_rec_free(handle) == 0);
    card.released = 1;
    if (rec == card.rec)
        card.rec = NULL;
    return 0;
}

static int fb_call(void *user, uint16_t op, const uint8_t *payload,
                   uint32_t len, int long_op, aar_zz_reply *reply)
{
    uint32_t handle = 0U;
    zzapp_rec_t *rec = NULL;
    int st = 0;

    (void)user;
    card.calls++;
    card.last_op = op;
    memset(reply, 0, sizeof(*reply));
    if (long_op)
        card.long_calls++;
    if (op != ZZAPP_OP_QUERY && op != ZZAPP_OP_CLAIM && op != ZZAPP_OP_LIST) {
        if (len != ZZAPP_MAILBOX_PAYLOAD_BYTES)
            return ZZ9K_STATUS_BAD_REQUEST;
        handle = zzapp_get_be32(payload + ZZAPP_SEL_HANDLE);
        if (handle != 0U && handle == card.revoked) {
            /* The stale path of a revoked claim: STOP and RELEASE release the
             * shell, any other call only answers STALE. */
            if (op == ZZAPP_OP_STOP || op == ZZAPP_OP_RELEASE) {
                rec = zzapp_xport_resolve(handle, &st);
                if (rec)
                    card_release_claim(rec);
            }
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
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_ABI_MAJOR, card.abi_major);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_ABI_MINOR, ZZAPP_ABI_MINOR);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_CAPS, card.caps);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_EVENTS,
                       card.events ? zzapp_xport_events_token() : 0U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_QUEUE_DEPTH, 64U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_RECORDS_FREE, 15U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_TABLES_FREE, 3871U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_TABLES_TOTAL, 3872U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_COMMIT_FREE, 462422016U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_COMMIT_TOTAL, 462422017U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_HOSTHEAP_LARGEST, 74379520U);
        zzapp_put_be32(reply->payload + ZZAPP_QUERY_HOSTHEAP_FREE, 1234U);
        reply->len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
        return 0;
    case ZZAPP_OP_CLAIM: {
        zzapp_xport_buf_t pair;
        uint32_t h = 0U;

        card.claims++;
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
        card_release_claim(rec);
        return 0;
    case ZZAPP_OP_LIST: {
        /* As put_list of zzapp_services.c: the first live claim whose record
         * index (pid) is at or after the cursor; INDEX is that raw pid. */
        uint32_t pid, cap = zzapp_rec_capacity();

        reply->len = ZZAPP_LIST_REPLY_BYTES;
        for (pid = zzapp_get_be32(payload + ZZAPP_LIST_CURSOR); pid < cap; pid++) {
            zzapp_rec_t *r;

            if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
                continue;
            r = zzapp_rec_by_pid(pid);
            if (!r || !zzapp_xport_live(r))
                continue;
            zzapp_put_be32(reply->payload + ZZAPP_LIST_HANDLE, r->hdr.handle);
            zzapp_put_be32(reply->payload + ZZAPP_LIST_STATE, AAR_STATE_RUNNING);
            zzapp_put_be32(reply->payload + ZZAPP_LIST_INDEX, r->hdr.pid);
            zzapp_put_be32(reply->payload + ZZAPP_LIST_COMMITTED, 0x400000U);
            zzapp_put_be32(reply->payload + ZZAPP_LIST_REASONS,
                           (ZZAPP_BLOCK_FILE << 24) | (AAR_END_NONE << 16) |
                           ZZAPP_LIST_FLAG_OVERLOAD);
            memcpy(reply->payload + ZZAPP_LIST_NAME, "swquake", 7);
            break;
        }
        return 0;
    }
    default:
        return ZZ9K_STATUS_UNSUPPORTED;
    }
}

/* The doorbell request of one claim lives in its aar_zz_bell. words[0] is 1
 * while the request is in flight. */
static int fb_bell_post(void *user, aar_zz_bell *bell, uint16_t op,
                        const uint8_t *payload, uint32_t len)
{
    uint32_t handle, flags;

    (void)user;
    if (card.bell_mt) {
        /* Two threads share a claim: count, and catch a request posted on
         * a bell that is still in flight. */
        if (__atomic_exchange_n(&bell->words[0], 1U, __ATOMIC_ACQ_REL) != 0U)
            __atomic_fetch_add(&card.mt_double, 1U, __ATOMIC_RELAXED);
        __atomic_fetch_add(&card.mt_posts, 1U, __ATOMIC_RELAXED);
        return AAR_OK;
    }
    card.posts++;
    if (card.post_busy) {
        card.post_busy--;
        return AAR_E_BUSY;
    }
    CHECK(op == ZZAPP_OP_KICK && len == ZZAPP_MAILBOX_PAYLOAD_BYTES);
    CHECK(bell->words[0] == 0U);          /* one request per bell at a time */
    handle = zzapp_get_be32(payload + ZZAPP_SEL_HANDLE);
    flags = zzapp_get_be32(payload + ZZAPP_KICK_FLAGS);
    card.kicks++;
    card.kick_flags_or |= flags;
    card.last_kick_flags = flags;
    card.last_bell = bell;
    bell->words[0] = 1U;
    return zzapp_xport_kick(handle, flags) == AAR_OK ? AAR_OK : AAR_E_STALE;
}

static int fb_bell_done(void *user, aar_zz_bell *bell)
{
    (void)user;
    if (card.bell_mt) {
        if (__atomic_load_n(&bell->words[0], __ATOMIC_ACQUIRE) == 0U)
            return 1;
        __atomic_fetch_add(&card.mt_dones, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&bell->words[0], 0U, __ATOMIC_RELEASE);
        return 1;
    }
    card.bell_dones++;
    if (card.bell_hold)
        return 0;
    bell->words[0] = 0U;
    return 1;
}

static void fb_bell_cancel(void *user, aar_zz_bell *bell)
{
    (void)user;
    card.bell_cancels++;
    card.cancel_bell = bell;
    bell->words[0] = 0U;
}

static int fb_prepare(void *user)
{
    (void)user;
    return card.no_mmu ? AAR_E_NOSYS : AAR_OK;
}

static int fb_map(void *user, uint32_t token, uint32_t bytes,
                  volatile uint8_t **ptr)
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
        return AAR_E_INVAL;
    /* A mapped range lies in one block and is whole 8 KiB steps. */
    if (!fits || (bytes & 0x1fffU))
        card.map_violations++;
    *ptr = p;
    return AAR_OK;
}

static int fb_token(void *user, const volatile void *ptr, uint32_t bytes,
                    uint32_t *token)
{
    (void)user;
    return ptr_token(ptr, bytes, token);
}

static aar_zz_board board;

/* ---- The client: the core over the platform --------------------------------- */

#define TASK_A ((const void *)0x1000)
#define TASK_B ((const void *)0x2000)

static aar_core core;
static const void *current_task;

static void *env_alloc(void *user, uint32_t bytes)
{
    void *p = low_alloc(bytes, 16U);

    (void)user;
    if (p)
        memset(p, 0, bytes);
    return p;
}

static void env_free(void *user, void *p, uint32_t bytes)
{
    (void)user; (void)bytes;
    low_free(p);
}

static void env_self(void *user, aar_owner_stamp *out)
{
    (void)user;
    memset(out, 0, sizeof(*out));
    out->id = current_task;
    out->name = "test";
    out->kind = 1U;
}

static void card_reset(void)
{
    memset(&card, 0, sizeof(card));
    card.abi_major = ZZAPP_ABI_MAJOR;
    card.caps = ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY;
}

static void client_init(void)
{
    struct aar_core_env env;

    memset(&board, 0, sizeof(board));
    board.call = fb_call;
    board.bell_post = fb_bell_post;
    board.bell_done = fb_bell_done;
    board.bell_cancel = fb_bell_cancel;
    board.prepare = fb_prepare;
    board.map = fb_map;
    board.token = fb_token;
    board.acquire = cl_acq;
    board.release = cl_rel;
    aar_zz9000_host_board(&board);
    memset(&env, 0, sizeof(env));
    env.size = sizeof(env);
    env.alloc = env_alloc;
    env.release = env_free;
    env.self = env_self;
    env.ticks_per_sec = 50U;
    current_task = TASK_A;
    CHECK(aar_core_init(&core, aar_plat_get(), &env) == AAR_OK);
    CHECK(aar_core_attach(&core) == AAR_OK);
}

/* 1: the card has an event block and QUERY names it, from the first query. */
static int events_at_reset;

static void reset_all(void)
{
    heap_reset();
    /* No allocator: the firmware forgets the previous test's event block,
     * which lived in a heap that is gone. A test that wants one asks. */
    zzapp_xport_init(NULL);
    (void)zzapp_xport_events_init();
    env_set();
    fw_acquire = fw_release = cl_acquire = cl_release = 0U;
    card_reset();
    if (events_at_reset) {
        CHECK(zzapp_xport_events_init() == 0);
        CHECK(zzapp_xport_events_block() != NULL);
        card.events = 1;
    }
    setup_arena(16U);
    client_init();
}

static void reset_all_events(void)
{
    /* A fresh attach: the library has not seen the card yet. */
    aar_plat_get()->ops->detach(aar_plat_get());
    events_at_reset = 1;
    reset_all();
    events_at_reset = 0;
}

/* Opens a handle for the current task. */
static AARHandle *claim(void)
{
    AARHandle *h = NULL;
    ULONG reason = 99U;

    CHECK(aar_core_open(&core, 0x400U, NULL, NULL, &h, &reason) == AAR_OK);
    CHECK(reason == AAR_REASON_NONE);
    CHECK(h != NULL);
    return h;
}

static uint32_t cbw(uint32_t off)
{
    return zzapp_xport_cb_get(card.rec, off);
}

static void req_init(AARServiceReq *r)
{
    memset(r, 0, sizeof(*r));
    r->size = sizeof(*r);
}

#define SNEXT(h, r) aar_core_service_next(&core, (h), (r))
#define SREPLY(h, r) aar_core_service_reply(&core, (h), (r))
#define GETMSG(h, m) aar_core_get_message(&core, (h), (AARMessage *)(void *)(m))
#define SENDMSG(h, m) aar_core_send_message(&core, (h), (const AARMessage *)(const void *)(m))
#define POST(h, ev) aar_core_post_event(&core, (h), (ev))
#define CLOSE(h) aar_core_close(&core, (h))
#define PTRU(p) ((uintptr_t)(p))

/* What a dispatcher pass does for a handle: flush a held mouse event, retry a
 * doorbell, retire the bell request. */
static void dispatch(void)
{
    aar_core_dispatch(&core, NULL, NULL);
}

static AARZZ9000Stats stats_of(AARHandle *h)
{
    AARZZ9000Stats s;

    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    CHECK(aar_core_ext_stats(h, &s) == AAR_OK);
    return s;
}

static const struct aar_plat_ops *ops(void)
{
    return aar_plat_get()->ops;
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
    AARHandle *h = NULL, *none = NULL;
    ULONG reason = 0U;
    zzapp_rec_stats_t st0, st1;
    zzapp_xport_buf_t pair;

    reset_all();
    zzapp_rec_stats(&st0);
    h = claim();
    CHECK(card.rec && h->claim.handle == card.handle && h->state == AAR_H_LIVE);
    CHECK(card.calls == 2U);                  /* QUERY, CLAIM */
    CHECK(zzapp_xport_block_get(card.rec, ZZAPP_XPORT_BLOCK_PAIR, &pair) == 1);
    /* The pair block is mapped through one slot, extended to hold both
     * queues, and every range lies inside it. */
    CHECK(h->maps[0].token == pair.token && h->maps[0].bytes == 0x4000U &&
          card.map_violations == 0U);
    CHECK(h->sq.depth == 64U && h->cq.depth == 64U);
    CHECK(cbw(AAR_CB_CLIENT_PRESENT) == 1U);
    CHECK(cbw(AAR_CB_LEASE_COUNTER) == 1U);   /* stored at open */

    /* Close it: everything comes back. */
    CHECK(CLOSE(h) == AAR_OK);
    CHECK(heap.live == 0U && card.released);
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free);

    /* The card refuses: the reason comes back, nothing is held. */
    card.refuse_reason = AAR_REASON_HOSTHEAP;
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &none, &reason) == AAR_E_NORESOURCE);
    CHECK(reason == AAR_REASON_HOSTHEAP && none == NULL && heap.live == 0U);
    card.refuse_reason = 0U;

    /* A control block that does not check: the claim is given back. */
    card.corrupt_magic = 1;
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &none, &reason) == AAR_E_IO);
    CHECK(heap.live == 0U && card.released && none == NULL);
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free);
    card.corrupt_magic = 0;

    /* A token the board does not have: the same. */
    card.bad_cb_token = 1;
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &none, &reason) == AAR_E_IO);
    CHECK(heap.live == 0U && card.released);
    card.bad_cb_token = 0;

    /* Bad arguments. */
    CHECK(aar_core_open(&core, 0U, NULL, NULL, NULL, &reason) == AAR_E_INVAL);
    CHECK(CLOSE(NULL) == AAR_E_STALE);
}

/* The platform checks the card before it claims. */
static void test_abi_and_capability_checks(void)
{
    AARHandle *h = NULL;
    AARInfo info;
    ULONG reason = 0U;
    unsigned claims;

    reset_all();
    /* A card with another ABI major, or no Zorro III, is refused before a
     * claim is sent. The check compares the reported major with the major the
     * firmware writes (ZZAPP_ABI_MAJOR), not with the word's offset. */
    card.abi_major = ZZAPP_ABI_MAJOR + 1U;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    CHECK(aar_core_query(&core, &info) == AAR_E_NOSYS);
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &h, &reason) == AAR_E_NOSYS);
    CHECK(card.claims == 0U && h == NULL);
    card.abi_major = ZZAPP_ABI_MAJOR;
    card.caps = ZZAPP_CAP_DISPLAY;                 /* no Z3 */
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &h, &reason) == AAR_E_NOSYS);
    CHECK(card.claims == 0U);
    /* Both are re-read: the card may come up later. */
    card.caps = ZZAPP_CAP_Z3;
    h = claim();
    CHECK(CLOSE(h) == AAR_OK);

    /* mmu.library missing: the query works, the claim is refused before it
     * reaches the card. */
    claims = card.claims;
    card.no_mmu = 1;
    info.size = sizeof(info);
    CHECK(aar_core_query(&core, &info) == AAR_OK);
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &h, &reason) == AAR_E_NOSYS);
    CHECK(card.claims == claims && heap.live == 0U);
    card.no_mmu = 0;

    /* No board at all: the library answers AAR_E_NOSYS and does not crash. */
    aar_core_shutdown(&core);
    aar_core_detach(&core);
    aar_zz9000_host_board(NULL);
    CHECK(aar_core_attach(&core) == AAR_E_NOSYS);
    info.size = sizeof(info);
    CHECK(aar_core_query(&core, &info) == AAR_E_NOSYS);
    CHECK(aar_core_open(&core, 0U, NULL, NULL, &h, &reason) == AAR_E_NOSYS);
}

/* A walk over several claims with gaps in the slot numbers: the cursor goes to
 * the card, INDEX is the raw slot, and the next cursor is INDEX + 1. Every
 * live claim is listed once, in slot order, and the walk then ends with 0. */
static void test_list_walks_claims_with_gaps(void)
{
    AARHandle *h[5];
    const aar_client_ext_zz9000 *ext;
    AARZZ9000List li;
    uint32_t want[3], got[3] = { 0 }, idx[3] = { 0 }, cursor = 0U;
    unsigned i, n = 0U;
    LONG rc;

    reset_all();
    ext = aar_core_ext(&core, AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL);
    for (i = 0U; i < 5U; i++)
        h[i] = claim();
    want[0] = h[0]->claim.handle;
    want[1] = h[2]->claim.handle;
    want[2] = h[4]->claim.handle;
    CHECK(CLOSE(h[1]) == AAR_OK);
    CHECK(CLOSE(h[3]) == AAR_OK);
    for (;;) {
        memset(&li, 0, sizeof(li));
        li.size = sizeof(li);
        rc = ext->list(cursor, &li);
        CHECK(rc >= 0);
        if (rc != 1)
            break;
        if (n < 3U) {
            got[n] = li.handle;
            idx[n] = li.index;
        }
        n++;
        if (n > 8U)
            break;
        cursor = li.index + 1U;
    }
    CHECK(rc == 0 && n == 3U);
    for (i = 0U; i < 3U; i++)
        CHECK(got[i] == want[i]);
    CHECK(idx[0] < idx[1] && idx[1] < idx[2] && idx[1] - idx[0] > 1U);
    /* The cursor is honoured: starting at a claim's own INDEX lists that claim
     * again, starting after it skips it. */
    li.size = sizeof(li);
    CHECK(ext->list(idx[1], &li) == 1 && li.handle == want[1]);
    li.size = sizeof(li);
    CHECK(ext->list(idx[1] + 1U, &li) == 1 && li.handle == want[2]);
    li.size = sizeof(li);
    CHECK(ext->list(idx[2] + 1U, &li) == 0 && li.handle == 0U);
    CHECK(CLOSE(h[0]) == AAR_OK && CLOSE(h[2]) == AAR_OK && CLOSE(h[4]) == AAR_OK);
}

static void test_query_and_extension(void)
{
    AARInfo info;
    AARHandle *h;
    const aar_client_ext_zz9000 *ext;
    AARZZ9000List li;
    int rc;

    reset_all();
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    CHECK(aar_core_query(&core, &info) == AAR_OK);
    CHECK(info.size == sizeof(info) && info.platform_id == AAR_PLATFORM_ZZ9000 &&
          info.profiles == AAR_PROFILEF_A32 &&
          info.groups == (AAR_GROUP_INPUT | AAR_GROUP_SERVICES | AAR_GROUP_DISPLAY) &&
          info.flags == (AAR_INFOF_ISOLATED | AAR_INFOF_SHARED_COHERENT) &&
          info.max_apps == 0U);
    CHECK(info.max_image_bytes == ZZAPP_IMAGE_MAX_BYTES &&
          info.max_args == ZZAPP_ARGV_MAX && info.max_arg_bytes == ZZAPP_ARGV_STR_MAX &&
          info.shared_granule == 4096U && info.stall_limit_ms == 30000U &&
          info.stop_grace_ms == 500U && info.shared_default_mib == AAR_SHARED_DEFAULT_MIB &&
          info.shared_max_mib == 4095U && info.weight_max == ZZAPP_WEIGHT_MAX);
    CHECK(info.queue_depth == 64U && info.msg_capacity == AAR_MSG_CAPACITY);
    CHECK(info.platform[AAR_ZZ9000_INFO_RECORDS_FREE] == 15U &&
          info.platform[AAR_ZZ9000_INFO_TABLES_FREE] == 3871U &&
          info.platform[AAR_ZZ9000_INFO_TABLES_TOTAL] == 3872U &&
          info.platform[AAR_ZZ9000_INFO_COMMIT_FREE] == 462422016U &&
          info.platform[AAR_ZZ9000_INFO_COMMIT_TOTAL] == 462422017U &&
          info.platform[AAR_ZZ9000_INFO_HOSTHEAP_FREE] == 1234U &&
          info.platform[AAR_ZZ9000_INFO_HOSTHEAP_LARGEST] == 74379520U &&
          info.platform[AAR_ZZ9000_INFO_CAPS] == (ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY));
    /* A query needs no claim and takes one mailbox call. */
    CHECK(card.calls == 1U && card.claims == 0U);

    ext = aar_core_ext(&core, AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL && AAR_CLIENT_EXT_ZZ9000_COVERS(ext, lease_pause));
    /* The table ends at lease_pause: set_log_sink left it. */
    CHECK(ext->size == offsetof(aar_client_ext_zz9000, lease_pause) + sizeof(ext->lease_pause));
    CHECK(aar_core_ext(&core, AAR_PLATFORM_ZZ9000, 99U) == NULL);
    CHECK(aar_core_ext(&core, AAR_PLATFORM_NONE, AAR_EXT_ZZ9000_CLIENT) == NULL);

    memset(&li, 0, sizeof(li));
    li.size = sizeof(li);
    CHECK(ext->list(0U, &li) == 0 && li.handle == 0U);
    h = claim();
    li.size = sizeof(li);
    CHECK(ext->list(0U, &li) == 1);
    CHECK(li.handle == h->claim.handle && li.state == AAR_STATE_RUNNING &&
          li.block_reason == ZZAPP_BLOCK_FILE && li.end_reason == AAR_END_NONE &&
          li.committed == 0x400000U && li.flags == AAR_ZZ9000_LISTF_OVERLOAD &&
          li.index == aar_handle_index(h->claim.handle) && !strcmp((const char *)li.name, "swquake"));
    /* A smaller structure is filled as far as it goes. */
    memset(&li, 0xee, sizeof(li));
    li.size = 8U;
    CHECK(ext->list(0U, &li) == 1 && li.size == 8U && li.handle == h->claim.handle &&
          li.state == 0xeeeeeeeeU);
    CHECK(ext->list(0U, NULL) == AAR_E_INVAL);
    /* The administrative release of a claim by its handle. */
    CHECK(ext->release_handle(0x12345678U) == AAR_E_STALE);
    CHECK(ext->release_handle(h->claim.handle) == AAR_OK);
    CHECK(card.released && heap.live == 0U);
    /* The library's own handle is closed as usual; the card no longer knows it. */
    rc = CLOSE(h);
    CHECK(rc == AAR_OK);
}

static void test_requests_end_to_end(void)
{
    AARHandle *h;
    AARServiceReq req;
    aar_completion c;
    zzapp_xport_buf_t scratch, data;
    uint32_t id, i;
    uint8_t *bytes;
    volatile uint8_t *ptr;
    AARZZ9000Stats s;

    reset_all();
    h = claim();
    req_init(&req);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);

    /* FILE_OPEN: the path is in the scratch area. */
    CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_SCRATCH,
                                   &scratch) == AAR_OK);
    strcpy((char *)scratch.ptr, "PROGDIR:data/pak0.pak");
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_OPEN, 3U, AAR_FILE_READ,
                    scratch.token, scratch.off, 22U);
    CHECK(zzapp_xport_req_state(card.rec, ZZAPP_XPORT_REQ_BLOCKING) ==
          ZZAPP_XPORT_REQ_POSTED);
    CHECK(SNEXT(h, &req) == AAR_OK);
    CHECK(req.size == sizeof(req) && req.op == AAR_OP_FILE_OPEN && req.id == id &&
          req.arg[0] == 3U && req.arg[1] == AAR_FILE_READ && req.buf == scratch.token &&
          req.buf_len == 22U && req.result == 0 && req.out[0] == 0U);
    CHECK(req.ptr && !strcmp((const char *)PTRU(req.ptr), "PROGDIR:data/pak0.pak"));
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);   /* the SQ is empty again */
    /* Not complete yet: the firmware sees nothing. */
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 0);
    {
        AARServiceReq done;

        req_init(&done);
        done.op = AAR_OP_FILE_OPEN;
        done.id = id;
        done.result = AAR_OK;
        done.out[0] = 3U;
        done.out[5] = 0xdeadbeefU;
        done.reply_flags = 0x0102U;
        CHECK(SREPLY(h, &done) == AAR_OK);
        /* A request is answered once. */
        CHECK(SREPLY(h, &done) == AAR_E_INVAL);
    }
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1);
    CHECK(c.op == AAR_OP_FILE_OPEN && c.id == id && c.result == AAR_OK &&
          c.out[0] == 3U && c.out[5] == 0xdeadbeefU && c.flags == 0x0102U);
    /* The doorbell went off for the completion. */
    CHECK(card.kicks == 1U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    CHECK(zzapp_xport_kick_take(card.rec) == ZZAPP_KICK_CQ_POSTED);
    s = stats_of(h);
    CHECK(s.requests == 1U && s.replies == 1U);

    /* FILE_READ: the client writes the bytes, then completes; the firmware
     * reads them from the data block it named. */
    CHECK(zzapp_xport_data_reserve(card.rec, 40000U, &data) == AAR_OK);
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_READ, 3U, 40000U,
                    data.token, 0U, 40000U);
    CHECK(SNEXT(h, &req) == AAR_OK && req.op == AAR_OP_FILE_READ);
    CHECK(PTRU(req.ptr) == PTRU(data.ptr));
    ptr = (volatile uint8_t *)PTRU(req.ptr);
    for (i = 0U; i < 40000U; i++)
        ptr[i] = (uint8_t)(i * 7U + 1U);
    req.result = AAR_OK;
    req.out[AAR_R_FILE_COUNT] = 40000U;
    CHECK(SREPLY(h, &req) == AAR_OK);
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

    /* The barrier hooks on both ends. */
    CHECK(fw_acquire > 0U && fw_release > 0U && cl_acquire > 0U && cl_release > 0U);
    CHECK(CLOSE(h) == AAR_OK && heap.live == 0U);
}

/* The list's path and returned names use the same scratch token/offset.
 * The server writes names before posting completion, and the firmware then
 * copies those bytes out. DOS packet name decoding is tested in aar_serve. */
static void test_list_names_cross_the_request_buffer(void)
{
    AARHandle *h;
    AARServiceReq req;
    aar_completion c;
    zzapp_xport_buf_t scratch;
    uint32_t i, id;

    reset_all();
    h = claim();
    req_init(&req);
    CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_SCRATCH,
                                   &scratch) == AAR_OK);
    strcpy(scratch.ptr, "RAM:fh");
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_LIST_OPEN, 3U, 0U,
                    scratch.token, scratch.off, 7U);
    CHECK(SNEXT(h, &req) == AAR_OK);
    CHECK(req.op == AAR_OP_LIST_OPEN && req.id == id &&
          req.buf == scratch.token && req.buf_off == scratch.off &&
          req.buf_len == 7U && !strcmp((const char *)PTRU(req.ptr), "RAM:fh"));
    CHECK(SREPLY(h, &req) == AAR_OK);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1);
    for (i = 0U; i < 47U; i++) {
        char expected[32];
        aar_dirent de;

        memset(scratch.ptr, 0xa5, 256U);
        id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_LIST_NEXT, 3U, 0U,
                        scratch.token, scratch.off, 256U);
        CHECK(SNEXT(h, &req) == AAR_OK);
        CHECK(req.op == AAR_OP_LIST_NEXT && req.id == id &&
              req.arg[AAR_A_LIST_ID] == 3U &&
              PTRU(req.ptr) == PTRU(scratch.ptr) && req.buf_len == 256U);
        snprintf(expected, sizeof(expected), "f%02u.bin", i);
        memcpy((void *)PTRU(req.ptr), expected, strlen(expected) + 1U);
        req.out[AAR_R_LIST_MODE] = AAR_MODE_FILE;
        CHECK(SREPLY(h, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1 &&
              c.result == AAR_OK && c.id == id);
        memcpy(de.name, scratch.ptr, strlen(expected) + 1U);
        de.mode = c.out[AAR_R_LIST_MODE];
        CHECK(!strcmp(de.name, expected) && de.mode == AAR_MODE_FILE);
    }
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_mapping_cache_and_shared_maps(void)
{
    AARHandle *h;
    AARServiceReq req;
    zzapp_xport_buf_t sc, data;
    struct AARSharedMap map, other;
    unsigned before, base;
    uint32_t i, token = 0U;
    aar_completion none;

    reset_all();
    h = claim();
    base = card.map_calls;
    req_init(&req);
    CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_SCRATCH, &sc) == AAR_OK);
    /* Many requests into the one service block: one mapping of 8 KiB. */
    for (i = 0U; i < 10U; i++) {
        fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, sc.token,
                   sc.off, 16U);
        CHECK(SNEXT(h, &req) == AAR_OK);
        req.result = 0;
        CHECK(SREPLY(h, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &none) == 1);
    }
    CHECK(card.map_calls == base + 1U && card.maps[base].token == sc.token &&
          card.maps[base].bytes == 0x2000U);
    /* A later request into the same block that reaches further extends it. */
    before = card.map_calls;
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_CONSOLE_WRITE, 0U, 0U, sc.token,
               ZZAPP_XPORT_CONSOLE_OUT_OFF, 0x2000U);
    CHECK(SNEXT(h, &req) == AAR_OK && req.ptr);
    CHECK(card.map_calls == before + 1U && card.maps[before].token == sc.token &&
          card.maps[before].bytes == 0x6000U);
    req.result = 0;
    CHECK(SREPLY(h, &req) == AAR_OK);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &none) == 1);
    /* A different block maps separately. */
    CHECK(zzapp_xport_data_reserve(card.rec, 0x4000U, &data) == AAR_OK);
    before = card.map_calls;
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_WRITE, 0U, 0U, data.token, 0U,
               0x4000U);
    CHECK(SNEXT(h, &req) == AAR_OK);
    CHECK(card.map_calls == before + 1U && card.maps[before].bytes == 0x4000U);
    CHECK(card.map_violations == 0U);

    /* AARSharedMap: the same cache, and the library's map object. */
    before = card.map_calls;
    memset(&map, 0, sizeof(map));
    map.size = sizeof(map);
    CHECK(aar_core_shared_map(&core, h, data.token, 0x400U, 0U, &map) == AAR_OK);
    CHECK(card.map_calls == before);
    CHECK(PTRU(map.base) == PTRU(data.ptr) && map.bytes == 0x400U &&
          map.haddr == data.token && map.flags == AAR_MAPF_UNCACHED &&
          map.granule == 4096U && map.reserved[0] != 0U);
    CHECK(aar_core_shared_publish(&core, h, &map, 0U, 0x400U) == AAR_OK);
    CHECK(aar_core_shared_acquire(&core, h, &map, 0x100U, 0x100U) == AAR_OK);
    CHECK(aar_core_shared_acquire(&core, h, &map, 0x300U, 0x200U) == AAR_E_INVAL);
    /* The flags argument is reserved. */
    other = map;
    CHECK(aar_core_shared_map(&core, h, data.token, 0x400U, 1U, &other) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, 0x7ff00000U, 16U, 0U, &other) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, data.token, 0U, 0U, &other) == AAR_E_INVAL);
    /* The inverse: a pointer into the board gives its token. */
    CHECK(aar_core_shared_token(&core, h, (const uint8_t *)data.ptr + 0x40U, 16U,
                                &token) == AAR_OK && token == data.token + 0x40U);
    CHECK(aar_core_shared_token(&core, h, (const uint8_t *)data.ptr + 0x3ff0U, 0x100U,
                                &token) == AAR_E_INVAL);
    CHECK(aar_core_shared_token(&core, h, &token, 4U, &token) == AAR_E_INVAL);
    /* Unmap is bookkeeping: the object is dead, the block stays mapped. */
    before = card.map_calls;
    CHECK(aar_core_shared_unmap(&core, h, &map) == AAR_OK);
    CHECK(map.base == 0U && aar_core_shared_unmap(&core, h, &map) == AAR_E_INVAL);
    CHECK(aar_core_shared_publish(&core, h, &map, 0U, 4U) == AAR_E_INVAL);
    CHECK(aar_core_shared_map(&core, h, data.token, 0x400U, 0U, &map) == AAR_OK);
    CHECK(card.map_calls == before);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_bad_buffer_is_refused(void)
{
    AARHandle *h;
    AARServiceReq req;
    aar_completion c;
    uint32_t id;
    AARZZ9000Stats s;

    reset_all();
    h = claim();
    req_init(&req);
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_OPEN, 0U, 0U, 0x7ff00000U,
                    0U, 8U);
    /* The client answers an unmappable buffer itself, so the app is not left
     * waiting: the request is not handed out. */
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);
    s = stats_of(h);
    CHECK(s.bad_buf == 1U);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1);
    CHECK(c.id == id && c.result == AAR_E_INVAL && c.op == AAR_OP_FILE_OPEN);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_messages_and_credits(void)
{
    AARHandle *h;
    uint8_t m[AAR_MSG_BYTES], got[AAR_MSG_BYTES];
    uint32_t i, n;
    AARZZ9000Stats s;

    reset_all();
    h = claim();
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
        AARServiceReq req;

        req_init(&req);
        CHECK(SNEXT(h, &req) == AAR_OK && req.op == AAR_OP_FILE_STAT);
        CHECK(h->inbox_tail - h->inbox_head == 5U);
        for (i = 0U; i < 5U; i++) {
            CHECK(GETMSG(h, got) == AAR_OK && got[0] == i + 1U);
        }
        CHECK(GETMSG(h, got) == AAR_OK && got[0] == 99U);  /* pumped now */
        CHECK(GETMSG(h, got) == AAR_E_BUSY);
        req.result = 0;
        CHECK(SREPLY(h, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    }
    s = stats_of(h);
    CHECK(s.msgs_taken == 6U);

    /* The credit: 63 messages and no more until the app takes some. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        m[0] = (uint8_t)i; m[1] = 0x5a;
        CHECK(SENDMSG(h, m) == AAR_OK);
    }
    CHECK(SENDMSG(h, m) == AAR_E_BUSY);
    s = stats_of(h);
    CHECK(s.msgs_sent == AAR_MSG_CAPACITY);
    /* A request completion still fits behind them: the CQ has room for it. */
    {
        uint32_t id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U,
                                 0U, 0U, 0U);
        AARServiceReq req;
        aar_completion c;

        req_init(&req);
        CHECK(SNEXT(h, &req) == AAR_OK);
        req.result = 0;
        CHECK(SREPLY(h, &req) == AAR_OK);
        /* The pump moves everything out of the CQ; the completion is reached
         * although 63 messages came first. */
        CHECK(zzapp_xport_pump(card.rec) == (int)AAR_MSG_CAPACITY + 1);
        CHECK(zzapp_xport_take_completion(card.rec, ZZAPP_XPORT_REQ_BLOCKING, &c) == 1 &&
              c.id == id);
    }
    /* Nothing consumed yet: still no credit, though the CQ itself is empty. */
    CHECK(SENDMSG(h, m) == AAR_E_BUSY);
    CHECK(zzapp_xport_msg_pending(card.rec) == AAR_MSG_CAPACITY);
    for (n = 0U; n < 3U; n++) {
        CHECK(zzapp_xport_take_msg(card.rec, got) == 1 && got[0] == n && got[1] == 0x5a);
    }
    CHECK(cbw(AAR_CB_MSG_CONSUMED) == 3U);
    for (n = 0U; n < 3U; n++) {
        m[0] = 0xc0 + (uint8_t)n;
        CHECK(SENDMSG(h, m) == AAR_OK);
    }
    CHECK(SENDMSG(h, m) == AAR_E_BUSY);
    CHECK(zzapp_xport_pump(card.rec) == 3);
    CHECK(zzapp_xport_stats(card.rec)->msgs_overflow == 0U);
    CHECK(CLOSE(h) == AAR_OK);
}

/* A full inbox must not drop: the message stays at the SQ head, the app sees
 * the queue fill (BUSY from its own post), and everything behind the message
 * waits until the owner takes some. */
static void test_full_inbox_holds_messages_at_the_head(void)
{
    AARHandle *h;
    AARServiceReq req;
    uint8_t m[AAR_MSG_BYTES] = { 0 }, got[AAR_MSG_BYTES];
    uint32_t i, posted = 0U, taken = 0U, id;
    uint32_t sq_used;
    AARZZ9000Stats s;

    reset_all();
    h = claim();
    req_init(&req);
    /* The app sends 63; the client moves them aside and reads none. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        m[0] = (uint8_t)posted++;
        CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    }
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);
    CHECK(h->inbox_tail - h->inbox_head == AAR_MSG_CAPACITY && !h->sq_blocked);
    /* 63 more: the inbox takes one more (64), the rest stay in the SQ. */
    for (i = 0U; i < AAR_MSG_CAPACITY; i++) {
        m[0] = (uint8_t)posted++;
        CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    }
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);
    CHECK(h->inbox_tail - h->inbox_head == AAR_CORE_INBOX);
    CHECK(h->sq_blocked == 1);
    s = stats_of(h);
    CHECK(s.sq_blocked == 1U);
    CHECK(aar_queue_used(&h->sq, &sq_used) == AAR_OK &&
          sq_used == 2U * AAR_MSG_CAPACITY - AAR_CORE_INBOX);
    /* The app fills the queue and then sees BUSY: nothing was dropped. */
    for (i = 0U; (m[0] = (uint8_t)posted, zzapp_xport_post_msg(card.rec, m) == AAR_OK); i++)
        posted++;
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_E_BUSY);
    CHECK(i > 0U);
    CHECK(aar_queue_used(&h->sq, &sq_used) == AAR_OK && sq_used == h->sq.depth - 1U);

    /* A request behind the stuck messages waits with them. */
    id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY && h->sq_blocked);
    /* The owner takes messages, in order; the request is reached once every
     * message in front of it has been moved out, not before. */
    for (;;) {
        int rc = SNEXT(h, &req);

        if (rc == AAR_OK)
            break;
        CHECK(rc == AAR_E_BUSY);
        CHECK(GETMSG(h, got) == AAR_OK);
        CHECK(got[0] == (uint8_t)taken);
        taken++;
        if (taken > 4U * AAR_CORE_INBOX)
            break;
    }
    CHECK(req.op == AAR_OP_FILE_STAT && req.id == id);
    CHECK(taken > 0U && taken < posted);
    while (GETMSG(h, got) == AAR_OK) {
        CHECK(got[0] == (uint8_t)taken);
        taken++;
    }
    CHECK(taken == posted);
    CHECK(!h->sq_blocked);
    CHECK(CLOSE(h) == AAR_OK);
}

/* ---- The ringtest on the library: ringflood against zz9k-app-ringtest's logic ---- */

#define RF_INFO 1U
#define RF_DATA 2U
#define RF_DONE 3U
#define RF_PING 5U
#define RF_PONG 6U
#define RF_CTRL 7U
#define RF_ACK 8U

typedef struct {
    uint32_t mode, seq, counter, busy, pongs, clock;
    int pend_valid, pend_is_pong;
    uint8_t pend[AAR_MSG_BYTES];
} rf_app;

/* One pass of aar_main of templates/ringflood.c: poll, answer, flood. */
static void rf_app_step(rf_app *a, unsigned passes)
{
    uint8_t in[AAR_MSG_BYTES], msg[AAR_MSG_BYTES];
    unsigned p, i;

    for (p = 0U; p < passes; p++) {
        a->clock++;
        if (a->mode == ZZ9K_RT_MODE_FLOOD)
            a->counter++;
        if (!a->pend_valid) {
            CHECK(zzapp_xport_pump(card.rec) >= 0);
            if (zzapp_xport_take_msg(card.rec, in) == 1) {
                if (zz9k_rt_get(in, 0) == RF_PING) {
                    memset(a->pend, 0, sizeof(a->pend));
                    zz9k_rt_put(a->pend, 0, RF_PONG);
                    zz9k_rt_put(a->pend, 1, zz9k_rt_get(in, 1));
                    zz9k_rt_put(a->pend, 2, zz9k_rt_get(in, 2));
                    zz9k_rt_put(a->pend, 3, zz9k_rt_get(in, 3));
                    zz9k_rt_put(a->pend, 4, a->clock);
                    zz9k_rt_put(a->pend, 5, a->counter);
                    a->pend_valid = 1;
                    a->pend_is_pong = 1;
                } else if (zz9k_rt_get(in, 0) == RF_CTRL) {
                    a->mode = zz9k_rt_get(in, 2);
                    memset(a->pend, 0, sizeof(a->pend));
                    zz9k_rt_put(a->pend, 0, RF_ACK);
                    zz9k_rt_put(a->pend, 1, zz9k_rt_get(in, 1));
                    zz9k_rt_put(a->pend, 2, a->mode);
                    zz9k_rt_put(a->pend, 3, a->seq);
                    zz9k_rt_put(a->pend, 4, a->counter);
                    a->pend_valid = 1;
                    a->pend_is_pong = 0;
                }
            }
        }
        if (a->pend_valid) {
            int rc = zzapp_xport_post_msg(card.rec, a->pend);

            if (rc == AAR_OK) {
                a->pend_valid = 0;
                if (a->pend_is_pong)
                    a->pongs++;
            } else {
                CHECK(rc == AAR_E_BUSY);
                a->busy++;
            }
            continue;
        }
        if (a->mode != ZZ9K_RT_MODE_FLOOD)
            continue;
        memset(msg, 0, sizeof(msg));
        zz9k_rt_put(msg, 0, RF_DATA);
        zz9k_rt_put(msg, 1, a->seq + 1U);
        zz9k_rt_put(msg, 2, a->clock);
        zz9k_rt_put(msg, 3, a->counter);
        for (i = 0U; i < 4U; i++)
            zz9k_rt_put(msg, 4U + i, zz9k_rt_pattern(a->seq + 1U, i));
        {
            int rc = zzapp_xport_post_msg(card.rec, msg);

            if (rc == AAR_OK) {
                a->seq++;
            } else {
                CHECK(rc == AAR_E_BUSY);
                a->busy++;
            }
        }
    }
}

typedef struct {
    AARHandle *h;
    rf_app *app;
    unsigned app_per_get;           /* app passes that run per message taken */
    ZZ9KRingTest t;
    uint64_t clock;
    uint32_t ping_id, ctrl_id;
} rf_client;

static int rf_get(void *ctx, uint8_t m[32])
{
    rf_client *c = ctx;
    int rc;

    /* The app runs while the client reads: on the card both are live. */
    rf_app_step(c->app, c->app_per_get);
    rc = GETMSG(c->h, m);
    return rc == AAR_OK ? 0 : rc == AAR_E_BUSY ? 1 : -1;
}

static uint64_t rf_now(void *ctx)
{
    rf_client *c = ctx;

    return ++c->clock;
}

/* A bounded drain, as the ringtest tool does between pings and checks. */
static void rf_drain(rf_client *c)
{
    ZZ9KRtIo io;
    uint32_t taken;

    io.get = rf_get;
    io.now = rf_now;
    io.sample = NULL;
    io.ctx = c;
    CHECK(zz9k_rt_drain(&c->t, &io, ZZ9K_RT_DRAIN_BATCH, &taken) == 0);
}

static void rf_ping(rf_client *c)
{
    uint8_t ping[AAR_MSG_BYTES];
    uint32_t id = c->ping_id + 1U;

    zz9k_rt_build_ping(ping, id, c->clock);
    if (SENDMSG(c->h, ping) == AAR_OK) {
        zz9k_rt_note_ping(&c->t, id, c->clock);
        c->ping_id = id;
    }
}

static void rf_ctrl(rf_client *c, rf_app *a, uint32_t mode, unsigned app_passes)
{
    uint8_t ctrl[AAR_MSG_BYTES];
    uint32_t id = c->ctrl_id + 1U;
    unsigned guard;

    zz9k_rt_build_ctrl(ctrl, id, mode);
    for (guard = 0U; guard < 100000U; guard++) {
        if (SENDMSG(c->h, ctrl) == AAR_OK) {
            zz9k_rt_note_ctrl(&c->t, id, mode);
            c->ctrl_id = id;
            return;
        }
        rf_drain(c);
        rf_app_step(a, app_passes);
    }
    CHECK(0 && "the CTRL never found credit");
}

static void test_ringflood_logic(void)
{
    static rf_client c;
    static rf_app a;
    AARHandle *h;
    uint8_t m[AAR_MSG_BYTES];
    unsigned step, guard;
    AARZZ9000Stats xs;

    reset_all();
    h = claim();
    memset(&c, 0, sizeof(c));
    memset(&a, 0, sizeof(a));
    c.h = h;
    c.app = &a;
    c.app_per_get = 2U;
    zz9k_rt_init(&c.t);

    /* INFO first, as the app does. */
    memset(m, 0, sizeof(m));
    zz9k_rt_put(m, 0, RF_INFO);
    zz9k_rt_put(m, 1, ZZ9K_RT_VERSION);
    zz9k_rt_put(m, 2, ZZ9K_RT_NO_COUNTER);
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    rf_drain(&c);
    CHECK(c.t.have_info);

    /* Idle: only pings, answered. */
    c.t.phase = ZZ9K_RT_PHASE_IDLE;
    for (step = 0U; step < 40U; step++) {
        rf_ping(&c);
        rf_app_step(&a, 4U);
        rf_drain(&c);
    }
    /* Flood: the app is far faster than the client's bounded drain, so the
     * inbox fills over and over. */
    rf_ctrl(&c, &a, ZZ9K_RT_MODE_FLOOD, 4U);
    c.t.phase = ZZ9K_RT_PHASE_FLOOD;
    for (step = 0U; step < 600U; step++) {
        if (step % 5U == 0U)
            rf_ping(&c);
        rf_app_step(&a, 300U);
        rf_drain(&c);
    }
    CHECK(a.seq > 10U * AAR_CORE_INBOX);
    CHECK(a.busy > 0U);
    xs = stats_of(h);
    CHECK(xs.sq_blocked > 0U);
    /* Quiet: the ACK must come back in order behind the last DATA. */
    rf_ctrl(&c, &a, ZZ9K_RT_MODE_QUIET, 300U);
    for (guard = 0U; guard < 100000U && zz9k_rt_ctrls_outstanding(&c.t); guard++) {
        rf_app_step(&a, 300U);
        rf_drain(&c);
    }
    CHECK(zz9k_rt_ctrls_outstanding(&c.t) == 0U);
    /* Idle again, then DONE. */
    c.t.phase = ZZ9K_RT_PHASE_IDLE;
    for (step = 0U; step < 40U; step++) {
        rf_ping(&c);
        rf_app_step(&a, 4U);
        rf_drain(&c);
    }
    for (guard = 0U; guard < 1000U && zz9k_rt_pings_outstanding(&c.t); guard++) {
        rf_app_step(&a, 4U);
        rf_drain(&c);
    }
    memset(m, 0, sizeof(m));
    zz9k_rt_put(m, 0, RF_DONE);
    zz9k_rt_put(m, 1, a.seq);
    zz9k_rt_put(m, 2, a.clock);
    zz9k_rt_put(m, 3, a.counter);
    zz9k_rt_put(m, 4, a.busy);
    zz9k_rt_put(m, 5, a.pongs);
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    for (guard = 0U; guard < 100U && !c.t.have_done; guard++)
        rf_drain(&c);
    CHECK(c.t.have_done);

    CHECK(c.t.lost == 0U && c.t.gaps == 0U && c.t.reordered == 0U);
    CHECK(c.t.bad_messages == 0U && c.t.unknown == 0U);
    CHECK(c.t.messages == a.seq);
    CHECK(c.t.ack_unmatched == 0U && c.t.ack_count_bad == 0U &&
          c.t.data_in_quiet == 0U);
    CHECK(c.t.pings_sent > 100U && zz9k_rt_pings_outstanding(&c.t) == 0U);
    CHECK(c.t.rtt[ZZ9K_RT_PHASE_FLOOD].n >= ZZ9K_RT_MIN_PONGS);
    CHECK(c.t.rtt[ZZ9K_RT_PHASE_IDLE].n >= ZZ9K_RT_MIN_PONGS);
    CHECK(c.t.pong_count_bad == 0U && c.t.tail_lost == 0U);
    CHECK(zz9k_rt_failures(&c.t) == 0U);
    CHECK(zzapp_xport_stats(card.rec)->msgs_overflow == 0U);
    CHECK(CLOSE(h) == AAR_OK);
}

static aar_event key_event(uint16_t type, uint16_t code)
{
    aar_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    return ev;
}

static void test_events_coalescing_and_drops(void)
{
    AARHandle *h;
    aar_event ev, got;
    uint32_t i;
    AARZZ9000Stats s;

    reset_all();
    h = claim();

    /* Keys are delivered one by one, in order. */
    for (i = 0U; i < 10U; i++) {
        ev = key_event(AAR_EV_KEY_DOWN, (uint16_t)(0x20U + i));
        ev.time_ms = i;
        CHECK(POST(h, &ev) == AAR_OK);
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
        ev = key_event(AAR_EV_KEY_UP, (uint16_t)i);
        CHECK(POST(h, &ev) == AAR_OK);
    }
    /* Full now: another key is dropped and counted in the control block. */
    CHECK(POST(h, &ev) == AAR_E_BUSY);
    CHECK(aar_get_be32(h->cb + AAR_CB_EVENTS_DROPPED) == 1U);
    CHECK(aar_get_be32(h->cb + AAR_CB_CQ_FULL) == 1U);
    s = stats_of(h);
    CHECK(s.events_dropped == 1U);
    /* Mouse motion meanwhile costs no entry: it is held and summed. */
    for (i = 0U; i < 1000U; i++) {
        ev = key_event(AAR_EV_MOUSE, 0U);
        ev.dx = 3; ev.dy = -1; ev.time_ms = 500U + i;
        CHECK(POST(h, &ev) == AAR_OK);
    }
    s = stats_of(h);
    CHECK(s.events_merged == 999U);
    CHECK(aar_get_be32(h->cb + AAR_CB_EVENTS_DROPPED) == 1U);    /* none lost */
    CHECK(zzapp_xport_pump(card.rec) == 64);
    CHECK(zzapp_xport_event_pending(card.rec) == 64U);
    for (i = 0U; i < 64U; i++) {
        CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.code == i);
    }
    /* Room again: the held motion goes out as one event with the sums. */
    dispatch();
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1);
    CHECK(got.type == AAR_EV_MOUSE && got.dx == 3000 && got.dy == -1000 &&
          got.time_ms == 1499U);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 0);
    dispatch();                                    /* nothing pending */
    CHECK(zzapp_xport_pump(card.rec) == 0);

    /* A key after motion keeps the order: motion first, then the key. */
    ev = key_event(AAR_EV_MOUSE, 0U);
    ev.dx = 5; ev.dy = 6;
    /* The CQ is empty, so the motion goes out at once; hold the next. */
    CHECK(POST(h, &ev) == AAR_OK);
    ev.dx = 1;
    CHECK(POST(h, &ev) == AAR_OK);
    ev = key_event(AAR_EV_KEY_DOWN, 0x45U);
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(zzapp_xport_pump(card.rec) == 3);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_MOUSE && got.dx == 5);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_MOUSE && got.dx == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_KEY_DOWN);

    /* A button change is not folded into earlier motion. */
    ev = key_event(AAR_EV_MOUSE, 0U);
    ev.buttons = 0; ev.dx = 4;
    for (i = 0U; i < 64U; i++) {            /* fill the CQ so events stay held */
        aar_event k = key_event(AAR_EV_KEY_UP, (uint16_t)i);

        CHECK(POST(h, &k) == AAR_OK);
    }
    CHECK(POST(h, &ev) == AAR_OK);       /* held */
    ev.buttons = 1; ev.dx = 2;
    CHECK(POST(h, &ev) == AAR_E_BUSY);   /* cannot flush: dropped */
    s = stats_of(h);
    CHECK(s.events_merged == 999U);      /* the click was not folded in */
    CHECK(zzapp_xport_pump(card.rec) == 64);
    for (i = 0U; i < 64U; i++)
        CHECK(zzapp_xport_take_event(card.rec, &got) == 1);
    dispatch();
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.dx == 4 && got.buttons == 0);
    /* The click, retried, goes out now. */
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.buttons == 1 && got.dx == 2);

    /* Saturation: motion past 16 bits does not wrap. */
    for (i = 0U; i < 64U; i++) {
        aar_event k = key_event(AAR_EV_KEY_UP, 0U);

        CHECK(POST(h, &k) == AAR_OK);
    }
    ev = key_event(AAR_EV_MOUSE, 0U);
    ev.dx = 30000;
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(zzapp_xport_pump(card.rec) == 64);
    for (i = 0U; i < 64U; i++)
        (void)zzapp_xport_take_event(card.rec, &got);
    dispatch();
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.dx == 32767);
    CHECK(CLOSE(h) == AAR_OK);
}

/* Each event and message that reaches the CQ rings one KICK with CQ_POSTED, so
 * an app parked on it is woken by the card's pass at once. A post that is
 * refused, held or merged rings nothing, and a bell the mailbox refuses is
 * kept for the next pass. */
static void test_events_and_messages_ring_the_doorbell(void)
{
    AARHandle *h;
    aar_event ev, got;
    uint8_t m[AAR_MSG_BYTES] = { 0 };
    unsigned before;
    uint32_t i;

    reset_all();
    h = claim();

    /* A key and a message: one bell each. */
    ev = key_event(AAR_EV_KEY_DOWN, 0x20U);
    before = card.kicks;
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(card.kicks == before + 1U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    CHECK(zzapp_xport_kick_take(card.rec) == ZZAPP_KICK_CQ_POSTED);
    before = card.kicks;
    CHECK(SENDMSG(h, m) == AAR_OK);
    CHECK(card.kicks == before + 1U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    CHECK(zzapp_xport_kick_take(card.rec) == ZZAPP_KICK_CQ_POSTED);
    CHECK(zzapp_xport_pump(card.rec) == 2);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1);

    /* Motion that goes out at once rings; motion that is merged does not. */
    ev = key_event(AAR_EV_MOUSE, 0U);
    ev.dx = 1;
    before = card.kicks;
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(card.kicks == before + 1U);
    (void)zzapp_xport_kick_take(card.rec);
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1);

    /* A full CQ refuses the event, holds the motion: no bell for either. */
    for (i = 0U; i < 64U; i++) {
        ev = key_event(AAR_EV_KEY_UP, (uint16_t)i);
        CHECK(POST(h, &ev) == AAR_OK);
    }
    before = card.kicks;
    CHECK(POST(h, &ev) == AAR_E_BUSY);
    CHECK(card.kicks == before);
    ev = key_event(AAR_EV_MOUSE, 0U);
    ev.dx = 2;
    CHECK(POST(h, &ev) == AAR_OK);       /* held */
    CHECK(POST(h, &ev) == AAR_OK);       /* merged */
    CHECK(card.kicks == before);
    CHECK(SENDMSG(h, m) == AAR_E_BUSY);   /* no room */
    CHECK(card.kicks == before);
    (void)zzapp_xport_kick_take(card.rec);
    CHECK(zzapp_xport_pump(card.rec) == 64);
    for (i = 0U; i < 64U; i++)
        CHECK(zzapp_xport_take_event(card.rec, &got) == 1);
    /* The held motion goes out once there is room, and rings once. */
    dispatch();
    CHECK(card.kicks == before + 1U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    (void)zzapp_xport_kick_take(card.rec);
    CHECK(zzapp_xport_pump(card.rec) == 1);
    CHECK(zzapp_xport_take_event(card.rec, &got) == 1 && got.dx == 4);

    /* The credit runs out: the messages that fit ring, the refused one not.
     * The one sent above is still unconsumed, so one credit is gone. */
    (void)zzapp_xport_pump(card.rec);
    before = card.kicks;
    for (i = 0U; i < AAR_MSG_CAPACITY - 1U; i++)
        CHECK(SENDMSG(h, m) == AAR_OK);
    CHECK(card.kicks == before + AAR_MSG_CAPACITY - 1U);
    CHECK(SENDMSG(h, m) == AAR_E_BUSY);
    CHECK(card.kicks == before + AAR_MSG_CAPACITY - 1U);
    (void)zzapp_xport_kick_take(card.rec);
    CHECK(zzapp_xport_pump(card.rec) == (int)AAR_MSG_CAPACITY - 1);

    /* The mailbox is busy with the last bell: the event is posted, the bell is
     * kept and goes out from the next dispatcher pass. */
    ev = key_event(AAR_EV_KEY_DOWN, 0x21U);
    before = card.kicks;
    card.post_busy = 1U;
    CHECK(POST(h, &ev) == AAR_OK);
    CHECK(card.kicks == before);
    dispatch();
    CHECK(card.kicks == before + 1U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    dispatch();
    CHECK(card.kicks == before + 1U);                     /* nothing pending */
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_logs(void)
{
    AARHandle *h;
    AARServiceReq req;
    AARZZ9000Stats s;

    reset_all();
    h = claim();
    req_init(&req);
    /* Without a sink the records are dropped and counted, not stuck. */
    CHECK(zzapp_xport_post_log(card.rec, "hello", 5U) == AAR_OK);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);
    s = stats_of(h);
    CHECK(s.logs_unclaimed == 1U && s.logs == 0U);
    CHECK(CLOSE(h) == AAR_OK);
}

static char sink_seen[8][AAR_LOG_MAX + 1];
static uint32_t sink_len[8];
static unsigned sink_count;
static void *sink_user_seen;

static void log_sink_fn(APTR user, const void *data, ULONG len)
{
    sink_user_seen = (void *)(uintptr_t)user;
    if (sink_count < 8U) {
        memcpy(sink_seen[sink_count], data, len);
        sink_seen[sink_count][len] = '\0';
        sink_len[sink_count] = len;
    }
    sink_count++;
}

static void test_log_sink(void)
{
    AARHandle *h;
    AARServiceReq req;
    char big[2000];
    AARZZ9000Stats s;

    reset_all();
    h = claim();
    req_init(&req);
    /* The sink is a call of the base library (AARSetLogSink), not a member of
     * the ZZ9000 table; the test calls the core function behind it. */
    sink_count = 0U;
    CHECK(aar_core_set_log_sink(&core, h, log_sink_fn, (APTR)0x1234U) == AAR_OK);
    CHECK(zzapp_xport_post_log(card.rec, "first line", 10U) == AAR_OK);
    memset(big, 'z', sizeof(big));
    CHECK(zzapp_xport_post_log(card.rec, big, sizeof(big)) == AAR_OK);   /* truncated to 1024 */
    CHECK(zzapp_xport_post_log(card.rec, "third", 5U) == AAR_OK);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);
    CHECK(sink_count == 3U && PTRU(sink_user_seen) == 0x1234U);
    CHECK(sink_len[0] == 10U && !strcmp(sink_seen[0], "first line"));
    CHECK(sink_len[1] == AAR_LOG_MAX && sink_seen[1][0] == 'z' &&
          sink_seen[1][AAR_LOG_MAX - 1U] == 'z');
    CHECK(!strcmp(sink_seen[2], "third"));
    s = stats_of(h);
    CHECK(s.logs == 3U);
    /* A log record between two requests does not hide the second. */
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
    CHECK(zzapp_xport_post_log(card.rec, "fourth", 6U) == AAR_OK);
    CHECK(SNEXT(h, &req) == AAR_OK && req.op == AAR_OP_FILE_STAT);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY && sink_count == 4U);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_doorbell(void)
{
    AARHandle *h;
    AARServiceReq req;
    unsigned i;
    AARZZ9000Stats s;

    reset_all();
    h = claim();
    req_init(&req);
    /* A reply rings once. */
    fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
    CHECK(SNEXT(h, &req) == AAR_OK);
    req.result = 0;
    CHECK(SREPLY(h, &req) == AAR_OK);
    CHECK(card.kicks == 1U && zzapp_xport_needs_pump(card.rec) == 1);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    (void)zzapp_xport_kick_take(card.rec);

    /* The mailbox is busy with the last bell: bits coalesce and go out later,
     * combined, from the next pass. */
    card.post_busy = 1000U;
    for (i = 0U; i < 3U; i++) {
        fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_FILE_STAT, 0U, 0U, 0U, 0U, 0U);
        CHECK(SNEXT(h, &req) == AAR_OK);
        req.result = 0;
        CHECK(SREPLY(h, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &(aar_completion){ 0 }) == 1);
    }
    CHECK(card.kicks == 1U);                          /* every attempt refused */
    card.post_busy = 0U;
    s = stats_of(h);
    CHECK(s.kicks_coalesced >= 2U);
    dispatch();
    CHECK(card.kicks == 2U && card.last_kick_flags == ZZAPP_KICK_CQ_POSTED);
    dispatch();
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
        CHECK(SNEXT(h, &req) == AAR_OK);
        CHECK(zzapp_xport_sq_space(card.rec) == 1);
        dispatch();
        CHECK(card.kick_flags_or & ZZAPP_KICK_SQ_SPACE);
        req.result = 0;
        CHECK(SREPLY(h, &req) == AAR_OK);
    }
    /* Unknown bits are not sent by the platform. */
    {
        unsigned kicks = card.kicks;

        CHECK(ops()->kick(aar_plat_get(), &h->claim, 0x80U) == AAR_OK);
        CHECK(card.kicks == kicks);
    }
    CHECK(CLOSE(h) == AAR_OK);
}

/* The doorbell request belongs to the claim. A request in flight on one claim
 * does not block another, and a claim that goes away withdraws its own. */
static void test_doorbell_is_per_claim(void)
{
    AARHandle *a, *b;
    aar_event ev;
    unsigned kicks;

    reset_all();
    a = claim();
    b = claim();
    card.bell_hold = 1;                               /* requests stay in flight */
    ev = key_event(AAR_EV_KEY_DOWN, 1U);
    kicks = card.kicks;
    CHECK(POST(a, &ev) == AAR_OK);
    CHECK(card.kicks == kicks + 1U);
    CHECK(POST(b, &ev) == AAR_OK);                    /* b has a bell of its own */
    CHECK(card.kicks == kicks + 2U);
    /* a's bell is still in flight: its next ring waits and keeps its bits. */
    CHECK(POST(a, &ev) == AAR_OK);
    CHECK(card.kicks == kicks + 2U);
    CHECK(a->kick_pending != 0U);
    card.bell_hold = 0;
    dispatch();                                       /* retires and rings again */
    CHECK(card.kicks >= kicks + 3U && a->kick_pending == 0U);
    /* The same pass retired the request it had just replaced. */
    CHECK(card.last_bell != NULL && card.last_bell->words[0] == 0U);

    /* Closing withdraws a request that is still in flight, from its own bell. */
    card.bell_hold = 1;
    CHECK(POST(b, &ev) == AAR_OK);
    CHECK(card.bell_cancels == 0U);
    CHECK(CLOSE(b) == AAR_OK);
    CHECK(card.bell_cancels == 1U && card.cancel_bell == card.last_bell);
    card.bell_hold = 0;
    CHECK(CLOSE(a) == AAR_OK);
    CHECK(card.bell_cancels == 1U);                   /* a had nothing in flight */
    CHECK(heap.live == 0U);
}

/* The owner rings and the dispatcher retires the bell of one claim at the same
 * time. The platform serialises them: one request per bell at a time. */
static void *bell_ring_thread(void *arg)
{
    AARHandle *h = arg;
    unsigned i;

    for (i = 0U; i < 200000U; i++)
        (void)ops()->kick(aar_plat_get(), &h->claim, ZZAPP_KICK_CQ_POSTED);
    return NULL;
}

static void *bell_poll_thread(void *arg)
{
    AARHandle *h = arg;
    unsigned i;

    for (i = 0U; i < 200000U; i++)
        ops()->poll(aar_plat_get(), &h->claim);
    return NULL;
}

static void test_doorbell_two_tasks_one_claim(void)
{
    AARHandle *h;
    pthread_t t1, t2;

    reset_all();
    h = claim();
    card.bell_mt = 1;
    CHECK(pthread_create(&t1, NULL, bell_ring_thread, h) == 0);
    CHECK(pthread_create(&t2, NULL, bell_poll_thread, h) == 0);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    CHECK(card.mt_double == 0U);
    CHECK(card.mt_posts > 0U && card.mt_dones <= card.mt_posts);
    card.bell_mt = 0;
    CHECK(CLOSE(h) == AAR_OK);
}

/* poll says whether the claim's doorbell request is still in flight, so the
 * dispatcher keeps polling that claim only until the card has taken it. */
static void test_poll_reports_in_flight(void)
{
    AARHandle *h;

    reset_all();
    h = claim();
    CHECK(ops()->poll(aar_plat_get(), &h->claim) == 0);   /* nothing rung yet */
    card.bell_hold = 1;
    CHECK(ops()->kick(aar_plat_get(), &h->claim, ZZAPP_KICK_CQ_POSTED) == AAR_OK);
    CHECK(ops()->poll(aar_plat_get(), &h->claim) == 1);   /* the card has not taken it */
    CHECK(ops()->poll(aar_plat_get(), &h->claim) == 1);
    card.bell_hold = 0;
    CHECK(ops()->poll(aar_plat_get(), &h->claim) == 0);   /* retired */
    CHECK(ops()->poll(aar_plat_get(), &h->claim) == 0);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_load_start_stop_status(void)
{
    AARHandle *h;
    AARStatusInfo st;
    struct aar_core_tags t;
    static uint8_t image[150000];
    const char *argv[3] = { "swquake", "-nosound", "+map demo1" };
    volatile uint8_t *staging;
    uint32_t chunk_max, i;
    aar_event got;

    reset_all();
    h = claim();
    CHECK(cbw(AAR_CB_STATE) == AAR_STATE_CLAIMED);
    for (i = 0U; i < sizeof(image); i++)
        image[i] = (uint8_t)(i * 31U + 7U);

    /* Tags: limits go through the control block at load begin, the client
     * capabilities through LOAD_BEGIN. A bad tag changes nothing. */
    aar_core_tags_init(&t);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PRIVATE_MIB, 64U) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_SHARED_MIB, 20U) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, 3U) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_CLIENT_CAPS,
                            AAR_CCAP_DISPLAY_PAN_ACK | AAR_CCAP_DISPLAY_CLUT8) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PRIVATE_MIB, 4096U) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_SHARED_MIB, 4096U) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_WEIGHT, ZZAPP_WEIGHT_MAX + 1U) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_CLIENT_CAPS, 0x80U) == AAR_E_INVAL);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_BASE + 99U, 1U) == AAR_E_INVAL);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_OK);

    CHECK(aar_core_load_chunk(&core, h, 0U, 10U) == AAR_E_INVAL);          /* no begin */
    CHECK(aar_core_start(&core, h, argv, 3U) == AAR_E_INVAL);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    CHECK(card.limits[0] == 64U && card.limits[1] == 20U && card.limits[2] == 3U);
    /* The lease is extended unless a tag says otherwise. */
    CHECK(card.caps_seen == (ZZAPP_CLIENT_LEASE_EXTENDED | ZZAPP_CLIENT_SCANOUT_ACK |
                             ZZAPP_CLIENT_CLUT8));
    CHECK(card.image_len == sizeof(image) && card.chunks == 3U &&
          !memcmp(card.image, image, sizeof(image)));
    CHECK(card.long_calls == 2U);                    /* begin, end */
    /* Once the load began the limits and tags are fixed, in the core and in
     * the platform. */
    aar_core_tags_init(&t);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_PRIVATE_MIB, 1U) == AAR_OK);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_BUSY);
    CHECK(aar_core_load_begin(&core, h, 100U, &staging, &chunk_max) == AAR_E_BUSY);

    CHECK(aar_core_start(&core, h, argv, 3U) == AAR_OK);
    CHECK(card.argc == 3U && !strcmp(card.argv[0], "swquake") &&
          !strcmp(card.argv[1], "-nosound") && !strcmp(card.argv[2], "+map demo1"));
    CHECK(card.argv_off[0] >= ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF);
    CHECK(card.started && h->load == NULL);
    CHECK(!zzapp_xport_block_get(card.rec, ZZAPP_XPORT_BLOCK_LOAD, NULL));
    CHECK(aar_core_start(&core, h, argv, 3U) == AAR_E_INVAL);          /* twice */
    /* After START the limits are fixed too (the card no longer reads them):
     * the core refuses, and so does the platform if it is asked directly. */
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_BUSY);
    CHECK(aar_core_load_begin(&core, h, 100U, &staging, &chunk_max) == AAR_E_BUSY);
    {
        struct aar_plat_load_opts opts;
        uint32_t reason = 0U;

        memset(&opts, 0, sizeof(opts));
        CHECK(ops()->load_begin(aar_plat_get(), &h->claim, &opts, &staging,
                                &chunk_max, &reason) == AAR_E_BUSY);
        CHECK(ops()->load_chunk(aar_plat_get(), &h->claim, 0U, 16U) == AAR_E_INVAL);
        CHECK(ops()->load_end(aar_plat_get(), &h->claim, &reason) == AAR_E_INVAL);
    }

    /* Status comes from the control block. */
    zzapp_xport_cb_set_fault(card.rec, AAR_FAULT_DATA, 0x805U, 0x60000010U,
                             0x00401234U, 0x7fff0000U, 0x00400100U, 0xdeadbeefU);
    zzapp_xport_cb_put(card.rec, AAR_CB_EXIT_CODE, (uint32_t)-5);
    zzapp_xport_cb_put(card.rec, AAR_CB_FLAGS, AAR_CBF_DISPLAY | AAR_CBF_CONSOLE);
    zzapp_xport_cb_put64(card.rec, AAR_CB_LOAD_BASE_HI, 0x0000000100400000ULL);
    zzapp_xport_cb_set_state(card.rec, AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    memset(&st, 0xcc, sizeof(st));
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK);
    CHECK(st.size == sizeof(st) && st.generation == aar_handle_generation(h->claim.handle) &&
          st.state == AAR_STATE_FAULTED && st.end_reason == AAR_END_USER_FAULT &&
          st.exit_code == -5 && st.flags == (AAR_CBF_DISPLAY | AAR_CBF_CONSOLE) &&
          st.fault_class == AAR_FAULT_DATA && st.fault_syndrome == 0x805U &&
          st.fault_pstate == 0x60000010U && st.fault_pc.hi == 0U && st.fault_pc.lo == 0x00401234U &&
          st.fault_sp.lo == 0x7fff0000U && st.fault_lr.lo == 0x00400100U &&
          st.fault_addr.lo == 0xdeadbeefU && st.load_base.hi == 1U &&
          st.load_base.lo == 0x00400000U);
    CHECK(st.lease_period_ms == cbw(AAR_CB_LEASE_PERIOD_MS));
    CHECK((((uint64_t)st.shared_used.hi << 32) | st.shared_used.lo) ==
          zzapp_xport_shared_used(card.rec));
    /* A poll of three words reads three words and leaves the rest alone. */
    memset(&st, 0xcc, sizeof(st));
    st.size = 16U;
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.size == 16U &&
          st.state == AAR_STATE_FAULTED && st.end_reason == AAR_END_USER_FAULT &&
          st.exit_code == (LONG)0xcccccccc);
    st.size = 3U;
    CHECK(aar_core_status(&core, h, &st) == AAR_E_INVAL);

    /* Stop: the word, the app event and the mailbox op. */
    CHECK(aar_core_stop(&core, h) == AAR_OK && card.stopped);
    CHECK(zzapp_xport_cb_read_client(card.rec, AAR_CB_STOP_REQUEST) != 0U);
    CHECK(zzapp_xport_pump(card.rec) == 1 &&
          zzapp_xport_take_event(card.rec, &got) == 1 && got.type == AAR_EV_STOP);
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.state == AAR_STATE_IDLE &&
          st.end_reason == AAR_END_FORCED);
    CHECK(aar_core_stop(&core, h) == AAR_OK);                         /* again */

    /* Argument limits. */
    CHECK(CLOSE(h) == AAR_OK);
    h = claim();
    {
        const char *toomany[17] = { "a" };
        char longarg[200];

        memset(longarg, 'x', sizeof(longarg) - 1U);
        longarg[sizeof(longarg) - 1U] = '\0';
        CHECK(aar_core_load_memory(&core, h, image, 100U) == AAR_OK);
        CHECK(aar_core_start(&core, h, toomany, 17U) == AAR_E_INVAL);
        toomany[0] = longarg;
        CHECK(aar_core_start(&core, h, toomany, 1U) == AAR_E_INVAL);
        CHECK(aar_core_start(&core, h, NULL, 0U) == AAR_OK && card.argc == 0U);
    }
    CHECK(CLOSE(h) == AAR_OK && heap.live == 0U);
}

/* Every client capability the tags carry reaches LOAD_BEGIN as the platform's
 * own bit. */
static void test_tags_reach_the_card(void)
{
    AARHandle *h;
    struct aar_core_tags t;
    static uint8_t image[100];

    reset_all();
    h = claim();
    aar_core_tags_init(&t);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_LEASE_EXTENDED, 0U) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_AUDIO_ROUTE, AAR_AUDIO_ROUTE_AHI) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_CLIENT_CAPS, AAR_CCAP_KNOWN) == AAR_OK);
    CHECK(aar_core_tags_add(&core, &t, AAR_TAG_AUDIO_ROUTE, 7U) == AAR_E_INVAL);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_OK);
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    CHECK(card.caps_seen == (ZZAPP_CLIENT_AUDIO_AHI | ZZAPP_CLIENT_CLUT8 |
                             ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_LATEST));
    CHECK(CLOSE(h) == AAR_OK);
    /* The defaults: the extended lease, nothing else. */
    h = claim();
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
    CHECK(card.caps_seen == ZZAPP_CLIENT_LEASE_EXTENDED);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_load_refused_and_unknown_caps(void)
{
    AARHandle *h;
    static uint8_t image[1000];
    AARStatusInfo st;
    volatile uint8_t *staging;
    uint32_t chunk_max;
    struct aar_plat_load_opts opts;
    uint32_t reason = 0U;

    reset_all();
    h = claim();
    /* The platform refuses capability bits it does not know. */
    memset(&opts, 0, sizeof(opts));
    opts.client_caps = 0x80000000U;
    CHECK(ops()->load_begin(aar_plat_get(), &h->claim, &opts, &staging,
                            &chunk_max, &reason) == AAR_E_INVAL);
    CHECK(card.long_calls == 0U);
    /* The card cannot allocate the load buffer: the refusal reason arrives. */
    zzapp_xport_set_shared_limit(card.rec, 0x4000U);   /* the pair alone fills it */
    CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_E_NORESOURCE);
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.reason == AAR_REASON_HOSTHEAP);
    CHECK(CLOSE(h) == AAR_OK);
}

/* The lease belongs to the vertical-blank interrupt: one store per claim about
 * every five ticks, only while the dispatcher is alive. */
static void test_lease(void)
{
    AARHandle *h, *g;
    const aar_client_ext_zz9000 *ext;
    uint32_t before, last, i;
    zzapp_rec_t *hrec;

    reset_all();
    h = claim();
    hrec = card.rec;                                /* later claims replace card.rec */
    ext = aar_core_ext(&core, AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL);
    dispatch();                                     /* the dispatcher is alive */
    before = zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER);
    CHECK(before == 1U);
    for (i = 0U; i < 5U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) == before + 1U);
    for (i = 0U; i < 50U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) == before + 11U);

    /* The word never reads 0 again, also across the wrap. */
    h->lease = 0xffffffffU;
    for (i = 0U; i < 10U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) != 0U && h->lease >= 1U && h->lease <= 2U);

    /* A second claim is renewed too, a tick apart from the first. */
    g = claim();
    for (i = 0U; i < 5U; i++)
        aar_core_vblank(&core);
    CHECK(aar_get_be32(g->cb + AAR_CB_LEASE_COUNTER) >= 2U);
    CHECK(CLOSE(g) == AAR_OK);

    /* The dispatcher stalls: after about five seconds the interrupt stops
     * storing, so the card's own lease expiry stays a backstop. */
    for (i = 0U; i < AAR_CORE_STALL_TICKS + 10U; i++)
        aar_core_vblank(&core);
    last = zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER);
    for (i = 0U; i < 100U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) == last);
    dispatch();                                     /* it is back */
    for (i = 0U; i < 5U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) != last);

    /* The test hook: no renewal for a while, then it resumes. */
    CHECK(ext->lease_pause(h, 1000U) == AAR_OK);
    dispatch();
    last = zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER);
    for (i = 0U; i < 40U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) == last);
    for (i = 0U; i < 20U; i++)
        aar_core_vblank(&core);
    CHECK(zzapp_xport_cb_get(hrec, AAR_CB_LEASE_COUNTER) != last);
    CHECK(ext->lease_pause(h, 0U) == AAR_OK);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_staleness_after_release(void)
{
    AARHandle *h, *y;
    AARServiceReq req;
    aar_event ev = { .type = AAR_EV_KEY_DOWN };
    uint8_t m[AAR_MSG_BYTES] = { 7 }, got[AAR_MSG_BYTES];
    AARStatusInfo st;
    AARMessage msg;
    struct AARSharedMap map;
    struct aar_core_tags t;
    uint32_t old;
    ULONG token;

    reset_all();
    heap.keep_freed = 1;
    h = claim();
    old = h->claim.handle;
    req_init(&req);
    memset(&map, 0, sizeof(map));
    map.size = sizeof(map);
    aar_core_tags_init(&t);
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);   /* the message is in the inbox now */

    CHECK(CLOSE(h) == AAR_OK);
    CHECK(card.released && heap.live == 0U && heap.dead_count == 1U);
    /* The pointer is dead. Every call answers STALE without touching it, and
     * nothing writes to the freed blocks. */
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_E_STALE);
    CHECK(POST(h, &ev) == AAR_E_STALE);
    CHECK(SENDMSG(h, m) == AAR_E_STALE);
    CHECK(SNEXT(h, &req) == AAR_E_STALE);
    CHECK(SREPLY(h, &req) == AAR_E_STALE);
    CHECK(aar_core_stop(&core, h) == AAR_E_STALE);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_STALE);
    CHECK(aar_core_load_memory(&core, h, m, sizeof(m)) == AAR_E_STALE);
    CHECK(aar_core_start(&core, h, NULL, 0U) == AAR_E_STALE);
    CHECK(aar_core_shared_map(&core, h, 0x10000U, 16U, 0U, &map) == AAR_E_STALE);
    CHECK(aar_core_shared_token(&core, h, m, 4U, &token) == AAR_E_STALE);
    CHECK(aar_core_audio_read(&core, h, m, 1U, &token) == AAR_E_STALE);
    CHECK(GETMSG(h, got) == AAR_E_STALE);
    CHECK(aar_core_message_signal(&core, h) == 0U);
    CHECK(CLOSE(h) == AAR_E_STALE);                  /* a second close finds nothing */
    (void)msg;
    dispatch();
    aar_core_vblank(&core);
    CHECK(graveyard_intact());
    CHECK(card.kicks == 0U);

    /* The index is reused by the next claim, with a newer generation: the old
     * handle is stale on the card. */
    {
        unsigned n;

        for (n = 0U; n < 40U; n++) {
            y = claim();
            if (aar_handle_index(y->claim.handle) == aar_handle_index(old))
                break;
            CHECK(CLOSE(y) == AAR_OK);
        }
        CHECK(n < 40U && aar_handle_index(y->claim.handle) == aar_handle_index(old));
        CHECK(aar_handle_generation(y->claim.handle) != aar_handle_generation(old));
        {
            int status = 0;

            CHECK(zzapp_xport_resolve(old, &status) == NULL && status == AAR_E_STALE);
            CHECK(zzapp_xport_resolve(y->claim.handle, &status) == card.rec);
        }
        CHECK(CLOSE(y) == AAR_OK);
    }
}

/* Only the task that opened a handle may use it. */
static void test_ownership(void)
{
    AARHandle *h, *mine;
    AARServiceReq req;
    AARStatusInfo st;
    uint8_t m[AAR_MSG_BYTES] = { 0 };
    aar_event ev = { .type = AAR_EV_KEY_DOWN };
    struct AARSharedMap map;
    struct aar_core_tags t;
    ULONG token;

    reset_all();
    h = claim();
    req_init(&req);
    memset(&map, 0, sizeof(map));
    map.size = sizeof(map);
    aar_core_tags_init(&t);
    st.size = sizeof(st);

    current_task = TASK_B;
    CHECK(aar_core_status(&core, h, &st) == AAR_E_OWNER);
    CHECK(POST(h, &ev) == AAR_E_OWNER);
    CHECK(SENDMSG(h, m) == AAR_E_OWNER);
    CHECK(GETMSG(h, m) == AAR_E_OWNER);
    CHECK(SNEXT(h, &req) == AAR_E_OWNER);
    CHECK(SREPLY(h, &req) == AAR_E_OWNER);
    CHECK(aar_core_stop(&core, h) == AAR_E_OWNER);
    CHECK(aar_core_start(&core, h, NULL, 0U) == AAR_E_OWNER);
    CHECK(aar_core_load_memory(&core, h, m, sizeof(m)) == AAR_E_OWNER);
    CHECK(aar_core_set_tags(&core, h, &t) == AAR_E_OWNER);
    CHECK(aar_core_shared_map(&core, h, 0x10000U, 16U, 0U, &map) == AAR_E_OWNER);
    CHECK(aar_core_shared_token(&core, h, m, 4U, &token) == AAR_E_OWNER);
    CHECK(aar_core_audio_read(&core, h, m, 1U, &token) == AAR_E_OWNER);
    CHECK(CLOSE(h) == AAR_E_OWNER);
    /* The signal mask is the one call any task may make. */
    CHECK(aar_core_message_signal(&core, h) == 0x400U);
    /* The other task has its own handle, unaffected. */
    mine = claim();
    CHECK(aar_core_status(&core, mine, &st) == AAR_OK);
    CHECK(aar_core_status(&core, h, &st) == AAR_E_OWNER);
    CHECK(CLOSE(mine) == AAR_OK);
    current_task = TASK_A;
    /* Nothing changed for the owner. */
    CHECK(aar_core_status(&core, h, &st) == AAR_OK && st.state == AAR_STATE_CLAIMED);
    /* The last close of a task closes what it still holds. */
    h = claim();
    current_task = TASK_B;
    mine = claim();
    (void)mine;
    CHECK(aar_core_close_owner(&core) == 1U);
    current_task = TASK_A;
    CHECK(aar_core_close_owner(&core) == 2U);
    CHECK(heap.live == 0U);
}

static void test_staleness_after_revocation(void)
{
    AARHandle *h;
    AARServiceReq req;
    AARStatusInfo st;
    uint8_t m[AAR_MSG_BYTES] = { 9 }, got[AAR_MSG_BYTES];
    uint32_t before, i;
    int rc;

    reset_all();
    heap.keep_freed = 1;
    h = claim();
    req_init(&req);
    dispatch();
    /* A message that arrives before the revocation (checked below). */
    CHECK(zzapp_xport_post_msg(card.rec, m) == AAR_OK);
    CHECK(SNEXT(h, &req) == AAR_E_BUSY);
    /* The card revoked the handle (lease lost, quarantine) but holds the
     * shell. The client does not know yet: its mapping is still the card's,
     * so the interrupt's store is harmless. The first mailbox call it makes
     * answers STALE and the card releases the shell then. */
    card.revoked = h->claim.handle;
    before = cbw(AAR_CB_LEASE_COUNTER);
    for (i = 0U; i < 5U; i++)
        aar_core_vblank(&core);
    CHECK(cbw(AAR_CB_LEASE_COUNTER) == before + 1U);
    CHECK(heap.live == 1U);
    rc = aar_core_stop(&core, h);
    CHECK(rc == AAR_E_STALE);
    CHECK(h->cb == NULL && h->revoked && !h->renew);
    CHECK(heap.live == 0U && heap.dead_count == 1U);
    /* After that the client never touches the memory again, and the interrupt
     * no longer stores. */
    for (i = 0U; i < 50U; i++)
        aar_core_vblank(&core);
    dispatch();
    st.size = sizeof(st);
    CHECK(aar_core_status(&core, h, &st) == AAR_E_STALE);
    CHECK(SNEXT(h, &req) == AAR_E_STALE);
    CHECK(SENDMSG(h, m) == AAR_E_STALE);
    CHECK(graveyard_intact());
    /* A message that arrived before the revocation is not handed out: a
     * revoked handle answers STALE before anything else. */
    CHECK(GETMSG(h, got) == AAR_E_STALE);
    /* The handle is still the caller's: closing it is fine. */
    CHECK(CLOSE(h) == AAR_OK);

    /* Revocation seen through a long call. */
    card.revoked = 0U;
    h = claim();
    card.revoked = h->claim.handle;
    CHECK(aar_core_load_memory(&core, h, "abcd", 4U) == AAR_E_STALE);
    CHECK(h->cb == NULL && h->revoked && heap.live == 1U);
    /* The library then releases the shell with a plain RELEASE when the owner
     * closes the handle. */
    CHECK(CLOSE(h) == AAR_OK);
    CHECK(card.last_op == ZZAPP_OP_RELEASE);
    CHECK(heap.live == 0U && graveyard_intact());

    /* Release of a revoked claim by its owner is fine too. */
    card.revoked = 0U;
    h = claim();
    card.revoked = h->claim.handle;
    CHECK(CLOSE(h) == AAR_OK);
    CHECK(heap.live == 0U && graveyard_intact());
}

/* The platform's own claim state, driven through its operations: release is
 * idempotent and clears the handle word; a STALE answer ends the load. */
static void test_platform_claim_state(void)
{
    AARHandle *h;
    volatile uint8_t *staging;
    uint32_t chunk_max;
    unsigned calls;
    const char *argv[1] = { "x" };

    reset_all();
    h = claim();
    calls = card.calls;
    CHECK(ops()->release(aar_plat_get(), &h->claim) == AAR_OK);
    CHECK(h->claim.handle == 0U && card.released && card.calls == calls + 1U);
    CHECK(ops()->release(aar_plat_get(), &h->claim) == AAR_OK);
    CHECK(card.calls == calls + 1U);                  /* nothing more is sent */
    CHECK(ops()->stop(aar_plat_get(), &h->claim) == AAR_E_STALE);
    CHECK(ops()->load_chunk(aar_plat_get(), &h->claim, 0U, 16U) == AAR_E_STALE);
    CHECK(card.calls == calls + 1U);
    CHECK(CLOSE(h) == AAR_OK);

    heap.keep_freed = 1;
    h = claim();
    CHECK(aar_core_load_begin(&core, h, 100U, &staging, &chunk_max) == AAR_OK);
    card.revoked = h->claim.handle;
    CHECK(ops()->load_chunk(aar_plat_get(), &h->claim, 0U, 16U) == AAR_E_STALE);
    /* The load buffer is gone with the claim: start must not write to it. */
    CHECK(ops()->start(aar_plat_get(), &h->claim, argv, 1U) == AAR_E_INVAL);
    CHECK(ops()->load_end(aar_plat_get(), &h->claim, &(uint32_t){ 0U }) == AAR_E_INVAL);
    CHECK(CLOSE(h) == AAR_OK);
    CHECK(heap.live == 0U && graveyard_intact());
}

static void test_teardown_returns_everything(void)
{
    AARHandle *h, *x, *y;
    zzapp_rec_stats_t st0, st1;
    zzapp_xport_buf_t b;
    static uint8_t image[70000];
    const char *argv[1] = { "app" };
    unsigned i;

    reset_all();
    zzapp_rec_stats(&st0);
    for (i = 0U; i < 5U; i++) {
        h = claim();
        CHECK(zzapp_xport_service_area(card.rec, ZZAPP_XPORT_AREA_LOG, &b) == AAR_OK);
        CHECK(zzapp_xport_data_reserve(card.rec, 0x20000U, &b) == AAR_OK);
        CHECK(zzapp_xport_block_alloc(card.rec, ZZAPP_XPORT_BLOCK_AUDIO,
                                      ZZAPP_XPORT_AUDIO_BYTES(8192U), &b) == AAR_OK);
        CHECK(aar_core_load_memory(&core, h, image, sizeof(image)) == AAR_OK);
        if (i & 1U)
            CHECK(aar_core_start(&core, h, argv, 1U) == AAR_OK);
        CHECK(heap.live >= 4U);
        CHECK(CLOSE(h) == AAR_OK);
        /* A started app is stopped before it is released. */
        CHECK((i & 1U) ? card.stopped : !card.stopped);
        CHECK(heap.live == 0U && heap.bad_frees == 0U);
    }
    zzapp_rec_stats(&st1);
    CHECK(st1.free == st0.free && st1.claimed == st0.claimed);

    /* The final close of the library closes whatever is open. */
    x = claim();
    CHECK(aar_core_load_memory(&core, x, image, 100U) == AAR_OK);
    CHECK(aar_core_start(&core, x, argv, 1U) == AAR_OK);
    y = claim();
    (void)y;
    aar_core_shutdown(&core);
    CHECK(heap.live == 0U && card.released && card.stopped);
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

/* The app opens its audio ring with AUDIO_OPEN: the library checks the ring
 * before it tells the app it is open, and reads it with AARAudioRead. */
static int audio_open(AARHandle *h, const zzapp_xport_buf_t *ring, uint32_t frames,
                      aar_completion *c)
{
    AARServiceReq req;
    uint32_t id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_AUDIO_OPEN, 44100U, 2U,
                             ring->token, 0U, ring->bytes);
    int rc;

    req_init(&req);
    CHECK(SNEXT(h, &req) == AAR_OK && req.op == AAR_OP_AUDIO_OPEN && req.id == id);
    req.result = AAR_OK;
    req.out[AAR_R_AUDIO_OPEN_FRAMES] = frames;
    /* A ring that does not check turns the completion into a refusal, and the
     * call reports why. */
    rc = SREPLY(h, &req);
    CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, c) == 1 && c->id == id);
    return rc;
}

static void test_audio_ring(void)
{
    AARHandle *h;
    AARServiceReq req;
    zzapp_xport_buf_t ring;
    aar_completion c;
    volatile uint8_t *r;
    int16_t out[64 * 2];
    ULONG got;
    uint32_t i;
    const uint32_t frames = 8192U;

    reset_all();
    h = claim();
    req_init(&req);
    CHECK(zzapp_xport_block_alloc(card.rec, ZZAPP_XPORT_BLOCK_AUDIO,
                                  ZZAPP_XPORT_AUDIO_BYTES(frames), &ring) == AAR_OK);
    r = ring.ptr;
    CHECK(aar_core_audio_read(&core, h, out, 10U, &got) == AAR_E_INVAL);   /* not open */
    /* A header that does not say S16LE stereo: the open is refused. */
    CHECK(audio_open(h, &ring, frames, &c) == AAR_E_IO);
    CHECK(c.result == AAR_E_IO);
    write_le32(r + AAR_AUDIO_RING_RATE, 44100U);
    write_le32(r + AAR_AUDIO_RING_CHANNELS, 2U);
    write_le32(r + AAR_AUDIO_RING_FORMAT, AAR_AUDIO_FORMAT_S16LE);
    CHECK(audio_open(h, &ring, 1000U, &c) == AAR_E_INVAL);   /* not a power of two */
    CHECK(c.result == AAR_E_INVAL);
    CHECK(aar_core_audio_read(&core, h, out, 10U, &got) == AAR_E_INVAL);
    CHECK(audio_open(h, &ring, frames, &c) == AAR_OK);
    CHECK(c.result == AAR_OK && c.out[AAR_R_AUDIO_OPEN_FRAMES] == frames);

    /* The app writes 40 frames. The client reads 64: 40 arrive, one underrun. */
    for (i = 0U; i < 40U; i++) {
        int16_t l = (int16_t)(-1000 + (int)i * 7), rr = (int16_t)(i * 300);

        r[0x80 + i * 4 + 0] = (uint8_t)l;           r[0x80 + i * 4 + 1] = (uint8_t)((uint16_t)l >> 8);
        r[0x80 + i * 4 + 2] = (uint8_t)rr;          r[0x80 + i * 4 + 3] = (uint8_t)((uint16_t)rr >> 8);
    }
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 40U);
    CHECK(aar_core_audio_read(&core, h, out, 64U, &got) == AAR_OK && got == 40U);
    for (i = 0U; i < 40U; i++)
        CHECK(out[i * 2] == (int16_t)(-1000 + (int)i * 7) && out[i * 2 + 1] == (int16_t)(i * 300));
    CHECK(read_le32(r + AAR_AUDIO_RING_READ_FRAME) == 40U);
    CHECK(read_le32(r + AAR_AUDIO_RING_UNDERRUNS) == 1U);
    /* An exact read is no underrun. */
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 50U);
    CHECK(aar_core_audio_read(&core, h, out, 10U, &got) == AAR_OK && got == 10U);
    CHECK(read_le32(r + AAR_AUDIO_RING_UNDERRUNS) == 1U);

    /* Free-running indices across the wrap of the ring and of 2^32. */
    write_le32(r + AAR_AUDIO_RING_READ_FRAME, 0xfffffff0U);
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 0x00000010U);       /* 32 frames */
    for (i = 0U; i < 32U; i++) {
        uint32_t at = 0x80U + (((0xfffffff0U + i) & (frames - 1U)) * 4U);

        write_le32(r + at, 0x12340000U + i);                       /* l = i, r = 0x1234 */
    }
    CHECK(aar_core_audio_read(&core, h, out, 32U, &got) == AAR_OK && got == 32U);
    for (i = 0U; i < 32U; i++)
        CHECK(out[i * 2] == (int16_t)i && out[i * 2 + 1] == 0x1234);
    CHECK(read_le32(r + AAR_AUDIO_RING_READ_FRAME) == 0x10U);
    /* A write index that runs further ahead than the ring is corrupt. */
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 0x10U + frames + 1U);
    CHECK(aar_core_audio_read(&core, h, out, 4U, &got) == AAR_E_IO && got == 0U);
    /* AUDIO_CLOSE detaches the ring. */
    {
        uint32_t id = fw_request(ZZAPP_XPORT_REQ_BLOCKING, AAR_OP_AUDIO_CLOSE, 0U, 0U,
                                 0U, 0U, 0U);

        CHECK(SNEXT(h, &req) == AAR_OK && req.op == AAR_OP_AUDIO_CLOSE && req.id == id);
        req.result = AAR_OK;
        CHECK(SREPLY(h, &req) == AAR_OK);
        CHECK(fw_completion(ZZAPP_XPORT_REQ_BLOCKING, &c) == 1 && c.id == id);
    }
    CHECK(aar_core_audio_read(&core, h, out, 4U, &got) == AAR_E_INVAL);
    CHECK(CLOSE(h) == AAR_OK);
}

/* The ring's index words are shared with the app, which reads and writes each
 * with one 32-bit access. The library must do the same, or a carry out of the
 * low byte (0x00ff to 0x0100) shows the other side a half-written word. The
 * app side is played by a thread that touches the words only with 32-bit
 * atomic accesses, as the ARM does. */
typedef struct {
    volatile uint8_t *ring;
    int stop;
    uint32_t bad;                   /* observations that were never a real value */
} tear_ctx_t;

static void *flip_write_thread(void *arg)
{
    tear_ctx_t *c = arg;
    volatile uint32_t *w = (volatile uint32_t *)(volatile void *)(c->ring + AAR_AUDIO_RING_WRITE_FRAME);

    while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED)) {
        __atomic_store_n(w, 0x000000ffU, __ATOMIC_RELAXED);
        __atomic_store_n(w, 0x00000100U, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void *watch_read_thread(void *arg)
{
    tear_ctx_t *c = arg;
    volatile uint32_t *r = (volatile uint32_t *)(volatile void *)(c->ring + AAR_AUDIO_RING_READ_FRAME);

    while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED)) {
        uint32_t v = __atomic_load_n(r, __ATOMIC_RELAXED);

        /* The test only ever stores 0xff, and the library stores 0x100. */
        if (v != 0xffU && v != 0x100U)
            c->bad++;
    }
    return NULL;
}

static void test_audio_ring_words_are_never_torn(void)
{
    AARHandle *h;
    zzapp_xport_buf_t ring;
    aar_completion c;
    volatile uint8_t *r;
    tear_ctx_t ctx;
    pthread_t th;
    int16_t out[2];
    static int16_t big[0x400 * 2];
    ULONG got;
    uint32_t i, bad = 0U;
    const uint32_t frames = 1024U;

    reset_all();
    h = claim();
    CHECK(zzapp_xport_block_alloc(card.rec, ZZAPP_XPORT_BLOCK_AUDIO,
                                  ZZAPP_XPORT_AUDIO_BYTES(frames), &ring) == AAR_OK);
    r = ring.ptr;
    write_le32(r + AAR_AUDIO_RING_RATE, 22050U);
    write_le32(r + AAR_AUDIO_RING_CHANNELS, 2U);
    write_le32(r + AAR_AUDIO_RING_FORMAT, AAR_AUDIO_FORMAT_S16LE);
    CHECK(audio_open(h, &ring, frames, &c) == AAR_OK);
    CHECK(c.result == AAR_OK);

    /* The library reads write_frame while the app moves it across a carry. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = r;
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 0xffU);
    write_le32(r + AAR_AUDIO_RING_READ_FRAME, 0U);
    CHECK(pthread_create(&th, NULL, flip_write_thread, &ctx) == 0);
    /* A read of 0x400 frames returns what is available: the write index as
     * the app left it. A half-written index shows as another count. */
    for (i = 0U; i < 100000U; i++) {
        __atomic_store_n((volatile uint32_t *)(volatile void *)(r + AAR_AUDIO_RING_READ_FRAME),
                         0U, __ATOMIC_RELAXED);
        if (aar_core_audio_read(&core, h, big, 0x400U, &got) != AAR_OK ||
            (got != 0xffU && got != 0x100U))
            bad++;
    }
    __atomic_store_n(&ctx.stop, 1, __ATOMIC_RELAXED);
    pthread_join(th, NULL);
    CHECK(bad == 0U);

    /* The library writes read_frame (0xff to 0x100, a carry every time) while
     * the app watches it. */
    write_le32(r + AAR_AUDIO_RING_WRITE_FRAME, 0x100U);
    write_le32(r + AAR_AUDIO_RING_READ_FRAME, 0xffU);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = r;
    CHECK(pthread_create(&th, NULL, watch_read_thread, &ctx) == 0);
    for (i = 0U; i < 300000U; i++) {
        __atomic_store_n((volatile uint32_t *)(volatile void *)(r + AAR_AUDIO_RING_READ_FRAME),
                         0xffU, __ATOMIC_RELAXED);
        if (aar_core_audio_read(&core, h, out, 1U, &got) != AAR_OK || got != 1U)
            bad++;
    }
    __atomic_store_n(&ctx.stop, 1, __ATOMIC_RELAXED);
    pthread_join(th, NULL);
    CHECK(bad == 0U);
    CHECK(ctx.bad == 0U);
    CHECK(read_le32(r + AAR_AUDIO_RING_READ_FRAME) == 0x100U);
    CHECK(CLOSE(h) == AAR_OK);
}

static void test_corrupt_queue(void)
{
    AARHandle *h;
    AARServiceReq req;
    AARMessage msg;
    aar_event ev = { .type = AAR_EV_KEY_DOWN };
    uint8_t m[AAR_MSG_BYTES] = { 0 };

    reset_all();
    h = claim();
    req_init(&req);
    aar_put_be32(h->cb + AAR_CB_SQ_TAIL, 1000U);      /* more than the depth */
    CHECK(SNEXT(h, &req) == AAR_E_IO);
    CHECK(aar_core_get_message(&core, h, &msg) == AAR_E_IO);
    aar_put_be32(h->cb + AAR_CB_SQ_TAIL, 0U);
    aar_put_be32(h->cb + AAR_CB_CQ_HEAD, 5000U);      /* the app side's index */
    CHECK(POST(h, &ev) == AAR_E_IO);
    CHECK(SENDMSG(h, m) == AAR_E_IO);
    aar_put_be32(h->cb + AAR_CB_CQ_HEAD, 0U);
    aar_put_be32(h->cb + AAR_CB_MSG_CONSUMED, 100U);  /* ahead of the posted count */
    CHECK(SENDMSG(h, m) == AAR_E_IO);
    CHECK(CLOSE(h) == AAR_OK);
}

/* ---- Two threads, one per role ------------------------------------------------------ */

static void full_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#define ROUNDS 3000U

static AARHandle *tx;
static volatile int fw_done, cl_stop;
static uint32_t cl_events_posted, cl_events_dropped_seen, cl_msgs_sent;
static uint32_t fw_events_taken, fw_msgs_taken, fw_requests_done;
static int thread_error;

static void *client_thread(void *arg)
{
    AARServiceReq req;
    uint8_t m[AAR_MSG_BYTES];
    uint8_t got[AAR_MSG_BYTES];
    uint32_t next_in = 0U, seq = 0U, ev_seq = 0U;

    (void)arg;
    memset(m, 0, sizeof(m));
    req_init(&req);
    while (!cl_stop || cl_msgs_sent < ROUNDS) {
        int rc = SNEXT(tx, &req);

        if (rc != AAR_OK && rc != AAR_E_BUSY) { thread_error = 1; return NULL; }
        if (rc == AAR_OK) {
            req.result = AAR_OK;
            req.out[0] = req.arg[0] * 2U + 1U;
            while (SREPLY(tx, &req) == AAR_E_BUSY)
                ;
        }
        while (GETMSG(tx, got) == AAR_OK) {
            uint32_t v = (uint32_t)got[0] | ((uint32_t)got[1] << 8) |
                         ((uint32_t)got[2] << 16) | ((uint32_t)got[3] << 24);

            if (v != next_in)
                thread_error = 2;
            next_in++;
        }
        if (cl_msgs_sent < ROUNDS) {
            m[0] = (uint8_t)seq; m[1] = (uint8_t)(seq >> 8);
            m[2] = (uint8_t)(seq >> 16); m[3] = (uint8_t)(seq >> 24);
            if (SENDMSG(tx, m) == AAR_OK) {
                seq++;
                cl_msgs_sent++;
            }
        }
        {
            aar_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.type = AAR_EV_KEY_DOWN;
            ev.time_ms = ev_seq;
            if (POST(tx, &ev) == AAR_OK)
                cl_events_posted++;
            ev_seq++;
        }
    }
    cl_events_dropped_seen = aar_get_be32(tx->cb + AAR_CB_EVENTS_DROPPED);
    return NULL;
}

/* The dispatcher and the interrupt run beside the client: the lease counter is
 * stored from here (the interrupt) and the doorbell retried and retired from
 * here (the dispatcher). */
static void *service_thread(void *arg)
{
    (void)arg;
    while (!cl_stop || cl_msgs_sent < ROUNDS) {
        aar_core_dispatch(&core, NULL, NULL);
        aar_core_vblank(&core);
        sched_yield();
    }
    return NULL;
}

static void test_two_threads(void)
{
    pthread_t t, svc;
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
    aar_zz9000_host_board(&board);
    aar_core_shutdown(&core);
    aar_core_detach(&core);
    CHECK(aar_core_attach(&core) == AAR_OK);
    tx = claim();
    /* Three threads: the client (the owner), the service thread (the interrupt
     * and the dispatcher) and this one (the firmware). The client and the
     * dispatcher both ring the doorbell of the one claim, one after the other:
     * the core keeps them apart while the owner is inside a call, and the
     * platform's try-lock covers the rest. */
    fw_done = cl_stop = 0;
    cl_events_posted = cl_events_dropped_seen = cl_msgs_sent = 0U;
    fw_events_taken = fw_msgs_taken = fw_requests_done = 0U;
    thread_error = 0;
    aar_core_dispatch(&core, NULL, NULL);
    CHECK(pthread_create(&t, NULL, client_thread, NULL) == 0);
    CHECK(pthread_create(&svc, NULL, service_thread, NULL) == 0);

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
    pthread_join(svc, NULL);
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
    CHECK(aar_get_be32(tx->cb + AAR_CB_LEASE_COUNTER) != 0U);
    CHECK(CLOSE(tx) == AAR_OK);
    (void)cl_events_dropped_seen;
    (void)out_msgs;
}

/* ---- Event counts ------------------------------------------------------------- */

static uint32_t evw(unsigned word)
{
    return aar_get_be32(zzapp_xport_events_block() + 4U * word);
}

static void evw_put(unsigned word, uint32_t v)
{
    aar_put_be32(zzapp_xport_events_block() + 4U * word, v);
}

/* The firmware posts one event for the record `index` (any index, whatever
 * the record arena holds). */
static void ev_post(uint32_t index)
{
    static zzapp_rec_t rec;

    rec.hdr.handle = (7U << 16) | index;
    zzapp_xport_event_post(&rec);
}

#define NEWS_CLAIMS 8U

/* A claim of the platform that the card does not know: claim_news looks only
 * at its handle and its own storage. */
static struct aar_plat_claim *fake_claim(unsigned n, uint32_t index)
{
    static struct aar_plat_claim c[NEWS_CLAIMS];
    static uint8_t priv[NEWS_CLAIMS][1024];

    CHECK(ops()->claim_priv_bytes <= sizeof(priv[0]));
    memset(&c[n], 0, sizeof(c[n]));
    memset(priv[n], 0, sizeof(priv[n]));
    c[n].handle = (3U << 16) | index;
    c[n].priv = priv[n];
    return &c[n];
}

#define SAMPLE() ops()->events_sample(aar_plat_get())
#define NEWS(c) ops()->claim_news(aar_plat_get(), (c))

/* A firmware that reports 0 in the word that held the state (every firmware
 * before the event block) gives no events: nothing is mapped and the sample
 * says so, whatever sits in the card's memory. */
static void test_events_absent_on_older_firmware(void)
{
    AARHandle *h;
    unsigned maps;

    ops()->detach(aar_plat_get());
    reset_all();
    CHECK(zzapp_xport_events_init() == 0);        /* a block, but QUERY says 0 */
    CHECK(card.events == 0);
    CHECK(ops()->events_sample != NULL && ops()->claim_news != NULL);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NONE);      /* before any QUERY */
    h = claim();
    maps = card.map_calls;
    CHECK(maps == 2U);                            /* the pair: one map, extended */
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NONE);
    ev_post(0U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NONE);
    CHECK(CLOSE(h) == AAR_OK);

    /* The reply of a card that has a block names it: the same open maps it,
     * whole, once, with the pair. */
    reset_all_events();
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NONE);      /* not mapped before the first QUERY */
    h = claim();
    CHECK(card.map_calls == 3U && card.map_violations == 0U);
    CHECK(card.maps[0].token == zzapp_xport_events_token() &&
          card.maps[0].bytes == ZZAPP_EVENT_BLOCK_BYTES);
    CHECK(SAMPLE() != AAR_PLAT_EVENTS_NONE);
    CHECK(CLOSE(h) == AAR_OK);
    h = claim();                                  /* a second claim maps no event block again */
    CHECK(card.map_calls == 5U);                  /* its pair only */
    CHECK(CLOSE(h) == AAR_OK);
}

/* One word is read when nothing moved. The group words are read only after
 * the global count changed: the test plants other values in the group words
 * and sees when the library notices them. */
static void test_events_sample_reads_one_word(void)
{
    struct aar_plat_claim *a;
    unsigned i;
    AARHandle *h;

    reset_all_events();
    h = claim();                                  /* maps the block */
    a = fake_claim(0U, 3U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);      /* the first sample */
    CHECK(NEWS(a) == 1);                          /* never asked */
    CHECK(NEWS(a) == 0);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);

    /* Group 0 moves without the global count: not read. */
    evw_put(1U, 41U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);
    CHECK(NEWS(a) == 0);
    /* The global count moves: the groups are read, the planted value shows. */
    evw_put(0U, 1U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(a) == 1);
    CHECK(NEWS(a) == 0);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);

    /* The firmware's own posts: one pass sees any number of them. */
    for (i = 0U; i < 5U; i++)
        ev_post(3U);
    CHECK(evw(0U) == 6U && evw(1U) == 46U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(a) == 1);
    CHECK(NEWS(a) == 0);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);
    CHECK(CLOSE(h) == AAR_OK);
}

/* A claim sees its group's count, an unknown group the global count, and a
 * wrapped count is a change like any other. */
static void test_claim_news_per_group(void)
{
    struct aar_plat_claim *g0, *g1, *g14, *u480, *uffff, *g0b;
    AARHandle *h;

    reset_all_events();
    h = claim();
    g0 = fake_claim(0U, 5U);
    g0b = fake_claim(1U, 31U);                    /* the same group, the last record */
    g1 = fake_claim(2U, 32U);                     /* the first of group 1 */
    g14 = fake_claim(3U, 479U);                   /* the last known record */
    u480 = fake_claim(4U, 480U);                  /* the first unknown */
    uffff = fake_claim(5U, 0xffffU);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    /* Every claim is news at its first question, then not. */
    CHECK(NEWS(g0) == 1 && NEWS(g0b) == 1 && NEWS(g1) == 1 && NEWS(g14) == 1 &&
          NEWS(u480) == 1 && NEWS(uffff) == 1);
    CHECK(NEWS(g0) == 0 && NEWS(g0b) == 0 && NEWS(g1) == 0 && NEWS(g14) == 0 &&
          NEWS(u480) == 0 && NEWS(uffff) == 0);

    ev_post(5U);                                  /* group 0 */
    CHECK(evw(0U) == 1U && evw(1U) == 1U && evw(2U) == 0U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(g0) == 1 && NEWS(g0b) == 1);
    CHECK(NEWS(g1) == 0 && NEWS(g14) == 0);
    CHECK(NEWS(u480) == 1 && NEWS(uffff) == 1);   /* the global count moved */
    CHECK(NEWS(g0) == 0 && NEWS(g0b) == 0 && NEWS(u480) == 0 && NEWS(uffff) == 0);

    ev_post(32U);                                 /* group 1 */
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(g1) == 1);
    CHECK(NEWS(g0) == 0 && NEWS(g0b) == 0 && NEWS(g14) == 0);
    CHECK(NEWS(u480) == 1 && NEWS(uffff) == 1);

    ev_post(479U);                                /* group 14, the last */
    CHECK(evw(15U) == 1U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(g14) == 1);
    CHECK(NEWS(g0) == 0 && NEWS(g1) == 0);
    CHECK(NEWS(u480) == 1);

    ev_post(480U);                                /* no group: only the global count */
    CHECK(evw(0U) == 4U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(u480) == 1 && NEWS(uffff) == 1);
    CHECK(NEWS(g0) == 0 && NEWS(g0b) == 0 && NEWS(g1) == 0 && NEWS(g14) == 0);
    CHECK(NEWS(u480) == 0);

    /* Counts that wrap: the group and the global word go from all ones to 0,
     * which differs from what the claim saw. */
    evw_put(1U, 0xffffffffU);
    evw_put(0U, 0xffffffffU);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(g0) == 1 && NEWS(u480) == 1);
    CHECK(NEWS(g0) == 0 && NEWS(u480) == 0);
    ev_post(7U);
    CHECK(evw(0U) == 0U && evw(1U) == 0U);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(NEWS(g0) == 1 && NEWS(u480) == 1);
    CHECK(NEWS(g1) == 0);
    CHECK(CLOSE(h) == AAR_OK);
}

/* The real claims of the arena: what the firmware posts for them reaches the
 * claim of the right record and no other. */
static void test_events_with_real_claims(void)
{
    AARHandle *a, *b;
    uint32_t id = 0U;
    aar_request r;
    uint8_t msg[AAR_MSG_BYTES];

    reset_all_events();
    a = claim();
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 1);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 0);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);

    /* An SQ entry of the card: a request, a message, an end reason. */
    memset(&r, 0, sizeof(r));
    r.op = AAR_OP_LOG;
    CHECK(zzapp_xport_post_request(card.rec, ZZAPP_XPORT_REQ_BLOCKING, &r, 0U,
                                   &id) == AAR_OK);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 1);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 0);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);
    memset(msg, 0, sizeof(msg));
    CHECK(zzapp_xport_post_msg(card.rec, msg) == AAR_OK);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 1);
    zzapp_xport_cb_set_state(card.rec, AAR_STATE_IDLE, AAR_END_FORCED);
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_NEWS);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 1);
    CHECK(ops()->claim_news(aar_plat_get(), &a->claim) == 0);

    /* A claim of the next record index is in the same group: it sees these
     * events too, as the groups are coarse. */
    current_task = TASK_B;
    b = claim();
    current_task = TASK_A;
    CHECK(SAMPLE() == AAR_PLAT_EVENTS_QUIET);
    CHECK(ops()->claim_news(aar_plat_get(), &b->claim) == 1);
    CHECK(ops()->claim_news(aar_plat_get(), &b->claim) == 0);
    current_task = TASK_B;
    CHECK(CLOSE(b) == AAR_OK);
    current_task = TASK_A;
    CHECK(CLOSE(a) == AAR_OK);
}

int main(void)
{
    test_open_and_refusal();
    test_abi_and_capability_checks();
    test_query_and_extension();
    test_list_walks_claims_with_gaps();
    test_requests_end_to_end();
    test_list_names_cross_the_request_buffer();
    test_mapping_cache_and_shared_maps();
    test_bad_buffer_is_refused();
    test_messages_and_credits();
    test_full_inbox_holds_messages_at_the_head();
    test_ringflood_logic();
    test_events_coalescing_and_drops();
    test_events_and_messages_ring_the_doorbell();
    test_logs();
    test_log_sink();
    test_doorbell();
    test_doorbell_is_per_claim();
    test_doorbell_two_tasks_one_claim();
    test_poll_reports_in_flight();
    test_load_start_stop_status();
    test_tags_reach_the_card();
    test_load_refused_and_unknown_caps();
    test_lease();
    test_staleness_after_release();
    test_ownership();
    test_staleness_after_revocation();
    test_teardown_returns_everything();
    test_platform_claim_state();
    test_audio_ring();
    test_audio_ring_words_are_never_torn();
    test_corrupt_queue();
    test_two_threads();
    test_events_absent_on_older_firmware();
    test_events_sample_reads_one_word();
    test_claim_news_per_group();
    test_events_with_real_claims();
    printf("aar_zz9000: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
