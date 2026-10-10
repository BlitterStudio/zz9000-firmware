/*
 * Bounded WebM/Matroska demux for media sessions.
 *
 * Subset: one Segment, Info/TimestampScale, one video track (V_VP8 or
 * V_VP9) and one optional audio track (A_OPUS or A_VORBIS), Clusters of
 * SimpleBlock and BlockGroup/Block, lacing none/Xiph/fixed/EBML. Cues,
 * SeekHead, Chapters, Tags and Attachments are skipped by size. Unknown
 * element size is accepted only for Segment and Cluster; a Segment, or a
 * Cluster of an unbounded Segment, ending past the 32-bit offsets counts
 * as unknown. Other elements must fit their parent (WEBM_ERR_LAYOUT).
 * Encrypted tracks fail closed. Not a general demuxer.
 *
 * Streaming: read() returns -2 when the caller has no more bytes yet and
 * has not signalled EOF (mapped to WEBM_ERR_NEED). seek() is forward-only
 * except within bytes the caller still holds; it returns -2 for the same
 * not-yet-available case. On WEBM_ERR_NEED the demux state is unspecified
 * and the caller must restore a snapshot (or restart webm_open from
 * offset 0). Block bytes point into the caller scratch buffer and are
 * valid only until the next webm_next().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WEBM_PARSE_H
#define WEBM_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define WEBM_MAX_TRACKS 4
#define WEBM_MAX_PRIV (64U * 1024U)
#define WEBM_MAX_FRAME (4U * 1024U * 1024U)
#define WEBM_MAX_LACE 32
#define WEBM_SAFETY_MAX_SIDE 1920U
#define WEBM_SAFETY_MAX_PIXELS (1920U * 1088U)

enum {
	WEBM_CODEC_NONE = 0,
	WEBM_CODEC_VP8,
	WEBM_CODEC_VP9,
	WEBM_CODEC_OPUS,
	WEBM_CODEC_VORBIS
};

enum {
	WEBM_OK = 0,
	WEBM_ERR_IO,
	WEBM_ERR_TRUNC,
	WEBM_ERR_LIMIT,
	WEBM_ERR_CODEC,
	WEBM_ERR_LAYOUT,
	WEBM_ERR_NEED
};

struct webm_track {
	uint32_t number;
	uint8_t type; /* 1 video, 2 audio */
	uint8_t codec;
	uint16_t channels;
	uint32_t rate;
	uint32_t width;
	uint32_t height;
	uint32_t priv_off;
	uint32_t priv_len;
	uint64_t default_duration_ns;
};

struct webm_block {
	uint32_t track;
	uint8_t codec;
	uint8_t keyframe;
	uint8_t lace_index;
	uint8_t lace_count;
	int64_t timecode;    /* TimestampScale units */
	int64_t timecode_ms;
	const uint8_t *data;
	uint32_t size;
};

/* read: copy up to n bytes. Return count, 0 at EOF, -2 if more input
 * may still arrive. seek: absolute file offset; 0 ok, -1 hard error,
 * -2 not yet available. */
struct webm_io {
	void *ctx;
	int (*read)(void *ctx, void *dst, uint32_t n);
	int (*seek)(void *ctx, uint32_t pos);
	uint32_t (*tell)(void *ctx);
	uint32_t (*size)(void *ctx);
};

struct webm_demux {
	struct webm_io io;
	struct webm_track tracks[WEBM_MAX_TRACKS];
	unsigned ntracks;
	uint32_t scale_ns;
	uint8_t *buf;
	uint32_t buf_cap;
	int error;
	int have_video;
	int have_audio;
	uint32_t segment_end;
	uint32_t cluster_end;
	int64_t cluster_tc;
	int in_cluster;
	uint8_t lace_i;
	uint8_t lace_n;
	uint8_t lace_key;
	uint32_t lace_track;
	int64_t lace_tc;
	uint32_t lace_off[WEBM_MAX_LACE];
	uint32_t lace_len[WEBM_MAX_LACE];
	uint8_t priv_store[WEBM_MAX_PRIV];
	uint32_t priv_used;
};

int webm_open(struct webm_demux *d, const struct webm_io *io, uint8_t *buf,
	      uint32_t cap);
/* 1 = a block, 0 = clean EOF, -1 = error (d->error). */
int webm_next(struct webm_demux *d, struct webm_block *blk);
void webm_close(struct webm_demux *d);
const struct webm_track *webm_track(const struct webm_demux *d, uint32_t number);
const uint8_t *webm_track_priv(const struct webm_demux *d,
			       const struct webm_track *t);
/* Longest side and pixel count of a coded frame the card will accept. */
int webm_size_allowed(uint32_t width, uint32_t height);
/* Codec keyframe, not the muxer bit. VP9 show-existing is not a reset. */
int webm_payload_is_keyframe(uint8_t codec, const uint8_t *data, uint32_t size);

/* SKIP_TO_KEYFRAME gate for one decode call: blocks that are not codec
 * keyframes are dropped, and after `budget` drops the caller yields so a
 * long GOP cannot stall one command. Inactive gates pass every block. */
enum {
	WEBM_SKIP_DECODE = 0,
	WEBM_SKIP_DROP,
	WEBM_SKIP_YIELD
};

struct webm_skip {
	uint32_t left;
	uint8_t active;
};

void webm_skip_start(struct webm_skip *s, int active, uint32_t budget);
int webm_skip_block(struct webm_skip *s, const struct webm_block *blk);

/* TimestampScale ticks to the 90 kHz media clock; 0 on overflow. Negative
 * ticks clamp to 0. */
int webm_pts90(int64_t ticks, uint32_t scale_ns, uint64_t *out);
/* Frame rate in milli-fps from DefaultDuration; 30000 when absent or
 * implausible. */
uint32_t webm_rate_milli(uint64_t duration_ns);
/* Splits Xiph-laced Vorbis CodecPrivate into its three header packets. */
int webm_xiph_split(const uint8_t *p, uint32_t n, const uint8_t *pkt[3],
		    uint32_t len[3]);

#endif
