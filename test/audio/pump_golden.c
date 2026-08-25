/*
 * Characterization golden for the pre-fabric AX playback pump.
 *
 * Verbatim capture of the sdk_mailbox.c pump fill machinery at 9b3654f;
 * see pump_golden.h for the seam contract. Computation lines are the
 * original ones, only g_audio_playback.* / g_pump_* statics became
 * struct members and the producer helpers became ops calls.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "pump_golden.h"
#include "audio_playback_frontier.h"
#include "sdk_mailbox.h"
#include "xil_cache.h"

static void golden_convert_reset(struct pump_golden *g)
{
	g->convert_rate = 0U;
	zz_audio_convert_reset(&g->convert);
}

static void golden_convert(struct pump_golden *g, const int16_t *pcm,
                           int16_t *slot, uint32_t rate, uint32_t src_frames)
{
	if (rate != g->convert_rate) {
		g->convert_rate = rate;
		zz_audio_convert_init(&g->convert, rate, 48000U);
	}
	if (g->convert.ratio == NULL) {
		/* Off-table decode rate (11.025/16/22.05 kHz and anything
		 * else outside the six advertised): no honest conversion
		 * exists, so this is an unusable geometry -- emit a
		 * silent period rather than wrong-speed audio with a
		 * stale tail. The stream still advances. */
		memset(slot, 0, PUMP_GOLDEN_PERIOD_BYTES);
		return;
	}
	zz_audio_convert_stream(&g->convert, pcm, slot,
	                        (uint16_t)src_frames,
	                        PUMP_GOLDEN_PERIOD_BYTES / 4);
}

/* Returns the number of source PCM bytes staged into this DMA period, or
 * zero when the slot contains silence (temporary shortage, fault, pause,
 * or drained end-of-stream). A zero-byte slot stays at the retryable
 * fill frontier; it is not committed as future silence. Per-period
 * metadata converts the staging cursor into an actual DMA-retirement
 * clock. -- audio_pump_fill_period(), verbatim. */
static uint32_t golden_fill_period(struct pump_golden *g,
	const struct pump_golden_source *source, uint8_t *slot)
{
	uint32_t rate;
	uint32_t channels;
	uint32_t src_frames;
	uint32_t src_bytes;
	uint32_t pull;
	uint32_t offset;
	uint32_t first;
	uint64_t available;
	uint8_t *ring;
	int16_t *pcm;
	uint32_t i;

	if (!source || source->faulted || !source->ring ||
	    source->capacity == 0U ||
	    source->produced_bytes < source->staged_bytes)
		goto silence;
	rate = source->sample_rate;
	channels = source->channels;
	if (rate == 0U || channels == 0U || channels > 2U)
		goto silence;
	if (source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		goto silence;
	src_frames = rate / 50U;
	if (src_frames == 0U || src_frames > (PUMP_GOLDEN_PERIOD_BYTES / 4U))
		goto silence;
	src_bytes = src_frames * channels * 2U;
	available = source->produced_bytes - source->staged_bytes;
	if (available >= src_bytes) {
		pull = src_bytes;
	} else if (!source->done || available == 0U) {
		goto silence;
	} else {
		pull = (uint32_t)available;
	}
	/* else: true end of stream (EOF fed, input fully consumed) with a
	 * final PCM tail shorter than one 20 ms period -- MP3 frames owe
	 * no alignment to rate/50. Drain it zero-padded; refusing partial
	 * pulls would pin used above zero and the stream could never
	 * report DONE. */

	/* Pull the source from the PCM ring. The decode side flushed these
	 * bytes before publishing pcm_ready_total, so a reader-side
	 * invalidate makes them visible on this core. */
	ring = source->ring;
	offset = audio_playback_source_offset(
		source->staged_bytes, source->capacity);
	first = source->capacity - offset;
	if (first > pull)
		first = pull;
	Xil_DCacheInvalidateRange((INTPTR)(ring + offset), first);
	memcpy(g->src, ring + offset, first);
	if (pull > first) {
		Xil_DCacheInvalidateRange((INTPTR)ring, pull - first);
		memcpy((uint8_t *)g->src + first, ring, pull - first);
	}
	if (pull < src_bytes)
		memset((uint8_t *)g->src + pull, 0, src_bytes - pull);
	if (!g->ops->stage(pull))
		goto silence;

	if (source->sample_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE) {
		uint8_t *bytes = (uint8_t *)g->src;

		for (i = 0U; i < src_bytes; i += 2U) {
			uint8_t high = bytes[i];

			bytes[i] = bytes[i + 1U];
			bytes[i + 1U] = high;
		}
	}

	pcm = g->src;
	if (channels == 1U) {
		for (i = 0; i < src_frames; i++) {
			g->stereo[2U * i] = g->src[i];
			g->stereo[2U * i + 1U] = g->src[i];
		}
		pcm = g->stereo;
	}
	if (rate == 48000U) {
		memcpy(slot, pcm, PUMP_GOLDEN_PERIOD_BYTES);
	} else {
		golden_convert(g, pcm, (int16_t *)slot, rate, src_frames);
	}
	return pull;
silence:
	memset(slot, 0, PUMP_GOLDEN_PERIOD_BYTES);
	return 0;
}

/* TX-fill half: sdk_mailbox_audio_playback_pump_isr(), verbatim. Called
 * from isr_audio (audio-formatter period IRQ, every 20 ms) so main-loop
 * load cannot starve the TX frontier. Integer-only. */
void pump_golden_isr(struct pump_golden *g)
{
	struct pump_golden_source source;
	uint8_t *tx = g->tx;
	uint32_t pos_period;
	uint32_t ahead;
	uint32_t guard;
	uint32_t retired;
	uint32_t ring_periods =
		PUMP_GOLDEN_RING_BYTES / PUMP_GOLDEN_PERIOD_BYTES;
	int staged_real = 0;

	if (g->session == 0U || g->paused)
		return;

	pos_period = (g->dma_count() % PUMP_GOLDEN_RING_BYTES);
	pos_period -= pos_period % PUMP_GOLDEN_PERIOD_BYTES;

	/* Retire every period the DMA advanced through since the preceding
	 * IRQ. Each slot records exactly how many source bytes were staged
	 * into it; silence contributes zero. This is the playback clock --
	 * not the decoder acknowledgement or the TX-fill frontier. */
	retired = audio_playback_retire_to(
		&g->last_dma_offset, pos_period,
		PUMP_GOLDEN_PERIOD_BYTES, PUMP_GOLDEN_RING_BYTES,
		g->period_source_bytes, AUDIO_NUM_PERIODS);
	if (retired != 0U && g->ops->retire)
		g->ops->retire(retired);

	if (!g->ops->snapshot(&source)) {
		g->session = 0U;
		g->source_kind = PUMP_GOLDEN_SOURCE_NONE;
		return;
	}

	/* If the DMA caught up with (or passed) the fill frontier, it has
	 * actually reached an unfilled silence slot. Count that played
	 * underrun (not speculative attempts to fill future periods), then
	 * restart one period ahead. Circular distance: bias by the ring
	 * size BEFORE the modulo -- a plain u32 (fill - pos) % RING is
	 * wrong on wrap because 2^32 is not a multiple of the 30720-byte
	 * ring. */
	if (audio_playback_frontier_needs_rebase(
	        g->fill_offset, pos_period,
	        PUMP_GOLDEN_TARGET_AHEAD, PUMP_GOLDEN_RING_BYTES)) {
		if (!source.done && !source.faulted) {
			if (g->ops->underrun)
				g->ops->underrun();
		}
		g->fill_offset =
		    (pos_period + PUMP_GOLDEN_PERIOD_BYTES) %
		    PUMP_GOLDEN_RING_BYTES;
	}

	guard = PUMP_GOLDEN_RING_BYTES / PUMP_GOLDEN_PERIOD_BYTES;
	while (guard--) {
		uint32_t index;
		uint32_t staged;
		uint32_t next_fill;

		ahead = audio_playback_ring_distance(
			g->fill_offset, pos_period,
			PUMP_GOLDEN_RING_BYTES);
		/* Stop AT the target, never past it: the frontier must stay
		 * inside [PERIOD, TARGET_AHEAD] so the caught-up reset above
		 * only fires on a genuine DMA overrun. */
		if (ahead >= PUMP_GOLDEN_TARGET_AHEAD)
			break;   /* frontier far enough ahead */
		index = g->fill_offset / PUMP_GOLDEN_PERIOD_BYTES;
		staged = golden_fill_period(
			g, &source, tx + g->fill_offset);
		g->period_source_bytes[index] = staged;
		if (staged != 0U)
			staged_real = 1;
		/* The TX ring is plain cacheable DDR (no TLB override) and
		 * the audio formatter DMA does not snoop: push the period
		 * to DRAM before the frontier advances over it. */
		Xil_DCacheFlushRange(
		    (INTPTR)(tx + g->fill_offset),
		    PUMP_GOLDEN_PERIOD_BYTES);
		next_fill = audio_playback_frontier_after_fill(
			g->fill_offset, staged,
			PUMP_GOLDEN_PERIOD_BYTES, PUMP_GOLDEN_RING_BYTES);
		if (next_fill == g->fill_offset)
			break;
		g->fill_offset = next_fill;
		/* Refresh the published source cursor before filling another
		 * period in this same IRQ. */
		if (!g->ops->snapshot(&source))
			memset(&source, 0, sizeof(source));
	}

	/* Play-out tail tracking. This ISR fires once per formatter period,
	 * so each call is one DMA period elapsed. While real PCM is
	 * flowing, pump_tail_pending stays armed; once the source is
	 * exhausted the loop only stages silence, and after a whole ring
	 * of silence periods the DMA has played the last real audio out
	 * of the TX ring. Only then may end-of-stream drop. */
	if (g->source_kind == PUMP_GOLDEN_SOURCE_STREAM) {
		if (staged_real) {
			g->silence_run = 0U;
			if (g->ops->tail_real)
				g->ops->tail_real();
		} else if (g->ops->tail_pending &&
		           g->ops->tail_pending()) {
			if (g->silence_run < ring_periods)
				g->silence_run++;
			if (g->silence_run >= ring_periods &&
			    g->ops->tail_drained)
				g->ops->tail_drained();
		}
	}
}

/* audio_playback_start(): the state half the ring sees (the formatter
 * re-point / conditional re-init is transport; the fabric owns it after
 * the refactor and audio_fabric_test covers it separately). */
void pump_golden_start(struct pump_golden *g, uint32_t source_kind,
                       uint32_t session)
{
	uint32_t pos;

	g->session = 0U;
	golden_convert_reset(g);
	pos = g->dma_count() % PUMP_GOLDEN_RING_BYTES;
	pos -= pos % PUMP_GOLDEN_PERIOD_BYTES;
	g->fill_offset =
		(pos + PUMP_GOLDEN_PERIOD_BYTES) % PUMP_GOLDEN_RING_BYTES;
	g->last_dma_offset = pos;
	audio_playback_clear_periods(
		g->period_source_bytes, AUDIO_NUM_PERIODS);
	g->silence_run = 0U;
	g->source_kind = source_kind;
	g->paused = 0U;
	/* Publish the session LAST: the next audio IRQ may now stage PCM. */
	g->session = session;
}

void pump_golden_stop(struct pump_golden *g)
{
	g->session = 0U;
	g->source_kind = PUMP_GOLDEN_SOURCE_NONE;
	g->paused = 0U;
	audio_playback_clear_periods(
		g->period_source_bytes, AUDIO_NUM_PERIODS);
	g->silence_run = 0U;
	/* audio_silence(): the TX-ring effect only. */
	memset(g->tx, 0, PUMP_GOLDEN_RING_BYTES);
}

void pump_golden_pause(struct pump_golden *g)
{
	g->paused = 1U;
	audio_playback_clear_periods(
		g->period_source_bytes, AUDIO_NUM_PERIODS);
	memset(g->tx, 0, PUMP_GOLDEN_RING_BYTES);
}
