/*
 * Native FLAC backend for the codec-aware audio stream (U7).
 *
 * The backend drives libFLAC's stream decoder over the session's bounded
 * compressed input ring. libFLAC pulls input through a read callback and
 * cannot suspend mid-frame, so the backend only hands it one complete unit
 * at a time: the "fLaC" marker plus STREAMINFO (re-flagged as the last
 * metadata block), then one whole frame. Every other metadata block is
 * validated and skipped by the backend itself while it streams through the
 * ring, so pictures/tags never need to be buffered. A frame is complete when
 * the next valid frame header (sync, CRC-8, matching geometry and a
 * continuous frame/sample number) is buffered, at FEED_EOF, or, during a
 * DRAIN, when the buffered tail carries a matching frame CRC-16.
 *
 * STREAMINFO is validated and gates the supported envelope at header time,
 * but no allocation is sized from it: libFLAC sizes its buffers from each
 * frame header, every frame header is checked against the stream geometry
 * before libFLAC sees it, and all libFLAC allocations are quota-bounded
 * (SDK_FLAC_ALLOC_LIMIT) through sdk_flac_alloc.h.
 *
 * Output: native rate and channels (mono/stereo), interleaved big-endian
 * PCM with each sample MSB-justified in its container (value << (container
 * bits - source bits)): S16BE for 4..16-bit sources, S32BE for 4..24-bit.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_AUDIO_FLAC_H
#define SDK_AUDIO_FLAC_H

#include <stddef.h>
#include <stdint.h>

#define SDK_FLAC_MIN_RATE        8000U
#define SDK_FLAC_MAX_RATE        192000U
#define SDK_FLAC_MAX_CHANNELS    2U
#define SDK_FLAC_MAX_BITS        24U
/* Per-stream ceiling for libFLAC heap state. A legal 65535-sample stereo
 * frame needs ~1 MiB of output/residual/side buffers; the rest is slack for
 * the decoder objects and the bitreader. */
#define SDK_FLAC_ALLOC_LIMIT     (2U * 1024U * 1024U)

struct sdk_flac_state {
	void *decoder;              /* FLAC__StreamDecoder *, NULL until first feed */
	size_t alloc_used;
	size_t alloc_peak;
	uint64_t total_samples;     /* STREAMINFO; 0 = unknown */
	uint64_t samples_decoded;
	uint32_t phase;
	uint32_t output_format;     /* SDK_AUDIO_SAMPLE_FORMAT_S16BE / S32BE */
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bits_per_sample;
	uint32_t container_bytes;
	uint32_t max_blocksize;
	uint32_t meta_remaining;    /* skipped metadata bytes still to discard */
	uint32_t meta_last;
	uint32_t scan_offset;       /* boundary scan resume point in the head frame */
	uint16_t status;            /* sticky failure status, 0 while healthy */
	/* Per-unit transient state shared with the libFLAC callbacks. */
	const uint8_t *grant;
	uint32_t grant_len;
	uint32_t overrun;
	uint32_t cb_error;
	uint32_t frames_written;
	uint8_t *pcm;
	uint32_t pcm_capacity;
	uint32_t pcm_pos;
	uint32_t pcm_limit;
	uint8_t header[42];
};

struct sdk_flac_io {
	/* in */
	const uint8_t *input;       /* contiguous unconsumed compressed bytes */
	uint32_t input_length;
	int eof;                    /* FEED_EOF: no further input will arrive */
	int drain;                  /* FEED_DRAIN: decode every complete frame */
	uint8_t *pcm;               /* PCM ring base */
	uint32_t pcm_capacity;
	uint32_t pcm_write;         /* ring offset of the next PCM byte */
	uint32_t pcm_free;          /* writable ring bytes */
	uint32_t pcm_budget;        /* soft cap on PCM produced by this call */
	/* out */
	uint32_t consumed;
	uint32_t produced;
	uint32_t frames;
	int starved;                /* blocked on an incomplete compressed unit */
	int complete;               /* end of stream reached and validated */
};

/* output_format must be S16BE or S32BE (validated by the caller). */
void sdk_flac_init(struct sdk_flac_state *st, uint32_t output_format);
/* Decode as many complete units as input, PCM room and budget allow.
 * Returns SDK_STATUS_OK or the sticky failure status (UNSUPPORTED for a
 * valid stream outside the envelope, IO_ERROR for malformed/corrupt/
 * truncated input, NO_MEMORY for allocation failure). */
uint16_t sdk_flac_decode(struct sdk_flac_state *st, struct sdk_flac_io *io);
/* Free the libFLAC decoder on the core that allocated it. */
void sdk_flac_release(struct sdk_flac_state *st);
/* Drop the decoder pointer after a core-1 reclaim already freed it. */
void sdk_flac_forget(struct sdk_flac_state *st);

#endif /* SDK_AUDIO_FLAC_H */
