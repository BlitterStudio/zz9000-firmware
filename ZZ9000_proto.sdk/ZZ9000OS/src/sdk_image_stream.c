/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Streaming image session state for SDK image services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_image_stream.h"
#include "sdk_surface.h"
#include "sdk_compression.h"
#include "memorymap.h"
#include <xil_cache.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jpeglib.h"
#include "png.h"

#define SDK_IMAGE_STREAM_MAX_DIMENSION 4096U
#define SDK_IMAGE_STREAM_MAX_PIXELS    (2048U * 2048U)
#define SDK_IMAGE_STREAM_MAX_FIT_SOURCE_PIXELS (256U * 1024U * 1024U)
#define SDK_IMAGE_STREAM_MAX_DECODE_WIDTH 8192U
#define SDK_IMAGE_STREAM_DIRECT_SCALE_ROWS_PER_FEED 64U

struct SDKImageStreamJpegErrorManager {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
};

struct SDKImageStreamSession {
	uint32_t session;
	uint32_t codec;
	uint32_t output_mode;
	uint32_t dst_surface;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t output_format;
	uint32_t tile_handle;
	uint32_t tile_stride;
	uint32_t tile_rows;
	uint32_t flags;
	uintptr_t dst_address;
	uint32_t dst_pitch;
	uint32_t dst_length;
	uintptr_t tile_address;
	uint32_t tile_length;
	uint32_t compressed_bytes;
	uint32_t image_width;
	uint32_t image_height;
	uint32_t decode_width;
	uint32_t decode_height;
	uint32_t output_width;
	uint32_t output_height;
	uint32_t scaled_next_y;
	uint32_t rows_output;
	uint32_t png_rows_this_feed;
	uint32_t png_write_start_y;
	uint32_t png_interlace_rowbytes;
	uint32_t png_interlace_buffer_length;
	uint16_t png_error_status;
	struct jpeg_decompress_struct cinfo;
	struct SDKImageStreamJpegErrorManager jerr;
	struct jpeg_source_mgr source;
	png_structp png_ptr;
	png_infop png_info;
	uint8_t *png_interlace_buffer;
	uint8_t eoi_buffer[2];
	uint8_t jpeg_created;
	uint8_t png_created;
	uint8_t png_complete;
	uint8_t png_row_rgb888;
	uint8_t png_interlaced;
	uint8_t png_interlace_direct;
	uint8_t header_ready;
	uint8_t output_prepared;
	uint8_t started;
	uint8_t input_eof;
	uint8_t failed;
	uint8_t in_use;
	uint8_t core1_affine;
	uint8_t direct_scale_row_valid[2];
	uint32_t direct_scale_row_y[2];
	uint8_t direct_scale_rows[2][SDK_IMAGE_STREAM_MAX_DECODE_WIDTH * 4U];
};

/*
 * The session table lives INSIDE the SCU-coherent scheduler region
 * (SDK_IMAGE_SESSIONS_ADDRESS, memorymap.h): core-1-affine sessions decode
 * on the worker while core 0 creates/validates/destroys them, and the
 * coherent section makes session state visible to both cores without
 * manual cache maintenance. The libjpeg/libpng objects hang off the newlib
 * heap but are only ever touched by the session's owning core (affinity is
 * fixed at begin), with fault reclaim via the decode-tracking wrappers and
 * sdk_image_stream_poison_core1_sessions().
 */
static struct SDKImageStreamSession *const image_sessions =
    (struct SDKImageStreamSession *)SDK_IMAGE_SESSIONS_ADDRESS;

typedef char image_sessions_fit_check[
    (SDK_MAX_IMAGE_SESSIONS * sizeof(struct SDKImageStreamSession) <=
     SDK_IMAGE_SESSIONS_MAX_BYTES) ? 1 : -1];

static uint32_t next_image_session_id;

static struct SDKImageStreamSession *find_session(uint32_t session)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_IMAGE_SESSIONS; i++) {
		if (image_sessions[i].in_use &&
		    image_sessions[i].session == session) {
			return &image_sessions[i];
		}
	}

	return 0;
}

static struct SDKImageStreamSession *find_free_session(void)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_IMAGE_SESSIONS; i++) {
		if (!image_sessions[i].in_use)
			return &image_sessions[i];
	}

	return 0;
}

static uint32_t next_session_id(void)
{
	uint32_t session = next_image_session_id++;

	if (next_image_session_id == 0U || next_image_session_id == 0xffffffffU)
		next_image_session_id = 1U;
	if (session == 0U || session == 0xffffffffU)
		session = next_session_id();

	return session;
}

static void jpeg_error_exit(j_common_ptr cinfo)
{
	struct SDKImageStreamJpegErrorManager *err =
		(struct SDKImageStreamJpegErrorManager *)cinfo->err;

	longjmp(err->jump, 1);
}

static void jpeg_output_message(j_common_ptr cinfo)
{
	(void)cinfo;
}

static void stream_init_source(j_decompress_ptr cinfo)
{
	(void)cinfo;
}

static boolean stream_fill_input_buffer(j_decompress_ptr cinfo)
{
	struct SDKImageStreamSession *session =
		(struct SDKImageStreamSession *)cinfo->client_data;

	if (!session || !session->input_eof)
		return FALSE;

	session->eoi_buffer[0] = 0xffU;
	session->eoi_buffer[1] = JPEG_EOI;
	cinfo->src->next_input_byte = session->eoi_buffer;
	cinfo->src->bytes_in_buffer = sizeof(session->eoi_buffer);
	return TRUE;
}

static void stream_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
	if (num_bytes <= 0)
		return;

	while (num_bytes > (long)cinfo->src->bytes_in_buffer) {
		num_bytes -= (long)cinfo->src->bytes_in_buffer;
		cinfo->src->next_input_byte += cinfo->src->bytes_in_buffer;
		cinfo->src->bytes_in_buffer = 0;
		if (!stream_fill_input_buffer(cinfo))
			return;
	}

	cinfo->src->next_input_byte += num_bytes;
	cinfo->src->bytes_in_buffer -= num_bytes;
}

static void stream_term_source(j_decompress_ptr cinfo)
{
	(void)cinfo;
}

static int output_format_supported(uint32_t format)
{
	return format == SDK_SURFACE_FORMAT_ARGB8888 ||
	       format == SDK_SURFACE_FORMAT_RGBA8888 ||
	       format == SDK_SURFACE_FORMAT_BGRA8888 ||
	       format == SDK_SURFACE_FORMAT_RGB888;
}

static int output_color_space(uint32_t format, J_COLOR_SPACE *color_space)
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

static uint16_t validate_begin(const struct SDKImageStreamBegin *begin)
{
	uint32_t tile_length;
	uint32_t allowed_flags;

	if (!begin)
		return SDK_STATUS_BAD_REQUEST;
	allowed_flags = SDK_IMAGE_DECODE_FLAG_FIT |
	                SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
	if ((begin->flags & ~allowed_flags) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (begin->codec != SDK_IMAGE_CODEC_JPEG &&
	    begin->codec != SDK_IMAGE_CODEC_PNG) {
		if (begin->codec == SDK_IMAGE_CODEC_GIF) {
			return SDK_STATUS_UNSUPPORTED;
		}
		return SDK_STATUS_BAD_REQUEST;
	}
	if (begin->codec == SDK_IMAGE_CODEC_PNG &&
	    (begin->flags & SDK_IMAGE_DECODE_FLAG_FIT) != 0U) {
		return SDK_STATUS_UNSUPPORTED;
	}
	if (!output_format_supported(begin->output_format))
		return SDK_STATUS_UNSUPPORTED;
	if (begin->output_mode == SDK_IMAGE_OUTPUT_SURFACE) {
		if (begin->dst_surface == SDK_INVALID_HANDLE ||
		    begin->dst_width == 0U || begin->dst_height == 0U ||
		    begin->dst_address == 0U || begin->dst_pitch == 0U) {
			return SDK_STATUS_BAD_REQUEST;
		}
	} else if (begin->output_mode == SDK_IMAGE_OUTPUT_FRAMEBUFFER) {
		if (begin->dst_surface != SDK_SURFACE_HANDLE_FRAMEBUFFER ||
		    begin->dst_width == 0U || begin->dst_height == 0U ||
		    begin->dst_address == 0U || begin->dst_pitch == 0U) {
			return SDK_STATUS_BAD_REQUEST;
		}
	} else if (begin->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		if ((begin->flags & SDK_IMAGE_DECODE_FLAG_FIT) != 0U)
			return SDK_STATUS_UNSUPPORTED;
		if (begin->tile_handle == SDK_INVALID_HANDLE ||
		    begin->tile_stride == 0U || begin->tile_rows == 0U ||
		    begin->tile_address == 0U || begin->tile_length == 0U) {
			return SDK_STATUS_BAD_REQUEST;
		}
		if (begin->tile_stride > (0xffffffffU / begin->tile_rows)) {
			return SDK_STATUS_BAD_REQUEST;
		}
		tile_length = begin->tile_stride * begin->tile_rows;
		if (tile_length > begin->tile_length) {
			return SDK_STATUS_BAD_REQUEST;
		}
	} else {
		return SDK_STATUS_BAD_REQUEST;
	}

	return SDK_STATUS_OK;
}

static void fill_result(const struct SDKImageStreamSession *session,
                        uint32_t state, uint32_t bytes_consumed,
                        struct SDKImageStreamResult *result)
{
	memset(result, 0, sizeof(*result));
	result->session = session->session;
	result->state = state;
	result->image_width = session->image_width;
	result->image_height = session->image_height;
	result->output_format = session->output_format;
	result->bytes_consumed = bytes_consumed;
}

static uint32_t stream_output_bytes_per_pixel(
	const struct SDKImageStreamSession *session)
{
	return sdk_surface_format_bytes(session->output_format);
}

static void fill_complete_output_result(
	const struct SDKImageStreamSession *session, uint32_t bytes_consumed,
	struct SDKImageStreamResult *result)
{
	uint32_t bytes_per_pixel;

	bytes_per_pixel = stream_output_bytes_per_pixel(session);
	fill_result(session, SDK_IMAGE_SESSION_STATE_COMPLETE,
	            bytes_consumed, result);
	result->tile_width = session->output_width;
	result->tile_height = session->output_height;
	result->bytes_written = session->output_width *
	                        session->output_height * bytes_per_pixel;
	if (session->output_width != session->image_width ||
	    session->output_height != session->image_height) {
		result->flags |= SDK_IMAGE_SESSION_RESULT_SCALED;
	}
}

static void set_strided_flush(struct SDKImageStreamResult *result,
                              uintptr_t base, uint32_t stride,
                              uint32_t row_bytes, uint32_t rows)
{
	uint32_t flush_length;

	if (!result || base == 0U || stride == 0U || row_bytes == 0U ||
	    rows == 0U) {
		return;
	}
	if (rows == 1U) {
		flush_length = row_bytes;
	} else {
		if ((rows - 1U) > ((0xffffffffU - row_bytes) / stride))
			return;
		flush_length = row_bytes + ((rows - 1U) * stride);
	}

	result->flush_address = base;
	result->flush_length = flush_length;
}

static void set_tile_flush(const struct SDKImageStreamSession *session,
                           uint32_t row_bytes, uint32_t rows,
                           struct SDKImageStreamResult *result)
{
	set_strided_flush(result, session->tile_address,
	                  session->tile_stride, row_bytes, rows);
}

static void set_direct_flush(const struct SDKImageStreamSession *session,
                             uint32_t start_y, uint32_t rows,
                             uint32_t row_bytes, uint32_t bytes_per_pixel,
                             struct SDKImageStreamResult *result)
{
	uintptr_t base;
	uint32_t x_offset;
	uint32_t y;

	if (session->dst_x > (0xffffffffU / bytes_per_pixel))
		return;
	x_offset = session->dst_x * bytes_per_pixel;
	if (start_y > (0xffffffffU - session->dst_y))
		return;
	y = session->dst_y + start_y;
	if (y > ((0xffffffffU - x_offset) / session->dst_pitch)) {
		return;
	}

	base = session->dst_address +
	       (y * session->dst_pitch) +
	       x_offset;
	set_strided_flush(result, base, session->dst_pitch, row_bytes, rows);
}

static uint32_t feed_consumed(const struct SDKImageStreamSession *session,
                              const uint8_t *src, uint32_t src_length)
{
	const uint8_t *next;

	if (!src || src_length == 0U)
		return 0;

	next = session->source.next_input_byte;
	if (next >= src && next <= (src + src_length))
		return (uint32_t)(next - src);

	return src_length;
}

static void destroy_jpeg(struct SDKImageStreamSession *session)
{
	if (session->jpeg_created) {
		jpeg_destroy_decompress(&session->cinfo);
		session->jpeg_created = 0U;
	}
	session->header_ready = 0U;
	session->output_prepared = 0U;
	session->started = 0U;
}

static void configure_source(struct SDKImageStreamSession *session)
{
	session->source.init_source = stream_init_source;
	session->source.fill_input_buffer = stream_fill_input_buffer;
	session->source.skip_input_data = stream_skip_input_data;
	session->source.resync_to_restart = jpeg_resync_to_restart;
	session->source.term_source = stream_term_source;
	session->source.bytes_in_buffer = 0;
	session->source.next_input_byte = 0;
	session->cinfo.src = &session->source;
	session->cinfo.client_data = session;
}

static uint16_t create_jpeg_if_needed(struct SDKImageStreamSession *session)
{
	if (session->jpeg_created)
		return SDK_STATUS_OK;

	memset(&session->cinfo, 0, sizeof(session->cinfo));
	memset(&session->jerr, 0, sizeof(session->jerr));
	session->cinfo.err = jpeg_std_error(&session->jerr.pub);
	session->jerr.pub.error_exit = jpeg_error_exit;
	session->jerr.pub.output_message = jpeg_output_message;
	jpeg_create_decompress(&session->cinfo);
	session->jpeg_created = 1U;
	configure_source(session);
	return SDK_STATUS_OK;
}

static int dimensions_valid(uint32_t width, uint32_t height)
{
	if (width == 0U || height == 0U)
		return 0;
	if (width > SDK_IMAGE_STREAM_MAX_DIMENSION ||
	    height > SDK_IMAGE_STREAM_MAX_DIMENSION)
		return 0;
	if (width > (SDK_IMAGE_STREAM_MAX_PIXELS / height))
		return 0;
	return 1;
}

static int fit_requested(const struct SDKImageStreamSession *session)
{
	return (session->flags & SDK_IMAGE_DECODE_FLAG_FIT) != 0U;
}

static int preserve_aspect_requested(
	const struct SDKImageStreamSession *session)
{
	return (session->flags & SDK_IMAGE_DECODE_FLAG_PRESERVE_ASPECT) != 0U;
}

static int source_dimensions_valid(
	const struct SDKImageStreamSession *session)
{
	if (!session || session->image_width == 0U ||
	    session->image_height == 0U) {
		return 0;
	}
	if (!fit_requested(session))
		return dimensions_valid(session->image_width,
		                        session->image_height);
	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER)
		return dimensions_valid(session->image_width,
		                        session->image_height);
	if (session->image_width >
	    (SDK_IMAGE_STREAM_MAX_FIT_SOURCE_PIXELS / session->image_height)) {
		return 0;
	}
	return 1;
}

static int direct_destination_valid(const struct SDKImageStreamSession *session,
                                    uint32_t output_width,
                                    uint32_t output_height,
                                    uint32_t bytes_per_pixel)
{
	uint32_t row_bytes;
	uint32_t x_offset;

	if (session->dst_width == 0U || session->dst_height == 0U ||
	    output_width == 0U || output_height == 0U ||
	    bytes_per_pixel == 0U) {
		return 0;
	}
	if (output_width > session->dst_width ||
	    output_height > session->dst_height)
		return 0;
	if (session->dst_x > (session->dst_width - output_width) ||
	    session->dst_y > (session->dst_height - output_height)) {
		return 0;
	}
	if (session->dst_width > (0xffffffffU / bytes_per_pixel) ||
	    output_width > (0xffffffffU / bytes_per_pixel) ||
	    session->dst_x > (0xffffffffU / bytes_per_pixel)) {
		return 0;
	}

	row_bytes = output_width * bytes_per_pixel;
	x_offset = session->dst_x * bytes_per_pixel;
	if (session->dst_pitch < (session->dst_width * bytes_per_pixel))
		return 0;
	if (x_offset > session->dst_pitch ||
	    row_bytes > (session->dst_pitch - x_offset)) {
		return 0;
	}
	return 1;
}

static uint32_t muldiv_floor_u32(uint32_t value, uint32_t multiplier,
                                 uint32_t divisor)
{
	uint64_t product;

	if (divisor == 0U)
		return 0U;
	product = (uint64_t)value * (uint64_t)multiplier;
	return (uint32_t)(product / divisor);
}

static int compute_fit_output_dimensions(
	const struct SDKImageStreamSession *session,
	uint32_t *out_width, uint32_t *out_height)
{
	uint64_t width_limited;
	uint32_t width;
	uint32_t height;

	if (!session || !out_width || !out_height ||
	    session->image_width == 0U || session->image_height == 0U ||
	    (fit_requested(session) &&
	     (session->dst_width == 0U || session->dst_height == 0U))) {
		return 0;
	}

	if (!fit_requested(session)) {
		*out_width = session->image_width;
		*out_height = session->image_height;
		return 1;
	}
	if (!preserve_aspect_requested(session)) {
		*out_width = session->dst_width;
		*out_height = session->dst_height;
		return 1;
	}
	if (session->image_width <= session->dst_width &&
	    session->image_height <= session->dst_height) {
		*out_width = session->image_width;
		*out_height = session->image_height;
		return 1;
	}

	width_limited = (uint64_t)session->image_width *
	                (uint64_t)session->dst_height;
	if (width_limited >
	    ((uint64_t)session->image_height * (uint64_t)session->dst_width)) {
		width = session->dst_width;
		height = muldiv_floor_u32(session->image_height,
		                          session->dst_width,
		                          session->image_width);
		if (height == 0U)
			height = 1U;
	} else {
		height = session->dst_height;
		width = muldiv_floor_u32(session->image_width,
		                         session->dst_height,
		                         session->image_height);
		if (width == 0U)
			width = 1U;
	}

	if (width > session->dst_width || height > session->dst_height)
		return 0;
	*out_width = width;
	*out_height = height;
	return 1;
}

static int decode_dimensions_supported(uint32_t width, uint32_t height)
{
	if (width == 0U || height == 0U)
		return 0;
	if (width > SDK_IMAGE_STREAM_MAX_DECODE_WIDTH)
		return 0;
	return 1;
}

static int choose_jpeg_fit_scale(struct SDKImageStreamSession *session)
{
	unsigned int numerator;
	unsigned int best_num = 0U;
	JDIMENSION best_width = 0U;
	JDIMENSION best_height = 0U;
	uint32_t target_width;
	uint32_t target_height;

	if (!compute_fit_output_dimensions(session, &target_width,
	                                   &target_height)) {
		return 0;
	}
	session->output_width = target_width;
	session->output_height = target_height;

	if (!fit_requested(session) ||
	    (target_width == session->image_width &&
	     target_height == session->image_height)) {
		session->cinfo.scale_num = 1U;
		session->cinfo.scale_denom = 1U;
		jpeg_calc_output_dimensions(&session->cinfo);
		session->decode_width = (uint32_t)session->cinfo.output_width;
		session->decode_height = (uint32_t)session->cinfo.output_height;
		return decode_dimensions_supported(session->decode_width,
		                                   session->decode_height);
	}

	for (numerator = 1U; numerator <= 8U; numerator++) {
		session->cinfo.scale_num = numerator;
		session->cinfo.scale_denom = 8U;
		jpeg_calc_output_dimensions(&session->cinfo);
		if (session->cinfo.output_width >= target_width &&
		    session->cinfo.output_height >= target_height &&
		    decode_dimensions_supported(
			    (uint32_t)session->cinfo.output_width,
			    (uint32_t)session->cinfo.output_height)) {
			best_num = numerator;
			best_width = session->cinfo.output_width;
			best_height = session->cinfo.output_height;
			break;
		}
	}

	if (best_num == 0U)
		return 0;

	session->cinfo.scale_num = best_num;
	session->cinfo.scale_denom = 8U;
	jpeg_calc_output_dimensions(&session->cinfo);
	session->decode_width = (uint32_t)best_width;
	session->decode_height = (uint32_t)best_height;
	return 1;
}

static uint8_t lerp_channel_8(uint8_t p0, uint8_t p1, uint32_t weight)
{
	uint32_t inverse = 256U - weight;
	uint32_t value = ((uint32_t)p0 * inverse) +
	                 ((uint32_t)p1 * weight);

	return (uint8_t)((value + 128U) >> 8);
}

static uint32_t scale_coord_8(uint32_t out_pos, uint32_t in_size,
                              uint32_t out_size)
{
	if (out_size <= 1U || in_size <= 1U)
		return 0U;

	return (uint32_t)(((uint64_t)out_pos * (in_size - 1U) * 256U) /
	                  (out_size - 1U));
}

void sdk_image_stream_scale_rows_bilinear_4byte(uint8_t *dst,
                                                const uint8_t *row0,
                                                const uint8_t *row1,
                                                uint32_t src_width,
                                                uint32_t dst_width,
                                                uint32_t y_weight);

void sdk_image_stream_scale_row_bilinear_4byte(uint8_t *dst,
                                               const uint8_t *src,
                                               uint32_t src_width,
                                               uint32_t dst_width)
{
	sdk_image_stream_scale_rows_bilinear_4byte(dst, src, src, src_width,
	                                           dst_width, 0U);
}

void sdk_image_stream_scale_rows_bilinear_4byte(uint8_t *dst,
                                                const uint8_t *row0,
                                                const uint8_t *row1,
                                                uint32_t src_width,
                                                uint32_t dst_width,
                                                uint32_t y_weight)
{
	uint32_t x;

	if (!dst || !row0 || !row1 || src_width == 0U || dst_width == 0U)
		return;
	for (x = 0U; x < dst_width; x++) {
		uint32_t sx_fp = scale_coord_8(x, src_width, dst_width);
		uint32_t sx0 = sx_fp >> 8;
		uint32_t sx1 = sx0;
		uint32_t x_weight = sx_fp & 0xffU;
		const uint8_t *top0;
		const uint8_t *top1;
		const uint8_t *bottom0;
		const uint8_t *bottom1;
		uint8_t *out = dst + (x * 4U);
		uint32_t b;

		if ((sx0 + 1U) < src_width)
			sx1 = sx0 + 1U;

		top0 = row0 + (sx0 * 4U);
		top1 = row0 + (sx1 * 4U);
		bottom0 = row1 + (sx0 * 4U);
		bottom1 = row1 + (sx1 * 4U);
		for (b = 0U; b < 4U; b++) {
			uint8_t top = lerp_channel_8(top0[b], top1[b],
			                             x_weight);
			uint8_t bottom = lerp_channel_8(bottom0[b],
			                                bottom1[b],
			                                x_weight);
			out[b] = lerp_channel_8(top, bottom, y_weight);
		}
	}
}

static int direct_scale_row_slot(
	const struct SDKImageStreamSession *session, uint32_t source_y)
{
	uint32_t i;

	for (i = 0U; i < 2U; i++) {
		if (session->direct_scale_row_valid[i] &&
		    session->direct_scale_row_y[i] == source_y) {
			return (int)i;
		}
	}

	return -1;
}

static uint32_t direct_scale_decode_slot(
	const struct SDKImageStreamSession *session)
{
	if (!session->direct_scale_row_valid[0])
		return 0U;
	if (!session->direct_scale_row_valid[1])
		return 1U;
	if (session->direct_scale_row_y[0] <= session->direct_scale_row_y[1])
		return 0U;
	return 1U;
}

static int direct_scale_read_until_row(struct SDKImageStreamSession *session,
                                       uint32_t source_y)
{
	while (direct_scale_row_slot(session, source_y) < 0) {
		uint32_t slot;
		uint32_t next_y;
		JSAMPROW row;

		if (session->cinfo.output_scanline > source_y)
			return -1;
		if (session->cinfo.output_scanline >= session->cinfo.output_height)
			return -1;

		slot = direct_scale_decode_slot(session);
		next_y = (uint32_t)session->cinfo.output_scanline;
		row = session->direct_scale_rows[slot];
		if (jpeg_read_scanlines(&session->cinfo, &row, 1U) != 1U)
			return 0;

		session->direct_scale_row_valid[slot] = 1U;
		session->direct_scale_row_y[slot] = next_y;
	}

	return 1;
}

static int direct_scale_rows_for_output(
	struct SDKImageStreamSession *session, uint32_t output_y,
	uint8_t **row0, uint8_t **row1, uint32_t *y_weight)
{
	uint32_t sy_fp;
	uint32_t sy0;
	uint32_t sy1;
	int status;
	int slot0;
	int slot1;

	if (!session || !row0 || !row1 || !y_weight)
		return -1;

	sy_fp = scale_coord_8(output_y, session->decode_height,
	                      session->output_height);
	sy0 = sy_fp >> 8;
	sy1 = sy0;
	*y_weight = sy_fp & 0xffU;
	if (*y_weight != 0U && (sy0 + 1U) < session->decode_height)
		sy1 = sy0 + 1U;

	status = direct_scale_read_until_row(session, sy0);
	if (status <= 0)
		return status;
	if (sy1 != sy0) {
		status = direct_scale_read_until_row(session, sy1);
		if (status <= 0)
			return status;
	}

	slot0 = direct_scale_row_slot(session, sy0);
	slot1 = direct_scale_row_slot(session, sy1);
	if (slot0 < 0 || slot1 < 0)
		return -1;

	*row0 = session->direct_scale_rows[slot0];
	*row1 = session->direct_scale_rows[slot1];
	return 1;
}

static int direct_scaling_needed(
	const struct SDKImageStreamSession *session)
{
	return session->decode_width != session->output_width ||
	       session->decode_height != session->output_height;
}

static void fill_direct_scaled_progress(
	const struct SDKImageStreamSession *session,
	uint32_t state, uint32_t bytes_consumed, uint32_t start_y,
	uint32_t rows_written, uint32_t bytes_per_pixel,
	struct SDKImageStreamResult *result)
{
	fill_result(session, state, bytes_consumed, result);
	result->bytes_written = session->output_width *
	                        rows_written * bytes_per_pixel;
	result->flags |= SDK_IMAGE_SESSION_RESULT_SCALED;
	result->tile_width = session->output_width;
	result->tile_height = session->output_height;
	if (state != SDK_IMAGE_SESSION_STATE_COMPLETE)
		result->flags |= SDK_IMAGE_SESSION_RESULT_PARTIAL;
	set_direct_flush(session, start_y, rows_written,
	                 session->output_width * bytes_per_pixel,
	                 bytes_per_pixel, result);
}

static uint16_t finish_stream(struct SDKImageStreamSession *session,
                              const uint8_t *src, uint32_t src_length,
                              struct SDKImageStreamResult *result)
{
	uint32_t consumed;

	if (!jpeg_finish_decompress(&session->cinfo)) {
		consumed = feed_consumed(session, src, src_length);
		fill_result(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT,
		            consumed, result);
		return SDK_STATUS_OK;
	}

	consumed = feed_consumed(session, src, src_length);
	fill_complete_output_result(session, consumed, result);
	destroy_jpeg(session);
	return SDK_STATUS_OK;
}

static void png_warning_handler(png_structp png_ptr, png_const_charp message)
{
	(void)png_ptr;
	(void)message;
}

static void png_error_handler(png_structp png_ptr, png_const_charp message)
{
	struct SDKImageStreamSession *session =
		(struct SDKImageStreamSession *)png_get_error_ptr(png_ptr);

	(void)message;
	if (session && session->png_error_status == SDK_STATUS_OK)
		session->png_error_status = SDK_STATUS_IO_ERROR;
	longjmp(png_jmpbuf(png_ptr), 1);
}

static void png_fail(struct SDKImageStreamSession *session, uint16_t status,
                     png_const_charp message)
{
	session->png_error_status = status;
	png_error(session->png_ptr, message);
}

/*
 * libpng allocator hooks routed through the decode-reclaim wrappers so a
 * core-1 fault mid-decode can reclaim every block (plain malloc/free on
 * core 0) -- same guarantee as the libjpeg jmem_zz9k backend.
 */
static png_voidp png_decode_alloc(png_structp png_ptr, png_alloc_size_t size)
{
	(void)png_ptr;
	return (png_voidp)sdk_decode_heap_alloc((size_t)size);
}

static void png_decode_free(png_structp png_ptr, png_voidp ptr)
{
	(void)png_ptr;
	sdk_decode_heap_free((void *)ptr);
}

static void destroy_png(struct SDKImageStreamSession *session)
{
	if (session->png_interlace_buffer) {
		sdk_decode_heap_free(session->png_interlace_buffer);
		session->png_interlace_buffer = 0;
	}
	if (session->png_created) {
		png_destroy_read_struct(&session->png_ptr, &session->png_info,
		                        0);
		session->png_created = 0U;
	}
	session->png_ptr = 0;
	session->png_info = 0;
	session->png_complete = 0U;
	session->png_row_rgb888 = 0U;
	session->png_interlaced = 0U;
	session->png_interlace_direct = 0U;
	session->png_interlace_rowbytes = 0U;
	session->png_interlace_buffer_length = 0U;
	session->png_rows_this_feed = 0U;
	session->rows_output = 0U;
	session->header_ready = 0U;
	session->output_prepared = 0U;
}

static void destroy_stream_codec(struct SDKImageStreamSession *session)
{
	if (session->codec == SDK_IMAGE_CODEC_PNG)
		destroy_png(session);
	else
		destroy_jpeg(session);
}

static uint16_t fail_stream_session(struct SDKImageStreamSession *session,
                                    uint16_t status,
                                    uint32_t bytes_consumed,
                                    struct SDKImageStreamResult *result)
{
	destroy_stream_codec(session);
	session->failed = 1U;
	fill_result(session, SDK_IMAGE_SESSION_STATE_ERROR,
	            bytes_consumed, result);
	return status;
}

static uint16_t create_png_if_needed(struct SDKImageStreamSession *session)
{
	if (session->png_created)
		return SDK_STATUS_OK;

	session->png_error_status = SDK_STATUS_OK;
	session->png_ptr = png_create_read_struct_2(PNG_LIBPNG_VER_STRING,
	                                            session,
	                                            png_error_handler,
	                                            png_warning_handler,
	                                            0,
	                                            png_decode_alloc,
	                                            png_decode_free);
	if (!session->png_ptr)
		return SDK_STATUS_NO_MEMORY;
	session->png_info = png_create_info_struct(session->png_ptr);
	if (!session->png_info) {
		png_destroy_read_struct(&session->png_ptr, 0, 0);
		return SDK_STATUS_NO_MEMORY;
	}
	session->png_created = 1U;
	png_set_progressive_read_fn(session->png_ptr, session,
	                            0, 0, 0);
	return SDK_STATUS_OK;
}

static void convert_rgba_row(uint8_t *dst, const uint8_t *src,
                             uint32_t pixels, uint32_t output_format)
{
	uint32_t bytes_per_pixel;
	uint32_t x;

	bytes_per_pixel = sdk_surface_format_bytes(output_format);
	if (bytes_per_pixel == 0U)
		return;

	for (x = 0U; x < pixels; x++) {
		const uint8_t *in = src + (x * 4U);
		uint8_t *out = dst + (x * bytes_per_pixel);
		uint8_t r = in[0];
		uint8_t g = in[1];
		uint8_t b = in[2];
		uint8_t a = in[3];

		switch (output_format) {
		case SDK_SURFACE_FORMAT_ARGB8888:
			out[0] = a;
			out[1] = r;
			out[2] = g;
			out[3] = b;
			break;
		case SDK_SURFACE_FORMAT_RGBA8888:
			out[0] = r;
			out[1] = g;
			out[2] = b;
			out[3] = a;
			break;
		case SDK_SURFACE_FORMAT_RGB888:
			out[0] = r;
			out[1] = g;
			out[2] = b;
			break;
		case SDK_SURFACE_FORMAT_BGRA8888:
		default:
			out[0] = b;
			out[1] = g;
			out[2] = r;
			out[3] = a;
			break;
		}
	}
}

static uint8_t *png_output_row_address(
	struct SDKImageStreamSession *session, uint32_t row_num,
	uint32_t row_bytes, uint32_t bytes_per_pixel)
{
	uint32_t x_offset;
	uint32_t y;

	if (!session || row_num >= session->output_height ||
	    row_bytes == 0U || bytes_per_pixel == 0U) {
		return 0;
	}

	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		if (session->tile_stride < row_bytes ||
		    row_num >= session->tile_rows) {
			return 0;
		}
		return (uint8_t *)(uintptr_t)
			(session->tile_address + (row_num * session->tile_stride));
	}

	if (session->dst_x > (0xffffffffU / bytes_per_pixel))
		return 0;
	x_offset = session->dst_x * bytes_per_pixel;
	if (row_num > (0xffffffffU - session->dst_y))
		return 0;
	y = session->dst_y + row_num;
	if (y > ((0xffffffffU - x_offset) / session->dst_pitch))
		return 0;

	return (uint8_t *)(uintptr_t)
		(session->dst_address + (y * session->dst_pitch) + x_offset);
}

static int png_interlace_can_combine_direct(
	const struct SDKImageStreamSession *session)
{
	if (!session)
		return 0;
	if (session->png_row_rgb888)
		return session->output_format == SDK_SURFACE_FORMAT_RGB888;
	return session->output_format == SDK_SURFACE_FORMAT_RGBA8888;
}

static int png_write_combined_interlace_row(
	struct SDKImageStreamSession *session, const uint8_t *row,
	uint32_t row_num)
{
	uint8_t *dst;
	uint32_t bytes_per_pixel;
	uint32_t row_bytes;

	if (!session || !row || row_num >= session->output_height)
		return -1;
	bytes_per_pixel = sdk_surface_format_bytes(session->output_format);
	if (bytes_per_pixel != 3U && bytes_per_pixel != 4U)
		return -1;
	if (session->output_width > (0xffffffffU / bytes_per_pixel))
		return -1;
	row_bytes = session->output_width * bytes_per_pixel;
	dst = png_output_row_address(session, row_num, row_bytes,
	                             bytes_per_pixel);
	if (!dst)
		return -1;

	if (session->png_row_rgb888) {
		if (session->output_format != SDK_SURFACE_FORMAT_RGB888)
			return -1;
		memcpy(dst, row, row_bytes);
	} else {
		convert_rgba_row(dst, row, session->output_width,
		                 session->output_format);
	}
	return 1;
}

static int png_prepare_interlace_storage(
	struct SDKImageStreamSession *session, png_size_t rowbytes)
{
	uint32_t row_bytes;
	uint32_t image_bytes;
	uint32_t y;

	if (!session || rowbytes == 0U || rowbytes > 0xffffffffU ||
	    session->output_height == 0U) {
		return 0;
	}

	row_bytes = (uint32_t)rowbytes;
	session->png_interlace_rowbytes = row_bytes;
	session->png_interlace_direct =
		png_interlace_can_combine_direct(session) ? 1U : 0U;

	if (session->png_interlace_direct) {
		for (y = 0U; y < session->output_height; y++) {
			uint8_t *dst = png_output_row_address(
				session, y, row_bytes,
				sdk_surface_format_bytes(session->output_format));
			if (!dst)
				return 0;
			memset(dst, 0, row_bytes);
		}
		return 1;
	}

	if (session->output_height > (0xffffffffU / row_bytes))
		return 0;
	image_bytes = session->output_height * row_bytes;
	session->png_interlace_buffer =
	    (uint8_t *)sdk_decode_heap_alloc(image_bytes);
	if (!session->png_interlace_buffer)
		return 0;
	memset(session->png_interlace_buffer, 0, image_bytes);
	session->png_interlace_buffer_length = image_bytes;
	return 1;
}

static int png_write_interlaced_output_row(
	struct SDKImageStreamSession *session, png_structp png_ptr,
	png_bytep new_row, uint32_t row_num)
{
	uint8_t *row;
	uint32_t rowbytes;

	if (!session || !png_ptr || !session->output_prepared ||
	    row_num >= session->output_height) {
		return -1;
	}
	if (!new_row)
		return 1;

	rowbytes = session->png_interlace_rowbytes;
	if (rowbytes == 0U)
		return -1;

	if (session->png_interlace_direct) {
		row = png_output_row_address(
			session, row_num, rowbytes,
			sdk_surface_format_bytes(session->output_format));
		if (!row)
			return -1;
		png_progressive_combine_row(png_ptr, row, new_row);
		return 1;
	}

	if (!session->png_interlace_buffer ||
	    row_num >= (session->png_interlace_buffer_length / rowbytes)) {
		return -1;
	}
	row = session->png_interlace_buffer + (row_num * rowbytes);
	png_progressive_combine_row(png_ptr, row, new_row);
	return png_write_combined_interlace_row(session, row, row_num);
}

static int png_write_output_row(struct SDKImageStreamSession *session,
                                const uint8_t *row, uint32_t row_num)
{
	uint8_t *dst;
	uint32_t bytes_per_pixel;
	uint32_t row_bytes;

	if (!session || !row || !session->output_prepared ||
	    row_num != session->rows_output ||
	    row_num >= session->output_height) {
		return -1;
	}

	bytes_per_pixel = sdk_surface_format_bytes(session->output_format);
	if (bytes_per_pixel != 3U && bytes_per_pixel != 4U)
		return -1;
	row_bytes = session->output_width * bytes_per_pixel;
	if (session->png_rows_this_feed == 0U)
		session->png_write_start_y = row_num;

	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		if (session->png_rows_this_feed >= session->tile_rows ||
		    session->tile_stride < row_bytes) {
			return -1;
		}
		dst = (uint8_t *)(uintptr_t)
			(session->tile_address +
			 (session->png_rows_this_feed * session->tile_stride));
	} else {
		dst = (uint8_t *)(uintptr_t)
			(session->dst_address +
			 ((session->dst_y + row_num) * session->dst_pitch) +
			 (session->dst_x * bytes_per_pixel));
	}

	if (session->png_row_rgb888) {
		if (session->output_format != SDK_SURFACE_FORMAT_RGB888)
			return -1;
		memcpy(dst, row, row_bytes);
	} else {
		convert_rgba_row(dst, row, session->output_width,
		                 session->output_format);
	}
	session->rows_output++;
	session->png_rows_this_feed++;
	return 1;
}

static void png_info_callback(png_structp png_ptr, png_infop info_ptr)
{
	struct SDKImageStreamSession *session =
		(struct SDKImageStreamSession *)png_get_progressive_ptr(png_ptr);
	png_uint_32 width = 0U;
	png_uint_32 height = 0U;
	int bit_depth = 0;
	int color_type = 0;
	int interlace_type = 0;
	int has_alpha;
	int has_trns;
	int direct_rgb888_rows;
	uint32_t bytes_per_pixel;
	png_size_t rowbytes;

	if (!session)
		return;

	png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth,
	             &color_type, &interlace_type, 0, 0);
	if (interlace_type != PNG_INTERLACE_NONE &&
	    interlace_type != PNG_INTERLACE_ADAM7) {
		png_fail(session, SDK_STATUS_UNSUPPORTED,
		         "unsupported PNG interlace method");
	}
	session->png_interlaced =
		interlace_type == PNG_INTERLACE_ADAM7 ? 1U : 0U;
	has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0;
	has_trns = png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0;
	direct_rgb888_rows = session->output_format == SDK_SURFACE_FORMAT_RGB888 &&
	                     !has_alpha && !has_trns;

	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png_ptr);
	if (has_trns)
		png_set_tRNS_to_alpha(png_ptr);
	if (bit_depth == 16)
		png_set_strip_16(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png_ptr);
	}
	if (!direct_rgb888_rows && !has_alpha && !has_trns)
		png_set_filler(png_ptr, 0xffU, PNG_FILLER_AFTER);
	if (session->png_interlaced)
		(void)png_set_interlace_handling(png_ptr);

	png_read_update_info(png_ptr, info_ptr);
	width = png_get_image_width(png_ptr, info_ptr);
	height = png_get_image_height(png_ptr, info_ptr);
	rowbytes = png_get_rowbytes(png_ptr, info_ptr);
	if (direct_rgb888_rows && rowbytes == ((png_size_t)width * 3U)) {
		session->png_row_rgb888 = 1U;
	} else if (rowbytes == ((png_size_t)width * 4U)) {
		session->png_row_rgb888 = 0U;
	} else {
		png_fail(session, SDK_STATUS_UNSUPPORTED,
		         "unsupported PNG output row format");
	}

	session->image_width = (uint32_t)width;
	session->image_height = (uint32_t)height;
	session->decode_width = session->image_width;
	session->decode_height = session->image_height;
	session->output_width = session->image_width;
	session->output_height = session->image_height;
	if (!source_dimensions_valid(session)) {
		png_fail(session, SDK_STATUS_BAD_REQUEST,
		         "unsupported PNG dimensions");
	}

	bytes_per_pixel = sdk_surface_format_bytes(session->output_format);
	if (bytes_per_pixel != 3U && bytes_per_pixel != 4U) {
		png_fail(session, SDK_STATUS_UNSUPPORTED,
		         "unsupported PNG destination format");
	}
	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		if (session->output_width >
		    (0xffffffffU / bytes_per_pixel) ||
		    session->tile_stride <
		    (session->output_width * bytes_per_pixel) ||
		    session->tile_rows < session->output_height) {
			png_fail(session, SDK_STATUS_BAD_REQUEST,
			         "PNG tile destination is too small");
		}
	} else {
		if (!direct_destination_valid(session, session->output_width,
		                              session->output_height,
		                              bytes_per_pixel)) {
			png_fail(session, SDK_STATUS_BAD_REQUEST,
			         "PNG destination is too small");
		}
	}

	if (session->png_interlaced &&
	    !png_prepare_interlace_storage(session, rowbytes)) {
		png_fail(session, SDK_STATUS_NO_MEMORY,
		         "PNG interlace storage unavailable");
	}

	session->header_ready = 1U;
	session->output_prepared = 1U;
}

static void png_row_callback(png_structp png_ptr, png_bytep new_row,
                             png_uint_32 row_num, int pass)
{
	struct SDKImageStreamSession *session =
		(struct SDKImageStreamSession *)png_get_progressive_ptr(png_ptr);
	int row_status;

	if (!session || !new_row)
		return;

	if (session->png_interlaced) {
		row_status = png_write_interlaced_output_row(
			session, png_ptr, new_row, (uint32_t)row_num);
	} else {
		row_status = png_write_output_row(session, new_row, row_num);
	}
	if (row_status < 0) {
		png_fail(session, SDK_STATUS_IO_ERROR,
		         "unexpected PNG row order");
	}

	(void)pass;
	(void)png_ptr;
}

static void png_end_callback(png_structp png_ptr, png_infop info_ptr)
{
	struct SDKImageStreamSession *session =
		(struct SDKImageStreamSession *)png_get_progressive_ptr(png_ptr);

	(void)info_ptr;
	if (session) {
		if (session->png_interlaced && session->output_prepared) {
			session->png_write_start_y = 0U;
			session->png_rows_this_feed = session->output_height;
			session->rows_output = session->output_height;
		}
		session->png_complete = 1U;
	}
}

static void configure_png_callbacks(struct SDKImageStreamSession *session)
{
	png_set_progressive_read_fn(session->png_ptr, session,
	                            png_info_callback, png_row_callback,
	                            png_end_callback);
}

static uint32_t png_feed_consumed(const struct SDKImageStreamSession *session,
                                  uint32_t src_length)
{
	(void)session;
	return src_length;
}

static void fill_png_tile_result(const struct SDKImageStreamSession *session,
                                 uint32_t bytes_consumed,
                                 struct SDKImageStreamResult *result)
{
	uint32_t bytes_per_pixel;

	bytes_per_pixel = stream_output_bytes_per_pixel(session);
	fill_result(session, SDK_IMAGE_SESSION_STATE_TILE_READY,
	            bytes_consumed, result);
	result->tile_y = session->png_write_start_y;
	result->tile_width = session->output_width;
	result->tile_height = session->png_rows_this_feed;
	result->bytes_written = session->output_width *
	                        session->png_rows_this_feed *
	                        bytes_per_pixel;
	if (!session->png_complete)
		result->flags |= SDK_IMAGE_SESSION_RESULT_PARTIAL;
	set_tile_flush(session, session->output_width * bytes_per_pixel,
	               session->png_rows_this_feed, result);
}

static void fill_png_direct_result(const struct SDKImageStreamSession *session,
                                   uint32_t state, uint32_t bytes_consumed,
                                   struct SDKImageStreamResult *result)
{
	uint32_t bytes_per_pixel;

	bytes_per_pixel = stream_output_bytes_per_pixel(session);
	if (state == SDK_IMAGE_SESSION_STATE_COMPLETE) {
		fill_complete_output_result(session, bytes_consumed, result);
	} else {
		fill_result(session, state, bytes_consumed, result);
		result->tile_width = session->output_width;
		result->tile_height = session->output_height;
		result->bytes_written = session->output_width *
		                        session->png_rows_this_feed *
		                        bytes_per_pixel;
		result->flags |= SDK_IMAGE_SESSION_RESULT_PARTIAL;
	}
	set_direct_flush(session, session->png_write_start_y,
	                 session->png_rows_this_feed,
	                 session->output_width * bytes_per_pixel,
	                 bytes_per_pixel, result);
}

static uint16_t process_png_stream(struct SDKImageStreamSession *session,
                                   const uint8_t *src, uint32_t src_length,
                                   struct SDKImageStreamResult *result)
{
	uint32_t consumed;

	if (create_png_if_needed(session) != SDK_STATUS_OK)
		return SDK_STATUS_NO_MEMORY;
	configure_png_callbacks(session);

	session->png_error_status = SDK_STATUS_OK;
	session->png_rows_this_feed = 0U;
	session->png_write_start_y = session->rows_output;

	if (setjmp(png_jmpbuf(session->png_ptr))) {
		uint16_t status = session->png_error_status;

		if (status == SDK_STATUS_OK)
			status = SDK_STATUS_IO_ERROR;
		return fail_stream_session(session, status, 0U, result);
	}

	if (session->png_complete) {
		fill_complete_output_result(session, 0U, result);
		destroy_png(session);
		return SDK_STATUS_OK;
	}

	if (src_length != 0U)
		png_process_data(session->png_ptr, session->png_info,
		                 (png_bytep)src, src_length);

	consumed = png_feed_consumed(session, src_length);

	if (session->png_rows_this_feed != 0U) {
		if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
			fill_png_tile_result(session, consumed, result);
			return SDK_STATUS_OK;
		}
		uint32_t state = session->png_complete ?
			SDK_IMAGE_SESSION_STATE_COMPLETE :
			SDK_IMAGE_SESSION_STATE_NEED_INPUT;

		fill_png_direct_result(session, state, consumed, result);
		if (state == SDK_IMAGE_SESSION_STATE_COMPLETE) {
			destroy_png(session);
		}
		return SDK_STATUS_OK;
	}

	if (session->png_complete) {
		fill_complete_output_result(session, consumed, result);
		destroy_png(session);
		return SDK_STATUS_OK;
	}

	if (session->input_eof) {
		return fail_stream_session(session, SDK_STATUS_IO_ERROR,
		                           consumed, result);
	}

	fill_result(session,
	            session->header_ready ? SDK_IMAGE_SESSION_STATE_HEADER_READY :
	            SDK_IMAGE_SESSION_STATE_NEED_INPUT, consumed, result);
	if (session->header_ready)
		result->flags |= SDK_IMAGE_SESSION_RESULT_HEADER_READY;
	return SDK_STATUS_OK;
}

static uint16_t process_jpeg_stream(struct SDKImageStreamSession *session,
                                    const uint8_t *src, uint32_t src_length,
                                    struct SDKImageStreamResult *result)
{
	J_COLOR_SPACE color_space;
	uint32_t bytes_per_pixel;
	uint32_t consumed;
	uint32_t tile_start_y;
	uint32_t tile_rows_written;
	uint32_t direct_start_y;
	uint32_t direct_rows_written;
	uint16_t status;
	int header_status;

	if (create_jpeg_if_needed(session) != SDK_STATUS_OK)
		return SDK_STATUS_INTERNAL_ERROR;

	session->source.next_input_byte = src;
	session->source.bytes_in_buffer = src_length;

	if (setjmp(session->jerr.jump)) {
		return fail_stream_session(
			session, SDK_STATUS_IO_ERROR,
			feed_consumed(session, src, src_length), result);
	}

	if (!session->header_ready) {
		header_status = jpeg_read_header(&session->cinfo, TRUE);
		if (header_status == JPEG_SUSPENDED) {
			consumed = feed_consumed(session, src, src_length);
			fill_result(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT,
			            consumed, result);
			return SDK_STATUS_OK;
		}
		if (header_status != JPEG_HEADER_OK)
			return SDK_STATUS_IO_ERROR;

		session->image_width = (uint32_t)session->cinfo.image_width;
		session->image_height = (uint32_t)session->cinfo.image_height;
		if (!source_dimensions_valid(session)) {
			return SDK_STATUS_BAD_REQUEST;
		}
		session->header_ready = 1U;
	}

	if (!output_color_space(session->output_format, &color_space))
		return SDK_STATUS_UNSUPPORTED;
	bytes_per_pixel = sdk_surface_format_bytes(session->output_format);
	if (bytes_per_pixel != 4U && bytes_per_pixel != 3U)
		return SDK_STATUS_UNSUPPORTED;
	if (!session->output_prepared) {
		session->cinfo.out_color_space = color_space;
		session->cinfo.dct_method = JDCT_IFAST;
		if (!choose_jpeg_fit_scale(session))
			return SDK_STATUS_BAD_REQUEST;
		session->output_prepared = 1U;
	}
	if (!dimensions_valid(session->output_width, session->output_height))
		return SDK_STATUS_BAD_REQUEST;

	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		if (session->tile_stride < session->decode_width * bytes_per_pixel)
			return SDK_STATUS_BAD_REQUEST;
	} else if (!direct_destination_valid(session, session->output_width,
	                                     session->output_height,
	                                     bytes_per_pixel)) {
		return SDK_STATUS_BAD_REQUEST;
	}

	if (!session->started) {
		if (!jpeg_start_decompress(&session->cinfo)) {
			consumed = feed_consumed(session, src, src_length);
			fill_result(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT,
			            consumed, result);
			return SDK_STATUS_OK;
		}
		if (session->cinfo.output_components != (int)bytes_per_pixel ||
		    session->cinfo.output_width != session->decode_width ||
		    session->cinfo.output_height != session->decode_height) {
			return SDK_STATUS_IO_ERROR;
		}
		session->started = 1U;
	}

	if (session->output_mode == SDK_IMAGE_OUTPUT_TILE_BUFFER) {
		tile_start_y = (uint32_t)session->cinfo.output_scanline;
		tile_rows_written = 0U;
		while (session->cinfo.output_scanline <
		       session->cinfo.output_height &&
		       tile_rows_written < session->tile_rows) {
			JSAMPROW row = (JSAMPROW)
				(session->tile_address +
				 (tile_rows_written * session->tile_stride));

			if (jpeg_read_scanlines(&session->cinfo, &row, 1U) != 1U)
				break;
			tile_rows_written++;
		}

		if (tile_rows_written != 0U) {
			consumed = feed_consumed(session, src, src_length);
			fill_result(session, SDK_IMAGE_SESSION_STATE_TILE_READY,
			            consumed, result);
			result->tile_x = 0U;
			result->tile_y = tile_start_y;
			result->tile_width = session->decode_width;
			result->tile_height = tile_rows_written;
			result->bytes_written = session->decode_width *
			                        tile_rows_written *
			                        bytes_per_pixel;
			set_tile_flush(session, session->decode_width *
			               bytes_per_pixel, tile_rows_written,
			               result);
			if (session->output_width != session->image_width ||
			    session->output_height != session->image_height) {
				result->flags |= SDK_IMAGE_SESSION_RESULT_SCALED;
			}
			if (session->cinfo.output_scanline <
			    session->cinfo.output_height) {
				result->flags |= SDK_IMAGE_SESSION_RESULT_PARTIAL;
			}
			return SDK_STATUS_OK;
		}
	} else if (!direct_scaling_needed(session)) {
		direct_start_y = (uint32_t)session->cinfo.output_scanline;
		direct_rows_written = 0U;
		while (session->cinfo.output_scanline <
		       session->cinfo.output_height) {
			JSAMPROW row = (JSAMPROW)
				(session->dst_address +
				 ((session->dst_y + session->cinfo.output_scanline) *
				  session->dst_pitch) +
				 (session->dst_x * bytes_per_pixel));

			if (jpeg_read_scanlines(&session->cinfo, &row, 1U) != 1U) {
				consumed = feed_consumed(session, src, src_length);
				fill_result(session,
				            SDK_IMAGE_SESSION_STATE_NEED_INPUT,
				            consumed, result);
				result->bytes_written =
					session->output_width *
					direct_rows_written *
					bytes_per_pixel;
				set_direct_flush(session, direct_start_y,
				                 direct_rows_written,
				                 session->output_width *
				                 bytes_per_pixel,
				                 bytes_per_pixel, result);
				if (session->output_width != session->image_width ||
				    session->output_height != session->image_height) {
					result->flags |=
						SDK_IMAGE_SESSION_RESULT_SCALED;
					result->tile_width =
						session->output_width;
					result->tile_height =
						session->output_height;
				}
				return SDK_STATUS_OK;
			}
			direct_rows_written++;
		}

		status = finish_stream(session, src, src_length, result);
		set_direct_flush(session, direct_start_y, direct_rows_written,
		                 session->output_width * bytes_per_pixel,
		                 bytes_per_pixel, result);
		return status;
	} else {
		direct_start_y = session->scaled_next_y;
		direct_rows_written = 0U;
		while (session->scaled_next_y < session->output_height) {
			uint8_t *src_row0;
			uint8_t *src_row1;
			uint8_t *dst_row;
			uint32_t y_weight;
			int row_status;

			row_status = direct_scale_rows_for_output(
				session, session->scaled_next_y,
				&src_row0, &src_row1, &y_weight);
			if (row_status == 0) {
				consumed = feed_consumed(session, src, src_length);
				fill_direct_scaled_progress(
					session, SDK_IMAGE_SESSION_STATE_NEED_INPUT,
					consumed, direct_start_y,
					direct_rows_written, bytes_per_pixel,
					result);
				return SDK_STATUS_OK;
			}
			if (row_status < 0)
				return SDK_STATUS_IO_ERROR;

			dst_row = (uint8_t *)(uintptr_t)
				(session->dst_address +
				 ((session->dst_y + session->scaled_next_y) *
				  session->dst_pitch) +
				 (session->dst_x * bytes_per_pixel));
			sdk_image_stream_scale_rows_bilinear_4byte(
				dst_row, src_row0, src_row1,
				session->decode_width, session->output_width,
				y_weight);
			session->scaled_next_y++;
			direct_rows_written++;
			if (direct_rows_written >=
			    SDK_IMAGE_STREAM_DIRECT_SCALE_ROWS_PER_FEED &&
			    session->scaled_next_y < session->output_height) {
				consumed = feed_consumed(session, src, src_length);
				fill_direct_scaled_progress(
					session, SDK_IMAGE_SESSION_STATE_NEED_INPUT,
					consumed, direct_start_y,
					direct_rows_written, bytes_per_pixel,
					result);
				return SDK_STATUS_OK;
			}
		}

		if (session->scaled_next_y != session->output_height)
			return SDK_STATUS_IO_ERROR;

		status = finish_stream(session, src, src_length, result);
		if (status == SDK_STATUS_OK) {
			if (result->state == SDK_IMAGE_SESSION_STATE_COMPLETE) {
				fill_direct_scaled_progress(
					session, SDK_IMAGE_SESSION_STATE_COMPLETE,
					result->bytes_consumed, direct_start_y,
					direct_rows_written, bytes_per_pixel,
					result);
				result->bytes_written = session->output_width *
				                        session->output_height *
				                        bytes_per_pixel;
			} else if (direct_rows_written != 0U) {
				fill_direct_scaled_progress(
					session, result->state, result->bytes_consumed,
					direct_start_y, direct_rows_written,
					bytes_per_pixel, result);
			}
		}
		return status;
	}

	if (session->cinfo.output_scanline < session->cinfo.output_height) {
		consumed = feed_consumed(session, src, src_length);
		fill_result(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT,
		            consumed, result);
		return SDK_STATUS_OK;
	}

	return finish_stream(session, src, src_length, result);
}

void sdk_image_stream_init(void)
{
	size_t table_bytes =
	    SDK_MAX_IMAGE_SESSIONS * sizeof(struct SDKImageStreamSession);

	memset(image_sessions, 0, table_bytes);
	/* This can run before the scheduler stamps the coherent MMU
	 * attributes on the region; flush so the zeroed table is in DRAM
	 * whatever attributes the lines were filled under. */
	Xil_DCacheFlushRange((INTPTR)(uintptr_t)image_sessions, table_bytes);
	next_image_session_id = 1U;
}

uint32_t sdk_image_stream_active_count(void)
{
	uint32_t i;
	uint32_t count = 0;

	for (i = 0; i < SDK_MAX_IMAGE_SESSIONS; i++) {
		if (image_sessions[i].in_use)
			count++;
	}

	return count;
}

uint16_t sdk_image_stream_begin(const struct SDKImageStreamBegin *begin,
                                struct SDKImageStreamResult *result)
{
	struct SDKImageStreamSession *session;
	uint16_t status;

	if (!result)
		return SDK_STATUS_BAD_REQUEST;
	memset(result, 0, sizeof(*result));

	status = validate_begin(begin);
	if (status != SDK_STATUS_OK)
		return status;

	session = find_free_session();
	if (!session)
		return SDK_STATUS_NO_MEMORY;

	memset(session, 0, sizeof(*session));
	session->session = next_session_id();
	session->codec = begin->codec;
	session->output_mode = begin->output_mode;
	session->dst_surface = begin->dst_surface;
	session->dst_x = begin->dst_x;
	session->dst_y = begin->dst_y;
	session->dst_width = begin->dst_width;
	session->dst_height = begin->dst_height;
	session->output_format = begin->output_format;
	session->tile_handle = begin->tile_handle;
	session->tile_stride = begin->tile_stride;
	session->tile_rows = begin->tile_rows;
	session->flags = begin->flags;
	session->dst_address = begin->dst_address;
	session->dst_pitch = begin->dst_pitch;
	session->dst_length = begin->dst_length;
	session->tile_address = begin->tile_address;
	session->tile_length = begin->tile_length;
	session->core1_affine = begin->core1_affine ? 1U : 0U;
	session->in_use = 1U;

	fill_result(session, SDK_IMAGE_SESSION_STATE_NEED_INPUT, 0U, result);
	return SDK_STATUS_OK;
}

uint16_t sdk_image_stream_feed(const struct SDKImageStreamFeed *feed,
                               const uint8_t *src,
                               struct SDKImageStreamResult *result)
{
	struct SDKImageStreamSession *session;

	if (!feed || !result || feed->session == 0U ||
	    feed->src_handle == SDK_INVALID_HANDLE ||
	    (feed->flags & ~SDK_IMAGE_SESSION_FEED_EOF) != 0U ||
	    (feed->src_length == 0U &&
	     (feed->flags & SDK_IMAGE_SESSION_FEED_EOF) == 0U)) {
		return SDK_STATUS_BAD_REQUEST;
	}
	memset(result, 0, sizeof(*result));

	session = find_session(feed->session);
	if (!session)
		return SDK_STATUS_BAD_HANDLE;
	if (feed->src_length != 0U && !src)
		return SDK_STATUS_BAD_REQUEST;
	if (feed->src_length > (0xffffffffU - session->compressed_bytes))
		return SDK_STATUS_BAD_REQUEST;

	if (session->failed)
		return SDK_STATUS_IO_ERROR;
	if (session->codec == SDK_IMAGE_CODEC_PNG &&
	    feed->src_length > SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES) {
		return SDK_STATUS_BAD_REQUEST;
	}

	session->input_eof =
		(feed->flags & SDK_IMAGE_SESSION_FEED_EOF) != 0U ? 1U : 0U;

	if (session->codec == SDK_IMAGE_CODEC_PNG)
		return process_png_stream(session, src, feed->src_length,
		                          result);
	return process_jpeg_stream(session, src, feed->src_length, result);
}

uint16_t sdk_image_stream_close(uint32_t session)
{
	struct SDKImageStreamSession *slot = find_session(session);

	if (!slot)
		return SDK_STATUS_BAD_HANDLE;

	destroy_jpeg(slot);
	destroy_png(slot);
	memset(slot, 0, sizeof(*slot));
	return SDK_STATUS_OK;
}

int sdk_image_stream_session_core1(uint32_t session)
{
	struct SDKImageStreamSession *slot = find_session(session);

	if (!slot)
		return -1;
	return slot->core1_affine ? 1 : 0;
}

/*
 * After a core-1 fault, the decode-reclaim pass frees every heap block
 * core-1 decodes held (libjpeg pools, libpng structs, the interlace
 * buffer). Drop the dangling references of core-1-affine sessions WITHOUT
 * running the codec destructors -- destroying against reclaimed memory
 * would be a use-after-free -- and mark the sessions failed so subsequent
 * feeds report IO_ERROR and close reduces to a plain slot reset.
 */
void sdk_image_stream_poison_core1_sessions(void)
{
	uint32_t i;

	for (i = 0; i < SDK_MAX_IMAGE_SESSIONS; i++) {
		struct SDKImageStreamSession *slot = &image_sessions[i];

		if (!slot->in_use || !slot->core1_affine)
			continue;
		slot->jpeg_created = 0U;
		slot->png_created = 0U;
		slot->png_ptr = 0;
		slot->png_info = 0;
		slot->png_interlace_buffer = 0;
		slot->png_interlace_buffer_length = 0U;
		slot->failed = 1U;
	}
}
