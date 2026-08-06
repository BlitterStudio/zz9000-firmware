/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Readable shadow of the primary display CLUT. See sdk_palette.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_palette.h"

/* Written only by the RTG palette upload path and read only by the mailbox
 * handler, both 32-bit aligned word accesses, so no lock is needed: a racing
 * query sees either the old or the new entry, never a torn one. */
static uint32_t g_palette[SDK_PALETTE_ENTRIES];

void sdk_palette_set(uint32_t idx, uint32_t xrgb)
{
	g_palette[idx & (SDK_PALETTE_ENTRIES - 1U)] = xrgb & 0x00FFFFFFU;
}

uint32_t sdk_palette_get(uint32_t idx)
{
	return g_palette[idx & (SDK_PALETTE_ENTRIES - 1U)];
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

	for (i = 0U; i < SDK_PALETTE_ENTRIES; i++)
		g_palette[i] = 0U;
}
