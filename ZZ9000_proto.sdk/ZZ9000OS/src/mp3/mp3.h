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
	size_t src_size;   /* length of the input buffer */
	size_t audio_end;  /* src_size without recognised trailing tags */
};

/* Where the decoder stands once decode_mp3_samples() has returned. */
enum mp3_input_state {
	MP3_INPUT_MORE,      /* frames or undelivered PCM remain */
	MP3_INPUT_COMPLETE,  /* every frame decoded, at most trailing tags left */
	MP3_INPUT_DAMAGED    /* the input ended in bytes that are no frame: a
	                      * truncated last frame or trailing junk, which
	                      * minimp3 skips without decoding */
};

int decode_mp3_init(struct mp3_decode_ctx *ctx, uint8_t *input_buffer,
                    size_t input_buffer_size);
int decode_mp3_samples(struct mp3_decode_ctx *ctx, void *output_buffer,
                       int max_samples);
int mp3_get_hz(const struct mp3_decode_ctx *ctx);
int mp3_get_channels(const struct mp3_decode_ctx *ctx);

/* Bytes of the input used. Once all the audio is decoded this is the input
 * length: a trailing ID3v1 or APEv2 tag counts as consumed. */
unsigned long mp3_get_bytes_consumed(const struct mp3_decode_ctx *ctx);
enum mp3_input_state mp3_input_state(const struct mp3_decode_ctx *ctx);

/* DECODE_MP3 with EXPECT_END: the input is the complete stream. Returns 0
 * unless the decode reached the end of a well-formed stream; stopping
 * because the output buffer (output_capacity bytes, bytes_written used) is
 * full is not an error. */
int mp3_expect_end_ok(const struct mp3_decode_ctx *ctx, size_t bytes_written,
                      size_t output_capacity);

/* Check whether the current input contains a complete decodable frame without
 * mutating the live decoder state. Used by resumable stream drains, which must
 * retain a frame split across later client buffers. */
int mp3_probe_complete_frame(const mp3dec_t *decoder,
                             const uint8_t *input_buffer,
                             int input_buffer_size,
                             int *decode_input_size);

#endif /* ZZ9K_MP3_H */
