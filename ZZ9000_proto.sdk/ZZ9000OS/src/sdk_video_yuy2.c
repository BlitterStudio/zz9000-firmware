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

/* Blend-pack `pairs` macropixels: every sample is the rounded average of
 * the two rows the caller picked, so like yuy2_pack_row_scalar this stays
 * branch-free per pixel and leaves the odd-width tail to the caller. */
static void yuy2_blend_pack_row_scalar(uint8_t *d, const uint8_t *luma0,
                                       const uint8_t *luma1,
                                       const uint8_t *blue0,
                                       const uint8_t *blue1,
                                       const uint8_t *red0,
                                       const uint8_t *red1,
                                       uint32_t pairs)
{
	uint32_t pair;

	for (pair = 0U; pair < pairs; pair++) {
		d[0] = (uint8_t)((luma0[2U * pair] + luma1[2U * pair] + 1U) >> 1);
		d[1] = (uint8_t)((blue0[pair] + blue1[pair] + 1U) >> 1);
		d[2] = (uint8_t)((luma0[2U * pair + 1U] +
		                  luma1[2U * pair + 1U] + 1U) >> 1);
		d[3] = (uint8_t)((red0[pair] + red1[pair] + 1U) >> 1);
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

int sdk_video_yuv420_to_yuy2_blend_scalar(uint8_t *dst, uint32_t dst_pitch,
                                          uint32_t width, uint32_t height,
                                          const uint8_t *y, uint32_t y_pitch,
                                          const uint8_t *cb,
                                          const uint8_t *cr,
                                          uint32_t chroma_pitch,
                                          uint32_t *bytes_written)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
	uint32_t full_pairs = width >> 1;
	uint32_t chroma_rows = (height + 1U) >> 1;
	uint32_t row;

	if (bytes_written)
		*bytes_written = 0U;
	if (!yuy2_params_valid(dst, dst_pitch, width, height, y, y_pitch,
	                       cb, cr, chroma_pitch))
		return 0;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma0 = y + row * y_pitch;
		/* The frame holds two interleaved fields: blending rows r and
		 * r+1 averages the field lines that land on scanline r. The
		 * last row has no successor and is copied. */
		const uint8_t *luma1 = row + 1U < height
			? y + (row + 1U) * y_pitch : luma0;
		uint32_t chroma_row = row >> 1;
		/* 4:2:0 interlaced chroma rows alternate fields too, so the
		 * chroma blend stays inside the chroma plane with the same
		 * row-pairing; the last chroma row is copied for the last
		 * luma row pair. */
		const uint8_t *blue0 = cb + chroma_row * chroma_pitch;
		const uint8_t *red0 = cr + chroma_row * chroma_pitch;
		const uint8_t *blue1 = chroma_row + 1U < chroma_rows
			? blue0 + chroma_pitch : blue0;
		const uint8_t *red1 = chroma_row + 1U < chroma_rows
			? red0 + chroma_pitch : red0;

		yuy2_blend_pack_row_scalar(d, luma0, luma1, blue0, blue1,
		                           red0, red1, full_pairs);
		if ((width & 1U) != 0U) {
			/* Odd width: the final macropixel duplicates the last
			 * blended luma sample, as the plain packer does. */
			uint32_t last = width - 1U;
			uint32_t pair = full_pairs;
			uint8_t ybl = (uint8_t)((luma0[last] + luma1[last] +
			                         1U) >> 1);

			d += full_pairs * 4U;
			d[0] = ybl;
			d[1] = (uint8_t)((blue0[pair] + blue1[pair] + 1U) >> 1);
			d[2] = ybl;
			d[3] = (uint8_t)((red0[pair] + red1[pair] + 1U) >> 1);
		}
	}

	if (bytes_written)
		*bytes_written = row_bytes * height;
	return 1;
}

int sdk_video_yuv420_to_yuy2_half(uint8_t *dst, uint32_t dst_pitch,
	                              uint32_t width, uint32_t height,
	                              const uint8_t *y, uint32_t y_pitch,
	                              const uint8_t *cb, const uint8_t *cr,
	                              uint32_t chroma_pitch,
	                              uint32_t *bytes_written)
{
	uint32_t out_w = width / 2U;
	uint32_t out_h = height / 2U;
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(out_w);
	uint32_t full_pairs = out_w >> 1;
	uint32_t row;

	if (bytes_written)
		*bytes_written = 0U;
	if (!dst || !y || !cb || !cr || !out_w || !out_h ||
	    dst_pitch < row_bytes || y_pitch < width ||
	    chroma_pitch < ((width + 1U) >> 1) ||
	    out_h > (0xffffffffU / dst_pitch))
		return 0;

	for (row = 0U; row < out_h; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma = y + 2U * row * y_pitch;
		/* Source row 2 * row pairs with chroma row `row`. */
		const uint8_t *blue = cb + row * chroma_pitch;
		const uint8_t *red = cr + row * chroma_pitch;
		uint32_t pair;

		/* Output pair p covers source pixels 4p..4p+3: luma 4p and
		 * 4p + 2, chroma column 2p. */
		for (pair = 0U; pair < full_pairs; pair++) {
			d[0] = luma[4U * pair];
			d[1] = blue[2U * pair];
			d[2] = luma[4U * pair + 2U];
			d[3] = red[2U * pair];
			d += 4;
		}
		if ((out_w & 1U) != 0U) {
			d[0] = luma[4U * full_pairs];
			d[1] = blue[2U * full_pairs];
			d[2] = luma[4U * full_pairs];
			d[3] = red[2U * full_pairs];
		}
	}
	if (bytes_written)
		*bytes_written = row_bytes * out_h;
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

int sdk_video_yuv420_to_yuy2_blend_neon(uint8_t *dst, uint32_t dst_pitch,
                                        uint32_t width, uint32_t height,
                                        const uint8_t *y, uint32_t y_pitch,
                                        const uint8_t *cb,
                                        const uint8_t *cr,
                                        uint32_t chroma_pitch,
                                        uint32_t *bytes_written)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
	uint32_t full_pairs = width >> 1;
	uint32_t vector_pairs = full_pairs & ~7U;
	uint32_t chroma_rows = (height + 1U) >> 1;
	uint32_t row;

	if (bytes_written)
		*bytes_written = 0U;
	if (!yuy2_params_valid(dst, dst_pitch, width, height, y, y_pitch,
	                       cb, cr, chroma_pitch))
		return 0;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma0 = y + row * y_pitch;
		const uint8_t *luma1 = row + 1U < height
			? y + (row + 1U) * y_pitch : luma0;
		uint32_t chroma_row = row >> 1;
		const uint8_t *blue0 = cb + chroma_row * chroma_pitch;
		const uint8_t *red0 = cr + chroma_row * chroma_pitch;
		const uint8_t *blue1 = chroma_row + 1U < chroma_rows
			? blue0 + chroma_pitch : blue0;
		const uint8_t *red1 = chroma_row + 1U < chroma_rows
			? red0 + chroma_pitch : red0;
		uint32_t pair = 0U;

		for (; pair < vector_pairs; pair += 8U) {
			/* Same interleaved load/store trick as the plain
			 * packer; vrhadd_u8 is the rounded average
			 * (a + b + 1) >> 1 of the two field rows, so the
			 * blend rides along at load cost. vrhadd(x, x) is x,
			 * which keeps the last-row copy uniform. */
			uint8x8x2_t even = vld2_u8(luma0 + 2U * pair);
			uint8x8x2_t odd = vld2_u8(luma1 + 2U * pair);
			uint8x8x4_t out;

			out.val[0] = vrhadd_u8(even.val[0], odd.val[0]);
			out.val[1] = vrhadd_u8(vld1_u8(blue0 + pair),
			                       vld1_u8(blue1 + pair));
			out.val[2] = vrhadd_u8(even.val[1], odd.val[1]);
			out.val[3] = vrhadd_u8(vld1_u8(red0 + pair),
			                       vld1_u8(red1 + pair));
			vst4_u8(d + pair * 4U, out);
		}
		if (pair < full_pairs) {
			yuy2_blend_pack_row_scalar(
				d + pair * 4U, luma0 + 2U * pair,
				luma1 + 2U * pair, blue0 + pair,
				blue1 + pair, red0 + pair, red1 + pair,
				full_pairs - pair);
		}
		if ((width & 1U) != 0U) {
			uint8_t *tail = d + full_pairs * 4U;
			uint32_t last = width - 1U;
			uint8_t ybl = (uint8_t)((luma0[last] + luma1[last] +
			                         1U) >> 1);

			tail[0] = ybl;
			tail[1] = (uint8_t)((blue0[full_pairs] +
			                     blue1[full_pairs] + 1U) >> 1);
			tail[2] = ybl;
			tail[3] = (uint8_t)((red0[full_pairs] +
			                     red1[full_pairs] + 1U) >> 1);
		}
	}

	if (bytes_written)
		*bytes_written = row_bytes * height;
	return 1;
}
#endif

/* Same three states for each kernel, checked once per session begin. */
static int yuy2_neon_state;
static int yuy2_blend_neon_state;

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

/* Same geometry as the plain check; the pattern must also disagree by an
 * odd amount per blended pair somewhere so a vrhadd off-by-one (truncating
 * halving add instead of rounding) cannot pass unnoticed. */
static int yuy2_blend_self_check(void)
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
		luma[i] = (uint8_t)(i * 5U + 3U);
	for (i = 0U; i < sizeof(blue); i++) {
		blue[i] = (uint8_t)(i * 11U + 17U);
		red[i] = (uint8_t)(i * 19U + 90U);
	}
	memset(want, 0xa5U, sizeof(want));
	memset(got, 0x5aU, sizeof(got));

	if (!sdk_video_yuv420_to_yuy2_blend_scalar(
	        want, pitch, YUY2_CHECK_W, YUY2_CHECK_H, luma, YUY2_CHECK_W,
	        blue, red, chroma_pitch, 0))
		return 0;
	if (!sdk_video_yuv420_to_yuy2_blend_neon(
	        got, pitch, YUY2_CHECK_W, YUY2_CHECK_H, luma, YUY2_CHECK_W,
	        blue, red, chroma_pitch, 0))
		return 0;
	return memcmp(want, got, sizeof(want)) == 0;
}
#endif

void sdk_video_yuy2_reset_dispatch(void)
{
	yuy2_neon_state = 0;
	yuy2_blend_neon_state = 0;
}

int sdk_video_yuy2_neon_active(void)
{
	return yuy2_neon_state > 0 || yuy2_blend_neon_state > 0;
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

int sdk_video_yuv420_to_yuy2_blend(uint8_t *dst, uint32_t dst_pitch,
                                   uint32_t width, uint32_t height,
                                   const uint8_t *y, uint32_t y_pitch,
                                   const uint8_t *cb, const uint8_t *cr,
                                   uint32_t chroma_pitch,
                                   uint32_t *bytes_written)
{
#if SDK_VIDEO_YUY2_HAVE_NEON
	if (yuy2_blend_neon_state == 0)
		yuy2_blend_neon_state = yuy2_blend_self_check() ? 1 : -1;
	if (yuy2_blend_neon_state > 0) {
		return sdk_video_yuv420_to_yuy2_blend_neon(
			dst, dst_pitch, width, height, y, y_pitch, cb, cr,
			chroma_pitch, bytes_written);
	}
#endif
	return sdk_video_yuv420_to_yuy2_blend_scalar(
		dst, dst_pitch, width, height, y, y_pitch, cb, cr,
		chroma_pitch, bytes_written);
}
