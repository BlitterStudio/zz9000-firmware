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

/* Opus stubs: every packet is 20 ms of stereo silence at 48 kHz. */
#define OPUS_PACKET_SAMPLES 960

static char opus_state;

OpusDecoder *opus_decoder_create(opus_int32 fs, int channels, int *error)
{
	(void)fs;
	(void)channels;
	*error = OPUS_OK;
	return (OpusDecoder *)(void *)&opus_state;
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
	(void)decode_fec;
	if (frame_size < OPUS_PACKET_SAMPLES)
		return -1;
	memset(pcm, 0, OPUS_PACKET_SAMPLES * 2U * sizeof(*pcm));
	return OPUS_PACKET_SAMPLES;
}

int opus_packet_get_nb_samples(const unsigned char *packet, opus_int32 len,
			       opus_int32 fs)
{
	(void)packet;
	(void)len;
	(void)fs;
	return OPUS_PACKET_SAMPLES;
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
	/* Retrying from the mark (7) with 3 bytes held: more input can help. */
	check(!webm_window_exhausted(&w, 7U), "partial window not exhausted");
	{
		struct webm_window full;
		uint8_t fstore[8];

		/* A retry from offset 0 that already holds a whole window
		 * and still needs input can never complete. */
		webm_window_init(&full, fstore, sizeof(fstore));
		check(webm_window_append(&full, src, 8), "fill small window");
		check(webm_window_exhausted(&full, 0U),
		      "full window from the retry origin is exhausted");
		check(!webm_window_exhausted(&full, 2U),
		      "compaction past the origin can still make room");
		full.eof = 1;
		check(!webm_window_exhausted(&full, 0U),
		      "at EOF the demux ends instead of waiting");
	}
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

/* A Void inside the Cluster larger than the whole input window: the
 * client fills the window, the demux still needs the rest of the Void,
 * and nothing can ever free room. The stream must fail as unsupported
 * instead of answering BUSY/NEED_INPUT forever. */
static void test_window_exhausted_by_skip(void)
{
	static struct stream s;
	static uint8_t zeros[64U * 1024U];
	/* Void, 8-byte size 16 MiB. */
	static const uint8_t void_head[] = {
		0xEC, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	void *dec = open_decoder();
	uint32_t fed = 0U;
	int rc = SDK_VIDEO_BACKEND_NEED_INPUT;

	stream_head(&s);
	stream_block(&s, 1, 1);
	check(feed(dec, s.bytes, s.n, s.n), "head + keyframe write");
	check(decode(dec) == SDK_VIDEO_BACKEND_FRAME, "keyframe decodes");
	check(feed(dec, void_head, sizeof(void_head), sizeof(void_head)),
	      "huge Void header write");
	for (;;) {
		uint32_t accepted = 0U;
		int wr = ops->write(dec, zeros, sizeof(zeros), 0, &accepted);

		rc = decode(dec);
		if (rc != SDK_VIDEO_BACKEND_NEED_INPUT)
			break;
		if (wr != SDK_VIDEO_BACKEND_WRITE_OK)
			break;
		fed += accepted;
		if (fed > 32U * 1024U * 1024U)
			break;
	}
	check(rc == SDK_VIDEO_BACKEND_UNSUPPORTED,
	      "skip larger than the window fails instead of waiting");
	ops->destroy(dec);
}

/* A Void between the Segment start and Tracks larger than the window: the
 * header can never parse, so the write that fills the window must refuse
 * the stream rather than keep asking for input. */
static void test_window_exhausted_in_header(void)
{
	static uint8_t zeros[64U * 1024U];
	/* EBML "webm", Segment of unknown size, Void with 8-byte size 16 MiB. */
	static const uint8_t head[] = {
		0x1A, 0x45, 0xDF, 0xA3, 0x87, 0x42, 0x82, 0x84,
		'w', 'e', 'b', 'm',
		0x18, 0x53, 0x80, 0x67, 0xFF,
		0xEC, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	void *dec = open_decoder();
	uint32_t fed = 0U;
	int wr;

	check(feed(dec, head, sizeof(head), sizeof(head)),
	      "header with huge Void write");
	for (;;) {
		uint32_t accepted = 0U;

		wr = ops->write(dec, zeros, sizeof(zeros), 0, &accepted);
		if (wr != SDK_VIDEO_BACKEND_WRITE_OK)
			break;
		fed += accepted;
		if (fed > 32U * 1024U * 1024U)
			break;
	}
	check(wr == SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED,
	      "header larger than the window fails instead of waiting");
	ops->destroy(dec);
}

/* A final write larger than the space left is taken in part. The end of
 * stream must wait for the write that delivers the last byte, or the
 * demux would see a truncated stream and stop early. */
static void test_partial_final_write(void)
{
	static struct stream s;
	/* More blocks than the 4 MiB + 64 KiB window holds. */
	const uint32_t blocks = 480U * 1024U;
	const uint32_t len = blocks * BLOCK_BYTES;
	uint8_t *tail = (uint8_t *)malloc(len);
	void *dec = open_decoder();
	uint32_t off = 0U;
	uint32_t frames = 0U;
	uint32_t accepted = 0U;
	uint32_t i;
	int rc;

	check(tail != 0, "tail allocation");
	if (!tail) {
		ops->destroy(dec);
		return;
	}
	for (i = 0U; i < blocks; i++)
		put_block(tail + i * BLOCK_BYTES, i == 0U, (uint8_t)(i & 0x7FU),
			  (int16_t)(i & 0x3FFU));
	stream_head(&s);
	check(feed(dec, s.bytes, s.n, s.n), "head write");
	check(ops->write(dec, tail, len, 1, &accepted) ==
		      SDK_VIDEO_BACKEND_WRITE_OK && accepted != 0U &&
	      accepted < len, "oversized final write is taken in part");
	off = accepted;
	while ((rc = decode(dec)) == SDK_VIDEO_BACKEND_FRAME)
		frames++;
	check(rc == SDK_VIDEO_BACKEND_NEED_INPUT,
	      "a partly taken final write is not the end of stream");
	while (off < len) {
		accepted = 0U;
		if (ops->write(dec, tail + off, len - off, 1, &accepted) !=
		    SDK_VIDEO_BACKEND_WRITE_OK)
			break;
		off += accepted;
		while ((rc = decode(dec)) == SDK_VIDEO_BACKEND_FRAME)
			frames++;
	}
	check(off == len, "remainder delivered");
	check(rc == SDK_VIDEO_BACKEND_DONE, "stream ends after the last byte");
	check(frames == blocks, "every block decoded");
	ops->destroy(dec);
	free(tail);
}

/* The same header with an Opus track 2 (stereo, no pre-skip). */
static const uint8_t webm_av_head[] = {
	0x1A, 0x45, 0xDF, 0xA3, 0x87, 0x42, 0x82, 0x84, 'w', 'e', 'b', 'm',
	0x18, 0x53, 0x80, 0x67, 0xFF,
	0x15, 0x49, 0xA9, 0x66, 0x87, 0x2A, 0xD7, 0xB1, 0x83, 0x0F, 0x42, 0x40,
	0x16, 0x54, 0xAE, 0x6B, 0xCA,
	0xAE, 0x97,
	0xD7, 0x81, 0x01,
	0x83, 0x81, 0x01,
	0x86, 0x85, 'V', '_', 'V', 'P', '8',
	0xE0, 0x88, 0xB0, 0x82, 0x00, 0xA0, 0xBA, 0x82, 0x00, 0x78,
	0xAE, 0xAF,
	0xD7, 0x81, 0x02,
	0x83, 0x81, 0x02,
	0x86, 0x86, 'A', '_', 'O', 'P', 'U', 'S',
	0x63, 0xA2, 0x93, 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2,
	0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xE1, 0x89, 0x9F, 0x81, 0x02, 0xB5, 0x84, 0x47, 0x3B, 0x80, 0x00,
	0x1F, 0x43, 0xB6, 0x75, 0xFF, 0xE7, 0x81, 0x00
};

static void stream_av_block(struct stream *s, uint8_t track, int key,
			    uint8_t tag, int16_t ms)
{
	put_block(s->bytes + s->n, key, tag, ms);
	s->bytes[s->n + 2U] = (uint8_t)(0x80U | track);
	if (track != 1U)
		s->bytes[s->n + 5U] = 0x80;
	s->n += BLOCK_BYTES;
}

/* Muxers interleave WebM by timestamp, so each Opus packet sits just after
 * the picture it plays with. The host shows a picture when the audio clock
 * reaches it and only then asks for the next one, so the backend must
 * demux audio past the queued pictures up to the PCM low-water mark; if it
 * stopped at each video block, the audio queued when a picture is shown
 * would be one frame period at most and playback would starve. */
static void test_audio_demuxed_ahead_of_video(void)
{
	enum { FRAMES = 25, PACKETS = 50, MS_BYTES = 192 };
	static struct stream s;
	static uint8_t ring[128U * 1024U];
	const uint32_t total = PACKETS * 20U * MS_BYTES;
	struct SDKVideoMediaConfig cfg;
	struct SDKVideoMediaInfo mi;
	void *dec = ops->create();
	uint32_t accepted = 0U;
	uint32_t frames = 0U;
	uint32_t i;
	int rc = SDK_VIDEO_BACKEND_ERROR;
	int ahead = 1;
	int ordered = 1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.audio_codec = SDK_VIDEO_MEDIA_AUDIO_OPUS;
	cfg.pcm_ring = ring;
	cfg.pcm_ring_capacity = sizeof(ring);
	cfg.pcm_low_water_bytes = 200U * MS_BYTES;
	cfg.pcm_high_water_bytes = 96U * 1024U;
	check(dec && ops->configure_media(dec, &cfg), "A/V create + configure");
	ndecoded = 0U;
	memcpy(s.bytes, webm_av_head, sizeof(webm_av_head));
	s.n = sizeof(webm_av_head);
	for (i = 0U; i < FRAMES; i++) {
		stream_av_block(&s, 1U, i == 0U, (uint8_t)i, (int16_t)(40U * i));
		stream_av_block(&s, 2U, 1, 0, (int16_t)(40U * i));
		stream_av_block(&s, 2U, 1, 0, (int16_t)(40U * i + 20U));
	}
	check(feed(dec, s.bytes, s.n, s.n) &&
	      ops->write(dec, 0, 0, 1, &accepted) ==
		      SDK_VIDEO_BACKEND_WRITE_OK, "A/V stream write");
	for (i = 0U; i < 4U * FRAMES; i++) {
		uint64_t played = (uint64_t)frames * 40U * MS_BYTES;
		uint64_t want = played + cfg.pcm_low_water_bytes;

		/* The audio clock has reached the next picture's time. */
		if (ops->get_media_info(dec, &mi))
			(void)ops->ack_media(dec, mi.pcm_produced < played
					     ? mi.pcm_produced : played);
		rc = decode(dec);
		if (rc == SDK_VIDEO_BACKEND_DONE)
			break;
		if (rc == SDK_VIDEO_BACKEND_PROGRESS ||
		    rc == SDK_VIDEO_BACKEND_BACKPRESSURE)
			continue;
		if (rc != SDK_VIDEO_BACKEND_FRAME)
			break;
		if (want > total)
			want = total;
		if (!ops->get_media_info(dec, &mi) || mi.pcm_produced < want) {
			if (ahead)
				fprintf(stderr, "frame %u: audio %llu of %llu\n",
					frames, (unsigned long long)mi.pcm_produced,
					(unsigned long long)want);
			ahead = 0;
		}
		if (ndecoded != frames + 1U || decoded[frames] != frames)
			ordered = 0;
		frames++;
	}
	check(ahead, "audio is decoded to the low-water mark past each picture");
	check(ordered, "pictures decode once, in order");
	check(rc == SDK_VIDEO_BACKEND_DONE && frames == FRAMES,
	      "every picture decodes, then the stream is done");
	check(ops->get_media_info(dec, &mi) && mi.pcm_produced == total,
	      "all the audio decodes");
	ops->destroy(dec);
}

int main(void)
{
	ops = sdk_video_webm_ops(SDK_VIDEO_CODEC_VP8);
	check(ops != 0, "VP8 ops");
	check(ops && ops->geometry_ok && ops->geometry_ok(1920U, 1088U) &&
	      !ops->geometry_ok(1921U, 1080U), "backend owns WebM geometry");
	{
		const struct SDKVideoDecoderOps *vp9 =
			sdk_video_webm_ops(SDK_VIDEO_CODEC_VP9);

		/* Portrait phone sizes must reach the VP9 backend too. */
		check(vp9 && vp9->geometry_ok &&
		      vp9->geometry_ok(1080U, 1920U) &&
		      !vp9->geometry_ok(1088U, 1921U),
		      "VP9 backend owns WebM geometry");
	}
	{
		const struct SDKVideoDecoderOps *vp9 =
			sdk_video_webm_ops(SDK_VIDEO_CODEC_VP9);
		int i;

		for (i = 0; i < 2; i++) {
			const struct SDKVideoDecoderOps *o = i ? vp9 : ops;

			check(o && o->audio_ok &&
			      o->audio_ok(SDK_VIDEO_MEDIA_AUDIO_NONE) &&
			      o->audio_ok(SDK_VIDEO_MEDIA_AUDIO_OPUS) &&
			      o->audio_ok(SDK_VIDEO_MEDIA_AUDIO_VORBIS) &&
			      !o->audio_ok(SDK_VIDEO_MEDIA_AUDIO_MP2) &&
			      !o->audio_ok(SDK_VIDEO_MEDIA_AUDIO_AC3),
			      "WebM carries Opus, Vorbis or no audio");
		}
	}
	test_window();
	if (ops) {
		test_chunked_feed();
		test_skip_to_keyframe();
		test_skip_without_keyframe();
		test_skip_budget();
		test_invisible_frame();
		test_window_exhausted_by_skip();
		test_window_exhausted_in_header();
		test_partial_final_write();
		test_audio_demuxed_ahead_of_video();
	}
	if (failures) {
		fprintf(stderr, "webm_backend_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("webm_backend_test: all checks passed\n");
	return 0;
}
