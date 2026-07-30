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

static inline uint32_t audio_playback_ring_distance(
	uint32_t fill_offset, uint32_t dma_offset, uint32_t ring_bytes)
{
	return (fill_offset + ring_bytes - dma_offset) % ring_bytes;
}

static inline int audio_playback_frontier_needs_rebase(
	uint32_t fill_offset, uint32_t dma_offset,
	uint32_t target_ahead, uint32_t ring_bytes)
{
	uint32_t ahead = audio_playback_ring_distance(
		fill_offset, dma_offset, ring_bytes);

	return ahead == 0U || ahead > target_ahead;
}

static inline uint32_t audio_playback_source_offset(
	uint64_t staged_bytes, uint32_t capacity)
{
	return capacity != 0U ? (uint32_t)(staged_bytes % capacity) : 0U;
}

static inline void audio_playback_clear_periods(
	uint32_t *period_source_bytes, uint32_t period_count)
{
	uint32_t i;

	if (!period_source_bytes)
		return;
	for (i = 0U; i < period_count; i++)
		period_source_bytes[i] = 0U;
}

/*
 * Retire every tagged DMA period crossed since the preceding IRQ. The return
 * value is the source-byte total represented by those periods.
 */
static inline uint32_t audio_playback_retire_to(
	uint32_t *last_dma_offset, uint32_t dma_offset,
	uint32_t period_bytes, uint32_t ring_bytes,
	uint32_t *period_source_bytes, uint32_t period_count)
{
	uint32_t retired = 0U;
	uint32_t guard = period_count;

	if (!last_dma_offset || !period_source_bytes ||
	    period_bytes == 0U || ring_bytes == 0U)
		return 0U;
	while (*last_dma_offset != dma_offset && guard--) {
		uint32_t index = *last_dma_offset / period_bytes;

		if (index >= period_count)
			break;
		retired += period_source_bytes[index];
		period_source_bytes[index] = 0U;
		*last_dma_offset =
			(*last_dma_offset + period_bytes) % ring_bytes;
	}
	if (*last_dma_offset != dma_offset)
		*last_dma_offset = dma_offset;
	return retired;
}

#endif /* AUDIO_PLAYBACK_FRONTIER_H */
