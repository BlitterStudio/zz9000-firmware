/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MMU_PAGE_H
#define MMU_PAGE_H

#include <stdint.h>

/*
 * Map one 4 KiB DDR page Normal non-cacheable while the rest of its 1 MB
 * section keeps the BSP's cached attributes. The BSP translation table maps
 * DDR in 1 MB sections only, so the first call for a cached section swaps
 * that section for a page table; one section can be split per boot.
 *
 * The change is break-before-make: the section (or page) is unmapped while
 * the TLBs are invalidated, so any access to it in that window faults.
 * Call only while nothing else can touch the section: on core 0 before
 * core 1 starts and before interrupts are enabled. The TLB invalidate is
 * inner-shareable because core 1 later walks the same table.
 *
 * Returns 1 when the page is non-cacheable on return: newly split, already
 * split, or inside a section firmware already mapped non-cacheable or
 * strongly ordered. Returns 0 (and changes nothing) for an unaligned
 * address, a section with any other mapping, or a second cached section.
 */
int mmu_page_set_noncacheable(uintptr_t page_addr);

#ifdef MMU_PAGE_HOST_TEST
/* Host tests supply the L1 table and read back the page table. */
extern uint32_t mmu_page_test_l1[4096];
const uint32_t *mmu_page_test_page_table(void);
#endif

#endif
