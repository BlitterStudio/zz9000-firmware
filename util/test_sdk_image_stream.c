/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host-side checks for SDK firmware image stream session state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define main sdk_jpeg_vector_owner_main
#include "test_sdk_jpeg.c"
#undef main

#include "sdk_image_stream.h"
#include "jpeglib.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t png_2x2[] = {
	0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
	0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
	0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x02U,
	0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x72U, 0xb6U, 0x0dU,
	0x24U, 0x00U, 0x00U, 0x00U, 0x13U, 0x49U, 0x44U, 0x41U,
	0x54U, 0x78U, 0x9cU, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xf0U,
	0x1fU, 0x0cU, 0x19U, 0x18U, 0xfeU, 0x83U, 0x01U, 0x00U,
	0x49U, 0xc8U, 0x09U, 0xf7U, 0x14U, 0x63U, 0x32U, 0x9dU,
	0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U,
	0xaeU, 0x42U, 0x60U, 0x82U
};

static const uint8_t png_2x2_rgb[] = {
	0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
	0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
	0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x02U,
	0x08U, 0x02U, 0x00U, 0x00U, 0x00U, 0xfdU, 0xd4U, 0x9aU,
	0x73U, 0x00U, 0x00U, 0x00U, 0x14U, 0x49U, 0x44U, 0x41U,
	0x54U, 0x78U, 0xdaU, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xc0U,
	0x00U, 0xc2U, 0x0cU, 0xffU, 0xffU, 0xffU, 0xffU, 0x0fU,
	0x00U, 0x1fU, 0xeeU, 0x05U, 0xfbU, 0x60U, 0x6cU, 0x70U,
	0xf2U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU,
	0x44U, 0xaeU, 0x42U, 0x60U, 0x82U
};

static const uint8_t png_2x1_transparent[] = {
	0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
	0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
	0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U,
	0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xf4U, 0x22U, 0x7fU,
	0x8aU, 0x00U, 0x00U, 0x00U, 0x0eU, 0x49U, 0x44U, 0x41U,
	0x54U, 0x78U, 0xdaU, 0x63U, 0x60U, 0x00U, 0x81U, 0xffU,
	0x0cU, 0xffU, 0x01U, 0x04U, 0x05U, 0x01U, 0xffU, 0x3eU,
	0x68U, 0x83U, 0xa0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U,
	0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U
};

void sdk_image_stream_scale_row_bilinear_4byte(uint8_t *dst,
                                               const uint8_t *src,
                                               uint32_t src_width,
                                               uint32_t dst_width);
void sdk_image_stream_scale_rows_bilinear_4byte(uint8_t *dst,
                                                const uint8_t *row0,
                                                const uint8_t *row1,
                                                uint32_t src_width,
                                                uint32_t dst_width,
                                                uint32_t y_weight);

static int make_solid_rgb_jpeg(uint32_t width, uint32_t height,
                               unsigned char **out,
                               unsigned long *out_size)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	uint8_t *row;
	uint32_t x;

	if (!out || !out_size || width == 0U || height == 0U ||
	    width > (0xffffffffU / 3U)) {
		return 0;
	}
	*out = 0;
	*out_size = 0;
	row = (uint8_t *)malloc(width * 3U);
	if (!row)
		return 0;
	for (x = 0; x < width; x++) {
		row[(x * 3U) + 0U] = 0xa0U;
		row[(x * 3U) + 1U] = 0x40U;
		row[(x * 3U) + 2U] = 0x20U;
	}

	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, out, out_size);
	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, 80, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW scanline = row;

		if (jpeg_write_scanlines(&cinfo, &scanline, 1U) != 1U) {
			jpeg_destroy_compress(&cinfo);
			free(row);
			free(*out);
			*out = 0;
			*out_size = 0;
			return 0;
		}
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	free(row);
	return *out != 0 && *out_size != 0UL;
}

static int test_direct_scale_row_uses_bilinear_filter(void)
{
	static const uint8_t src[] = {
		0x00, 0x0a, 0x14, 0xff,
		0x64, 0x6e, 0x78, 0xff,
		0xc8, 0xd2, 0xdc, 0xff
	};
	static const uint8_t expected[] = {
		0x00, 0x0a, 0x14, 0xff,
		0x32, 0x3c, 0x46, 0xff,
		0x64, 0x6e, 0x78, 0xff,
		0x96, 0xa0, 0xaa, 0xff,
		0xc8, 0xd2, 0xdc, 0xff
	};
	uint8_t dst[sizeof(expected)];

	memset(dst, 0, sizeof(dst));
	sdk_image_stream_scale_row_bilinear_4byte(dst, src, 3U, 5U);
	if (memcmp(dst, expected, sizeof(expected)) != 0) {
		printf("bilinear row scale output mismatch\n");
		return 1;
	}

	return 0;
}

static int test_direct_scale_rows_use_vertical_bilinear_filter(void)
{
	static const uint8_t row0[] = {
		0x00, 0x00, 0x00, 0xff,
		0x64, 0x00, 0x00, 0xff
	};
	static const uint8_t row1[] = {
		0x00, 0x64, 0x00, 0xff,
		0x64, 0x64, 0x00, 0xff
	};
	static const uint8_t expected[] = {
		0x00, 0x32, 0x00, 0xff,
		0x32, 0x32, 0x00, 0xff,
		0x64, 0x32, 0x00, 0xff
	};
	uint8_t dst[sizeof(expected)];

	memset(dst, 0, sizeof(dst));
	sdk_image_stream_scale_rows_bilinear_4byte(dst, row0, row1,
	                                           2U, 3U, 128U);
	if (memcmp(dst, expected, sizeof(expected)) != 0) {
		printf("vertical bilinear row scale output mismatch\n");
		return 1;
	}

	return 0;
}

static int test_begin_feed_and_close_lifecycle(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t chunk[2];
	uint8_t tile[16U * 1280U];
	uint32_t session;

	sdk_image_stream_init();
	if (sdk_image_stream_active_count() != 0U)
		return 1;

	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 1280U;
	begin.tile_rows = 16U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);

	memset(&result, 0xff, sizeof(result));
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 2;
	if (result.session == 0U)
		return 3;
	if (result.state != SDK_IMAGE_SESSION_STATE_NEED_INPUT)
		return 4;
	if (result.output_format != SDK_SURFACE_FORMAT_BGRA8888)
		return 5;
	if (sdk_image_stream_active_count() != 1U)
		return 6;

	session = result.session;
	chunk[0] = 0xffU;
	chunk[1] = 0xd8U;
	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = sizeof(chunk);

	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, chunk, &result) != SDK_STATUS_OK)
		return 7;
	if (result.session != session)
		return 8;
	if (result.state != SDK_IMAGE_SESSION_STATE_NEED_INPUT)
		return 9;
	if (result.bytes_consumed != sizeof(chunk))
		return 10;

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 11;
	if (sdk_image_stream_active_count() != 0U)
		return 12;

	return 0;
}

static int test_validation_and_capacity(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t tile[16U * 1280U];
	uint32_t sessions[SDK_MAX_IMAGE_SESSIONS];
	uint32_t i;

	sdk_image_stream_init();
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_GIF;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 1280U;
	begin.tile_rows = 16U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);
	if (sdk_image_stream_begin(&begin, &result) !=
	    SDK_STATUS_UNSUPPORTED) {
		return 1;
	}

	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.tile_rows = 0U;
	if (sdk_image_stream_begin(&begin, &result) !=
	    SDK_STATUS_BAD_REQUEST) {
		return 2;
	}

	begin.tile_rows = 16U;
	for (i = 0; i < SDK_MAX_IMAGE_SESSIONS; i++) {
		if (sdk_image_stream_begin(&begin, &result) !=
		    SDK_STATUS_OK) {
			return 3;
		}
		sessions[i] = result.session;
	}
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_NO_MEMORY)
		return 4;

	for (i = 0; i < SDK_MAX_IMAGE_SESSIONS; i++) {
		if (sdk_image_stream_close(sessions[i]) != SDK_STATUS_OK)
			return 5;
	}

	if (sdk_image_stream_close(0x12345678UL) != SDK_STATUS_BAD_HANDLE)
		return 6;

	return 0;
}

static int test_png_stream_decodes_to_tile_buffer(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t tile[2U * 2U * 3U];
	uint32_t offset = 0U;
	uint32_t session;
	uint32_t tiles_seen = 0U;
	uint32_t guard = 0U;

	sdk_image_stream_init();
	memset(tile, 0, sizeof(tile));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_RGB888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 2U * 3U;
	begin.tile_rows = 2U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	while (result.state != SDK_IMAGE_SESSION_STATE_COMPLETE) {
		uint32_t remaining = (uint32_t)sizeof(png_2x2) - offset;
		uint16_t status;

		memset(&feed, 0, sizeof(feed));
		feed.session = session;
		feed.src_handle = 0x40000002UL;
		feed.src_length = remaining;
		feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
		memset(&result, 0, sizeof(result));
		status = sdk_image_stream_feed(&feed,
		                               remaining != 0U ?
		                               &png_2x2[offset] : 0,
		                               &result);
		if (status != SDK_STATUS_OK) {
			return 2;
		}
		if (result.bytes_consumed > remaining)
			return 3;
		offset += result.bytes_consumed;
		if (result.state == SDK_IMAGE_SESSION_STATE_TILE_READY) {
			if (result.output_format != SDK_SURFACE_FORMAT_RGB888 ||
			    result.tile_width != 2U ||
			    result.tile_height != 2U ||
			    result.bytes_written != sizeof(tile)) {
				return 4;
			}
			if (result.tile_y != 0U ||
			    !dominant_red_rgb888(&tile[0]) ||
			    !dominant_green_rgb888(&tile[3]) ||
			    !dominant_blue_rgb888(&tile[6]) ||
			    !near_white_rgb888(&tile[9])) {
				return 5;
			}
			tiles_seen++;
		} else if (result.state != SDK_IMAGE_SESSION_STATE_COMPLETE &&
		           result.state != SDK_IMAGE_SESSION_STATE_NEED_INPUT &&
		           result.state != SDK_IMAGE_SESSION_STATE_HEADER_READY) {
			return 8;
		}
		if (++guard > 8U)
			return 9;
	}
	if (tiles_seen != 1U ||
	    result.image_width != 2U ||
	    result.image_height != 2U ||
	    result.output_format != SDK_SURFACE_FORMAT_RGB888) {
		return 10;
	}
	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 11;
	return 0;
}

static int test_png_rgb_stream_copies_rgb888_tile_rows(void)
{
	static const uint8_t expected[] = {
		0xffU, 0x00U, 0x00U,
		0x00U, 0xffU, 0x00U,
		0x00U, 0x00U, 0xffU,
		0xffU, 0xffU, 0xffU
	};
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t tile[2U * 2U * 3U];
	uint32_t session;

	sdk_image_stream_init();
	memset(tile, 0, sizeof(tile));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_RGB888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 2U * 3U;
	begin.tile_rows = 2U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(png_2x2_rgb);
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, png_2x2_rgb, &result) != SDK_STATUS_OK)
		return 2;
	if (result.session != session ||
	    result.state != SDK_IMAGE_SESSION_STATE_TILE_READY) {
		return 3;
	}
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.output_format != SDK_SURFACE_FORMAT_RGB888 ||
	    result.tile_width != 2U || result.tile_height != 2U ||
	    result.bytes_written != sizeof(tile)) {
		return 4;
	}
	if (memcmp(tile, expected, sizeof(expected)) != 0)
		return 5;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, 0, &result) != SDK_STATUS_OK)
		return 6;
	if (result.state != SDK_IMAGE_SESSION_STATE_COMPLETE)
		return 7;

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 8;

	return 0;
}

static int test_png_stream_decodes_to_direct_framebuffer(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[2U * 2U * 4U];
	uint32_t session;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 2U;
	begin.dst_height = 2U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 2U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(png_2x2);
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, png_2x2, &result) != SDK_STATUS_OK)
		return 2;
	if (result.session != session ||
	    result.state != SDK_IMAGE_SESSION_STATE_COMPLETE) {
		return 3;
	}
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.tile_width != 2U || result.tile_height != 2U ||
	    result.bytes_written != sizeof(framebuffer)) {
		return 4;
	}
	if (!dominant_red(&framebuffer[0]) ||
	    !dominant_green(&framebuffer[4]) ||
	    !dominant_blue(&framebuffer[8]) ||
	    !near_white(&framebuffer[12])) {
		return 5;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 6;

	return 0;
}

static int test_png_stream_decodes_to_rgb888_surface(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t surface[2U * 2U * 3U];
	uint32_t session;

	sdk_image_stream_init();
	memset(surface, 0, sizeof(surface));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_SURFACE;
	begin.dst_surface = 0x40000003UL;
	begin.dst_width = 2U;
	begin.dst_height = 2U;
	begin.output_format = SDK_SURFACE_FORMAT_RGB888;
	begin.dst_address = (uintptr_t)surface;
	begin.dst_pitch = 2U * 3U;
	begin.dst_length = sizeof(surface);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(png_2x2);
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, png_2x2, &result) != SDK_STATUS_OK)
		return 2;
	if (result.session != session ||
	    result.state != SDK_IMAGE_SESSION_STATE_COMPLETE) {
		return 3;
	}
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.output_format != SDK_SURFACE_FORMAT_RGB888 ||
	    result.tile_width != 2U || result.tile_height != 2U ||
	    result.bytes_written != sizeof(surface)) {
		return 4;
	}
	if (!dominant_red_rgb888(&surface[0]) ||
	    !dominant_green_rgb888(&surface[3]) ||
	    !dominant_blue_rgb888(&surface[6]) ||
	    !near_white_rgb888(&surface[9])) {
		return 5;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 6;

	return 0;
}

static int test_png_stream_preserves_bgra_alpha(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t surface[2U * 1U * 4U];
	uint32_t session;

	sdk_image_stream_init();
	memset(surface, 0xff, sizeof(surface));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_SURFACE;
	begin.dst_surface = 0x40000003UL;
	begin.dst_width = 2U;
	begin.dst_height = 1U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.dst_address = (uintptr_t)surface;
	begin.dst_pitch = 2U * 4U;
	begin.dst_length = sizeof(surface);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(png_2x1_transparent);
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, png_2x1_transparent,
	                          &result) != SDK_STATUS_OK)
		return 2;
	if (result.session != session ||
	    result.state != SDK_IMAGE_SESSION_STATE_COMPLETE) {
		return 3;
	}
	if (result.image_width != 2U || result.image_height != 1U ||
	    result.output_format != SDK_SURFACE_FORMAT_BGRA8888 ||
	    result.bytes_written != sizeof(surface)) {
		return 4;
	}
	if (surface[0] != 0U || surface[1] != 0U ||
	    surface[2] != 0U || surface[3] != 0U ||
	    !dominant_green(&surface[4])) {
		return 5;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 6;

	return 0;
}

static int test_png_stream_accepts_sdk_staging_feed_limit(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[2U * 2U * 4U];
	static uint8_t max_input[256U * 1024U];
	static uint8_t oversized_input[(256U * 1024U) + 1U];
	uint32_t session;
	uint16_t status;

	if (SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES != (256U * 1024U))
		return 1;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(max_input, 0, sizeof(max_input));
	memset(oversized_input, 0, sizeof(oversized_input));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_PNG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 2U;
	begin.dst_height = 2U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 2U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(max_input);
	status = sdk_image_stream_feed(&feed, max_input, &result);
	if (status == SDK_STATUS_BAD_REQUEST) {
		sdk_image_stream_close(session);
		return 2;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 3;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 4;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(oversized_input);
	status = sdk_image_stream_feed(&feed, oversized_input, &result);
	if (status != SDK_STATUS_BAD_REQUEST) {
		sdk_image_stream_close(session);
		return 5;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 6;

	return 0;
}

static int test_jpeg_stream_decodes_to_tile_buffer(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t tile[2U * 2U * 4U];
	uint32_t consumed;
	uint32_t session;

	sdk_image_stream_init();
	memset(tile, 0, sizeof(tile));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 2U * 4U;
	begin.tile_rows = 2U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(jpeg_2x2);
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	{
		uint16_t feed_status = sdk_image_stream_feed(&feed, jpeg_2x2,
		                                             &result);

		if (feed_status != SDK_STATUS_OK) {
			printf("tile feed status: %u\n", feed_status);
			return 2;
		}
	}
	if (result.session != session ||
	    result.state != SDK_IMAGE_SESSION_STATE_TILE_READY) {
		return 3;
	}
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.tile_width != 2U || result.tile_height != 2U) {
		return 4;
	}
	if (!dominant_red(&tile[0]) || !dominant_green(&tile[4]) ||
	    !dominant_blue(&tile[8]) || !near_white(&tile[12])) {
		return 5;
	}

	consumed = result.bytes_consumed;
	if (consumed > sizeof(jpeg_2x2))
		return 6;
	feed.src_offset = consumed;
	feed.src_length = (uint32_t)sizeof(jpeg_2x2) - consumed;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed,
	                          feed.src_length != 0U ?
	                          &jpeg_2x2[consumed] : 0,
	                          &result) != SDK_STATUS_OK) {
		return 7;
	}
	if (result.state != SDK_IMAGE_SESSION_STATE_COMPLETE ||
	    result.image_width != 2U || result.image_height != 2U) {
		return 8;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 9;

	return 0;
}

static int test_jpeg_stream_decodes_rgb888_to_tile_buffer(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamFeed feed;
	struct SDKImageStreamResult result;
	uint8_t tile[2U * 2U * 3U];
	uint32_t session;

	sdk_image_stream_init();
	memset(tile, 0, sizeof(tile));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_RGB888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 2U * 3U;
	begin.tile_rows = 2U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	memset(&feed, 0, sizeof(feed));
	feed.session = session;
	feed.src_handle = 0x40000002UL;
	feed.src_length = (uint32_t)sizeof(jpeg_2x2);
	feed.flags = SDK_IMAGE_SESSION_FEED_EOF;
	memset(&result, 0, sizeof(result));
	if (sdk_image_stream_feed(&feed, jpeg_2x2, &result) != SDK_STATUS_OK)
		return 2;
	if (result.session != session ||
	    result.state != SDK_IMAGE_SESSION_STATE_TILE_READY) {
		return 3;
	}
	if (result.output_format != SDK_SURFACE_FORMAT_RGB888 ||
	    result.tile_width != 2U || result.tile_height != 2U ||
	    result.bytes_written != sizeof(tile)) {
		return 4;
	}
	if (!dominant_red_rgb888(&tile[0])) {
		return 5;
	}

	{
		uint32_t consumed = result.bytes_consumed;

		if (consumed > sizeof(jpeg_2x2))
			return 6;
		feed.src_offset = consumed;
		feed.src_length = (uint32_t)sizeof(jpeg_2x2) - consumed;
		memset(&result, 0, sizeof(result));
		if (sdk_image_stream_feed(&feed,
		                          feed.src_length != 0U ?
		                          &jpeg_2x2[consumed] : 0,
		                          &result) != SDK_STATUS_OK) {
			return 7;
		}
		if (result.state != SDK_IMAGE_SESSION_STATE_COMPLETE ||
		    result.output_format != SDK_SURFACE_FORMAT_RGB888 ||
		    result.bytes_written != sizeof(tile)) {
			return 8;
		}
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 9;

	return 0;
}

static int feed_stream_with_sliding_buffer(uint32_t session,
                                           const uint8_t *jpeg,
                                           uint32_t jpeg_length,
                                           uint32_t chunk_size,
                                           struct SDKImageStreamResult *result,
                                           uint32_t *states_seen)
{
	struct SDKImageStreamFeed feed;
	uint8_t staging[512U];
	uint32_t file_offset = 0U;
	uint32_t buffered = 0U;
	uint32_t guard = 0U;
	uint32_t consumed;
	int eof;

	memset(result, 0, sizeof(*result));
	if (states_seen)
		*states_seen = 0U;

	while (result->state != SDK_IMAGE_SESSION_STATE_COMPLETE) {
		while (file_offset < jpeg_length && buffered < sizeof(staging)) {
			uint32_t want = jpeg_length - file_offset;
			uint32_t room = (uint32_t)sizeof(staging) - buffered;

			if (want > chunk_size)
				want = chunk_size;
			if (want > room)
				want = room;
			memcpy(&staging[buffered], &jpeg[file_offset], want);
			file_offset += want;
			buffered += want;
		}
		eof = file_offset == jpeg_length;

		memset(&feed, 0, sizeof(feed));
		feed.session = session;
		feed.src_handle = 0x40000002UL;
		feed.src_length = buffered;
		feed.flags = eof ? SDK_IMAGE_SESSION_FEED_EOF : 0U;

		if (sdk_image_stream_feed(&feed,
		                          buffered != 0U ? staging : 0,
		                          result) != SDK_STATUS_OK) {
			return 1;
		}
		if (states_seen && result->state < 32U)
			*states_seen |= (1UL << result->state);
		if (result->bytes_consumed > buffered)
			return 2;

		consumed = result->bytes_consumed;
		if (consumed != 0U) {
			buffered -= consumed;
			if (buffered != 0U)
				memmove(staging, &staging[consumed], buffered);
		} else if (result->state != SDK_IMAGE_SESSION_STATE_COMPLETE &&
		           result->bytes_written == 0U) {
			if (eof || buffered == sizeof(staging))
				return 3;
		}

		if (++guard > 512U)
			return 4;
	}

	return 0;
}

static int feed_stream_with_full_buffer(uint32_t session,
                                        const uint8_t *jpeg,
                                        uint32_t jpeg_length,
                                        struct SDKImageStreamResult *result,
                                        uint32_t *direct_progress_count)
{
	struct SDKImageStreamFeed feed;
	uint32_t offset = 0U;
	uint32_t guard = 0U;

	memset(result, 0, sizeof(*result));
	if (direct_progress_count)
		*direct_progress_count = 0U;

	while (result->state != SDK_IMAGE_SESSION_STATE_COMPLETE) {
		uint32_t remaining = jpeg_length - offset;

		memset(&feed, 0, sizeof(feed));
		feed.session = session;
		feed.src_handle = 0x40000002UL;
		feed.src_length = remaining;
		feed.flags = SDK_IMAGE_SESSION_FEED_EOF;

		if (sdk_image_stream_feed(&feed,
		                          remaining != 0U ? &jpeg[offset] : 0,
		                          result) != SDK_STATUS_OK) {
			return 1;
		}
		if (result->bytes_consumed > remaining)
			return 2;

		offset += result->bytes_consumed;
		if (result->state == SDK_IMAGE_SESSION_STATE_NEED_INPUT &&
		    result->bytes_written != 0U) {
			if (direct_progress_count)
				(*direct_progress_count)++;
		} else if (result->state != SDK_IMAGE_SESSION_STATE_COMPLETE &&
		           result->bytes_consumed == 0U &&
		           result->bytes_written == 0U) {
			return 3;
		}

		if (++guard > 512U)
			return 4;
	}

	return 0;
}

static int test_jpeg_stream_decodes_chunked_to_tile_buffer(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t tile[2U * 2U * 4U];
	uint32_t states_seen;
	uint32_t session;

	sdk_image_stream_init();
	memset(tile, 0, sizeof(tile));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_TILE_BUFFER;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.tile_handle = 0x40000001UL;
	begin.tile_stride = 2U * 4U;
	begin.tile_rows = 2U;
	begin.tile_address = (uintptr_t)tile;
	begin.tile_length = sizeof(tile);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	result = (struct SDKImageStreamResult){0};
	{
		int feed_result;

		feed_result = feed_stream_with_sliding_buffer(
			session, jpeg_2x2, (uint32_t)sizeof(jpeg_2x2),
			17U, &result, &states_seen);
		if (feed_result != 0)
			return 20 + feed_result;
	}
	if ((states_seen & (1UL << SDK_IMAGE_SESSION_STATE_TILE_READY)) == 0U)
		return 3;
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.bytes_written != sizeof(tile)) {
		return 4;
	}
	if (!dominant_red(&tile[0]) || !dominant_green(&tile[4]) ||
	    !dominant_blue(&tile[8]) || !near_white(&tile[12])) {
		return 5;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 6;

	return 0;
}

static int test_jpeg_stream_decodes_chunked_to_direct_framebuffer(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[2U * 2U * 4U];
	uint32_t session;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 2U;
	begin.dst_height = 2U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 2U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	result = (struct SDKImageStreamResult){0};
	{
		int feed_result;

		feed_result = feed_stream_with_sliding_buffer(
			session, jpeg_2x2, (uint32_t)sizeof(jpeg_2x2),
			17U, &result, 0);
		if (feed_result != 0)
			return 20 + feed_result;
	}
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.bytes_written != sizeof(framebuffer)) {
		return 3;
	}
	if (!dominant_red(&framebuffer[0]) ||
	    !dominant_green(&framebuffer[4]) ||
	    !dominant_blue(&framebuffer[8]) ||
	    !near_white(&framebuffer[12])) {
		return 4;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 5;

	return 0;
}

static int test_jpeg_stream_dct_scales_direct_framebuffer_to_fit(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[1U * 1U * 4U];
	uint32_t session;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 1U;
	begin.dst_height = 1U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.flags = SDK_IMAGE_DECODE_FLAG_FIT |
	              SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 1U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK)
		return 1;
	session = result.session;

	result = (struct SDKImageStreamResult){0};
	{
		int feed_result;

		feed_result = feed_stream_with_sliding_buffer(
			session, jpeg_2x2, (uint32_t)sizeof(jpeg_2x2),
			17U, &result, 0);
		if (feed_result != 0)
			return 20 + feed_result;
	}
	if (result.image_width != 2U || result.image_height != 2U ||
	    result.tile_width != 1U || result.tile_height != 1U ||
	    result.bytes_written != sizeof(framebuffer)) {
		return 3;
	}
	if ((result.flags & SDK_IMAGE_SESSION_RESULT_SCALED) == 0U)
		return 4;
	if (framebuffer[3] != 0xffU)
		return 5;

	if (sdk_image_stream_close(session) != SDK_STATUS_OK)
		return 6;

	return 0;
}

static int test_jpeg_stream_fit_accepts_oversized_source_dimensions(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[1024U * 16U * 4U];
	unsigned char *jpeg = 0;
	unsigned long jpeg_size = 0UL;
	uint32_t session;

	if (!make_solid_rgb_jpeg(4097U, 2U, &jpeg, &jpeg_size))
		return 1;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 1024U;
	begin.dst_height = 16U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.flags = SDK_IMAGE_DECODE_FLAG_FIT |
	              SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 1024U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK) {
		free(jpeg);
		return 2;
	}
	session = result.session;

	result = (struct SDKImageStreamResult){0};
	{
		int feed_result;

		feed_result = feed_stream_with_sliding_buffer(
			session, jpeg, (uint32_t)jpeg_size, 257U, &result, 0);
		if (feed_result != 0) {
			sdk_image_stream_close(session);
			free(jpeg);
			return 20 + feed_result;
		}
	}
	if (result.image_width != 4097U || result.image_height != 2U ||
	    result.tile_width == 0U || result.tile_width > 1024U ||
	    result.tile_height == 0U || result.tile_height > 16U ||
	    result.bytes_written != result.tile_width * result.tile_height * 4U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 3;
	}
	if ((result.flags & SDK_IMAGE_SESSION_RESULT_SCALED) == 0U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 4;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK) {
		free(jpeg);
		return 5;
	}
	free(jpeg);
	return 0;
}

static int test_jpeg_stream_fit_scales_beyond_libjpeg_eighth_limit(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[1024U * 16U * 4U];
	unsigned char *jpeg = 0;
	unsigned long jpeg_size = 0UL;
	uint32_t session;

	if (!make_solid_rgb_jpeg(20000U, 2U, &jpeg, &jpeg_size))
		return 1;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 1024U;
	begin.dst_height = 16U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.flags = SDK_IMAGE_DECODE_FLAG_FIT |
	              SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 1024U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK) {
		free(jpeg);
		return 2;
	}
	session = result.session;

	result = (struct SDKImageStreamResult){0};
	{
		int feed_result;

		feed_result = feed_stream_with_sliding_buffer(
			session, jpeg, (uint32_t)jpeg_size, 1024U, &result, 0);
		if (feed_result != 0) {
			sdk_image_stream_close(session);
			free(jpeg);
			return 20 + feed_result;
		}
	}
	if (result.image_width != 20000U || result.image_height != 2U ||
	    result.tile_width != 1024U || result.tile_height != 1U ||
	    result.bytes_written != 1024U * 4U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 3;
	}
	if ((result.flags & SDK_IMAGE_SESSION_RESULT_SCALED) == 0U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 4;
	}
	if (framebuffer[3] != 0xffU) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 5;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK) {
		free(jpeg);
		return 6;
	}
	free(jpeg);
	return 0;
}

static int test_jpeg_stream_direct_scaled_output_is_sliced(void)
{
	struct SDKImageStreamBegin begin;
	struct SDKImageStreamResult result;
	uint8_t framebuffer[128U * 96U * 4U];
	unsigned char *jpeg = 0;
	unsigned long jpeg_size = 0UL;
	uint32_t direct_progress_count = 0U;
	uint32_t session;

	if (!make_solid_rgb_jpeg(300U, 200U, &jpeg, &jpeg_size))
		return 1;

	sdk_image_stream_init();
	memset(framebuffer, 0, sizeof(framebuffer));
	memset(&begin, 0, sizeof(begin));
	begin.codec = SDK_IMAGE_CODEC_JPEG;
	begin.output_mode = SDK_IMAGE_OUTPUT_FRAMEBUFFER;
	begin.dst_surface = SDK_SURFACE_HANDLE_FRAMEBUFFER;
	begin.dst_width = 128U;
	begin.dst_height = 96U;
	begin.output_format = SDK_SURFACE_FORMAT_BGRA8888;
	begin.flags = SDK_IMAGE_DECODE_FLAG_FIT |
	              SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
	begin.dst_address = (uintptr_t)framebuffer;
	begin.dst_pitch = 128U * 4U;
	begin.dst_length = sizeof(framebuffer);
	if (sdk_image_stream_begin(&begin, &result) != SDK_STATUS_OK) {
		free(jpeg);
		return 2;
	}
	session = result.session;

	result = (struct SDKImageStreamResult){0};
	if (feed_stream_with_full_buffer(session, jpeg, (uint32_t)jpeg_size,
	                                 &result,
	                                 &direct_progress_count) != 0) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 3;
	}
	if (direct_progress_count == 0U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 4;
	}
	if (result.image_width != 300U || result.image_height != 200U ||
	    result.tile_width != 128U || result.tile_height != 85U ||
	    result.bytes_written != 128U * 85U * 4U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 5;
	}
	if ((result.flags & SDK_IMAGE_SESSION_RESULT_SCALED) == 0U) {
		sdk_image_stream_close(session);
		free(jpeg);
		return 6;
	}

	if (sdk_image_stream_close(session) != SDK_STATUS_OK) {
		free(jpeg);
		return 7;
	}
	free(jpeg);
	return 0;
}

int main(void)
{
	int result;

	result = test_direct_scale_row_uses_bilinear_filter();
	if (result) {
		printf("direct row scaler failed: %d\n", result);
		return 5 + result;
	}

	result = test_direct_scale_rows_use_vertical_bilinear_filter();
	if (result) {
		printf("direct two-row scaler failed: %d\n", result);
		return 7 + result;
	}

	result = test_begin_feed_and_close_lifecycle();
	if (result) {
		printf("lifecycle failed: %d\n", result);
		return 10 + result;
	}

	result = test_validation_and_capacity();
	if (result) {
		printf("validation failed: %d\n", result);
		return 40 + result;
	}

	result = test_jpeg_stream_decodes_to_tile_buffer();
	if (result) {
		printf("jpeg stream failed: %d\n", result);
		return 70 + result;
	}

	result = test_jpeg_stream_decodes_rgb888_to_tile_buffer();
	if (result) {
		printf("rgb888 tile jpeg stream failed: %d\n", result);
		return 75 + result;
	}

	result = test_png_stream_decodes_to_tile_buffer();
	if (result) {
		printf("png tile stream failed: %d\n", result);
		return 80 + result;
	}

	result = test_png_rgb_stream_copies_rgb888_tile_rows();
	if (result) {
		printf("png rgb888 tile stream failed: %d\n", result);
		return 82 + result;
	}

	result = test_png_stream_decodes_to_direct_framebuffer();
	if (result) {
		printf("png direct stream failed: %d\n", result);
		return 85 + result;
	}

	result = test_png_stream_decodes_to_rgb888_surface();
	if (result) {
		printf("png rgb888 surface stream failed: %d\n", result);
		return 87 + result;
	}

	result = test_png_stream_preserves_bgra_alpha();
	if (result) {
		printf("png bgra alpha preservation failed: %d\n", result);
		return 89 + result;
	}

	result = test_png_stream_accepts_sdk_staging_feed_limit();
	if (result) {
		printf("png feed limit failed: %d\n", result);
		return 91 + result;
	}

	result = test_jpeg_stream_decodes_chunked_to_tile_buffer();
	if (result) {
		printf("chunked tile jpeg stream failed: %d\n", result);
		return 90 + result;
	}

	result = test_jpeg_stream_decodes_chunked_to_direct_framebuffer();
	if (result) {
		printf("chunked framebuffer jpeg stream failed: %d\n", result);
		return 110 + result;
	}

	result = test_jpeg_stream_dct_scales_direct_framebuffer_to_fit();
	if (result) {
		printf("scaled framebuffer jpeg stream failed: %d\n", result);
		return 130 + result;
	}

	result = test_jpeg_stream_fit_accepts_oversized_source_dimensions();
	if (result) {
		printf("oversized fit jpeg stream failed: %d\n", result);
		return 150 + result;
	}

	result = test_jpeg_stream_fit_scales_beyond_libjpeg_eighth_limit();
	if (result) {
		printf("eighth-limit fit jpeg stream failed: %d\n", result);
		return 180 + result;
	}

	result = test_jpeg_stream_direct_scaled_output_is_sliced();
	if (result) {
		printf("sliced direct scaled jpeg stream failed: %d\n", result);
		return 200 + result;
	}

	return 0;
}
