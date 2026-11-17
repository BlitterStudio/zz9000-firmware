/*
 * Host unit tests for the card pool allocator (card_pool.c).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Build/run: make -C test/card_pool test
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "card_pool.h"
#include "rasterix_fence_guard.h"
#include "memorymap.h"

/* Single-threaded host build. smp_cpu_id is switchable so a test can leave
 * the lock held by "core 1"; taking a held raw lock would spin forever on
 * the card, so here it fails the run instead. */
static int current_cpu = 0;
int smp_cpu_id(void) { return current_cpu; }
uint32_t smp_local_irq_save(void) { return 0u; }
void smp_local_irq_restore(uint32_t s) { (void)s; }
void smp_raw_spin_lock(volatile uint32_t *w)
{
    if (*w) {
        printf("FAIL: pool lock is held by another core (deadlock)\n");
        exit(1);
    }
    *w = 1u;
}
int smp_raw_spin_try_lock(volatile uint32_t *w)
{
    if (*w) return 0;
    *w = 1u;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *w) { *w = 0u; }

#define PAGE CARD_POOL_PAGE_BYTES
#define MB(n) ((uint32_t)(n) << 20)

#define MEDIA(i)    CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, (i))
#define IMAGE(i)    CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, (i))
#define FIRMWARE(i) CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, (i))
#define SDK_SURFACE(i) CARD_POOL_OWNER(CARD_POOL_CLASS_SDK_SURFACE, (i))
#define CONSOLE(i)  CARD_POOL_OWNER(CARD_POOL_CLASS_CONSOLE, (i))
#define MINIGL(i)   CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, (i))
#define MINIGL_CONTEXT(i) CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL_CONTEXT, (i))
#define APP(i)      CARD_POOL_OWNER(CARD_POOL_CLASS_APP, (i))

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static card_pool_t pool;

/* Two small open ranges: "A" at 0x10000000 and "C" at 0x30000000. */
#define A_BASE 0x10000000u
#define C_BASE 0x30000000u
static void two_ranges(uint32_t a_size, uint32_t c_size)
{
    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, A_BASE, A_BASE + a_size, 0) == 0);
    CHECK(card_pool_add_range(&pool, C_BASE, C_BASE + c_size, 0) == 1);
    card_pool_open_range(&pool, 0);
    card_pool_open_range(&pool, 1);
}

static void test_fills_ranges_in_order(void)
{
    two_ranges(2 * PAGE, MB(4));
    uint32_t a = card_pool_alloc(&pool, 100, MEDIA(1));
    uint32_t b = card_pool_alloc(&pool, PAGE, MEDIA(1));
    uint32_t c = card_pool_alloc(&pool, PAGE + 1, MEDIA(1));
    CHECK(a == A_BASE);
    CHECK(b == A_BASE + PAGE);
    CHECK(c == C_BASE);
    CHECK(pool.class_used[CARD_POOL_CLASS_MEDIA] == 4 * PAGE);
    uint32_t d = card_pool_alloc(&pool, 1, MEDIA(1));
    CHECK(d == C_BASE + 2 * PAGE);
}

static void test_aligned_allocation_skips_unaligned_range(void)
{
    uint32_t addr;

    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, A_BASE + PAGE,
                              A_BASE + PAGE + MB(1), 0) == 0);
    CHECK(card_pool_add_range(&pool, C_BASE, C_BASE + MB(2), 0) == 1);
    card_pool_open_range(&pool, 0);
    card_pool_open_range(&pool, 1);
    addr = card_pool_alloc_aligned(&pool, MB(1), MB(1), MEDIA(1));
    CHECK(addr == C_BASE);
    CHECK((addr & (MB(1) - 1u)) == 0u);
}

static void test_block_ownership(void)
{
    uint32_t address;

    two_ranges(MB(1), MB(1));
    address = card_pool_alloc(&pool, PAGE + 1, MEDIA(1));
    CHECK(card_pool_block_owned(&pool, address, PAGE + 1, MEDIA(1)));
    CHECK(card_pool_block_owned(&pool, address, 2 * PAGE, MEDIA(1)));
    CHECK(!card_pool_block_owned(&pool, address, 2 * PAGE + 1, MEDIA(1)));
    CHECK(!card_pool_block_owned(&pool, address, PAGE, MEDIA(2)));
    CHECK(!card_pool_block_owned(&pool, address + PAGE, PAGE, MEDIA(1)));
    CHECK(card_pool_free(&pool, address, MEDIA(1)) == 1);
    CHECK(!card_pool_block_owned(&pool, address, PAGE, MEDIA(1)));
}

static void test_range_owned_rejects_past_end(void)
{
    uint32_t address;

    two_ranges(MB(1), MB(1));
    address = card_pool_alloc(&pool, PAGE, MEDIA(1));
    CHECK(address != 0);
    CHECK(card_pool_range_owned(&pool, address, PAGE, MEDIA(1)));
    CHECK(card_pool_range_owned(&pool, address + PAGE - 4U, 4U, MEDIA(1)));
    CHECK(!card_pool_range_owned(&pool, address + PAGE, 4U, MEDIA(1)));
    CHECK(!card_pool_range_owned(&pool, address + PAGE - 4U, 8U, MEDIA(1)));
    CHECK(!card_pool_range_owned(&pool, address + PAGE + 0x1000U, 1U, MEDIA(1)));
}

static void test_too_large_leaves_table(void)
{
    two_ranges(MB(1), MB(1));
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == A_BASE);
    card_pool_t before = pool;
    CHECK(card_pool_alloc(&pool, MB(1) + PAGE, MEDIA(1)) == 0);
    CHECK(card_pool_alloc(&pool, 0, MEDIA(1)) == 0);
    CHECK(card_pool_alloc(&pool, UINT32_MAX, MEDIA(1)) == 0);
    CHECK(card_pool_alloc(&pool, PAGE, 0) == 0);
    CHECK(memcmp(&before, &pool, sizeof(pool)) == 0);
}

static void test_class_limit(void)
{
    two_ranges(MB(200), MB(200));
    CHECK(card_pool_alloc(&pool, MB(72), IMAGE(1)) != 0);
    CHECK(card_pool_alloc(&pool, PAGE, IMAGE(2)) == 0);
    CHECK(card_pool_alloc(&pool, MB(128), MEDIA(1)) != 0);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == 0);
    CHECK(card_pool_alloc(&pool, MB(64),
                          CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA_INPUT, 1)) != 0);
    CHECK(card_pool_alloc(&pool, PAGE,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA_INPUT, 2)) == 0);
    CHECK(card_pool_alloc(&pool, MB(32), CONSOLE(1)) != 0);
    CHECK(card_pool_alloc(&pool, PAGE, CONSOLE(2)) == 0);
    CHECK(card_pool_alloc(&pool, MB(64), FIRMWARE(1)) != 0);
    CHECK(card_pool_alloc(&pool, PAGE, FIRMWARE(2)) == 0);
    CHECK(card_pool_release_owner(&pool, CONSOLE(1)) == MB(32));
    CHECK(card_pool_alloc(&pool, PAGE, CONSOLE(2)) != 0);
    CHECK(card_pool_release_owner(&pool, IMAGE(1)) == MB(72));
    CHECK(card_pool_alloc(&pool, PAGE, IMAGE(2)) != 0);
}

static void test_wrong_owner_and_unknown_frees(void)
{
    two_ranges(MB(1), MB(1));
    uint32_t a = card_pool_alloc(&pool, PAGE, MEDIA(1));
    CHECK(card_pool_free(&pool, a, MEDIA(2)) == 0);
    CHECK(card_pool_free(&pool, a, IMAGE(1)) == 0);
    CHECK(card_pool_free(&pool, a + PAGE, MEDIA(1)) == 0);
    CHECK(card_pool_free(&pool, A_BASE + 0x100, MEDIA(1)) == 0);
    CHECK(pool.ignored_frees == 4);
    CHECK(pool.block_count == 1);
    /* Still live: the next page goes after it. */
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == a + PAGE);
    CHECK(card_pool_free(&pool, a, MEDIA(1)) == 1);
    CHECK(card_pool_free(&pool, a, MEDIA(1)) == 0);
    CHECK(pool.ignored_frees == 5);
    CHECK(card_pool_free(&pool, 0, MEDIA(1)) == 0);
    CHECK(pool.ignored_frees == 5);
}

static void test_release_owner_merges_gaps(void)
{
    two_ranges(4 * PAGE, 0x10000u);
    uint32_t a = card_pool_alloc(&pool, PAGE, MEDIA(7));
    uint32_t b = card_pool_alloc(&pool, PAGE, MEDIA(8));
    uint32_t c = card_pool_alloc(&pool, PAGE, MEDIA(8));
    uint32_t d = card_pool_alloc(&pool, PAGE, MEDIA(7));
    CHECK(a == A_BASE && b == A_BASE + PAGE && c == A_BASE + 2 * PAGE &&
          d == A_BASE + 3 * PAGE);
    CHECK(card_pool_release_owner(&pool, MEDIA(8)) == 2 * PAGE);
    CHECK(pool.block_count == 2);
    CHECK(pool.blocks[0].addr == a && pool.blocks[1].addr == d);
    CHECK(card_pool_alloc(&pool, 2 * PAGE, IMAGE(1)) == b);
    /* Owner 7's blocks were untouched. */
    CHECK(card_pool_free(&pool, a, MEDIA(7)) == 1);
    CHECK(card_pool_free(&pool, d, MEDIA(7)) == 1);
}

static void test_shared_media_owner_isolated_from_session_reclaim(void)
{
    uint32_t session_owner = MEDIA(1);
    uint32_t shared_owner =
        CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA_INPUT, 1);
    uint32_t session_block;
    uint32_t shared_block;

    two_ranges(4 * PAGE, PAGE);
    session_block = card_pool_alloc(&pool, PAGE, session_owner);
    shared_block = card_pool_alloc(&pool, PAGE, shared_owner);
    CHECK(session_block == A_BASE);
    CHECK(shared_block == A_BASE + PAGE);
    CHECK(session_owner != shared_owner);
    CHECK(card_pool_release_owner(&pool, session_owner) == PAGE);
    CHECK(pool.block_count == 1);
    CHECK(pool.blocks[0].addr == shared_block);
    CHECK(pool.blocks[0].owner == shared_owner);
    CHECK(card_pool_free(&pool, shared_block, shared_owner) == 1);
}

static void test_sdk_surface_reclaimed_on_reset(void)
{
    uint32_t firmware;
    uint32_t surface;

    two_ranges(4 * PAGE, PAGE);
    firmware = card_pool_alloc(&pool, PAGE, FIRMWARE(1));
    surface = card_pool_alloc(&pool, PAGE, SDK_SURFACE(1));
    CHECK(firmware == A_BASE);
    CHECK(surface == A_BASE + PAGE);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 1);
    CHECK(pool.blocks[0].addr == firmware);
    CHECK(pool.blocks[0].owner == FIRMWARE(1));
    CHECK(pool.class_used[CARD_POOL_CLASS_SDK_SURFACE] == 0U);
    CHECK(!card_pool_block_owned(&pool, surface, PAGE, SDK_SURFACE(1)));
}

static void test_console_reclaimed_on_reset(void)
{
    uint32_t console;

    two_ranges(4 * PAGE, PAGE);
    console = card_pool_alloc(&pool, PAGE, CONSOLE(1));
    CHECK(console == A_BASE);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 0);
    CHECK(pool.class_used[CARD_POOL_CLASS_CONSOLE] == 0U);
    CHECK(!card_pool_block_owned(&pool, console, PAGE, CONSOLE(1)));
}

static void test_decode_classes_reclaimed_on_reset(void)
{
    uint32_t image;
    uint32_t media;

    two_ranges(4 * PAGE, PAGE);
    image = card_pool_alloc(&pool, PAGE, IMAGE(1));
    media = card_pool_alloc(&pool, PAGE, MEDIA(1));
    CHECK(image == A_BASE);
    CHECK(media == A_BASE + PAGE);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 0U);
    CHECK(pool.class_used[CARD_POOL_CLASS_IMAGE] == 0U);
    CHECK(pool.class_used[CARD_POOL_CLASS_MEDIA] == 0U);
    CHECK(!card_pool_block_owned(&pool, image, PAGE, IMAGE(1)));
    CHECK(!card_pool_block_owned(&pool, media, PAGE, MEDIA(1)));
}

static void test_minigl_class(void)
{
    uint32_t ctx1, ctx2;

    two_ranges(4 * PAGE, PAGE);
    /* MiniGL render contexts allocate under their own class with the
     * context handle+generation as the owner instance. */
    ctx1 = card_pool_alloc(&pool, PAGE, MINIGL(0x00010001U));
    ctx2 = card_pool_alloc(&pool, PAGE, MINIGL(0x00010002U));
    CHECK(ctx1 == A_BASE);
    CHECK(ctx2 == A_BASE + PAGE);
    CHECK(pool.class_used[CARD_POOL_CLASS_MINIGL] == 2U * PAGE);
    /* Destroying one context releases only its own owner's pages. */
    CHECK(card_pool_release_owner(&pool, MINIGL(0x00010001U)) == PAGE);
    CHECK(card_pool_block_owned(&pool, ctx2, PAGE, MINIGL(0x00010002U)));
    CHECK(!card_pool_block_owned(&pool, ctx1, PAGE, MINIGL(0x00010001U)));
    /* An Amiga reset reclaims every live MiniGL context. */
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.class_used[CARD_POOL_CLASS_MINIGL] == 0U);
    CHECK(pool.block_count == 0);
    /* The class limit is the published MiniGL budget. */
    CHECK(pool.class_limit[CARD_POOL_CLASS_MINIGL] == CARD_POOL_LIMIT_MINIGL);
}

static void test_firmware_never_in_revocable(void)
{
    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, A_BASE, A_BASE + 2 * PAGE, 0) == 0);
    CHECK(card_pool_add_range(&pool, 0x20000000u, 0x20000000u + MB(16), 1) == 1);
    card_pool_open_range(&pool, 0);
    card_pool_open_range(&pool, 1);
    CHECK(card_pool_alloc(&pool, 2 * PAGE, FIRMWARE(1)) == A_BASE);
    CHECK(card_pool_alloc(&pool, PAGE, FIRMWARE(1)) == 0);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == 0x20000000u);
}

/* One permanent prefix plus the revocable Fast-RAM tail. */
#define B_BASE 0x20000000u
static void card_like_pool(void)
{
    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, A_BASE, A_BASE + 3 * PAGE, 0) == 0);
    CHECK(card_pool_add_range(&pool, B_BASE, B_BASE + MB(1), 1) == 1);
    card_pool_open_range(&pool, 0);
}

static void test_reset_fast_ram_switches_on(void)
{
    uint32_t fw;
    uint32_t media;
    uint32_t fw_next;
    uint32_t in_b;

    card_like_pool();
    CHECK(!card_pool_has_lent(&pool));
    card_pool_finish_amiga_reset(&pool, 1, 0);
    CHECK(card_pool_has_lent(&pool));
    fw = card_pool_alloc(&pool, PAGE, FIRMWARE(1));
    media = card_pool_alloc(&pool, PAGE, MEDIA(1));
    fw_next = card_pool_alloc(&pool, PAGE, FIRMWARE(2));
    in_b = card_pool_alloc(&pool, MB(1), IMAGE(2));
    CHECK(fw == A_BASE && media == A_BASE + PAGE &&
          fw_next == A_BASE + 2 * PAGE && in_b == B_BASE);

    card_pool_take_back_lent(&pool);
    CHECK(!card_pool_has_lent(&pool));
    CHECK(card_pool_range_free_bytes(&pool, 1) == 0);
    CHECK(pool.block_count == 3 && pool.blocks[2].addr == fw_next);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 2 &&
          pool.blocks[0].addr == fw && pool.blocks[0].owner == FIRMWARE(1) &&
          pool.blocks[1].addr == fw_next &&
          pool.blocks[1].owner == FIRMWARE(2));
    CHECK(pool.class_used[CARD_POOL_CLASS_MEDIA] == 0);
    CHECK(pool.class_used[CARD_POOL_CLASS_IMAGE] == 0);
    CHECK(pool.class_used[CARD_POOL_CLASS_FIRMWARE] == 2 * PAGE);
    CHECK(card_pool_range_free_bytes(&pool, 1) == 0);
    CHECK(card_pool_free(&pool, in_b, IMAGE(2)) == 0);
    CHECK(pool.ignored_frees == 1);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == A_BASE + PAGE);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == 0);
    CHECK(!card_pool_has_lent(&pool));
}

static void test_reset_relends_when_unused(void)
{
    card_like_pool();
    card_pool_finish_amiga_reset(&pool, 1, 0);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == A_BASE);
    CHECK(card_pool_alloc(&pool, 2 * PAGE, MEDIA(1)) == A_BASE + PAGE);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == B_BASE);

    CHECK(card_pool_has_lent(&pool));
    card_pool_take_back_lent(&pool);
    card_pool_finish_amiga_reset(&pool, 1, 0);
    CHECK(pool.block_count == 0);
    CHECK(card_pool_range_free_bytes(&pool, 1) == MB(1));

    card_pool_take_back_lent(&pool);
    card_pool_finish_amiga_reset(&pool, 0, 1);
    CHECK(card_pool_range_free_bytes(&pool, 1) == MB(1));
}

static void test_quarantine_blocks_every_release_path(void)
{
    uint32_t legacy;
    uint32_t canonical;

    card_like_pool();
    card_pool_finish_amiga_reset(&pool, 1, 0);
    legacy = card_pool_alloc(&pool, 3 * PAGE, MINIGL(7));
    canonical = card_pool_alloc(&pool, PAGE, MINIGL_CONTEXT(7));
    CHECK(legacy == A_BASE && canonical == B_BASE);
    card_pool_quarantine_owner(&pool, MINIGL(7));
    CHECK((pool.blocks[0].flags & CARD_POOL_BLOCK_QUARANTINED) != 0U &&
          (pool.blocks[1].flags & CARD_POOL_BLOCK_QUARANTINED) != 0U);
    CHECK(card_pool_free(&pool, legacy, MINIGL(7)) == 0);
    CHECK(card_pool_release_owner(&pool, MINIGL_CONTEXT(7)) == 0U);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 2U);
    card_pool_take_back_lent(&pool);
    CHECK(pool.block_count == 2U && !card_pool_has_lent(&pool));
    CHECK(!card_pool_can_lend_to_amiga(&pool));
}
static void test_quarantine_minigl_excludes_app(void)
{
    uint32_t app;
    uint32_t minigl;

    two_ranges(MB(1), MB(1));
    app = card_pool_alloc(&pool, PAGE, APP(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(1));
    CHECK(app != 0U && minigl != 0U);
    card_pool_quarantine_minigl(&pool);
    CHECK(card_pool_free(&pool, app, APP(1)) == 1);
    CHECK(card_pool_free(&pool, minigl, MINIGL(1)) == 0);
}
static void test_synchronous_reset_reclaims_before_new_allocation(void)
{
    uint32_t minigl;
    uint32_t app;
    uint32_t fresh;

    two_ranges(MB(1), MB(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(2));
    app = card_pool_alloc(&pool, PAGE, APP(2));
    CHECK(minigl != 0U && app != 0U);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 0U);
    fresh = card_pool_alloc(&pool, PAGE, APP(3));
    CHECK(fresh != 0U && card_pool_block_owned(&pool, fresh, PAGE, APP(3)));
}

static void test_unacknowledged_reset_quarantines_minigl_only(void)
{
    uint32_t minigl;
    uint32_t app;

    two_ranges(MB(1), MB(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(3));
    app = card_pool_alloc(&pool, PAGE, APP(4));
    CHECK(minigl != 0U && app != 0U);
    card_pool_quarantine_minigl(&pool);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(card_pool_block_owned(&pool, minigl, PAGE, MINIGL(3)));
    CHECK(!card_pool_block_owned(&pool, app, PAGE, APP(4)));
}

static void test_lock_left_by_faulted_core(void)
{
    card_like_pool();
    uint32_t a = card_pool_alloc(&pool, PAGE, MEDIA(5));
    /* Core 1 faults inside the pool and never releases the lock. With
     * nothing lent, the reset's take-back must not touch the lock (the
     * mock fails the run if it is taken while held). */
    current_cpu = 1;
    sdk_smp_lock_acquire(&pool.lock);
    current_cpu = 0;
    CHECK(!card_pool_has_lent(&pool));
    card_pool_take_back_lent(&pool);
    card_pool_reset_lock(&pool);
    CHECK(card_pool_release_owner(&pool, MEDIA(5)) == PAGE);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(6)) == a);
}

static void test_full_table(void)
{
    two_ranges(MB(64), MB(64));
    for (uint32_t i = 0; i < CARD_POOL_MAX_BLOCKS; i++)
        CHECK(card_pool_alloc(&pool, PAGE, FIRMWARE(1)) != 0);
    CHECK(pool.block_count == CARD_POOL_MAX_BLOCKS);
    CHECK(card_pool_range_free_bytes(&pool, 1) == MB(64));
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == 0);
    CHECK(card_pool_free(&pool, A_BASE, FIRMWARE(1)) == 1);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == A_BASE);
}

static void test_card_setup(void)
{
    uint32_t permanent;
    uint32_t revocable;

    card_pool_setup_card(&pool);
    permanent = card_pool_range_free_bytes(
        &pool, CARD_POOL_RANGE_PERMANENT);
    revocable = card_pool_range_free_bytes(
        &pool, CARD_POOL_RANGE_REVOCABLE);
    CHECK(permanent == CARD_POOL_PERMANENT_END -
          CARD_POOL_PERMANENT_ADDRESS);
    CHECK(revocable == 0);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) ==
          CARD_POOL_PERMANENT_ADDRESS);
    card_pool_open_range(&pool, CARD_POOL_RANGE_REVOCABLE);
    CHECK(card_pool_range_free_bytes(&pool, CARD_POOL_RANGE_REVOCABLE) ==
          CARD_POOL_REVOCABLE_END - CARD_POOL_REVOCABLE_ADDRESS);
}

/* Randomised allocate/free run: blocks stay sorted, non-overlapping, inside
 * open ranges, the per-class totals match the table, and freeing
 * everything returns the pool to empty. */
static void test_random(void)
{
    enum { LIVE = 300 };
    struct { uint32_t addr, owner; } live[LIVE];
    uint32_t nlive = 0;
    uint32_t seed = 12345u;
    two_ranges(MB(96), MB(96));
    for (int step = 0; step < 20000; step++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t r = seed >> 8;
        if (nlive < LIVE && (r & 1u)) {
            uint32_t owner = CARD_POOL_OWNER(1u + (r >> 1) % 3u, (r >> 3) & 7u);
            uint32_t size = 1u + (r >> 6) % (3u * PAGE);
            uint32_t a = card_pool_alloc(&pool, size, owner);
            if (a) {
                live[nlive].addr = a;
                live[nlive].owner = owner;
                nlive++;
            }
        } else if (nlive) {
            uint32_t k = (r >> 1) % nlive;
            CHECK(card_pool_free(&pool, live[k].addr, live[k].owner) == 1);
            live[k] = live[--nlive];
        }
        uint32_t used[CARD_POOL_CLASS_COUNT] = {0};
        for (uint32_t i = 0; i < pool.block_count; i++) {
            const card_pool_block_t *b = &pool.blocks[i];
            int inside = (b->addr >= A_BASE && b->addr + b->size <= A_BASE + MB(96)) ||
                         (b->addr >= C_BASE && b->addr + b->size <= C_BASE + MB(96));
            if (!inside || b->addr % PAGE ||
                (i && pool.blocks[i - 1].addr + pool.blocks[i - 1].size > b->addr)) {
                CHECK(0);
                return;
            }
            used[CARD_POOL_OWNER_CLASS(b->owner)] += b->size;
        }
        if (memcmp(used, pool.class_used, sizeof(used))) {
            CHECK(0);
            return;
        }
    }
    while (nlive) {
        nlive--;
        CHECK(card_pool_free(&pool, live[nlive].addr, live[nlive].owner) == 1);
    }
    CHECK(pool.block_count == 0);
    CHECK(card_pool_range_free_bytes(&pool, 0) == MB(96));
    CHECK(pool.ignored_frees == 0);
}

typedef struct {
    uint32_t address;
    uint32_t sequence;
    uint32_t now;
    uint32_t snapshots;
    uint32_t reads;
    uint32_t idle_reads;
    uint32_t sleeps;
    int complete_after;
    int idle_after;
    int snapshot_result;
    int forgot;
} fence_fixture_t;

static int fence_snapshot(void *context, uint32_t *address, uint32_t *sequence)
{
    fence_fixture_t *f = context;
    f->snapshots++;
    if (f->snapshot_result != 0)
        return f->snapshot_result;
    *address = f->address;
    *sequence = f->sequence;
    return 1;
}

static uint16_t fence_read(void *context, uint32_t address)
{
    fence_fixture_t *f = context;
    (void)address;
    f->reads++;
    return f->complete_after >= 0 && f->reads > (uint32_t)f->complete_after ?
        (uint16_t)f->sequence : (uint16_t)(f->sequence - 1U);
}
static int fence_idle(void *context)
{
    fence_fixture_t *f = context;

    f->idle_reads++;
    return f->idle_after >= 0 && f->idle_reads > (uint32_t)f->idle_after;
}


static uint32_t fence_clock(void *context)
{
    return ((fence_fixture_t *)context)->now;
}

static void fence_sleep(void *context, uint32_t milliseconds)
{
    fence_fixture_t *f = context;
    f->sleeps++;
    f->now += milliseconds;
}

static void fence_forget(void *context)
{
    ((fence_fixture_t *)context)->forgot = 1;
}

static const rasterix_fence_guard_hooks_t fence_hooks = {
    .snapshot = fence_snapshot,
    .read_fence = fence_read,
    .engine_idle = fence_idle,
    .clock_ms = fence_clock,
    .sleep_ms = fence_sleep,
    .forget = fence_forget,
};
static void fence_guard(fence_fixture_t *fixture, int rasterix_active)
{
    rasterix_fence_guard_hooks_t hooks = fence_hooks;

    hooks.context = fixture;
    rasterix_fence_guard_pool(&pool, rasterix_active, &hooks, 100U);
}


static void test_fence_guard_completed_reset_reclaims_normally(void)
{
    fence_fixture_t f = { .address = 0x12340000U, .sequence = 7U,
                          .complete_after = 2 };
    uint32_t minigl;
    uint32_t app;

    two_ranges(MB(1), MB(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(5));
    app = card_pool_alloc(&pool, PAGE, APP(5));
    fence_guard(&f, 1);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(f.forgot && f.sleeps == 2U);
    CHECK(!card_pool_block_owned(&pool, minigl, PAGE, MINIGL(5)));
    CHECK(!card_pool_block_owned(&pool, app, PAGE, APP(5)));
}

static void test_fence_guard_waits_for_idle_after_fence(void)
{
    fence_fixture_t f = { .address = 0x12340000U, .sequence = 11U,
                          .complete_after = 0, .idle_after = 2 };

    two_ranges(MB(1), MB(1));
    CHECK(card_pool_alloc(&pool, PAGE, MINIGL(9)) != 0U);
    fence_guard(&f, 1);
    CHECK(f.forgot && f.reads == 3U && f.idle_reads == 3U && f.sleeps == 2U);
}

static void test_fence_guard_unknown_snapshot_quarantines(void)
{
    fence_fixture_t f = { .snapshot_result = -1 };
    uint32_t minigl;

    two_ranges(MB(1), MB(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(10));
    fence_guard(&f, 1);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(f.snapshots == 1U && f.sleeps == 0U && !f.forgot);
    CHECK(card_pool_block_owned(&pool, minigl, PAGE, MINIGL(10)));
}

static void test_fence_guard_timeout_quarantines_minigl_only(void)
{
    fence_fixture_t f = { .address = 0x12340000U, .sequence = 8U,
                          .complete_after = -1 };
    uint32_t minigl;
    uint32_t app;

    two_ranges(MB(1), MB(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(6));
    app = card_pool_alloc(&pool, PAGE, APP(6));
    fence_guard(&f, 1);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(f.sleeps == 100U && !f.forgot);
    CHECK(card_pool_block_owned(&pool, minigl, PAGE, MINIGL(6)));
    CHECK(!card_pool_block_owned(&pool, app, PAGE, APP(6)));
}

static void test_fence_guard_disabled_core_and_no_platform(void)
{
    fence_fixture_t f = { .address = 0x12340000U, .sequence = 9U,
                          .complete_after = -1 };
    uint32_t minigl;

    two_ranges(MB(1), MB(1));
    minigl = card_pool_alloc(&pool, PAGE, MINIGL(7));
    fence_guard(&f, 1);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(card_pool_block_owned(&pool, minigl, PAGE, MINIGL(7)));

    f.now = f.snapshots = f.reads = f.sleeps = 0U;
    fence_guard(&f, 0);
    CHECK(f.snapshots == 0U && f.reads == 0U && f.sleeps == 0U);
}

static void test_fence_guard_healthy_reset_keeps_fastram_gate_ready(void)
{
    fence_fixture_t f = { .address = 0x12340000U, .sequence = 10U,
                          .complete_after = 0 };

    card_like_pool();
    CHECK(card_pool_alloc(&pool, PAGE, MINIGL(8)) == A_BASE);
    fence_guard(&f, 1);
    CHECK(f.sleeps == 0U && card_pool_can_lend_to_amiga(&pool));
    card_pool_finish_amiga_reset(&pool, 1, card_pool_can_lend_to_amiga(&pool));
    CHECK(!card_pool_has_lent(&pool));
}

int main(void)
{
    test_fills_ranges_in_order();
    test_aligned_allocation_skips_unaligned_range();
    test_block_ownership();
    test_range_owned_rejects_past_end();
    test_too_large_leaves_table();
    test_class_limit();
    test_wrong_owner_and_unknown_frees();
    test_release_owner_merges_gaps();
    test_shared_media_owner_isolated_from_session_reclaim();
    test_sdk_surface_reclaimed_on_reset();
    test_console_reclaimed_on_reset();
    test_minigl_class();
    test_firmware_never_in_revocable();
    test_decode_classes_reclaimed_on_reset();
    test_reset_fast_ram_switches_on();
    test_reset_relends_when_unused();
    test_quarantine_blocks_every_release_path();
    test_lock_left_by_faulted_core();
    test_quarantine_minigl_excludes_app();
    test_synchronous_reset_reclaims_before_new_allocation();
    test_unacknowledged_reset_quarantines_minigl_only();
    test_fence_guard_completed_reset_reclaims_normally();
    test_fence_guard_timeout_quarantines_minigl_only();
    test_fence_guard_waits_for_idle_after_fence();
    test_fence_guard_unknown_snapshot_quarantines();
    test_fence_guard_disabled_core_and_no_platform();
    test_fence_guard_healthy_reset_keeps_fastram_gate_ready();
    test_full_table();
    test_card_setup();
    test_random();
    printf("card_pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
