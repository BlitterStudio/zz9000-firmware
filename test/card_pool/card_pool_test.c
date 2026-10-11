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
void smp_raw_spin_unlock(volatile uint32_t *w) { *w = 0u; }

#define PAGE CARD_POOL_PAGE_BYTES
#define MB(n) ((uint32_t)(n) << 20)

#define MEDIA(i)    CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, (i))
#define IMAGE(i)    CARD_POOL_OWNER(CARD_POOL_CLASS_IMAGE, (i))
#define FIRMWARE(i) CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, (i))

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
    CHECK(card_pool_alloc(&pool, MB(64), FIRMWARE(1)) != 0);
    CHECK(card_pool_alloc(&pool, PAGE, FIRMWARE(2)) == 0);
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

/* Ranges in card order: A and C fixed, B revocable and tried last. */
#define B_BASE 0x20000000u
static void card_like_pool(void)
{
    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, A_BASE, A_BASE + PAGE, 0) == 0);
    CHECK(card_pool_add_range(&pool, C_BASE, C_BASE + 2 * PAGE, 0) == 1);
    CHECK(card_pool_add_range(&pool, B_BASE, B_BASE + MB(1), 1) == 2);
    card_pool_open_range(&pool, 0);
    card_pool_open_range(&pool, 1);
}

static void test_reset_fast_ram_switches_on(void)
{
    card_like_pool();
    /* Boot with fast RAM off lends B. */
    CHECK(!card_pool_has_lent(&pool));
    card_pool_finish_amiga_reset(&pool, 1, 0);
    CHECK(card_pool_has_lent(&pool));
    uint32_t fw = card_pool_alloc(&pool, PAGE, FIRMWARE(1));
    uint32_t in_c = card_pool_alloc(&pool, PAGE, MEDIA(1));
    uint32_t fw_c = card_pool_alloc(&pool, PAGE, FIRMWARE(2));
    uint32_t in_b = card_pool_alloc(&pool, MB(1), IMAGE(2));
    CHECK(fw == A_BASE && in_c == C_BASE && fw_c == C_BASE + PAGE &&
          in_b == B_BASE);

    /* Warm reset that enables fast RAM. Range C sits above B, so its
     * blocks follow B's in the table and must survive B's removal. */
    card_pool_take_back_lent(&pool);
    CHECK(!card_pool_has_lent(&pool));
    CHECK(card_pool_range_free_bytes(&pool, 2) == 0);
    CHECK(pool.block_count == 3 && pool.blocks[2].addr == fw_c);
    card_pool_finish_amiga_reset(&pool, 1, 1);
    CHECK(pool.block_count == 2 &&
          pool.blocks[0].addr == fw && pool.blocks[0].owner == FIRMWARE(1) &&
          pool.blocks[1].addr == fw_c && pool.blocks[1].owner == FIRMWARE(2));
    CHECK(pool.class_used[CARD_POOL_CLASS_MEDIA] == 0);
    CHECK(pool.class_used[CARD_POOL_CLASS_IMAGE] == 0);
    CHECK(pool.class_used[CARD_POOL_CLASS_FIRMWARE] == 2 * PAGE);
    CHECK(card_pool_range_free_bytes(&pool, 2) == 0);
    /* The revoked block's late free is ignored and B is never handed out. */
    CHECK(card_pool_free(&pool, in_b, IMAGE(2)) == 0);
    CHECK(pool.ignored_frees == 1);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == C_BASE);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == 0);
    CHECK(!card_pool_has_lent(&pool));
}

static void test_reset_relends_when_unused(void)
{
    card_like_pool();
    card_pool_finish_amiga_reset(&pool, 1, 0);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == A_BASE);
    CHECK(card_pool_alloc(&pool, 2 * PAGE, MEDIA(1)) == C_BASE);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == B_BASE);

    /* Fast RAM stays off: B comes back empty. */
    CHECK(card_pool_has_lent(&pool));
    card_pool_take_back_lent(&pool);
    card_pool_finish_amiga_reset(&pool, 1, 0);
    CHECK(pool.block_count == 0);
    CHECK(card_pool_range_free_bytes(&pool, 2) == MB(1));

    /* Zorro II has no fast-RAM window: B is lent whatever the config says. */
    card_pool_take_back_lent(&pool);
    card_pool_finish_amiga_reset(&pool, 0, 1);
    CHECK(card_pool_range_free_bytes(&pool, 2) == MB(1));
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
    card_pool_setup_card(&pool);
    uint32_t a1 = card_pool_range_free_bytes(&pool, CARD_POOL_RANGE_A1);
    uint32_t a2 = card_pool_range_free_bytes(&pool, CARD_POOL_RANGE_A2);
    uint32_t c = card_pool_range_free_bytes(&pool, CARD_POOL_RANGE_C);
    CHECK(a1 == MB(62) && a2 == MB(64) && c == MB(252));
    CHECK(a1 + a2 + c == MB(378));
    CHECK(card_pool_range_free_bytes(&pool, CARD_POOL_RANGE_B) == 0);
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == CARD_POOL_A1_ADDRESS);
    card_pool_open_range(&pool, CARD_POOL_RANGE_B);
    CHECK(card_pool_range_free_bytes(&pool, CARD_POOL_RANGE_B) == MB(256));
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

int main(void)
{
    test_fills_ranges_in_order();
    test_too_large_leaves_table();
    test_class_limit();
    test_wrong_owner_and_unknown_frees();
    test_release_owner_merges_gaps();
    test_firmware_never_in_revocable();
    test_reset_fast_ram_switches_on();
    test_reset_relends_when_unused();
    test_lock_left_by_faulted_core();
    test_full_table();
    test_card_setup();
    test_random();
    printf("card_pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
