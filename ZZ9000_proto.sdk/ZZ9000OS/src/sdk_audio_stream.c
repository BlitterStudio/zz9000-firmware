/*
 * MP3 ring-decode pass for the codec-aware audio stream.
 *
 * Moved verbatim from sdk_mailbox.c (with the session struct, now in
 * sdk_audio_stream.h) so the host tests drive the exact production
 * decode loop; sdk_mailbox.c keeps the codec dispatch. Cache maintenance
 * here uses the resolved ring addresses exactly like the firmware: the
 * flush before pcm_ready_total publication orders the PCM bytes ahead of
 * the consumer's observation of the new total.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "sdk_audio_stream.h"
#include "sdk_audio_codec_io.h"
#include "audio_reduce.h"
#include "audio_stream_drain.h"
#include "xil_cache.h"
#include "mp3/mp3.h"

uint32_t audio_stream_pcm_used(const struct SDKAudioStream *stream)
{
	return stream->pcm_ready_total - stream->pcm_consumed_total;
}

/* Producer's free space: against written_total (conservative -- counts
 * frames not yet published by the end-of-decode flush too). */
uint32_t audio_stream_pcm_free(const struct SDKAudioStream *stream)
{
	uint32_t used;

	if (!stream)
		return 0;
	used = stream->pcm_written_total - stream->pcm_consumed_total;
	if (used >= stream->pcm_capacity)
		return 0;
	return stream->pcm_capacity - used;
}

void flush_audio_pcm_written(const struct SDKAudioStream *stream,
                             uint8_t *dst, uint32_t offset,
                             uint32_t bytes)
{
	uint32_t first;

	if (!stream || !dst || bytes == 0U || stream->pcm_capacity == 0U)
		return;
	if (offset >= stream->pcm_capacity)
		offset %= stream->pcm_capacity;
	first = stream->pcm_capacity - offset;
	if (first > bytes)
		first = bytes;
	Xil_DCacheFlushRange((INTPTR)(dst + offset), first);
	if (bytes > first)
		Xil_DCacheFlushRange((INTPTR)dst, bytes - first);
}

void audio_stream_pcm_write(struct SDKAudioStream *stream,
                            const uint8_t *src, uint32_t bytes)
{
	uint8_t *dst = (uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	uint32_t offset = stream->pcm_written_total % stream->pcm_capacity;
	uint32_t first = stream->pcm_capacity - offset;

	if (first > bytes)
		first = bytes;
	memcpy(dst + offset, src, first);
	if (bytes > first)
		memcpy(dst, src + first, bytes - first);
	stream->pcm_written_total += bytes;
	stream->bytes_produced += bytes;
}

void audio_stream_consume_input(struct SDKAudioStream *stream,
                                uint32_t bytes)
{
	if (bytes >= stream->input_length) {
		stream->input_offset = 0U;
		stream->input_length = 0;
		return;
	}
	stream->input_offset += bytes;
	stream->input_length -= bytes;
}

void audio_stream_discard_input(struct SDKAudioStream *stream)
{
	uint32_t bytes;

	if (!stream || stream->input_length == 0U)
		return;
	bytes = stream->input_length;
	audio_stream_consume_input(stream, bytes);
	stream->bytes_consumed += bytes;
}

void byteswap_pcm16(uint8_t *data, uint32_t bytes)
{
	uint32_t i;
	uint8_t tmp;

	for (i = 0U; i + 1U < bytes; i += 2U) {
		tmp = data[i];
		data[i] = data[i + 1U];
		data[i + 1U] = tmp;
	}
}

/* The reducer and the ring staging treat PCM as int16_t. */
typedef char audio_stream_sample_is_s16[
	sizeof(mp3d_sample_t) == sizeof(int16_t) ? 1 : -1];

uint16_t audio_stream_reduce_begin_status(uint32_t sample_format,
                                          uint32_t output_hz,
                                          uint32_t output_channels)
{
	if (output_hz == 0U && output_channels == 0U)
		return SDK_STATUS_OK;
	if (sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		return SDK_STATUS_UNSUPPORTED;
	if (output_channels > AUDIO_REDUCE_MAX_CHANNELS)
		return SDK_STATUS_UNSUPPORTED;
	return SDK_STATUS_OK;
}

void audio_stream_set_format(struct SDKAudioStream *stream, uint32_t hz,
                             uint32_t channels)
{
	if (!audio_reduce_set_input(&stream->reduce, hz, channels))
		return;
	stream->sample_rate = stream->reduce.out_hz;
	stream->channels = stream->reduce.out_channels;
}

/* Copy ring bytes [offset, offset + bytes) (wrapping) to or from a linear
 * buffer. */
static void audio_stream_ring_read(const struct SDKAudioStream *stream,
                                   uint32_t offset, uint8_t *dst,
                                   uint32_t bytes)
{
	const uint8_t *ring = (const uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	uint32_t first = stream->pcm_capacity - offset;

	if (first > bytes)
		first = bytes;
	memcpy(dst, ring + offset, first);
	if (bytes > first)
		memcpy(dst + first, ring, bytes - first);
}

static void audio_stream_ring_write(const struct SDKAudioStream *stream,
                                    uint32_t offset, const uint8_t *src,
                                    uint32_t bytes)
{
	uint8_t *ring = (uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	uint32_t first = stream->pcm_capacity - offset;

	if (first > bytes)
		first = bytes;
	memcpy(ring + offset, src, first);
	if (bytes > first)
		memcpy(ring, src + first, bytes - first);
}

uint32_t audio_stream_reduce_ring(struct SDKAudioStream *stream,
                                  uint32_t start, uint32_t raw_bytes)
{
	struct audio_reduce *r = &stream->reduce;
	const uint32_t cap = stream->pcm_capacity;
	const uint32_t in_fb = 2U * r->in_channels;
	const uint32_t out_fb = 2U * r->out_channels;
	const uint32_t total = r->in_channels != 0U ? raw_bytes / in_fb : 0U;
	const int be = stream->sample_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE;
	uint8_t *in_buf = (uint8_t *)stream->scratch;
	uint8_t *out_buf = (uint8_t *)stream->reduced;
	const uint32_t in_cap = (uint32_t)sizeof(stream->scratch) / in_fb;
	uint32_t done;

	if (!audio_reduce_active(r) || total == 0U)
		return raw_bytes;

	if (r->divisor == 1U && r->in_channels == 1U && r->out_channels == 2U) {
		/* Duplication doubles the data and the output runs ahead of the
		 * input, so walk backward: each chunk is read before its output
		 * lands, and the output never reaches input still to be read. */
		const uint32_t chunk_max = in_cap < sizeof(stream->reduced) / 4U ?
		                           in_cap : (uint32_t)sizeof(stream->reduced) / 4U;
		uint32_t remaining = total;

		while (remaining != 0U) {
			uint32_t n = remaining < chunk_max ? remaining : chunk_max;
			uint32_t first = remaining - n;
			const int16_t *src = (const int16_t *)in_buf;
			int16_t *dst = (int16_t *)out_buf;
			uint32_t i;

			audio_stream_ring_read(stream, (start + first * in_fb) % cap,
			                       in_buf, n * in_fb);
			if (be)
				byteswap_pcm16(in_buf, n * in_fb);
			for (i = 0U; i < n; i++) {
				dst[2U * i] = src[i];
				dst[2U * i + 1U] = src[i];
			}
			if (be)
				byteswap_pcm16(out_buf, n * out_fb);
			audio_stream_ring_write(stream,
			                        (start + first * out_fb) % cap,
			                        out_buf, n * out_fb);
			remaining = first;
		}
		return total * out_fb;
	}

	/* Forward pass: cumulative output never exceeds cumulative input, so
	 * a chunk's output only lands on bytes that were already read. */
	{
		const uint32_t chunk_max = in_cap < 1024U ? in_cap : 1024U;
		uint32_t out_off = 0U;

		for (done = 0U; done < total;) {
			uint32_t n = total - done;
			uint32_t out_frames;

			if (n > chunk_max)
				n = chunk_max;
			audio_stream_ring_read(stream,
			                       (start + done * in_fb) % cap,
			                       in_buf, n * in_fb);
			if (be)
				byteswap_pcm16(in_buf, n * in_fb);
			out_frames = audio_reduce_run(
				r, (const int16_t *)in_buf, n,
				(int16_t *)out_buf);
			if (out_frames != 0U) {
				if (be)
					byteswap_pcm16(out_buf, out_frames * out_fb);
				audio_stream_ring_write(
					stream, (start + out_off) % cap, out_buf,
					out_frames * out_fb);
				out_off += out_frames * out_fb;
			}
			done += n;
		}
		return out_off;
	}
}

static uint32_t audio_stream_get_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) |
	       ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) |
	       (uint32_t)data[3];
}

static int audio_stream_check_vbr_tag(const uint8_t *frame,
                                      uint32_t frame_size,
                                      uint32_t *frame_count)
{
	uint32_t tag_off;
	uint32_t flags;

	if (!frame || !frame_count || frame_size < 12U)
		return 0;
	tag_off = 4U;
	if ((frame[1] & 1U) == 0U)
		tag_off += 2U;
	if ((frame[1] & 0x08U) != 0U)
		tag_off += ((frame[3] & 0xC0U) == 0xC0U) ? 17U : 32U;
	else
		tag_off += ((frame[3] & 0xC0U) == 0xC0U) ? 9U : 17U;
	if (tag_off > frame_size || frame_size - tag_off < 12U)
		return 0;
	if (memcmp(frame + tag_off, "Xing", 4U) != 0 &&
	    memcmp(frame + tag_off, "Info", 4U) != 0)
		return 0;
	flags = frame[tag_off + 7U];
	if ((flags & 1U) == 0U)
		return -1;
	tag_off += 8U;
	if (frame_size - tag_off < 4U)
		return 0;
	*frame_count = audio_stream_get_be32(frame + tag_off);
	if (*frame_count == 0U)
		return 0;
	return 1;
}

static int audio_stream_process_vbr_tag(struct SDKAudioStream *stream,
                                        const uint8_t *input,
                                        const mp3dec_frame_info_t *info)
{
	uint32_t frame_count = 0U;
	uint32_t frame_size;
	int ret;

	if (!stream || !input || !info || stream->vbr_checked ||
	    info->frame_bytes <= info->frame_offset)
		return 0;
	stream->vbr_checked = 1U;
	frame_size = (uint32_t)(info->frame_bytes - info->frame_offset);
	ret = audio_stream_check_vbr_tag(
		input + info->frame_offset, frame_size, &frame_count);
	if (ret > 0)
		stream->output_frame_limit = frame_count;
	return ret != 0;
}

uint32_t audio_stream_decode_mp3(struct SDKAudioStream *stream)
{
	const uint32_t frame_pcm_bytes =
		MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t);
	uint32_t produced_this_call = 0U;
	uint32_t raw_this_call = 0U;
	uint32_t frames_this_call = 0U;
	uint32_t max_pcm_this_call;
	uint32_t max_raw_this_call;
	uint32_t pcm_flush_start;
	uint32_t progress = 0U;
	int drain_blocked_on_input = 0;
	uint8_t *input;
	uint8_t *pcm_dst;

	if (!stream || stream->mp3_ring_addr == 0U ||
	    stream->pcm_ring_addr == 0U)
		return 0;
	input = (uint8_t *)(uintptr_t)stream->mp3_ring_addr;
	pcm_dst = (uint8_t *)(uintptr_t)stream->pcm_ring_addr;
	if (stream->decode_complete) {
		if (stream->input_length != 0U) {
			audio_stream_discard_input(stream);
			progress = 1U;
		}
		if (audio_stream_drain_input_done(
			    stream->drain_requested, stream->decode_complete,
			    stream->input_length, 0, 0))
			stream->drain_input_complete = 1;
		return progress;
	}
	max_pcm_this_call = stream->high_water_bytes;
	if (max_pcm_this_call == 0U ||
	    max_pcm_this_call > stream->pcm_capacity)
		max_pcm_this_call = stream->pcm_capacity;
	/* Bound this pass to the measured ~3.5 ms of core-1 service regardless
	 * of the client's high-water budget; see sdk_audio_codec_io.h for the
	 * 333 MHz global-timer basis. The decode cost follows the decoder's
	 * PCM, so the cap counts it before reduction, while the client's
	 * high-water budget counts the bytes that reach the ring. */
	max_raw_this_call = sdk_audio_decode_pass_budget(
		audio_reduce_raw_bytes(&stream->reduce, max_pcm_this_call));
	if (max_raw_this_call < frame_pcm_bytes)
		max_raw_this_call = frame_pcm_bytes;
	if (max_pcm_this_call < frame_pcm_bytes)
		max_pcm_this_call = frame_pcm_bytes;
	pcm_flush_start = stream->pcm_written_total % stream->pcm_capacity;

	while (stream->input_length > 0U &&
	       audio_stream_pcm_free(stream) >= frame_pcm_bytes) {
		mp3dec_frame_info_t info;
		int decode_input_size = (int)stream->input_length;
		int samples;
		uint32_t consumed;
		uint32_t bytes;

		if (!audio_stream_decode_quantum_available(
			    frames_this_call, audio_stream_pcm_used(stream),
			    stream->low_water_bytes))
			break;
		if (!audio_stream_decode_may_run(
			    stream->input_length,
			    SDK_AUDIO_STREAM_MIN_INPUT_BYTES,
			    stream->eof, stream->drain_requested))
			break;
		if (produced_this_call != 0U &&
		    ((max_raw_this_call - raw_this_call) < frame_pcm_bytes ||
		     (max_pcm_this_call - produced_this_call) <
		         frame_pcm_bytes)) {
			break;
		}
		if (stream->drain_requested && !stream->eof &&
		    !mp3_probe_complete_frame(
			    &stream->decoder, input + stream->input_offset,
			    (int)stream->input_length, &decode_input_size)) {
			drain_blocked_on_input = 1;
			break;
		}
		memset(&info, 0, sizeof(info));
		samples = mp3dec_decode_frame(
			&stream->decoder,
			input + stream->input_offset,
			decode_input_size,
			stream->scratch, &info);
		frames_this_call++;
		consumed = (uint32_t)info.frame_bytes;
		if (consumed == 0U) {
			drain_blocked_on_input = 1;
			break;
		}
		if (stream->eof && stream->frames_decoded != 0U &&
		    info.frame_offset != 0) {
			audio_stream_discard_input(stream);
			progress = 1U;
			break;
		}

		if (samples > 0) {
			if (stream->reduce.in_hz == 0U)
				audio_stream_set_format(stream, (uint32_t)info.hz,
				                        (uint32_t)info.channels);
			if ((uint32_t)info.hz != stream->reduce.in_hz ||
			    (uint32_t)info.channels != stream->reduce.in_channels) {
				stream->faulted = 1U;
				break;
			}
			if (audio_stream_process_vbr_tag(
				    stream, input + stream->input_offset,
				    &info)) {
				bytes = 0U;
			} else if (stream->output_frame_limit != 0U &&
			           stream->frames_decoded >=
			                   stream->output_frame_limit) {
				stream->decode_complete = 1;
				bytes = 0U;
			} else {
				bytes = (uint32_t)samples *
				        (uint32_t)info.channels *
				        sizeof(mp3d_sample_t);
			}
			if (bytes != 0U) {
				uint8_t *pcm = (uint8_t *)stream->scratch;
				uint32_t out_bytes = bytes;

				raw_this_call += bytes;
				if (audio_reduce_active(&stream->reduce)) {
					/* Reduction first; the byteswap stays
					 * last so the filter sees native S16. */
					out_bytes = audio_reduce_run(
						&stream->reduce, stream->scratch,
						(uint32_t)samples,
						stream->reduced) *
						2U * stream->reduce.out_channels;
					pcm = (uint8_t *)stream->reduced;
				}
				if (out_bytes != 0U) {
					if (stream->sample_format ==
					    SDK_AUDIO_SAMPLE_FORMAT_S16BE)
						byteswap_pcm16(pcm, out_bytes);
					audio_stream_pcm_write(stream, pcm,
					                       out_bytes);
				}
				stream->frames_decoded++;
				produced_this_call += out_bytes;
				progress = 1U;
				if (stream->output_frame_limit != 0U &&
				    stream->frames_decoded >=
				            stream->output_frame_limit)
					stream->decode_complete = 1;
			}
		}

		audio_stream_consume_input(stream, consumed);
		stream->bytes_consumed += consumed;
		progress = 1U;
		if (stream->decode_complete) {
			audio_stream_discard_input(stream);
			break;
		}
	}

	if (produced_this_call != 0U) {
		flush_audio_pcm_written(stream, pcm_dst, pcm_flush_start,
		                        produced_this_call);
		/* Publish only after the flush: Xil_DCacheFlushRange ends in
		 * a DSB, so consumers that see the new ready total also see
		 * the PCM bytes in DRAM. */
		stream->pcm_ready_total = stream->pcm_written_total;
	}
	if (stream->eof && stream->input_length == 0U)
		stream->decode_complete = 1U;
	if (audio_stream_drain_input_done(
		    stream->drain_requested, stream->decode_complete,
		    stream->input_length, drain_blocked_on_input, 0))
		stream->drain_input_complete = 1;
	return progress;
}
