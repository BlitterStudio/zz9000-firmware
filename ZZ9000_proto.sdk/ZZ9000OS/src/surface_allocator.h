/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * Host-visible surface allocator shared by P96 off-screen bitmaps and SDK
 * mailbox allocations.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SURFACE_ALLOCATOR_H
#define SURFACE_ALLOCATOR_H

#include <stdint.h>

/* P96 alloc ABI guarantees 256-byte aligned surfaces. */
#define SURFACE_ALLOCATOR_ALIGNMENT 256u

/* P96 allocates a bitmap per Workbench window/icon backing store, so
 * size the table for hundreds of concurrent bitmaps. 8 bytes per entry. */
#define SURFACE_ALLOCATOR_MAX_BLOCKS 1024u

/* Initialize (or reset) the allocator over [base, base+size). Drops all
 * existing allocations; called at boot and on Amiga reset. */
void surface_allocator_init(uint32_t base, uint32_t size);

/* Reserve a known host-visible hardware/control range so no client allocator
 * may hand it out. The range must fit, not overlap a live block, and is
 * rounded to the allocator's 256-byte granularity. */
int surface_allocator_reserve(uint32_t address, uint32_t size);

/* Allocate `size` bytes with at least the requested power-of-two alignment.
 * Every block consumes a multiple of SURFACE_ALLOCATOR_ALIGNMENT so P96
 * allocations remain 256-byte aligned. Returns 0 on invalid input or
 * exhaustion. */
uint32_t surface_allocator_alloc_aligned(uint32_t size, uint32_t alignment);

/* P96 allocation shorthand. */
uint32_t surface_allocator_alloc(uint32_t size);

/* Free the block starting exactly at `addr`. Returns 0 on success, -1 if
 * `addr` is not a live block start (double free, mid-block, unknown) —
 * in which case no state changes. */
int surface_allocator_free(uint32_t addr);

/* Rounded size of the live block at `addr`, or 0 if unknown. */
uint32_t surface_allocator_block_size(uint32_t addr);

/* Whether a live block starts at `addr` and covers `length` bytes. */
int surface_allocator_owns(uint32_t addr, uint32_t length);

/* Total bytes currently allocated (rounded sizes). */
uint32_t surface_allocator_used_bytes(void);

/* Total managed bytes and current free-space shape. */
uint32_t surface_allocator_total_bytes(void);
void surface_allocator_free_stats(uint32_t *free_total,
                                  uint32_t *largest_free);

#endif /* SURFACE_ALLOCATOR_H */
