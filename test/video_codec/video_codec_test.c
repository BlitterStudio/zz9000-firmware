/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpeg1_ps_fixture.inc"

#define EXPECTED_FRAMES 5U
#define EXPECTED_FNV64 UINT64_C(0x12cf0e63c2e2c840)

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
	if (!ops || sdk_video_backend_find(2U, 1U) != 0)
		return 1;
	decoder = ops->create();
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
	result = test_streaming_decode();
	if (result != 0)
		return 30 + result;
	return 0;
}
