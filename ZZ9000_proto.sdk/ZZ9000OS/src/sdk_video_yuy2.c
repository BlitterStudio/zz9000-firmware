/*
 * Planar YUV 4:2:0 to packed YUY2 conversion for P96 video-window sources.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_yuy2.h"

uint32_t sdk_video_yuy2_row_bytes(uint32_t width)
{
	if (width == 0U || width > 0x7ffffffeU)
		return 0U;
	return ((width >> 1) + (width & 1U)) * 4U;
}

int sdk_video_yuv420_to_yuy2(uint8_t *dst, uint32_t dst_pitch,
	                         uint32_t width, uint32_t height,
	                         const uint8_t *y, uint32_t y_pitch,
	                         const uint8_t *cb, const uint8_t *cr,
	                         uint32_t chroma_pitch,
	                         uint32_t *bytes_written)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(width);
	uint32_t row;

	if (bytes_written)
		*bytes_written = 0U;
	if (!dst || !y || !cb || !cr || !row_bytes || !height ||
	    dst_pitch < row_bytes || y_pitch < width ||
	    chroma_pitch < ((width + 1U) >> 1)) {
		return 0;
	}
	if (height > (0xffffffffU / row_bytes))
		return 0;

	for (row = 0U; row < height; row++) {
		uint8_t *d = dst + row * dst_pitch;
		const uint8_t *luma = y + row * y_pitch;
		const uint8_t *blue = cb + (row >> 1) * chroma_pitch;
		const uint8_t *red = cr + (row >> 1) * chroma_pitch;
		uint32_t x;

		for (x = 0U; x < width; x += 2U) {
			uint32_t pair = x >> 1;
			d[0] = luma[x];
			d[1] = blue[pair];
			d[2] = luma[x + 1U < width ? x + 1U : x];
			d[3] = red[pair];
			d += 4;
		}
	}

	if (bytes_written)
		*bytes_written = row_bytes * height;
	return 1;
}
