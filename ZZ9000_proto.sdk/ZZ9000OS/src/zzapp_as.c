/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "memorymap.h"
#include "mmu_page.h"
#include "zzapp_as.h"
#include "sdk_smp_lock.h"
#ifndef MMU_PAGE_HOST_TEST
#include "xil_cache.h"
#include "xil_types.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#endif

#define L1_ENTRIES 4096U
#define L2_ENTRIES 256U
#define SECTION_SHIFT 20U
#define PAGE_SHIFT 12U
#define SECTION_MASK 0xfff00000U
#define PAGE_MASK 0xfffff000U
#define L1_TYPE_MASK 3U
#define L1_SECTION 2U
#define L1_TABLE 1U
#define L1_TABLE_BASE_MASK 0xfffffc00U
#define L1_ATTR_MASK 0x000fffffU
#define L1_AP_MASK (3U << 10)
#define L1_AP_PRIV_RW (1U << 10)
#define L1_NG (1U << 17)
#define L2_TYPE 2U
#define L2_AP_MASK (3U << 4)
#define L2_AP_PRIV_RW (1U << 4)
#define L2_AP_USER_RW (3U << 4)
#define L2_AP_USER_RO (2U << 4)
#define L2_TEX_MASK (7U << 6)
#define L2_C (1U << 3)
#define L2_B (1U << 2)
#define L2_S (1U << 10)
#define L2_AP2 (1U << 9)
#define L2_NG (1U << 11)
#define L2_XN 1U
#define L2_MEMORY_MASK (L2_TEX_MASK | L2_C | L2_B | L2_S)
#define DDR_SECTION 0x00015de6U
#define DDR_PAGE 0x00000576U
#define TTBR0_ATTR 0x5bU

#ifndef MMU_PAGE_HOST_TEST
#define ZZAPP_AS_TABLES __attribute__((section(".zzapp_as_tables")))
#else
#define ZZAPP_AS_TABLES
#endif

static uint32_t template_l1[L1_ENTRIES] ZZAPP_AS_TABLES
	__attribute__((aligned(0x4000)));
static uint32_t app_l1[ZZAPP_AS_SLOT_COUNT][L1_ENTRIES] ZZAPP_AS_TABLES
	__attribute__((aligned(0x4000)));
static uint32_t l2_pool[ZZAPP_AS_L2_TABLE_COUNT][L2_ENTRIES] ZZAPP_AS_TABLES
	__attribute__((aligned(1024)));
static uint8_t l2_owner[ZZAPP_AS_L2_TABLE_COUNT];
static uint8_t slot_live[ZZAPP_AS_SLOT_COUNT];
static uint32_t asid_generation[ZZAPP_AS_SLOT_COUNT];
static uint8_t alias_used[ZZAPP_AS_SLOT_COUNT][ZZAPP_AS_ALIAS_PAGE_COUNT];
static uint16_t alias_length[ZZAPP_AS_SLOT_COUNT][ZZAPP_AS_ALIAS_PAGE_COUNT];
static int template_ready;
static sdk_smp_lock_t as_lock = SDK_SMP_LOCK_INIT;

static void as_clean(const void *address, uint32_t bytes)
{
#ifndef MMU_PAGE_HOST_TEST
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)address, bytes);
#else
	(void)address;
	(void)bytes;
#endif
}

/* Inner-shareable: a change may come from core 0 (mmu_page) while core 1
 * walks the same template or app table. */
static void as_sync(void)
{
#ifndef MMU_PAGE_HOST_TEST
	dsb();
	mtcp(XREG_CP15_INVAL_TLB_IS, 0U);
	mtcp(XREG_CP15_INVAL_BRANCH_ARRAY_IS, 0U);
	dsb();
	isb();
#endif
}

static void invalidate_asid(uint32_t asid)
{
#ifndef MMU_PAGE_HOST_TEST
	__asm__ __volatile__("dsb sy\n\t"
		"mcr p15, 0, %0, c8, c3, 2\n\t"
		"dsb sy\n\tisb" : : "r" (asid) : "memory");
#else
	(void)asid;
#endif
}

static void replace_live_descriptor(uint32_t *entry, uint32_t descriptor)
{
	if (*entry == descriptor)
		return;
	*entry = 0U;
	as_clean(entry, sizeof(*entry));
	as_sync();
	*entry = descriptor;
	as_clean(entry, sizeof(*entry));
	as_sync();
}

static void replace_live_page(uint32_t *entry, uint32_t descriptor)
{
	replace_live_descriptor(entry, descriptor);
}

static uint32_t priv_global_section(uint32_t descriptor)
{
	if ((descriptor & L1_TYPE_MASK) != L1_SECTION)
		return descriptor;
	return (descriptor & ~(L1_AP_MASK | L1_NG)) | L1_AP_PRIV_RW;
}

static uint32_t priv_global_page(uint32_t descriptor)
{
	if ((descriptor & L1_TYPE_MASK) != L2_TYPE)
		return descriptor;
	return (descriptor & ~(L2_AP_MASK | L2_NG)) | L2_AP_PRIV_RW;
}

static uint32_t section_page_descriptor(uint32_t section, uint32_t page,
					uint32_t descriptor)
{
	uint32_t attrs = 0U;

	attrs |= ((descriptor >> 12) & 7U) << 6;
	attrs |= descriptor & (L2_C | L2_B);
	attrs |= ((descriptor >> 16) & 1U) << 10;
	attrs |= ((descriptor >> 15) & 1U) << 9;
	attrs |= ((descriptor >> 10) & 3U) << 4;
	attrs |= (descriptor >> 4) & L2_XN;
	return (section << SECTION_SHIFT) | (page << PAGE_SHIFT) | attrs | L2_TYPE;
}

static uint32_t l2_descriptor(const uint32_t *table, uint32_t domain)
{
	return ((uint32_t)(uintptr_t)table & L1_TABLE_BASE_MASK) |
		(domain & 0x1e0U) | L1_TABLE;
}

static int l2_index_for_descriptor(uint32_t descriptor)
{
	uint32_t i;
	uint32_t base = descriptor & L1_TABLE_BASE_MASK;

	for (i = 0U; i < ZZAPP_AS_L2_TABLE_COUNT; i++)
		if (((uint32_t)(uintptr_t)l2_pool[i] & L1_TABLE_BASE_MASK) == base)
			return (int)i;
	return -1;
}

static int l2_alloc(uint8_t owner)
{
	uint32_t i;

	for (i = 0U; i < ZZAPP_AS_L2_TABLE_COUNT; i++) {
		if (l2_owner[i] == 0xffU) {
			l2_owner[i] = owner;
			memset(l2_pool[i], 0, sizeof(l2_pool[i]));
			return (int)i;
		}
	}
	return -1;
}

static void l2_release_owner(uint8_t owner)
{
	uint32_t i;

	for (i = 0U; i < ZZAPP_AS_L2_TABLE_COUNT; i++) {
		if (l2_owner[i] == owner) {
			l2_owner[i] = 0xffU;
			memset(l2_pool[i], 0, sizeof(l2_pool[i]));
		}
	}
}

static int grant_l2_available(const zzapp_as_t *as, uint32_t first,
			      uint32_t end)
{
	uint32_t section;
	uint32_t needed = 0U;
	uint32_t available = 0U;

	for (section = first >> SECTION_SHIFT;
	     section <= (end - 1U) >> SECTION_SHIFT; section++) {
		uint32_t descriptor = as->l1[section];
		int index = l2_index_for_descriptor(descriptor);

		if ((descriptor & L1_TYPE_MASK) == L1_TABLE &&
		    index >= 0 && l2_owner[index] == as->slot + 1U)
			continue;
		if ((descriptor & L1_TYPE_MASK) == L1_TABLE && index < 0)
			return 0;
		needed++;
	}
	for (section = 0U; section < ZZAPP_AS_L2_TABLE_COUNT; section++)
		if (l2_owner[section] == 0xffU)
			available++;
	return needed <= available;
}

static void populate_section_pages(uint32_t *table, uint32_t section,
				   uint32_t descriptor)
{
	uint32_t page;

	for (page = 0U; page < L2_ENTRIES; page++)
		table[page] = priv_global_page(
			section_page_descriptor(section, page, descriptor));
}

static int template_l2(uint32_t section, const uint32_t *pages)
{
	int index = l2_index_for_descriptor(template_l1[section]);
	uint32_t page;

	if (index < 0 || l2_owner[index] != 0U) {
		index = l2_alloc(0U);
		if (index < 0)
			return ZZAPP_AS_ENOMEM;
		template_l1[section] = l2_descriptor(l2_pool[index], 15U << 5);
	}
	for (page = 0U; page < L2_ENTRIES; page++)
		l2_pool[index][page] = priv_global_page(pages[page]);
	as_clean(l2_pool[index], sizeof(l2_pool[index]));
	return 0;
}

static uint32_t template_page(uint32_t section, uint32_t page)
{
	uint32_t descriptor = template_l1[section];
	int index;

	if ((descriptor & L1_TYPE_MASK) == L1_SECTION)
		return priv_global_page(section_page_descriptor(section, page,
			descriptor));
	index = l2_index_for_descriptor(descriptor);
	return index < 0 ? 0U : l2_pool[index][page];
}

static int ensure_l2(zzapp_as_t *as, uint32_t section)
{
	uint32_t descriptor = as->l1[section];
	int old_index;
	int new_index;

	if ((descriptor & L1_TYPE_MASK) == L1_TABLE) {
		old_index = l2_index_for_descriptor(descriptor);
		if (old_index >= 0 && l2_owner[old_index] == as->slot + 1U)
			return old_index;
		if (old_index < 0)
			return ZZAPP_AS_ENOMEM;
		new_index = l2_alloc(as->slot + 1U);
		if (new_index < 0)
			return ZZAPP_AS_ENOMEM;
		memcpy(l2_pool[new_index], l2_pool[old_index],
		       sizeof(l2_pool[new_index]));
		as->l1[section] = l2_descriptor(l2_pool[new_index], descriptor);
		as_clean(l2_pool[new_index], sizeof(l2_pool[new_index]));
		return new_index;
	}
	new_index = l2_alloc(as->slot + 1U);
	if (new_index < 0)
		return ZZAPP_AS_ENOMEM;
	if ((descriptor & L1_TYPE_MASK) == L1_SECTION)
		populate_section_pages(l2_pool[new_index], section, descriptor);
	as->l1[section] = l2_descriptor(l2_pool[new_index],
		descriptor ? descriptor : 15U << 5);
	as_clean(l2_pool[new_index], sizeof(l2_pool[new_index]));
	return new_index;
}

static int app_range_ok(const zzapp_as_t *as, uint32_t address,
			uint32_t bytes, uint32_t align)
{
	if (!as || !as->active || bytes == 0U || (address & (align - 1U)) != 0U ||
	    (bytes & (align - 1U)) != 0U || address < ZZAPP_AS_WINDOW_BASE ||
	    bytes > ZZAPP_AS_WINDOW_END - address)
		return 0;
	return 1;
}

static int alias_range_ok(const zzapp_as_t *as, uint32_t address,
			  uint32_t bytes)
{
	if (!as || !as->active || bytes == 0U ||
	    (address & (4096U - 1U)) != 0U ||
	    (bytes & (4096U - 1U)) != 0U ||
	    address < ZZAPP_AS_ALIAS_BASE ||
	    bytes > ZZAPP_AS_ALIAS_END - address)
		return 0;
	return 1;
}

static int alias_first_free(uint32_t slot, uint32_t pages)
{
	uint32_t first;
	uint32_t page;

	for (first = 0U; first <= ZZAPP_AS_ALIAS_PAGE_COUNT - pages; first++) {
		for (page = 0U; page < pages; page++)
			if (alias_used[slot][first + page])
				break;
		if (page == pages)
			return (int)first;
	}
	return -1;
}

void zzapp_as_init(const uint32_t *bsp_l1)
{
	uint32_t section;

	memset(template_l1, 0, sizeof(template_l1));
	memset(app_l1, 0, sizeof(app_l1));
	memset(slot_live, 0, sizeof(slot_live));
	memset(asid_generation, 0, sizeof(asid_generation));
	memset(alias_used, 0, sizeof(alias_used));
	memset(alias_length, 0, sizeof(alias_length));
	memset(l2_owner, 0xff, sizeof(l2_owner));
	if (!bsp_l1)
		return;
	for (section = 0U; section < L1_ENTRIES; section++) {
		uint32_t descriptor = bsp_l1[section];

		if ((descriptor & L1_TYPE_MASK) == L1_SECTION)
			template_l1[section] = priv_global_section(descriptor);
		else if ((descriptor & L1_TYPE_MASK) == L1_TABLE) {
#ifdef MMU_PAGE_HOST_TEST
			const uint32_t *pages = mmu_page_test_page_table();
#else
			const uint32_t *pages = (const uint32_t *)(uintptr_t)
				(descriptor & L1_TABLE_BASE_MASK);
#endif
			(void)template_l2(section, pages);
		}
	}
	as_clean(template_l1, sizeof(template_l1));
	mmu_page_set_change_hook(zzapp_as_mmu_page_changed);
	template_ready = 1;
}

int zzapp_as_create(zzapp_as_t *as, uint32_t slot, uint32_t generation)
{
	if (!as || slot >= ZZAPP_AS_SLOT_COUNT)
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	if (slot_live[slot]) {
		sdk_smp_lock_release(&as_lock);
		return -1;
	}
	memcpy(app_l1[slot], template_l1, sizeof(template_l1));
	as->l1 = app_l1[slot];
	as->generation = generation;
	as->asid = (uint16_t)(slot + 1U);
	as->slot = (uint8_t)slot;
	as->active = 1U;
	memset(alias_used[slot], 0, sizeof(alias_used[slot]));
	memset(alias_length[slot], 0, sizeof(alias_length[slot]));
	slot_live[slot] = 1U;
	as_clean(as->l1, sizeof(app_l1[slot]));
	sdk_smp_lock_release(&as_lock);
	return 0;
}

void zzapp_as_destroy(zzapp_as_t *as)
{
	if (!as || !as->active)
		return;
	zzapp_as_switch_kernel();
	invalidate_asid(as->asid);
	sdk_smp_lock_acquire(&as_lock);
	l2_release_owner(as->slot + 1U);
	memset(alias_used[as->slot], 0, sizeof(alias_used[as->slot]));
	memset(alias_length[as->slot], 0, sizeof(alias_length[as->slot]));
	memset(as->l1, 0, sizeof(app_l1[0]));
	asid_generation[as->slot] = 0U;
	slot_live[as->slot] = 0U;
	as->l1 = NULL;
	as->active = 0U;
	sdk_smp_lock_release(&as_lock);
}

int zzapp_as_map_frames(zzapp_as_t *as, uint32_t virtual_base,
			const uint32_t *frames, uint32_t frame_count)
{
	uint32_t i;

	if (frame_count > (ZZAPP_AS_WINDOW_END - ZZAPP_AS_WINDOW_BASE) >>
	    SECTION_SHIFT ||
	    !app_range_ok(as, virtual_base, frame_count << SECTION_SHIFT,
			  1U << SECTION_SHIFT) || !frames)
		return -1;
	for (i = 0U; i < frame_count; i++)
		if ((frames[i] & ~SECTION_MASK) != 0U)
			return -1;
	sdk_smp_lock_acquire(&as_lock);
	for (i = 0U; i < frame_count; i++)
		as->l1[(virtual_base >> SECTION_SHIFT) + i] = frames[i] |
			DDR_SECTION | L1_NG;
	as_clean(as->l1, sizeof(app_l1[0]));
	as_sync();
	sdk_smp_lock_release(&as_lock);
	return 0;
}

int zzapp_as_unmap_frames(zzapp_as_t *as, uint32_t virtual_base,
                          uint32_t frame_count)
{
	uint32_t i;

	if (frame_count > (ZZAPP_AS_WINDOW_END - ZZAPP_AS_WINDOW_BASE) >>
	    SECTION_SHIFT ||
	    !app_range_ok(as, virtual_base, frame_count << SECTION_SHIFT,
			  1U << SECTION_SHIFT))
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	for (i = 0U; i < frame_count; i++)
		as->l1[(virtual_base >> SECTION_SHIFT) + i] = 0U;
	as_clean(as->l1, sizeof(app_l1[0]));
	as_sync();
	sdk_smp_lock_release(&as_lock);
	return 0;
}

int zzapp_as_alias_map_flags(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			     uint32_t flags, uint32_t *va)
{
	uint32_t offset;
	uint32_t virtual_base;
	uint32_t pages;
	int first;

	if (!as || !as->active || !va || (phys & (4096U - 1U)) != 0U ||
	    bytes == 0U || (bytes & (4096U - 1U)) != 0U ||
	    (uint64_t)phys + bytes > (uint64_t)UINT32_MAX + 1U ||
	    (flags != ZZAPP_AS_ALIAS_RO && flags != ZZAPP_AS_ALIAS_RW &&
	     flags != ZZAPP_AS_ALIAS_RX))
		return -1;
	for (offset = 0U; offset < bytes; offset += 4096U)
		if ((template_page((phys + offset) >> SECTION_SHIFT,
				   ((phys + offset) >> PAGE_SHIFT) & 0xffU) &
		     L2_TYPE) != L2_TYPE)
			return -1;
	pages = bytes >> PAGE_SHIFT;
	sdk_smp_lock_acquire(&as_lock);
	first = alias_first_free(as->slot, pages);
	if (first < 0) {
		sdk_smp_lock_release(&as_lock);
		return ZZAPP_AS_ENOMEM;
	}
	virtual_base = ZZAPP_AS_ALIAS_BASE + (uint32_t)first * 4096U;
	if (!grant_l2_available(as, virtual_base, virtual_base + bytes)) {
		sdk_smp_lock_release(&as_lock);
		return ZZAPP_AS_ENOMEM;
	}
	for (offset = 0U; offset < bytes; offset += 4096U) {
		uint32_t address = virtual_base + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
		uint32_t source = template_page((phys + offset) >> SECTION_SHIFT,
					      ((phys + offset) >> PAGE_SHIFT) & 0xffU);
		int index = ensure_l2(as, section);

		if (index < 0) {
			sdk_smp_lock_release(&as_lock);
			return index;
		}
		l2_pool[index][page] = (phys + offset) |
			(source & ~(PAGE_MASK | L2_AP_MASK | L2_NG | L2_XN)) |
			(flags == ZZAPP_AS_ALIAS_RW ? L2_AP_USER_RW : L2_AP_USER_RO) |
			L2_NG | (flags == ZZAPP_AS_ALIAS_RX ? 0U : L2_XN);
		as_clean(&l2_pool[index][page], sizeof(l2_pool[index][page]));
		alias_used[as->slot][(uint32_t)first + (offset >> PAGE_SHIFT)] = 1U;
	}
	alias_length[as->slot][first] = (uint16_t)pages;
	as_clean(as->l1, sizeof(app_l1[0]));
	as_sync();
	sdk_smp_lock_release(&as_lock);
	*va = virtual_base;
	return 0;
}

int zzapp_as_alias_map(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
		       int writable, uint32_t *va)
{
	return zzapp_as_alias_map_flags(as, phys, bytes,
		writable ? ZZAPP_AS_ALIAS_RW : ZZAPP_AS_ALIAS_RO, va);
}

int zzapp_as_alias_unmap(zzapp_as_t *as, uint32_t va, uint32_t bytes)
{
	uint32_t offset;
	uint32_t first;
	uint32_t pages;

	if (!alias_range_ok(as, va, bytes))
		return -1;
	first = (va - ZZAPP_AS_ALIAS_BASE) >> PAGE_SHIFT;
	pages = bytes >> PAGE_SHIFT;
	sdk_smp_lock_acquire(&as_lock);
	if (alias_length[as->slot][first] != pages) {
		sdk_smp_lock_release(&as_lock);
		return -1;
	}
	for (offset = 0U; offset < bytes; offset += 4096U) {
		uint32_t address = va + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
		int index = l2_index_for_descriptor(as->l1[section]);

		if (index < 0 || !alias_used[as->slot][first +
						 (offset >> PAGE_SHIFT)]) {
			sdk_smp_lock_release(&as_lock);
			return -1;
		}
		l2_pool[index][page] = template_page(section, page);
		as_clean(&l2_pool[index][page], sizeof(l2_pool[index][page]));
		alias_used[as->slot][first + (offset >> PAGE_SHIFT)] = 0U;
	}
	alias_length[as->slot][first] = 0U;
	as_clean(as->l1, sizeof(app_l1[0]));
	as_sync();
	invalidate_asid(as->asid);
	sdk_smp_lock_release(&as_lock);
	return 0;
}

uint32_t zzapp_as_alias_phys(const zzapp_as_t *as, uint32_t va)
{
	uint32_t page;
	uint32_t descriptor;
	int index;

	if (!as || !as->active || va < ZZAPP_AS_ALIAS_BASE ||
	    va >= ZZAPP_AS_ALIAS_END)
		return 0U;
	page = (va - ZZAPP_AS_ALIAS_BASE) >> PAGE_SHIFT;
	if (!alias_used[as->slot][page])
		return 0U;
	descriptor = as->l1[va >> SECTION_SHIFT];
	index = l2_index_for_descriptor(descriptor);
	if (index < 0)
		return 0U;
	descriptor = l2_pool[index][(va >> PAGE_SHIFT) & 0xffU];
	if ((descriptor & L2_TYPE) != L2_TYPE)
		return 0U;
	return (descriptor & PAGE_MASK) | (va & (4096U - 1U));
}


int zzapp_as_map_image(zzapp_as_t *as, uint32_t virtual_base,
			uint32_t physical_base, uint32_t bytes, uint32_t permissions)
{
	uint32_t offset;
	uint32_t attrs;

	if (!app_range_ok(as, virtual_base, bytes, 4096U) ||
	    (physical_base & 0xfffU) != 0U)
		return -1;
	if (permissions == ZZAPP_AS_IMAGE_RX)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RO | L2_NG;
	else if (permissions == ZZAPP_AS_IMAGE_RO)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RO | L2_NG | L2_XN;
	else if (permissions == ZZAPP_AS_IMAGE_RW)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RW | L2_NG | L2_XN;
	else
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	for (offset = 0U; offset < bytes; offset += 4096U) {
		uint32_t address = virtual_base + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
		int index = ensure_l2(as, section);

		if (index < 0) {
			sdk_smp_lock_release(&as_lock);
			return index;
		}
		l2_pool[index][page] = (physical_base + offset) | attrs;
		as_clean(&l2_pool[index][page], sizeof(l2_pool[index][page]));
	}
	as_clean(as->l1, sizeof(app_l1[0]));
	as_sync();
	sdk_smp_lock_release(&as_lock);
	return 0;
}

static void replace_memory_attributes(uint32_t *entry, uint32_t source)
{
	uint32_t descriptor = (*entry & ~L2_MEMORY_MASK) |
		(source & L2_MEMORY_MASK);

	replace_live_page(entry, descriptor);
}

void zzapp_as_mmu_page_changed(uint32_t section, uint32_t descriptor,
				const uint32_t *pages)
{
	uint32_t slot;

	if (section >= L1_ENTRIES)
		return;
	sdk_smp_lock_acquire(&as_lock);
	if ((descriptor & L1_TYPE_MASK) == L1_TABLE && pages) {
		if (template_l2(section, pages) != 0) {
			sdk_smp_lock_release(&as_lock);
			return;
		}
	} else {
		replace_live_descriptor(&template_l1[section],
					priv_global_section(descriptor));
	}
	for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; slot++) {
		uint32_t app_descriptor;
		int index;

		if (!slot_live[slot])
			continue;
		app_descriptor = app_l1[slot][section];
		index = l2_index_for_descriptor(app_descriptor);
		if (index < 0 || l2_owner[index] == 0U) {
			replace_live_descriptor(&app_l1[slot][section],
						template_l1[section]);
		} else {
			uint32_t page;

			/* A section-wide change (Xil_SetTlbAttributes) carries
			 * no page table; every page takes the section's type. */
			for (page = 0U; page < L2_ENTRIES; page++)
				replace_memory_attributes(&l2_pool[index][page],
					pages ? pages[page] :
					section_page_descriptor(section, page,
								descriptor));
			as_clean(l2_pool[index], sizeof(l2_pool[index]));
		}
		{
			uint32_t table;

			/* Alias VAs are unrelated to the mapped physical section,
			 * so find every owned L2 entry that names this section. */
			for (table = 0U; table < ZZAPP_AS_L2_TABLE_COUNT; table++) {
				uint32_t page;

				if (l2_owner[table] != slot + 1U)
					continue;
				for (page = 0U; page < L2_ENTRIES; page++) {
					uint32_t mapped = l2_pool[table][page];
					uint32_t physical = mapped & PAGE_MASK;
					uint32_t source;

					if ((mapped & L2_TYPE) != L2_TYPE ||
					    physical >> SECTION_SHIFT != section)
						continue;
					source = pages ?
						pages[(physical >> PAGE_SHIFT) & 0xffU] :
						section_page_descriptor(section,
							(physical >> PAGE_SHIFT) & 0xffU,
							descriptor);
					replace_memory_attributes(&l2_pool[table][page],
								  source);
				}
				as_clean(l2_pool[table], sizeof(l2_pool[table]));
			}
		}
	}
	as_clean(template_l1, sizeof(template_l1));
	as_clean(app_l1, sizeof(app_l1));
	as_sync();
	sdk_smp_lock_release(&as_lock);
}

void zzapp_as_switch(zzapp_as_t *as)
{
	if (!as || !as->active)
		return;
#ifndef MMU_PAGE_HOST_TEST
	{
		uint32_t asid = as->asid;
		uint32_t ttbr0 = (uint32_t)(uintptr_t)as->l1 | TTBR0_ATTR;

		if (asid_generation[as->slot] != as->generation) {
			invalidate_asid(asid);
			asid_generation[as->slot] = as->generation;
		}
		__asm__ __volatile__("dsb sy\n\t"
			"mcr p15, 0, %0, c13, c0, 1\n\tisb\n\t"
			"mcr p15, 0, %1, c2, c0, 0\n\tisb\n\t"
			"dsb sy\n\tdsb sy\n\tisb"
			: : "r" (asid), "r" (ttbr0) : "memory");
	}
#else
	(void)asid_generation;
#endif
}

void zzapp_as_switch_kernel(void)
{
#ifndef MMU_PAGE_HOST_TEST
	uint32_t ttbr0 = (uint32_t)(uintptr_t)template_l1 | TTBR0_ATTR;
	uint32_t asid = 0U;

	__asm__ __volatile__("dsb sy\n\t"
		"mcr p15, 0, %0, c13, c0, 1\n\tisb\n\t"
		"mcr p15, 0, %1, c2, c0, 0\n\tisb\n\t"
		"dsb sy\n\tdsb sy\n\tisb"
		: : "r" (asid), "r" (ttbr0) : "memory");
#endif
}

const uint32_t *zzapp_as_template(void)
{
	return template_l1;
}

const uint32_t *zzapp_as_l1(const zzapp_as_t *as)
{
	return as && as->active ? as->l1 : NULL;
}

const uint32_t *zzapp_as_l2_for(const zzapp_as_t *as, uint32_t address)
{
	uint32_t descriptor;
	int index;

	if (!as || !as->active)
		return NULL;
	descriptor = as->l1[address >> SECTION_SHIFT];
	index = l2_index_for_descriptor(descriptor);
	return index < 0 ? NULL : l2_pool[index];
}

#ifndef MMU_PAGE_HOST_TEST
/* Every Xil_SetTlbAttributes call, including those inside the BSP archive,
 * lands here (-Wl,--wrap in the Makefile). It rewrites a BSP section that
 * core 1, running on the template, would otherwise keep with its old memory
 * type. */
extern u32 MMUTable;
void __real_Xil_SetTlbAttributes(UINTPTR addr, u32 attrib);

void __wrap_Xil_SetTlbAttributes(UINTPTR addr, u32 attrib)
{
	uint32_t section = (uint32_t)addr >> SECTION_SHIFT;

	__real_Xil_SetTlbAttributes(addr, attrib);
	if (template_ready)
		zzapp_as_mmu_page_changed(section, (&MMUTable)[section], NULL);
}
#endif
