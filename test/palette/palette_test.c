/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Unit tests for the primary-CLUT shadow behind SDK_OP_QUERY_PALETTE.
 *
 * The video formatter's palette registers are write-only, so a query can only
 * ever be as correct as this shadow. These tests pin the two things hardware
 * cannot easily show us: that a known synthetic palette round-trips exactly,
 * and that the big-endian packing is byte-for-byte what the mailbox ABI
 * promises regardless of host byte order.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_palette.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *fmt, unsigned long a, unsigned long b,
                 unsigned long c)
{
	printf("FAIL: ");
	printf(fmt, a, b, c);
	printf("\n");
	failures++;
}

/* A palette with a distinct, non-symmetric value per index, so a swapped or
 * off-by-one entry cannot accidentally match. */
static uint32_t synthetic_entry(uint32_t idx)
{
	uint32_t r = (idx * 7U) & 0xFFU;
	uint32_t g = (idx * 13U + 5U) & 0xFFU;
	uint32_t b = (idx * 29U + 17U) & 0xFFU;

	return (r << 16) | (g << 8) | b;
}

static void load_synthetic(void)
{
	uint32_t i;

	sdk_palette_reset();
	for (i = 0U; i < SDK_PALETTE_ENTRIES; i++) {
		/* The RTG path passes the index in the high byte, exactly as
		 * it is written to the hardware register. */
		sdk_palette_set(i, (i << 24) | synthetic_entry(i));
	}
}

static void test_set_get_round_trip(void)
{
	uint32_t i;

	load_synthetic();
	for (i = 0U; i < SDK_PALETTE_ENTRIES; i++) {
		uint32_t got = sdk_palette_get(i);

		if (got != synthetic_entry(i)) {
			fail("entry %lu: got 0x%06lx expected 0x%06lx",
			     (unsigned long)i, (unsigned long)got,
			     (unsigned long)synthetic_entry(i));
			return;
		}
	}
}

/* The index byte the hardware write carries must not survive into the reply:
 * the ABI promises 0x00RRGGBB. */
static void test_index_byte_is_stripped(void)
{
	sdk_palette_reset();
	sdk_palette_set(0x12U, 0x12ABCDEFUL);
	if (sdk_palette_get(0x12U) != 0x00ABCDEFUL) {
		fail("index byte leaked: got 0x%08lx expected 0x00abcdef%lu%lu",
		     (unsigned long)sdk_palette_get(0x12U), 0UL, 0UL);
	}
}

static void test_unset_entries_read_zero(void)
{
	sdk_palette_reset();
	sdk_palette_set(5U, 0x00112233UL);
	if (sdk_palette_get(6U) != 0U) {
		fail("unset entry 6: got 0x%08lx expected 0%lu%lu",
		     (unsigned long)sdk_palette_get(6U), 0UL, 0UL);
	}
}

/* The wire format is big-endian 0x00RRGGBB per entry. Checking raw bytes is
 * the point: a host-order store would pass a value comparison on a
 * little-endian test box and corrupt every reply on the card. */
static void test_pack_is_big_endian(void)
{
	uint8_t buffer[8];
	uint32_t written;

	sdk_palette_reset();
	sdk_palette_set(0U, 0x00AABBCCUL);
	sdk_palette_set(1U, 0x00010203UL);

	memset(buffer, 0xA5, sizeof(buffer));
	written = sdk_palette_pack_be(buffer, 0U, 2U);
	if (written != 8U) {
		fail("pack wrote %lu bytes, expected 8%lu%lu",
		     (unsigned long)written, 0UL, 0UL);
		return;
	}
	if (buffer[0] != 0x00U || buffer[1] != 0xAAU ||
	    buffer[2] != 0xBBU || buffer[3] != 0xCCU) {
		fail("entry 0 bytes: %02lx %02lx %02lx",
		     (unsigned long)buffer[1], (unsigned long)buffer[2],
		     (unsigned long)buffer[3]);
	}
	if (buffer[4] != 0x00U || buffer[5] != 0x01U ||
	    buffer[6] != 0x02U || buffer[7] != 0x03U) {
		fail("entry 1 bytes: %02lx %02lx %02lx",
		     (unsigned long)buffer[5], (unsigned long)buffer[6],
		     (unsigned long)buffer[7]);
	}
}

/* Firmware must return the exact synthetic palette a caller uploaded --
 * this is the plan's "host/service test returns a known synthetic palette
 * exactly" scenario, covering first, middle, and last entries. */
static void test_pack_returns_synthetic_palette_exactly(void)
{
	uint8_t buffer[SDK_PALETTE_ENTRIES * SDK_PALETTE_ENTRY_BYTES];
	uint32_t written;
	uint32_t i;

	load_synthetic();
	memset(buffer, 0xA5, sizeof(buffer));
	written = sdk_palette_pack_be(buffer, 0U, SDK_PALETTE_ENTRIES);
	if (written != sizeof(buffer)) {
		fail("full pack wrote %lu bytes, expected %lu%lu",
		     (unsigned long)written, (unsigned long)sizeof(buffer),
		     0UL);
		return;
	}

	for (i = 0U; i < SDK_PALETTE_ENTRIES; i++) {
		uint32_t got = ((uint32_t)buffer[i * 4U] << 24) |
		               ((uint32_t)buffer[i * 4U + 1U] << 16) |
		               ((uint32_t)buffer[i * 4U + 2U] << 8) |
		               (uint32_t)buffer[i * 4U + 3U];

		if (got != synthetic_entry(i)) {
			fail("packed entry %lu: got 0x%08lx expected 0x%06lx",
			     (unsigned long)i, (unsigned long)got,
			     (unsigned long)synthetic_entry(i));
			return;
		}
	}
}

/* A sub-window must start at the right entry, not at entry 0. */
static void test_pack_honours_start_offset(void)
{
	uint8_t buffer[16];
	uint32_t written;
	uint32_t i;

	load_synthetic();
	memset(buffer, 0xA5, sizeof(buffer));
	written = sdk_palette_pack_be(buffer, 128U, 4U);
	if (written != 16U) {
		fail("window pack wrote %lu bytes, expected 16%lu%lu",
		     (unsigned long)written, 0UL, 0UL);
		return;
	}

	for (i = 0U; i < 4U; i++) {
		uint32_t got = ((uint32_t)buffer[i * 4U] << 24) |
		               ((uint32_t)buffer[i * 4U + 1U] << 16) |
		               ((uint32_t)buffer[i * 4U + 2U] << 8) |
		               (uint32_t)buffer[i * 4U + 3U];

		if (got != synthetic_entry(128U + i)) {
			fail("window entry %lu: got 0x%08lx expected 0x%06lx",
			     (unsigned long)(128U + i), (unsigned long)got,
			     (unsigned long)synthetic_entry(128U + i));
			return;
		}
	}
}

/* The last entry alone: start + count lands exactly on the bound and must
 * still be accepted. */
static void test_pack_accepts_last_entry(void)
{
	uint8_t buffer[4];

	load_synthetic();
	if (sdk_palette_pack_be(buffer, SDK_PALETTE_ENTRIES - 1U, 1U) != 4U) {
		fail("last entry rejected%lu%lu%lu", 0UL, 0UL, 0UL);
	}
}

/* Out-of-range windows must write nothing at all, not a truncated reply:
 * the caller's buffer is sized from count, and a partial write would leave
 * it holding stale bytes that look like palette data. */
static void test_pack_rejects_out_of_range(void)
{
	uint8_t buffer[SDK_PALETTE_ENTRIES * SDK_PALETTE_ENTRY_BYTES];
	static const uint8_t poison = 0xA5U;
	uint32_t i;

	load_synthetic();

	memset(buffer, poison, sizeof(buffer));
	if (sdk_palette_pack_be(buffer, 0U, 0U) != 0U) {
		fail("zero count accepted%lu%lu%lu", 0UL, 0UL, 0UL);
	}
	if (sdk_palette_pack_be(buffer, SDK_PALETTE_ENTRIES, 1U) != 0U) {
		fail("start past end accepted%lu%lu%lu", 0UL, 0UL, 0UL);
	}
	if (sdk_palette_pack_be(buffer, 200U, 100U) != 0U) {
		fail("window past end accepted%lu%lu%lu", 0UL, 0UL, 0UL);
	}
	if (sdk_palette_pack_be(buffer, 0U, SDK_PALETTE_ENTRIES + 1U) != 0U) {
		fail("oversized count accepted%lu%lu%lu", 0UL, 0UL, 0UL);
	}
	/* start + count must not wrap past the bound check. */
	if (sdk_palette_pack_be(buffer, 0xFFFFFFFFUL, 2U) != 0U) {
		fail("wrapping window accepted%lu%lu%lu", 0UL, 0UL, 0UL);
	}
	if (sdk_palette_pack_be(0, 0U, 4U) != 0U) {
		fail("null destination accepted%lu%lu%lu", 0UL, 0UL, 0UL);
	}

	for (i = 0U; i < sizeof(buffer); i++) {
		if (buffer[i] != poison) {
			fail("rejected pack wrote byte %lu (0x%02lx)%lu",
			     (unsigned long)i, (unsigned long)buffer[i], 0UL);
			return;
		}
	}
}

int main(void)
{
	test_set_get_round_trip();
	test_index_byte_is_stripped();
	test_unset_entries_read_zero();
	test_pack_is_big_endian();
	test_pack_returns_synthetic_palette_exactly();
	test_pack_honours_start_offset();
	test_pack_accepts_last_entry();
	test_pack_rejects_out_of_range();

	if (failures) {
		printf("palette_test: %d failure(s)\n", failures);
		return 1;
	}

	printf("palette_test: all tests passed\n");
	return 0;
}
