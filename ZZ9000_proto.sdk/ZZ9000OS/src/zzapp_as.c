/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "memorymap.h"
#include "mmu_page.h"
#include "zzapp_as.h"
#include "sdk_smp_lock.h"
#include "xil_cache.h"
#ifndef MMU_PAGE_HOST_TEST
#include "xil_types.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#endif

#define L1_ENTRIES 4096U
#define L2_ENTRIES 256U
#define SECTION_SHIFT 20U
#define PAGE_SHIFT 12U
#define LARGE_PAGE_SHIFT 16U
#define SECTION_MASK 0xfff00000U
#define PAGE_MASK 0xfffff000U
#define LARGE_PAGE_MASK 0xffff0000U
#define PAGE_BYTES (1U << PAGE_SHIFT)
#define LARGE_PAGE_BYTES (1U << LARGE_PAGE_SHIFT)
#define SECTION_BYTES (1U << SECTION_SHIFT)
#define L1_TYPE_MASK 3U
#define L1_SECTION 2U
#define L1_TABLE 1U
#define L1_TABLE_BASE_MASK 0xfffffc00U
#define L1_ATTR_MASK 0x000fffffU
#define L1_AP_MASK (3U << 10)
#define L1_AP_PRIV_RW (1U << 10)
#define L1_AP_USER_RO (2U << 10)
#define L1_AP_USER_RW (3U << 10)
#define L1_XN (1U << 4)
#define L1_NG (1U << 17)
#define L1_MEMORY_MASK ((7U << 12) | L2_C | L2_B | (1U << 16))
#define L2_SMALL 2U
#define L2_LARGE 1U
#define L2_TYPE_MASK 3U
#define L2_AP_MASK (3U << 4)
#define L2_AP_PRIV_RW (1U << 4)
#define L2_AP_USER_RW (3U << 4)
#define L2_AP_USER_RO (2U << 4)
#define L2_TEX_MASK (7U << 6)
#define L2_LARGE_TEX_MASK (7U << 12)
#define L2_C (1U << 3)
#define L2_B (1U << 2)
#define L2_S (1U << 10)
#define L2_AP2 (1U << 9)
#define L2_NG (1U << 11)
#define L2_SMALL_XN 1U
#define L2_LARGE_XN (1U << 15)
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
/* The virtual section each L2 table serves, recorded where the owning
 * path installs the table's only L1 pointer. */
static uint16_t l2_vsection[ZZAPP_AS_L2_TABLE_COUNT];
/* Reverse lookup for private L1 sections. Page tables carry their virtual
 * section in l2_vsection; lists retain every private section that maps a
 * changed BSP physical section without scanning the app L1. */
static uint16_t app_section_head[ZZAPP_AS_SLOT_COUNT][L1_ENTRIES];
static uint16_t app_section_next[ZZAPP_AS_SLOT_COUNT][L1_ENTRIES];
static uint8_t slot_live[ZZAPP_AS_SLOT_COUNT];
static uint32_t asid_generation[ZZAPP_AS_SLOT_COUNT];
static uint8_t alias_used[ZZAPP_AS_SLOT_COUNT][ZZAPP_AS_ALIAS_PAGE_COUNT];
static uint16_t alias_length[ZZAPP_AS_SLOT_COUNT][ZZAPP_AS_ALIAS_PAGE_COUNT];
static int template_ready;
static uint32_t firmware_text_start;
static uint32_t firmware_text_end;
static sdk_smp_lock_t as_lock = SDK_SMP_LOCK_INIT;
#ifdef MMU_PAGE_HOST_TEST
unsigned zzapp_as_test_asid_invalidations;
unsigned zzapp_as_test_tlb_mva_invalidations;
uint32_t zzapp_as_test_tlb_mva_pages[256];
unsigned zzapp_as_test_bp_invalidations;
#endif

static void as_clean(const void *address, uint32_t bytes)
{
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)address, bytes);
}

/* Retire every cached translation that covers [first, end): one
 * TLBIMVAAIS per step hits the section or page entry mapping the address,
 * in any ASID on both cores, so a mapping change no longer broadcasts the
 * whole-TLB invalidate and refill storm of the previous full sync.
 * Callers pick the step from what the outgoing descriptor could have
 * cached: 1 MiB for section entries, one page otherwise. */
static void as_invalidate_range(uint32_t first, uint32_t end, uint32_t step)
{
	uint32_t va;

	if (!step || first >= end)
		return;
#ifndef MMU_PAGE_HOST_TEST
	dsb();
	for (va = first; va < end; va += step)
		__asm__ __volatile__("mcr p15, 0, %0, c8, c3, 3" : : "r" (va)
			: "memory");
	dsb();
	isb();
#else
	for (va = first; va < end; va += step) {
		if (zzapp_as_test_tlb_mva_invalidations < 256U) {
			zzapp_as_test_tlb_mva_pages[
				zzapp_as_test_tlb_mva_invalidations] = va;
			xil_cache_mock_note_mva(
				zzapp_as_test_tlb_mva_invalidations);
		}
		zzapp_as_test_tlb_mva_invalidations++;
	}
#endif
}

/* The branch predictor is not tagged by context: the ARM ARM requires
 * invalidation whenever an executable mapping is created or replaced, so
 * VA reuse cannot fetch a stale branch.  Data-only paths do not call it. */
static void as_invalidate_predictors(void)
{
#ifndef MMU_PAGE_HOST_TEST
	dsb();
	mtcp(XREG_CP15_INVAL_BRANCH_ARRAY_IS, 0U);
	dsb();
	isb();
#else
	zzapp_as_test_bp_invalidations++;
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
	zzapp_as_test_asid_invalidations++;
#endif
}

/* Invalidation granularity for one live descriptor slot swap: page step
 * whenever either side is a page table, because the outgoing table may
 * have cached any of its 256 pages; one section-wide op otherwise. */
static uint32_t as_entry_granularity(uint32_t descriptor)
{
	return (descriptor & L1_TYPE_MASK) == L1_TABLE ?
		PAGE_BYTES : SECTION_BYTES;
}

static int l2_is_small(uint32_t descriptor)
{
	return (descriptor & L2_SMALL) != 0U;
}

static int l2_is_large(uint32_t descriptor)
{
	return (descriptor & L2_TYPE_MASK) == L2_LARGE;
}

static int l2_is_mapped(uint32_t descriptor)
{
	return l2_is_small(descriptor) || l2_is_large(descriptor);
}

static uint32_t l2_physical(uint32_t descriptor, uint32_t address)
{
	if (l2_is_large(descriptor))
		return (descriptor & LARGE_PAGE_MASK) |
			(address & (LARGE_PAGE_BYTES - 1U));
	return (descriptor & PAGE_MASK) | (address & (PAGE_BYTES - 1U));
}

static uint32_t l2_memory_attributes(uint32_t descriptor)
{
	uint32_t attrs = descriptor & (L2_C | L2_B | L2_S);

	if (l2_is_large(descriptor))
		return attrs | ((descriptor & L2_LARGE_TEX_MASK) >> 6);
	return attrs | (descriptor & L2_TEX_MASK);
}

static uint32_t l2_with_memory_attributes(uint32_t descriptor,
					   uint32_t attrs)
{
	if (l2_is_large(descriptor))
		return (descriptor & ~(L2_LARGE_TEX_MASK | L2_C | L2_B | L2_S)) |
			((attrs & L2_TEX_MASK) << 6) |
			(attrs & (L2_C | L2_B | L2_S));
	return (descriptor & ~L2_MEMORY_MASK) | attrs;
}

static uint32_t large_page_descriptor(uint32_t physical, uint32_t attrs)
{
	return (physical & LARGE_PAGE_MASK) |
		(attrs & ~(PAGE_MASK | L2_TYPE_MASK | L2_SMALL_XN |
			   L2_TEX_MASK | L2_LARGE_TEX_MASK)) |
		((attrs & L2_TEX_MASK) << 6) |
		((attrs & L2_SMALL_XN) ? L2_LARGE_XN : 0U) | L2_LARGE;
}

/* Break-before-make on one live descriptor: the zeroed entry is cleaned
 * and its cached translation retired before the replacement is
 * published. */
static void replace_live_descriptor(uint32_t va, uint32_t *entry,
				    uint32_t descriptor)
{
	uint32_t step;

	if (*entry == descriptor)
		return;
	step = as_entry_granularity(*entry);
	if (as_entry_granularity(descriptor) < step)
		step = as_entry_granularity(descriptor);
	va &= ~(SECTION_BYTES - 1U);
	if (*entry == 0U) {
		*entry = descriptor;
		as_clean(entry, sizeof(*entry));
		if (step == SECTION_BYTES)
			as_invalidate_range(va, va + SECTION_BYTES, step);
		return;
	}
	if (descriptor == 0U) {
		*entry = 0U;
		as_clean(entry, sizeof(*entry));
		as_invalidate_range(va, va + SECTION_BYTES, step);
		return;
	}
	*entry = 0U;
	as_clean(entry, sizeof(*entry));
	as_invalidate_range(va, va + SECTION_BYTES, step);
	*entry = descriptor;
	as_clean(entry, sizeof(*entry));
	as_invalidate_range(va, va + SECTION_BYTES, step);
}

/* TLBIMVAAIS invalidates entries that would translate the supplied VA.
 * One VA therefore retires the whole 64 KiB short-descriptor large page;
 * the MVA is its aligned base. */
static uint32_t l2_granularity(uint32_t descriptor)
{
	return l2_is_large(descriptor) ? LARGE_PAGE_BYTES : PAGE_BYTES;
}

static void replace_live_page(uint32_t va, uint32_t *entry, uint32_t descriptor)
{
	uint32_t step;

	if (*entry == descriptor)
		return;
	step = l2_granularity(*entry);
	if (l2_granularity(descriptor) > step)
		step = l2_granularity(descriptor);
	va &= ~(step - 1U);
	if (*entry == 0U || descriptor == 0U) {
		*entry = descriptor;
		as_clean(entry, sizeof(*entry));
		as_invalidate_range(va, va + step, step);
		return;
	}
	*entry = 0U;
	as_clean(entry, sizeof(*entry));
	as_invalidate_range(va, va + step, step);
	*entry = descriptor;
	as_clean(entry, sizeof(*entry));
	as_invalidate_range(va, va + step, step);
}

static void split_large_page(uint32_t va, uint32_t *table, uint32_t page)
{
	uint32_t first = page & ~(LARGE_PAGE_BYTES / PAGE_BYTES - 1U);
	uint32_t large = table[first];
	uint32_t attrs = large & ~(LARGE_PAGE_MASK | L2_TYPE_MASK |
		L2_LARGE_XN | L2_LARGE_TEX_MASK);
	uint32_t i;

	attrs |= (large & L2_LARGE_TEX_MASK) >> 6;
	if (large & L2_LARGE_XN)
		attrs |= L2_SMALL_XN;
	va &= ~(LARGE_PAGE_BYTES - 1U);
	for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
		table[first + i] = 0U;
	as_clean(&table[first], LARGE_PAGE_BYTES / PAGE_BYTES *
		 sizeof(table[first]));
	as_invalidate_range(va, va + LARGE_PAGE_BYTES, LARGE_PAGE_BYTES);
	for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
		table[first + i] = ((large & LARGE_PAGE_MASK) +
			i * PAGE_BYTES) | attrs | L2_SMALL;
	as_clean(&table[first], LARGE_PAGE_BYTES / PAGE_BYTES *
		 sizeof(table[first]));
	as_invalidate_range(va, va + LARGE_PAGE_BYTES, LARGE_PAGE_BYTES);
}

static void replace_large_page(uint32_t va, uint32_t *table, uint32_t page,
			       uint32_t descriptor)
{
	uint32_t first = page & ~(LARGE_PAGE_BYTES / PAGE_BYTES - 1U);
	uint32_t i;
	int empty = 1;
	int old_large;

	for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
		if (table[first + i] != descriptor)
			break;
	if (i == LARGE_PAGE_BYTES / PAGE_BYTES)
		return;
	old_large = l2_is_large(table[first]);
	for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
		if (table[first + i] != 0U)
			empty = 0;
	va &= ~(LARGE_PAGE_BYTES - 1U);
	if (!empty) {
		for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
			table[first + i] = 0U;
		as_clean(&table[first], LARGE_PAGE_BYTES / PAGE_BYTES *
			 sizeof(table[first]));
		as_invalidate_range(va, va + LARGE_PAGE_BYTES,
				    old_large ? LARGE_PAGE_BYTES : PAGE_BYTES);
	}
	for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
		table[first + i] = descriptor;
	as_clean(&table[first], LARGE_PAGE_BYTES / PAGE_BYTES *
		 sizeof(table[first]));
	as_invalidate_range(va, va + LARGE_PAGE_BYTES, LARGE_PAGE_BYTES);
}

static uint32_t priv_global_section(uint32_t descriptor)
{
	if ((descriptor & L1_TYPE_MASK) != L1_SECTION)
		return descriptor;
	return (descriptor & ~(L1_AP_MASK | L1_NG)) | L1_AP_PRIV_RW;
}

static uint32_t priv_global_page(uint32_t descriptor)
{
	if (!l2_is_mapped(descriptor))
		return descriptor;
	return (descriptor & ~(L2_AP_MASK | L2_NG)) | L2_AP_PRIV_RW;
}

static uint32_t mapped_section_page_descriptor(uint32_t physical_section,
						uint32_t page,
						uint32_t descriptor)
{
	uint32_t attrs = 0U;

	attrs |= ((descriptor >> 12) & 7U) << 6;
	attrs |= descriptor & (L2_C | L2_B);
	attrs |= ((descriptor >> 16) & 1U) << 10;
	attrs |= ((descriptor >> 15) & 1U) << 9;
	attrs |= ((descriptor >> 10) & 3U) << 4;
	attrs |= ((descriptor >> 17) & 1U) << 11;
	attrs |= (descriptor >> 4) & L2_SMALL_XN;
	return physical_section | (page << PAGE_SHIFT) | attrs | L2_SMALL;
}

static uint32_t section_page_descriptor(uint32_t section, uint32_t page,
					uint32_t descriptor)
{
	return mapped_section_page_descriptor(section << SECTION_SHIFT, page,
					      descriptor);
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

static void remember_app_section(zzapp_as_t *as, uint32_t virtual_section,
				 uint32_t old_descriptor,
				 uint32_t new_descriptor)
{
	uint32_t old_physical = old_descriptor >> SECTION_SHIFT;
	uint32_t new_physical = new_descriptor >> SECTION_SHIFT;
	uint16_t *link;

	if ((old_descriptor & (L1_TYPE_MASK | L1_NG)) ==
	    (L1_SECTION | L1_NG)) {
		link = &app_section_head[as->slot][old_physical];
		while (*link != 0xffffU) {
			if (*link == virtual_section) {
				*link = app_section_next[as->slot][*link];
				break;
			}
			link = &app_section_next[as->slot][*link];
		}
		app_section_next[as->slot][virtual_section] = 0xffffU;
	}
	if ((new_descriptor & (L1_TYPE_MASK | L1_NG)) ==
	    (L1_SECTION | L1_NG)) {
		app_section_next[as->slot][virtual_section] =
			app_section_head[as->slot][new_physical];
		app_section_head[as->slot][new_physical] =
			(uint16_t)virtual_section;
	}
}

static int l2_alloc(uint8_t owner)
{
	uint32_t i;

	for (i = 0U; i < ZZAPP_AS_L2_TABLE_COUNT; i++) {
		if (l2_owner[i] == 0xffU) {
			l2_owner[i] = owner;
			l2_vsection[i] = 0xffffU;
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
			l2_vsection[i] = 0xffffU;
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

static void populate_section_pages(uint32_t *table, uint32_t physical_base,
				   uint32_t descriptor)
{
	uint32_t page;

	for (page = 0U; page < L2_ENTRIES; page++)
		table[page] = priv_global_page(mapped_section_page_descriptor(
			physical_base, page, descriptor));
}

static void populate_app_section_pages(uint32_t *table,
				       uint32_t physical_base,
				       uint32_t descriptor)
{
	uint32_t page;

	for (page = 0U; page < L2_ENTRIES; page++)
		table[page] = mapped_section_page_descriptor(physical_base, page,
						      descriptor);
}

static int section_covers_firmware_text(uint32_t section)
{
	uint32_t base = section << SECTION_SHIFT;
	uint32_t end = base + (1U << SECTION_SHIFT);

	return firmware_text_end > firmware_text_start &&
		end > firmware_text_start && base < firmware_text_end;
}

static int template_l2(uint32_t section, const uint32_t *pages)
{
	int index = l2_index_for_descriptor(template_l1[section]);
	int fresh = 0;
	uint32_t page;
	uint32_t descriptor;

	if (index < 0 || l2_owner[index] != 0U) {
		/* The template is already walked. Clearing a section that
		 * contains this code faults the instruction fetch. Those
		 * sections were split to L2 before the template went live. */
		if (template_ready && section_covers_firmware_text(section))
			return ZZAPP_AS_ENOMEM;
		index = l2_alloc(0U);
		if (index < 0)
			return ZZAPP_AS_ENOMEM;
		fresh = 1;
	}
	for (page = 0U; page < L2_ENTRIES; page++)
		l2_pool[index][page] = priv_global_page(pages[page]);
	as_clean(l2_pool[index], sizeof(l2_pool[index]));
	if (!fresh)
		return 0;
	descriptor = l2_descriptor(l2_pool[index], 15U << 5);
	if (!template_ready)
		template_l1[section] = descriptor;
	else
		replace_live_descriptor(section << SECTION_SHIFT,
					&template_l1[section], descriptor);
	return 0;
}

/* Template is not walked yet: a direct store cannot unmap executing code. */
static int split_text_section(uint32_t section)
{
	uint32_t descriptor = template_l1[section];
	int index;

	if ((descriptor & L1_TYPE_MASK) != L1_SECTION)
		return 0;
	index = l2_alloc(0U);
	if (index < 0)
		return -1;
	populate_section_pages(l2_pool[index], section << SECTION_SHIFT,
			       descriptor);
	as_clean(l2_pool[index], sizeof(l2_pool[index]));
	template_l1[section] = l2_descriptor(l2_pool[index], 15U << 5);
	return 0;
}

void zzapp_as_set_firmware_text(uint32_t start, uint32_t end)
{
	firmware_text_start = start;
	firmware_text_end = end;
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
		as_clean(l2_pool[new_index], sizeof(l2_pool[new_index]));
		replace_live_descriptor(section << SECTION_SHIFT,
					&as->l1[section],
					l2_descriptor(l2_pool[new_index],
						      descriptor));
		remember_app_section(as, section, descriptor, as->l1[section]);
		l2_vsection[new_index] = (uint16_t)section;
		return new_index;
	}
	new_index = l2_alloc(as->slot + 1U);
	if (new_index < 0)
		return ZZAPP_AS_ENOMEM;
	if ((descriptor & L1_TYPE_MASK) == L1_SECTION &&
	    (descriptor & L1_NG) != 0U)
		populate_app_section_pages(l2_pool[new_index],
					   descriptor & SECTION_MASK,
					   descriptor);
	else if ((descriptor & L1_TYPE_MASK) == L1_SECTION)
		populate_section_pages(l2_pool[new_index], descriptor & SECTION_MASK,
				       descriptor);
	as_clean(l2_pool[new_index], sizeof(l2_pool[new_index]));
	replace_live_descriptor(section << SECTION_SHIFT, &as->l1[section],
				l2_descriptor(l2_pool[new_index],
					      descriptor ? descriptor : 15U << 5));
	remember_app_section(as, section, descriptor, as->l1[section]);
	l2_vsection[new_index] = (uint16_t)section;
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
	    address < ZZAPP_AS_ALIAS_BASE || address >= ZZAPP_AS_ALIAS_END ||
	    bytes > ZZAPP_AS_ALIAS_END - address)
		return 0;
	return 1;
}

static int alias_first_free(uint32_t slot, uint32_t pages, uint32_t alignment)
{
	uint32_t first;
	uint32_t page;

	if (pages == 0U || pages > ZZAPP_AS_ALIAS_PAGE_COUNT)
		return -1;
	for (first = 0U; first <= ZZAPP_AS_ALIAS_PAGE_COUNT - pages; first++) {
		if (((ZZAPP_AS_ALIAS_BASE + first * 4096U) & (alignment - 1U)) != 0U)
			continue;
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
	memset(app_section_head, 0xff, sizeof(app_section_head));
	memset(app_section_next, 0xff, sizeof(app_section_next));
	memset(slot_live, 0, sizeof(slot_live));
	memset(asid_generation, 0, sizeof(asid_generation));
	memset(alias_used, 0, sizeof(alias_used));
	memset(alias_length, 0, sizeof(alias_length));
	memset(l2_owner, 0xff, sizeof(l2_owner));
	memset(l2_vsection, 0xff, sizeof(l2_vsection));
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
	if (firmware_text_end > firmware_text_start) {
		uint32_t first = firmware_text_start >> SECTION_SHIFT;
		uint32_t last = (firmware_text_end - 1U) >> SECTION_SHIFT;

		for (section = first; section <= last && section < L1_ENTRIES;
		     section++)
			(void)split_text_section(section);
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
	memset(app_section_head[slot], 0xff, sizeof(app_section_head[slot]));
	memset(app_section_next[slot], 0xff, sizeof(app_section_next[slot]));
	slot_live[slot] = 1U;
	asid_generation[slot] = generation;
	as_clean(as->l1, sizeof(app_l1[slot]));
	sdk_smp_lock_release(&as_lock);
	return 0;
}

void zzapp_as_destroy(zzapp_as_t *as)
{
	if (!as || !as->active)
		return;
	/* Core 0 keeps the BSP table. The template's domain 15 is Manager in
	 * core 0's DACR, which would drop the BSP's read-only text protection. */
	if (smp_cpu_id() == 1)
		zzapp_as_switch_kernel();
	invalidate_asid(as->asid);
	sdk_smp_lock_acquire(&as_lock);
	l2_release_owner(as->slot + 1U);
	memset(alias_used[as->slot], 0, sizeof(alias_used[as->slot]));
	memset(alias_length[as->slot], 0, sizeof(alias_length[as->slot]));
	memset(app_section_head[as->slot], 0xff,
	       sizeof(app_section_head[as->slot]));
	memset(app_section_next[as->slot], 0xff,
	       sizeof(app_section_next[as->slot]));
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
	{
		uint32_t virtual_section =
			(virtual_base >> SECTION_SHIFT) + i;
		uint32_t old_descriptor = as->l1[virtual_section];

		replace_live_descriptor(virtual_base + i * SECTION_BYTES,
					&as->l1[virtual_section],
					frames[i] | DDR_SECTION | L1_NG | L1_XN);
		remember_app_section(as, virtual_section, old_descriptor,
				     as->l1[virtual_section]);
	}
	as_clean(as->l1, sizeof(app_l1[0]));
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
	{
		uint32_t virtual_section =
			(virtual_base >> SECTION_SHIFT) + i;
		uint32_t old_descriptor = as->l1[virtual_section];

		replace_live_descriptor(virtual_base + i * SECTION_BYTES,
					&as->l1[virtual_section], 0U);
		remember_app_section(as, virtual_section, old_descriptor, 0U);
	}
	as_clean(as->l1, sizeof(app_l1[0]));
	sdk_smp_lock_release(&as_lock);
	return 0;
}

static int alias_large_page_compatible(uint32_t physical,
				       uint32_t source)
{
	uint32_t i;
	uint32_t attrs = l2_memory_attributes(source);

	for (i = 1U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++) {
		uint32_t other = template_page((physical + i * PAGE_BYTES) >>
					      SECTION_SHIFT,
					      ((physical + i * PAGE_BYTES) >>
					       PAGE_SHIFT) & 0xffU);

		if (!l2_is_small(other) || l2_memory_attributes(other) != attrs)
			return 0;
	}
	return 1;
}

int zzapp_as_alias_map_flags_aligned(zzapp_as_t *as, uint32_t phys,
				     uint32_t bytes, uint32_t flags,
				     uint32_t alignment, uint32_t *va)
{
	uint32_t offset;
	uint32_t virtual_base;
	uint32_t pages;
	int first;

	if (bytes > ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE)
		return ZZAPP_AS_ENOMEM;
	if (!as || !as->active || !va || (phys & (PAGE_BYTES - 1U)) != 0U ||
	    bytes == 0U || (bytes & (PAGE_BYTES - 1U)) != 0U ||
	    alignment < PAGE_BYTES || (alignment & (alignment - 1U)) != 0U ||
	    (alignment & (PAGE_BYTES - 1U)) != 0U ||
	    (uint64_t)phys + bytes > (uint64_t)UINT32_MAX + 1U ||
	    (flags != ZZAPP_AS_ALIAS_RO && flags != ZZAPP_AS_ALIAS_RW &&
	     flags != ZZAPP_AS_ALIAS_RX))
		return -1;
	for (offset = 0U; offset < bytes; offset += PAGE_BYTES)
		if (!l2_is_small(template_page((phys + offset) >> SECTION_SHIFT,
					       ((phys + offset) >>
						PAGE_SHIFT) & 0xffU)))
			return -1;
	pages = bytes >> PAGE_SHIFT;
	sdk_smp_lock_acquire(&as_lock);
	first = alias_first_free(as->slot, pages, alignment);
	if (first < 0) {
		sdk_smp_lock_release(&as_lock);
		return ZZAPP_AS_ENOMEM;
	}
	virtual_base = ZZAPP_AS_ALIAS_BASE + (uint32_t)first * PAGE_BYTES;
	if (!grant_l2_available(as, virtual_base, virtual_base + bytes)) {
		sdk_smp_lock_release(&as_lock);
		return ZZAPP_AS_ENOMEM;
	}
	for (offset = 0U; offset < bytes;) {
		uint32_t address = virtual_base + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
		uint32_t source = template_page((phys + offset) >> SECTION_SHIFT,
					      ((phys + offset) >>
					       PAGE_SHIFT) & 0xffU);
		uint32_t attrs = source & ~(PAGE_MASK | L2_AP_MASK | L2_NG |
			L2_TYPE_MASK | L2_SMALL_XN | L2_LARGE_XN);
		uint32_t mapped_bytes;
		int index = ensure_l2(as, section);

		if (index < 0) {
			sdk_smp_lock_release(&as_lock);
			return index;
		}
		attrs |= flags == ZZAPP_AS_ALIAS_RW ? L2_AP_USER_RW :
			L2_AP_USER_RO;
		attrs |= L2_NG;
		if (flags != ZZAPP_AS_ALIAS_RX)
			attrs |= L2_SMALL_XN;
		mapped_bytes = ((address & (LARGE_PAGE_BYTES - 1U)) == 0U &&
				((phys + offset) & (LARGE_PAGE_BYTES - 1U)) == 0U &&
				bytes - offset >= LARGE_PAGE_BYTES &&
				alias_large_page_compatible(phys + offset, source)) ?
			LARGE_PAGE_BYTES : PAGE_BYTES;
		if (mapped_bytes == LARGE_PAGE_BYTES) {
			uint32_t large = large_page_descriptor(phys + offset, attrs);
			uint32_t i;

			for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
				l2_pool[index][page + i] = large;
			as_clean(&l2_pool[index][page],
				 LARGE_PAGE_BYTES / PAGE_BYTES *
				 sizeof(l2_pool[index][page]));
			as_invalidate_range(address, address + LARGE_PAGE_BYTES,
					    LARGE_PAGE_BYTES);
		} else {
			l2_pool[index][page] = (phys + offset) | attrs | L2_SMALL;
			as_clean(&l2_pool[index][page], sizeof(l2_pool[index][page]));
			as_invalidate_range(address, address + PAGE_BYTES, PAGE_BYTES);
		}
		for (uint32_t i = 0U; i < mapped_bytes / PAGE_BYTES; i++)
			alias_used[as->slot][(uint32_t)first +
				((offset >> PAGE_SHIFT) + i)] = 1U;
		offset += mapped_bytes;
	}
	alias_length[as->slot][first] = (uint16_t)pages;
	as_clean(as->l1, sizeof(app_l1[0]));
	if (flags == ZZAPP_AS_ALIAS_RX)
		as_invalidate_predictors();
	sdk_smp_lock_release(&as_lock);
	*va = virtual_base;
	return 0;
}

int zzapp_as_alias_map_aligned(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			       int writable, uint32_t alignment, uint32_t *va)
{
	return zzapp_as_alias_map_flags_aligned(as, phys, bytes,
		writable ? ZZAPP_AS_ALIAS_RW : ZZAPP_AS_ALIAS_RO, alignment, va);
}

int zzapp_as_alias_map_flags(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			     uint32_t flags, uint32_t *va)
{
	return zzapp_as_alias_map_flags_aligned(as, phys, bytes, flags, 4096U,
						va);
}

int zzapp_as_alias_map(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
		       int writable, uint32_t *va)
{
	return zzapp_as_alias_map_aligned(as, phys, bytes, writable, 4096U, va);
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
	for (offset = 0U; offset < bytes;) {
		uint32_t address = va + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
		uint32_t mapped_bytes = PAGE_BYTES;
		int index = l2_index_for_descriptor(as->l1[section]);

		if (index < 0 || !alias_used[as->slot][first +
						 (offset >> PAGE_SHIFT)]) {
			sdk_smp_lock_release(&as_lock);
			return -1;
		}
		if (l2_is_large(l2_pool[index][page])) {
			uint32_t i;

			if ((address & (LARGE_PAGE_BYTES - 1U)) != 0U ||
			    bytes - offset < LARGE_PAGE_BYTES) {
				sdk_smp_lock_release(&as_lock);
				return -1;
			}
			mapped_bytes = LARGE_PAGE_BYTES;
			for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
				l2_pool[index][page + i] = 0U;
			as_clean(&l2_pool[index][page],
				 LARGE_PAGE_BYTES / PAGE_BYTES *
				 sizeof(l2_pool[index][page]));
			as_invalidate_range(address, address + LARGE_PAGE_BYTES,
					    LARGE_PAGE_BYTES);
			for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++) {
				l2_pool[index][page + i] =
					template_page(section, page + i);
				alias_used[as->slot][first +
					((offset >> PAGE_SHIFT) + i)] = 0U;
			}
			as_clean(&l2_pool[index][page],
				 LARGE_PAGE_BYTES / PAGE_BYTES *
				 sizeof(l2_pool[index][page]));
			as_invalidate_range(address, address + LARGE_PAGE_BYTES,
					    LARGE_PAGE_BYTES);
		} else {
			replace_live_page(address, &l2_pool[index][page],
					  template_page(section, page));
			alias_used[as->slot][first + (offset >> PAGE_SHIFT)] = 0U;
		}
		offset += mapped_bytes;
	}
	alias_length[as->slot][first] = 0U;
	as_clean(as->l1, sizeof(app_l1[0]));
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
	return l2_is_mapped(descriptor) ? l2_physical(descriptor, va) : 0U;
}


static int section_has_owned_l2(const zzapp_as_t *as, uint32_t section)
{
	int index = l2_index_for_descriptor(as->l1[section]);

	return index >= 0 && l2_owner[index] == as->slot + 1U;
}

static uint32_t image_section_attrs(uint32_t permissions)
{
	uint32_t attrs = (DDR_SECTION & ~(L1_AP_MASK | L1_NG | L1_XN)) |
		L1_NG;

	attrs |= permissions == ZZAPP_AS_IMAGE_RX ||
		 permissions == ZZAPP_AS_IMAGE_RO ? L1_AP_USER_RO :
		 L1_AP_USER_RW;
	if (permissions == ZZAPP_AS_IMAGE_RO ||
	    permissions == ZZAPP_AS_IMAGE_RW)
		attrs |= L1_XN;
	return attrs;
}

int zzapp_as_map_image(zzapp_as_t *as, uint32_t virtual_base,
			uint32_t physical_base, uint32_t bytes, uint32_t permissions)
{
	uint32_t offset;
	uint32_t attrs;

	if (!app_range_ok(as, virtual_base, bytes, PAGE_BYTES) ||
	    (physical_base & (PAGE_BYTES - 1U)) != 0U)
		return -1;
	if (permissions == ZZAPP_AS_IMAGE_RX)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RO | L2_NG;
	else if (permissions == ZZAPP_AS_IMAGE_RO)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RO | L2_NG |
			L2_SMALL_XN;
	else if (permissions == ZZAPP_AS_IMAGE_RW)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RW | L2_NG |
			L2_SMALL_XN;
	else if (permissions == ZZAPP_AS_IMAGE_RWX)
		attrs = (DDR_PAGE & ~L2_AP_MASK) | L2_AP_USER_RW | L2_NG;
	else
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	for (offset = 0U; offset < bytes;) {
		uint32_t address = virtual_base + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t mapped_bytes = PAGE_BYTES;

		if ((address & (SECTION_BYTES - 1U)) == 0U &&
		    ((physical_base + offset) & (SECTION_BYTES - 1U)) == 0U &&
		    bytes - offset >= SECTION_BYTES &&
		    !section_has_owned_l2(as, section)) {
			uint32_t old_descriptor = as->l1[section];

			replace_live_descriptor(address, &as->l1[section],
						(physical_base + offset) |
						image_section_attrs(permissions));
			remember_app_section(as, section, old_descriptor,
					     as->l1[section]);
			offset += SECTION_BYTES;
			continue;
		}
		{
			uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
			int index = ensure_l2(as, section);

			if (index < 0) {
				sdk_smp_lock_release(&as_lock);
				return index;
			}
			if ((address & (LARGE_PAGE_BYTES - 1U)) == 0U &&
			    ((physical_base + offset) &
			     (LARGE_PAGE_BYTES - 1U)) == 0U &&
			    bytes - offset >= LARGE_PAGE_BYTES) {
				replace_large_page(address, l2_pool[index], page,
					large_page_descriptor(physical_base + offset,
							      attrs));
				mapped_bytes = LARGE_PAGE_BYTES;
			} else {
				if (l2_is_large(l2_pool[index][page]))
					split_large_page(address, l2_pool[index], page);
				replace_live_page(address, &l2_pool[index][page],
						  (physical_base + offset) |
						  attrs | L2_SMALL);
			}
		}
		offset += mapped_bytes;
	}
	as_clean(as->l1, sizeof(app_l1[0]));
	if (permissions == ZZAPP_AS_IMAGE_RX ||
	    permissions == ZZAPP_AS_IMAGE_RWX)
		as_invalidate_predictors();
	sdk_smp_lock_release(&as_lock);
	return 0;
}

static void replace_memory_attributes(uint32_t va, uint32_t *entry,
				      uint32_t source)
{
	uint32_t descriptor = l2_with_memory_attributes(*entry,
		l2_memory_attributes(source));

	replace_live_page(va, entry, descriptor);
}

static void propagate_l2_attributes(int index, uint32_t virtual_section,
				    uint32_t section, uint32_t descriptor,
				    const uint32_t *pages)
{
	uint32_t page;
	uint32_t virtual_base = virtual_section << SECTION_SHIFT;

	for (page = 0U; page < L2_ENTRIES; page++) {
		uint32_t address = virtual_base + page * PAGE_BYTES;
		uint32_t mapped = l2_pool[index][page];
		uint32_t physical;
		uint32_t source;

		if (!l2_is_mapped(mapped))
			continue;
		physical = l2_physical(mapped, address);
		if (physical >> SECTION_SHIFT != section)
			continue;
		source = pages ?
			pages[(physical >> PAGE_SHIFT) & 0xffU] :
			section_page_descriptor(section,
				(physical >> PAGE_SHIFT) & 0xffU, descriptor);
		if (l2_is_large(mapped) &&
		    l2_memory_attributes(mapped) != l2_memory_attributes(source))
			split_large_page(address, l2_pool[index], page);
		replace_memory_attributes(address, &l2_pool[index][page], source);
	}
	as_clean(l2_pool[index], sizeof(l2_pool[index]));
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
		replace_live_descriptor(section << SECTION_SHIFT,
					&template_l1[section],
					priv_global_section(descriptor));
	}
	for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; slot++) {
		uint32_t virtual_section;
		uint32_t table;

		if (!slot_live[slot])
			continue;
		{
			uint32_t app_descriptor = app_l1[slot][section];
			int index = l2_index_for_descriptor(app_descriptor);

			if (!((app_descriptor & L1_TYPE_MASK) == L1_SECTION &&
			      (app_descriptor & L1_NG) != 0U) &&
			    !(index >= 0 && l2_owner[index] == slot + 1U))
				replace_live_descriptor(section << SECTION_SHIFT,
							&app_l1[slot][section],
							template_l1[section]);
		}
		virtual_section = app_section_head[slot][section];
		while (virtual_section != 0xffffU) {
			uint32_t next_section =
				app_section_next[slot][virtual_section];
			uint32_t app_descriptor =
				app_l1[slot][virtual_section];

			if ((app_descriptor & (L1_TYPE_MASK | L1_NG)) ==
			    (L1_SECTION | L1_NG) &&
			    (app_descriptor & SECTION_MASK) ==
			    section << SECTION_SHIFT) {
				if (!pages)
					replace_live_descriptor(
						virtual_section << SECTION_SHIFT,
						&app_l1[slot][virtual_section],
						(app_descriptor & ~L1_MEMORY_MASK) |
						(descriptor & L1_MEMORY_MASK));
				else {
					zzapp_as_t as = {
						.l1 = app_l1[slot],
						.asid = (uint16_t)(slot + 1U),
						.slot = (uint8_t)slot
					};

					(void)ensure_l2(&as, virtual_section);
				}
			}
			virtual_section = next_section;
		}
		for (table = 0U; table < ZZAPP_AS_L2_TABLE_COUNT; table++) {
			if (l2_owner[table] != slot + 1U ||
			    l2_vsection[table] == 0xffffU)
				continue;
			propagate_l2_attributes((int)table, l2_vsection[table],
						 section, descriptor, pages);
		}
	}
	as_clean(template_l1, sizeof(template_l1));
	as_clean(app_l1, sizeof(app_l1));
	as_invalidate_range(section << SECTION_SHIFT,
			    (section << SECTION_SHIFT) + SECTION_BYTES,
			    PAGE_BYTES);
	as_invalidate_predictors();
	sdk_smp_lock_release(&as_lock);
}

void zzapp_as_switch(zzapp_as_t *as)
{
	if (!as || !as->active)
		return;
	if (asid_generation[as->slot] != as->generation) {
		invalidate_asid(as->asid);
		asid_generation[as->slot] = as->generation;
	}
#ifndef MMU_PAGE_HOST_TEST
	{
		uint32_t asid = as->asid;
		uint32_t ttbr0 = (uint32_t)(uintptr_t)as->l1 | TTBR0_ATTR;

		__asm__ __volatile__("dsb sy\n\t"
			"mcr p15, 0, %0, c13, c0, 1\n\tisb\n\t"
			"mcr p15, 0, %1, c2, c0, 0\n\tisb\n\t"
			"dsb sy\n\tisb"
			: : "r" (asid), "r" (ttbr0) : "memory");
	}
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
		"dsb sy\n\tisb"
		: : "r" (asid), "r" (ttbr0) : "memory");
#else
	zzapp_as_test_kernel_switches++;
#endif
}

void zzapp_as_reset_lock(void)
{
	sdk_smp_lock_reset(&as_lock);
}

#ifdef MMU_PAGE_HOST_TEST
unsigned zzapp_as_test_kernel_switches;

void zzapp_as_test_orphan_lock(void)
{
	as_lock.owner = 1;
	as_lock.depth = 1U;
	as_lock.raw = 1U;
}

int zzapp_as_test_lock_held(void)
{
	return as_lock.owner >= 0 || as_lock.depth != 0U || as_lock.raw != 0U;
}
#endif

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
