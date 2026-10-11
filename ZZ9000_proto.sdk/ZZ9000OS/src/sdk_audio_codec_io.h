/*
 * Per-call I/O contract between the codec-aware audio stream (sdk_mailbox.c)
 * and the native decode backends (sdk_audio_flac.c, sdk_audio_vorbis.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_AUDIO_CODEC_IO_H
#define SDK_AUDIO_CODEC_IO_H

#include <stdint.h>

struct sdk_audio_codec_io {
	/* in */
	const uint8_t *input;       /* contiguous unconsumed compressed bytes */
	uint32_t input_length;
	int eof;                    /* FEED_EOF: no further input will arrive */
	int drain;                  /* FEED_DRAIN: decode every complete unit */
	uint8_t *pcm;               /* PCM ring base */
	uint32_t pcm_capacity;
	uint32_t pcm_write;         /* ring offset of the next PCM byte */
	uint32_t pcm_free;          /* writable ring bytes */
	uint32_t pcm_budget;        /* soft cap on PCM produced by this call */
	uint32_t pcm_unread;        /* published PCM the client has not read */
	/* out */
	uint32_t consumed;
	uint32_t produced;
	uint32_t frames;            /* codec units (frames/packets) decoded */
	int starved;                /* blocked on an incomplete compressed unit */
	int complete;               /* end of stream reached and validated */
};

#endif /* SDK_AUDIO_CODEC_IO_H */
