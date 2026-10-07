/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include "mmu_page.h"
#include "xil_cache.h"
#ifndef MMU_PAGE_HOST_TEST
#include "xil_types.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#endif

#define MMU_SECTION_SHIFT      20U
#define MMU_SECTION_COUNT      4096U
#define MMU_PAGE_SHIFT         12U
#define MMU_PAGE_SIZE          (1U << MMU_PAGE_SHIFT)
#define MMU_PAGES_PER_SECTION  256U

/*
 * ARMv7 short-descriptor encodings. translation_table.S gives every DDR
 * section 0x15de6: Normal, inner and outer write-back write-allocate,
 * shareable, AP=11, domain 15. Firmware stamps whole sections with the
 * BSP's NORM_NONCACHE (0x11de2) or STRONG_ORDERED (0xc02). Section
 * descriptors keep the attributes in bits [19:0] and use type 0b10 with
 * bit 18 clear (bit 18 set is a supersection).
 */
#define MMU_L1_TYPE_MASK       0x00040003U
#define MMU_L1_TYPE_SECTION    0x00000002U
#define MMU_L1_TYPE_PAGE_TABLE 0x00000001U
#define MMU_L1_ATTR_MASK       0x000FFFFFU
#define MMU_SECTION_DDR_WB     0x00015DE6U
#define MMU_SECTION_NONCACHE   0x00011DE2U
#define MMU_SECTION_STRONG     0x00000C02U

/* Small pages with the same memory types: S [10], TEX [8:6], AP [5:4],
 * C [3], B [2], type 0b1x [1], XN [0] clear. */
#define MMU_PAGE_DDR_WB        0x00000576U /* S=1 TEX=101 AP=11 C=0 B=1 */
#define MMU_PAGE_NONCACHE      0x00000472U /* S=1 TEX=001 AP=11 C=0 B=0 */

/* Page-table descriptor: table base [31:10], domain [8:5], type 0b01. */
#define MMU_L1_TABLE_BASE_MASK 0xFFFFFC00U
#define MMU_L1_DOMAIN_15       (15U << 5)

static uint32_t page_table[MMU_PAGES_PER_SECTION]
	__attribute__((aligned(1024)));
static uint32_t page_table_section = MMU_SECTION_COUNT;

#ifdef MMU_PAGE_HOST_TEST
uint32_t mmu_page_test_l1[MMU_SECTION_COUNT];

const uint32_t *mmu_page_test_page_table(void)
{
	return page_table;
}

static uint32_t *l1_table(void)
{
	return mmu_page_test_l1;
}

static void sync_translation(void)
{
}
#else
/* BSP flat 1 MB-section translation table (translation_table.S). */
extern u32 MMUTable;

static uint32_t *l1_table(void)
{
	return (uint32_t *)&MMUTable;
}

static void sync_translation(void)
{
	dsb();
	/* Inner-shareable: core 1 walks the same table. */
	mtcp(XREG_CP15_INVAL_TLB_IS, 0U);
	mtcp(XREG_CP15_INVAL_BRANCH_ARRAY_IS, 0U);
	dsb();
	isb();
}
#endif

static uint32_t page_table_descriptor(void)
{
	return ((uint32_t)(uintptr_t)page_table & MMU_L1_TABLE_BASE_MASK) |
		MMU_L1_DOMAIN_15 | MMU_L1_TYPE_PAGE_TABLE;
}

int mmu_page_set_noncacheable(uintptr_t page_addr)
{
	uint32_t *l1 = l1_table();
	uint32_t section;
	uint32_t index;
	uint32_t entry;
	uint32_t noncache_page;
	int install = 0;

	if ((page_addr & (MMU_PAGE_SIZE - 1U)) != 0U ||
	    (page_addr >> MMU_SECTION_SHIFT) >= MMU_SECTION_COUNT)
		return 0;
	section = (uint32_t)(page_addr >> MMU_SECTION_SHIFT);
	index = (uint32_t)(page_addr >> MMU_PAGE_SHIFT) &
		(MMU_PAGES_PER_SECTION - 1U);
	noncache_page = (uint32_t)page_addr | MMU_PAGE_NONCACHE;
	entry = l1[section];

	if ((entry & MMU_L1_TYPE_MASK) == MMU_L1_TYPE_PAGE_TABLE) {
		if (section != page_table_section ||
		    entry != page_table_descriptor())
			return 0;
		if (page_table[index] == noncache_page)
			return 1;
	} else {
		uint32_t attr = entry & MMU_L1_ATTR_MASK;
		uint32_t i;

		if ((entry & MMU_L1_TYPE_MASK) != MMU_L1_TYPE_SECTION ||
		    (entry >> MMU_SECTION_SHIFT) != section)
			return 0;
		if (attr == MMU_SECTION_NONCACHE || attr == MMU_SECTION_STRONG)
			return 1;
		if (attr != MMU_SECTION_DDR_WB ||
		    page_table_section != MMU_SECTION_COUNT)
			return 0;
		for (i = 0U; i < MMU_PAGES_PER_SECTION; i++)
			page_table[i] = (section << MMU_SECTION_SHIFT) |
				(i << MMU_PAGE_SHIFT) | MMU_PAGE_DDR_WB;
		install = 1;
	}

	/* Write back and drop the page while it is still cacheable: once it is
	 * not, a dirty line evicted later would overwrite host writes. */
	Xil_DCacheFlushRange((INTPTR)page_addr, MMU_PAGE_SIZE);
	page_table[index] = noncache_page;
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)page_table, sizeof(page_table));
	if (install) {
		l1[section] = page_table_descriptor();
		Xil_DCacheFlushRange((INTPTR)(uintptr_t)&l1[section],
		                     sizeof(l1[section]));
		page_table_section = section;
	}
	sync_translation();
	/* Drop any clean line a speculative fill brought in under the old
	 * cacheable entry before the TLB invalidate took effect. */
	Xil_DCacheFlushRange((INTPTR)page_addr, MMU_PAGE_SIZE);
	return 1;
}
