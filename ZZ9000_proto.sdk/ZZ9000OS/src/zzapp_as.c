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

#define L2_OWNER_FREE     0xffffU
#define L2_OWNER_TEMPLATE 0xfffeU
#define L2_NONE           0xffffU
#define PID_LIVE          0x01U
#define PID_RESERVED      0x02U
#define PID_FAULTED       0x08U
#define PID_NOTIFIED      0x10U

/*
 * The table pool: kernel template L1, one L1 per record index, and one shared
 * array of 1 KiB L2 tables, laid out by zzapp_as_pool_init() in memory the
 * caller owns (the record arena). Each L2 table has an owner mark (a record
 * index, the template, or free) and a link: a free table points to the next
 * free one, an owned table to the next table of the same owner. Every counter
 * below can be rebuilt from the owner marks (zzapp_as_reconcile).
 */
static uint32_t *template_l1;
static uint8_t *l1_base;
static uint32_t (*l2_pool)[L2_ENTRIES];
static uint16_t *l2_owner;
static uint16_t *l2_next;
/* The virtual section each L2 table serves, recorded where the owning
 * path installs the table's only L1 pointer. */
static uint16_t *l2_vsection;
static uint32_t pool_apps;
static uint32_t pool_l2_total;
static uint32_t l2_free_head = L2_NONE;
static uint32_t l2_free_count;
static uint32_t l2_template_used;
/* Tables promised to admitted records, and the part of that promise no
 * allocation has used yet. free - reserved_unused is what an allocation
 * outside any reserve may still take. */
static uint32_t l2_reserved_total;
static uint32_t l2_reserved_unused;
static uint32_t tables_refused;
static uint16_t pid_l2_head[ZZAPP_AS_MAX_APPS];
static uint16_t pid_l2_count[ZZAPP_AS_MAX_APPS];
static uint8_t pid_flags[ZZAPP_AS_MAX_APPS];
static void (*table_fault_hook)(uint32_t pid);

/*
 * ASID allocator, core 1 private: no lock, local IRQs masked while it runs.
 * asid_owner holds pid + 1 (0 is free). The assigned ASIDs form a doubly
 * linked list from most to least recently dispatched; links use 0 for none,
 * which is safe because 0 is the kernel ASID and never listed.
 */
static uint16_t asid_owner[ZZAPP_AS_ASID_MAX + 1U];
static uint32_t asid_gen_tab[ZZAPP_AS_ASID_MAX + 1U];
static uint8_t asid_prev[ZZAPP_AS_ASID_MAX + 1U];
static uint8_t asid_next[ZZAPP_AS_ASID_MAX + 1U];
static uint8_t asid_lru_head;
static uint8_t asid_lru_tail;
static uint8_t asid_free_stack[ZZAPP_AS_ASID_MAX];
static uint32_t asid_free_top;
static uint32_t asid_assigned;
static uint32_t asid_steals;
static uint32_t running_pid = ZZAPP_AS_NO_PID;

static int template_ready;
static uint32_t firmware_text_start;
static uint32_t firmware_text_end;
static sdk_smp_lock_t as_lock = SDK_SMP_LOCK_INIT;
#ifdef MMU_PAGE_HOST_TEST
static uint32_t test_current_asid;
unsigned zzapp_as_test_asid_invalidations;
uint32_t zzapp_as_test_last_invalidated_asid;
unsigned zzapp_as_test_tlb_all_invalidations;
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
	zzapp_as_test_last_invalidated_asid = asid;
	zzapp_as_test_asid_invalidations++;
#endif
}

/* Every ASID on both cores, for recovery with core 1 halted. */
static void invalidate_all_asids(void)
{
#ifndef MMU_PAGE_HOST_TEST
	__asm__ __volatile__("dsb sy\n\t"
		"mcr p15, 0, %0, c8, c3, 0\n\t"
		"dsb sy\n\tisb" : : "r" (0U) : "memory");
#else
	zzapp_as_test_tlb_all_invalidations++;
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
	uint32_t first = (uint32_t)(uintptr_t)l2_pool;
	uint32_t base = descriptor & L1_TABLE_BASE_MASK;

	if (!l2_pool || (descriptor & L1_TYPE_MASK) != L1_TABLE || base < first)
		return -1;
	base = (base - first) >> 10;
	return base < pool_l2_total ? (int)base : -1;
}

/* Tables an allocation outside any reserve can take right now. */
static uint32_t l2_shared_free(void)
{
	return l2_free_count > l2_reserved_unused ?
		l2_free_count - l2_reserved_unused : 0U;
}

/* Tables `pid` may still allocate: what is left of its reserve plus the
 * shared remainder. */
static uint32_t pid_l2_available(uint32_t pid)
{
	uint32_t own = 0U;

	if ((pid_flags[pid] & PID_RESERVED) &&
	    pid_l2_count[pid] < ZZAPP_AS_L2_RESERVE)
		own = ZZAPP_AS_L2_RESERVE - pid_l2_count[pid];
	return own + l2_shared_free();
}

/* Take one zeroed table for a record index (its reserve first) or for the
 * template (shared remainder only). -1 when none may be given. */
static int l2_alloc(uint32_t owner)
{
	int from_reserve = 0;
	uint16_t index;

	if (owner != L2_OWNER_TEMPLATE &&
	    (pid_flags[owner] & PID_RESERVED) &&
	    pid_l2_count[owner] < ZZAPP_AS_L2_RESERVE)
		from_reserve = 1;
	if (l2_free_head == L2_NONE || (!from_reserve && l2_shared_free() == 0U)) {
		tables_refused++;
		return -1;
	}
	index = (uint16_t)l2_free_head;
	l2_free_head = l2_next[index];
	l2_free_count--;
	l2_owner[index] = (uint16_t)owner;
	l2_vsection[index] = 0xffffU;
	if (owner == L2_OWNER_TEMPLATE) {
		l2_template_used++;
		l2_next[index] = L2_NONE;
	} else {
		l2_next[index] = pid_l2_head[owner];
		pid_l2_head[owner] = index;
		pid_l2_count[owner]++;
		if (from_reserve)
			l2_reserved_unused--;
	}
	memset(l2_pool[index], 0, sizeof(l2_pool[index]));
	return (int)index;
}

/* Return every table of one record index to the free stack. */
static void l2_release_owner(uint32_t pid)
{
	uint16_t index = pid_l2_head[pid];

	while (index != L2_NONE) {
		uint16_t next = l2_next[index];

		if ((pid_flags[pid] & PID_RESERVED) &&
		    pid_l2_count[pid] <= ZZAPP_AS_L2_RESERVE)
			l2_reserved_unused++;
		pid_l2_count[pid]--;
		l2_owner[index] = L2_OWNER_FREE;
		l2_vsection[index] = 0xffffU;
		memset(l2_pool[index], 0, sizeof(l2_pool[index]));
		l2_next[index] = (uint16_t)l2_free_head;
		l2_free_head = index;
		l2_free_count++;
		index = next;
	}
	pid_l2_head[pid] = L2_NONE;
}

static int grant_l2_available(const zzapp_as_t *as, uint32_t first,
			      uint32_t end)
{
	uint32_t section;
	uint32_t needed = 0U;

	for (section = first >> SECTION_SHIFT;
	     section <= (end - 1U) >> SECTION_SHIFT; section++) {
		uint32_t descriptor = as->l1[section];
		int index = l2_index_for_descriptor(descriptor);

		if ((descriptor & L1_TYPE_MASK) == L1_TABLE &&
		    index >= 0 && l2_owner[index] == as->pid)
			continue;
		if ((descriptor & L1_TYPE_MASK) == L1_TABLE && index < 0)
			return 0;
		needed++;
	}
	if (needed > pid_l2_available(as->pid)) {
		tables_refused++;
		return 0;
	}
	return 1;
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

	if (index < 0 || l2_owner[index] != L2_OWNER_TEMPLATE) {
		/* The template is already walked. Clearing a section that
		 * contains this code faults the instruction fetch. Those
		 * sections were split to L2 before the template went live. */
		if (template_ready && section_covers_firmware_text(section))
			return ZZAPP_AS_ENOMEM;
		index = l2_alloc(L2_OWNER_TEMPLATE);
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
	index = l2_alloc(L2_OWNER_TEMPLATE);
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
		if (old_index >= 0 && l2_owner[old_index] == as->pid)
			return old_index;
		if (old_index < 0)
			return ZZAPP_AS_ENOMEM;
		new_index = l2_alloc(as->pid);
		if (new_index < 0)
			return ZZAPP_AS_ENOMEM;
		memcpy(l2_pool[new_index], l2_pool[old_index],
		       sizeof(l2_pool[new_index]));
		as_clean(l2_pool[new_index], sizeof(l2_pool[new_index]));
		replace_live_descriptor(section << SECTION_SHIFT,
					&as->l1[section],
					l2_descriptor(l2_pool[new_index],
						      descriptor));
		l2_vsection[new_index] = (uint16_t)section;
		return new_index;
	}
	new_index = l2_alloc(as->pid);
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

/*
 * First-fit placement among the sorted alias extents: the lowest page whose
 * address has the requested alignment and whose `pages` pages fit before the
 * next extent (or the end of the range). On success `insert` is the extent
 * index the new one goes to.
 */
static int alias_find_free(const zzapp_as_t *as, uint32_t pages,
			   uint32_t alignment, uint32_t *first,
			   uint32_t *insert)
{
	uint32_t cursor = 0U;
	uint32_t i;

	if (pages == 0U || pages > ZZAPP_AS_ALIAS_PAGE_COUNT)
		return -1;
	for (i = 0U; i <= as->alias_count; i++) {
		uint32_t limit = i < as->alias_count ? as->alias[i].first :
			ZZAPP_AS_ALIAS_PAGE_COUNT;
		uint32_t start = cursor;
		uint32_t skew = (ZZAPP_AS_ALIAS_BASE + start * PAGE_BYTES) &
			(alignment - 1U);

		if (skew)
			start += (alignment - skew) / PAGE_BYTES;
		if (start <= limit && pages <= limit - start) {
			*first = start;
			*insert = i;
			return 0;
		}
		if (i < as->alias_count)
			cursor = as->alias[i].first + as->alias[i].pages;
	}
	return -1;
}

/* The extent that starts at `first`, or -1. */
static int alias_extent_at(const zzapp_as_t *as, uint32_t first)
{
	uint32_t i;

	for (i = 0U; i < as->alias_count; i++)
		if (as->alias[i].first == first)
			return (int)i;
	return -1;
}

uint32_t zzapp_as_pool_bytes(uint32_t apps, uint32_t l2_total)
{
	return ZZAPP_AS_POOL_BYTES(apps, l2_total);
}

uint32_t *zzapp_as_pool_l1(uint32_t pid)
{
	return l1_base && pid < pool_apps ?
		(uint32_t *)(l1_base + pid * ZZAPP_AS_L1_BYTES) : NULL;
}

static void asid_reset_lists(void)
{
	uint32_t a;

	memset(asid_owner, 0, sizeof(asid_owner));
	memset(asid_prev, 0, sizeof(asid_prev));
	memset(asid_next, 0, sizeof(asid_next));
	asid_lru_head = 0U;
	asid_lru_tail = 0U;
	/* Pop order 1, 2, 3, ... */
	for (a = 0U; a < ZZAPP_AS_ASID_MAX; a++)
		asid_free_stack[a] = (uint8_t)(ZZAPP_AS_ASID_MAX - a);
	asid_free_top = ZZAPP_AS_ASID_MAX;
	asid_assigned = 0U;
	running_pid = ZZAPP_AS_NO_PID;
}

/* Back to a freshly initialised pool: every table free, no reserves, no
 * ASIDs, template not built. */
static void pool_reset(void)
{
	uint32_t i;

	l2_free_head = L2_NONE;
	for (i = pool_l2_total; i-- > 0U;) {
		l2_owner[i] = L2_OWNER_FREE;
		l2_vsection[i] = 0xffffU;
		l2_next[i] = (uint16_t)l2_free_head;
		l2_free_head = i;
	}
	l2_free_count = pool_l2_total;
	l2_template_used = 0U;
	l2_reserved_total = 0U;
	l2_reserved_unused = 0U;
	tables_refused = 0U;
	for (i = 0U; i < ZZAPP_AS_MAX_APPS; i++) {
		pid_l2_head[i] = L2_NONE;
		pid_l2_count[i] = 0U;
		pid_flags[i] = 0U;
	}
	memset(asid_gen_tab, 0, sizeof(asid_gen_tab));
	asid_steals = 0U;
	asid_reset_lists();
	template_ready = 0;
}

int zzapp_as_pool_init(void *base, uint32_t bytes, uint32_t apps,
		       uint32_t l2_total)
{
	uint8_t *p = base;

	/* Table addresses go into 32-bit descriptors. A host build keeps the
	 * pool wherever the linker put it: the only use of the descriptor's
	 * address is the index arithmetic below, which truncates consistently. */
#ifndef MMU_PAGE_HOST_TEST
	if ((uint64_t)(uintptr_t)base + bytes > 0x100000000ULL)
		return -1;
#endif
	if (!base || ((uintptr_t)base & (ZZAPP_AS_L1_BYTES - 1U)) != 0U ||
	    apps == 0U || apps > ZZAPP_AS_MAX_APPS || l2_total == 0U ||
	    l2_total >= L2_OWNER_TEMPLATE ||
	    bytes < ZZAPP_AS_POOL_BYTES(apps, l2_total))
		return -1;
	template_l1 = (uint32_t *)p;
	l1_base = p + ZZAPP_AS_L1_BYTES;
	l2_pool = (uint32_t (*)[L2_ENTRIES])(p + ZZAPP_AS_L1_BYTES *
					     (1U + apps));
	l2_owner = (uint16_t *)((uint8_t *)l2_pool +
				l2_total * ZZAPP_AS_L2_BYTES);
	l2_next = l2_owner + l2_total;
	l2_vsection = l2_next + l2_total;
	pool_apps = apps;
	pool_l2_total = l2_total;
	pool_reset();
	return 0;
}

void zzapp_as_init(const uint32_t *bsp_l1)
{
	uint32_t section;

	/* No pool, no address spaces: create() refuses and nothing is hooked. */
	if (!template_l1)
		return;
	pool_reset();
	memset(template_l1, 0, ZZAPP_AS_L1_BYTES);
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
	as_clean(template_l1, ZZAPP_AS_L1_BYTES);
	mmu_page_set_change_hook(zzapp_as_mmu_page_changed);
	template_ready = 1;
}

int zzapp_as_create(zzapp_as_t *as, uint32_t pid)
{
	if (!as || !template_l1 || pid >= pool_apps)
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	if (pid_flags[pid] & PID_LIVE) {
		sdk_smp_lock_release(&as_lock);
		return -1;
	}
	memset(as, 0, sizeof(*as));
	as->l1 = zzapp_as_pool_l1(pid);
	memcpy(as->l1, template_l1, ZZAPP_AS_L1_BYTES);
	as->pid = pid;
	as->active = 1U;
	pid_flags[pid] = (uint8_t)((pid_flags[pid] | PID_LIVE) &
				   ~(PID_FAULTED | PID_NOTIFIED));
	as_clean(as->l1, ZZAPP_AS_L1_BYTES);
	sdk_smp_lock_release(&as_lock);
	return 0;
}

/* as_lock held. Frees the tables the pid still owns, then its reserve. */
static void reserve_release_locked(uint32_t pid)
{
	if (!(pid_flags[pid] & PID_RESERVED))
		return;
	l2_release_owner(pid);
	l2_reserved_total -= ZZAPP_AS_L2_RESERVE;
	l2_reserved_unused -= ZZAPP_AS_L2_RESERVE;
	pid_flags[pid] &= (uint8_t)~PID_RESERVED;
}

/* as_lock held. 0, ZZAPP_AS_ETABLES, or -1 when already reserved. */
static int reserve_locked(uint32_t pid)
{
	uint32_t own;

	if (pid_flags[pid] & PID_RESERVED)
		return -1;
	if (l2_reserved_total + ZZAPP_AS_L2_RESERVE >
	    pool_l2_total - l2_template_used)
		return ZZAPP_AS_ETABLES;
	own = pid_l2_count[pid] < ZZAPP_AS_L2_RESERVE ?
		ZZAPP_AS_L2_RESERVE - pid_l2_count[pid] : 0U;
	l2_reserved_total += ZZAPP_AS_L2_RESERVE;
	l2_reserved_unused += own;
	pid_flags[pid] |= PID_RESERVED;
	return 0;
}

int zzapp_as_reserve_l2(uint32_t pid)
{
	int rc;

	if (!template_l1 || pid >= pool_apps)
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	rc = reserve_locked(pid);
	sdk_smp_lock_release(&as_lock);
	return rc;
}

int zzapp_as_release_l2(uint32_t pid)
{
	if (!template_l1 || pid >= pool_apps)
		return -1;
	sdk_smp_lock_acquire(&as_lock);
	if (pid_flags[pid] & PID_LIVE) {
		sdk_smp_lock_release(&as_lock);
		return -1;
	}
	reserve_release_locked(pid);
	sdk_smp_lock_release(&as_lock);
	return 0;
}

void zzapp_as_destroy(zzapp_as_t *as)
{
	uint32_t pid;

	if (!as || !as->active)
		return;
	pid = as->pid;
	/* Core 0 keeps the BSP table. The template's domain 15 is Manager in
	 * core 0's DACR, which would drop the BSP's read-only text protection. */
	if (smp_cpu_id() == 1)
		zzapp_as_switch_kernel();
	zzapp_as_release_asid(as);
	sdk_smp_lock_acquire(&as_lock);
	l2_release_owner(pid);
	memset(as->l1, 0, ZZAPP_AS_L1_BYTES);
	pid_flags[pid] &= (uint8_t)~(PID_LIVE | PID_FAULTED | PID_NOTIFIED);
	as->l1 = NULL;
	as->active = 0U;
	as->alias_count = 0U;
	sdk_smp_lock_release(&as_lock);
}

void zzapp_as_destroy_pid(uint32_t pid)
{
	zzapp_as_t as;
	uint32_t a;

	if (!template_l1 || pid >= pool_apps || !(pid_flags[pid] & PID_LIVE))
		return;
	memset(&as, 0, sizeof(as));
	as.l1 = zzapp_as_pool_l1(pid);
	as.pid = pid;
	as.active = 1U;
	for (a = 1U; a <= ZZAPP_AS_ASID_MAX; a++) {
		if (asid_owner[a] == pid + 1U) {
			as.asid = (uint16_t)a;
			as.asid_gen = asid_gen_tab[a];
		}
	}
	zzapp_as_destroy(&as);
}

int zzapp_as_table_faulted(uint32_t pid)
{
	return pid < pool_apps && (pid_flags[pid] & PID_FAULTED) != 0U;
}

void zzapp_as_set_table_fault_hook(void (*hook)(uint32_t pid))
{
	table_fault_hook = hook;
}

void zzapp_as_stats(zzapp_as_stats_t *out)
{
	if (!out)
		return;
	sdk_smp_lock_acquire(&as_lock);
	out->apps = pool_apps;
	out->l2_total = pool_l2_total;
	out->l2_free = l2_free_count;
	out->l2_template = l2_template_used;
	out->l2_reserved = l2_reserved_total;
	out->l2_reserved_unused = l2_reserved_unused;
	out->l2_shared_free = l2_shared_free();
	out->tables_refused = tables_refused;
	out->asid_assigned = asid_assigned;
	out->asid_steals = asid_steals;
	sdk_smp_lock_release(&as_lock);
}

void zzapp_as_reconcile(void)
{
	uint32_t i;

	if (!template_l1)
		return;
	sdk_smp_lock_acquire(&as_lock);
	for (i = 0U; i < pool_apps; i++) {
		pid_l2_head[i] = L2_NONE;
		pid_l2_count[i] = 0U;
	}
	l2_free_head = L2_NONE;
	l2_free_count = 0U;
	l2_template_used = 0U;
	/* Descending, so the lowest index ends up first on each list. */
	for (i = pool_l2_total; i-- > 0U;) {
		uint16_t owner = l2_owner[i];

		if (owner == L2_OWNER_TEMPLATE) {
			l2_next[i] = L2_NONE;
			l2_template_used++;
		} else if (owner < pool_apps) {
			l2_next[i] = pid_l2_head[owner];
			pid_l2_head[owner] = (uint16_t)i;
			pid_l2_count[owner]++;
		} else {
			l2_owner[i] = L2_OWNER_FREE;
			l2_vsection[i] = 0xffffU;
			l2_next[i] = (uint16_t)l2_free_head;
			l2_free_head = i;
			l2_free_count++;
		}
	}
	l2_reserved_total = 0U;
	l2_reserved_unused = 0U;
	for (i = 0U; i < pool_apps; i++) {
		if (!(pid_flags[i] & PID_RESERVED))
			continue;
		l2_reserved_total += ZZAPP_AS_L2_RESERVE;
		if (pid_l2_count[i] < ZZAPP_AS_L2_RESERVE)
			l2_reserved_unused += ZZAPP_AS_L2_RESERVE -
				pid_l2_count[i];
	}
	sdk_smp_lock_release(&as_lock);
}

static void asid_lru_unlink(uint8_t a)
{
	uint8_t p = asid_prev[a];
	uint8_t n = asid_next[a];

	if (p)
		asid_next[p] = n;
	else
		asid_lru_head = n;
	if (n)
		asid_prev[n] = p;
	else
		asid_lru_tail = p;
	asid_prev[a] = 0U;
	asid_next[a] = 0U;
}

static void asid_lru_push_mru(uint8_t a)
{
	asid_prev[a] = 0U;
	asid_next[a] = asid_lru_head;
	if (asid_lru_head)
		asid_prev[asid_lru_head] = a;
	else
		asid_lru_tail = a;
	asid_lru_head = a;
}

int zzapp_as_asid_valid(const zzapp_as_t *as)
{
	return as && as->active && as->asid != 0U &&
	       as->asid <= ZZAPP_AS_ASID_MAX &&
	       asid_owner[as->asid] == as->pid + 1U &&
	       asid_gen_tab[as->asid] == as->asid_gen;
}

int zzapp_as_acquire_asid(zzapp_as_t *as)
{
	uint32_t irq;
	uint8_t a;

	if (!as || !as->active)
		return -1;
	irq = smp_local_irq_save();
	if (zzapp_as_asid_valid(as)) {
		a = (uint8_t)as->asid;
		if (a != asid_lru_head) {
			asid_lru_unlink(a);
			asid_lru_push_mru(a);
		}
		smp_local_irq_restore(irq);
		return a;
	}
	if (asid_free_top) {
		a = asid_free_stack[--asid_free_top];
	} else {
		/* Steal the least recently dispatched ASID whose owner is not
		 * the running record. Its translations leave every TLB now, so
		 * the new owner never sees them. */
		a = asid_lru_tail;
		while (a && (uint32_t)asid_owner[a] - 1U == running_pid)
			a = asid_prev[a];
		if (!a) {
			smp_local_irq_restore(irq);
			return -1;
		}
		invalidate_asid(a);
		asid_gen_tab[a]++;
		asid_steals++;
		asid_lru_unlink(a);
		asid_assigned--;
		as_invalidate_predictors();
	}
	asid_owner[a] = (uint16_t)(as->pid + 1U);
	as->asid = a;
	as->asid_gen = asid_gen_tab[a];
	asid_lru_push_mru(a);
	asid_assigned++;
	smp_local_irq_restore(irq);
	return a;
}

void zzapp_as_release_asid(zzapp_as_t *as)
{
	uint32_t irq;

	if (!as)
		return;
	irq = smp_local_irq_save();
	if (zzapp_as_asid_valid(as)) {
		uint8_t a = (uint8_t)as->asid;

		/* Flush first: a free ASID never has live translations. */
		invalidate_asid(a);
		asid_gen_tab[a]++;
		asid_owner[a] = 0U;
		asid_lru_unlink(a);
		asid_free_stack[asid_free_top++] = a;
		asid_assigned--;
	}
	as->asid = 0U;
	smp_local_irq_restore(irq);
}

void zzapp_as_reset_asids(void)
{
	uint32_t a;
	uint32_t irq = smp_local_irq_save();

	invalidate_all_asids();
	/* Generations never go down, so a stale assignment cannot match. */
	for (a = 1U; a <= ZZAPP_AS_ASID_MAX; a++)
		asid_gen_tab[a]++;
	asid_reset_lists();
	smp_local_irq_restore(irq);
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
		replace_live_descriptor(virtual_base + i * SECTION_BYTES,
					&as->l1[(virtual_base >> SECTION_SHIFT) + i],
					frames[i] | DDR_SECTION | L1_NG | L1_XN);
	as_clean(as->l1, ZZAPP_AS_L1_BYTES);
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
		replace_live_descriptor(virtual_base + i * SECTION_BYTES,
					&as->l1[(virtual_base >> SECTION_SHIFT) + i], 0U);
	as_clean(as->l1, ZZAPP_AS_L1_BYTES);
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
	uint32_t first;
	uint32_t insert;

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
	if (as->alias_count >= ZZAPP_AS_ALIAS_EXTENTS ||
	    alias_find_free(as, pages, alignment, &first, &insert) != 0) {
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
		offset += mapped_bytes;
	}
	memmove(&as->alias[insert + 1U], &as->alias[insert],
		(as->alias_count - insert) * sizeof(as->alias[0]));
	as->alias[insert].first = (uint16_t)first;
	as->alias[insert].pages = (uint16_t)pages;
	as->alias_count++;
	as_clean(as->l1, ZZAPP_AS_L1_BYTES);
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
	int extent;

	if (!alias_range_ok(as, va, bytes))
		return -1;
	first = (va - ZZAPP_AS_ALIAS_BASE) >> PAGE_SHIFT;
	pages = bytes >> PAGE_SHIFT;
	sdk_smp_lock_acquire(&as_lock);
	extent = alias_extent_at(as, first);
	if (extent < 0 || as->alias[extent].pages != pages) {
		sdk_smp_lock_release(&as_lock);
		return -1;
	}
	for (offset = 0U; offset < bytes;) {
		uint32_t address = va + offset;
		uint32_t section = address >> SECTION_SHIFT;
		uint32_t page = (address >> PAGE_SHIFT) & 0xffU;
		uint32_t mapped_bytes = PAGE_BYTES;
		int index = l2_index_for_descriptor(as->l1[section]);

		if (index < 0) {
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
			for (i = 0U; i < LARGE_PAGE_BYTES / PAGE_BYTES; i++)
				l2_pool[index][page + i] =
					template_page(section, page + i);
			as_clean(&l2_pool[index][page],
				 LARGE_PAGE_BYTES / PAGE_BYTES *
				 sizeof(l2_pool[index][page]));
			as_invalidate_range(address, address + LARGE_PAGE_BYTES,
					    LARGE_PAGE_BYTES);
		} else {
			replace_live_page(address, &l2_pool[index][page],
					  template_page(section, page));
		}
		offset += mapped_bytes;
	}
	memmove(&as->alias[extent], &as->alias[extent + 1],
		(as->alias_count - (uint32_t)extent - 1U) *
		sizeof(as->alias[0]));
	as->alias_count--;
	as_clean(as->l1, ZZAPP_AS_L1_BYTES);
	sdk_smp_lock_release(&as_lock);
	return 0;
}

uint32_t zzapp_as_alias_phys(const zzapp_as_t *as, uint32_t va)
{
	uint32_t page;
	uint32_t descriptor;
	uint32_t i;
	int index;

	if (!as || !as->active || va < ZZAPP_AS_ALIAS_BASE ||
	    va >= ZZAPP_AS_ALIAS_END)
		return 0U;
	page = (va - ZZAPP_AS_ALIAS_BASE) >> PAGE_SHIFT;
	for (i = 0U; i < as->alias_count; i++)
		if (page >= as->alias[i].first &&
		    page - as->alias[i].first < as->alias[i].pages)
			break;
	if (i == as->alias_count)
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

	return index >= 0 && l2_owner[index] == as->pid;
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
			replace_live_descriptor(address, &as->l1[section],
						(physical_base + offset) |
						image_section_attrs(permissions));
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
	as_clean(as->l1, ZZAPP_AS_L1_BYTES);
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
	uint32_t pid;
	uint8_t notify[ZZAPP_AS_MAX_APPS / 8U];
	int notify_any = 0;

	if (section >= L1_ENTRIES || !template_l1)
		return;
	memset(notify, 0, sizeof(notify));
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
	for (pid = 0U; pid < pool_apps; pid++) {
		uint32_t *l1;
		uint32_t virtual_section;
		uint16_t table;

		if (!(pid_flags[pid] & PID_LIVE))
			continue;
		l1 = zzapp_as_pool_l1(pid);
		{
			uint32_t app_descriptor = l1[section];
			int index = l2_index_for_descriptor(app_descriptor);

			if (!((app_descriptor & L1_TYPE_MASK) == L1_SECTION &&
			      (app_descriptor & L1_NG) != 0U) &&
			    !(index >= 0 && l2_owner[index] == pid))
				replace_live_descriptor(section << SECTION_SHIFT,
							&l1[section],
							template_l1[section]);
		}
		/* Private sections that map the changed physical section. The
		 * scan covers the private window only, once per BSP change. */
		for (virtual_section = ZZAPP_AS_WINDOW_BASE >> SECTION_SHIFT;
		     virtual_section < ZZAPP_AS_WINDOW_END >> SECTION_SHIFT;
		     virtual_section++) {
			uint32_t app_descriptor = l1[virtual_section];

			if ((app_descriptor & (L1_TYPE_MASK | L1_NG)) !=
			    (L1_SECTION | L1_NG) ||
			    (app_descriptor & SECTION_MASK) !=
			    section << SECTION_SHIFT)
				continue;
			if (!pages) {
				replace_live_descriptor(
					virtual_section << SECTION_SHIFT,
					&l1[virtual_section],
					(app_descriptor & ~L1_MEMORY_MASK) |
					(descriptor & L1_MEMORY_MASK));
			} else {
				zzapp_as_t as = {
					.l1 = l1, .pid = pid, .active = 1U
				};

				/* No table to give: this record cannot see the
				 * new memory type. The host stops it. */
				if (ensure_l2(&as, virtual_section) < 0) {
					pid_flags[pid] |= PID_FAULTED;
					if (!(pid_flags[pid] & PID_NOTIFIED)) {
						pid_flags[pid] |= PID_NOTIFIED;
						notify[pid >> 3] |=
							(uint8_t)(1U << (pid & 7U));
						notify_any = 1;
					}
				}
			}
		}
		for (table = pid_l2_head[pid]; table != L2_NONE;
		     table = l2_next[table]) {
			if (l2_vsection[table] == 0xffffU)
				continue;
			propagate_l2_attributes((int)table, l2_vsection[table],
						 section, descriptor, pages);
		}
		as_clean(l1, ZZAPP_AS_L1_BYTES);
	}
	as_clean(template_l1, ZZAPP_AS_L1_BYTES);
	as_invalidate_range(section << SECTION_SHIFT,
			    (section << SECTION_SHIFT) + SECTION_BYTES,
			    PAGE_BYTES);
	as_invalidate_predictors();
	sdk_smp_lock_release(&as_lock);
	if (notify_any && table_fault_hook)
		for (pid = 0U; pid < pool_apps; pid++)
			if (notify[pid >> 3] & (1U << (pid & 7U)))
				table_fault_hook(pid);
}

void zzapp_as_switch(zzapp_as_t *as)
{
	if (!as || !as->active)
		return;
	if (zzapp_as_acquire_asid(as) < 0)
		return;
	running_pid = as->pid;
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
#else
	test_current_asid = as->asid;
#endif
}

void zzapp_as_switch_kernel(void)
{
	running_pid = ZZAPP_AS_NO_PID;
	/* Without a table pool (the record arena did not fit) there is no
	 * template and no app: core 1 keeps the boot tables. */
	if (!template_l1)
		return;
#ifndef MMU_PAGE_HOST_TEST
	{
		uint32_t ttbr0 = (uint32_t)(uintptr_t)template_l1 | TTBR0_ATTR;
		uint32_t asid = 0U;

		__asm__ __volatile__("dsb sy\n\t"
			"mcr p15, 0, %0, c13, c0, 1\n\tisb\n\t"
			"mcr p15, 0, %1, c2, c0, 0\n\tisb\n\t"
			"dsb sy\n\tisb"
			: : "r" (asid), "r" (ttbr0) : "memory");
	}
#else
	test_current_asid = 0U;
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

void zzapp_as_test_scramble_pool(void)
{
	uint32_t i;

	l2_free_head = L2_NONE;
	l2_free_count = 12345U;
	l2_template_used = 77U;
	l2_reserved_total = 999U;
	l2_reserved_unused = 555U;
	for (i = 0U; i < pool_apps; i++) {
		pid_l2_head[i] = L2_NONE;
		pid_l2_count[i] = 31U;
	}
	for (i = 0U; i < pool_l2_total; i++)
		l2_next[i] = 0U;
}

uint32_t zzapp_as_test_current_asid(void)
{
	return test_current_asid;
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
