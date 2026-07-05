/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host test for the core-1 decode allocation tracker (sdk_decode_reclaim.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "sdk_decode_reclaim.h"

static void *g_freed[64];
static unsigned g_freed_count;

static void mock_free(void *ptr)
{
	if (g_freed_count < (sizeof(g_freed) / sizeof(g_freed[0])))
		g_freed[g_freed_count++] = ptr;
}

static void reset_mock(void)
{
	memset(g_freed, 0, sizeof(g_freed));
	g_freed_count = 0u;
}

static int mock_freed(void *ptr)
{
	unsigned i;

	for (i = 0u; i < g_freed_count; i++)
		if (g_freed[i] == ptr)
			return 1;
	return 0;
}

/* Distinct non-NULL fake pointers. */
static char blocks[SDK_DECODE_MAX_TRACKED + 8u];
static void *blk(unsigned i) { return &blocks[i]; }

static int test_clean_decode_reclaims_nothing(void)
{
	reset_mock();
	/* A decode that frees everything it allocated leaves an empty table. */
	sdk_decode_track(blk(0));
	sdk_decode_track(blk(1));
	if (sdk_decode_tracked_count() != 2u) return 1;
	sdk_decode_untrack(blk(0));
	sdk_decode_untrack(blk(1));
	if (sdk_decode_tracked_count() != 0u) return 2;
	if (sdk_decode_reclaim(mock_free) != 0u) return 3;
	if (g_freed_count != 0u) return 4;
	return 0;
}

static int test_killed_decode_reclaims_survivors(void)
{
	reset_mock();
	/* A decode killed mid-flight: some blocks were never freed. */
	sdk_decode_track(blk(0));
	sdk_decode_track(blk(1));
	sdk_decode_track(blk(2));
	sdk_decode_untrack(blk(1));            /* one freed before the reset */
	if (sdk_decode_tracked_count() != 2u) return 1;
	if (sdk_decode_reclaim(mock_free) != 2u) return 2;
	if (!mock_freed(blk(0)) || !mock_freed(blk(2))) return 3;
	if (mock_freed(blk(1))) return 4;      /* already freed: not double-freed */
	if (sdk_decode_tracked_count() != 0u) return 5;   /* table cleared */
	/* A second reclaim frees nothing. */
	reset_mock();
	if (sdk_decode_reclaim(mock_free) != 0u) return 6;
	if (g_freed_count != 0u) return 7;
	return 0;
}

static int test_untrack_before_free_prevents_double_free(void)
{
	reset_mock();
	/* Ordering contract: untrack happens before free, so a block the worker
	 * already freed is not tracked and reclaim never touches it. */
	sdk_decode_track(blk(3));
	sdk_decode_untrack(blk(3));
	if (sdk_decode_reclaim(mock_free) != 0u) return 1;
	if (mock_freed(blk(3))) return 2;
	return 0;
}

static int test_null_and_untracked_are_ignored(void)
{
	reset_mock();
	sdk_decode_track(NULL);                /* ignored */
	if (sdk_decode_tracked_count() != 0u) return 1;
	sdk_decode_untrack(NULL);              /* ignored */
	sdk_decode_untrack(blk(4));            /* never tracked: ignored */
	if (sdk_decode_tracked_count() != 0u) return 2;
	if (sdk_decode_reclaim(mock_free) != 0u) return 3;
	return 0;
}

static int test_table_full_drops_extra_without_overrun(void)
{
	unsigned i;

	reset_mock();
	/* Fill the table exactly. */
	for (i = 0u; i < SDK_DECODE_MAX_TRACKED; i++)
		sdk_decode_track(blk(i));
	if (sdk_decode_tracked_count() != SDK_DECODE_MAX_TRACKED) return 1;
	/* Extra allocations are silently dropped (safer than overrunning). */
	sdk_decode_track(blk(SDK_DECODE_MAX_TRACKED));
	sdk_decode_track(blk(SDK_DECODE_MAX_TRACKED + 1u));
	if (sdk_decode_tracked_count() != SDK_DECODE_MAX_TRACKED) return 2;
	/* Reclaim frees exactly the tracked set; the dropped ones are not seen. */
	if (sdk_decode_reclaim(mock_free) != SDK_DECODE_MAX_TRACKED) return 3;
	if (mock_freed(blk(SDK_DECODE_MAX_TRACKED))) return 4;
	if (sdk_decode_tracked_count() != 0u) return 5;
	return 0;
}

static int test_reclaim_tolerates_null_free_fn(void)
{
	reset_mock();
	sdk_decode_track(blk(0));
	sdk_decode_track(blk(1));
	/* Should still clear the table and count, without calling through NULL. */
	if (sdk_decode_reclaim(NULL) != 2u) return 1;
	if (sdk_decode_tracked_count() != 0u) return 2;
	return 0;
}

static int test_table_extent_accessors(void)
{
	/* The firmware uses these to bound its cache clean/invalidate range. */
	if (sdk_decode_table_base() == NULL) return 1;
	if (sdk_decode_table_bytes() != SDK_DECODE_MAX_TRACKED * sizeof(void *))
		return 2;
	/* Cache-line aligned so a range flush cannot touch neighbouring data. */
	if (((unsigned long)(size_t)sdk_decode_table_base() & 31u) != 0u) return 3;
	return 0;
}

int main(void)
{
	struct { const char *name; int (*fn)(void); } tests[] = {
		{ "clean_decode_reclaims_nothing", test_clean_decode_reclaims_nothing },
		{ "killed_decode_reclaims_survivors", test_killed_decode_reclaims_survivors },
		{ "untrack_before_free_prevents_double_free", test_untrack_before_free_prevents_double_free },
		{ "null_and_untracked_are_ignored", test_null_and_untracked_are_ignored },
		{ "table_full_drops_extra_without_overrun", test_table_full_drops_extra_without_overrun },
		{ "reclaim_tolerates_null_free_fn", test_reclaim_tolerates_null_free_fn },
		{ "table_extent_accessors", test_table_extent_accessors },
	};
	unsigned i;
	int failures = 0;

	for (i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int rc = tests[i].fn();

		if (rc != 0) {
			printf("FAIL %s (rc=%d)\n", tests[i].name, rc);
			failures++;
		} else {
			printf("ok   %s\n", tests[i].name);
		}
	}
	if (failures) {
		printf("decode_reclaim_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("decode_reclaim_test: all passed\n");
	return 0;
}
