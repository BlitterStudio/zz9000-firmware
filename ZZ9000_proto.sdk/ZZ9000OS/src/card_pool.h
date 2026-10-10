/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * Card pool: owner-tracked large-block allocator over the DDR ranges no
 * fixed buffer claims (memorymap.h CARD_POOL_*). Firmware services take
 * their working memory from here in 64 KB pages. Every block belongs to an
 * owner, so a session can give back everything it holds in one call and
 * the firmware can reclaim a dead session's memory without its help.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CARD_POOL_H
#define CARD_POOL_H

#include <stdint.h>
#include "sdk_smp_lock.h"

#define CARD_POOL_PAGE_BYTES 0x00010000u
#define CARD_POOL_MAX_RANGES 4u
/* A full table fails the allocation like an out-of-memory. */
#define CARD_POOL_MAX_BLOCKS 512u

/* An owner is a class in the top byte and an instance below it. Instances
 * carry a generation, so a reused session slot never matches the owner of
 * memory a previous session lost in a reclaim. */
enum card_pool_class {
    CARD_POOL_CLASS_NONE = 0,
    /* Permanent firmware allocations: survive Amiga resets, never placed in
     * a revocable range. */
    CARD_POOL_CLASS_FIRMWARE = 1,
    CARD_POOL_CLASS_MEDIA = 2,
    CARD_POOL_CLASS_IMAGE = 3,
    CARD_POOL_CLASS_COUNT
};

#define CARD_POOL_LIMIT_FIRMWARE (64u << 20)
#define CARD_POOL_LIMIT_MEDIA    (128u << 20)
/* Matches the image decode budget all image sessions share
 * (SDK_IMAGE_STREAM_MAX_DECODE_STATE_BYTES). */
#define CARD_POOL_LIMIT_IMAGE    (72u << 20)

#define CARD_POOL_OWNER(cls, instance) \
    (((uint32_t)(cls) << 24) | ((uint32_t)(instance) & 0x00FFFFFFu))
#define CARD_POOL_OWNER_CLASS(owner) ((uint32_t)(owner) >> 24)

typedef struct {
    uint32_t base;
    uint32_t end;
    uint8_t open;
    /* Lent memory the firmware takes back (the Z3 fast-RAM window). */
    uint8_t revocable;
} card_pool_range_t;

typedef struct {
    uint32_t addr;
    uint32_t size;
    uint32_t owner;
} card_pool_block_t;

typedef struct {
    sdk_smp_lock_t lock;
    card_pool_range_t ranges[CARD_POOL_MAX_RANGES];
    uint32_t range_count;
    /* Live blocks, sorted by address. */
    card_pool_block_t blocks[CARD_POOL_MAX_BLOCKS];
    uint32_t block_count;
    uint32_t class_used[CARD_POOL_CLASS_COUNT];
    uint32_t class_limit[CARD_POOL_CLASS_COUNT];
    /* Frees naming an unknown address or another owner. */
    uint32_t ignored_frees;
} card_pool_t;

/* The firmware's pool. Ranges A1, A2 and C open; B (the fast-RAM window)
 * stays closed until card_pool_open_range. */
extern card_pool_t card_pool;
enum {
    CARD_POOL_RANGE_A1,
    CARD_POOL_RANGE_A2,
    CARD_POOL_RANGE_C,
    CARD_POOL_RANGE_B,
};
void card_pool_setup_card(card_pool_t *pool);

/* Empty pool with the class limits above and no ranges. */
void card_pool_init(card_pool_t *pool);

/* Ranges are tried in the order they are added. Page-aligned bounds; a new
 * range starts closed. Returns the range index, or -1 when the table is
 * full or the bounds are invalid. */
int card_pool_add_range(card_pool_t *pool, uint32_t base, uint32_t end,
                        int revocable);
void card_pool_open_range(card_pool_t *pool, uint32_t range);
/* Drop every block in the range and close it. Their owners' later frees
 * are ignored. Returns the bytes dropped. */
uint32_t card_pool_revoke_range(card_pool_t *pool, uint32_t range);

/* `size` bytes rounded up to whole pages, page aligned. Returns the card
 * address, or 0 for a zero size, an invalid owner class, a class over its
 * limit, no gap large enough, or a full block table. */
uint32_t card_pool_alloc(card_pool_t *pool, uint32_t size, uint32_t owner);
/* Frees the block starting at `addr` when `owner` owns it and returns 1.
 * Otherwise nothing changes, the free is counted in ignored_frees, and it
 * returns 0. A zero address is a no-op returning 0. */
int card_pool_free(card_pool_t *pool, uint32_t addr, uint32_t owner);
/* Free every block of one owner. Returns the bytes freed. */
uint32_t card_pool_release_owner(card_pool_t *pool, uint32_t owner);

/* Whether a revocable range is lent out. Reads no lock: only core 0 opens
 * and closes ranges, so its own answer cannot change underneath it. */
int card_pool_has_lent(const card_pool_t *pool);
/* Amiga reset, before the fast-RAM gate can reopen: revoke every lent
 * range. The caller must first stop core 1 (which also frees a pool lock a
 * faulted core 1 still holds) and afterwards write back the ARM cache
 * lines for that memory. Takes no lock when nothing is lent. */
void card_pool_take_back_lent(card_pool_t *pool);
/* Amiga reset, after the module resets: release every owner except the
 * FIRMWARE class, then lend the revocable ranges again when the Amiga
 * cannot use them -- a Zorro II board has no fast-RAM window, and on
 * Zorro III the window is free while fast RAM is not advertised. */
void card_pool_finish_amiga_reset(card_pool_t *pool, int is_zorro3,
                                  int fastram_advertised);

/* Free bytes in one range; 0 while it is closed. */
uint32_t card_pool_range_free_bytes(card_pool_t *pool, uint32_t range);

/* Force the pool lock free. Only for core-1 fault recovery, while core 1
 * is halted and cannot be inside the pool. */
void card_pool_reset_lock(card_pool_t *pool);

#endif /* CARD_POOL_H */
