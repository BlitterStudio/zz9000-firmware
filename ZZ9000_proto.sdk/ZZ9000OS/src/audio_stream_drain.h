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

/* A resumable drain finishes at decoder completion, an incomplete retained
 * frame, or an empty ring for a decoder with no internal PCM tail (MP3).
 * Native decoders can retain a complete PCM tail after their input empties,
 * so they must explicitly report completion. */
static inline int audio_stream_drain_input_done(int drain_requested,
		int decode_complete, uint32_t input_length,
		int blocked_on_incomplete_frame, int may_hold_output)
{
	return drain_requested &&
		(decode_complete || blocked_on_incomplete_frame ||
		 (!may_hold_output && input_length == 0U));
}

/* An empty refill that left its compressed input, PCM producer/consumer
 * cursors and decoder-visible state unchanged cannot make progress again
 * until one of those inputs changes. Tracking both cursors distinguishes a
 * productive refill from space freed concurrently by the playback consumer. */
struct audio_stream_refill_checkpoint {
	uint32_t input_length;
	uint32_t pcm_written_total;
	uint32_t pcm_consumed_total;
	uint32_t decoder_state;
	int stalled;
};

static inline int audio_stream_refill_may_run(uint32_t input_length,
		int decode_complete, int faulted, int needs_more_input,
		int drain_input_complete, uint32_t pcm_used, uint32_t low_water,
		uint32_t pcm_written_total, uint32_t pcm_consumed_total,
		uint32_t decoder_state,
		const struct audio_stream_refill_checkpoint *checkpoint)
{
	if (!((input_length != 0U || !decode_complete) && !faulted &&
	      !needs_more_input && !drain_input_complete &&
	      pcm_used <= low_water))
		return 0;
	return !checkpoint || !checkpoint->stalled ||
		checkpoint->input_length != input_length ||
		checkpoint->pcm_written_total != pcm_written_total ||
		checkpoint->pcm_consumed_total != pcm_consumed_total ||
		checkpoint->decoder_state != decoder_state;
}

static inline void audio_stream_refill_checkpoint_note(
		struct audio_stream_refill_checkpoint *checkpoint,
		uint32_t input_length, uint32_t pcm_written_total,
		uint32_t pcm_consumed_total, uint32_t decoder_state,
		int made_progress)
{
	checkpoint->input_length = input_length;
	checkpoint->pcm_written_total = pcm_written_total;
	checkpoint->pcm_consumed_total = pcm_consumed_total;
	checkpoint->decoder_state = decoder_state;
	checkpoint->stalled = !made_progress;
}

/* A bound AX pump may zero-pad a final short PCM period only after the
 * compressed side has reached a real boundary. A resumable drain retains
 * incomplete compressed input; permanent EOF requires no external input and
 * either validated completion or a decoder terminal boundary whose status is
 * deferred until published PCM is drained. */
static inline int audio_stream_source_tail_ready(int eof, int decode_complete,
		int output_terminal, uint32_t input_length, int drain_requested,
		int drain_input_complete)
{
	return (eof && (decode_complete || output_terminal) &&
		input_length == 0U) ||
		(drain_requested && drain_input_complete);
}

/* A decoder error found while moving source PCM into the preconversion ring
 * must not silence audio that the fabric has already staged. Publish it to
 * the fabric only after that ring and its real-DMA tail are both drained. */
static inline int audio_stream_playback_fault_ready(int faulted,
		uint32_t preconvert_used, int pump_tail_pending)
{
	return faulted && preconvert_used == 0U && !pump_tail_pending;
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
