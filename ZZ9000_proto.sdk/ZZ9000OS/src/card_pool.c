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

#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "memorymap.h"

#if CARD_POOL_PAGE_SIZE != CARD_POOL_PAGE_BYTES
#error "card pool page size differs between memorymap.h and card_pool.h"
#endif

#if CARD_POOL_PERMANENT_ADDRESS != FIRMWARE_MODULE_ARENA_END
#error "module arena must reserve the first permanent card-pool pages"
#endif

card_pool_t card_pool;

void card_pool_init(card_pool_t *pool)
{
    memset(pool, 0, sizeof(*pool));
    pool->lock = (sdk_smp_lock_t)SDK_SMP_LOCK_INIT;
    pool->class_limit[CARD_POOL_CLASS_FIRMWARE] = CARD_POOL_LIMIT_FIRMWARE;
    pool->class_limit[CARD_POOL_CLASS_MEDIA] = CARD_POOL_LIMIT_MEDIA;
    pool->class_limit[CARD_POOL_CLASS_MEDIA_INPUT] =
        CARD_POOL_LIMIT_MEDIA_INPUT;
    pool->class_limit[CARD_POOL_CLASS_SDK_SURFACE] =
        CARD_POOL_LIMIT_SDK_SURFACE;
    pool->class_limit[CARD_POOL_CLASS_CONSOLE] = CARD_POOL_LIMIT_CONSOLE;
    pool->class_limit[CARD_POOL_CLASS_MINIGL] = CARD_POOL_LIMIT_MINIGL;
    pool->class_limit[CARD_POOL_CLASS_IMAGE] = CARD_POOL_LIMIT_IMAGE;
    pool->class_limit[CARD_POOL_CLASS_APP] = CARD_POOL_LIMIT_APP;
    pool->class_limit[CARD_POOL_CLASS_APP_DISPLAY] =
        CARD_POOL_LIMIT_APP_DISPLAY;
    pool->class_limit[CARD_POOL_CLASS_APP_FRAMES] =
        CARD_POOL_LIMIT_APP_FRAMES;
    pool->class_limit[CARD_POOL_CLASS_APP_ARENA] =
        CARD_POOL_LIMIT_APP_ARENA;
    pool->class_limit[CARD_POOL_CLASS_MINIGL_MODULE] =
        CARD_POOL_LIMIT_MINIGL_MODULE;
    pool->class_limit[CARD_POOL_CLASS_MINIGL_TRANSPORT] =
        CARD_POOL_LIMIT_MINIGL_TRANSPORT;
    pool->class_limit[CARD_POOL_CLASS_MINIGL_CONTEXT] =
        CARD_POOL_LIMIT_MINIGL_CONTEXT;
    pool->class_limit[CARD_POOL_CLASS_MINIGL_APP_CONTEXT] =
        CARD_POOL_LIMIT_MINIGL_APP_CONTEXT;
    pool->class_limit[CARD_POOL_CLASS_MINIGL_RIX] =
        CARD_POOL_LIMIT_MINIGL_RIX;
}

void card_pool_setup_card(card_pool_t *pool)
{
    card_pool_init(pool);
    /* The permanent prefix is always card-owned. The contiguous tail is
     * revocable because the Amiga owns it whenever Fast RAM is advertised. */
    card_pool_add_range(pool, CARD_POOL_PERMANENT_ADDRESS,
                        CARD_POOL_PERMANENT_END, 0);
    card_pool_add_range(pool, CARD_POOL_REVOCABLE_ADDRESS,
                        CARD_POOL_REVOCABLE_END, 1);
    card_pool_open_range(pool, CARD_POOL_RANGE_PERMANENT);
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

/* Drop non-quarantined blocks [first, last) and return their bytes. A
 * quarantined block permanently occupies its range, including a revocable
 * one which must never be handed to the Amiga. */
static uint32_t remove_blocks(card_pool_t *pool, uint32_t first, uint32_t last)
{
    uint32_t bytes = 0;
    uint32_t kept = first;

    for (uint32_t i = first; i < last; i++) {
        card_pool_block_t b = pool->blocks[i];

        if (b.flags & CARD_POOL_BLOCK_QUARANTINED) {
            pool->blocks[kept++] = b;
            continue;
        }
        pool->class_used[CARD_POOL_OWNER_CLASS(b.owner)] -= b.size;
        bytes += b.size;
    }
    memmove(&pool->blocks[kept], &pool->blocks[last],
            (pool->block_count - last) * sizeof(pool->blocks[0]));
    pool->block_count -= last - kept;
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

/* First aligned gap of `size` bytes in an open range, or 0. *insert_at
 * receives the table index the new block takes. */
static uint32_t find_gap(const card_pool_t *pool, const card_pool_range_t *r,
                         uint32_t size, uint32_t alignment,
                         uint32_t *insert_at)
{
    uint32_t i = lower_bound(pool, r->base);
    uint32_t candidate = r->base;
    uint32_t mask = alignment - 1u;

    for (; i < pool->block_count && pool->blocks[i].addr < r->end; i++) {
        uint32_t aligned = (candidate + mask) & ~mask;

        if (aligned >= candidate && aligned <= pool->blocks[i].addr &&
            pool->blocks[i].addr - aligned >= size) {
            *insert_at = i;
            return aligned;
        }
        candidate = pool->blocks[i].addr + pool->blocks[i].size;
    }
    {
        uint32_t aligned = (candidate + mask) & ~mask;

        if (aligned >= candidate && aligned <= r->end &&
            r->end - aligned >= size) {
            *insert_at = i;
            return aligned;
        }
    }
    return 0;
}

/* Classes that live in the permanent range only: never in the revocable
 * Fast-RAM tail, and kept across an Amiga reset. */
static int permanent_class(uint32_t cls)
{
    return cls == CARD_POOL_CLASS_FIRMWARE || cls == CARD_POOL_CLASS_APP_ARENA;
}

static int range_allows(const card_pool_range_t *range, uint32_t cls)
{
    return range->open &&
           !(range->revocable &&
             (permanent_class(cls) || cls == CARD_POOL_CLASS_APP ||
              cls == CARD_POOL_CLASS_APP_FRAMES));
}

uint32_t card_pool_alloc_aligned(card_pool_t *pool, uint32_t size,
                                 uint32_t alignment, uint32_t owner)
{
    uint32_t cls = CARD_POOL_OWNER_CLASS(owner);
    uint32_t rounded;
    uint32_t addr = 0;

    if (!size || size > UINT32_MAX - (CARD_POOL_PAGE_BYTES - 1u) ||
        alignment < CARD_POOL_PAGE_BYTES ||
        (alignment & (alignment - 1u)) ||
        (alignment & (CARD_POOL_PAGE_BYTES - 1u)) ||
        cls == CARD_POOL_CLASS_NONE || cls >= CARD_POOL_CLASS_COUNT)
        return 0;
    rounded = (size + CARD_POOL_PAGE_BYTES - 1u) &
              ~(CARD_POOL_PAGE_BYTES - 1u);

    sdk_smp_lock_acquire(&pool->lock);
    if (pool->block_count < CARD_POOL_MAX_BLOCKS &&
        rounded <= pool->class_limit[cls] - pool->class_used[cls]) {
        for (uint32_t r = 0; r < pool->range_count && !addr; r++) {
            const card_pool_range_t *range = &pool->ranges[r];
            uint32_t insert_at;

            if (!range_allows(range, cls))
                continue;
            addr = find_gap(pool, range, rounded, alignment, &insert_at);
            if (addr) {
                memmove(&pool->blocks[insert_at + 1], &pool->blocks[insert_at],
                        (pool->block_count - insert_at) *
                        sizeof(pool->blocks[0]));
                pool->blocks[insert_at].addr = addr;
                pool->blocks[insert_at].size = rounded;
                pool->blocks[insert_at].owner = owner;
                pool->blocks[insert_at].flags = 0U;
                pool->block_count++;
                pool->class_used[cls] += rounded;
            }
        }
    }
    sdk_smp_lock_release(&pool->lock);
    return addr;
}

uint32_t card_pool_alloc(card_pool_t *pool, uint32_t size, uint32_t owner)
{
    return card_pool_alloc_aligned(pool, size, CARD_POOL_PAGE_BYTES, owner);
}

int card_pool_block_owned(card_pool_t *pool, uint32_t addr, uint32_t length,
                          uint32_t owner)
{
    uint32_t i;
    int owned = 0;

    if (!pool || !addr || !length || !owner)
        return 0;
    sdk_smp_lock_acquire(&pool->lock);
    i = lower_bound(pool, addr);
    if (i < pool->block_count && pool->blocks[i].addr == addr &&
        pool->blocks[i].owner == owner &&
        length <= pool->blocks[i].size)
        owned = 1;
    sdk_smp_lock_release(&pool->lock);
    return owned;
}

int card_pool_range_owned(card_pool_t *pool, uint32_t addr, uint32_t length,
                          uint32_t owner)
{
    uint32_t i;
    int owned = 0;

    if (!pool || !addr || !length || !owner)
        return 0;
    sdk_smp_lock_acquire(&pool->lock);
    i = lower_bound(pool, addr);
    if (i != 0U && (i == pool->block_count ||
                    pool->blocks[i].addr > addr))
        --i;
    if (i < pool->block_count && pool->blocks[i].owner == owner &&
        addr >= pool->blocks[i].addr) {
        uint32_t offset = addr - pool->blocks[i].addr;

        /* Subtract only after the address is inside the block. An address
         * past the end used to wrap the unsigned difference and pass. */
        if (offset < pool->blocks[i].size &&
            length <= pool->blocks[i].size - offset)
            owned = 1;
    }
    sdk_smp_lock_release(&pool->lock);
    return owned;
}

int card_pool_free(card_pool_t *pool, uint32_t addr, uint32_t owner)
{
    int freed = 0;
    if (!addr)
        return 0;
    sdk_smp_lock_acquire(&pool->lock);
    uint32_t i = lower_bound(pool, addr);
    if (i < pool->block_count && pool->blocks[i].addr == addr &&
        pool->blocks[i].owner == owner &&
        !(pool->blocks[i].flags & CARD_POOL_BLOCK_QUARANTINED)) {
        remove_blocks(pool, i, i + 1);
        freed = 1;
    } else {
        pool->ignored_frees++;
    }
    sdk_smp_lock_release(&pool->lock);
    return freed;
}

/* Free every non-quarantined block owned by `owner`, or with
 * `all_but_permanent` set, every non-quarantined block whose class is not a
 * permanent one (FIRMWARE, APP_ARENA). One compaction pass keeps the table
 * sorted. */
static uint32_t release_where(card_pool_t *pool, uint32_t owner,
                              int all_but_permanent)
{
    uint32_t freed = 0, kept = 0;
    sdk_smp_lock_acquire(&pool->lock);
    for (uint32_t i = 0; i < pool->block_count; i++) {
        card_pool_block_t b = pool->blocks[i];
        uint32_t cls = CARD_POOL_OWNER_CLASS(b.owner);

        if ((all_but_permanent ? !permanent_class(cls) : b.owner == owner) &&
            !(b.flags & CARD_POOL_BLOCK_QUARANTINED)) {
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

static int owner_matches(uint32_t held, uint32_t requested)
{
    uint32_t held_class = CARD_POOL_OWNER_CLASS(held);
    uint32_t requested_class = CARD_POOL_OWNER_CLASS(requested);

    if (held == requested)
        return 1;
    return (held & 0x00ffffffU) == (requested & 0x00ffffffU) &&
           ((held_class == CARD_POOL_CLASS_MINIGL &&
             requested_class == CARD_POOL_CLASS_MINIGL_CONTEXT) ||
            (held_class == CARD_POOL_CLASS_MINIGL_CONTEXT &&
             requested_class == CARD_POOL_CLASS_MINIGL));
}

static void quarantine_block(card_pool_block_t *block)
{
    if (block->flags & CARD_POOL_BLOCK_QUARANTINED)
        return;
    block->flags |= CARD_POOL_BLOCK_QUARANTINED;
    printf("[card-pool] quarantined %08lx-%08lx owner %08lx\r\n",
           (unsigned long)block->addr,
           (unsigned long)(block->addr + block->size - 1U),
           (unsigned long)block->owner);
}

void card_pool_quarantine_owner(card_pool_t *pool, uint32_t owner)
{
    uint32_t i;

    if (!pool || owner == 0U)
        return;
    sdk_smp_lock_acquire(&pool->lock);
    for (i = 0U; i < pool->block_count; ++i)
        if (owner_matches(pool->blocks[i].owner, owner))
            quarantine_block(&pool->blocks[i]);
    sdk_smp_lock_release(&pool->lock);
}

void card_pool_quarantine_minigl(card_pool_t *pool)
{
    uint32_t i;

    if (!pool)
        return;
    sdk_smp_lock_acquire(&pool->lock);
    for (i = 0U; i < pool->block_count; ++i) {
        uint32_t cls = CARD_POOL_OWNER_CLASS(pool->blocks[i].owner);

        if (cls == CARD_POOL_CLASS_MINIGL ||
            (cls >= CARD_POOL_CLASS_MINIGL_MODULE &&
             cls <= CARD_POOL_CLASS_MINIGL_RIX))
            quarantine_block(&pool->blocks[i]);
    }
    sdk_smp_lock_release(&pool->lock);
}

int card_pool_can_lend_to_amiga(const card_pool_t *pool)
{
    uint32_t r;
    uint32_t i;

    for (r = 0U; r < pool->range_count; ++r) {
        const card_pool_range_t *range = &pool->ranges[r];

        if (!range->revocable)
            continue;
        for (i = 0U; i < pool->block_count; ++i) {
            const card_pool_block_t *block = &pool->blocks[i];

            if ((block->flags & CARD_POOL_BLOCK_QUARANTINED) &&
                block->addr >= range->base && block->addr < range->end)
                return 0;
        }
    }
    return 1;
}

int card_pool_has_lent(const card_pool_t *pool)
{
    for (uint32_t r = 0; r < pool->range_count; r++) {
        if (pool->ranges[r].revocable && pool->ranges[r].open)
            return 1;
    }
    return 0;
}

void card_pool_take_back_lent(card_pool_t *pool)
{
    for (uint32_t r = 0; r < pool->range_count; r++) {
        if (pool->ranges[r].revocable && pool->ranges[r].open)
            card_pool_revoke_range(pool, r);
    }
}

void card_pool_finish_amiga_reset(card_pool_t *pool, int is_zorro3,
                                  int fastram_advertised)
{
    release_where(pool, 0, 1);
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

uint32_t card_pool_available_bytes(card_pool_t *pool, uint32_t owner)
{
    uint32_t cls = CARD_POOL_OWNER_CLASS(owner);
    uint32_t free_bytes = 0;

    if (cls == CARD_POOL_CLASS_NONE || cls >= CARD_POOL_CLASS_COUNT)
        return 0;
    sdk_smp_lock_acquire(&pool->lock);
    for (uint32_t range = 0; range < pool->range_count; range++) {
        const card_pool_range_t *r = &pool->ranges[range];

        if (!range_allows(r, cls))
            continue;
        free_bytes += r->end - r->base;
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
