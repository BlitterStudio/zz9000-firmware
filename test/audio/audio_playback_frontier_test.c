/*
 * AX playback fill-frontier scheduling regression.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>

#include "audio_playback_frontier.h"

#define PERIOD_BYTES 3840U
#define RING_BYTES   30720U

static int check_temporary_shortage_is_retryable(void)
{
	uint32_t fill = PERIOD_BYTES;

	fill = audio_playback_frontier_after_fill(
		fill, PERIOD_BYTES, PERIOD_BYTES, RING_BYTES);
	if (fill != 2U * PERIOD_BYTES)
		return 0;

	/* A speculative silence period must not reserve this slot. Newly
	 * decoded PCM must be able to retry the same frontier. */
	fill = audio_playback_frontier_after_fill(
		fill, 0U, PERIOD_BYTES, RING_BYTES);
	if (fill != 2U * PERIOD_BYTES)
		return 0;

	fill = audio_playback_frontier_after_fill(
		fill, PERIOD_BYTES, PERIOD_BYTES, RING_BYTES);
	return fill == 3U * PERIOD_BYTES;
}

static int check_ring_wrap(void)
{
	return audio_playback_frontier_after_fill(
		       RING_BYTES - PERIOD_BYTES, PERIOD_BYTES,
		       PERIOD_BYTES, RING_BYTES) == 0U;
}

static int check_retirement_across_ring_wrap(void)
{
	uint32_t periods[RING_BYTES / PERIOD_BYTES] = {0U};
	uint32_t last = 6U * PERIOD_BYTES;
	uint32_t retired;

	periods[6] = 100U;
	periods[7] = 200U;
	periods[0] = 300U;
	retired = audio_playback_retire_to(
		&last, PERIOD_BYTES, PERIOD_BYTES, RING_BYTES,
		periods, RING_BYTES / PERIOD_BYTES);
	return retired == 600U && last == PERIOD_BYTES &&
	       periods[6] == 0U && periods[7] == 0U &&
	       periods[0] == 0U;
}

static int check_underrun_rebase(void)
{
	uint32_t fill = 3U * PERIOD_BYTES;
	uint32_t dma = 2U * PERIOD_BYTES;

	if (audio_playback_ring_distance(fill, dma, RING_BYTES) !=
	    PERIOD_BYTES)
		return 0;
	if (audio_playback_frontier_needs_rebase(
	        fill, dma, 4U * PERIOD_BYTES, RING_BYTES))
		return 0;
	dma = fill;
	if (!audio_playback_frontier_needs_rebase(
	        fill, dma, 4U * PERIOD_BYTES, RING_BYTES))
		return 0;
	fill = (dma + PERIOD_BYTES) % RING_BYTES;
	return !audio_playback_frontier_needs_rebase(
		fill, dma, 4U * PERIOD_BYTES, RING_BYTES);
}

static int check_lifecycle_clear_and_64bit_offset(void)
{
	uint32_t periods[4] = {1U, 2U, 3U, 4U};
	uint32_t i;
	uint64_t cursor = UINT64_C(0xfffffffc) + 4608U;

	audio_playback_clear_periods(periods, 4U);
	for (i = 0U; i < 4U; i++) {
		if (periods[i] != 0U)
			return 0;
	}
	return audio_playback_source_offset(cursor, 32768U) ==
	       (uint32_t)(cursor % 32768U);
}

int main(void)
{
	if (!check_temporary_shortage_is_retryable())
		return 1;
	if (!check_ring_wrap())
		return 2;
	if (!check_retirement_across_ring_wrap())
		return 3;
	if (!check_underrun_rebase())
		return 4;
	if (!check_lifecycle_clear_and_64bit_offset())
		return 5;
	return 0;
}
