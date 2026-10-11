/*
 * Host tests for the bounded WebM demux (tracks, lacing, clusters, block
 * groups), the safety cap, keyframe checks and the backend helpers that
 * live beside them. Hostile sizes and split headers: webm_bounds_test.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ebml_build.h"
#include "webm_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Encoders put master and long elements inside Video and Audio: Colour
 * (here a copy of what ffmpeg writes for a BT.709 phone clip), Projection,
 * and others this demux does not read. They must be skipped, and the sizes
 * around them still read. */
static int test_track_metadata_masters(void)
{
	static struct webm_demux d;
	static struct got g;
	struct buf seg, info, video, colour, track, tracks, audio, atrack;
	struct buf cluster, file;
	uint8_t scale[3] = {0x0F, 0x42, 0x40};
	uint8_t frame[4] = {0x10, 0x00, 0x00, 1};
	uint8_t projection[20] = {0};
	float f48k = 48000.0f;
	const struct webm_track *v, *a;
	int failed = 0;

	memset(&seg, 0, sizeof(seg));
	memset(&info, 0, sizeof(info));
	memset(&video, 0, sizeof(video));
	memset(&colour, 0, sizeof(colour));
	memset(&track, 0, sizeof(track));
	memset(&tracks, 0, sizeof(tracks));
	memset(&audio, 0, sizeof(audio));
	memset(&atrack, 0, sizeof(atrack));
	memset(&cluster, 0, sizeof(cluster));
	belem(&info, 0x2AD7B1, scale, 3);
	bnest(&seg, 0x1549A966, &info);
	bu16(&video, 0xB0, 720);
	bu8(&colour, 0x55BA, 1);
	bu8(&colour, 0x55B1, 1);
	bu8(&colour, 0x55BB, 1);
	bu8(&colour, 0x55B9, 1);
	bnest(&video, 0x55B0, &colour);
	belem(&video, 0x7670, projection, sizeof(projection));
	bu16(&video, 0xBA, 1280);
	bu8(&track, 0xD7, 1);
	bu8(&track, 0x83, 1);
	belem(&track, 0x86, "V_VP9", 5);
	bnest(&track, 0xE0, &video);
	bnest(&tracks, 0xAE, &track);
	belem(&audio, 0x7E7B, projection, 12);
	bu8(&audio, 0x9F, 2);
	bfloat_be(&audio, 0xB5, &f48k, 4);
	bu8(&atrack, 0xD7, 2);
	bu8(&atrack, 0x83, 2);
	belem(&atrack, 0x86, "A_OPUS", 6);
	bnest(&atrack, 0xE1, &audio);
	bnest(&tracks, 0xAE, &atrack);
	bnest(&seg, 0x1654AE6B, &tracks);
	bu8(&cluster, 0xE7, 0);
	add_block(&cluster, 0xA3, 1, 0, 0x80, frame, sizeof(frame));
	bnest(&seg, 0x1F43B675, &cluster);
	file = make_file(&seg, SEG_SIZED);
	if (collect(&file, 1, &d, &g) != 0 || g.n != 1U) {
		fprintf(stderr, "metadata masters: n=%u err=%d\n", g.n, d.error);
		free(file.p);
		return 1;
	}
	v = webm_track(&d, 1);
	a = webm_track(&d, 2);
	if (!v || v->width != 720U || v->height != 1280U ||
	    v->codec != WEBM_CODEC_VP9 || !a || a->channels != 2U ||
	    a->rate != 48000U) {
		fprintf(stderr, "metadata masters: track fields\n");
		failed = 1;
	}
	free(file.p);
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
	failed |= test_track_metadata_masters();
	failed |= test_lacing();
	failed |= test_multi_cluster();
	failed |= test_blockgroup_keyframes();
	failed |= test_helpers();
	if (failed)
		fprintf(stderr, "webm_parse_test failed\n");
	return failed ? 1 : 0;
}
