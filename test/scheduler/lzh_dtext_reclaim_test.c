/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host test for the LZH dtext reset-time reclaim state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "lzh/zz9k_lzh.h"

/* zz9k_lzh_support.c references these bitio globals from zz9k_lzh_bitio.c.
 * The reclaim tests never call the I/O setup path, so inert host definitions
 * are sufficient. */
FILE *infile;
FILE *outfile;

extern unsigned char *dtext;

static unsigned flush_count;
static unsigned invalidate_count;

void zz9k_lzh_flush_dtext_reclaim(void)
{
	flush_count++;
}

void zz9k_lzh_invalidate_dtext_reclaim(void)
{
	invalidate_count++;
}

static unsigned char **tracked_slot(void)
{
	return (unsigned char **)zz9k_lzh_dtext_reclaim_base();
}

static void reset_counts(void)
{
	flush_count = 0u;
	invalidate_count = 0u;
}

static int test_reclaim_state_extent_is_cache_line(void)
{
	void *base = zz9k_lzh_dtext_reclaim_base();
	unsigned bytes = zz9k_lzh_dtext_reclaim_bytes();

	if (base == NULL) return 1;
	if (bytes != 32u) return 2;
	if (((uintptr_t)base & 31u) != 0u) return 3;
	return 0;
}

static int test_track_cleans_non_null_pointer(void)
{
	unsigned char block[16];

	reset_counts();
	zz9k_lzh_track_dtext(block);
	if (dtext != block) return 1;
	if (*tracked_slot() != block) return 2;
	if (flush_count != 1u) return 3;
	if (invalidate_count != 0u) return 4;
	zz9k_lzh_disarm_dtext();
	return 0;
}

static int test_disarm_cleans_null_pointer(void)
{
	unsigned char block[16];

	zz9k_lzh_track_dtext(block);
	reset_counts();
	zz9k_lzh_disarm_dtext();
	if (dtext != NULL) return 1;
	if (*tracked_slot() != NULL) return 2;
	if (flush_count != 1u) return 3;
	if (invalidate_count != 0u) return 4;
	return 0;
}

static int test_free_clears_tracking_before_release(void)
{
	unsigned char *block = malloc(16u);

	if (!block) return 1;
	zz9k_lzh_track_dtext(block);
	reset_counts();
	zz9k_lzh_free_dtext();
	if (dtext != NULL) return 2;
	if (*tracked_slot() != NULL) return 3;
	if (flush_count != 1u) return 4;
	if (invalidate_count != 0u) return 5;
	return 0;
}

static int test_reclaim_invalidates_then_cleans_survivor(void)
{
	unsigned char *block = malloc(16u);

	if (!block) return 1;
	zz9k_lzh_track_dtext(block);
	reset_counts();
	zz9k_lzh_reclaim();
	if (dtext != NULL) return 2;
	if (*tracked_slot() != NULL) return 3;
	if (invalidate_count != 1u) return 4;
	if (flush_count != 1u) return 5;
	return 0;
}

int main(void)
{
	struct { const char *name; int (*fn)(void); } tests[] = {
		{ "reclaim_state_extent_is_cache_line", test_reclaim_state_extent_is_cache_line },
		{ "track_cleans_non_null_pointer", test_track_cleans_non_null_pointer },
		{ "disarm_cleans_null_pointer", test_disarm_cleans_null_pointer },
		{ "free_clears_tracking_before_release", test_free_clears_tracking_before_release },
		{ "reclaim_invalidates_then_cleans_survivor", test_reclaim_invalidates_then_cleans_survivor },
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
		printf("lzh_dtext_reclaim_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("lzh_dtext_reclaim_test: all passed\n");
	return 0;
}
