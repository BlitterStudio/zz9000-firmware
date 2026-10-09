/*
 * Host tests for the bounded WebM demux, the safety cap and keyframe checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "webm_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mem_io {
	const uint8_t *data;
	uint32_t len;
	uint32_t pos;
	int eof;
};

static int mem_read(void *ctx, void *dst, uint32_t n)
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

static int mem_seek(void *ctx, uint32_t pos)
{
	struct mem_io *m = ctx;

	if (pos > m->len)
		return m->eof ? -1 : -2;
	m->pos = pos;
	return 0;
}

static uint32_t mem_tell(void *ctx)
{
	return ((struct mem_io *)ctx)->pos;
}

static uint32_t mem_size(void *ctx)
{
	return ((struct mem_io *)ctx)->len;
}

struct buf {
	uint8_t *p;
	uint32_t n;
	uint32_t cap;
};

static void bput(struct buf *b, const void *src, uint32_t n)
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

static void bbyte(struct buf *b, uint8_t v)
{
	bput(b, &v, 1);
}

static void bid(struct buf *b, uint32_t id)
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

static void bsize(struct buf *b, uint32_t n)
{
	if (n < 0x7FU)
		bbyte(b, (uint8_t)(0x80U | n));
	else {
		bbyte(b, (uint8_t)(0x40U | (n >> 8)));
		bbyte(b, (uint8_t)n);
	}
}

static void belem(struct buf *b, uint32_t id, const void *payload, uint32_t n)
{
	bid(b, id);
	bsize(b, n);
	bput(b, payload, n);
}

static void bu8(struct buf *b, uint32_t id, uint8_t v)
{
	belem(b, id, &v, 1);
}

static void bu16(struct buf *b, uint32_t id, uint16_t v)
{
	uint8_t p[2];

	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
	belem(b, id, p, 2);
}

static struct buf nest(uint32_t id, struct buf *body)
{
	struct buf out;

	memset(&out, 0, sizeof(out));
	bid(&out, id);
	bsize(&out, body->n);
	bput(&out, body->p, body->n);
	return out;
}

/* lace: 0 none, 1 xiph two-frame. */
static struct buf make_webm(uint16_t w, uint16_t h, const char *codec,
			    int second_video, int encrypt, int lace, int trunc)
{
	struct buf info, video, track, tracks, block, cluster, seg, ebml, file;
	uint8_t scale[3] = {0x0F, 0x42, 0x40};
	uint8_t plain[] = {0x10, 0x00, 0x00, 1, 2, 3, 4, 5};
	uint8_t laced[] = {1, 2, 0x11, 0x22, 0x33, 0x44, 0x55};
	const char *webm = "webm";

	memset(&info, 0, sizeof(info));
	memset(&video, 0, sizeof(video));
	memset(&track, 0, sizeof(track));
	memset(&tracks, 0, sizeof(tracks));
	memset(&block, 0, sizeof(block));
	memset(&cluster, 0, sizeof(cluster));
	memset(&seg, 0, sizeof(seg));
	memset(&ebml, 0, sizeof(ebml));
	memset(&file, 0, sizeof(file));

	belem(&info, 0x2AD7B1, scale, 3);
	bu16(&video, 0xB0, w);
	bu16(&video, 0xBA, h);
	bu8(&track, 0xD7, 1);
	bu8(&track, 0x83, 1);
	belem(&track, 0x86, codec, (uint32_t)strlen(codec));
	{
		struct buf wrapped = nest(0xE0, &video);
		bput(&track, wrapped.p, wrapped.n);
		free(wrapped.p);
	}
	if (encrypt)
		belem(&track, 0x6D80, "x", 1);
	{
		struct buf wrapped = nest(0xAE, &track);
		bput(&tracks, wrapped.p, wrapped.n);
		free(wrapped.p);
	}
	if (second_video) {
		struct buf t2;
		memset(&t2, 0, sizeof(t2));
		bu8(&t2, 0xD7, 2);
		bu8(&t2, 0x83, 1);
		belem(&t2, 0x86, "V_VP8", 5);
		{
			struct buf wrapped = nest(0xAE, &t2);
			bput(&tracks, wrapped.p, wrapped.n);
			free(wrapped.p);
		}
		free(t2.p);
	}
	bu8(&cluster, 0xE7, 0);
	bbyte(&block, 0x81);
	bbyte(&block, 0);
	bbyte(&block, 0);
	if (lace) {
		bbyte(&block, 0x82);
		bput(&block, laced, sizeof(laced));
	} else {
		bbyte(&block, 0x80);
		bput(&block, plain, sizeof(plain));
	}
	{
		struct buf wrapped = nest(0xA3, &block);
		bput(&cluster, wrapped.p, wrapped.n);
		free(wrapped.p);
	}
	{
		struct buf a = nest(0x1549A966, &info);
		struct buf b = nest(0x1654AE6B, &tracks);
		struct buf c = nest(0x1F43B675, &cluster);
		bput(&seg, a.p, a.n);
		bput(&seg, b.p, b.n);
		bput(&seg, c.p, c.n);
		free(a.p);
		free(b.p);
		free(c.p);
	}
	belem(&ebml, 0x4282, webm, 4);
	{
		struct buf a = nest(0x1A45DFA3, &ebml);
		struct buf b = nest(0x18538067, &seg);
		bput(&file, a.p, a.n);
		bput(&file, b.p, b.n);
		free(a.p);
		free(b.p);
	}
	if (trunc && file.n > 8U)
		file.n -= 8U;
	free(info.p);
	free(video.p);
	free(track.p);
	free(tracks.p);
	free(block.p);
	free(cluster.p);
	free(seg.p);
	free(ebml.p);
	return file;
}

static int open_file(const uint8_t *data, uint32_t len, int eof,
		     struct webm_demux *d, uint8_t *scratch, uint32_t scap)
{
	struct mem_io mem;
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

static int count_blocks(const uint8_t *data, uint32_t len, struct webm_demux *d,
			struct webm_block *last, unsigned *nblocks)
{
	uint8_t scratch[8192];
	int rc = open_file(data, len, 1, d, scratch, sizeof(scratch));

	*nblocks = 0U;
	if (rc != 0)
		return rc;
	for (;;) {
		rc = webm_next(d, last);
		if (rc == 0)
			return 0;
		if (rc < 0)
			return rc;
		(*nblocks)++;
	}
}

static int expect_ok(const char *name, uint16_t w, uint16_t h, const char *codec,
		     unsigned want)
{
	struct buf file = make_webm(w, h, codec, 0, 0, 0, 0);
	struct webm_demux d;
	struct webm_block blk;
	unsigned n = 0U;
	int rc = count_blocks(file.p, file.n, &d, &blk, &n);

	if (rc != 0 || n != want || d.tracks[0].width != w ||
	    d.tracks[0].height != h || !blk.keyframe) {
		fprintf(stderr, "%s: rc=%d err=%d n=%u %ux%u\n", name, rc,
			d.error, n, d.tracks[0].width, d.tracks[0].height);
		free(file.p);
		return 1;
	}
	webm_close(&d);
	free(file.p);
	return 0;
}

static int expect_err(const char *name, struct buf file, int err)
{
	struct webm_demux d;
	struct webm_block blk;
	unsigned n = 0U;
	int rc = count_blocks(file.p, file.n, &d, &blk, &n);

	if (rc == 0 || d.error != err) {
		fprintf(stderr, "%s: rc=%d err=%d want=%d\n", name, rc, d.error,
			err);
		free(file.p);
		return 1;
	}
	free(file.p);
	return 0;
}

int main(void)
{
	int failed = 0;
	struct buf laced = make_webm(160, 120, "V_VP8", 0, 0, 1, 0);
	struct webm_demux d;
	struct webm_block blk;
	unsigned n = 0U;
	uint8_t vp8_key[] = {0x10, 0x00, 0x00};
	uint8_t vp8_delta[] = {0x11, 0x00, 0x00};
	uint8_t vp9_key[] = {0x82, 0x49, 0x83};
	uint8_t vp9_inter[] = {0x86, 0x00};

	if (!webm_size_allowed(1920, 1088) || !webm_size_allowed(1080, 1920) ||
	    !webm_size_allowed(271, 481) || !webm_size_allowed(320, 564) ||
	    webm_size_allowed(2560, 1440) || webm_size_allowed(1920, 1089))
		failed = 1;
	if (!webm_payload_is_keyframe(WEBM_CODEC_VP8, vp8_key, 3) ||
	    webm_payload_is_keyframe(WEBM_CODEC_VP8, vp8_delta, 3) ||
	    !webm_payload_is_keyframe(WEBM_CODEC_VP9, vp9_key, 3) ||
	    webm_payload_is_keyframe(WEBM_CODEC_VP9, vp9_inter, 2))
		failed = 1;
	failed |= expect_ok("vp8", 160, 120, "V_VP8", 1);
	failed |= expect_ok("portrait", 320, 564, "V_VP8", 1);
	failed |= expect_ok("odd", 271, 481, "V_VP9", 1);
	failed |= expect_err("second", make_webm(160, 120, "V_VP8", 1, 0, 0, 0),
			     WEBM_ERR_CODEC);
	failed |= expect_err("unknown", make_webm(160, 120, "V_AV1", 0, 0, 0, 0),
			     WEBM_ERR_CODEC);
	failed |= expect_err("encrypt", make_webm(160, 120, "V_VP8", 0, 1, 0, 0),
			     WEBM_ERR_CODEC);
	failed |= expect_err("trunc", make_webm(160, 120, "V_VP8", 0, 0, 0, 1),
			     WEBM_ERR_TRUNC);
	if (count_blocks(laced.p, laced.n, &d, &blk, &n) != 0 || n != 2U) {
		fprintf(stderr, "lace n=%u err=%d\n", n, d.error);
		failed = 1;
	} else {
		webm_close(&d);
	}
	free(laced.p);
	if (failed)
		fprintf(stderr, "webm_parse_test failed\n");
	return failed ? 1 : 0;
}
