/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * libjpeg-turbo backed JPEG decode helpers for SDK image services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_jpeg.h"
#include "sdk_surface.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "jpeglib.h"

#define SDK_JPEG_MAX_DIMENSION 4096U
#define SDK_JPEG_MAX_PIXELS    (2048U * 2048U)

struct SDKJpegErrorManager {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
};

static void sdk_jpeg_error_exit(j_common_ptr cinfo)
{
	struct SDKJpegErrorManager *err =
		(struct SDKJpegErrorManager *)cinfo->err;

	longjmp(err->jump, 1);
}

static void sdk_jpeg_output_message(j_common_ptr cinfo)
{
	(void)cinfo;
}

const char *sdk_jpeg_backend_name(void)
{
	return "libjpeg-turbo";
}

uint32_t sdk_jpeg_backend_flags(void)
{
	return SDK_JPEG_BACKEND_FLAG_BASELINE |
	       SDK_JPEG_BACKEND_FLAG_PROGRESSIVE |
	       SDK_JPEG_BACKEND_FLAG_DIRECT_BGRA |
	       SDK_JPEG_BACKEND_FLAG_SCALING |
	       SDK_JPEG_BACKEND_FLAG_DIRECT_RGB888;
}

uint32_t sdk_jpeg_service_flags(void)
{
	uint32_t backend_flags = sdk_jpeg_backend_flags();
	uint32_t service_flags = 0;

	if ((backend_flags & SDK_JPEG_BACKEND_FLAG_BASELINE) != 0U)
		service_flags |= SDK_SERVICE_FLAG_IMAGE_JPEG_BASELINE;
	if ((backend_flags & SDK_JPEG_BACKEND_FLAG_PROGRESSIVE) != 0U)
		service_flags |= SDK_SERVICE_FLAG_IMAGE_JPEG_PROGRESSIVE;
	if ((backend_flags & SDK_JPEG_BACKEND_FLAG_DIRECT_BGRA) != 0U)
		service_flags |= SDK_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA;
	if ((backend_flags & SDK_JPEG_BACKEND_FLAG_SCALING) != 0U)
		service_flags |= SDK_SERVICE_FLAG_IMAGE_JPEG_SCALING;
	if ((backend_flags & SDK_JPEG_BACKEND_FLAG_DIRECT_RGB888) != 0U)
		service_flags |= SDK_SERVICE_FLAG_IMAGE_RGB888_OUTPUT;

	return service_flags;
}

static int sdk_jpeg_output_color_space(uint32_t format,
                                       J_COLOR_SPACE *color_space)
{
	if (!color_space)
		return 0;

	switch (format) {
	case SDK_SURFACE_FORMAT_ARGB8888:
		*color_space = JCS_EXT_ARGB;
		return 1;
	case SDK_SURFACE_FORMAT_RGBA8888:
		*color_space = JCS_EXT_RGBA;
		return 1;
	case SDK_SURFACE_FORMAT_BGRA8888:
		*color_space = JCS_EXT_BGRA;
		return 1;
	case SDK_SURFACE_FORMAT_RGB888:
		*color_space = JCS_RGB;
		return 1;
	default:
		return 0;
	}
}

static int sdk_jpeg_destination_valid(uint32_t image_width,
                                      uint32_t image_height,
                                      uint32_t dst_width,
                                      uint32_t dst_height,
                                      uint32_t dst_pitch,
                                      uint32_t dst_x,
                                      uint32_t dst_y,
                                      uint32_t bytes_per_pixel)
{
	uint32_t row_bytes;
	uint32_t x_offset;

	if (image_width == 0U || image_height == 0U || dst_width == 0U ||
	    dst_height == 0U || bytes_per_pixel == 0U)
		return 0;
	if (image_width > SDK_JPEG_MAX_DIMENSION ||
	    image_height > SDK_JPEG_MAX_DIMENSION ||
	    image_width > (SDK_JPEG_MAX_PIXELS / image_height))
		return 0;
	if (dst_x > dst_width || dst_y > dst_height)
		return 0;
	if (image_width > (dst_width - dst_x) ||
	    image_height > (dst_height - dst_y))
		return 0;
	if (dst_width > (0xffffffffU / bytes_per_pixel) ||
	    image_width > (0xffffffffU / bytes_per_pixel) ||
	    dst_x > (0xffffffffU / bytes_per_pixel))
		return 0;

	row_bytes = image_width * bytes_per_pixel;
	x_offset = dst_x * bytes_per_pixel;
	if (dst_pitch < (dst_width * bytes_per_pixel))
		return 0;
	if (x_offset > dst_pitch || row_bytes > (dst_pitch - x_offset))
		return 0;

	return 1;
}

int sdk_jpeg_decode_to_surface(const uint8_t *jpeg, uint32_t jpeg_length,
                               uint8_t *dst, uint32_t dst_width,
                               uint32_t dst_height, uint32_t dst_pitch,
                               uint32_t dst_format, uint32_t dst_x,
                               uint32_t dst_y, uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_bytes_written)
{
	struct jpeg_decompress_struct cinfo;
	struct SDKJpegErrorManager jerr;
	J_COLOR_SPACE color_space;
	uint32_t bytes_per_pixel;
	uint32_t width;
	uint32_t height;
	volatile int created = 0;

	if (!jpeg || jpeg_length == 0U || !dst)
		return 0;
	if (!sdk_jpeg_output_color_space(dst_format, &color_space))
		return 0;

	bytes_per_pixel = sdk_surface_format_bytes(dst_format);
	if (bytes_per_pixel != 4U && bytes_per_pixel != 3U)
		return 0;

	memset(&cinfo, 0, sizeof(cinfo));
	memset(&jerr, 0, sizeof(jerr));
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = sdk_jpeg_error_exit;
	jerr.pub.output_message = sdk_jpeg_output_message;

	if (setjmp(jerr.jump)) {
		if (created)
			jpeg_destroy_decompress(&cinfo);
		return 0;
	}

	jpeg_create_decompress(&cinfo);
	created = 1;
	jpeg_mem_src(&cinfo, jpeg, jpeg_length);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&cinfo);
		return 0;
	}

	width = (uint32_t)cinfo.image_width;
	height = (uint32_t)cinfo.image_height;
	if (!sdk_jpeg_destination_valid(width, height, dst_width, dst_height,
	                                dst_pitch, dst_x, dst_y,
	                                bytes_per_pixel)) {
		jpeg_destroy_decompress(&cinfo);
		return 0;
	}

	cinfo.out_color_space = color_space;
	cinfo.dct_method = JDCT_IFAST;
	if (!jpeg_start_decompress(&cinfo) ||
	    cinfo.output_components != (int)bytes_per_pixel ||
	    cinfo.output_width != width || cinfo.output_height != height) {
		jpeg_destroy_decompress(&cinfo);
		return 0;
	}

	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW row = dst + ((dst_y + cinfo.output_scanline) *
		                      dst_pitch) + (dst_x * bytes_per_pixel);

		if (jpeg_read_scanlines(&cinfo, &row, 1U) != 1U) {
			jpeg_destroy_decompress(&cinfo);
			return 0;
		}
	}

	if (!jpeg_finish_decompress(&cinfo)) {
		jpeg_destroy_decompress(&cinfo);
		return 0;
	}
	jpeg_destroy_decompress(&cinfo);

	if (out_width)
		*out_width = width;
	if (out_height)
		*out_height = height;
	if (out_bytes_written)
		*out_bytes_written = width * height * bytes_per_pixel;

	return 1;
}
