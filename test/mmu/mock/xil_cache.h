/*
 * Host stub for the Xilinx cache API used by mmu_page.c. Every flush is
 * recorded together with the watched L1 entry and page-table entry at the
 * time of the call, so the test can check what was cleaned before, during
 * and after the break-before-make translation change.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include <stdint.h>

typedef uintptr_t INTPTR;

#define XIL_CACHE_MOCK_CAPACITY 16U

struct xil_cache_mock_flush {
	uintptr_t adr;
	unsigned long len;
	uint32_t l1_entry;   /* mmu_page_test_l1[watch_section] */
	uint32_t page_entry; /* page table[watch_page] */
};

struct xil_cache_mock_state {
	unsigned watch_section;
	unsigned watch_page;
	unsigned count;
	struct xil_cache_mock_flush ops[XIL_CACHE_MOCK_CAPACITY];
};

extern struct xil_cache_mock_state g_xil_cache_mock;
extern uint32_t mmu_page_test_l1[4096];
const uint32_t *mmu_page_test_page_table(void);

static inline void Xil_DCacheFlushRange(INTPTR adr, unsigned long len)
{
	struct xil_cache_mock_flush *op;

	if (g_xil_cache_mock.count >= XIL_CACHE_MOCK_CAPACITY)
		return;
	op = &g_xil_cache_mock.ops[g_xil_cache_mock.count++];
	op->adr = adr;
	op->len = len;
	op->l1_entry = mmu_page_test_l1[g_xil_cache_mock.watch_section & 0xfffU];
	op->page_entry =
		mmu_page_test_page_table()[g_xil_cache_mock.watch_page & 0xffU];
}

#endif
