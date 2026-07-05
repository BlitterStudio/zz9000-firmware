/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Single-writer allocation tracker for the core-1 one-shot decode path.
 * See sdk_decode_reclaim.h for the concurrency contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include "sdk_decode_reclaim.h"

/*
 * Cross-core visibility CANNOT rely on SCU snoop coherency here: at the reclaim
 * point core 1 is held in reset, and a reset core's dirty L1 lines are dropped,
 * not snooped. The firmware wrappers therefore maintain this table to the point
 * of coherency explicitly -- core 1 cleans it to DRAM after every track/untrack,
 * core 0 invalidates before reading and flushes after clearing (see
 * sdk_compression.c). That works on both reset paths because it happens at
 * store time, not in core-1 cleanup code (the quiesce path runs none).
 *
 * Aligned to a cache line and sized to a whole number of lines so the range
 * clean/invalidate cannot touch neighbouring data.
 */
static void *g_tracked[SDK_DECODE_MAX_TRACKED] __attribute__((aligned(32)));

/*
 * The firmware cleans/invalidates the whole table by range, so it must be a
 * whole number of 32-byte cache lines or a range op could touch a neighbour.
 * Fails the build if SDK_DECODE_MAX_TRACKED is ever set to a non-multiple-of-8.
 */
typedef char sdk_decode_table_is_line_multiple[
	(sizeof(g_tracked) % 32u == 0u) ? 1 : -1];

void *sdk_decode_table_base(void)
{
	return (void *)g_tracked;
}

unsigned sdk_decode_table_bytes(void)
{
	return (unsigned)sizeof(g_tracked);
}

void sdk_decode_track(void *ptr)
{
	unsigned i;

	if (!ptr)
		return;
	for (i = 0u; i < SDK_DECODE_MAX_TRACKED; i++) {
		if (g_tracked[i] == NULL) {
			g_tracked[i] = ptr;   /* single aligned store: atomic */
			return;
		}
	}
	/*
	 * Table full: this allocation cannot be tracked and would leak if the
	 * worker is reset before freeing it. That is the pre-existing behaviour
	 * and strictly safer than overrunning the table; the backends never
	 * exceed the ceiling in practice.
	 */
}

void sdk_decode_untrack(void *ptr)
{
	unsigned i;

	if (!ptr)
		return;
	for (i = 0u; i < SDK_DECODE_MAX_TRACKED; i++) {
		if (g_tracked[i] == ptr) {
			g_tracked[i] = NULL;   /* single aligned store: atomic */
			return;
		}
	}
}

unsigned sdk_decode_reclaim(void (*free_fn)(void *))
{
	unsigned i;
	unsigned reclaimed = 0u;

	for (i = 0u; i < SDK_DECODE_MAX_TRACKED; i++) {
		void *p = g_tracked[i];

		if (p != NULL) {
			g_tracked[i] = NULL;
			if (free_fn)
				free_fn(p);
			reclaimed++;
		}
	}
	return reclaimed;
}

unsigned sdk_decode_tracked_count(void)
{
	unsigned i;
	unsigned count = 0u;

	for (i = 0u; i < SDK_DECODE_MAX_TRACKED; i++)
		if (g_tracked[i] != NULL)
			count++;
	return count;
}
