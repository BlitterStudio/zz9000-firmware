/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpeg1_ps_fixture.inc"
#include "mpeg1_crop_ps_fixture.inc"

/* All 6 pictures of the fixture, decoded by libmpeg2; every sample is
 * within 1 of ffmpeg's decode of the same stream. */
#define EXPECTED_FRAMES 6U
#define EXPECTED_FNV64 UINT64_C(0x9db20b102d9ff58c)

static uint64_t fnv1a64(uint64_t hash, const uint8_t *bytes, uint32_t length)
{
	uint32_t i;

	for (i = 0U; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int test_yuy2_conversion(void)
{
	static const uint8_t y[8] = {1, 2, 3, 0, 4, 5, 6, 0};
	static const uint8_t cb[2] = {10, 11};
	static const uint8_t cr[2] = {20, 21};
	static const uint8_t expected[20] = {
		1, 10, 2, 20, 3, 11, 3, 21, 0xaa, 0xaa,
		4, 10, 5, 20, 6, 11, 6, 21, 0xaa, 0xaa,
	};
	uint8_t dst[20];
	uint32_t bytes_written = 0U;

	memset(dst, 0xaa, sizeof(dst));
	if (sdk_video_yuy2_row_bytes(3U) != 8U)
		return 1;
	if (!sdk_video_yuv420_to_yuy2(dst, 10U, 3U, 2U, y, 4U,
	                              cb, cr, 2U, &bytes_written))
		return 2;
	if (bytes_written != 16U || memcmp(dst, expected, sizeof(dst)) != 0)
		return 3;
	return 0;
}

/* An independent reference, written straight from the format definition
 * rather than sharing code with the kernel, so this checks the kernel rather
 * than checking it against itself. */
static void yuy2_reference(uint8_t *dst, uint32_t dst_pitch, uint32_t width,
	                       uint32_t height, const uint8_t *y,
	                       uint32_t y_pitch, const uint8_t *cb,
	                       const uint8_t *cr, uint32_t chroma_pitch)
{
	uint32_t row;
	uint32_t x;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma = y + row * y_pitch;
		const uint8_t *blue = cb + (row >> 1) * chroma_pitch;
		const uint8_t *red = cr + (row >> 1) * chroma_pitch;

		for (x = 0U; x < width; x += 2U) {
			uint32_t pair = x >> 1;

			d[0] = luma[x];
			d[1] = blue[pair];
			d[2] = luma[x + 1U < width ? x + 1U : x];
			d[3] = red[pair];
			d += 4;
		}
	}
}

/* U7: the pack was rewritten for speed (branch hoisted out of the inner
 * loop, NEON added on ARM). Exactness is the ship condition, so sweep the
 * geometries that break naive vectorisation: odd widths, odd heights, and
 * the widths either side of the eight-macropixel vector step. */
static int test_yuy2_exactness_sweep(void)
{
	static const uint32_t widths[] = {
		1U, 2U, 3U, 4U, 5U, 15U, 16U, 17U, 31U, 32U, 33U,
		37U, 64U, 65U, 176U, 320U, 352U, 639U, 640U
	};
	static const uint32_t heights[] = { 1U, 2U, 3U, 5U, 16U };
	static uint8_t luma[640 * 16];
	static uint8_t blue[320 * 16];
	static uint8_t red[320 * 16];
	static uint8_t want[640 * 2 * 16];
	static uint8_t got[sizeof(want)];
	unsigned wi;
	unsigned hi;
	uint32_t i;

	for (i = 0U; i < sizeof(luma); i++)
		luma[i] = (uint8_t)(i * 7U + 1U);
	for (i = 0U; i < sizeof(blue); i++) {
		blue[i] = (uint8_t)(i * 13U + 5U);
		red[i] = (uint8_t)(i * 29U + 200U);
	}

	for (wi = 0U; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
		for (hi = 0U; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
			uint32_t width = widths[wi];
			uint32_t height = heights[hi];
			uint32_t chroma_pitch = (width + 1U) / 2U;
			uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
			/* A pitch wider than the row proves the kernel honours
			 * it instead of assuming a packed destination. */
			uint32_t pitch = row_bytes + 8U;
			uint32_t written = 0U;

			if (pitch * height > sizeof(want))
				continue;
			memset(want, 0xa5U, sizeof(want));
			memset(got, 0xa5U, sizeof(got));
			yuy2_reference(want, pitch, width, height, luma, width,
			               blue, red, chroma_pitch);
			if (!sdk_video_yuv420_to_yuy2_scalar(
			        got, pitch, width, height, luma, width, blue,
			        red, chroma_pitch, &written))
				return 1;
			if (written != row_bytes * height)
				return 2;
			if (memcmp(want, got, pitch * height) != 0)
				return 3;

			/* The dispatcher must agree too - on ARM that is the
			 * NEON kernel, on the host it re-checks the scalar. */
			memset(got, 0xa5U, sizeof(got));
			if (!sdk_video_yuv420_to_yuy2(
			        got, pitch, width, height, luma, width, blue,
			        red, chroma_pitch, &written))
				return 4;
			if (memcmp(want, got, pitch * height) != 0)
				return 5;
		}
	}
	return 0;
}

/* Bad geometry must still be refused after the rewrite. */
static int test_yuy2_rejects_bad_params(void)
{
	static uint8_t plane[64];
	uint8_t dst[64];
	uint32_t written = 1U;

	/* Destination pitch narrower than one row. */
	if (sdk_video_yuv420_to_yuy2(dst, 3U, 4U, 1U, plane, 4U, plane,
	                             plane, 2U, &written))
		return 1;
	if (written != 0U)
		return 2;
	/* Luma pitch narrower than the width. */
	if (sdk_video_yuv420_to_yuy2(dst, 8U, 4U, 1U, plane, 3U, plane,
	                             plane, 2U, &written))
		return 3;
	/* Chroma pitch narrower than ceil(width/2). */
	if (sdk_video_yuv420_to_yuy2(dst, 8U, 4U, 1U, plane, 4U, plane,
	                             plane, 1U, &written))
		return 4;
	/* Zero height, and a null plane. */
	if (sdk_video_yuv420_to_yuy2(dst, 8U, 4U, 0U, plane, 4U, plane,
	                             plane, 2U, &written))
		return 5;
	if (sdk_video_yuv420_to_yuy2(dst, 8U, 4U, 1U, 0, 4U, plane,
	                             plane, 2U, &written))
		return 6;
	return 0;
}

/* Half-size pack for frames the overlay cannot fetch at full size: output
 * pixel (r, x) is luma (2r, 2x); output pair p in row r takes chroma
 * (r, 2p), the chroma row of source row 2r. Written from that definition,
 * not from the kernel. */
static void yuy2_half_reference(uint8_t *dst, uint32_t dst_pitch,
	                            uint32_t width, uint32_t height,
	                            const uint8_t *y, uint32_t y_pitch,
	                            const uint8_t *cb, const uint8_t *cr,
	                            uint32_t chroma_pitch)
{
	uint32_t out_w = width / 2U;
	uint32_t out_h = height / 2U;
	uint32_t row;
	uint32_t x;

	for (row = 0U; row < out_h; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma = y + 2U * row * y_pitch;

		for (x = 0U; x < out_w; x += 2U) {
			uint32_t pair = x >> 1;
			uint32_t second = x + 1U < out_w ? x + 1U : x;

			d[0] = luma[2U * x];
			d[1] = cb[row * chroma_pitch + 2U * pair];
			d[2] = luma[2U * second];
			d[3] = cr[row * chroma_pitch + 2U * pair];
			d += 4;
		}
	}
}

/* U2: interlaced frames are blend-packed - output row r is the rounded
 * average of luma rows r and r+1 (last row copied) and, within each chroma
 * plane, of chroma rows (r>>1) and (r>>1)+1 (last chroma row copied),
 * because 4:2:0 interlaced chroma rows alternate fields too. Written from
 * that definition, not from the kernel. */
static void yuy2_blend_reference(uint8_t *dst, uint32_t dst_pitch,
                                 uint32_t width, uint32_t height,
                                 const uint8_t *y, uint32_t y_pitch,
                                 const uint8_t *cb, const uint8_t *cr,
                                 uint32_t chroma_pitch)
{
	uint32_t chroma_rows = (height + 1U) / 2U;
	uint32_t row;
	uint32_t x;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *y0 = y + row * y_pitch;
		const uint8_t *y1 = row + 1U < height
			? y + (row + 1U) * y_pitch : y0;
		uint32_t c = row >> 1;
		const uint8_t *b0 = cb + c * chroma_pitch;
		const uint8_t *r0 = cr + c * chroma_pitch;
		const uint8_t *b1 = c + 1U < chroma_rows
			? b0 + chroma_pitch : b0;
		const uint8_t *r1 = c + 1U < chroma_rows
			? r0 + chroma_pitch : r0;

		for (x = 0U; x < width; x += 2U) {
			uint32_t pair = x >> 1;
			uint32_t xl = x + 1U < width ? x + 1U : x;

			d[0] = (uint8_t)((y0[x] + y1[x] + 1U) >> 1);
			d[1] = (uint8_t)((b0[pair] + b1[pair] + 1U) >> 1);
			d[2] = (uint8_t)((y0[xl] + y1[xl] + 1U) >> 1);
			d[3] = (uint8_t)((r0[pair] + r1[pair] + 1U) >> 1);
			d += 4;
		}
	}
}

static int test_yuy2_blend_exactness_sweep(void)
{
	static const uint32_t widths[] = {
		1U, 2U, 3U, 4U, 5U, 15U, 16U, 17U, 31U, 32U, 33U,
		37U, 64U, 65U, 176U, 320U, 352U, 639U, 640U
	};
	static const uint32_t heights[] = { 1U, 2U, 3U, 5U, 16U };
	static uint8_t luma[640 * 16];
	static uint8_t blue[320 * 16];
	static uint8_t red[320 * 16];
	static uint8_t want[640 * 2 * 16];
	static uint8_t got[sizeof(want)];
	unsigned wi;
	unsigned hi;
	uint32_t i;

	for (i = 0U; i < sizeof(luma); i++)
		luma[i] = (uint8_t)(i * 7U + 1U);
	for (i = 0U; i < sizeof(blue); i++) {
		blue[i] = (uint8_t)(i * 13U + 5U);
		red[i] = (uint8_t)(i * 29U + 200U);
	}

	for (wi = 0U; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
		for (hi = 0U; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
			uint32_t width = widths[wi];
			uint32_t height = heights[hi];
			uint32_t chroma_pitch = (width + 1U) / 2U;
			uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
			uint32_t pitch = row_bytes + 8U;
			uint32_t written = 0U;

			if (pitch * height > sizeof(want))
				continue;
			memset(want, 0xa5U, sizeof(want));
			memset(got, 0xa5U, sizeof(got));
			yuy2_blend_reference(want, pitch, width, height,
			                     luma, width, blue, red,
			                     chroma_pitch);
			if (!sdk_video_yuv420_to_yuy2_blend_scalar(
			        got, pitch, width, height, luma, width, blue,
			        red, chroma_pitch, &written))
				return 1;
			if (written != row_bytes * height)
				return 2;
			if (memcmp(want, got, pitch * height) != 0)
				return 3;

			/* The dispatcher must agree too - on ARM that is the
			 * vrhadd NEON kernel after its self check, on the
			 * host it re-checks the scalar. */
			memset(got, 0xa5U, sizeof(got));
			if (!sdk_video_yuv420_to_yuy2_blend(
			        got, pitch, width, height, luma, width, blue,
			        red, chroma_pitch, &written))
				return 4;
			if (memcmp(want, got, pitch * height) != 0)
				return 5;
		}
	}
	return 0;
}

/* Interlacing must not disturb the progressive pack: with both fields
 * holding the same data the blend is the identity, so a "blended" call on
 * duplicated rows must equal the plain packer bit for bit. */
static int test_blend_of_field_duplicated_frame_matches_plain(void)
{
	static uint8_t luma[320 * 16];
	static uint8_t blue[160 * 16];
	static uint8_t red[160 * 16];
	static uint8_t plain[320 * 2 * 16];
	static uint8_t blended[320 * 2 * 16];
	uint32_t width = 320U;
	uint32_t height = 16U;
	uint32_t chroma_pitch = 160U;
	uint32_t pitch = sdk_video_yuy2_row_bytes(width);
	uint32_t i;
	uint32_t x;

	/* Every luma row identical and every chroma row identical (the two
	 * fields agree completely): blending rows r and r+1, and chroma rows
	 * c and c+1, must then be the identity, because (a + a + 1) >> 1 is
	 * a. The row content still varies per column, so this is not a
	 * constant-fill comparison. */
	for (i = 0U; i < width; i++) {
		uint8_t sample = (uint8_t)(i * 7U + 1U);

		for (x = 0U; x < height; x++)
			luma[x * width + i] = sample;
	}
	for (i = 0U; i < chroma_pitch; i++) {
		uint8_t bsample = (uint8_t)(i * 13U + 5U);
		uint8_t rsample = (uint8_t)(i * 29U + 200U);

		for (x = 0U; x < height / 2U; x++) {
			blue[x * chroma_pitch + i] = bsample;
			red[x * chroma_pitch + i] = rsample;
		}
	}
	memset(plain, 0xa5U, sizeof(plain));
	memset(blended, 0x5aU, sizeof(blended));
	if (!sdk_video_yuv420_to_yuy2_scalar(plain, pitch, width, height,
	                                     luma, width, blue, red,
	                                     chroma_pitch, 0))
		return 1;
	if (!sdk_video_yuv420_to_yuy2_blend(blended, pitch, width, height,
	                                    luma, width, blue, red,
	                                    chroma_pitch, 0))
		return 2;
	if (memcmp(plain, blended, pitch * height) != 0)
		return 3;
	return 0;
}

/* Bad geometry must be refused by the blend pack too. */
static int test_yuy2_blend_rejects_bad_params(void)
{
	static uint8_t plane[64];
	uint8_t dst[64];
	uint32_t written = 1U;

	if (sdk_video_yuv420_to_yuy2_blend(dst, 3U, 4U, 1U, plane, 4U,
	                                   plane, plane, 2U, &written))
		return 1;
	if (written != 0U)
		return 2;
	if (sdk_video_yuv420_to_yuy2_blend(dst, 8U, 4U, 1U, plane, 3U,
	                                   plane, plane, 2U, &written))
		return 3;
	if (sdk_video_yuv420_to_yuy2_blend(dst, 8U, 4U, 1U, plane, 4U,
	                                   plane, plane, 1U, &written))
		return 4;
	if (sdk_video_yuv420_to_yuy2_blend(dst, 8U, 4U, 0U, plane, 4U,
	                                   plane, plane, 2U, &written))
		return 5;
	if (sdk_video_yuv420_to_yuy2_blend(dst, 8U, 4U, 1U, 0, 4U,
	                                   plane, plane, 2U, &written))
		return 6;
	return 0;
}

static int test_yuy2_half_pack(void)
{
	static const uint32_t widths[] = { 2U, 3U, 4U, 6U, 7U, 34U, 64U, 642U };
	static const uint32_t heights[] = { 2U, 3U, 4U, 9U };
	static uint8_t luma[642 * 9];
	static uint8_t blue[321 * 9];
	static uint8_t red[321 * 9];
	static uint8_t want[(642 / 2 + 8) * 2 * 9];
	static uint8_t got[sizeof(want)];
	unsigned wi;
	unsigned hi;
	uint32_t i;

	for (i = 0U; i < sizeof(luma); i++)
		luma[i] = (uint8_t)(i * 7U + 1U);
	for (i = 0U; i < sizeof(blue); i++) {
		blue[i] = (uint8_t)(i * 13U + 5U);
		red[i] = (uint8_t)(i * 29U + 200U);
	}
	for (wi = 0U; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
		for (hi = 0U; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
			uint32_t width = widths[wi];
			uint32_t height = heights[hi];
			uint32_t chroma_pitch = (width + 1U) / 2U;
			uint32_t row_bytes = sdk_video_yuy2_row_bytes(width / 2U);
			uint32_t pitch = row_bytes + 8U;
			uint32_t written = 0U;

			memset(want, 0xa5U, sizeof(want));
			memset(got, 0xa5U, sizeof(got));
			yuy2_half_reference(want, pitch, width, height, luma,
			                    width, blue, red, chroma_pitch);
			if (!sdk_video_yuv420_to_yuy2_half(
			        got, pitch, width, height, luma, width, blue,
			        red, chroma_pitch, &written))
				return 1;
			if (written != row_bytes * (height / 2U))
				return 2;
			/* Rows past the half height stay untouched too. */
			if (memcmp(want, got, sizeof(want)) != 0)
				return 3;
		}
	}
	/* A frame with no half-size row or column is refused. */
	if (sdk_video_yuv420_to_yuy2_half(got, 8U, 1U, 4U, luma, 4U, blue,
	                                  red, 2U, 0))
		return 4;
	if (sdk_video_yuv420_to_yuy2_half(got, 8U, 4U, 1U, luma, 4U, blue,
	                                  red, 2U, 0))
		return 5;
	return 0;
}

static int test_streaming_decode(void)
{
	const struct SDKVideoDecoderOps *ops;
	struct SDKVideoDecoderInfo info;
	struct SDKVideoDecodedFrame frame;
	uint8_t *yuy2;
	uint32_t row_bytes;
	uint32_t offset = 0U;
	uint32_t frames = 0U;
	uint64_t hash = UINT64_C(14695981039346656037);
	void *decoder;
	int result;

	ops = sdk_video_backend_find(1U, 1U);
	if (!ops || ops->codec != 1U || ops->container != 1U)
		return 1;
	decoder = ops->create(0U);
	if (!decoder)
		return 2;

	/* Deliberately awkward chunks exercise header and packet boundaries. */
	while (offset < zz9k_mpeg1_ps_fixture_len) {
		uint32_t chunk = 137U;
		uint32_t accepted = 0U;

		if (chunk > zz9k_mpeg1_ps_fixture_len - offset)
			chunk = zz9k_mpeg1_ps_fixture_len - offset;
		if (!ops->write(decoder, zz9k_mpeg1_ps_fixture + offset,
		                chunk, 0, &accepted) || accepted != chunk) {
			ops->destroy(decoder);
			return 3;
		}
		offset += chunk;

		for (;;) {
			uint32_t bytes_written;
			uint32_t row;

			result = ops->decode(decoder, &frame);
			if (result != SDK_VIDEO_BACKEND_FRAME)
				break;
			if (frame.width != 32U || frame.height != 16U) {
				ops->destroy(decoder);
				return 4;
			}
			if (frame.time_millis != frames * 40U) {
				ops->destroy(decoder);
				return 13;
			}
			row_bytes = sdk_video_yuy2_row_bytes(frame.width);
			yuy2 = (uint8_t *)malloc(row_bytes * frame.height);
			if (!yuy2) {
				ops->destroy(decoder);
				return 5;
			}
			if (!sdk_video_yuv420_to_yuy2(
			        yuy2, row_bytes, frame.width, frame.height,
			        frame.y, frame.y_pitch, frame.cb, frame.cr,
			        frame.chroma_pitch, &bytes_written) ||
			    bytes_written != row_bytes * frame.height) {
				free(yuy2);
				ops->destroy(decoder);
				return 6;
			}
			for (row = 0U; row < frame.height; row++)
				hash = fnv1a64(hash, yuy2 + row * row_bytes, row_bytes);
			free(yuy2);
			frames++;
		}
		if (result == SDK_VIDEO_BACKEND_ERROR) {
			ops->destroy(decoder);
			return 7;
		}
	}

	/* The mailbox API permits clients to release/reuse their shared input
	 * buffer after the final data write, then signal EOF with no source. */
	{
		uint32_t accepted = 1U;

		if (!ops->write(decoder, NULL, 0U, 1, &accepted) || accepted != 0U) {
			ops->destroy(decoder);
			return 8;
		}
	}

	while (result != SDK_VIDEO_BACKEND_DONE) {
		result = ops->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_NEED_INPUT ||
		    result == SDK_VIDEO_BACKEND_ERROR) {
			ops->destroy(decoder);
			return 9;
		}
		if (result == SDK_VIDEO_BACKEND_FRAME) {
			uint32_t bytes_written;
			uint32_t row;

			if (frame.time_millis != frames * 40U) {
				ops->destroy(decoder);
				return 14;
			}
			row_bytes = sdk_video_yuy2_row_bytes(frame.width);
			yuy2 = (uint8_t *)malloc(row_bytes * frame.height);
			if (!yuy2 || !sdk_video_yuv420_to_yuy2(
			        yuy2, row_bytes, frame.width, frame.height,
			        frame.y, frame.y_pitch, frame.cb, frame.cr,
			        frame.chroma_pitch, &bytes_written)) {
				free(yuy2);
				ops->destroy(decoder);
				return 10;
			}
			for (row = 0U; row < frame.height; row++)
				hash = fnv1a64(hash, yuy2 + row * row_bytes, row_bytes);
			free(yuy2);
			frames++;
		}
	}

	if (!ops->get_info(decoder, &info) || info.width != 32U ||
	    info.height != 16U || info.frame_rate_milli != 25000U) {
		ops->destroy(decoder);
		return 11;
	}
	ops->destroy(decoder);

	printf("video fixture: frames=%lu rate_milli=%lu fnv64=%016llx\n",
	       (unsigned long)frames, (unsigned long)info.frame_rate_milli,
	       (unsigned long long)hash);
	if (EXPECTED_FRAMES != 0U &&
	    (frames != EXPECTED_FRAMES || hash != EXPECTED_FNV64))
		return 12;
	return 0;
}

/* A 40x24 MPEG-1 picture is coded as 48x32 macroblocks: frames and stream
 * info carry the picture size (the session refuses frames that differ from
 * the size the client began with) over the coded stride. Both Program
 * Stream codecs decode up to 800x600. */
static int test_picture_size_and_cap(void)
{
	const struct SDKVideoDecoderOps *m1 = sdk_video_backend_find(1U, 1U);
	const struct SDKVideoDecoderOps *m2 = sdk_video_backend_find(2U, 1U);
	struct SDKVideoDecoderInfo info;
	struct SDKVideoDecodedFrame frame;
	uint32_t accepted = 0U;
	uint32_t frames = 0U;
	uint32_t guard = 0U;
	void *decoder;
	int result;

	if (!m1 || !m2 || !m1->geometry_ok || !m2->geometry_ok)
		return 1;
	if (!m1->geometry_ok(800U, 600U) || !m2->geometry_ok(800U, 600U) ||
	    m1->geometry_ok(801U, 600U) || m2->geometry_ok(801U, 600U) ||
	    m1->geometry_ok(800U, 601U) || m2->geometry_ok(800U, 601U) ||
	    m1->geometry_ok(0U, 16U) || m2->geometry_ok(16U, 0U))
		return 2;
	decoder = m1->create(0U);
	if (!decoder)
		return 3;
	if (m1->write(decoder, zz9k_mpeg1_crop_ps_fixture,
	              zz9k_mpeg1_crop_ps_fixture_len, 1, &accepted) !=
	        SDK_VIDEO_BACKEND_WRITE_OK ||
	    accepted != zz9k_mpeg1_crop_ps_fixture_len) {
		m1->destroy(decoder);
		return 4;
	}
	do {
		result = m1->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_FRAME) {
			if (frame.width != 40U || frame.height != 24U ||
			    frame.y_pitch != 48U || frame.chroma_pitch != 24U) {
				m1->destroy(decoder);
				return 5;
			}
			frames++;
		}
	} while (result != SDK_VIDEO_BACKEND_DONE &&
	         result != SDK_VIDEO_BACKEND_ERROR && guard++ < 100U);
	if (result != SDK_VIDEO_BACKEND_DONE || frames != 3U ||
	    !m1->get_info(decoder, &info) || info.width != 40U ||
	    info.height != 24U) {
		m1->destroy(decoder);
		return 6;
	}
	m1->destroy(decoder);
	return 0;
}

int main(void)
{
	int result = test_yuy2_conversion();

	if (result != 0)
		return 10 + result;
	result = test_yuy2_exactness_sweep();
	if (result != 0)
		return 50 + result;
	result = test_yuy2_rejects_bad_params();
	if (result != 0)
		return 70 + result;
	result = test_yuy2_half_pack();
	if (result != 0)
		return 90 + result;
	result = test_yuy2_blend_exactness_sweep();
	if (result != 0)
		return 110 + result;
	result = test_blend_of_field_duplicated_frame_matches_plain();
	if (result != 0)
		return 130 + result;
	result = test_yuy2_blend_rejects_bad_params();
	if (result != 0)
		return 150 + result;
	result = test_streaming_decode();
	if (result != 0)
		return 30 + result;
	result = test_picture_size_and_cap();
	if (result != 0)
		return 170 + result;
	return 0;
}
