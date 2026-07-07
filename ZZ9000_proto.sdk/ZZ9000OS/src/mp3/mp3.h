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

/*
 * Per-decoder state for the SDK one-shot DECODE_MP3 op. sdk_mailbox.c owns
 * one context for the core-0 inline path and one for the core-1 runner;
 * each context is used by exactly one core, so no cache maintenance is
 * needed on the context itself. (The FIFO/streaming half of this API --
 * the legacy register-driven decoder -- was removed with the MHI
 * modernization; streaming MP3 now runs through the SDK audio-stream
 * sessions and the AX playback pump.)
 */
struct mp3_decode_ctx {
	mp3dec_ex_t mp3d;
	mp3dec_frame_info_t frame_info;
};

int decode_mp3_init(struct mp3_decode_ctx *ctx, uint8_t *input_buffer,
                    size_t input_buffer_size);
int decode_mp3_samples(struct mp3_decode_ctx *ctx, void *output_buffer,
                       int max_samples);
int mp3_get_hz(const struct mp3_decode_ctx *ctx);
int mp3_get_channels(const struct mp3_decode_ctx *ctx);
unsigned long mp3_get_bytes_consumed(const struct mp3_decode_ctx *ctx);

#endif /* ZZ9K_MP3_H */
