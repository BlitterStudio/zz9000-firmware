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

/* Single-threaded host build: the SMP lock primitives are no-ops. */
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0u; }
void smp_local_irq_restore(uint32_t s) { (void)s; }
void smp_raw_spin_lock(volatile uint32_t *w) { *w = 1u; }
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

static void test_revoke_and_reset_release(void)
{
    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, A_BASE, A_BASE + PAGE, 0) == 0);
    CHECK(card_pool_add_range(&pool, 0x20000000u, 0x20000000u + MB(1), 1) == 1);
    card_pool_open_range(&pool, 0);
    card_pool_open_range(&pool, 1);
    uint32_t fw = card_pool_alloc(&pool, PAGE, FIRMWARE(1));
    uint32_t lent = card_pool_alloc(&pool, MB(1), MEDIA(1));
    CHECK(fw == A_BASE && lent == 0x20000000u);
    CHECK(card_pool_revoke_range(&pool, 1) == MB(1));
    CHECK(pool.class_used[CARD_POOL_CLASS_MEDIA] == 0);
    CHECK(card_pool_range_free_bytes(&pool, 1) == 0);
    /* Closed: nothing lands there, and the old owner's free is ignored. */
    CHECK(card_pool_alloc(&pool, PAGE, MEDIA(1)) == 0);
    CHECK(card_pool_free(&pool, lent, MEDIA(1)) == 0);
    CHECK(pool.ignored_frees == 1);
    card_pool_open_range(&pool, 1);
    CHECK(card_pool_range_free_bytes(&pool, 1) == MB(1));
    CHECK(card_pool_alloc(&pool, PAGE, IMAGE(3)) == 0x20000000u);
    CHECK(card_pool_release_all_except(&pool, CARD_POOL_CLASS_FIRMWARE) == PAGE);
    CHECK(pool.block_count == 1 && pool.blocks[0].addr == fw);
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
    test_revoke_and_reset_release();
    test_full_table();
    test_card_setup();
    test_random();
    printf("card_pool: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
