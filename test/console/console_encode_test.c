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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define OUT_CAP (256u * 1024u)
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

int main(void)
{
	test_parse_request();
	test_session_keyframes();
	test_geometry_must_match();
	test_rgb565_surface();
	test_rgb555_refused();
	test_bgra_conversion();

	if (failures) {
		printf("console_encode_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("console_encode_test: OK\n");
	return 0;
}
