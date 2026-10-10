#include "audio_pump_preconvert.h"

#include <string.h>

#include "sdk_mailbox.h"
#include "xil_cache.h"
#include "sdk_smp_lock.h"

void audio_pump_preconvert_reset(struct audio_pump_preconvert *state,
	uint8_t *ring, uint32_t capacity)
{
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
	state->ring = ring;
	state->capacity = capacity;
	if (ring && capacity)
		memset(ring, 0, capacity);
}

uint32_t audio_pump_preconvert_used(
	const struct audio_pump_preconvert *state)
{
	uint32_t used;

	if (!state)
		return 0U;
	used = state->produced - state->staged;
	return used <= state->capacity ? used : state->capacity;
}

static void preconvert_copy_source(struct audio_pump_preconvert *state,
	const struct audio_pump_preconvert_source *source,
	uint32_t offset, uint32_t pull, uint32_t source_bytes)
{
	uint8_t *dst = (uint8_t *)state->source;
	uint32_t first = source->capacity - offset;
	uint32_t i;

	if (first > pull)
		first = pull;
	Xil_DCacheInvalidateRange((INTPTR)(source->ring + offset), first);
	memcpy(dst, source->ring + offset, first);
	if (pull > first) {
		Xil_DCacheInvalidateRange((INTPTR)source->ring, pull - first);
		memcpy(dst + first, source->ring, pull - first);
	}
	if (pull < source_bytes)
		memset(dst + pull, 0, source_bytes - pull);
	if (source->sample_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE) {
		for (i = 0U; i < source_bytes; i += 2U) {
			uint8_t high = dst[i];

			dst[i] = dst[i + 1U];
			dst[i + 1U] = high;
		}
	}
}

static void preconvert_expand_mono(struct audio_pump_preconvert *state,
	uint32_t frames)
{
	uint32_t i = frames;

	while (i != 0U) {
		int16_t sample;

		i--;
		sample = state->source[i];
		state->source[i * 2U] = sample;
		state->source[i * 2U + 1U] = sample;
	}
}

static void preconvert_write_period(struct audio_pump_preconvert *state,
                                    const int16_t *samples)
{
	const uint8_t *src = (const uint8_t *)samples;
	uint32_t offset = state->produced % state->capacity;
	uint32_t first = state->capacity - offset;

	if (first > AUDIO_PUMP_PRECONVERT_PERIOD_BYTES)
		first = AUDIO_PUMP_PRECONVERT_PERIOD_BYTES;
	memcpy(state->ring + offset, src, first);
	Xil_DCacheFlushRange((INTPTR)(state->ring + offset), first);
	if (first < AUDIO_PUMP_PRECONVERT_PERIOD_BYTES) {
		uint32_t second = AUDIO_PUMP_PRECONVERT_PERIOD_BYTES - first;

		memcpy(state->ring, src + first, second);
		Xil_DCacheFlushRange((INTPTR)state->ring, second);
	}
	state->produced += AUDIO_PUMP_PRECONVERT_PERIOD_BYTES;
	/* The 32-bit cursors wrap after ~6.2 h at 192 kB/s, and the ring
	 * capacity does not divide 2^32, so a natural wrap would jump every
	 * produced % capacity position while staged still holds the old
	 * lap (PR #88 review). Rebase both cursors by the capacity-aligned
	 * prefix of the lower one: each keeps its modulo position, the
	 * used-bytes difference is unchanged, and neither underflows. */
	if (state->produced >
	    0xFFFFFFFFU - 16U * AUDIO_PUMP_PRECONVERT_PERIOD_BYTES) {
		uint32_t lowest = (state->staged < state->produced) ?
			state->staged : state->produced;
		uint32_t rebase = lowest - lowest % state->capacity;
		/* The audio ISR concurrently advances staged (and observes
		 * produced); the two-word adjustment must be atomic or a
		 * period staged between the load and store is overwritten
		 * and the compositor repeats/skips a period (PR #88
		 * review). IRQ-safe critical section, held for two stores. */
		uint32_t irq_state = smp_local_irq_save();

		state->produced -= rebase;
		state->staged -= rebase;
		smp_local_irq_restore(irq_state);
	}
}

int audio_pump_preconvert_fill(struct audio_pump_preconvert *state,
	const struct audio_pump_preconvert_source *source,
	uint64_t *source_consumed)
{
	uint32_t source_frames;
	uint32_t out_frames;
	uint32_t periods;
	uint32_t period;
	uint32_t source_bytes;
	uint64_t available;
	uint32_t pull;
	uint32_t offset;

	if (!state || !source || !source_consumed || !state->ring ||
	    state->capacity < AUDIO_PUMP_PRECONVERT_PERIOD_BYTES ||
	    !source->ring || source->capacity == 0U ||
	    source->produced < source->consumed ||
	    source->sample_rate == 0U || source->channels == 0U ||
	    source->channels > 2U ||
	    (source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	     source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE))
		return -1;
	/* Space check keeps the rebuild reserve below the staged cursor
	 * (see AUDIO_PUMP_PRECONVERT_REPLAY_KEEP). 11025's 20 ms period
	 * is 220.5 frames, so that rate converts a 40 ms quantum (441
	 * frames -> 1920) and the rational position lands on the call
	 * boundary. Every other pump rate divides 20 ms. */
	source_frames = source->sample_rate == 11025U ? 441U :
	                source->sample_rate / 50U;
	out_frames = source->sample_rate == 11025U ? 1920U :
	             (AUDIO_PUMP_PRECONVERT_PERIOD_BYTES / 4U);
	periods = out_frames / (AUDIO_PUMP_PRECONVERT_PERIOD_BYTES / 4U);
	if (source_frames == 0U ||
	    source_frames > AUDIO_PUMP_PRECONVERT_MAX_SOURCE_FRAMES ||
	    out_frames > 1920U)
		return -1;
	if (state->capacity - audio_pump_preconvert_used(state) <
	    periods * AUDIO_PUMP_PRECONVERT_PERIOD_BYTES +
	    AUDIO_PUMP_PRECONVERT_REPLAY_KEEP)
		return 0;
	source_bytes = source_frames * source->channels * 2U;
	available = source->produced - source->consumed;
	if (available >= source_bytes)
		pull = source_bytes;
	else if (!source->done || available == 0U)
		return 0;
	else
		pull = (uint32_t)available;
	offset = (uint32_t)(source->consumed % source->capacity);
	preconvert_copy_source(state, source, offset, pull, source_bytes);
	if (source->channels == 1U)
		preconvert_expand_mono(state, source_frames);
	if (state->convert_rate != source->sample_rate) {
		state->convert_rate = source->sample_rate;
		zz_audio_convert_init(&state->convert, source->sample_rate, 48000U);
	}
	if (source->sample_rate != 48000U && state->convert.ratio == NULL) {
		/* Off-table rate: emit silence and still advance so drain
		 * and end-of-stream complete (PR #88 review). */
		memset(state->output, 0, (size_t)out_frames * 4U);
	} else {
		zz_audio_convert_stream(&state->convert, state->source,
			state->output, (uint16_t)source_frames,
			(uint16_t)out_frames);
	}
	for (period = 0U; period < periods; period++)
		preconvert_write_period(state,
			state->output + (size_t)period * 960U * 2U);
	*source_consumed = source->consumed + pull;
	return 1;
}

int audio_pump_preconvert_stage(struct audio_pump_preconvert *state,
	uint32_t bytes)
{
	if (!state || bytes > audio_pump_preconvert_used(state))
		return 0;
	state->staged += bytes;
	return 1;
}
