#ifndef AUDIO_STREAM_DRAIN_H
#define AUDIO_STREAM_DRAIN_H

#include <stdint.h>

/* Core 1 also decodes video. Yield after two MP3 frames once playback has
 * crossed its configured low-water reserve. Below that reserve, completing
 * the refill is more urgent than sharing the worker. */
#define AUDIO_STREAM_DECODE_FRAMES_PER_TASK 2U

static inline int audio_stream_decode_quantum_available(
		uint32_t attempted_frames, uint32_t pcm_used,
		uint32_t low_water_bytes)
{
	return attempted_frames < AUDIO_STREAM_DECODE_FRAMES_PER_TASK ||
		pcm_used <= low_water_bytes;
}

/* A zero-byte refill only reads the compressed ring and must not issue cache
 * maintenance. A real feed dirties either its append span or, when append
 * would cross the end, the compacted live input plus the appended bytes. */
static inline int audio_stream_feed_dirty_span(
		uint32_t input_offset, uint32_t input_length,
		uint32_t source_length, uint32_t capacity,
		uint32_t *dirty_offset, uint32_t *dirty_length)
{
	if (!dirty_offset || !dirty_length || source_length == 0U ||
	    input_length > capacity ||
	    input_offset > capacity - input_length ||
	    source_length > capacity - input_length)
		return 0;
	if (input_offset > capacity - input_length - source_length) {
		*dirty_offset = 0U;
		*dirty_length = input_length + source_length;
	} else {
		*dirty_offset = input_offset + input_length;
		*dirty_length = source_length;
	}
	return 1;
}

/* A normal streaming decode keeps a lookahead reserve. EOF and a resumable
 * drain both relax that gate so every currently complete frame is decoded. */
static inline int audio_stream_decode_may_run(uint32_t input_length,
		uint32_t minimum_input, int eof, int drain_requested)
{
	return input_length != 0U &&
		(input_length >= minimum_input || eof || drain_requested);
}

static inline int audio_stream_drain_input_done(int drain_requested,
		int decode_complete, uint32_t input_length,
		int blocked_on_incomplete_frame)
{
	return drain_requested &&
		(decode_complete || input_length == 0U ||
		 blocked_on_incomplete_frame);
}

/* Once a resumable drain has classified the retained compressed tail as an
 * incomplete frame, the playback pump must wait for new input instead of
 * scheduling the same no-progress refill on every main-loop pass. */
static inline int audio_stream_refill_may_run(uint32_t input_length,
		int faulted, int needs_more_input, int drain_input_complete,
		uint32_t pcm_used, uint32_t low_water)
{
	return input_length != 0U && !faulted && !needs_more_input &&
		!drain_input_complete && pcm_used <= low_water;
}

/* A bound AX pump may zero-pad a final short PCM period only after the
 * compressed side has reached a real boundary. A resumable drain retains
 * incomplete compressed input; permanent EOF requires no input remaining. */
static inline int audio_stream_source_tail_ready(int eof,
		uint32_t input_length, int drain_requested,
		int drain_input_complete)
{
	return (eof && input_length == 0U) ||
		(drain_requested && drain_input_complete);
}

/* Transient OUT_OF_DATA is published only after decoded PCM and the real DMA
 * tail have both retired. It does not make the compressed stream permanent. */
static inline int audio_stream_transient_drained(int drain_requested,
		int drain_input_complete, uint32_t pcm_used,
		int pump_tail_pending)
{
	return drain_requested && drain_input_complete && pcm_used == 0U &&
		!pump_tail_pending;
}

#endif
