/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Cortex-A9 cache maintenance for the core-1 decode allocation tracker;
 * compiled only for ARM. Kept separate from sdk_compression.c because
 * xil_cache.h pulls in Xilinx's LONG/ULONG typedefs, which collide with the
 * LZMA SDK's 7zTypes.h that sdk_compression.c also includes.
 *
 * The table is cache-line aligned and line-sized (see sdk_decode_reclaim.c), so
 * these range operations touch nothing but the table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "xil_cache.h"
#include "sdk_decode_reclaim.h"

/* Core 1: clean the table to DRAM after each track/untrack. */
void sdk_decode_flush_table(void)
{
	Xil_DCacheFlushRange((INTPTR)sdk_decode_table_base(),
	                     sdk_decode_table_bytes());
}

/* Core 0: discard any stale cached copy before reading the table at reclaim. */
void sdk_decode_invalidate_table(void)
{
	Xil_DCacheInvalidateRange((INTPTR)sdk_decode_table_base(),
	                          sdk_decode_table_bytes());
}
