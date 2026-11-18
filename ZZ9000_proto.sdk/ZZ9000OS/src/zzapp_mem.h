/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_MEM_H
#define ZZAPP_MEM_H

#include <stdint.h>

#include "card_pool.h"
#include "sdk_smp_lock.h"

#define ZZAPP_MEM_FRAME_BYTES          (1u << 20)
#define ZZAPP_MEM_EXTENT_BYTES         (8u << 20)
#define ZZAPP_MEM_SYSTEM_RESERVE_BYTES (128u << 20)
#define ZZAPP_MEM_LOW_WATER_BYTES      (32u << 20)
#define ZZAPP_MEM_HIGH_WATER_BYTES     (64u << 20)
#define ZZAPP_MEM_PRIVATE_WINDOW_BYTES (512u << 20)
#define ZZAPP_MEM_MAX_FRAMES \
    (ZZAPP_MEM_PRIVATE_WINDOW_BYTES / ZZAPP_MEM_FRAME_BYTES)
#define ZZAPP_MEM_FRAMES_PER_EXTENT \
    (ZZAPP_MEM_EXTENT_BYTES / ZZAPP_MEM_FRAME_BYTES)
#define ZZAPP_MEM_MAX_EXTENTS \
    (ZZAPP_MEM_MAX_FRAMES / ZZAPP_MEM_FRAMES_PER_EXTENT)

/* Procs the ledger can track at once: one per process record, so the bound is
 * the handle table's (zzapp_rec.h ZZAPP_REC_MAX_APPS). */
#define ZZAPP_MEM_REGISTRY_PROCS 1024u

typedef struct {
    uint32_t base;
    uint8_t used_frames;
} zzapp_mem_extent_t;

typedef struct {
    uint32_t committed;
    uint32_t limit;
    uint32_t frame_count;
    uint32_t frames[ZZAPP_MEM_MAX_FRAMES];
} zzapp_mem_proc_t;

typedef void (*zzapp_mem_low_callback_t)(void *context);

typedef struct {
    sdk_smp_lock_t lock;
    card_pool_t *pool;
    uint32_t committed;
    uint32_t extent_count;
    uint32_t proc_count;
    uint8_t low_armed;
    zzapp_mem_low_callback_t low_callback;
    void *low_context;
    zzapp_mem_proc_t *procs[ZZAPP_MEM_REGISTRY_PROCS];
    zzapp_mem_extent_t extents[ZZAPP_MEM_MAX_EXTENTS];
} zzapp_mem_t;

/* The permanent card-pool capacity left after the reserve must fit in one
 * app's private virtual window. */
int zzapp_mem_boot_check(uint32_t reserve);
/* Initialize the frame allocator. Returns nonzero when the configured
 * reserve cannot satisfy the private-window bound. */
int zzapp_mem_init(zzapp_mem_t *memory, card_pool_t *pool);
/* Core 1 is halted. Free the lock if that core died holding it. */
void zzapp_mem_reset_lock(zzapp_mem_t *memory);
/* Reset the allocator last passed to zzapp_mem_init, if any. */
void zzapp_mem_reset_live_lock(void);
/* Rebuild allocator accounting from recorded frames after core 1 is halted. */
void zzapp_mem_reconcile_halted(void);
void zzapp_mem_proc_init(zzapp_mem_proc_t *proc, uint32_t limit);
void zzapp_mem_set_low_memory_callback(zzapp_mem_t *memory,
                                       zzapp_mem_low_callback_t callback,
                                       void *context);
/* Allocate whole 1 MiB frames and charge them to `proc`. */
int zzapp_mem_alloc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                    uint32_t frame_count, uint32_t *frames);
/* Reserve frames for a loader-private mapping. The caller MUST zero them
 * with zzapp_mem_zero_frames before exposing any mapping to an app. */
int zzapp_mem_alloc_unzeroed(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                             uint32_t frame_count, uint32_t *frames);
/* Zero at most `max_frames` frames in the recorded prefix. Returns the
 * number zeroed; the loader uses this between service passes. */
uint32_t zzapp_mem_zero_frames(const zzapp_mem_proc_t *proc,
                               uint32_t first_frame, uint32_t max_frames);
/* Release one frame previously returned for `proc`. */
int zzapp_mem_release(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                      uint32_t frame);
/* Release every frame held by `proc`, returning the bytes released. */
uint32_t zzapp_mem_release_proc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc);
/* Free bytes still admissible for app commits. */
uint32_t zzapp_mem_free_bytes(zzapp_mem_t *memory);
#ifdef ZZAPP_MEM_HOST_TEST
enum zzapp_mem_test_store {
    ZZAPP_MEM_TEST_AFTER_PROC_POINTER,
    ZZAPP_MEM_TEST_AFTER_EXTENT_BIT,
    ZZAPP_MEM_TEST_AFTER_FRAME_RECORD,
    ZZAPP_MEM_TEST_AFTER_PROC_CHARGE,
    ZZAPP_MEM_TEST_AFTER_MEMORY_CHARGE,
    ZZAPP_MEM_TEST_AFTER_POOL_ALLOCATION,
    ZZAPP_MEM_TEST_AFTER_PROC_COMPACT,
    ZZAPP_MEM_TEST_AFTER_FRAME_COMPACT,
    ZZAPP_MEM_TEST_AFTER_FRAME_COUNT,
    ZZAPP_MEM_TEST_AFTER_EXTENT_COMPACT,
};
void zzapp_mem_test_frame_store(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                                enum zzapp_mem_test_store store);
#endif

#endif /* ZZAPP_MEM_H */
