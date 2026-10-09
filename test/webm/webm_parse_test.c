/*
 * Host tests for the bounded WebM demux (tracks, lacing, clusters, block
 * groups, hostile sizes, split headers), the safety cap, keyframe checks
 * and the backend helpers that live beside them.
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

/* ---- Composable builders for the structure-specific tests below. ---- */

#define SEG_SIZED 0
#define SEG_UNKNOWN 1
#define SEG_4GB 2  /* 0x100000010: past 32-bit offsets */
#define SEG_EDGE 3 /* 0xFFFFFFF0: fits 32 bits, end wraps */
#define HOSTILE 0xFFFFFFF4ULL
#define MAX_GOT 16

/* 8-byte EBML size, for sizes the 1/2-byte bsize cannot express. */
static void bsize64(struct buf *b, uint64_t v)
{
	int i;

	bbyte(b, 0x01);
	for (i = 6; i >= 0; i--)
		bbyte(b, (uint8_t)(v >> (8 * i)));
}

/* An element header claiming `size` bytes, followed by a little filler so
 * the parent has bytes after the header. */
static void bhostile(struct buf *b, uint32_t id, uint64_t size)
{
	static const uint8_t fill[4] = {0, 0, 0, 0};

	bid(b, id);
	bsize64(b, size);
	bput(b, fill, sizeof(fill));
}

static void bnest(struct buf *dst, uint32_t id, struct buf *body)
{
	struct buf wrapped = nest(id, body);

	bput(dst, wrapped.p, wrapped.n);
	free(wrapped.p);
	free(body->p);
	memset(body, 0, sizeof(*body));
}

static void bfloat_be(struct buf *b, uint32_t id, const void *v, uint32_t n)
{
	uint8_t le[8], be[8];
	uint32_t i;

	memcpy(le, v, n);
	for (i = 0U; i < n; i++)
		be[i] = le[n - 1U - i];
	belem(b, id, be, n);
}

static void add_video_track(struct buf *tracks)
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
static void add_head(struct buf *seg, struct buf *more_tracks)
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

static void add_block(struct buf *dst, uint32_t id, uint8_t track,
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

static struct buf make_file(struct buf *seg, int mode)
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
static int collect(const struct buf *f, int eof, struct webm_demux *d,
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

static void pattern(uint8_t *p, uint32_t n, uint8_t seed)
{
	uint32_t i;

	for (i = 0U; i < n; i++)
		p[i] = (uint8_t)(seed + i * 3U);
}

static int test_audio_tracks(void)
{
	static const uint8_t opus_head[19] = {
		'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01,
		0x80, 0xBB, 0, 0, 0, 0, 0
	};
	static const uint8_t vorbis_priv[11] = {
		2, 3, 2, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'
	};
	static struct webm_demux d;
	static struct got g;
	struct buf seg, more, audio, track, cluster, file;
	const struct webm_track *t;
	const uint8_t *priv, *pkt[3];
	uint32_t len[3];
	uint8_t frame[4] = {0xF8, 1, 2, 3};
	float f48k = 48000.0f;
	double d44k = 44100.0;
	int failed = 0, pass;

	for (pass = 0; pass < 2; pass++) {
		int opus = pass == 0;

		memset(&seg, 0, sizeof(seg));
		memset(&more, 0, sizeof(more));
		memset(&audio, 0, sizeof(audio));
		memset(&track, 0, sizeof(track));
		memset(&cluster, 0, sizeof(cluster));
		bu8(&audio, 0x9F, opus ? 2 : 1);
		if (opus)
			bfloat_be(&audio, 0xB5, &f48k, 4);
		else
			bfloat_be(&audio, 0xB5, &d44k, 8);
		bu8(&track, 0xD7, 2);
		bu8(&track, 0x83, 2);
		if (opus) {
			belem(&track, 0x86, "A_OPUS", 6);
			belem(&track, 0x63A2, opus_head, sizeof(opus_head));
		} else {
			belem(&track, 0x86, "A_VORBIS", 8);
			belem(&track, 0x63A2, vorbis_priv, sizeof(vorbis_priv));
		}
		bnest(&track, 0xE1, &audio);
		bnest(&more, 0xAE, &track);
		add_head(&seg, &more);
		bu8(&cluster, 0xE7, 0);
		add_block(&cluster, 0xA3, 2, 0, 0x80, frame, sizeof(frame));
		bnest(&seg, 0x1F43B675, &cluster);
		file = make_file(&seg, SEG_SIZED);
		if (collect(&file, 1, &d, &g) != 0 || g.n != 1U) {
			fprintf(stderr, "audio %d: n=%u err=%d\n", pass, g.n,
				d.error);
			failed = 1;
			free(file.p);
			continue;
		}
		t = webm_track(&d, 2);
		priv = webm_track_priv(&d, t);
		if (!t || t->type != 2 ||
		    t->codec != (opus ? WEBM_CODEC_OPUS : WEBM_CODEC_VORBIS) ||
		    t->channels != (opus ? 2U : 1U) ||
		    t->rate != (opus ? 48000U : 44100U) ||
		    t->priv_len != (opus ? sizeof(opus_head)
					 : sizeof(vorbis_priv)) ||
		    !priv ||
		    memcmp(priv, opus ? opus_head : vorbis_priv,
			   t->priv_len) != 0 ||
		    g.track[0] != 2U || g.codec[0] != t->codec ||
		    g.size[0] != sizeof(frame) ||
		    memcmp(g.data[0], frame, sizeof(frame)) != 0) {
			fprintf(stderr, "audio %d: track fields\n", pass);
			failed = 1;
		}
		if (!opus && (!webm_xiph_split(priv, t->priv_len, pkt, len) ||
			      len[0] != 3U || len[1] != 2U || len[2] != 3U ||
			      memcmp(pkt[0], "abc", 3) != 0 ||
			      memcmp(pkt[1], "de", 2) != 0 ||
			      memcmp(pkt[2], "fgh", 3) != 0)) {
			fprintf(stderr, "vorbis xiph split\n");
			failed = 1;
		}
		if (webm_track(&d, 1) == 0 || webm_track(&d, 1)->width != 160U ||
		    webm_track(&d, 1)->default_duration_ns != 40000000U) {
			fprintf(stderr, "audio %d: video track lost\n", pass);
			failed = 1;
		}
		free(file.p);
	}
	return failed;
}

static int check_laces(const struct got *g, unsigned first, unsigned count,
		       const uint32_t *sizes, uint8_t seed, const char *name)
{
	unsigned i;
	uint8_t want[320];

	for (i = 0U; i < count; i++) {
		const unsigned k = first + i;

		pattern(want, sizes[i], (uint8_t)(seed + i * 16U));
		if (k >= g->n || g->index[k] != i || g->count[k] != count ||
		    g->size[k] != sizes[i] ||
		    memcmp(g->data[k], want, sizes[i]) != 0) {
			fprintf(stderr, "%s lace %u: idx=%u cnt=%u size=%u\n",
				name, i, k < g->n ? g->index[k] : 0U,
				k < g->n ? g->count[k] : 0U,
				k < g->n ? g->size[k] : 0U);
			return 1;
		}
	}
	return 0;
}

/* One SimpleBlock payload: lace header bytes then frames with pattern(). */
static void add_laced(struct buf *cluster, uint8_t flags, const uint8_t *hdr,
		      uint32_t hdr_len, const uint32_t *sizes, unsigned count,
		      uint8_t seed)
{
	struct buf p;
	uint8_t frame[320];
	unsigned i;

	memset(&p, 0, sizeof(p));
	bput(&p, hdr, hdr_len);
	for (i = 0U; i < count; i++) {
		pattern(frame, sizes[i], (uint8_t)(seed + i * 16U));
		bput(&p, frame, sizes[i]);
	}
	add_block(cluster, 0xA3, 1, 0, flags, p.p, p.n);
	free(p.p);
}

static int test_lacing(void)
{
	static const uint32_t xiph[3] = {300, 2, 4};
	static const uint8_t xiph_hdr[4] = {2, 255, 45, 2};
	static const uint32_t fixed[3] = {3, 3, 3};
	static const uint8_t fixed_hdr[1] = {2};
	static const uint32_t ebml[4] = {5, 205, 3, 4};
	/* 5; +200 (2-byte signed vint, bias 8191); -202. */
	static const uint8_t ebml_hdr[6] = {3, 0x85, 0x60, 0xC7, 0x5F, 0x35};
	static struct webm_demux d;
	static struct got g;
	struct buf seg, cluster, file;
	int failed = 0;

	memset(&seg, 0, sizeof(seg));
	memset(&cluster, 0, sizeof(cluster));
	add_head(&seg, 0);
	bu8(&cluster, 0xE7, 0);
	add_laced(&cluster, 0x82, xiph_hdr, sizeof(xiph_hdr), xiph, 3, 0x10);
	add_laced(&cluster, 0x84, fixed_hdr, sizeof(fixed_hdr), fixed, 3, 0x40);
	add_laced(&cluster, 0x86, ebml_hdr, sizeof(ebml_hdr), ebml, 4, 0x70);
	bnest(&seg, 0x1F43B675, &cluster);
	file = make_file(&seg, SEG_SIZED);
	if (collect(&file, 1, &d, &g) != 0 || g.n != 10U) {
		fprintf(stderr, "lacing: n=%u err=%d\n", g.n, d.error);
		free(file.p);
		return 1;
	}
	failed |= check_laces(&g, 0U, 3U, xiph, 0x10, "xiph");
	failed |= check_laces(&g, 3U, 3U, fixed, 0x40, "fixed");
	failed |= check_laces(&g, 6U, 4U, ebml, 0x70, "ebml");
	free(file.p);
	return failed;
}

static int test_multi_cluster(void)
{
	static const int64_t want[5] = {0, 33, 100, 120, 205};
	static struct webm_demux d;
	static struct got g;
	struct buf seg, a, c, file;
	uint8_t frame[3] = {0x10, 0, 0};
	unsigned i;
	int eof, rc, failed = 0;

	for (eof = 0; eof < 2; eof++) {
		memset(&seg, 0, sizeof(seg));
		memset(&a, 0, sizeof(a));
		memset(&c, 0, sizeof(c));
		add_head(&seg, 0);
		bu8(&a, 0xE7, 0);
		add_block(&a, 0xA3, 1, 0, 0x80, frame, 3);
		add_block(&a, 0xA3, 1, 33, 0x00, frame, 3);
		bnest(&seg, 0x1F43B675, &a);
		/* Top-level element between Clusters is skipped by size. */
		belem(&seg, 0xEC, "\0\0\0", 3);
		/* Unknown-size Cluster ends at the next Cluster ID. */
		bid(&seg, 0x1F43B675);
		bbyte(&seg, 0xFF);
		bu8(&seg, 0xE7, 100);
		add_block(&seg, 0xA3, 1, 0, 0x80, frame, 3);
		add_block(&seg, 0xA3, 1, 20, 0x00, frame, 3);
		bu8(&c, 0xE7, 200);
		add_block(&c, 0xA3, 1, 5, 0x00, frame, 3);
		bnest(&seg, 0x1F43B675, &c);
		file = make_file(&seg, SEG_UNKNOWN);
		/* Without EOF the stream ends in NEED once the bytes run out. */
		rc = collect(&file, eof, &d, &g);
		if ((eof ? rc != 0 : d.error != WEBM_ERR_NEED) || g.n != 5U)
			failed = 1;
		for (i = 0U; i < g.n && i < 5U; i++)
			if (g.tc[i] != want[i] || g.key[i] != (i == 0U || i == 2U))
				failed = 1;
		if (failed)
			fprintf(stderr, "multi-cluster eof=%d: n=%u err=%d\n", eof,
				g.n, d.error);
		free(file.p);
	}
	return failed;
}

static int test_blockgroup_keyframes(void)
{
	static const uint8_t want_key[5] = {0, 0, 1, 0, 0};
	static struct webm_demux d;
	static struct got g;
	struct buf seg, cluster, group, file;
	uint8_t frame[3] = {0x10, 0, 0};
	uint8_t laced[6] = {1, 2, 0xA, 0xB, 0xC, 0xD};
	uint8_t ref = 0xDF; /* -33 */
	unsigned i;
	int failed = 0;

	memset(&seg, 0, sizeof(seg));
	memset(&cluster, 0, sizeof(cluster));
	memset(&group, 0, sizeof(group));
	add_head(&seg, 0);
	bu8(&cluster, 0xE7, 0);
	/* Block before ReferenceBlock: not a keyframe. */
	add_block(&group, 0xA1, 1, 0, 0, frame, 3);
	belem(&group, 0xFB, &ref, 1);
	bnest(&cluster, 0xA0, &group);
	/* ReferenceBlock before Block: not a keyframe. */
	belem(&group, 0xFB, &ref, 1);
	add_block(&group, 0xA1, 1, 1, 0, frame, 3);
	bnest(&cluster, 0xA0, &group);
	/* No ReferenceBlock: keyframe. */
	add_block(&group, 0xA1, 1, 2, 0, frame, 3);
	bnest(&cluster, 0xA0, &group);
	/* Laced Block then ReferenceBlock: every lace is not a keyframe. */
	add_block(&group, 0xA1, 1, 3, 0x02, laced, sizeof(laced));
	belem(&group, 0xFB, &ref, 1);
	bnest(&cluster, 0xA0, &group);
	bnest(&seg, 0x1F43B675, &cluster);
	file = make_file(&seg, SEG_SIZED);
	if (collect(&file, 1, &d, &g) != 0 || g.n != 5U) {
		fprintf(stderr, "blockgroup: n=%u err=%d\n", g.n, d.error);
		free(file.p);
		return 1;
	}
	for (i = 0U; i < 5U; i++) {
		if (g.key[i] != want_key[i]) {
			fprintf(stderr, "blockgroup %u: key=%u want=%u\n", i,
				g.key[i], want_key[i]);
			failed = 1;
		}
	}
	free(file.p);
	return failed;
}

/* Hostile sizes must fail closed with WEBM_ERR_LAYOUT: never wrap an
 * offset, re-parse earlier bytes or loop. `where` picks the element that
 * carries the hostile size. */
enum {
	H_TRACKS_CHILD,
	H_INFO_CHILD,
	H_TRACKENTRY_CHILD,
	H_SEGMENT_CHILD,
	H_CLUSTER_CHILD,
	H_CLUSTER,
	H_GROUP,
	H_GROUP_CHILD,
	H_GROUP_BLOCK64,
	H_SIMPLE64,
	H_COUNT
};

static struct buf make_hostile(int where, int seg_mode)
{
	struct buf seg, info, tracks, track, video, cluster, group;
	uint8_t scale[3] = {0x0F, 0x42, 0x40};
	uint8_t frame[3] = {0x10, 0, 0};

	memset(&seg, 0, sizeof(seg));
	memset(&info, 0, sizeof(info));
	memset(&tracks, 0, sizeof(tracks));
	memset(&track, 0, sizeof(track));
	memset(&video, 0, sizeof(video));
	memset(&cluster, 0, sizeof(cluster));
	memset(&group, 0, sizeof(group));
	belem(&info, 0x2AD7B1, scale, 3);
	if (where == H_INFO_CHILD)
		bhostile(&info, 0x4D80, HOSTILE);
	bnest(&seg, 0x1549A966, &info);
	bu16(&video, 0xB0, 160);
	bu16(&video, 0xBA, 120);
	bu8(&track, 0xD7, 1);
	bu8(&track, 0x83, 1);
	belem(&track, 0x86, "V_VP8", 5);
	bnest(&track, 0xE0, &video);
	if (where == H_TRACKENTRY_CHILD)
		bhostile(&track, 0x536E, HOSTILE);
	bnest(&tracks, 0xAE, &track);
	if (where == H_TRACKS_CHILD)
		bhostile(&tracks, 0xAE, HOSTILE);
	bnest(&seg, 0x1654AE6B, &tracks);
	if (where == H_SEGMENT_CHILD)
		bhostile(&seg, 0xEC, HOSTILE);
	bu8(&cluster, 0xE7, 0);
	add_block(&cluster, 0xA3, 1, 0, 0x80, frame, 3);
	if (where == H_CLUSTER_CHILD)
		bhostile(&cluster, 0xEC, HOSTILE);
	if (where == H_GROUP)
		bhostile(&cluster, 0xA0, HOSTILE);
	if (where == H_SIMPLE64)
		bhostile(&cluster, 0xA3, 0x100000010ULL);
	if (where == H_GROUP_CHILD || where == H_GROUP_BLOCK64) {
		if (where == H_GROUP_CHILD)
			bhostile(&group, 0xEC, HOSTILE);
		else
			bhostile(&group, 0xA1, 0x100000010ULL);
		add_block(&group, 0xA1, 1, 1, 0, frame, 3);
		bnest(&cluster, 0xA0, &group);
	}
	if (where == H_CLUSTER) {
		bhostile(&seg, 0x1F43B675, HOSTILE);
		free(cluster.p);
	} else {
		bnest(&seg, 0x1F43B675, &cluster);
	}
	return make_file(&seg, seg_mode);
}

static int test_hostile_sizes(void)
{
	static struct webm_demux d;
	static struct got g;
	int where, mode, failed = 0;

	for (where = 0; where < H_COUNT; where++) {
		for (mode = SEG_SIZED; mode <= SEG_UNKNOWN; mode++) {
			int eof;

			/* A Cluster in an unbounded stream counts as unknown
			 * size instead (covered by test_segment_sizes). */
			if (where == H_CLUSTER && mode == SEG_UNKNOWN)
				continue;
			for (eof = 0; eof < 2; eof++) {
				struct buf file = make_hostile(where, mode);
				int rc = collect(&file, eof, &d, &g);

				if (rc != -1 || d.error != WEBM_ERR_LAYOUT) {
					fprintf(stderr,
						"hostile %d seg=%d eof=%d: rc=%d err=%d n=%u\n",
						where, mode, eof, rc, d.error, g.n);
					failed = 1;
				}
				free(file.p);
			}
		}
	}
	return failed;
}

/* A Segment whose declared end does not fit 32 bits is bounded by the
 * input (or unbounded), not by a wrapped offset. */
static int test_segment_sizes(void)
{
	static struct webm_demux d;
	static struct got g;
	int mode, eof, failed = 0;

	for (mode = SEG_4GB; mode <= SEG_EDGE; mode++) {
		for (eof = 0; eof < 2; eof++) {
			struct buf seg, cluster, file;
			uint8_t frame[3] = {0x10, 0, 0};
			int rc;

			memset(&seg, 0, sizeof(seg));
			memset(&cluster, 0, sizeof(cluster));
			add_head(&seg, 0);
			bu8(&cluster, 0xE7, 0);
			add_block(&cluster, 0xA3, 1, 0, 0x80, frame, 3);
			bnest(&seg, 0x1F43B675, &cluster);
			file = make_file(&seg, mode);
			rc = collect(&file, eof, &d, &g);
			if (g.n != 1U || d.segment_end != (eof ? file.n : 0U) ||
			    (eof ? rc != 0 : d.error != WEBM_ERR_NEED)) {
				fprintf(stderr,
					"segment mode=%d eof=%d: rc=%d err=%d n=%u end=%u\n",
					mode, eof, rc, d.error, g.n,
					d.segment_end);
				failed = 1;
			}
			free(file.p);
		}
	}
	/* A 4 GB Cluster in an unbounded Segment runs like unknown size. */
	{
		struct buf seg, file;
		uint8_t frame[3] = {0x10, 0, 0};
		int rc;

		memset(&seg, 0, sizeof(seg));
		add_head(&seg, 0);
		bid(&seg, 0x1F43B675);
		bsize64(&seg, 0x100000010ULL);
		bu8(&seg, 0xE7, 7);
		add_block(&seg, 0xA3, 1, 0, 0x80, frame, 3);
		file = make_file(&seg, SEG_UNKNOWN);
		rc = collect(&file, 0, &d, &g);
		if (g.n != 1U || g.tc[0] != 7 || d.error != WEBM_ERR_NEED) {
			fprintf(stderr, "4 GB cluster: rc=%d err=%d n=%u\n", rc,
				d.error, g.n);
			failed = 1;
		}
		free(file.p);
	}
	return failed;
}

/* Every prefix of a valid header is "need more input", never an error:
 * a client may split the header across writes at any byte. */
static int test_header_prefixes(void)
{
	static struct webm_demux d;
	static uint8_t scratch[8192];
	struct buf seg, cluster, file;
	uint8_t frame[3] = {0x10, 0, 0};
	uint32_t len;
	int failed = 0;

	memset(&seg, 0, sizeof(seg));
	memset(&cluster, 0, sizeof(cluster));
	add_head(&seg, 0);
	bu8(&cluster, 0xE7, 0);
	add_block(&cluster, 0xA3, 1, 0, 0x80, frame, 3);
	bnest(&seg, 0x1F43B675, &cluster);
	file = make_file(&seg, SEG_UNKNOWN);
	for (len = 0U; len < file.n; len++) {
		int rc = open_file(file.p, len, 0, &d, scratch, sizeof(scratch));

		if (rc != 0 && d.error != WEBM_ERR_NEED) {
			fprintf(stderr, "prefix %u/%u: err=%d\n", len, file.n,
				d.error);
			failed = 1;
		}
	}
	free(file.p);
	return failed;
}

static int test_helpers(void)
{
	struct webm_skip s;
	struct webm_block key, inter;
	uint8_t vp8_key[3] = {0x10, 0, 0};
	uint8_t vp8_inter[3] = {0x11, 0, 0};
	uint64_t pts = 0U;
	const uint8_t *pkt[3];
	uint32_t len[3];
	uint8_t bad[4] = {2, 255, 255, 1};
	unsigned i;
	int failed = 0;

	memset(&key, 0, sizeof(key));
	key.codec = WEBM_CODEC_VP8;
	key.data = vp8_key;
	key.size = 3;
	inter = key;
	inter.data = vp8_inter;
	/* Inactive: everything decodes. */
	webm_skip_start(&s, 0, 4U);
	if (webm_skip_block(&s, &inter) != WEBM_SKIP_DECODE)
		failed = 1;
	/* Active: inter frames drop, the 4th asks to yield, keys decode. */
	webm_skip_start(&s, 1, 4U);
	for (i = 0U; i < 3U; i++)
		if (webm_skip_block(&s, &inter) != WEBM_SKIP_DROP)
			failed = 1;
	if (webm_skip_block(&s, &key) != WEBM_SKIP_DECODE ||
	    webm_skip_block(&s, &inter) != WEBM_SKIP_YIELD)
		failed = 1;

	if (!webm_pts90(1000, 1000000U, &pts) || pts != 90000U ||
	    !webm_pts90(-5, 1000000U, &pts) || pts != 0U ||
	    !webm_pts90(3, 0U, &pts) || pts != 270U ||
	    webm_pts90(INT64_MAX, 1000000000U, &pts))
		failed = 1;
	if (webm_rate_milli(0U) != 30000U ||
	    webm_rate_milli(40000000U) != 25000U ||
	    webm_rate_milli(41708333U) != 23976U ||
	    webm_rate_milli(1000U) != 30000U)
		failed = 1;
	if (webm_xiph_split(bad, sizeof(bad), pkt, len))
		failed = 1;
	if (failed)
		fprintf(stderr, "helpers\n");
	return failed;
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
	failed |= test_audio_tracks();
	failed |= test_lacing();
	failed |= test_multi_cluster();
	failed |= test_blockgroup_keyframes();
	failed |= test_hostile_sizes();
	failed |= test_segment_sizes();
	failed |= test_helpers();
	failed |= test_header_prefixes();
	if (failed)
		fprintf(stderr, "webm_parse_test failed\n");
	return failed ? 1 : 0;
}
