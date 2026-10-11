/*
 * EBML byte builders and an in-memory demux harness shared by the WebM
 * host tests (webm_parse_test.c, webm_bounds_test.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EBML_BUILD_H
#define EBML_BUILD_H

#include "webm_parse.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct mem_io {
	const uint8_t *data;
	uint32_t len;
	uint32_t pos;
	int eof;
};

static inline int mem_read(void *ctx, void *dst, uint32_t n)
{
	struct mem_io *m = ctx;
	uint32_t avail = m->pos < m->len ? m->len - m->pos : 0U;

	if (avail == 0U)
		return m->eof ? 0 : -2;
	if (n > avail)
		n = avail;
	memcpy(dst, m->data + m->pos, n);
	m->pos += n;
	return (int)n;
}

static inline int mem_seek(void *ctx, uint32_t pos)
{
	struct mem_io *m = ctx;

	if (pos > m->len)
		return m->eof ? -1 : -2;
	m->pos = pos;
	return 0;
}

static inline uint32_t mem_tell(void *ctx)
{
	return ((struct mem_io *)ctx)->pos;
}

static inline uint32_t mem_size(void *ctx)
{
	return ((struct mem_io *)ctx)->len;
}

struct buf {
	uint8_t *p;
	uint32_t n;
	uint32_t cap;
};

static inline void bput(struct buf *b, const void *src, uint32_t n)
{
	if (b->n + n > b->cap) {
		uint32_t cap = b->cap ? b->cap * 2U : 256U;

		while (cap < b->n + n)
			cap *= 2U;
		b->p = realloc(b->p, cap);
		b->cap = cap;
	}
	if (n)
		memcpy(b->p + b->n, src, n);
	b->n += n;
}

static inline void bbyte(struct buf *b, uint8_t v)
{
	bput(b, &v, 1);
}

static inline void bid(struct buf *b, uint32_t id)
{
	uint8_t tmp[4];
	uint32_t n = 1U;
	uint32_t i;
	uint32_t v = id;

	while (v > 0xFFU) {
		v >>= 8;
		n++;
	}
	for (i = 0U; i < n; i++)
		tmp[i] = (uint8_t)((id >> (8U * (n - 1U - i))) & 0xFFU);
	bput(b, tmp, n);
}

static inline void bsize(struct buf *b, uint32_t n)
{
	if (n < 0x7FU)
		bbyte(b, (uint8_t)(0x80U | n));
	else {
		bbyte(b, (uint8_t)(0x40U | (n >> 8)));
		bbyte(b, (uint8_t)n);
	}
}

static inline void belem(struct buf *b, uint32_t id, const void *payload, uint32_t n)
{
	bid(b, id);
	bsize(b, n);
	bput(b, payload, n);
}

static inline void bu8(struct buf *b, uint32_t id, uint8_t v)
{
	belem(b, id, &v, 1);
}

static inline void bu16(struct buf *b, uint32_t id, uint16_t v)
{
	uint8_t p[2];

	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
	belem(b, id, p, 2);
}

static inline struct buf nest(uint32_t id, struct buf *body)
{
	struct buf out;

	memset(&out, 0, sizeof(out));
	bid(&out, id);
	bsize(&out, body->n);
	bput(&out, body->p, body->n);
	return out;
}

/* make_file Segment size modes. */

#define SEG_SIZED 0
#define SEG_UNKNOWN 1
#define SEG_4GB 2  /* 0x100000010: past 32-bit offsets */
#define SEG_EDGE 3 /* 0xFFFFFFF0: fits 32 bits, end wraps */

/* 8-byte EBML size, for sizes the 1/2-byte bsize cannot express. */
static inline void bsize64(struct buf *b, uint64_t v)
{
	int i;

	bbyte(b, 0x01);
	for (i = 6; i >= 0; i--)
		bbyte(b, (uint8_t)(v >> (8 * i)));
}

static inline void bnest(struct buf *dst, uint32_t id, struct buf *body)
{
	struct buf wrapped = nest(id, body);

	bput(dst, wrapped.p, wrapped.n);
	free(wrapped.p);
	free(body->p);
	memset(body, 0, sizeof(*body));
}

static inline void bfloat_be(struct buf *b, uint32_t id, const void *v, uint32_t n)
{
	uint8_t le[8], be[8];
	uint32_t i;

	memcpy(le, v, n);
	for (i = 0U; i < n; i++)
		be[i] = le[n - 1U - i];
	belem(b, id, be, n);
}

static inline void add_video_track(struct buf *tracks)
{
	struct buf video, track;

	memset(&video, 0, sizeof(video));
	memset(&track, 0, sizeof(track));
	bu16(&video, 0xB0, 160);
	bu16(&video, 0xBA, 120);
	bu8(&track, 0xD7, 1);
	bu8(&track, 0x83, 1);
	belem(&track, 0x86, "V_VP8", 5);
	/* DefaultDuration 40 ms: header-prefix tests cut through it too. */
	belem(&track, 0x23E383, (const uint8_t *)"\x02\x62\x5A\x00", 4);
	bnest(&track, 0xE0, &video);
	bnest(tracks, 0xAE, &track);
}

/* Info (1 ms scale) + Tracks with one VP8 track and optional extra tracks
 * body, appended to a Segment body. */
static inline void add_head(struct buf *seg, struct buf *more_tracks)
{
	struct buf info, tracks;
	uint8_t scale[3] = {0x0F, 0x42, 0x40};

	memset(&info, 0, sizeof(info));
	memset(&tracks, 0, sizeof(tracks));
	belem(&info, 0x2AD7B1, scale, 3);
	bnest(seg, 0x1549A966, &info);
	add_video_track(&tracks);
	if (more_tracks) {
		bput(&tracks, more_tracks->p, more_tracks->n);
		free(more_tracks->p);
		memset(more_tracks, 0, sizeof(*more_tracks));
	}
	bnest(seg, 0x1654AE6B, &tracks);
}

static inline void add_block(struct buf *dst, uint32_t id, uint8_t track,
		      int16_t rel, uint8_t flags, const uint8_t *p, uint32_t n)
{
	struct buf b;

	memset(&b, 0, sizeof(b));
	bbyte(&b, (uint8_t)(0x80U | track));
	bbyte(&b, (uint8_t)((uint16_t)rel >> 8));
	bbyte(&b, (uint8_t)rel);
	bbyte(&b, flags);
	bput(&b, p, n);
	bnest(dst, id, &b);
}

static inline struct buf make_file(struct buf *seg, int mode)
{
	struct buf ebml, file;

	memset(&ebml, 0, sizeof(ebml));
	memset(&file, 0, sizeof(file));
	belem(&ebml, 0x4282, "webm", 4);
	bnest(&file, 0x1A45DFA3, &ebml);
	bid(&file, 0x18538067);
	if (mode == SEG_SIZED)
		bsize(&file, seg->n);
	else if (mode == SEG_UNKNOWN)
		bbyte(&file, 0xFF);
	else
		bsize64(&file, mode == SEG_4GB ? 0x100000010ULL : 0xFFFFFFF0ULL);
	bput(&file, seg->p, seg->n);
	free(seg->p);
	memset(seg, 0, sizeof(*seg));
	return file;
}

static inline int open_file(const uint8_t *data, uint32_t len, int eof,
		     struct webm_demux *d, uint8_t *scratch, uint32_t scap)
{
	/* The demux keeps io.ctx past this call: one live stream at a time. */
	static struct mem_io mem;
	struct webm_io io;

	memset(&mem, 0, sizeof(mem));
	memset(d, 0, sizeof(*d));
	mem.data = data;
	mem.len = len;
	mem.eof = eof;
	memset(&io, 0, sizeof(io));
	io.ctx = &mem;
	io.read = mem_read;
	io.seek = mem_seek;
	io.tell = mem_tell;
	io.size = eof ? mem_size : 0;
	return webm_open(d, &io, scratch, scap);
}

#define MAX_GOT 16

struct got {
	unsigned n;
	uint32_t track[MAX_GOT];
	uint8_t codec[MAX_GOT];
	uint8_t key[MAX_GOT];
	uint8_t index[MAX_GOT];
	uint8_t count[MAX_GOT];
	int64_t tc[MAX_GOT];
	uint32_t size[MAX_GOT];
	uint8_t data[MAX_GOT][320];
};

/* Demuxes a whole file; copies every block out. Returns the last webm_next
 * result (0 clean EOF) or the webm_open failure. */
static inline int collect(const struct buf *f, int eof, struct webm_demux *d,
		   struct got *g)
{
	static uint8_t scratch[8192];
	struct webm_block blk;
	int rc;

	memset(g, 0, sizeof(*g));
	rc = open_file(f->p, f->n, eof, d, scratch, sizeof(scratch));
	if (rc != 0)
		return rc;
	for (;;) {
		rc = webm_next(d, &blk);
		if (rc <= 0)
			return rc;
		if (g->n == MAX_GOT || blk.size > sizeof(g->data[0]))
			return -2;
		g->track[g->n] = blk.track;
		g->codec[g->n] = blk.codec;
		g->key[g->n] = blk.keyframe;
		g->index[g->n] = blk.lace_index;
		g->count[g->n] = blk.lace_count;
		g->tc[g->n] = blk.timecode;
		g->size[g->n] = blk.size;
		memcpy(g->data[g->n], blk.data, blk.size);
		g->n++;
	}
}

#endif /* EBML_BUILD_H */
