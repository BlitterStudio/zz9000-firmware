/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * Card pool allocator (card_pool.h). First fit over the ranges in the
 * order they were added, with one address-sorted table of live blocks
 * shared by every range, in the style of surface_allocator.c.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "card_pool.h"
#include "memorymap.h"

#if CARD_POOL_PAGE_SIZE != CARD_POOL_PAGE_BYTES
#error "card pool page size differs between memorymap.h and card_pool.h"
#endif

card_pool_t card_pool;

void card_pool_init(card_pool_t *pool)
{
    memset(pool, 0, sizeof(*pool));
    pool->lock = (sdk_smp_lock_t)SDK_SMP_LOCK_INIT;
    pool->class_limit[CARD_POOL_CLASS_FIRMWARE] = CARD_POOL_LIMIT_FIRMWARE;
    pool->class_limit[CARD_POOL_CLASS_MEDIA] = CARD_POOL_LIMIT_MEDIA;
    pool->class_limit[CARD_POOL_CLASS_IMAGE] = CARD_POOL_LIMIT_IMAGE;
}

void card_pool_setup_card(card_pool_t *pool)
{
    card_pool_init(pool);
    /* Indices follow the CARD_POOL_RANGE_* order, which is also the
     * allocation order: the revocable range is used last. */
    card_pool_add_range(pool, CARD_POOL_A1_ADDRESS, CARD_POOL_A1_END, 0);
    card_pool_add_range(pool, CARD_POOL_A2_ADDRESS, CARD_POOL_A2_END, 0);
    card_pool_add_range(pool, CARD_POOL_C_ADDRESS, CARD_POOL_C_END, 0);
    card_pool_add_range(pool, CARD_POOL_B_ADDRESS, CARD_POOL_B_END, 1);
    card_pool_open_range(pool, CARD_POOL_RANGE_A1);
    card_pool_open_range(pool, CARD_POOL_RANGE_A2);
    card_pool_open_range(pool, CARD_POOL_RANGE_C);
}

int card_pool_add_range(card_pool_t *pool, uint32_t base, uint32_t end,
                        int revocable)
{
    if (pool->range_count >= CARD_POOL_MAX_RANGES || base >= end ||
        ((base | end) & (CARD_POOL_PAGE_BYTES - 1u)))
        return -1;
    card_pool_range_t *r = &pool->ranges[pool->range_count];
    r->base = base;
    r->end = end;
    r->open = 0;
    r->revocable = revocable ? 1 : 0;
    return (int)pool->range_count++;
}

/* Index of the first block at or above `addr`. */
static uint32_t lower_bound(const card_pool_t *pool, uint32_t addr)
{
    uint32_t lo = 0, hi = pool->block_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (pool->blocks[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Drop blocks [first, last) and return their bytes. One memmove, so a
 * whole revoked range costs no more than a single free. */
static uint32_t remove_blocks(card_pool_t *pool, uint32_t first, uint32_t last)
{
    uint32_t bytes = 0;
    for (uint32_t i = first; i < last; i++) {
        const card_pool_block_t *b = &pool->blocks[i];
        pool->class_used[CARD_POOL_OWNER_CLASS(b->owner)] -= b->size;
        bytes += b->size;
    }
    memmove(&pool->blocks[first], &pool->blocks[last],
            (pool->block_count - last) * sizeof(pool->blocks[0]));
    pool->block_count -= last - first;
    return bytes;
}

void card_pool_open_range(card_pool_t *pool, uint32_t range)
{
    sdk_smp_lock_acquire(&pool->lock);
    if (range < pool->range_count)
        pool->ranges[range].open = 1;
    sdk_smp_lock_release(&pool->lock);
}

uint32_t card_pool_revoke_range(card_pool_t *pool, uint32_t range)
{
    uint32_t dropped = 0;
    sdk_smp_lock_acquire(&pool->lock);
    if (range < pool->range_count) {
        card_pool_range_t *r = &pool->ranges[range];
        dropped = remove_blocks(pool, lower_bound(pool, r->base),
                                lower_bound(pool, r->end));
        r->open = 0;
    }
    sdk_smp_lock_release(&pool->lock);
    return dropped;
}

/* First gap of `size` bytes in an open range, or 0. *insert_at receives
 * the table index the new block takes. */
static uint32_t find_gap(const card_pool_t *pool, const card_pool_range_t *r,
                         uint32_t size, uint32_t *insert_at)
{
    uint32_t i = lower_bound(pool, r->base);
    uint32_t candidate = r->base;
    for (; i < pool->block_count && pool->blocks[i].addr < r->end; i++) {
        if (pool->blocks[i].addr - candidate >= size)
            break;
        candidate = pool->blocks[i].addr + pool->blocks[i].size;
    }
    if (r->end - candidate < size)
        return 0;
    *insert_at = i;
    return candidate;
}

uint32_t card_pool_alloc(card_pool_t *pool, uint32_t size, uint32_t owner)
{
    uint32_t cls = CARD_POOL_OWNER_CLASS(owner);
    if (!size || size > UINT32_MAX - (CARD_POOL_PAGE_BYTES - 1u) ||
        cls == CARD_POOL_CLASS_NONE || cls >= CARD_POOL_CLASS_COUNT)
        return 0;
    uint32_t rounded = (size + CARD_POOL_PAGE_BYTES - 1u) &
                       ~(CARD_POOL_PAGE_BYTES - 1u);

    uint32_t addr = 0;
    sdk_smp_lock_acquire(&pool->lock);
    if (pool->block_count < CARD_POOL_MAX_BLOCKS &&
        rounded <= pool->class_limit[cls] - pool->class_used[cls]) {
        for (uint32_t r = 0; r < pool->range_count && !addr; r++) {
            const card_pool_range_t *range = &pool->ranges[r];
            uint32_t insert_at;
            if (!range->open ||
                (range->revocable && cls == CARD_POOL_CLASS_FIRMWARE))
                continue;
            addr = find_gap(pool, range, rounded, &insert_at);
            if (addr) {
                memmove(&pool->blocks[insert_at + 1], &pool->blocks[insert_at],
                        (pool->block_count - insert_at) * sizeof(pool->blocks[0]));
                pool->blocks[insert_at].addr = addr;
                pool->blocks[insert_at].size = rounded;
                pool->blocks[insert_at].owner = owner;
                pool->block_count++;
                pool->class_used[cls] += rounded;
            }
        }
    }
    sdk_smp_lock_release(&pool->lock);
    return addr;
}

int card_pool_free(card_pool_t *pool, uint32_t addr, uint32_t owner)
{
    int freed = 0;
    if (!addr)
        return 0;
    sdk_smp_lock_acquire(&pool->lock);
    uint32_t i = lower_bound(pool, addr);
    if (i < pool->block_count && pool->blocks[i].addr == addr &&
        pool->blocks[i].owner == owner) {
        remove_blocks(pool, i, i + 1);
        freed = 1;
    } else {
        pool->ignored_frees++;
    }
    sdk_smp_lock_release(&pool->lock);
    return freed;
}

/* Free every block owned by `key`, or with `except_class` set, every block
 * whose class is not `key`. One compaction pass keeps the table sorted. */
static uint32_t release_where(card_pool_t *pool, uint32_t key,
                              int except_class)
{
    uint32_t freed = 0, kept = 0;
    sdk_smp_lock_acquire(&pool->lock);
    for (uint32_t i = 0; i < pool->block_count; i++) {
        card_pool_block_t b = pool->blocks[i];
        uint32_t cls = CARD_POOL_OWNER_CLASS(b.owner);
        if (except_class ? cls != key : b.owner == key) {
            pool->class_used[cls] -= b.size;
            freed += b.size;
        } else {
            pool->blocks[kept++] = b;
        }
    }
    pool->block_count = kept;
    sdk_smp_lock_release(&pool->lock);
    return freed;
}

uint32_t card_pool_release_owner(card_pool_t *pool, uint32_t owner)
{
    return release_where(pool, owner, 0);
}

int card_pool_take_back_lent(card_pool_t *pool)
{
    int was_open = 0;
    for (uint32_t r = 0; r < pool->range_count; r++) {
        if (!pool->ranges[r].revocable)
            continue;
        was_open |= pool->ranges[r].open;
        card_pool_revoke_range(pool, r);
    }
    return was_open;
}

void card_pool_finish_amiga_reset(card_pool_t *pool, int is_zorro3,
                                  int fastram_advertised)
{
    release_where(pool, CARD_POOL_CLASS_FIRMWARE, 1);
    if (is_zorro3 && fastram_advertised)
        return;
    for (uint32_t r = 0; r < pool->range_count; r++) {
        if (pool->ranges[r].revocable)
            card_pool_open_range(pool, r);
    }
}

uint32_t card_pool_range_free_bytes(card_pool_t *pool, uint32_t range)
{
    uint32_t free_bytes = 0;
    sdk_smp_lock_acquire(&pool->lock);
    if (range < pool->range_count && pool->ranges[range].open) {
        const card_pool_range_t *r = &pool->ranges[range];
        free_bytes = r->end - r->base;
        for (uint32_t i = lower_bound(pool, r->base);
             i < pool->block_count && pool->blocks[i].addr < r->end; i++)
            free_bytes -= pool->blocks[i].size;
    }
    sdk_smp_lock_release(&pool->lock);
    return free_bytes;
}

void card_pool_reset_lock(card_pool_t *pool)
{
    sdk_smp_lock_reset(&pool->lock);
}
