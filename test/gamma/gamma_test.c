/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Unit tests for the output gamma LUT curve loaded into the video
 * formatter (gamma.c). The ARM ships one curve for the red, green and
 * blue tables alike, so these tests pin the curve properties hardware
 * cannot easily show: identity at 1.00, monotonic growth, fixed black
 * and white endpoints, and the accepted range.
 *
 * The pinned entries were computed independently of gamma.c with
 * round-half-up arithmetic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gamma.h"
#include <stdio.h>

static int failures;

static void check(int ok, const char *what, long a, long b)
{
	if (!ok) {
		printf("FAIL: %s (%ld != %ld)\n", what, a, b);
		failures++;
	}
}

static void test_identity(void)
{
	uint8_t lut[GAMMA_LUT_ENTRIES];
	uint32_t i;

	check(gamma_table_build(lut, GAMMA_HUNDREDTHS_IDENTITY) == 1,
		"identity gamma builds", 1, 0);
	for (i = 0; i < GAMMA_LUT_ENTRIES; i++)
		if (lut[i] != i) {
			check(0, "identity entry", lut[i], i);
			return;
		}
	check(1, "identity table", 0, 0);
}

static void test_pinned_entries(void)
{
	/* gamma 2.00: out = round(255 * sqrt(in/255)) */
	static const uint8_t g200[] = { 16, 64, 128, 181, 226, 254 };
	/* gamma 0.50: out = round(255 * (in/255)^2) */
	static const uint8_t g050[] = { 0, 1, 16, 64, 157, 253 };
	static const uint32_t at[] = { 1, 16, 64, 128, 200, 254 };
	uint8_t lut[GAMMA_LUT_ENTRIES];
	size_t k;

	check(gamma_table_build(lut, 200) == 1, "gamma 2.00 builds", 1, 0);
	for (k = 0; k < sizeof(at) / sizeof(at[0]); k++)
		check(lut[at[k]] == g200[k], "gamma 2.00 entry",
			lut[at[k]], g200[k]);

	check(gamma_table_build(lut, 50) == 1, "gamma 0.50 builds", 1, 0);
	for (k = 0; k < sizeof(at) / sizeof(at[0]); k++)
		check(lut[at[k]] == g050[k], "gamma 0.50 entry",
			lut[at[k]], g050[k]);
}

static void test_direction(void)
{
	/* The issue text fixes the direction: values above 1.0 brighten
	 * (raise the midtones), values below 1.0 darken them. */
	uint8_t lut[GAMMA_LUT_ENTRIES];

	gamma_table_build(lut, 200);
	check(lut[64] > 64, "gamma 2.00 brightens midtones", lut[64], 64);
	check(lut[128] > 128, "gamma 2.00 brightens half gray", lut[128], 128);

	gamma_table_build(lut, 50);
	check(lut[64] < 64, "gamma 0.50 darkens midtones", lut[64], 64);
	check(lut[128] < 128, "gamma 0.50 darkens half gray", lut[128], 128);
}

static void test_monotonic_endpoints(void)
{
	static const uint16_t gammas[] = {
		GAMMA_HUNDREDTHS_MIN, 50, 100, 180, 200, 350,
		GAMMA_HUNDREDTHS_MAX
	};
	uint8_t lut[GAMMA_LUT_ENTRIES];
	size_t g;
	uint32_t i;

	for (g = 0; g < sizeof(gammas) / sizeof(gammas[0]); g++) {
		check(gamma_table_build(lut, gammas[g]) == 1,
			"gamma builds", gammas[g], 0);
		check(lut[0] == 0, "black endpoint fixed", lut[0], 0);
		check(lut[GAMMA_LUT_ENTRIES - 1] == 255,
			"white endpoint fixed", lut[GAMMA_LUT_ENTRIES - 1], 255);
		for (i = 1; i < GAMMA_LUT_ENTRIES; i++)
			if (lut[i] < lut[i - 1]) {
				check(0, "monotonic", lut[i], lut[i - 1]);
				return;
			}
	}
	check(1, "monotonic across gammas", 0, 0);
}

static void test_range(void)
{
	uint8_t lut[GAMMA_LUT_ENTRIES];
	uint8_t snapshot[GAMMA_LUT_ENTRIES];
	static const uint16_t bad[] = { 0, 1, 19, 501, 1000, 65535 };
	size_t k;

	/* A rejected value must not partially overwrite a loaded table. */
	gamma_table_build(lut, 200);
	for (k = 0; k < GAMMA_LUT_ENTRIES; k++)
		snapshot[k] = lut[k];
	for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		uint32_t i;
		int same = 1;

		check(gamma_table_build(lut, bad[k]) == 0,
			"out-of-range rejected", bad[k], 0);
		for (i = 0; i < GAMMA_LUT_ENTRIES; i++)
			if (lut[i] != snapshot[i])
				same = 0;
		check(same, "rejected build leaves lut untouched", bad[k], 0);
	}
}

int main(void)
{
	test_identity();
	test_pinned_entries();
	test_direction();
	test_monotonic_endpoints();
	test_range();

	if (failures) {
		printf("gamma_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("gamma_test: all pass\n");
	return 0;
}
