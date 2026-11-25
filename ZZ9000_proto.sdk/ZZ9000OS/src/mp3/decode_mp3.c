/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION 1
#include "mp3.h"

int decode_mp3_samples(struct mp3_decode_ctx *ctx, void* output_buffer,
                       int max_samples) {
	int max_bytes = max_samples * 2;
	int out_offset = 0;
	int total_bytes_decoded = 0;

	// Clear destination buffer before trying to decode.
	memset(output_buffer, 0, max_bytes);

	// this will point into mp3d->buffer, which is defined on the stack
	// as mp3d_sample_t buffer[MINIMP3_MAX_SAMPLES_PER_FRAME]
	mp3d_sample_t * pcm_buffer = NULL;

	while (1) {
		size_t read_samples = mp3dec_ex_read_frame(&ctx->mp3d, &pcm_buffer,
		                                           &ctx->frame_info,
		                                           max_samples);
		max_samples -= read_samples;

		int bytes_decoded = read_samples * sizeof(mp3d_sample_t);
		total_bytes_decoded += bytes_decoded;

		if (bytes_decoded > 0) {
			int bytes_to_copy = bytes_decoded;
			memcpy(output_buffer + out_offset, pcm_buffer, bytes_to_copy);
			out_offset += bytes_decoded;

			if (out_offset >= max_bytes) {
				break;
			}
		} else {
			break;
		}
	}

	return total_bytes_decoded;
}

static uint32_t mp3_le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* End of the audio in the input: the length with trailing ID3v1 (and its
 * "TAG+" extension) and APEv2 tags removed. minimp3's own skip treats an
 * APEv2 tag as always carrying a header and cuts 32 bytes too many from one
 * that has none, which drops the last frame, so the tags are measured here.
 * Nothing before first, the offset of the first frame, is removed. */
static size_t mp3_audio_end(const uint8_t *buf, size_t size, size_t first) {
	size_t end = size;
	size_t before;

	if (first > size)
		return size;
	do {
		before = end;
		if (end - first >= 128 && !memcmp(buf + end - 128, "TAG", 3)) {
			end -= 128;
			if (end - first >= 227 && !memcmp(buf + end - 227, "TAG+", 4))
				end -= 227;
		}
		if (end - first >= 32 && !memcmp(buf + end - 32, "APETAGEX", 8)) {
			/* Footer: size counts the items and the footer, flag bit 31
			 * says a 32-byte header precedes the items. */
			const uint8_t *footer = buf + end - 32;
			uint32_t tag_size = mp3_le32(footer + 12);
			size_t total = tag_size;

			if (mp3_le32(footer + 20) & 0x80000000UL)
				total += 32;
			if (tag_size >= 32 && total <= end - first)
				end -= total;
		}
	} while (end != before);
	return end;
}

int decode_mp3_init(struct mp3_decode_ctx *ctx, uint8_t* input_buffer,
                    size_t input_buffer_size) {
	memset(&ctx->frame_info, 0, sizeof(ctx->frame_info));
	ctx->src_size = 0;
	ctx->audio_end = 0;

	// sets up input_buffer as mp3d->file.buffer
	int ret = mp3dec_ex_open_buf(&ctx->mp3d, input_buffer,
	                             input_buffer_size, MP3D_DO_NOT_SCAN);
	if (ret) {
		printf("mp3dec_ex_open_buf failed: %d\n", ret);
		return ret;
	}
	ctx->src_size = input_buffer_size;
	ctx->audio_end = mp3_audio_end(input_buffer, input_buffer_size,
	                               (size_t)ctx->mp3d.start_offset);
	/* end_offset is zero when no frame was found; the caller rejects that
	 * through the zero sample rate. */
	if (ctx->mp3d.end_offset)
		ctx->mp3d.end_offset = ctx->audio_end;
	return 0;
}

int mp3_get_hz(const struct mp3_decode_ctx *ctx) {
	return ctx->mp3d.info.hz;
}

int mp3_get_channels(const struct mp3_decode_ctx *ctx) {
	return ctx->mp3d.info.channels;
}

/* PCM the decoder holds that decode_mp3_samples() could not deliver. The
 * samples a VBR header marks as encoder padding are not pending. */
static int mp3_pcm_pending(const mp3dec_ex_t *d) {
	if (d->detected_samples && d->cur_sample >= d->detected_samples)
		return 0;
	return d->buffer_consumed < d->buffer_samples;
}

enum mp3_input_state mp3_input_state(const struct mp3_decode_ctx *ctx) {
	const mp3dec_ex_t *d = &ctx->mp3d;

	if (d->offset < ctx->audio_end || mp3_pcm_pending(d))
		return MP3_INPUT_MORE;
	/* All the audio bytes are consumed. minimp3 resets its header state
	 * when the last decode call found no frame, which is what a truncated
	 * final frame or trailing junk looks like: the call skips the rest. */
	if (d->mp3d.header[0] != 0xff)
		return MP3_INPUT_DAMAGED;
	/* A Xing or Info header declares the sample count (detected_samples,
	 * after minimp3 removes the encoder delay and padding). Input that ends
	 * on a frame boundary with fewer samples delivered lost whole frames.
	 * Streams without that header declare no length, so a cut on a frame
	 * boundary cannot be told from a complete file. */
	if (d->detected_samples && d->cur_sample < d->detected_samples)
		return MP3_INPUT_DAMAGED;
	return MP3_INPUT_COMPLETE;
}

int mp3_expect_end_ok(const struct mp3_decode_ctx *ctx, size_t bytes_written,
                      size_t output_capacity) {
	switch (mp3_input_state(ctx)) {
	case MP3_INPUT_COMPLETE:
		return 1;
	case MP3_INPUT_MORE:
		/* The output filled first: the caller asked for a partial decode. */
		return bytes_written >= output_capacity;
	default:
		return 0;
	}
}

unsigned long mp3_get_bytes_consumed(const struct mp3_decode_ctx *ctx) {
	const mp3dec_ex_t *d = &ctx->mp3d;
	size_t used = (size_t)d->offset;

	if (mp3_pcm_pending(d)) {
		/* The output filled inside the last frame: that frame is not used
		 * up, so it and anything after it stay unconsumed. */
		size_t last = ctx->frame_info.frame_bytes > 0 ?
		              (size_t)ctx->frame_info.frame_bytes : 0;
		used = used > last ? used - last : 0;
	} else if (used >= ctx->audio_end) {
		/* Recognised trailing tags count as consumed once the audio is. */
		return ctx->src_size;
	}
	return (unsigned long)(used < ctx->src_size ? used : ctx->src_size);
}

int mp3_probe_complete_frame(const mp3dec_t *decoder,
                             const uint8_t *input_buffer,
                             int input_buffer_size,
                             int *decode_input_size) {
	mp3dec_t probe;
	mp3dec_frame_info_t info;
	int frame_size;
	int samples;

	if (!decoder || !input_buffer || input_buffer_size <= 0 ||
	    !decode_input_size)
		return 0;
	*decode_input_size = 0;
	probe = *decoder;
	memset(&info, 0, sizeof(info));
	samples = mp3dec_decode_frame(&probe, input_buffer, input_buffer_size,
	                              NULL, &info);
	if (samples > 0 && info.frame_bytes > 0 &&
	    info.frame_bytes <= input_buffer_size) {
		*decode_input_size = info.frame_bytes;
		return 1;
	}

	/* minimp3 normally confirms a frame against the next four-byte header.
	 * A resumable drain can end with one complete frame followed by only the
	 * first one to three bytes of the next header. In that narrow case, parse
	 * the leading non-free-format frame length and let the live decoder see
	 * exactly that frame; the trailing prefix remains buffered for refill. */
	if (input_buffer_size <= HDR_SIZE || !hdr_valid(input_buffer) ||
	    HDR_IS_FREE_FORMAT(input_buffer))
		return 0;
	frame_size = hdr_frame_bytes(input_buffer, decoder->free_format_bytes) +
	             hdr_padding(input_buffer);
	if (frame_size <= 0 || frame_size > input_buffer_size ||
	    input_buffer_size - frame_size >= HDR_SIZE)
		return 0;
	*decode_input_size = frame_size;
	return 1;
}
