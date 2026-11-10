/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_AS_H
#define ZZAPP_AS_H

#include <stdint.h>

#include "memorymap.h"

#define ZZAPP_AS_SLOT_COUNT 4U
#define ZZAPP_AS_WINDOW_BASE 0xc0000000U
#define ZZAPP_AS_WINDOW_END  0xe0000000U
/*
 * Host-visible buffers live below the 1 MiB stack guard and the largest
 * permitted 1 MiB app stack at the top of the private window. The loader
 * must leave this range unmapped for private image, heap, and stack pages.
 */
#define ZZAPP_AS_ALIAS_BASE 0xdc000000U
#define ZZAPP_AS_ALIAS_END  0xdfe00000U
#define ZZAPP_AS_ALIAS_PAGE_COUNT \
	((ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE) / 4096U)
#define ZZAPP_AS_ENOMEM       (-3)

#define ZZAPP_AS_IMAGE_RX 1U
#define ZZAPP_AS_IMAGE_RO 2U
#define ZZAPP_AS_IMAGE_RW 3U

#define ZZAPP_AS_ALIAS_RO 1U
#define ZZAPP_AS_ALIAS_RW 2U
#define ZZAPP_AS_ALIAS_RX 3U

typedef struct {
	uint32_t *l1;
	uint32_t generation;
	uint16_t asid;
	uint8_t slot;
	uint8_t active;
} zzapp_as_t;

/* Build the privileged kernel template from the BSP's identity table. */
void zzapp_as_init(const uint32_t *bsp_l1);
/* Allocate the fixed table for one slot. ASIDs are permanently slot + 1. */
int zzapp_as_create(zzapp_as_t *as, uint32_t slot, uint32_t generation);
void zzapp_as_destroy(zzapp_as_t *as);
/* Map 1 MiB app frames at a private virtual address. */
int zzapp_as_map_frames(zzapp_as_t *as, uint32_t virtual_base,
			const uint32_t *frames, uint32_t frame_count);
/* Remove 1 MiB private mappings. */
int zzapp_as_unmap_frames(zzapp_as_t *as, uint32_t virtual_base,
                          uint32_t frame_count);
/* Map [phys, phys + bytes) into this app's private alias range. phys and
 * bytes must be page aligned. alignment must be a power-of-two page
 * alignment. RO and RW mappings are XN; RX is executable. */
int zzapp_as_alias_map_flags_aligned(zzapp_as_t *as, uint32_t phys,
				     uint32_t bytes, uint32_t flags,
				     uint32_t alignment, uint32_t *va);
/* Map an XN RO or RW alias with the requested virtual alignment. */
int zzapp_as_alias_map_aligned(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			       int writable, uint32_t alignment, uint32_t *va);
/* Convenience wrapper for a page-aligned RO, RW, or RX alias. */
int zzapp_as_alias_map_flags(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			     uint32_t flags, uint32_t *va);
/* Convenience wrapper for a page-aligned XN RO or RW alias. */
int zzapp_as_alias_map(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
		       int writable, uint32_t *va);
int zzapp_as_alias_unmap(zzapp_as_t *as, uint32_t va, uint32_t bytes);
/* Return the physical address for an alias address, or zero when unmapped. */
uint32_t zzapp_as_alias_phys(const zzapp_as_t *as, uint32_t va);
/* Map page-aligned image memory with the requested user permissions. */
int zzapp_as_map_image(zzapp_as_t *as, uint32_t virtual_base,
			uint32_t physical_base, uint32_t bytes, uint32_t permissions);
/* Install this address space on core 1. */
void zzapp_as_switch(zzapp_as_t *as);
/* Restore core 1's privileged kernel template. */
void zzapp_as_switch_kernel(void);

const uint32_t *zzapp_as_template(void);
const uint32_t *zzapp_as_l1(const zzapp_as_t *as);
const uint32_t *zzapp_as_l2_for(const zzapp_as_t *as, uint32_t address);
/* mmu_page.c calls this after changing a BSP mapping. */
void zzapp_as_mmu_page_changed(uint32_t section, uint32_t descriptor,
				const uint32_t *pages);

#endif
