/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <xil_cache.h>
#define MINIMP3_IMPLEMENTATION 1
#include "mp3.h"

#define SWAP16(a) a = __builtin_bswap16(a)
#define SWAP32(a) a = __builtin_bswap32(a)

static size_t read_cb(void *buf, size_t size, void *user_data) {
	struct mp3_decode_ctx *ctx = user_data;
	unsigned long BytesRead = 0;
	long BytesToRead = size;
	unsigned char *src = ctx->fifo_addr;
	unsigned char *dst = buf;

	while(BytesToRead) {
		// If FiFo is empty then exit the loop.
		if(ctx->fifo_read_idx == ctx->fifo_write_idx) break;
		dst[BytesRead++] = src[ctx->fifo_read_idx++];
		if(ctx->fifo_read_idx >= ctx->fifo_size) ctx->fifo_read_idx = 0;
		BytesToRead--;
	}

	return BytesRead;
}

static int seek_cb(uint64_t position, void *user_data) {
	struct mp3_decode_ctx *ctx = user_data;

	ctx->fifo_read_idx = position % ctx->fifo_size;
	return 0;
}

void fifo_clear(struct mp3_decode_ctx *ctx) {
	ctx->fifo_read_idx  = 0;
	ctx->fifo_write_idx = 0;
}

void fifo_set_write_index(struct mp3_decode_ctx *ctx,
                          unsigned short aWriteIndex) {
	ctx->fifo_write_idx = aWriteIndex;
	// New data has arrived from the 68k size.
	// We need to invalidate the data cache where the data came in.
	if(ctx->fifo_write_idx > ctx->old_fifo_write_idx) {
		// Invalidate range from old til new.
		Xil_DCacheInvalidateRange((INTPTR)&ctx->fifo_addr[ctx->old_fifo_write_idx],
		                          ctx->fifo_write_idx-ctx->old_fifo_write_idx);
	}
	else {
		// 1. Invalidate range from old til end.
		Xil_DCacheInvalidateRange((INTPTR)&ctx->fifo_addr[ctx->old_fifo_write_idx],
		                          ctx->fifo_size-ctx->old_fifo_write_idx);
		// 2. Invalidate range from beginning til new.
		Xil_DCacheInvalidateRange((INTPTR)&ctx->fifo_addr[0],
		                          ctx->fifo_write_idx);
	}
	ctx->old_fifo_write_idx = ctx->fifo_write_idx;
}

unsigned short fifo_get_read_index(const struct mp3_decode_ctx *ctx) {
	return ctx->fifo_read_idx;
}

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

int decode_mp3_init_fifo(struct mp3_decode_ctx *ctx, uint8_t* input_buffer,
                         size_t input_buffer_size) {
	memset(&ctx->frame_info, 0, sizeof(ctx->frame_info));

	ctx->fifo_size   = input_buffer_size;
	ctx->fifo_addr   = input_buffer;
	ctx->mp3io.read = read_cb;
	ctx->mp3io.seek = seek_cb;
	ctx->mp3io.read_data = ctx->mp3io.seek_data = ctx;

	int ret = mp3dec_ex_open_cb(&ctx->mp3d, &ctx->mp3io, MP3D_DO_NOT_SCAN);
	if (ret) {
		printf("mp3dec_ex_open_cb failed: %d\n", ret);
	}

	return ret;
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
