/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Small SDK surface helpers used by firmware mailbox services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_surface.h"
#include <string.h>

uint32_t sdk_surface_format_bytes(uint32_t format)
{
	switch (format) {
	case SDK_SURFACE_FORMAT_INDEX8:
		return 1U;
	case SDK_SURFACE_FORMAT_RGB565:
	case SDK_SURFACE_FORMAT_RGB555:
		return 2U;
	case SDK_SURFACE_FORMAT_RGB888:
		return 3U;
	case SDK_SURFACE_FORMAT_ARGB8888:
	case SDK_SURFACE_FORMAT_RGBA8888:
	case SDK_SURFACE_FORMAT_BGRA8888:
		return 4U;
	default:
		return 0U;
	}
}

static int surface_rect_valid(uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t bytes_per_pixel,
                              uint32_t x, uint32_t y,
                              uint32_t rect_width,
                              uint32_t rect_height)
{
	uint32_t row_bytes;

	if (bytes_per_pixel == 0U || width == 0U || height == 0U ||
	    pitch == 0U || rect_width == 0U || rect_height == 0U) {
		return 0;
	}
	if (x > width || y > height || rect_width > (width - x) ||
	    rect_height > (height - y)) {
		return 0;
	}
	if (rect_width > (0xffffffffU / bytes_per_pixel) ||
	    x > (0xffffffffU / bytes_per_pixel)) {
		return 0;
	}

	row_bytes = rect_width * bytes_per_pixel;
	if ((x * bytes_per_pixel) > pitch ||
	    row_bytes > (pitch - (x * bytes_per_pixel))) {
		return 0;
	}

	return 1;
}

static int intersect_rect(uint32_t ax, uint32_t ay, uint32_t aw,
                          uint32_t ah, uint32_t bx, uint32_t by,
                          uint32_t bw, uint32_t bh, uint32_t *out_x,
                          uint32_t *out_y, uint32_t *out_w,
                          uint32_t *out_h)
{
	uint64_t ax2;
	uint64_t ay2;
	uint64_t bx2;
	uint64_t by2;
	uint64_t x1;
	uint64_t y1;
	uint64_t x2;
	uint64_t y2;

	if (!out_x || !out_y || !out_w || !out_h ||
	    aw == 0U || ah == 0U || bw == 0U || bh == 0U)
		return 0;

	ax2 = (uint64_t)ax + aw;
	ay2 = (uint64_t)ay + ah;
	bx2 = (uint64_t)bx + bw;
	by2 = (uint64_t)by + bh;
	x1 = ax > bx ? ax : bx;
	y1 = ay > by ? ay : by;
	x2 = ax2 < bx2 ? ax2 : bx2;
	y2 = ay2 < by2 ? ay2 : by2;
	if (x1 >= x2 || y1 >= y2)
		return 0;
	if (x1 > 0xffffffffULL || y1 > 0xffffffffULL ||
	    (x2 - x1) > 0xffffffffULL || (y2 - y1) > 0xffffffffULL)
		return 0;

	*out_x = (uint32_t)x1;
	*out_y = (uint32_t)y1;
	*out_w = (uint32_t)(x2 - x1);
	*out_h = (uint32_t)(y2 - y1);
	return 1;
}

static void write_pixel(uint8_t *dst, uint32_t format, uint32_t color)
{
	switch (format) {
	case SDK_SURFACE_FORMAT_INDEX8:
		dst[0] = (uint8_t)color;
		break;
	case SDK_SURFACE_FORMAT_RGB565:
	case SDK_SURFACE_FORMAT_RGB555:
		dst[0] = (uint8_t)(color >> 8);
		dst[1] = (uint8_t)color;
		break;
	case SDK_SURFACE_FORMAT_ARGB8888:
		dst[0] = (uint8_t)(color >> 24);
		dst[1] = (uint8_t)(color >> 16);
		dst[2] = (uint8_t)(color >> 8);
		dst[3] = (uint8_t)color;
		break;
	case SDK_SURFACE_FORMAT_RGBA8888:
		dst[0] = (uint8_t)(color >> 16);
		dst[1] = (uint8_t)(color >> 8);
		dst[2] = (uint8_t)color;
		dst[3] = (uint8_t)(color >> 24);
		break;
	case SDK_SURFACE_FORMAT_BGRA8888:
		dst[0] = (uint8_t)color;
		dst[1] = (uint8_t)(color >> 8);
		dst[2] = (uint8_t)(color >> 16);
		dst[3] = (uint8_t)(color >> 24);
		break;
	case SDK_SURFACE_FORMAT_RGB888:
		dst[0] = (uint8_t)(color >> 16);
		dst[1] = (uint8_t)(color >> 8);
		dst[2] = (uint8_t)color;
		break;
	default:
		break;
	}
}

int sdk_surface_fill_rect(uint8_t *surface, uint32_t width, uint32_t height,
                          uint32_t pitch, uint32_t format,
                          uint32_t x, uint32_t y,
                          uint32_t rect_width, uint32_t rect_height,
                          uint32_t color)
{
	uint32_t bytes_per_pixel = sdk_surface_format_bytes(format);
	uint32_t row;
	uint32_t col;

	if (!surface ||
	    !surface_rect_valid(width, height, pitch, bytes_per_pixel,
	                        x, y, rect_width, rect_height)) {
		return 0;
	}

	for (row = 0; row < rect_height; row++) {
		uint8_t *dst_row = surface + ((y + row) * pitch) +
		                   (x * bytes_per_pixel);

		for (col = 0; col < rect_width; col++) {
			write_pixel(dst_row + (col * bytes_per_pixel),
			            format, color);
		}
	}

	return 1;
}

int sdk_surface_copy_rect(uint8_t *dst, uint32_t dst_width,
                          uint32_t dst_height, uint32_t dst_pitch,
                          const uint8_t *src, uint32_t src_width,
                          uint32_t src_height, uint32_t src_pitch,
                          uint32_t format, uint32_t src_x,
                          uint32_t src_y, uint32_t dst_x,
                          uint32_t dst_y, uint32_t rect_width,
                          uint32_t rect_height)
{
	uint32_t bytes_per_pixel = sdk_surface_format_bytes(format);
	uint32_t row_bytes;
	uint32_t row;
	int bottom_up;

	if (!dst || !src ||
	    !surface_rect_valid(dst_width, dst_height, dst_pitch,
	                        bytes_per_pixel, dst_x, dst_y,
	                        rect_width, rect_height) ||
	    !surface_rect_valid(src_width, src_height, src_pitch,
	                        bytes_per_pixel, src_x, src_y,
	                        rect_width, rect_height)) {
		return 0;
	}

	row_bytes = rect_width * bytes_per_pixel;
	bottom_up = (dst == src && dst_y > src_y);

	for (row = 0; row < rect_height; row++) {
		uint32_t copy_row = bottom_up ? (rect_height - 1U - row) : row;
		const uint8_t *src_row =
			src + ((src_y + copy_row) * src_pitch) +
			(src_x * bytes_per_pixel);
		uint8_t *dst_row =
			dst + ((dst_y + copy_row) * dst_pitch) +
			(dst_x * bytes_per_pixel);

		memmove(dst_row, src_row, row_bytes);
	}

	return 1;
}

static uint8_t bilinear_channel(uint8_t p00, uint8_t p10,
                                uint8_t p01, uint8_t p11,
                                uint32_t wx, uint32_t wy)
{
	uint32_t ix = 256U - wx;
	uint32_t iy = 256U - wy;
	uint32_t value;

	value = ((uint32_t)p00 * ix * iy) +
	        ((uint32_t)p10 * wx * iy) +
	        ((uint32_t)p01 * ix * wy) +
	        ((uint32_t)p11 * wx * wy);
	return (uint8_t)((value + 32768U) >> 16);
}

static uint32_t scale_coord_8(uint32_t out_pos, uint32_t in_size,
                              uint32_t out_size)
{
	if (out_size <= 1U || in_size <= 1U)
		return 0U;

	return (uint32_t)(((uint64_t)out_pos * (in_size - 1U) * 256U) /
	                  (out_size - 1U));
}

static void scale_nearest_rect_clipped(uint8_t *dst, uint32_t dst_pitch,
                                       const uint8_t *src,
                                       uint32_t src_pitch,
                                       uint32_t bytes_per_pixel,
                                       uint32_t src_x, uint32_t src_y,
                                       uint32_t src_rect_width,
                                       uint32_t src_rect_height,
                                       uint32_t dst_x, uint32_t dst_y,
                                       uint32_t dst_rect_width,
                                       uint32_t dst_rect_height,
                                       uint32_t clip_x, uint32_t clip_y,
                                       uint32_t clip_width,
                                       uint32_t clip_height)
{
	uint32_t y;

	for (y = 0; y < clip_height; y++) {
		uint32_t dst_global_y = clip_y + y;
		uint32_t dst_local_y = dst_global_y - dst_y;
		uint32_t sample_y =
			src_y + (uint32_t)(((uint64_t)dst_local_y *
			                    src_rect_height) / dst_rect_height);
		uint8_t *dst_row =
			dst + (dst_global_y * dst_pitch) +
			(clip_x * bytes_per_pixel);
		uint32_t x;

		for (x = 0; x < clip_width; x++) {
			uint32_t dst_global_x = clip_x + x;
			uint32_t dst_local_x = dst_global_x - dst_x;
			uint32_t sample_x =
				src_x + (uint32_t)(((uint64_t)dst_local_x *
				                    src_rect_width) /
				                   dst_rect_width);
			const uint8_t *src_px =
				src + (sample_y * src_pitch) +
				(sample_x * bytes_per_pixel);
			uint8_t *dst_px = dst_row + (x * bytes_per_pixel);
			uint32_t b;

			for (b = 0; b < bytes_per_pixel; b++)
				dst_px[b] = src_px[b];
		}
	}
}

static void scale_nearest_rect(uint8_t *dst, uint32_t dst_pitch,
                               const uint8_t *src, uint32_t src_pitch,
                               uint32_t bytes_per_pixel,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t src_rect_width,
                               uint32_t src_rect_height,
                               uint32_t dst_x, uint32_t dst_y,
                               uint32_t dst_rect_width,
                               uint32_t dst_rect_height)
{
	scale_nearest_rect_clipped(dst, dst_pitch, src, src_pitch,
	                           bytes_per_pixel, src_x, src_y,
	                           src_rect_width, src_rect_height,
	                           dst_x, dst_y, dst_rect_width,
	                           dst_rect_height, dst_x, dst_y,
	                           dst_rect_width, dst_rect_height);
}

static void scale_bilinear_4byte_rect_clipped(
	uint8_t *dst, uint32_t dst_pitch, const uint8_t *src,
	uint32_t src_pitch, uint32_t src_x, uint32_t src_y,
	uint32_t src_rect_width, uint32_t src_rect_height,
	uint32_t dst_x, uint32_t dst_y, uint32_t dst_rect_width,
	uint32_t dst_rect_height, uint32_t clip_x, uint32_t clip_y,
	uint32_t clip_width, uint32_t clip_height)
{
	uint32_t y;

	for (y = 0; y < clip_height; y++) {
		uint32_t dst_global_y = clip_y + y;
		uint32_t dst_local_y = dst_global_y - dst_y;
		uint32_t sy_fp = scale_coord_8(dst_local_y, src_rect_height,
		                               dst_rect_height);
		uint32_t sy0 = src_y + (sy_fp >> 8);
		uint32_t sy1 = sy0;
		uint32_t wy = sy_fp & 0xffU;
		uint8_t *dst_row = dst + (dst_global_y * dst_pitch) +
		                    (clip_x * 4U);
		uint32_t x;

		if ((sy0 - src_y + 1U) < src_rect_height)
			sy1 = sy0 + 1U;

		for (x = 0; x < clip_width; x++) {
			uint32_t dst_global_x = clip_x + x;
			uint32_t dst_local_x = dst_global_x - dst_x;
			uint32_t sx_fp = scale_coord_8(dst_local_x,
			                               src_rect_width,
			                               dst_rect_width);
			uint32_t sx0 = src_x + (sx_fp >> 8);
			uint32_t sx1 = sx0;
			uint32_t wx = sx_fp & 0xffU;
			const uint8_t *p00;
			const uint8_t *p10;
			const uint8_t *p01;
			const uint8_t *p11;
			uint8_t *dst_px = dst_row + (x * 4U);
			uint32_t b;

			if ((sx0 - src_x + 1U) < src_rect_width)
				sx1 = sx0 + 1U;

			p00 = src + (sy0 * src_pitch) + (sx0 * 4U);
			p10 = src + (sy0 * src_pitch) + (sx1 * 4U);
			p01 = src + (sy1 * src_pitch) + (sx0 * 4U);
			p11 = src + (sy1 * src_pitch) + (sx1 * 4U);
			for (b = 0; b < 4U; b++) {
				dst_px[b] = bilinear_channel(p00[b], p10[b],
				                             p01[b], p11[b],
				                             wx, wy);
			}
		}
	}
}

static void scale_bilinear_4byte_rect(uint8_t *dst, uint32_t dst_pitch,
                                      const uint8_t *src, uint32_t src_pitch,
                                      uint32_t src_x, uint32_t src_y,
                                      uint32_t src_rect_width,
                                      uint32_t src_rect_height,
                                      uint32_t dst_x, uint32_t dst_y,
                                      uint32_t dst_rect_width,
                                      uint32_t dst_rect_height)
{
	scale_bilinear_4byte_rect_clipped(dst, dst_pitch, src, src_pitch,
	                                  src_x, src_y, src_rect_width,
	                                  src_rect_height, dst_x, dst_y,
	                                  dst_rect_width, dst_rect_height,
	                                  dst_x, dst_y, dst_rect_width,
	                                  dst_rect_height);
}

int sdk_surface_scale_rect(uint8_t *dst, uint32_t dst_width,
                           uint32_t dst_height, uint32_t dst_pitch,
                           const uint8_t *src, uint32_t src_width,
                           uint32_t src_height, uint32_t src_pitch,
                           uint32_t format, uint32_t src_x,
                           uint32_t src_y, uint32_t src_rect_width,
                           uint32_t src_rect_height, uint32_t dst_x,
                           uint32_t dst_y, uint32_t dst_rect_width,
                           uint32_t dst_rect_height, uint32_t filter)
{
	uint32_t bytes_per_pixel = sdk_surface_format_bytes(format);

	if (!dst || !src ||
	    !surface_rect_valid(src_width, src_height, src_pitch,
	                        bytes_per_pixel, src_x, src_y,
	                        src_rect_width, src_rect_height) ||
	    !surface_rect_valid(dst_width, dst_height, dst_pitch,
	                        bytes_per_pixel, dst_x, dst_y,
	                        dst_rect_width, dst_rect_height)) {
		return 0;
	}

	if (filter == SDK_SCALE_NEAREST) {
		scale_nearest_rect(dst, dst_pitch, src, src_pitch,
		                   bytes_per_pixel, src_x, src_y,
		                   src_rect_width, src_rect_height,
		                   dst_x, dst_y,
		                   dst_rect_width, dst_rect_height);
		return 1;
	}

	if (filter == SDK_SCALE_BILINEAR && bytes_per_pixel == 4U) {
		scale_bilinear_4byte_rect(dst, dst_pitch, src, src_pitch,
		                          src_x, src_y,
		                          src_rect_width, src_rect_height,
		                          dst_x, dst_y,
		                          dst_rect_width, dst_rect_height);
		return 1;
	}

	return 0;
}

int sdk_surface_scale_rect_clipped(
	uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
	uint32_t dst_pitch, const uint8_t *src, uint32_t src_width,
	uint32_t src_height, uint32_t src_pitch, uint32_t format,
	uint32_t src_x, uint32_t src_y, uint32_t src_rect_width,
	uint32_t src_rect_height, uint32_t dst_x, uint32_t dst_y,
	uint32_t dst_rect_width, uint32_t dst_rect_height,
	uint32_t clip_x, uint32_t clip_y, uint32_t clip_width,
	uint32_t clip_height, uint32_t filter)
{
	uint32_t bytes_per_pixel = sdk_surface_format_bytes(format);
	uint32_t visible_x;
	uint32_t visible_y;
	uint32_t visible_w;
	uint32_t visible_h;

	if (!dst || !src ||
	    !surface_rect_valid(src_width, src_height, src_pitch,
	                        bytes_per_pixel, src_x, src_y,
	                        src_rect_width, src_rect_height) ||
	    !surface_rect_valid(dst_width, dst_height, dst_pitch,
	                        bytes_per_pixel, dst_x, dst_y,
	                        dst_rect_width, dst_rect_height) ||
	    clip_width == 0U || clip_height == 0U) {
		return 0;
	}

	if (!intersect_rect(dst_x, dst_y, dst_rect_width, dst_rect_height,
	                    clip_x, clip_y, clip_width, clip_height,
	                    &visible_x, &visible_y, &visible_w, &visible_h)) {
		return 1;
	}
	if (!surface_rect_valid(dst_width, dst_height, dst_pitch,
	                        bytes_per_pixel, visible_x, visible_y,
	                        visible_w, visible_h)) {
		return 0;
	}

	if (filter == SDK_SCALE_NEAREST) {
		scale_nearest_rect_clipped(dst, dst_pitch, src, src_pitch,
		                           bytes_per_pixel, src_x, src_y,
		                           src_rect_width, src_rect_height,
		                           dst_x, dst_y, dst_rect_width,
		                           dst_rect_height, visible_x,
		                           visible_y, visible_w, visible_h);
		return 1;
	}

	if (filter == SDK_SCALE_BILINEAR && bytes_per_pixel == 4U) {
		scale_bilinear_4byte_rect_clipped(
			dst, dst_pitch, src, src_pitch, src_x, src_y,
			src_rect_width, src_rect_height, dst_x, dst_y,
			dst_rect_width, dst_rect_height, visible_x, visible_y,
			visible_w, visible_h);
		return 1;
	}

	return 0;
}
