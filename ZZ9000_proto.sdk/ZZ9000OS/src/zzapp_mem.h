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
    uint8_t low_armed;
    zzapp_mem_low_callback_t low_callback;
    void *low_context;
    zzapp_mem_extent_t extents[ZZAPP_MEM_MAX_EXTENTS];
} zzapp_mem_t;

/* The permanent card-pool capacity left after the reserve must fit in one
 * app's private virtual window. */
int zzapp_mem_boot_check(uint32_t reserve);
/* Initialize the frame allocator. Returns nonzero when the configured
 * reserve cannot satisfy the private-window bound. */
int zzapp_mem_init(zzapp_mem_t *memory, card_pool_t *pool);
void zzapp_mem_proc_init(zzapp_mem_proc_t *proc, uint32_t limit);
void zzapp_mem_set_low_memory_callback(zzapp_mem_t *memory,
                                       zzapp_mem_low_callback_t callback,
                                       void *context);
/* Allocate whole 1 MiB frames and charge them to `proc`. */
int zzapp_mem_alloc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                    uint32_t frame_count, uint32_t *frames);
/* Release one frame previously returned for `proc`. */
int zzapp_mem_release(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                      uint32_t frame);
/* Release every frame held by `proc`, returning the bytes released. */
uint32_t zzapp_mem_release_proc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc);
/* Free bytes still admissible for app commits. */
uint32_t zzapp_mem_free_bytes(zzapp_mem_t *memory);

#endif /* ZZAPP_MEM_H */
