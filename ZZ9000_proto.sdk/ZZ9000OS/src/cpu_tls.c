/*
 * Per-core thread-local storage for firmware code.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libjpeg-turbo keeps its SIMD capability flags in THREAD_LOCAL variables,
 * which the compiler reads through TPIDRURO. The BSP zeroes TPIDRURO on
 * core 0, so those accesses landed at physical address 0x8-0xF, and core 1
 * never set it at all: its reset value is unknown, and a JPEG decode on
 * core 1 data-aborted once core 1 ran on page tables that no longer map the
 * PL address space. Each core now gets its own initialised block.
 */
#include <string.h>

#include "cpu_tls.h"

uintptr_t cpu_tls_layout(uint8_t *block, uint32_t block_bytes,
			 const uint8_t *tdata, uint32_t tdata_bytes,
			 uint32_t tbss_offset, uint32_t tbss_bytes)
{
	uint32_t image_bytes;

	if (!block || tbss_offset < tdata_bytes ||
	    tbss_bytes > UINT32_MAX - tbss_offset)
		return 0;
	image_bytes = tbss_offset + tbss_bytes;
	if (image_bytes > block_bytes ||
	    CPU_TLS_TCB_BYTES > block_bytes - image_bytes)
		return 0;
	memset(block, 0, CPU_TLS_TCB_BYTES + image_bytes);
	if (tdata_bytes)
		memcpy(block + CPU_TLS_TCB_BYTES, tdata, tdata_bytes);
	return (uintptr_t)block;
}

#ifndef CPU_TLS_HOST_TEST
extern uint8_t __tdata_start[], __tdata_end[], __tbss_start[], __tbss_end[];

static uint8_t cpu_tls_blocks[CPU_TLS_CPUS]
			     [CPU_TLS_TCB_BYTES + CPU_TLS_MAX_BYTES]
	__attribute__((aligned(8)));

int cpu_tls_init(uint32_t cpu)
{
	uintptr_t tp;

	if (cpu >= CPU_TLS_CPUS)
		return -1;
	tp = cpu_tls_layout(cpu_tls_blocks[cpu], sizeof(cpu_tls_blocks[cpu]),
			    __tdata_start,
			    (uint32_t)(__tdata_end - __tdata_start),
			    (uint32_t)(__tbss_start - __tdata_start),
			    (uint32_t)(__tbss_end - __tbss_start));
	if (!tp)
		return -1;
	__asm__ volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(tp) : "memory");
	return 0;
}
#endif
