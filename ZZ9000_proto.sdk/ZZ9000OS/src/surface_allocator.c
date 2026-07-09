/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * RTG surface allocator: first-fit over a block table kept sorted by
 * address. Free space is implicit in the gaps between blocks, so a free
 * is just a table removal and no coalescing pass is ever needed.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include "surface_allocator.h"

struct sa_block {
    uint32_t addr;
    uint32_t size;
};

static struct sa_block sa_blocks[SURFACE_ALLOCATOR_MAX_BLOCKS];
static uint32_t sa_count;
static uint32_t sa_base;
static uint32_t sa_end;
static uint32_t sa_used;

void surface_allocator_init(uint32_t base, uint32_t size) {
    sa_base = base;
    sa_end = base + size;
    sa_count = 0;
    sa_used = 0;
}

/* Index of the block at exactly `addr`, or -1. */
static int sa_find(uint32_t addr) {
    uint32_t lo = 0, hi = sa_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (sa_blocks[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < sa_count && sa_blocks[lo].addr == addr)
        return (int)lo;
    return -1;
}

uint32_t surface_allocator_alloc(uint32_t size) {
    if (!size || sa_count >= SURFACE_ALLOCATOR_MAX_BLOCKS)
        return 0;

    uint32_t rounded = (size + SURFACE_ALLOCATOR_ALIGNMENT - 1) &
                       ~(SURFACE_ALLOCATOR_ALIGNMENT - 1);
    if (!rounded || rounded > sa_end - sa_base)
        return 0;

    /* First fit: try the gap before each block, then the tail gap. */
    uint32_t candidate = sa_base;
    uint32_t insert_at = sa_count;
    for (uint32_t i = 0; i < sa_count; i++) {
        if (sa_blocks[i].addr - candidate >= rounded) {
            insert_at = i;
            break;
        }
        candidate = sa_blocks[i].addr + sa_blocks[i].size;
    }
    if (insert_at == sa_count && sa_end - candidate < rounded)
        return 0;

    memmove(&sa_blocks[insert_at + 1], &sa_blocks[insert_at],
            (sa_count - insert_at) * sizeof(struct sa_block));
    sa_blocks[insert_at].addr = candidate;
    sa_blocks[insert_at].size = rounded;
    sa_count++;
    sa_used += rounded;
    return candidate;
}

int surface_allocator_free(uint32_t addr) {
    int i = sa_find(addr);
    if (i < 0)
        return -1;
    sa_used -= sa_blocks[i].size;
    memmove(&sa_blocks[i], &sa_blocks[i + 1],
            (sa_count - (uint32_t)i - 1) * sizeof(struct sa_block));
    sa_count--;
    return 0;
}

uint32_t surface_allocator_block_size(uint32_t addr) {
    int i = sa_find(addr);
    return (i < 0) ? 0 : sa_blocks[i].size;
}

uint32_t surface_allocator_used_bytes(void) {
    return sa_used;
}
