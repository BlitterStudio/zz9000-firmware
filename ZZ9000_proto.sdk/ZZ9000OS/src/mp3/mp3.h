/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_MP3_H
#define ZZ9K_MP3_H

#include <stddef.h>
#include <stdint.h>

#include "mp3_backend_config.h"
#include "minimp3_ex.h"

typedef enum {
	DECODE_CLEAR,
	DECODE_INIT,
	DECODE_RUN
} DECODE_COMMAND;

/*
 * Per-decoder state. Historically file-global in decode_mp3.c, which made
 * the SDK DECODE_MP3 handler and the legacy register-driven FIFO path
 * (main.c) corrupt each other's stream, and blocked routing the SDK op to
 * core 1. Each caller now owns a context: main.c keeps the legacy FIFO
 * one; sdk_mailbox.c keeps one for the core-0 inline path and one for the
 * core-1 runner. Each context is used by exactly one core, so no cache
 * maintenance is needed on the context itself.
 */
struct mp3_decode_ctx {
	mp3dec_ex_t mp3d;
	mp3dec_io_t mp3io;
	mp3dec_frame_info_t frame_info;
	uint8_t *fifo_addr;
	unsigned long fifo_size;
	unsigned short old_fifo_write_idx;
	unsigned short fifo_write_idx;
	unsigned short fifo_read_idx;
};

int decode_mp3_init(struct mp3_decode_ctx *ctx, uint8_t *input_buffer,
                    size_t input_buffer_size);
int decode_mp3_init_fifo(struct mp3_decode_ctx *ctx, uint8_t *input_buffer,
                         size_t input_buffer_size);
void fifo_clear(struct mp3_decode_ctx *ctx);
void fifo_set_write_index(struct mp3_decode_ctx *ctx,
                          unsigned short aWriteIndex);
unsigned short fifo_get_read_index(const struct mp3_decode_ctx *ctx);
int decode_mp3_samples(struct mp3_decode_ctx *ctx, void *output_buffer,
                       int max_samples);
int mp3_get_hz(const struct mp3_decode_ctx *ctx);
int mp3_get_channels(const struct mp3_decode_ctx *ctx);
unsigned long mp3_get_bytes_consumed(const struct mp3_decode_ctx *ctx);

#endif /* ZZ9K_MP3_H */
