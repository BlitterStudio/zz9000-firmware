/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Readable shadow of the primary display CLUT. See sdk_palette.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_palette.h"

/* RTG palette uploads run on core 0 while the mailbox presenter runs on core
 * 1. The generation is a sequence lock: odd while a writer owns the shadow,
 * even only after its palette words are fully published. */
static volatile uint32_t g_palette[SDK_PALETTE_ENTRIES];
static volatile uint32_t g_palette_generation;

static void sdk_palette_begin_update(void)
{
	++g_palette_generation;
}

static void sdk_palette_end_update(void)
{
	++g_palette_generation;
	if (g_palette_generation == 0U)
		g_palette_generation = 2U;
}

void sdk_palette_set(uint32_t idx, uint32_t xrgb)
{
	uint32_t slot = idx & (SDK_PALETTE_ENTRIES - 1U);
	uint32_t value = xrgb & 0x00FFFFFFU;

	if (g_palette[slot] != value) {
		sdk_palette_begin_update();
		g_palette[slot] = value;
		sdk_palette_end_update();
	}
}

uint32_t sdk_palette_get(uint32_t idx)
{
	return g_palette[idx & (SDK_PALETTE_ENTRIES - 1U)];
}

uint32_t sdk_palette_generation(void)
{
	return g_palette_generation;
}

int sdk_palette_snapshot(uint32_t entries[SDK_PALETTE_ENTRIES],
                         uint32_t *generation)
{
	uint32_t before;
	uint32_t after;
	uint32_t i;

	if (!entries || !generation)
		return 0;
	before = g_palette_generation;
	if (before & 1U)
		return 0;
	for (i = 0U; i < SDK_PALETTE_ENTRIES; ++i)
		entries[i] = g_palette[i];
	after = g_palette_generation;
	if (before != after || (after & 1U))
		return 0;
	*generation = after;
	return 1;
}

uint32_t sdk_palette_pack_be(void *dst, uint32_t start, uint32_t count)
{
	uint8_t *out = (uint8_t *)dst;
	uint32_t i;

	if (!out || count == 0U || count > SDK_PALETTE_ENTRIES ||
	    start > SDK_PALETTE_ENTRIES ||
	    (start + count) > SDK_PALETTE_ENTRIES)
		return 0U;

	for (i = 0U; i < count; i++) {
		uint32_t value = g_palette[start + i];

		/* Byte-at-a-time: the destination is a caller-supplied shared
		 * buffer offset with no alignment guarantee. */
		out[i * 4U + 0U] = (uint8_t)(value >> 24);
		out[i * 4U + 1U] = (uint8_t)(value >> 16);
		out[i * 4U + 2U] = (uint8_t)(value >> 8);
		out[i * 4U + 3U] = (uint8_t)value;
	}

	return count * SDK_PALETTE_ENTRY_BYTES;
}

void sdk_palette_reset(void)
{
	uint32_t i;

	sdk_palette_begin_update();
	for (i = 0U; i < SDK_PALETTE_ENTRIES; i++)
		g_palette[i] = 0U;
	sdk_palette_end_update();
}
