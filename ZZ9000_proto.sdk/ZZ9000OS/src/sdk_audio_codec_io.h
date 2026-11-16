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

/*
 * One decode call runs as a core-1 service task under the kernel system
 * lock, so no app thread can run on core 1 for its whole duration. The
 * client's high-water budget (up to the whole PCM ring) would hold the
 * lock for the full refill -- a measured 7.1M XTime global-timer ticks,
 * about 21.3 ms at COUNTS_PER_SECOND = CPU/2 = 333 MHz. Every decode path
 * therefore caps one pass to about 2 ms of decode (24 KiB of PCM) and later
 * passes (the playback pump's refill kick, the next FEED or READ) continue
 * the stream. A pass still always makes progress: the first frame decodes
 * even when larger than the cap, so a pass is bounded by the cap plus one
 * frame.
 */
#define SDK_AUDIO_DECODE_PASS_PCM_BYTES (24U * 1024U)

static inline uint32_t sdk_audio_decode_pass_budget(uint32_t pcm_budget)
{
	return pcm_budget > SDK_AUDIO_DECODE_PASS_PCM_BYTES ?
	       SDK_AUDIO_DECODE_PASS_PCM_BYTES : pcm_budget;
}
#endif /* SDK_AUDIO_CODEC_IO_H */
