/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "sdk_audio_stream.h"

/* Native codec sessions (FLAC U7, Vorbis U8): the backend decodes straight
 * into the PCM ring. Cursor ownership, flush-before-publish and drain
 * bookkeeping follow the MP3 loop below; a backend failure faults the
 * stream with its status (UNSUPPORTED / IO_ERROR / NO_MEMORY). */
static uint32_t audio_stream_decode_native(struct SDKAudioStream *stream)
{
	struct sdk_audio_codec_io io;
	uint8_t *input = (uint8_t *)(uintptr_t)stream->mp3_ring_addr;
	uint8_t *pcm_dst = (uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	uint32_t pcm_flush_start;
	uint32_t progress = 0U;
	uint16_t status;

	if (stream->faulted)
		return 0U;
	if (stream->decode_complete) {
		if (stream->input_length != 0U) {
			audio_stream_discard_input(stream);
			progress = 1U;
		}
		if (audio_stream_drain_input_done(
			    stream->drain_requested, stream->decode_complete,
			    stream->input_length, 0, 1))
			stream->drain_input_complete = 1;
		return progress;
	}
	memset(&io, 0, sizeof(io));
	io.input = input + stream->input_offset;
	io.input_length = stream->input_length;
	io.eof = stream->eof;
	io.drain = stream->drain_requested && !stream->eof;
	io.pcm = pcm_dst;
	io.pcm_capacity = stream->pcm_capacity;
	pcm_flush_start = stream->pcm_written_total % stream->pcm_capacity;
	io.pcm_write = pcm_flush_start;
	/* The backend writes decoder-format PCM straight into the ring and
	 * audio_stream_reduce_ring() shrinks it in place afterwards, so the
	 * room it may use must not exceed what the reduced form needs, and
	 * the client's high-water budget counts reduced bytes. */
	io.pcm_free = audio_reduce_raw_room(&stream->reduce,
	                                    audio_stream_pcm_free(stream));
	io.pcm_budget = stream->high_water_bytes;
	if (io.pcm_budget == 0U || io.pcm_budget > stream->pcm_capacity)
		io.pcm_budget = stream->pcm_capacity;
	io.pcm_budget = audio_reduce_raw_bytes(&stream->reduce, io.pcm_budget);
	io.pcm_unread = audio_stream_pcm_used(stream);
	if (stream->codec == SDK_AUDIO_CODEC_VORBIS)
		status = sdk_vorbis_decode(&stream->vorbis, &io);
	else
		status = sdk_flac_decode(&stream->flac, &io);
	if (io.consumed != 0U) {
		audio_stream_consume_input(stream, io.consumed);
		stream->bytes_consumed += io.consumed;
		progress = 1U;
	}
	{
		const uint32_t dec_hz = stream->codec == SDK_AUDIO_CODEC_VORBIS ?
		                        stream->vorbis.sample_rate :
		                        stream->flac.sample_rate;
		const uint32_t dec_channels =
			stream->codec == SDK_AUDIO_CODEC_VORBIS ?
			stream->vorbis.channels : stream->flac.channels;

		/* Also re-evaluates the divisor if the rate changed. */
		audio_stream_set_format(stream, dec_hz, dec_channels);
	}
	if (io.produced != 0U)
		io.produced = audio_stream_reduce_ring(stream, pcm_flush_start,
		                                       io.produced);
	if (io.produced != 0U) {
		stream->pcm_written_total += io.produced;
		stream->bytes_produced += io.produced;
		stream->frames_decoded += io.frames;
		flush_audio_pcm_written(stream, pcm_dst, pcm_flush_start,
		                        io.produced);
		/* Publish only after the flush (ends in a DSB). */
		stream->pcm_ready_total = stream->pcm_written_total;
		progress = 1U;
	}
	stream->backend_starved = io.starved ? 1U : 0U;
	if (io.output_terminal || status != SDK_STATUS_OK)
		stream->output_terminal = 1U;
	if (io.complete) {
		stream->decode_complete = 1;
		audio_stream_discard_input(stream);
	}
	if (status != SDK_STATUS_OK) {
		stream->faulted = 1U;
		stream->fault_status = status;
	}
	if (audio_stream_drain_input_done(
		    stream->drain_requested, stream->decode_complete,
		    stream->input_length, io.starved, 1))
		stream->drain_input_complete = 1;
	return progress;
}

/* Codec dispatch only: the native backends decode above, the MP3 ring
 * loop lives in sdk_audio_stream.c (shared with the host tests). */
uint32_t audio_stream_decode(struct SDKAudioStream *stream)
{
	if (stream && stream->codec != SDK_AUDIO_CODEC_MP3)
		return audio_stream_decode_native(stream);
	return audio_stream_decode_mp3(stream);
}
