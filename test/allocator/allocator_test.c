/*
 * Host unit tests for the RTG surface allocator (surface_allocator.c).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Build/run: make -C test/allocator test
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "surface_allocator.h"

/* Fake heap: addresses are never dereferenced by the allocator. */
#define HEAP_BASE 0x03500000u
#define HEAP_SIZE 0x00100000u /* 1 MB keeps exhaustion tests fast */
#define ALIGN     SURFACE_ALLOCATOR_ALIGNMENT

/* ---- tiny test harness --------------------------------------------- */

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---- tests ---------------------------------------------------------- */

static void test_basic_alloc(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t a = surface_allocator_alloc(100);
    uint32_t b = surface_allocator_alloc(256);
    uint32_t c = surface_allocator_alloc(257);
    CHECK(a == HEAP_BASE);
    CHECK(a % ALIGN == 0);
    CHECK(b == a + ALIGN);              /* 100 rounded up to 256 */
    CHECK(c == b + ALIGN);
    CHECK(surface_allocator_block_size(a) == ALIGN);
    CHECK(surface_allocator_block_size(b) == ALIGN);
    CHECK(surface_allocator_block_size(c) == 2 * ALIGN); /* 257 -> 512 */
    CHECK(surface_allocator_used_bytes() == 4 * ALIGN);
}

static void test_zero_size(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    CHECK(surface_allocator_alloc(0) == 0);
    CHECK(surface_allocator_used_bytes() == 0);
}

static void test_whole_heap(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t a = surface_allocator_alloc(HEAP_SIZE);
    CHECK(a == HEAP_BASE);
    CHECK(surface_allocator_block_size(a) == HEAP_SIZE);
    CHECK(surface_allocator_alloc(ALIGN) == 0); /* full */
    CHECK(surface_allocator_free(a) == 0);
    CHECK(surface_allocator_alloc(HEAP_SIZE + 1) == 0); /* one over */
    CHECK(surface_allocator_used_bytes() == 0);
    /* boundary block ends exactly at base+size */
    uint32_t b = surface_allocator_alloc(HEAP_SIZE - ALIGN);
    uint32_t c = surface_allocator_alloc(ALIGN);
    CHECK(b == HEAP_BASE);
    CHECK(c == HEAP_BASE + HEAP_SIZE - ALIGN);
    CHECK(surface_allocator_alloc(ALIGN) == 0);
}

static void test_free_realloc_reuse(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t a = surface_allocator_alloc(4096);
    uint32_t b = surface_allocator_alloc(4096);
    CHECK(a && b);
    CHECK(surface_allocator_free(a) == 0);
    uint32_t a2 = surface_allocator_alloc(4096);
    CHECK(a2 == a); /* first fit lands back in the hole */
    (void)b;
}

static void test_fragmentation(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t a = surface_allocator_alloc(0x10000);
    uint32_t b = surface_allocator_alloc(0x10000);
    uint32_t c = surface_allocator_alloc(0x10000);
    CHECK(a && b && c);
    CHECK(surface_allocator_free(b) == 0);
    /* smaller-than-hole alloc lands in the hole */
    uint32_t d = surface_allocator_alloc(0x8000);
    CHECK(d == b);
    /* bigger-than-hole alloc lands after c */
    uint32_t e = surface_allocator_alloc(0x20000);
    CHECK(e == c + 0x10000);
    /* total free suffices but no single gap does -> 0 (non-compacting) */
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t half = HEAP_SIZE / 2;
    uint32_t f = surface_allocator_alloc(half - ALIGN);
    uint32_t g = surface_allocator_alloc(2 * ALIGN);
    uint32_t h = surface_allocator_alloc(half - 3 * ALIGN);
    CHECK(f && g && h);
    CHECK(surface_allocator_free(f) == 0);
    CHECK(surface_allocator_free(h) == 0);
    /* free = whole heap minus the 2*ALIGN block in the middle, but split */
    CHECK(surface_allocator_alloc(HEAP_SIZE - 2 * ALIGN) == 0);
    CHECK(surface_allocator_alloc(half - ALIGN) != 0);
}

static void test_exhaustion_recovery(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t blocks[16];
    uint32_t chunk = HEAP_SIZE / 16;
    for (int i = 0; i < 16; i++) {
        blocks[i] = surface_allocator_alloc(chunk);
        CHECK(blocks[i] != 0);
    }
    CHECK(surface_allocator_alloc(ALIGN) == 0);
    CHECK(surface_allocator_free(blocks[7]) == 0);
    CHECK(surface_allocator_alloc(chunk) == blocks[7]);
}

static void test_double_free(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t a = surface_allocator_alloc(512);
    uint32_t b = surface_allocator_alloc(512);
    CHECK(a && b);
    uint32_t used = surface_allocator_used_bytes();
    CHECK(surface_allocator_free(a) == 0);
    CHECK(surface_allocator_free(a) == -1); /* double free */
    CHECK(surface_allocator_used_bytes() == used - 512);
    CHECK(surface_allocator_block_size(b) == 512);
}

static void test_free_unknown(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    uint32_t a = surface_allocator_alloc(4096);
    CHECK(a != 0);
    uint32_t used = surface_allocator_used_bytes();
    CHECK(surface_allocator_free(0) == -1);
    CHECK(surface_allocator_free(a + ALIGN) == -1);      /* mid-block */
    CHECK(surface_allocator_free(HEAP_BASE + HEAP_SIZE) == -1); /* out of range */
    CHECK(surface_allocator_free(a + 1) == -1);          /* misaligned */
    CHECK(surface_allocator_used_bytes() == used);
    CHECK(surface_allocator_block_size(a) == 4096);
}

static void test_slot_exhaustion(void) {
    /* heap big enough for more blocks than the table can track */
    surface_allocator_init(HEAP_BASE, (SURFACE_ALLOCATOR_MAX_BLOCKS + 8) * ALIGN);
    uint32_t first = 0, last = 0;
    for (uint32_t i = 0; i < SURFACE_ALLOCATOR_MAX_BLOCKS; i++) {
        uint32_t a = surface_allocator_alloc(ALIGN);
        CHECK(a != 0);
        if (i == 0) first = a;
        last = a;
    }
    CHECK(surface_allocator_alloc(ALIGN) == 0); /* table full, space left */
    CHECK(surface_allocator_free(first) == 0);
    CHECK(surface_allocator_alloc(ALIGN) == first); /* slot recovered */
    (void)last;
}

static void test_reset(void) {
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    CHECK(surface_allocator_alloc(1024) != 0);
    CHECK(surface_allocator_alloc(1024) != 0);
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    CHECK(surface_allocator_used_bytes() == 0);
    CHECK(surface_allocator_alloc(1024) == HEAP_BASE);
}

/* ---- randomized churn against a naive occupancy-bitmap oracle ------- */

#define UNITS (HEAP_SIZE / ALIGN)

static uint8_t  oracle_used[UNITS];       /* 256-byte-granularity occupancy */
static uint32_t oracle_size[UNITS];       /* rounded size, indexed by start unit */
static uint32_t oracle_used_bytes;

static int oracle_alloc_check(uint32_t addr, uint32_t rounded) {
    if (addr < HEAP_BASE || addr % ALIGN ||
        addr + rounded > HEAP_BASE + HEAP_SIZE)
        return 0;
    uint32_t u = (addr - HEAP_BASE) / ALIGN;
    for (uint32_t i = 0; i < rounded / ALIGN; i++)
        if (oracle_used[u + i])
            return 0; /* overlaps a live block */
    for (uint32_t i = 0; i < rounded / ALIGN; i++)
        oracle_used[u + i] = 1;
    oracle_size[u] = rounded;
    oracle_used_bytes += rounded;
    return 1;
}

static int oracle_free(uint32_t addr) {
    if (addr < HEAP_BASE || addr % ALIGN || addr >= HEAP_BASE + HEAP_SIZE)
        return -1;
    uint32_t u = (addr - HEAP_BASE) / ALIGN;
    if (!oracle_size[u])
        return -1; /* not a live block start */
    for (uint32_t i = 0; i < oracle_size[u] / ALIGN; i++)
        oracle_used[u + i] = 0;
    oracle_used_bytes -= oracle_size[u];
    oracle_size[u] = 0;
    return 0;
}

static uint32_t rng_state;
static uint32_t rng(void) { /* xorshift32 */
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void test_churn(void) {
    const uint32_t seed = 0xC0FFEE42u;
    rng_state = seed;
    surface_allocator_init(HEAP_BASE, HEAP_SIZE);
    memset(oracle_used, 0, sizeof oracle_used);
    memset(oracle_size, 0, sizeof oracle_size);
    oracle_used_bytes = 0;

    uint32_t live[512];
    uint32_t nlive = 0;
    int bad = 0;

    for (int op = 0; op < 100000 && !bad; op++) {
        if ((rng() & 3) != 0 || nlive == 0) { /* 75% alloc */
            /* biased small; occasionally huge */
            uint32_t size = (rng() % 16 == 0) ? (rng() % HEAP_SIZE)
                                              : (rng() % 8192) + 1;
            uint32_t rounded = (size + ALIGN - 1) & ~(uint32_t)(ALIGN - 1);
            uint32_t a = surface_allocator_alloc(size);
            if (a) {
                if (!oracle_alloc_check(a, rounded)) {
                    printf("FAIL churn(seed=%08x) op=%d: bad alloc %08x size %u\n",
                           seed, op, a, size);
                    bad = 1;
                } else if (nlive < 512) {
                    live[nlive++] = a;
                } else {
                    /* keep the working set bounded: forget by freeing */
                    uint32_t victim = rng() % nlive;
                    if (surface_allocator_free(live[victim]) != 0 ||
                        oracle_free(live[victim]) != 0) {
                        printf("FAIL churn(seed=%08x) op=%d: evict free\n", seed, op);
                        bad = 1;
                    }
                    live[victim] = a;
                }
            }
        } else { /* 25% free, half valid / half bogus */
            if (rng() & 1) {
                uint32_t victim = rng() % nlive;
                int r = surface_allocator_free(live[victim]);
                int o = oracle_free(live[victim]);
                if (r != 0 || o != 0) {
                    printf("FAIL churn(seed=%08x) op=%d: valid free r=%d o=%d\n",
                           seed, op, r, o);
                    bad = 1;
                }
                live[victim] = live[--nlive];
            } else {
                uint32_t addr = HEAP_BASE + (rng() % HEAP_SIZE);
                int r = surface_allocator_free(addr);
                int o = oracle_free(addr);
                if (r != o) {
                    printf("FAIL churn(seed=%08x) op=%d: free %08x r=%d o=%d\n",
                           seed, op, addr, r, o);
                    bad = 1;
                }
                if (o == 0) { /* randomly hit a live block: drop it */
                    for (uint32_t i = 0; i < nlive; i++)
                        if (live[i] == addr) { live[i] = live[--nlive]; break; }
                }
            }
        }
        if (surface_allocator_used_bytes() != oracle_used_bytes) {
            printf("FAIL churn(seed=%08x) op=%d: used %u != oracle %u\n",
                   seed, op, surface_allocator_used_bytes(), oracle_used_bytes);
            bad = 1;
        }
    }
    CHECK(!bad);
}

/* ---------------------------------------------------------------------- */

int main(void) {
    test_basic_alloc();
    test_zero_size();
    test_whole_heap();
    test_free_realloc_reuse();
    test_fragmentation();
    test_exhaustion_recovery();
    test_double_free();
    test_free_unknown();
    test_slot_exhaustion();
    test_reset();
    test_churn();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
