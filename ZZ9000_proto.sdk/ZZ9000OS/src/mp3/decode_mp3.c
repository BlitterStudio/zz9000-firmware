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

int decode_mp3_init(struct mp3_decode_ctx *ctx, uint8_t* input_buffer,
                    size_t input_buffer_size) {
	memset(&ctx->frame_info, 0, sizeof(ctx->frame_info));

	// sets up input_buffer as mp3d->file.buffer
	int ret = mp3dec_ex_open_buf(&ctx->mp3d, input_buffer,
	                             input_buffer_size, MP3D_DO_NOT_SCAN);
	if (ret) {
		printf("mp3dec_ex_open_buf failed: %d\n", ret);
	}
	return ret;
}

int mp3_get_hz(const struct mp3_decode_ctx *ctx) {
	return ctx->mp3d.info.hz;
}

int mp3_get_channels(const struct mp3_decode_ctx *ctx) {
	return ctx->mp3d.info.channels;
}

unsigned long mp3_get_bytes_consumed(const struct mp3_decode_ctx *ctx) {
	return (unsigned long)ctx->mp3d.offset;
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
