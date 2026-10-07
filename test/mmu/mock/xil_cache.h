/*
 * Host stub for the Xilinx cache API used by mmu_page.c. Records every
 * flush together with the L1 entry of the flushed address's section at the
 * time of the call, so the test can check what was cleaned before and
 * after the translation change.
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
	uint32_t l1_entry;
};

struct xil_cache_mock_state {
	unsigned count;
	struct xil_cache_mock_flush ops[XIL_CACHE_MOCK_CAPACITY];
};

extern struct xil_cache_mock_state g_xil_cache_mock;
extern uint32_t mmu_page_test_l1[4096];

static inline void Xil_DCacheFlushRange(INTPTR adr, unsigned long len)
{
	struct xil_cache_mock_flush *op;

	if (g_xil_cache_mock.count >= XIL_CACHE_MOCK_CAPACITY)
		return;
	op = &g_xil_cache_mock.ops[g_xil_cache_mock.count++];
	op->adr = adr;
	op->len = len;
	op->l1_entry = mmu_page_test_l1[(adr >> 20) & 0xfffU];
}

#endif
