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
#include "minigl_pool.h"

#define CARD_POOL_PAGE_BYTES 0x00010000u
#define CARD_POOL_MAX_RANGES 2u
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
    /* Decoder working sets. */
    CARD_POOL_CLASS_MEDIA = 2,
    CARD_POOL_CLASS_IMAGE = 3,
    /* Client-owned resident media input. This is deliberately distinct from
     * decoder MEDIA: fault recovery releases decoder owners by session id. */
    CARD_POOL_CLASS_MEDIA_INPUT = 4,
    /* ARM-only SDK surfaces. They are released on every Amiga reset. */
    CARD_POOL_CLASS_SDK_SURFACE = 5,
    /* RFB console shadow, snapshot, scratch, and deflate workspace. */
    CARD_POOL_CLASS_CONSOLE = 6,
    /* MiniGL wire-v9 render contexts: colour/depth/texture working sets.
     * Released per context destroy, by owner on fault recovery, and on
     * every Amiga reset. */
    CARD_POOL_CLASS_MINIGL = 7,
    /* Trusted PIE app arena. */
    CARD_POOL_CLASS_APP = 8,
    /* Module-ABI v3 MiniGL resources are distinct so reclaim cannot free a
     * transport lease while closing a render context. */
    CARD_POOL_CLASS_MINIGL_MODULE = 9,
    CARD_POOL_CLASS_MINIGL_TRANSPORT = 10,
    CARD_POOL_CLASS_MINIGL_CONTEXT = 11,
    CARD_POOL_CLASS_MINIGL_APP_CONTEXT = 12,
    CARD_POOL_CLASS_MINIGL_RIX = 13,
    /* Firmware-owned back buffers for a trusted app's single-buffer bind. */
    CARD_POOL_CLASS_APP_DISPLAY = 14,
    /* 1 MiB-aligned extents the app frame allocator carves into frames.
     * Commit admission in zzapp_mem bounds them, not a class limit. */
    CARD_POOL_CLASS_APP_FRAMES = 15,
    /* The boot-time record arena (zzapp_rec): per-record L1 tables, core-1
     * stacks, records, user table pages, and the L2 table pool. Permanent
     * like FIRMWARE: never in the revocable tail, kept across Amiga reset,
     * never freed. The arena size is set by memory (zzapp_rec_layout_for),
     * not by a class limit. */
    CARD_POOL_CLASS_APP_ARENA = 16,
    CARD_POOL_CLASS_COUNT
};

#define CARD_POOL_LIMIT_FIRMWARE    (64u << 20)
#define CARD_POOL_LIMIT_MEDIA       (128u << 20)
#define CARD_POOL_LIMIT_MEDIA_INPUT (64u << 20)
#define CARD_POOL_LIMIT_CONSOLE     (32u << 20)
/* Shared MiniGL budget. Context resources grow on demand, so the ceiling
 * protects the card pool without reserving it for idle contexts. */
#define CARD_POOL_LIMIT_MINIGL      MINIGL_CARD_POOL_LIMIT_BYTES
/* ARM-only client surfaces are card-resident and reset-reclaimable. */
#define CARD_POOL_LIMIT_SDK_SURFACE (128u << 20)
/* Matches the image decode budget all image sessions share
 * (SDK_IMAGE_STREAM_MAX_DECODE_STATE_BYTES). */
#define CARD_POOL_LIMIT_IMAGE       (72u << 20)
#define CARD_POOL_LIMIT_APP         (96u << 20)
#define CARD_POOL_LIMIT_APP_DISPLAY (32u << 20)
#define CARD_POOL_LIMIT_APP_FRAMES  UINT32_MAX
#define CARD_POOL_LIMIT_APP_ARENA   UINT32_MAX
#define CARD_POOL_LIMIT_MINIGL_MODULE    (4u << 20)
#define CARD_POOL_LIMIT_MINIGL_TRANSPORT (4u << 20)
#define CARD_POOL_LIMIT_MINIGL_CONTEXT   MINIGL_CARD_POOL_LIMIT_BYTES
#define CARD_POOL_LIMIT_MINIGL_APP_CONTEXT MINIGL_CARD_POOL_LIMIT_BYTES
#define CARD_POOL_LIMIT_MINIGL_RIX       MINIGL_CARD_POOL_LIMIT_BYTES

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
    uint8_t flags;
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

#define CARD_POOL_BLOCK_QUARANTINED 0x01u

/* One contiguous card pool: a permanent prefix plus a revocable Fast-RAM
 * tail. The tail stays closed while Fast RAM is advertised. */
extern card_pool_t card_pool;
enum {
    CARD_POOL_RANGE_PERMANENT,
    CARD_POOL_RANGE_REVOCABLE,
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
/* Drop every non-quarantined block in the range and close it. Their owners'
 * later frees are ignored. Returns the bytes dropped. */
uint32_t card_pool_revoke_range(card_pool_t *pool, uint32_t range);

/* `size` bytes rounded up to whole pages, page aligned. Returns the card
 * address, or 0 for a zero size, an invalid owner class, a class over its
 * limit, no gap large enough, or a full block table. */
uint32_t card_pool_alloc(card_pool_t *pool, uint32_t size, uint32_t owner);
/* Like card_pool_alloc(), but starts the block at an `alignment` boundary.
 * The alignment must be a power of two and at least one pool page. */
uint32_t card_pool_alloc_aligned(card_pool_t *pool, uint32_t size,
                                 uint32_t alignment, uint32_t owner);
/* Whether `owner` owns the live block starting at `addr` and that block
 * covers `length`. */
int card_pool_block_owned(card_pool_t *pool, uint32_t addr, uint32_t length,
                          uint32_t owner);
/* Whether `owner` owns a live block containing `[addr, addr + length)`. */
int card_pool_range_owned(card_pool_t *pool, uint32_t addr, uint32_t length,
                          uint32_t owner);
/* Frees the block starting at `addr` when `owner` owns it and the block is
 * not quarantined, and returns 1. Otherwise nothing changes, the free is
 * counted in ignored_frees, and it returns 0. A zero address is a no-op
 * returning 0. */
int card_pool_free(card_pool_t *pool, uint32_t addr, uint32_t owner);
/* Free every non-quarantined block of one owner. Returns the bytes freed. */
uint32_t card_pool_release_owner(card_pool_t *pool, uint32_t owner);
/* Permanently retain every live block of `owner`. Legacy MiniGL (class 7)
 * and module MiniGL context (class 11) owners with the same instance name
 * the same context. */
void card_pool_quarantine_owner(card_pool_t *pool, uint32_t owner);
/* Permanently retain every live MiniGL block. Used only while core 1 is
 * halted after a cold restart. */
void card_pool_quarantine_minigl(card_pool_t *pool);
/* Whether the revocable Fast-RAM window contains quarantined memory and
 * therefore cannot be handed to the Amiga. */
int card_pool_can_lend_to_amiga(const card_pool_t *pool);

/* Whether a revocable range is lent out. Reads no lock: only core 0 opens
 * and closes ranges, so its own answer cannot change underneath it. */
int card_pool_has_lent(const card_pool_t *pool);
/* Amiga reset, before the fast-RAM gate can reopen: revoke every lent range's
 * non-quarantined blocks. Quarantined blocks remain reserved; callers must
 * keep the gate closed while card_pool_can_lend_to_amiga() is false. The
 * caller must first stop core 1 (which also frees a pool lock a faulted core
 * 1 still holds) and afterwards write back the ARM cache lines for that
 * memory. Takes no lock when nothing is lent. */
void card_pool_take_back_lent(card_pool_t *pool);
/* Amiga reset, after core 0 has observed RasterIX completion or quarantined
 * its MiniGL blocks: release every non-quarantined owner except the permanent
 * classes FIRMWARE and APP_ARENA,
 * then lend the revocable tail when the Amiga cannot use it -- a Zorro II
 * board has no Fast-RAM window, and Zorro III lends it while Fast RAM is
 * withheld. */
void card_pool_finish_amiga_reset(card_pool_t *pool, int is_zorro3,
                                  int fastram_advertised);

/* Free bytes in one interval; 0 while it is closed. */
uint32_t card_pool_range_free_bytes(card_pool_t *pool, uint32_t range);
/* Free bytes available to `owner`, excluding closed and incompatible
 * revocable ranges. */
uint32_t card_pool_available_bytes(card_pool_t *pool, uint32_t owner);

/* Force the pool lock free. Only for core-1 fault recovery, while core 1
 * is halted and cannot be inside the pool. */
void card_pool_reset_lock(card_pool_t *pool);

#endif /* CARD_POOL_H */
