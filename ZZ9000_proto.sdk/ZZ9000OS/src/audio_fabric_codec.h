/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUDIO_FABRIC_CODEC_H
#define AUDIO_FABRIC_CODEC_H

#include <stdint.h>

struct fabric_codec_input {
	const uint8_t *ring;
	uint64_t cursor;
	uint32_t capacity;
	uint32_t available;
	int end;
};

struct fabric_codec_view {
	uint32_t copied;
	uint32_t rate;
	uint32_t native_rate;
	uint32_t channels;
	uint32_t frames;
	int tail;
	uint32_t error;
	int complete;
};

/* Main-loop-only backend. The fabric owns the lease and its PCM staging;
 * the backend owns the decoder and its private compressed and PCM FIFOs. */
struct fabric_codec_ops {
	void (*begin)(uint32_t slot, uint32_t codec, uint32_t param);
	void (*end)(uint32_t slot);
	struct fabric_codec_view (*step)(uint32_t slot,
		const struct fabric_codec_input *input);
	void (*read_stereo)(uint32_t slot, int16_t *out, uint32_t frames);
};

void audio_fabric_codec_register(const struct fabric_codec_ops *ops);
extern const struct fabric_codec_ops audio_fabric_stream_codec_ops;

#endif
