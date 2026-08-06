/*
 * planar YUV 4:2:0 to packed YUY2 (U7).
 *
 * The r1 profiling round measured this at 2.767 ms per 640x480 frame, a
 * quarter of all card pipeline time, so it is worth optimising - and, at
 * only a quarter, not worth the planar FPGA subproject that would have
 * removed it entirely (R13 declined on that evidence).
 *
 * KTD8 requires the scalar kernel to remain independently callable as the
 * exactness oracle, so it is never deleted: the NEON path is checked
 * against it on the card and disables itself on any disagreement.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_yuy2.h"

#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SDK_VIDEO_YUY2_HAVE_NEON 1
#else
#define SDK_VIDEO_YUY2_HAVE_NEON 0
#endif

uint32_t sdk_video_yuy2_row_bytes(uint32_t width)
{
	/* One macropixel per two source pixels, rounded up so an odd width
	 * still gets a complete final pair. */
	return ((width + 1U) >> 1) * 4U;
}

static int yuy2_params_valid(const uint8_t *dst, uint32_t dst_pitch,
	                         uint32_t width, uint32_t height,
	                         const uint8_t *y, uint32_t y_pitch,
	                         const uint8_t *cb, const uint8_t *cr,
	                         uint32_t chroma_pitch)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);

	if (!dst || !y || !cb || !cr || !row_bytes || !height ||
	    dst_pitch < row_bytes || y_pitch < width ||
	    chroma_pitch < ((width + 1U) >> 1))
		return 0;
	if (height > (0xffffffffU / row_bytes))
		return 0;
	return 1;
}

/* Emit `pairs` macropixels from the given row pointers. The odd-width tail
 * is the caller's business, so this stays branch-free per pixel - the
 * original loop tested `x + 1 < width` on every pair to serve a case that
 * can only arise once per row. */
static void yuy2_pack_row_scalar(uint8_t *d, const uint8_t *luma,
	                             const uint8_t *blue, const uint8_t *red,
	                             uint32_t pairs)
{
	uint32_t pair;

	for (pair = 0U; pair < pairs; pair++) {
		d[0] = luma[2U * pair];
		d[1] = blue[pair];
		d[2] = luma[2U * pair + 1U];
		d[3] = red[pair];
		d += 4;
	}
}

int sdk_video_yuv420_to_yuy2_scalar(uint8_t *dst, uint32_t dst_pitch,
	                                uint32_t width, uint32_t height,
	                                const uint8_t *y, uint32_t y_pitch,
	                                const uint8_t *cb, const uint8_t *cr,
	                                uint32_t chroma_pitch,
	                                uint32_t *bytes_written)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
	uint32_t full_pairs = width >> 1;
	uint32_t row;

	if (bytes_written)
		*bytes_written = 0U;
	if (!yuy2_params_valid(dst, dst_pitch, width, height, y, y_pitch,
	                       cb, cr, chroma_pitch))
		return 0;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma = y + row * y_pitch;
		const uint8_t *blue = cb + (row >> 1) * chroma_pitch;
		const uint8_t *red = cr + (row >> 1) * chroma_pitch;

		yuy2_pack_row_scalar(d, luma, blue, red, full_pairs);
		if ((width & 1U) != 0U) {
			/* Odd width: the final macropixel duplicates the last
			 * luma sample, exactly as the original loop did. */
			uint32_t pair = full_pairs;

			d += full_pairs * 4U;
			d[0] = luma[width - 1U];
			d[1] = blue[pair];
			d[2] = luma[width - 1U];
			d[3] = red[pair];
		}
	}

	if (bytes_written)
		*bytes_written = row_bytes * height;
	return 1;
}

#if SDK_VIDEO_YUY2_HAVE_NEON
int sdk_video_yuv420_to_yuy2_neon(uint8_t *dst, uint32_t dst_pitch,
	                              uint32_t width, uint32_t height,
	                              const uint8_t *y, uint32_t y_pitch,
	                              const uint8_t *cb, const uint8_t *cr,
	                              uint32_t chroma_pitch,
	                              uint32_t *bytes_written)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
	uint32_t full_pairs = width >> 1;
	/* Eight macropixels - sixteen source pixels - per vector pass. */
	uint32_t vector_pairs = full_pairs & ~7U;
	uint32_t row;

	if (bytes_written)
		*bytes_written = 0U;
	if (!yuy2_params_valid(dst, dst_pitch, width, height, y, y_pitch,
	                       cb, cr, chroma_pitch))
		return 0;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma = y + row * y_pitch;
		const uint8_t *blue = cb + (row >> 1) * chroma_pitch;
		const uint8_t *red = cr + (row >> 1) * chroma_pitch;
		uint32_t pair = 0U;

		for (; pair < vector_pairs; pair += 8U) {
			/* vld2 splits luma into even and odd samples, which is
			 * precisely the Y0/Y1 of each macropixel; vst4 then
			 * interleaves Y0,Cb,Y1,Cr in one store. */
			uint8x8x2_t luma_pair = vld2_u8(luma + 2U * pair);
			uint8x8x4_t out;

			out.val[0] = luma_pair.val[0];
			out.val[1] = vld1_u8(blue + pair);
			out.val[2] = luma_pair.val[1];
			out.val[3] = vld1_u8(red + pair);
			vst4_u8(d + pair * 4U, out);
		}
		if (pair < full_pairs) {
			yuy2_pack_row_scalar(d + pair * 4U, luma + 2U * pair,
			                     blue + pair, red + pair,
			                     full_pairs - pair);
		}
		if ((width & 1U) != 0U) {
			uint8_t *tail = d + full_pairs * 4U;

			tail[0] = luma[width - 1U];
			tail[1] = blue[full_pairs];
			tail[2] = luma[width - 1U];
			tail[3] = red[full_pairs];
		}
	}

	if (bytes_written)
		*bytes_written = row_bytes * height;
	return 1;
}
#endif

/* 0 = not yet checked, 1 = NEON verified against the oracle, -1 = disabled
 * because it disagreed. Checked once per session begin. */
static int yuy2_neon_state;

#if SDK_VIDEO_YUY2_HAVE_NEON
/* Deliberately an odd width and an odd height, so the scalar tail, the
 * duplicated final luma and the chroma row-pairing are all exercised rather
 * than only the vector-aligned happy path. */
#define YUY2_CHECK_W 37U
#define YUY2_CHECK_H 5U

static int yuy2_self_check(void)
{
	static uint8_t luma[YUY2_CHECK_W * YUY2_CHECK_H];
	static uint8_t blue[((YUY2_CHECK_W + 1U) / 2U) * YUY2_CHECK_H];
	static uint8_t red[((YUY2_CHECK_W + 1U) / 2U) * YUY2_CHECK_H];
	static uint8_t want[((YUY2_CHECK_W + 1U) / 2U) * 4U * YUY2_CHECK_H];
	static uint8_t got[sizeof(want)];
	uint32_t chroma_pitch = (YUY2_CHECK_W + 1U) / 2U;
	uint32_t pitch = chroma_pitch * 4U;
	uint32_t i;

	for (i = 0U; i < sizeof(luma); i++)
		luma[i] = (uint8_t)(i * 7U + 1U);
	for (i = 0U; i < sizeof(blue); i++) {
		blue[i] = (uint8_t)(i * 13U + 5U);
		red[i] = (uint8_t)(i * 29U + 200U);
	}
	memset(want, 0xa5U, sizeof(want));
	memset(got, 0x5aU, sizeof(got));

	if (!sdk_video_yuv420_to_yuy2_scalar(
	        want, pitch, YUY2_CHECK_W, YUY2_CHECK_H, luma, YUY2_CHECK_W,
	        blue, red, chroma_pitch, 0))
		return 0;
	if (!sdk_video_yuv420_to_yuy2_neon(
	        got, pitch, YUY2_CHECK_W, YUY2_CHECK_H, luma, YUY2_CHECK_W,
	        blue, red, chroma_pitch, 0))
		return 0;
	return memcmp(want, got, sizeof(want)) == 0;
}
#endif

void sdk_video_yuy2_reset_dispatch(void)
{
	yuy2_neon_state = 0;
}

int sdk_video_yuy2_neon_active(void)
{
	return yuy2_neon_state > 0;
}

int sdk_video_yuv420_to_yuy2(uint8_t *dst, uint32_t dst_pitch,
	                         uint32_t width, uint32_t height,
	                         const uint8_t *y, uint32_t y_pitch,
	                         const uint8_t *cb, const uint8_t *cr,
	                         uint32_t chroma_pitch,
	                         uint32_t *bytes_written)
{
#if SDK_VIDEO_YUY2_HAVE_NEON
	if (yuy2_neon_state == 0)
		yuy2_neon_state = yuy2_self_check() ? 1 : -1;
	if (yuy2_neon_state > 0) {
		return sdk_video_yuv420_to_yuy2_neon(
			dst, dst_pitch, width, height, y, y_pitch, cb, cr,
			chroma_pitch, bytes_written);
	}
#endif
	return sdk_video_yuv420_to_yuy2_scalar(
		dst, dst_pitch, width, height, y, y_pitch, cb, cr,
		chroma_pitch, bytes_written);
}
