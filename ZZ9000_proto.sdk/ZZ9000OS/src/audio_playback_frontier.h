/*
 * AX playback DMA fill-frontier scheduling.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_PLAYBACK_FRONTIER_H
#define AUDIO_PLAYBACK_FRONTIER_H

#include <stdint.h>

/*
 * Only source-backed periods own space beyond the fill frontier. A zero-byte
 * fill is a retryable slot containing silence, not a reservation: decoded PCM
 * that arrives before the DMA reaches it can still replace that silence.
 */
static inline uint32_t audio_playback_frontier_after_fill(
	uint32_t fill_offset, uint32_t staged_bytes,
	uint32_t period_bytes, uint32_t ring_bytes)
{
	if (staged_bytes == 0U)
		return fill_offset;
	return (fill_offset + period_bytes) % ring_bytes;
}

#endif /* AUDIO_PLAYBACK_FRONTIER_H */
