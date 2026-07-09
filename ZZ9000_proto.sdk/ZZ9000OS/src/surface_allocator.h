/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * RTG surface allocator: manages the legacy surface heap that backs
 * P96 off-screen bitmaps (ACC_OP_ALLOC_SURFACE / ACC_OP_FREE_SURFACE).
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SURFACE_ALLOCATOR_H
#define SURFACE_ALLOCATOR_H

#include <stdint.h>

/* The ACC alloc ABI guarantees 256-byte aligned surfaces. */
#define SURFACE_ALLOCATOR_ALIGNMENT 256u

/* P96 allocates a bitmap per Workbench window/icon backing store, so
 * size the table for hundreds of concurrent bitmaps. 8 bytes per entry. */
#define SURFACE_ALLOCATOR_MAX_BLOCKS 1024u

/* Initialize (or reset) the allocator over [base, base+size). Drops all
 * existing allocations; called at boot and on Amiga reset. */
void surface_allocator_init(uint32_t base, uint32_t size);

/* Allocate `size` bytes (rounded up to the alignment). Returns the block
 * address, or 0 on failure (zero size, no gap large enough, table full). */
uint32_t surface_allocator_alloc(uint32_t size);

/* Free the block starting exactly at `addr`. Returns 0 on success, -1 if
 * `addr` is not a live block start (double free, mid-block, unknown) —
 * in which case no state changes. */
int surface_allocator_free(uint32_t addr);

/* Rounded size of the live block at `addr`, or 0 if unknown. */
uint32_t surface_allocator_block_size(uint32_t addr);

/* Total bytes currently allocated (rounded sizes). */
uint32_t surface_allocator_used_bytes(void);

#endif /* SURFACE_ALLOCATOR_H */
