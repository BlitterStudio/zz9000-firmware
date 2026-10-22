/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * Host-visible allocator: first-fit over a block table kept sorted by
 * address. Free space is implicit in the gaps between blocks, so a free
 * is just a table removal and no coalescing pass is ever needed.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include "sdk_smp_lock.h"
#include "surface_allocator.h"

struct sa_block {
    uint32_t addr;
    uint32_t size;
};

static struct sa_block sa_blocks[SURFACE_ALLOCATOR_MAX_BLOCKS];
static sdk_smp_lock_t sa_lock = SDK_SMP_LOCK_INIT;
static uint32_t sa_count;
static uint32_t sa_base;
static uint32_t sa_end;
static uint32_t sa_used;

void surface_allocator_init(uint32_t base, uint32_t size) {
    sdk_smp_lock_acquire(&sa_lock);
    if (size > UINT32_MAX - base) {
        sa_base = 0;
        sa_end = 0;
    } else {
        sa_base = base;
        sa_end = base + size;
    }
    sa_count = 0;
    sa_used = 0;
    sdk_smp_lock_release(&sa_lock);
}

/* Index of the block at exactly `addr`, or -1. Caller holds sa_lock. */
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

static uint32_t sa_align_up(uint32_t value, uint32_t alignment) {
    if (value > UINT32_MAX - (alignment - 1u))
        return 0;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

int surface_allocator_reserve(uint32_t address, uint32_t size) {
    uint32_t rounded;
    uint32_t insert_at = 0;

    if (!size || (address & (SURFACE_ALLOCATOR_ALIGNMENT - 1u)) != 0u ||
        size > UINT32_MAX - (SURFACE_ALLOCATOR_ALIGNMENT - 1u))
        return -1;
    rounded = (size + SURFACE_ALLOCATOR_ALIGNMENT - 1u) &
              ~(SURFACE_ALLOCATOR_ALIGNMENT - 1u);
    sdk_smp_lock_acquire(&sa_lock);
    if (sa_count >= SURFACE_ALLOCATOR_MAX_BLOCKS || address < sa_base ||
        address > sa_end || rounded > sa_end - address) {
        sdk_smp_lock_release(&sa_lock);
        return -1;
    }
    while (insert_at < sa_count && sa_blocks[insert_at].addr < address)
        insert_at++;
    if ((insert_at != 0u &&
         sa_blocks[insert_at - 1u].addr +
             sa_blocks[insert_at - 1u].size > address) ||
        (insert_at < sa_count &&
         address + rounded > sa_blocks[insert_at].addr)) {
        sdk_smp_lock_release(&sa_lock);
        return -1;
    }
    memmove(&sa_blocks[insert_at + 1u], &sa_blocks[insert_at],
            (sa_count - insert_at) * sizeof(struct sa_block));
    sa_blocks[insert_at].addr = address;
    sa_blocks[insert_at].size = rounded;
    sa_count++;
    sa_used += rounded;
    sdk_smp_lock_release(&sa_lock);
    return 0;
}

uint32_t surface_allocator_alloc_aligned(uint32_t size, uint32_t alignment) {
    uint32_t rounded;
    uint32_t candidate;
    uint32_t insert_at;

    if (!size || !alignment || (alignment & (alignment - 1u)) ||
        size > UINT32_MAX - (SURFACE_ALLOCATOR_ALIGNMENT - 1u))
        return 0;
    rounded = (size + SURFACE_ALLOCATOR_ALIGNMENT - 1u) &
              ~(SURFACE_ALLOCATOR_ALIGNMENT - 1u);

    sdk_smp_lock_acquire(&sa_lock);
    if (sa_count >= SURFACE_ALLOCATOR_MAX_BLOCKS ||
        rounded > sa_end - sa_base) {
        sdk_smp_lock_release(&sa_lock);
        return 0;
    }

    /* First fit: try the gap before each block, then the tail gap. */
    candidate = sa_align_up(sa_base, alignment);
    insert_at = sa_count;
    for (uint32_t i = 0; i < sa_count; i++) {
        if (sa_blocks[i].addr - candidate >= rounded) {
            insert_at = i;
            break;
        }
        candidate = sa_align_up(sa_blocks[i].addr + sa_blocks[i].size,
                                alignment);
    }
    if (candidate < sa_base || candidate > sa_end ||
        rounded > sa_end - candidate) {
        sdk_smp_lock_release(&sa_lock);
        return 0;
    }

    memmove(&sa_blocks[insert_at + 1], &sa_blocks[insert_at],
            (sa_count - insert_at) * sizeof(struct sa_block));
    sa_blocks[insert_at].addr = candidate;
    sa_blocks[insert_at].size = rounded;
    sa_count++;
    sa_used += rounded;
    sdk_smp_lock_release(&sa_lock);
    return candidate;
}

uint32_t surface_allocator_alloc(uint32_t size) {
    return surface_allocator_alloc_aligned(size, SURFACE_ALLOCATOR_ALIGNMENT);
}

int surface_allocator_free(uint32_t addr) {
    int i;

    sdk_smp_lock_acquire(&sa_lock);
    i = sa_find(addr);
    if (i < 0) {
        sdk_smp_lock_release(&sa_lock);
        return -1;
    }
    sa_used -= sa_blocks[i].size;
    memmove(&sa_blocks[i], &sa_blocks[i + 1],
            (sa_count - (uint32_t)i - 1) * sizeof(struct sa_block));
    sa_count--;
    sdk_smp_lock_release(&sa_lock);
    return 0;
}

uint32_t surface_allocator_block_size(uint32_t addr) {
    int i;
    uint32_t size;

    sdk_smp_lock_acquire(&sa_lock);
    i = sa_find(addr);
    size = (i < 0) ? 0 : sa_blocks[i].size;
    sdk_smp_lock_release(&sa_lock);
    return size;
}

int surface_allocator_owns(uint32_t addr, uint32_t length) {
    int i;
    int owns;

    if (!length)
        return 0;
    sdk_smp_lock_acquire(&sa_lock);
    i = sa_find(addr);
    owns = i >= 0 && length <= sa_blocks[i].size;
    sdk_smp_lock_release(&sa_lock);
    return owns;
}

uint32_t surface_allocator_used_bytes(void) {
    uint32_t used;

    sdk_smp_lock_acquire(&sa_lock);
    used = sa_used;
    sdk_smp_lock_release(&sa_lock);
    return used;
}

uint32_t surface_allocator_total_bytes(void) {
    uint32_t total;

    sdk_smp_lock_acquire(&sa_lock);
    total = sa_end - sa_base;
    sdk_smp_lock_release(&sa_lock);
    return total;
}

void surface_allocator_free_stats(uint32_t *free_total,
                                  uint32_t *largest_free) {
    uint32_t candidate;
    uint32_t largest = 0;

    if (!free_total || !largest_free)
        return;
    sdk_smp_lock_acquire(&sa_lock);
    candidate = sa_base;
    for (uint32_t i = 0; i < sa_count; i++) {
        uint32_t gap = sa_blocks[i].addr - candidate;

        if (gap > largest)
            largest = gap;
        candidate = sa_blocks[i].addr + sa_blocks[i].size;
    }
    if (sa_end - candidate > largest)
        largest = sa_end - candidate;
    *free_total = (sa_end - sa_base) - sa_used;
    *largest_free = largest;
    sdk_smp_lock_release(&sa_lock);
}
