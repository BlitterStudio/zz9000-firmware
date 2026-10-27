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

/* Every allocation consumes at least one 256-byte unit. */
#define SURFACE_ALLOCATOR_ALIGNMENT 256u

/* P96 allocates a bitmap per Workbench window/icon backing store, so
 * size the table for hundreds of concurrent bitmaps. */
#define SURFACE_ALLOCATOR_MAX_BLOCKS 1024u

/* Initialize (or reset) [base, base+size). Reset drops every dynamic P96
 * and firmware allocation; callers reinstall immutable reservations. */
void surface_allocator_init(uint32_t base, uint32_t size);

/* Reserve an immutable fixed ABI range. It can never be freed by either
 * owner and is rounded to the allocator's 256-byte granularity. */
int surface_allocator_reserve(uint32_t address, uint32_t size);
/* Release a fixed reservation at exactly `address`. Dynamic allocations
 * remain protected; only the driver-announcement guard is releasable. */
int surface_allocator_release_reservation(uint32_t address);

/* P96 allocations scan free space bottom-up. `alignment` is a power of two;
 * zero selects the mandatory 256-byte granularity. */
uint32_t surface_allocator_p96_alloc(uint32_t size, uint32_t alignment);

/* Grant an absolute P96 address when free, or regrant the P96 block exactly
 * at that address when it covers the rounded size. Regrants add one reference. */
uint32_t surface_allocator_p96_alloc_abs(uint32_t address, uint32_t size);
int surface_allocator_p96_free(uint32_t address);

/* Firmware allocations scan free space top-down. Firmware and P96 ownership
 * are intentionally disjoint: each free operation rejects the other owner. */
uint32_t surface_allocator_firmware_alloc_aligned(uint32_t size,
                                                  uint32_t alignment);
int surface_allocator_firmware_free(uint32_t address);

/* Rounded size of any allocated or reserved block at `addr`, or 0. */
uint32_t surface_allocator_block_size(uint32_t addr);

/* Whether a firmware-owned block starts at `addr` and covers `length`. */
int surface_allocator_firmware_owns(uint32_t addr, uint32_t length);

/* Total occupied bytes include fixed ABI reservations. */
uint32_t surface_allocator_used_bytes(void);
uint32_t surface_allocator_total_bytes(void);
void surface_allocator_free_stats(uint32_t *free_total,
                                  uint32_t *largest_free);

#endif /* SURFACE_ALLOCATOR_H */
