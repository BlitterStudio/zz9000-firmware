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
#define ZZAPP_AS_ENOMEM       (-3)

#define ZZAPP_AS_IMAGE_RX 1U
#define ZZAPP_AS_IMAGE_RO 2U
#define ZZAPP_AS_IMAGE_RW 3U

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
/* Map the launcher window as one identity-mapped user section. */
int zzapp_as_map_window(zzapp_as_t *as, uint32_t physical_base);
/* Add or remove identity-mapped user RW pages for an owned low-DDR grant. */
int zzapp_as_grant_user(zzapp_as_t *as, uint32_t address, uint32_t bytes);
int zzapp_as_ungrant_user(zzapp_as_t *as, uint32_t address, uint32_t bytes);
/* Map page-aligned image memory with the requested user permissions. */
int zzapp_as_map_image(zzapp_as_t *as, uint32_t virtual_base,
			uint32_t physical_base, uint32_t bytes, uint32_t permissions);
/* Install this address space on core 1. */
void zzapp_as_switch(zzapp_as_t *as);

const uint32_t *zzapp_as_template(void);
const uint32_t *zzapp_as_l1(const zzapp_as_t *as);
const uint32_t *zzapp_as_l2_for(const zzapp_as_t *as, uint32_t address);
/* mmu_page.c calls this after changing a BSP mapping. */
void zzapp_as_mmu_page_changed(uint32_t section, uint32_t descriptor,
				const uint32_t *pages);

#endif
