/*
 * Host tests for the WebM backend input path: the sliding window on its
 * own, then sdk_video_webm.c driven through its ops with libvpx, libopus
 * and Tremor replaced by the stubs below (stubs/ shadows their headers).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_video_backend.h"
#include "webm_parse.h"
#include "webm_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <opus/opus.h>
#include "tremor/ivorbiscodec.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

/* ---- Codec stubs. A stub VP8/VP9 "decode" logs payload byte 1 (the
 * test's frame tag) and yields one picture, unless the tag has bit 7 set
 * (an invisible frame: decodes without a picture). ---- */

struct vpx_codec_iface {
	int vp9;
};

static struct vpx_codec_iface vp8_iface = {0};
static struct vpx_codec_iface vp9_iface = {1};
static uint8_t decoded[256];
static unsigned ndecoded;
static uint8_t plane[1920U * 1088U];

vpx_codec_iface_t *vpx_codec_vp8_dx(void)
{
	return &vp8_iface;
}

vpx_codec_iface_t *vpx_codec_vp9_dx(void)
{
	return &vp9_iface;
}

vpx_codec_err_t vpx_codec_dec_init(vpx_codec_ctx_t *ctx,
				   vpx_codec_iface_t *iface,
				   const vpx_codec_dec_cfg_t *cfg, long flags)
{
	(void)flags;
	memset(ctx, 0, sizeof(*ctx));
	ctx->iface = iface;
	ctx->w = cfg->w;
	ctx->h = cfg->h;
	return VPX_CODEC_OK;
}

vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const uint8_t *data,
				 unsigned int size, void *user_priv,
				 long deadline)
{
	(void)user_priv;
	(void)deadline;
	if (!data || size < 2U)
		return VPX_CODEC_ERROR;
	if (ndecoded < sizeof(decoded))
		decoded[ndecoded++] = data[1];
	ctx->pending = (data[1] & 0x80U) == 0U;
	return VPX_CODEC_OK;
}

vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter)
{
	if (*iter || !ctx->pending)
		return 0;
	ctx->pending = 0;
	*iter = ctx;
	memset(&ctx->img, 0, sizeof(ctx->img));
	ctx->img.fmt = VPX_IMG_FMT_I420;
	ctx->img.d_w = ctx->w;
	ctx->img.d_h = ctx->h;
	ctx->img.planes[0] = plane;
	ctx->img.planes[1] = plane;
	ctx->img.planes[2] = plane;
	ctx->img.stride[0] = (int)ctx->w;
	ctx->img.stride[1] = (int)(ctx->w / 2U);
	ctx->img.stride[2] = (int)(ctx->w / 2U);
	return &ctx->img;
}

vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *ctx)
{
	(void)ctx;
	return VPX_CODEC_OK;
}

/* Audio is configured off in these tests; the stubs only satisfy the
 * linker and fail closed if reached. */
OpusDecoder *opus_decoder_create(opus_int32 fs, int channels, int *error)
{
	(void)fs;
	(void)channels;
	*error = -1;
	return 0;
}

void opus_decoder_destroy(OpusDecoder *st)
{
	(void)st;
}

int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len,
		opus_int16 *pcm, int frame_size, int decode_fec)
{
	(void)st;
	(void)data;
	(void)len;
	(void)pcm;
	(void)frame_size;
	(void)decode_fec;
	return -1;
}

int opus_packet_get_nb_samples(const unsigned char *packet, opus_int32 len,
			       opus_int32 fs)
{
	(void)packet;
	(void)len;
	(void)fs;
	return -1;
}

void vorbis_info_init(vorbis_info *vi)
{
	memset(vi, 0, sizeof(*vi));
}

void vorbis_info_clear(vorbis_info *vi)
{
	(void)vi;
}

void vorbis_comment_init(vorbis_comment *vc)
{
	memset(vc, 0, sizeof(*vc));
}

void vorbis_comment_clear(vorbis_comment *vc)
{
	(void)vc;
}

int vorbis_synthesis_headerin(vorbis_info *vi, vorbis_comment *vc,
			      ogg_packet *op)
{
	(void)vi;
	(void)vc;
	(void)op;
	return -1;
}

int vorbis_synthesis_init(vorbis_dsp_state *v, vorbis_info *vi)
{
	(void)v;
	(void)vi;
	return -1;
}

int vorbis_block_init(vorbis_dsp_state *v, vorbis_block *vb)
{
	(void)v;
	(void)vb;
	return -1;
}

int vorbis_block_clear(vorbis_block *vb)
{
	(void)vb;
	return 0;
}

void vorbis_dsp_clear(vorbis_dsp_state *v)
{
	(void)v;
}

int vorbis_synthesis(vorbis_block *vb, ogg_packet *op)
{
	(void)vb;
	(void)op;
	return -1;
}

int vorbis_synthesis_blockin(vorbis_dsp_state *v, vorbis_block *vb)
{
	(void)v;
	(void)vb;
	return -1;
}

int vorbis_synthesis_pcmout(vorbis_dsp_state *v, ogg_int32_t ***pcm)
{
	(void)v;
	(void)pcm;
	return 0;
}

int vorbis_synthesis_read(vorbis_dsp_state *v, int samples)
{
	(void)v;
	(void)samples;
	return 0;
}

/* ---- Sliding window. ---- */

static void test_window(void)
{
	uint8_t store[32];
	uint8_t src[64];
	uint8_t out[64];
	struct webm_window w;
	struct webm_io io;
	uint32_t i;

	for (i = 0U; i < sizeof(src); i++)
		src[i] = (uint8_t)(i + 1U);
	webm_window_init(&w, store, sizeof(store));
	webm_window_bind(&w, &io);
	check(io.read(io.ctx, out, 1) == -2, "empty window needs input");
	check(io.tell(io.ctx) == 0U, "empty window at offset 0");

	/* Reads span append boundaries and stop at the filled edge. */
	check(webm_window_append(&w, src, 3) &&
	      webm_window_append(&w, src + 3, 4), "append two chunks");
	check(io.read(io.ctx, out, 5) == 5 && memcmp(out, src, 5) == 0,
	      "read across chunk boundary");
	check(io.read(io.ctx, out, 5) == 2 && memcmp(out, src + 5, 2) == 0,
	      "short read at filled edge");
	check(io.read(io.ctx, out, 1) == -2, "drained window needs input");

	/* A demux call that runs dry rewinds to its mark; bytes appended
	 * before the retry stay. */
	webm_window_mark(&w);
	check(webm_window_append(&w, src + 7, 3), "append after mark");
	check(io.read(io.ctx, out, 2) == 2 && memcmp(out, src + 7, 2) == 0,
	      "read after mark");
	check(io.seek(io.ctx, 2) == 0 && io.tell(io.ctx) == 2U,
	      "seek back inside retained bytes");
	webm_window_rewind(&w);
	check(io.tell(io.ctx) == 7U && w.filled == 10U,
	      "rewind restores the cursor, keeps appended bytes");
	check(io.read(io.ctx, out, 3) == 3 && memcmp(out, src + 7, 3) == 0,
	      "read after rewind");
	check(io.seek(io.ctx, 20) == -2, "seek past fill needs input");
	check(io.seek(io.ctx, 10) == 0, "seek to filled edge");

	/* Compaction drops consumed bytes but keeps absolute offsets. */
	webm_window_compact(&w);
	check(w.start == 10U && w.filled == 0U && io.tell(io.ctx) == 10U,
	      "compact keeps absolute offsets");
	check(io.seek(io.ctx, 5) == -1, "seek before window start fails");

	/* Near full: an append that does not fit is refused whole. */
	check(webm_window_space(&w) == sizeof(store), "space after compact");
	check(webm_window_append(&w, src + 10, 31), "fill to one short");
	check(!webm_window_append(&w, src + 41, 2) && w.filled == 31U,
	      "overflowing append refused whole");
	check(webm_window_append(&w, src + 41, 1) && webm_window_space(&w) == 0U,
	      "exact fit fills the window");
	check(io.read(io.ctx, out, 30) == 30 && memcmp(out, src + 10, 30) == 0,
	      "read most of a full window");
	webm_window_compact(&w);
	check(w.start == 40U && w.filled == 2U && webm_window_space(&w) == 30U,
	      "compact frees consumed space");
	check(webm_window_append(&w, src + 42, 20), "append after compact");
	check(io.read(io.ctx, out, 22) == 22 && memcmp(out, src + 40, 22) == 0,
	      "bytes stay ordered across compaction");

	w.eof = 1;
	check(io.read(io.ctx, out, 1) == 0, "EOF read returns 0");
	check(io.seek(io.ctx, 100) == -1, "seek past EOF fails");
}

/* ---- Backend through its ops. ---- */

static const uint8_t webm_head[] = {
	/* EBML, DocType "webm" */
	0x1A, 0x45, 0xDF, 0xA3, 0x87, 0x42, 0x82, 0x84, 'w', 'e', 'b', 'm',
	/* Segment, unknown size */
	0x18, 0x53, 0x80, 0x67, 0xFF,
	/* Info: TimestampScale 1 ms */
	0x15, 0x49, 0xA9, 0x66, 0x87, 0x2A, 0xD7, 0xB1, 0x83, 0x0F, 0x42, 0x40,
	/* Tracks: one VP8 track, 160x120 */
	0x16, 0x54, 0xAE, 0x6B, 0x99,
	0xAE, 0x97,
	0xD7, 0x81, 0x01,
	0x83, 0x81, 0x01,
	0x86, 0x85, 'V', '_', 'V', 'P', '8',
	0xE0, 0x88, 0xB0, 0x82, 0x00, 0xA0, 0xBA, 0x82, 0x00, 0x78,
	/* Cluster, unknown size, timecode 0 */
	0x1F, 0x43, 0xB6, 0x75, 0xFF, 0xE7, 0x81, 0x00
};

#define BLOCK_BYTES 10U

/* SimpleBlock on track 1. Payload byte 0 is the VP8 frame tag (bit 0
 * clear = keyframe), byte 1 the test tag the stub decoder logs. */
static void put_block(uint8_t *dst, int key, uint8_t tag, int16_t rel)
{
	dst[0] = 0xA3;
	dst[1] = 0x88;
	dst[2] = 0x81;
	dst[3] = (uint8_t)((uint16_t)rel >> 8);
	dst[4] = (uint8_t)rel;
	dst[5] = key ? 0x80 : 0x00;
	dst[6] = key ? 0x10 : 0x11;
	dst[7] = tag;
	dst[8] = 0x00;
	dst[9] = 0x00;
}

struct stream {
	uint8_t bytes[4096];
	uint32_t n;
};

static void stream_head(struct stream *s)
{
	memcpy(s->bytes, webm_head, sizeof(webm_head));
	s->n = sizeof(webm_head);
}

static void stream_block(struct stream *s, int key, uint8_t tag)
{
	put_block(s->bytes + s->n, key, tag, (int16_t)(s->n & 0x3FF));
	s->n += BLOCK_BYTES;
}

static const struct SDKVideoDecoderOps *ops;

static void *open_decoder(void)
{
	struct SDKVideoMediaConfig cfg;
	void *dec = ops->create();

	memset(&cfg, 0, sizeof(cfg));
	cfg.audio_codec = SDK_VIDEO_MEDIA_AUDIO_NONE;
	check(dec && ops->configure_media(dec, &cfg), "create + configure");
	ndecoded = 0U;
	return dec;
}

static int feed(void *dec, const uint8_t *p, uint32_t n, uint32_t chunk)
{
	uint32_t off = 0U;

	while (off < n) {
		uint32_t len = n - off < chunk ? n - off : chunk;
		uint32_t accepted = 0U;

		if (ops->write(dec, p + off, len, 0, &accepted) !=
		    SDK_VIDEO_BACKEND_WRITE_OK || accepted != len)
			return 0;
		off += len;
	}
	return 1;
}

static int decode(void *dec)
{
	struct SDKVideoDecodedFrame frame;
	int rc = ops->decode(dec, &frame);

	if (rc == SDK_VIDEO_BACKEND_FRAME)
		check(frame.width == 160U && frame.height == 120U,
		      "frame geometry");
	return rc;
}

/* The header and blocks arrive in chunks smaller than any element: the
 * open must survive repeated NEED restarts, and a block split across
 * writes decodes once its last byte lands. */
static void test_chunked_feed(void)
{
	static struct stream s;
	struct SDKVideoDecoderInfo info;
	void *dec = open_decoder();
	uint32_t accepted = 0U;
	uint32_t i;
	uint8_t block[BLOCK_BYTES];

	stream_head(&s);
	stream_block(&s, 1, 1);
	stream_block(&s, 0, 2);
	check(feed(dec, s.bytes, s.n, 7U), "chunked header write");
	check(ops->get_info(dec, &info) && info.width == 160U &&
	      info.height == 120U, "header parsed from 7-byte chunks");
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME &&
	      decode(dec) == SDK_VIDEO_BACKEND_FRAME, "two frames decode");
	check(decode(dec) == SDK_VIDEO_BACKEND_NEED_INPUT, "then needs input");

	put_block(block, 0, 3, 40);
	for (i = 0U; i < BLOCK_BYTES; i += 3U) {
		uint32_t len = BLOCK_BYTES - i < 3U ? BLOCK_BYTES - i : 3U;
		int rc;

		check(feed(dec, block + i, len, len), "split block write");
		rc = decode(dec);
		if (i + len < BLOCK_BYTES)
			check(rc == SDK_VIDEO_BACKEND_NEED_INPUT,
			      "partial block needs input");
		else
			check(rc == SDK_VIDEO_BACKEND_FRAME,
			      "completed block decodes");
	}
	check(ndecoded == 3U && decoded[0] == 1U && decoded[1] == 2U &&
	      decoded[2] == 3U, "frames decode once, in order");
	check(ops->write(dec, 0, 0, 1, &accepted) == SDK_VIDEO_BACKEND_WRITE_OK,
	      "EOF write");
	check(decode(dec) == SDK_VIDEO_BACKEND_DONE, "done at EOF");
	ops->destroy(dec);
}

static void test_skip_to_keyframe(void)
{
	static struct stream s;
	void *dec = open_decoder();

	stream_head(&s);
	stream_block(&s, 0, 1);
	stream_block(&s, 0, 2);
	stream_block(&s, 1, 3);
	stream_block(&s, 0, 4);
	check(feed(dec, s.bytes, s.n, s.n), "skip stream write");
	ops->set_decode_flags(dec, 1U);
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME, "skip lands on keyframe");
	check(ndecoded == 1U && decoded[0] == 3U,
	      "non-key frames never reach the decoder");
	ops->set_decode_flags(dec, 0U);
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME && ndecoded == 2U &&
	      decoded[1] == 4U, "decoding resumes after the keyframe");
	ops->destroy(dec);
}

static void test_skip_without_keyframe(void)
{
	static struct stream s;
	uint8_t block[BLOCK_BYTES];
	void *dec = open_decoder();

	stream_head(&s);
	stream_block(&s, 0, 1);
	stream_block(&s, 0, 2);
	check(feed(dec, s.bytes, s.n, s.n), "keyless stream write");
	ops->set_decode_flags(dec, 1U);
	check(decode(dec) == SDK_VIDEO_BACKEND_NEED_INPUT,
	      "no keyframe yet: needs input");
	check(decode(dec) == SDK_VIDEO_BACKEND_NEED_INPUT,
	      "still needs input, not failed");
	check(ndecoded == 0U, "nothing decoded while skipping");
	put_block(block, 1, 3, 90);
	check(feed(dec, block, sizeof(block), sizeof(block)),
	      "keyframe arrives later");
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME && ndecoded == 1U &&
	      decoded[0] == 3U, "late keyframe decodes");
	ops->destroy(dec);
}

static void test_skip_budget(void)
{
	static struct stream s;
	void *dec = open_decoder();
	unsigned i;

	stream_head(&s);
	for (i = 0U; i < 130U; i++)
		stream_block(&s, 0, (uint8_t)(i & 0x7FU));
	stream_block(&s, 1, 0x55);
	check(feed(dec, s.bytes, s.n, s.n), "long GOP write");
	ops->set_decode_flags(dec, 1U);
	check(decode(dec) == SDK_VIDEO_BACKEND_PROGRESS,
	      "a long skip yields PROGRESS");
	check(ndecoded == 0U, "yield decodes nothing");
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME && ndecoded == 1U &&
	      decoded[0] == 0x55, "next call reaches the keyframe");
	ops->destroy(dec);
}

/* A frame that decodes without a picture keeps the call pulling. */
static void test_invisible_frame(void)
{
	static struct stream s;
	void *dec = open_decoder();

	stream_head(&s);
	stream_block(&s, 1, 0x81);
	stream_block(&s, 0, 2);
	check(feed(dec, s.bytes, s.n, s.n), "invisible stream write");
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME && ndecoded == 2U &&
	      decoded[0] == 0x81 && decoded[1] == 2U,
	      "invisible frame is decoded, next one shown");
	ops->destroy(dec);
}

int main(void)
{
	ops = sdk_video_webm_ops(SDK_VIDEO_CODEC_VP8);
	check(ops != 0, "VP8 ops");
	check(ops && ops->geometry_ok && ops->geometry_ok(1920U, 1088U) &&
	      !ops->geometry_ok(1921U, 1080U), "backend owns WebM geometry");
	test_window();
	if (ops) {
		test_chunked_feed();
		test_skip_to_keyframe();
		test_skip_without_keyframe();
		test_skip_budget();
		test_invisible_frame();
	}
	if (failures) {
		fprintf(stderr, "webm_backend_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("webm_backend_test: all checks passed\n");
	return 0;
}
