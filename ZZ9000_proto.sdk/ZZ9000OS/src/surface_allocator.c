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

enum sa_owner {
    SA_OWNER_RESERVED,
    SA_OWNER_P96,
    SA_OWNER_FIRMWARE,
};

struct sa_block {
    uint32_t addr;
    uint32_t size;
    uint32_t refs;
    uint8_t owner;
};

static struct sa_block sa_blocks[SURFACE_ALLOCATOR_MAX_BLOCKS];
static sdk_smp_lock_t sa_lock = SDK_SMP_LOCK_INIT;
static uint32_t sa_count;
static uint32_t sa_base;
static uint32_t sa_end;
static uint32_t sa_used;

static uint32_t sa_round_size(uint32_t size)
{
    if (size == 0U || size > UINT32_MAX - (SURFACE_ALLOCATOR_ALIGNMENT - 1U))
        return 0U;
    return (size + SURFACE_ALLOCATOR_ALIGNMENT - 1U) &
           ~(SURFACE_ALLOCATOR_ALIGNMENT - 1U);
}

static uint32_t sa_align_up(uint32_t value, uint32_t alignment)
{
    if (value > UINT32_MAX - (alignment - 1U))
        return 0U;
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t sa_align_down(uint32_t value, uint32_t alignment)
{
    return value & ~(alignment - 1U);
}

/* Index of a block at exactly addr, or -1. Caller holds sa_lock. */
static int sa_find(uint32_t addr)
{
    uint32_t lo = 0U;
    uint32_t hi = sa_count;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2U;

        if (sa_blocks[mid].addr < addr)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo < sa_count && sa_blocks[lo].addr == addr ? (int)lo : -1;
}

static int sa_insert(uint32_t index, uint32_t address, uint32_t size,
                     enum sa_owner owner, uint32_t refs)
{
    if (sa_count >= SURFACE_ALLOCATOR_MAX_BLOCKS)
        return -1;
    memmove(&sa_blocks[index + 1U], &sa_blocks[index],
            (sa_count - index) * sizeof(sa_blocks[0]));
    sa_blocks[index].addr = address;
    sa_blocks[index].size = size;
    sa_blocks[index].owner = (uint8_t)owner;
    sa_blocks[index].refs = refs;
    sa_count++;
    sa_used += size;
    return 0;
}

static void sa_remove(uint32_t index)
{
    sa_used -= sa_blocks[index].size;
    memmove(&sa_blocks[index], &sa_blocks[index + 1U],
            (sa_count - index - 1U) * sizeof(sa_blocks[0]));
    sa_count--;
}

static int sa_gap_insert_index(uint32_t address, uint32_t size,
                               uint32_t *insert_at)
{
    uint32_t index = 0U;

    if (address < sa_base || address > sa_end || size > sa_end - address)
        return -1;
    while (index < sa_count && sa_blocks[index].addr < address)
        index++;
    if ((index != 0U &&
         sa_blocks[index - 1U].addr + sa_blocks[index - 1U].size > address) ||
        (index < sa_count && address + size > sa_blocks[index].addr))
        return -1;
    *insert_at = index;
    return 0;
}

void surface_allocator_init(uint32_t base, uint32_t size)
{
    sdk_smp_lock_acquire(&sa_lock);
    if (size > UINT32_MAX - base) {
        sa_base = 0U;
        sa_end = 0U;
    } else {
        sa_base = base;
        sa_end = base + size;
    }
    sa_count = 0U;
    sa_used = 0U;
    sdk_smp_lock_release(&sa_lock);
}

int surface_allocator_reserve(uint32_t address, uint32_t size)
{
    uint32_t rounded = sa_round_size(size);
    uint32_t insert_at;
    int result = -1;

    if (rounded == 0U || (address & (SURFACE_ALLOCATOR_ALIGNMENT - 1U)) != 0U)
        return -1;
    sdk_smp_lock_acquire(&sa_lock);
    if (sa_gap_insert_index(address, rounded, &insert_at) == 0)
        result = sa_insert(insert_at, address, rounded, SA_OWNER_RESERVED, 0U);
    sdk_smp_lock_release(&sa_lock);
    return result;
}

int surface_allocator_release_reservation(uint32_t address)
{
    int found;
    int result = -1;

    sdk_smp_lock_acquire(&sa_lock);
    found = sa_find(address);
    if (found >= 0 && sa_blocks[found].owner == SA_OWNER_RESERVED) {
        sa_remove((uint32_t)found);
        result = 0;
    }
    sdk_smp_lock_release(&sa_lock);
    return result;
}

uint32_t surface_allocator_p96_alloc(uint32_t size, uint32_t alignment)
{
    uint32_t rounded = sa_round_size(size);
    uint32_t candidate;
    uint32_t insert_at = sa_count;

    if (alignment == 0U)
        alignment = SURFACE_ALLOCATOR_ALIGNMENT;
    if (rounded == 0U || (alignment & (alignment - 1U)) != 0U)
        return 0U;
    sdk_smp_lock_acquire(&sa_lock);
    candidate = sa_align_up(sa_base, alignment);
    for (uint32_t i = 0U; i < sa_count; i++) {
        if (candidate >= sa_base && candidate <= sa_blocks[i].addr &&
            rounded <= sa_blocks[i].addr - candidate) {
            insert_at = i;
            break;
        }
        candidate = sa_align_up(sa_blocks[i].addr + sa_blocks[i].size,
                                alignment);
    }
    if (candidate < sa_base || candidate > sa_end ||
        rounded > sa_end - candidate ||
        sa_insert(insert_at, candidate, rounded, SA_OWNER_P96, 1U) != 0)
        candidate = 0U;
    sdk_smp_lock_release(&sa_lock);
    return candidate;
}

uint32_t surface_allocator_p96_alloc_abs(uint32_t address, uint32_t size)
{
    uint32_t rounded = sa_round_size(size);
    uint32_t insert_at;
    int found;
    uint32_t result = 0U;

    if (rounded == 0U || (address & (SURFACE_ALLOCATOR_ALIGNMENT - 1U)) != 0U)
        return 0U;
    sdk_smp_lock_acquire(&sa_lock);
    found = sa_find(address);
    if (found >= 0) {
        if (sa_blocks[found].owner == SA_OWNER_P96 &&
            sa_blocks[found].size >= rounded &&
            sa_blocks[found].refs != UINT32_MAX) {
            sa_blocks[found].refs++;
            result = address;
        }
    } else if (sa_gap_insert_index(address, rounded, &insert_at) == 0 &&
               sa_insert(insert_at, address, rounded, SA_OWNER_P96, 1U) == 0) {
        result = address;
    }
    sdk_smp_lock_release(&sa_lock);
    return result;
}

int surface_allocator_p96_free(uint32_t address)
{
    int found;
    int result = -1;

    sdk_smp_lock_acquire(&sa_lock);
    found = sa_find(address);
    if (found >= 0 && sa_blocks[found].owner == SA_OWNER_P96) {
        if (--sa_blocks[found].refs == 0U)
            sa_remove((uint32_t)found);
        result = 0;
    }
    sdk_smp_lock_release(&sa_lock);
    return result;
}

uint32_t surface_allocator_firmware_alloc_aligned(uint32_t size,
                                                  uint32_t alignment)
{
    uint32_t rounded = sa_round_size(size);
    uint32_t gap_end;
    uint32_t candidate = 0U;
    uint32_t insert_at = 0U;

    if (alignment == 0U)
        alignment = SURFACE_ALLOCATOR_ALIGNMENT;
    if (rounded == 0U || (alignment & (alignment - 1U)) != 0U)
        return 0U;
    sdk_smp_lock_acquire(&sa_lock);
    gap_end = sa_end;
    for (uint32_t i = sa_count; i > 0U; i--) {
        uint32_t gap_start = sa_blocks[i - 1U].addr +
                             sa_blocks[i - 1U].size;

        if (rounded <= gap_end - gap_start) {
            candidate = sa_align_down(gap_end - rounded, alignment);
            if (candidate >= gap_start) {
                insert_at = i;
                break;
            }
        }
        gap_end = sa_blocks[i - 1U].addr;
    }
    if (candidate == 0U && rounded <= gap_end - sa_base) {
        candidate = sa_align_down(gap_end - rounded, alignment);
        if (candidate >= sa_base)
            insert_at = 0U;
        else
            candidate = 0U;
    }
    if (candidate != 0U &&
        sa_insert(insert_at, candidate, rounded, SA_OWNER_FIRMWARE, 1U) != 0)
        candidate = 0U;
    sdk_smp_lock_release(&sa_lock);
    return candidate;
}

int surface_allocator_firmware_free(uint32_t address)
{
    int found;
    int result = -1;

    sdk_smp_lock_acquire(&sa_lock);
    found = sa_find(address);
    if (found >= 0 && sa_blocks[found].owner == SA_OWNER_FIRMWARE) {
        sa_remove((uint32_t)found);
        result = 0;
    }
    sdk_smp_lock_release(&sa_lock);
    return result;
}

uint32_t surface_allocator_block_size(uint32_t addr)
{
    int found;
    uint32_t size = 0U;

    sdk_smp_lock_acquire(&sa_lock);
    found = sa_find(addr);
    if (found >= 0)
        size = sa_blocks[found].size;
    sdk_smp_lock_release(&sa_lock);
    return size;
}

int surface_allocator_firmware_owns(uint32_t addr, uint32_t length)
{
    int found;
    int owns = 0;

    if (length == 0U)
        return 0;
    sdk_smp_lock_acquire(&sa_lock);
    found = sa_find(addr);
    if (found >= 0 && sa_blocks[found].owner == SA_OWNER_FIRMWARE)
        owns = length <= sa_blocks[found].size;
    sdk_smp_lock_release(&sa_lock);
    return owns;
}

uint32_t surface_allocator_used_bytes(void)
{
    uint32_t used;

    sdk_smp_lock_acquire(&sa_lock);
    used = sa_used;
    sdk_smp_lock_release(&sa_lock);
    return used;
}

uint32_t surface_allocator_total_bytes(void)
{
    uint32_t total;

    sdk_smp_lock_acquire(&sa_lock);
    total = sa_end - sa_base;
    sdk_smp_lock_release(&sa_lock);
    return total;
}

void surface_allocator_free_stats(uint32_t *free_total, uint32_t *largest_free)
{
    uint32_t candidate;
    uint32_t largest = 0U;

    if (!free_total || !largest_free)
        return;
    sdk_smp_lock_acquire(&sa_lock);
    candidate = sa_base;
    for (uint32_t i = 0U; i < sa_count; i++) {
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
