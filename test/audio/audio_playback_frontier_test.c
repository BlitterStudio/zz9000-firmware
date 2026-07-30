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

int main(void)
{
	if (!check_temporary_shortage_is_retryable())
		return 1;
	if (!check_ring_wrap())
		return 2;
	return 0;
}
