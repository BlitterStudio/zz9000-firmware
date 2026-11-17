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
#define SDK_JPEG_INPUT_BYTES_PER_STEP 2048U
/* iMCU rows of coefficient work a decode step may absorb before the
 * progress monitor forces the entropy decoder to suspend. */
#define SDK_JPEG_WORK_ROWS_PER_STEP 16U
static struct SDKJpegDecode *sdk_jpeg_from_cinfo(j_common_ptr cinfo)
{
	return (struct SDKJpegDecode *)((uint8_t *)cinfo->err -
		offsetof(struct SDKJpegDecode, error));
}

static void sdk_jpeg_error_exit(j_common_ptr cinfo)
{
	longjmp(sdk_jpeg_from_cinfo(cinfo)->jump, 1);
}

static void sdk_jpeg_source_init(j_decompress_ptr cinfo)
{
	(void)cinfo;
}

static boolean sdk_jpeg_source_fill(j_decompress_ptr cinfo)
{
	static const JOCTET eoi[] = { 0xffU, JPEG_EOI };
	struct SDKJpegDecode *decode = sdk_jpeg_from_cinfo((j_common_ptr)cinfo);
	uint32_t available;

	while (decode->skip_pending != 0U) {
		if (decode->jpeg_offset == decode->jpeg_length) {
			decode->skip_pending = 0U;
			break;
		}
		if (decode->input_budget == 0U)
			return FALSE;
		available = decode->jpeg_length - decode->jpeg_offset;
		if (available > decode->skip_pending)
			available = decode->skip_pending;
		if (available > decode->input_budget)
			available = decode->input_budget;
		decode->jpeg_offset += available;
		decode->input_budget -= available;
		decode->input_this_step += available;
		decode->skip_pending -= available;
	}
	if (decode->skip_pending != 0U)
		return FALSE;
	if (decode->jpeg_offset == decode->jpeg_length) {
		decode->source.next_input_byte = eoi;
		decode->source.bytes_in_buffer = sizeof(eoi);
		return TRUE;
	}
	if (decode->input_budget == 0U)
		return FALSE;
	available = decode->jpeg_length - decode->jpeg_offset;
	if (available > decode->input_budget)
		available = decode->input_budget;
	/* source_fill exposes bytes by advancing jpeg_offset; handing them
	 * back when the progress monitor suspends mid-buffer keeps the two in
	 * step.  Count them here so input_this_step reflects exposure, not
	 * consumption. */
	decode->source.next_input_byte = decode->jpeg + decode->jpeg_offset;
	decode->source.bytes_in_buffer = available;
	decode->jpeg_offset += available;
	decode->input_budget -= available;
	decode->input_this_step += available;
	return TRUE;
}

static void sdk_jpeg_source_skip(j_decompress_ptr cinfo, long bytes)
{
	struct SDKJpegDecode *decode = sdk_jpeg_from_cinfo((j_common_ptr)cinfo);
	size_t available = decode->source.bytes_in_buffer;

	if (bytes <= 0L)
		return;
	/* jdatasrc contract: consume what is buffered first; only the
	 * remainder crosses the step boundary as skip_pending.  Skipping
	 * the whole window unconditionally wrapped the subtraction whenever
	 * the marker was smaller than the window and lost the scan data. */
	if ((uint32_t)bytes <= available) {
		decode->source.next_input_byte += bytes;
		decode->source.bytes_in_buffer = available - (size_t)bytes;
		return;
	}
	decode->skip_pending += (uint32_t)bytes - (uint32_t)available;
	decode->source.next_input_byte += available;
	decode->source.bytes_in_buffer = 0U;
}

static void sdk_jpeg_progress_monitor(j_common_ptr cinfo)
{
	struct SDKJpegDecode *decode = sdk_jpeg_from_cinfo(cinfo);
	const JOCTET *next = decode->source.next_input_byte;
	size_t left = decode->source.bytes_in_buffer;

	/* jpeg_start_decompress preloads a progressive file by looping over
	 * consume_input, calling this hook before every call, and a scan's
	 * EOB runs can decode many blocks without consuming input, so the
	 * input budget alone does not bound a step's work.  Let N iMCU rows
	 * of coefficient work run, then empty the source buffer: the entropy
	 * decoder suspends on its next bit and the next step resumes here.
	 *
	 * Exposed-but-unread bytes must go back to jpeg_offset or the next
	 * fill would skip them; the range check keeps the synthetic EOI pair
	 * out of that give-back. */
	if (++decode->monitor_calls <= SDK_JPEG_WORK_ROWS_PER_STEP)
		return;
	if (next >= decode->jpeg &&
	    next + left <= decode->jpeg + decode->jpeg_length)
		decode->jpeg_offset -= (uint32_t)left;
	decode->monitor_suspended = 1U;
	decode->input_budget = 0U;
	decode->source.bytes_in_buffer = 0U;
}

static void sdk_jpeg_source_term(j_decompress_ptr cinfo)
{
	(void)cinfo;
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

void sdk_jpeg_decode_init(struct SDKJpegDecode *decode)
{
	if (decode)
		memset(decode, 0, sizeof(*decode));
}

void sdk_jpeg_decode_destroy(struct SDKJpegDecode *decode)
{
	if (!decode)
		return;
	if (decode->created)
		jpeg_destroy_decompress(&decode->cinfo);
	decode->created = 0U;
	decode->started = 0U;
}

int sdk_jpeg_decode_begin(struct SDKJpegDecode *decode, const uint8_t *jpeg,
			  uint32_t jpeg_length, uint8_t *dst,
			  uint32_t dst_width, uint32_t dst_height,
			  uint32_t dst_pitch, uint32_t dst_format,
			  uint32_t dst_x, uint32_t dst_y)
{
	if (!decode || !jpeg || jpeg_length == 0U || !dst)
		return 0;
	if (!sdk_jpeg_output_color_space(dst_format, &decode->color_space))
		return 0;
	decode->bytes_per_pixel = sdk_surface_format_bytes(dst_format);
	if (decode->bytes_per_pixel != 4U && decode->bytes_per_pixel != 3U)
		return 0;
	decode->jpeg = jpeg;
	decode->jpeg_length = jpeg_length;
	decode->dst = dst;
	decode->dst_width = dst_width;
	decode->dst_height = dst_height;
	decode->dst_pitch = dst_pitch;
	decode->dst_x = dst_x;
	decode->dst_y = dst_y;
	decode->cinfo.err = jpeg_std_error(&decode->error);
	decode->error.error_exit = sdk_jpeg_error_exit;
	decode->error.output_message = sdk_jpeg_output_message;
	if (setjmp(decode->jump)) {
		sdk_jpeg_decode_destroy(decode);
		return 0;
	}
	jpeg_create_decompress(&decode->cinfo);
	decode->created = 1U;
	decode->source.init_source = sdk_jpeg_source_init;
	decode->source.fill_input_buffer = sdk_jpeg_source_fill;
	decode->source.skip_input_data = sdk_jpeg_source_skip;
	decode->source.resync_to_restart = jpeg_resync_to_restart;
	decode->source.term_source = sdk_jpeg_source_term;
	decode->source.bytes_in_buffer = 0U;
	decode->source.next_input_byte = NULL;
	decode->progress.progress_monitor = sdk_jpeg_progress_monitor;
	decode->cinfo.progress = &decode->progress;
	decode->cinfo.src = &decode->source;
	return 1;
}

uint32_t sdk_jpeg_decode_monitor_suspended(const struct SDKJpegDecode *decode)
{
	return decode ? decode->monitor_suspended : 0U;
}

uint32_t sdk_jpeg_decode_input_bytes(const struct SDKJpegDecode *decode)
{
	return decode ? decode->input_this_step : 0U;
}

int sdk_jpeg_decode_step(struct SDKJpegDecode *decode, uint32_t max_rows,
			 uint32_t *rows_decoded, int *complete)
{
	volatile uint32_t rows = 0U;

	if (rows_decoded)
		*rows_decoded = 0U;
	if (complete)
		*complete = 0;
	if (!decode || !decode->created || max_rows == 0U)
		return 0;
	decode->input_budget = SDK_JPEG_INPUT_BYTES_PER_STEP;
	decode->input_this_step = 0U;
	decode->monitor_calls = 0U;
	decode->monitor_suspended = 0U;
	if (setjmp(decode->jump)) {
		sdk_jpeg_decode_destroy(decode);
		return 0;
	}
	if (!decode->header_ready) {
		if (jpeg_read_header(&decode->cinfo, TRUE) == JPEG_SUSPENDED)
			goto out;
		decode->width = (uint32_t)decode->cinfo.image_width;
		decode->height = (uint32_t)decode->cinfo.image_height;
		if (!sdk_jpeg_destination_valid(decode->width, decode->height,
						decode->dst_width, decode->dst_height,
						decode->dst_pitch, decode->dst_x,
						decode->dst_y, decode->bytes_per_pixel))
			goto failed;
		decode->cinfo.out_color_space = decode->color_space;
		decode->cinfo.dct_method = JDCT_IFAST;
		decode->header_ready = 1U;
	}
	if (!decode->started) {
		if (!jpeg_start_decompress(&decode->cinfo))
			goto out;
		if (decode->cinfo.output_components != (int)decode->bytes_per_pixel ||
		    decode->cinfo.output_width != decode->width ||
		    decode->cinfo.output_height != decode->height)
			goto failed;
		decode->started = 1U;
	}
	while (decode->cinfo.output_scanline < decode->cinfo.output_height &&
	       rows < max_rows) {
		JSAMPROW row = decode->dst +
			((decode->dst_y + decode->cinfo.output_scanline) *
			 decode->dst_pitch) +
			(decode->dst_x * decode->bytes_per_pixel);

		if (jpeg_read_scanlines(&decode->cinfo, &row, 1U) != 1U)
			goto out;
		rows++;
	}
	if (decode->cinfo.output_scanline == decode->cinfo.output_height) {
		if (!jpeg_finish_decompress(&decode->cinfo))
			goto out;
		decode->complete = 1U;
		sdk_jpeg_decode_destroy(decode);
		if (complete)
			*complete = 1;
	}
out:
	if (rows_decoded)
		*rows_decoded = rows;
	return 1;

failed:
	sdk_jpeg_decode_destroy(decode);
	return 0;
}

int sdk_jpeg_decode_to_surface(const uint8_t *jpeg, uint32_t jpeg_length,
                               uint8_t *dst, uint32_t dst_width,
                               uint32_t dst_height, uint32_t dst_pitch,
                               uint32_t dst_format, uint32_t dst_x,
                               uint32_t dst_y, uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_bytes_written)
{
	struct SDKJpegDecode decode;
	uint32_t rows;
	int complete = 0;

	sdk_jpeg_decode_init(&decode);
	if (!sdk_jpeg_decode_begin(&decode, jpeg, jpeg_length, dst, dst_width,
	                           dst_height, dst_pitch, dst_format, dst_x,
	                           dst_y))
		return 0;
	while (!complete) {
		if (!sdk_jpeg_decode_step(&decode, UINT32_MAX, &rows, &complete)) {
			sdk_jpeg_decode_destroy(&decode);
			return 0;
		}
	}
	if (out_width)
		*out_width = decode.width;
	if (out_height)
		*out_height = decode.height;
	if (out_bytes_written)
		*out_bytes_written = decode.width * decode.height *
		                     decode.bytes_per_pixel;
	return 1;
}
