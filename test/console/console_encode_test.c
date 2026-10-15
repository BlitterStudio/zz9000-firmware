/*
 * Host tests for the console encoder service (SDK service 0x8200): request
 * parsing, request checks against the surface, the session and its
 * keyframes, and the RGB565 snapshot of a 32-bit screen.
 *
 * The encoder runs with no optional codes (enc_flags 0), so every changed
 * tile is a TILE8 op with a RAW plane and the stream can be walked here.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_console_encode.h"
#include "sdk_mailbox.h"
#include "zz9k/abi.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The firmware's request/reply offsets and constants are the SDK's
 * (sdk/include/zz9k/abi.h).  check-abi-mirror.sh compares the service,
 * opcode and capability; this compares the payload layout. */
#define SAME_OFFSET(f, off) (offsetof(ZZ9KConsoleEncodePayload, f) == (off))
typedef char console_request_layout_mirrored[
	(SAME_OFFSET(surface_handle, SDK_CONSOLE_REQ_SURFACE) &&
	 SAME_OFFSET(out_handle, SDK_CONSOLE_REQ_OUT_HANDLE) &&
	 SAME_OFFSET(out_capacity, SDK_CONSOLE_REQ_OUT_CAPACITY) &&
	 SAME_OFFSET(enc_flags, SDK_CONSOLE_REQ_ENC_FLAGS) &&
	 SAME_OFFSET(width, SDK_CONSOLE_REQ_WIDTH) &&
	 SAME_OFFSET(height, SDK_CONSOLE_REQ_HEIGHT) &&
	 SAME_OFFSET(bytes_per_row, SDK_CONSOLE_REQ_BYTES_PER_ROW) &&
	 SAME_OFFSET(ty0, SDK_CONSOLE_REQ_TY0) &&
	 SAME_OFFSET(ty1, SDK_CONSOLE_REQ_TY1) &&
	 SAME_OFFSET(flags, SDK_CONSOLE_REQ_FLAGS) &&
	 SAME_OFFSET(codec, SDK_CONSOLE_REQ_CODEC) &&
	 SAME_OFFSET(depth, SDK_CONSOLE_REQ_DEPTH) &&
	 SAME_OFFSET(tile_w, SDK_CONSOLE_REQ_TILE_W) &&
	 SAME_OFFSET(tile_h, SDK_CONSOLE_REQ_TILE_H) &&
	 SAME_OFFSET(format, SDK_CONSOLE_REQ_FORMAT) &&
	 SAME_OFFSET(reserved, SDK_CONSOLE_REQ_LEN)) ? 1 : -1];
typedef char console_reply_layout_mirrored[
	(offsetof(ZZ9KConsoleEncodeResultPayload, out_len) ==
	     SDK_CONSOLE_REPLY_OUT_LEN &&
	 offsetof(ZZ9KConsoleEncodeResultPayload, codec) ==
	     SDK_CONSOLE_REPLY_CODEC &&
	 offsetof(ZZ9KConsoleEncodeResultPayload, flags) ==
	     SDK_CONSOLE_REPLY_FLAGS &&
	 offsetof(ZZ9KConsoleEncodeResultPayload, reserved) ==
	     SDK_CONSOLE_REPLY_LEN) ? 1 : -1];
typedef char console_constants_mirrored[
	(ZZ9K_CONSOLE_ENCODE_RESET == SDK_CONSOLE_F_RESET &&
	 ZZ9K_CONSOLE_ENCODE_RESULT_KEYFRAME == SDK_CONSOLE_RF_KEYFRAME &&
	 ZZ9K_CONSOLE_CODEC_NONE == SDK_CONSOLE_CODEC_NONE &&
	 ZZ9K_OP_CONSOLE_ENCODE == SDK_OP_CONSOLE_ENCODE) ? 1 : -1];

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define OUT_CAP (512u * 1024u)
static uint8_t out_buf[OUT_CAP];

struct walk {
	int ok;
	uint32_t tiles;
	uint32_t first_index;
	uint32_t last_index;
	int nonzero;            /* a tile carried a nonzero byte */
};

/* Walk an uncompressed stream of TILE8/RAW ops; tile_bytes is the size of
 * every tile (the tests use geometries that are whole tiles). */
static struct walk walk_stream(const uint8_t *p, uint32_t n,
                               uint32_t tile_bytes)
{
	struct walk w;
	uint32_t o = 4u;

	memset(&w, 0, sizeof(w));
	if (n < 5u || p[0] != RFB_VERSION)
		return w;
	while (o < n) {
		uint8_t op = p[o++];
		uint32_t i;

		if (op == RFB_OP_END) {
			w.ok = (o == n);
			return w;
		}
		if (op != RFB_OP_TILE8 || o + 3u + tile_bytes > n)
			return w;
		w.last_index = ((uint32_t)p[o] << 8) | p[o + 1];
		if (w.tiles == 0u)
			w.first_index = w.last_index;
		o += 2u;
		if (p[o++] != RFB_CODE_RAW)
			return w;
		for (i = 0u; i < tile_bytes; i++)
			if (p[o + i] != 0u)
				w.nonzero = 1;
		o += tile_bytes;
		w.tiles++;
	}
	return w;
}

static void make_request(struct sdk_console_request *r, uint32_t out_handle,
                         uint16_t width, uint16_t height, uint16_t bpr,
                         uint8_t format, uint8_t tile_w, uint8_t tile_h)
{
	memset(r, 0, sizeof(*r));
	r->surface_handle = 0x40000001u;
	r->out_handle = out_handle;
	r->out_capacity = OUT_CAP;
	r->enc_flags = 0u;
	r->width = width;
	r->height = height;
	r->bytes_per_row = bpr;
	r->ty0 = 0u;
	r->ty1 = (uint16_t)((height + tile_h - 1u) / tile_h);
	r->format = format;
	r->depth = (uint8_t)(format == RFB_FMT_RGB565 ? 16u : 8u);
	r->tile_w = tile_w;
	r->tile_h = tile_h;
}

static void make_surface(struct sdk_console_surface *s, uint32_t format,
                         uint32_t width, uint32_t height, uint32_t pitch,
                         const uint8_t *pixels)
{
	s->format = format;
	s->width = width;
	s->height = height;
	s->pitch = pitch;
	s->length = pitch * height;
	s->pixels = pixels;
}

static uint16_t encode(const struct sdk_console_request *r,
                       const struct sdk_console_surface *s,
                       uint32_t *n, uint16_t *rflags)
{
	memset(out_buf, 0xEE, sizeof(out_buf));
	return sdk_console_encode_band(r, s, out_buf, r->out_capacity, n,
	                               rflags);
}

static void test_parse_request(void)
{
	static const uint8_t p[SDK_CONSOLE_REQ_LEN] = {
		0x40, 0x00, 0x00, 0x07,  0x00, 0x00, 0x00, 0x09,
		0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x5b,
		0x03, 0x20,  0x02, 0x58,  0x06, 0x40,  0x00, 0x00,
		0x00, 0x26,  0x00, 0x01,  0x00, 0x00,
		16, 32, 16, RFB_FMT_RGB565
	};
	struct sdk_console_request r;

	sdk_console_parse_request(p, &r);
	CHECK(r.surface_handle == 0x40000007u);
	CHECK(r.out_handle == 9u);
	CHECK(r.out_capacity == 0x10000u);
	CHECK(r.enc_flags == 0x5bu);
	CHECK(r.width == 800u && r.height == 600u);
	CHECK(r.bytes_per_row == 1600u);
	CHECK(r.ty0 == 0u && r.ty1 == 38u);
	CHECK(r.flags == SDK_CONSOLE_F_RESET);
	CHECK(r.codec == SDK_CONSOLE_CODEC_NONE);
	CHECK(r.depth == 16u && r.tile_w == 32u && r.tile_h == 16u);
	CHECK(r.format == RFB_FMT_RGB565);
}

/* A new session's band 0 is a full frame flagged KEYFRAME; an unchanged
 * screen then codes as no tiles; another output buffer is another client
 * and gets a keyframe of its own. */
static void test_session_keyframes(void)
{
	static uint8_t px[64 * 32];
	struct sdk_console_request r;
	struct sdk_console_surface s;
	struct walk w;
	uint32_t n;
	uint16_t rf;

	memset(px, 0x5a, sizeof(px));
	make_surface(&s, SDK_SURFACE_FORMAT_INDEX8, 64, 32, 64, px);
	make_request(&r, 0x100u, 64, 32, 64, RFB_FMT_CLUT8, 16, 16);
	r.flags = SDK_CONSOLE_F_RESET;

	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	CHECK(rf & SDK_CONSOLE_RF_KEYFRAME);
	w = walk_stream(out_buf, n, 16u * 16u);
	CHECK(w.ok && w.tiles == 8u);

	r.flags = 0u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	CHECK(!(rf & SDK_CONSOLE_RF_KEYFRAME));
	w = walk_stream(out_buf, n, 16u * 16u);
	CHECK(w.ok && w.tiles == 0u);

	r.out_handle = 0x101u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	CHECK(rf & SDK_CONSOLE_RF_KEYFRAME);
	w = walk_stream(out_buf, n, 16u * 16u);
	CHECK(w.ok && w.tiles == 8u);
}

/* The request's geometry must be the surface's. */
static void test_geometry_must_match(void)
{
	static uint8_t px[64 * 32];
	struct sdk_console_request r;
	struct sdk_console_surface s;
	uint32_t n;
	uint16_t rf;

	make_surface(&s, SDK_SURFACE_FORMAT_INDEX8, 64, 32, 64, px);
	make_request(&r, 0x100u, 64, 32, 64, RFB_FMT_CLUT8, 16, 16);

	r.width = 63u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
	r.width = 64u;
	r.bytes_per_row = 128u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
	r.bytes_per_row = 64u;
	r.format = RFB_FMT_PLANAR;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
	r.format = RFB_FMT_RGB565;
	r.depth = 16u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
}

static void test_rgb565_surface(void)
{
	static uint8_t px[32 * 2 * 16];
	struct sdk_console_request r;
	struct sdk_console_surface s;
	struct walk w;
	uint32_t n;
	uint16_t rf;

	memset(px, 0x12, sizeof(px));
	make_surface(&s, SDK_SURFACE_FORMAT_RGB565, 32, 16, 64, px);
	make_request(&r, 0x200u, 32, 16, 64, RFB_FMT_RGB565, 16, 16);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	w = walk_stream(out_buf, n, 16u * 16u);
	CHECK(w.ok && w.tiles == 4u);
}

/* A 15-bit screen is two bytes a pixel like RGB565, but its bytes are not
 * RGB565: refused, so the client falls back instead of showing wrong
 * colours. */
static void test_rgb555_refused(void)
{
	static uint8_t px[32 * 2 * 16];
	struct sdk_console_request r;
	struct sdk_console_surface s;
	uint32_t n;
	uint16_t rf;

	memset(px, 0x7c, sizeof(px));
	make_surface(&s, SDK_SURFACE_FORMAT_RGB555, 32, 16, 64, px);
	make_request(&r, 0x200u, 32, 16, 64, RFB_FMT_RGB565, 16, 16);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
	CHECK(n == 0u);

	/* Nor is any other two-byte surface, or a 16-bit one as CLUT8. */
	s.format = SDK_SURFACE_FORMAT_UNKNOWN;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
	s.format = SDK_SURFACE_FORMAT_RGB565;
	make_request(&r, 0x200u, 64, 16, 64, RFB_FMT_CLUT8, 16, 16);
	s.width = 64u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);
}

static void test_bgra_conversion(void)
{
	static const uint8_t bgra[8] = {
		0x00, 0x00, 0xff, 0x80,   /* red   */
		0xff, 0x00, 0x00, 0x80    /* blue  */
	};
	uint8_t dst[4];

	sdk_console_snap_bgra_rgb565(dst, 4, bgra, 8, 2, 1);
	CHECK(dst[0] == 0xf8 && dst[1] == 0x00);
	CHECK(dst[2] == 0x00 && dst[3] == 0x1f);
}

/* The converter writes width * 2 bytes of each host row; the rest of the
 * row, up to the host's pitch, must be zero, not what the buffer held. */
static void test_bgra_padding_zeroed(void)
{
	static const uint8_t bgra[3 * 4 * 2] = {
		0xff, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0,
		0xff, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0
	};
	uint8_t dst[2 * 8];
	uint32_t i;

	memset(dst, 0xaa, sizeof(dst));
	sdk_console_snap_bgra_rgb565(dst, 8, bgra, 12, 3, 2);
	for (i = 0u; i < 6u; i++) {
		CHECK(dst[i] == 0xff);
		CHECK(dst[8u + i] == 0xff);
	}
	CHECK(dst[6] == 0 && dst[7] == 0);
	CHECK(dst[14] == 0 && dst[15] == 0);
}

/* End to end: a CLUT8 session leaves 0xaa in the snapshot; a black 32-bit
 * screen then asked as RGB565 with padded rows (bpr > width * 2) reuses
 * that buffer.  Its keyframe against a zeroed shadow must be empty: a
 * nonzero byte in it is the old frame leaking through the padding. */
static void test_bgra_padding_not_sent(void)
{
	static uint8_t clut[64 * 16];
	static uint8_t black[30 * 4 * 16];
	struct sdk_console_request r;
	struct sdk_console_surface s;
	struct walk w;
	uint32_t n;
	uint16_t rf;

	memset(clut, 0xaa, sizeof(clut));
	make_surface(&s, SDK_SURFACE_FORMAT_INDEX8, 64, 16, 64, clut);
	make_request(&r, 0x300u, 64, 16, 64, RFB_FMT_CLUT8, 16, 16);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);

	memset(black, 0, sizeof(black));
	make_surface(&s, SDK_SURFACE_FORMAT_BGRA8888, 30, 16, 30 * 4, black);
	make_request(&r, 0x300u, 30, 16, 64, RFB_FMT_RGB565, 16, 16);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	CHECK(rf & SDK_CONSOLE_RF_KEYFRAME);
	w = walk_stream(out_buf, n, 16u * 16u);
	CHECK(w.ok);
	CHECK(w.tiles == 0u && !w.nonzero);
}

/* The wire's tile index is 16 bits.  65536 tiles fit (0..65535); one row
 * of tiles more is refused by the encoder and by the service, where it was
 * accepted and tile 65536 went out as tile 0. */
static void test_tile_index_space(void)
{
	static uint8_t px[320 * 256];
	rfb_geom g;
	rfb_encoder e;
	struct sdk_console_request r;
	struct sdk_console_surface s;
	uint32_t n;
	uint16_t rf;

	memset(&g, 0, sizeof(g));
	g.format = RFB_FMT_CLUT8;
	g.depth = 8u;
	g.tile_w = 1u;
	g.tile_h = 1u;
	g.width = 256u;
	g.bytes_per_row = 256u;
	g.height = 256u;
	CHECK(rfb_shadow_size(&g) == 256u * 256u);
	g.height = 257u;
	CHECK(rfb_shadow_size(&g) == 0u);
	CHECK(rfb_scratch_size(&g, 0u, NULL) == 0u);
	CHECK(rfb_worst_case_frame(&g) == 0u);
	{
		static uint8_t sh[256 * 257];
		static uint8_t sc[4096];
		CHECK(rfb_encoder_init(&e, &g, 0u, NULL, sh, sizeof(sh),
		                       sc, sizeof(sc)) == RFB_E_GEOM);
	}

	/* The reviewer's case: 320x256 CLUT8, 1x1 tiles, 81920 tiles. */
	memset(px, 1, sizeof(px));
	make_surface(&s, SDK_SURFACE_FORMAT_INDEX8, 320, 256, 320, px);
	make_request(&r, 0x400u, 320, 256, 320, RFB_FMT_CLUT8, 1, 1);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_BAD_REQUEST);

	/* 256x256 at 1x1 is exactly 65536 tiles: served, and the last tile
	 * goes out as 65535. */
	make_surface(&s, SDK_SURFACE_FORMAT_INDEX8, 256, 256, 256, px);
	make_request(&r, 0x400u, 256, 256, 256, RFB_FMT_CLUT8, 1, 1);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	{
		struct walk w = walk_stream(out_buf, n, 1u);
		CHECK(w.ok && w.tiles == 65536u);
		CHECK(w.first_index == 0u && w.last_index == 65535u);
	}
}

/* A warm reset restarts shared-buffer handles at 1, so a rebooted client
 * can come back with the old output handle and the old geometry and no
 * RESET flag.  After sdk_console_encode_reset() its band 0 must be a
 * keyframe coded against a zero baseline, with the sequence restarted. */
static void test_reset_forgets_session(void)
{
	static uint8_t px[64 * 32];
	struct sdk_console_request r;
	struct sdk_console_surface s;
	struct walk w;
	uint32_t n;
	uint16_t rf;

	memset(px, 0x33, sizeof(px));
	make_surface(&s, SDK_SURFACE_FORMAT_INDEX8, 64, 32, 64, px);
	make_request(&r, 0x1u, 64, 32, 64, RFB_FMT_CLUT8, 16, 16);
	r.flags = SDK_CONSOLE_F_RESET;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	r.flags = 0u;
	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	CHECK(!(rf & SDK_CONSOLE_RF_KEYFRAME));
	CHECK(out_buf[2] != 0u || out_buf[3] != 0u);    /* seq advanced */

	sdk_console_encode_reset();

	CHECK(encode(&r, &s, &n, &rf) == SDK_STATUS_OK);
	CHECK(rf & SDK_CONSOLE_RF_KEYFRAME);
	CHECK(out_buf[2] == 0u && out_buf[3] == 0u);    /* seq 0 */
	w = walk_stream(out_buf, n, 16u * 16u);
	CHECK(w.ok && w.tiles == 8u && w.nonzero);
}

/* sdk_mailbox_init() is the mailbox reset; it must call the reset above.
 * The function needs the board, so its source is checked instead. */
static void test_mailbox_init_resets(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *src;
	const char *body;
	const char *end;
	const char *call;
	long len;

	CHECK(f != NULL);
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	src = (char *)malloc((size_t)len + 1u);
	CHECK(src != NULL && fread(src, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	if (!src)
		return;
	src[len] = 0;
	body = strstr(src, "\nvoid sdk_mailbox_init(void)\n{");
	CHECK(body != NULL);
	if (body) {
		end = strstr(body, "\n}\n");
		call = strstr(body, "\tsdk_console_encode_reset();");
		CHECK(end != NULL && call != NULL && call < end);
	}
	free(src);
}

int main(int argc, char **argv)
{
	test_parse_request();
	test_session_keyframes();
	test_geometry_must_match();
	test_rgb565_surface();
	test_rgb555_refused();
	test_bgra_conversion();
	test_bgra_padding_zeroed();
	test_bgra_padding_not_sent();
	test_tile_index_space();
	test_reset_forgets_session();
	if (argc > 1)
		test_mailbox_init_resets(argv[1]);
	else
		CHECK(!"usage: console_encode_test path/to/sdk_mailbox.c");

	if (failures) {
		printf("console_encode_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("console_encode_test: OK\n");
	return 0;
}
