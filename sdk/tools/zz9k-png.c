/*
 * PNG streaming decode smoke tool for the ZZ9000 SDK image service.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-fb-common.h"
#include "zz9k-image-window.h"
#include "zz9k-png-view.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/image.h"
#include "zz9k/shared.h"
#include "zz9k/surface.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZ9K_PNG_STAGING_BYTES (32UL * 1024UL)
#define ZZ9K_PNG_DEFAULT_CHUNK_BYTES (16UL * 1024UL)
#define ZZ9K_PNG_DEFAULT_FRAMEBUFFER_HOLD_TICKS 100U
#define ZZ9K_PNG_MAX_FRAMEBUFFER_HOLD_TICKS 1000U
#define ZZ9K_PNG_STEP_ALLOC_INPUT 1U
#define ZZ9K_PNG_STEP_PREPARE_OUTPUT 2U
#define ZZ9K_PNG_STEP_SESSION_BEGIN 3U
#define ZZ9K_PNG_STEP_FILE_OPEN 4U
#define ZZ9K_PNG_STEP_FIRST_FEED 5U
#define ZZ9K_PNG_WINDOW_MIN_WIDTH 180U
#define ZZ9K_PNG_WINDOW_MIN_HEIGHT 120U
#define ZZ9K_PNG_WINDOW_MARGIN_X 32U
#define ZZ9K_PNG_WINDOW_MARGIN_Y 48U

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
	defined(__VBCC__)
#define ZZ9K_PNG_AMIGA 1
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#else
#define ZZ9K_PNG_AMIGA 0
#endif

typedef struct ZZ9KPngHeader {
	uint32_t width;
	uint32_t height;
	uint8_t bit_depth;
	uint8_t color_type;
	uint8_t compression;
	uint8_t filter;
	uint8_t interlace;
} ZZ9KPngHeader;

typedef struct ZZ9KPngInput {
	const char *path;
	uint32_t length;
	uint32_t width;
	uint32_t height;
	uint32_t hold_ticks;
	uint32_t stop_after_step;
	uint32_t chunk_bytes;
	int use_framebuffer;
	int fit_framebuffer;
	int window_framebuffer;
	int resize_framebuffer;
	int restore_framebuffer;
	int view_framebuffer;
	int trace;
} ZZ9KPngInput;

typedef struct ZZ9KPngFramebufferBackup {
	ZZ9KFbRect rect;
	ZZ9KSurface surface;
	ZZ9KSurfaceCopyDesc save_copy;
	ZZ9KSurfaceCopyDesc restore_copy;
	int allocated;
	int active;
} ZZ9KPngFramebufferBackup;

typedef ZZ9KImageWindow ZZ9KPngWindow;

static uint32_t zz9k_png_read_be32(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) |
	       ((uint32_t)bytes[1] << 16) |
	       ((uint32_t)bytes[2] << 8) |
	       (uint32_t)bytes[3];
}

static int zz9k_png_signature_matches(const uint8_t *bytes)
{
	static const uint8_t signature[8] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
	};

	return memcmp(bytes, signature, sizeof(signature)) == 0;
}

static int zz9k_png_read_header(const uint8_t *bytes, uint32_t length,
                                ZZ9KPngHeader *out_header)
{
	ZZ9KPngHeader header;

	if (!bytes || !out_header || length < 33U)
		return 0;
	if (!zz9k_png_signature_matches(bytes))
		return 0;
	if (zz9k_png_read_be32(&bytes[8]) != 13U ||
	    memcmp(&bytes[12], "IHDR", 4U) != 0) {
		return 0;
	}

	memset(&header, 0, sizeof(header));
	header.width = zz9k_png_read_be32(&bytes[16]);
	header.height = zz9k_png_read_be32(&bytes[20]);
	header.bit_depth = bytes[24];
	header.color_type = bytes[25];
	header.compression = bytes[26];
	header.filter = bytes[27];
	header.interlace = bytes[28];
	if (header.width == 0U || header.height == 0U)
		return 0;

	*out_header = header;
	return 1;
}

static int zz9k_png_bit_depth_supported(uint8_t color_type,
                                        uint8_t bit_depth)
{
	switch (color_type) {
	case 0U:
		return bit_depth == 1U || bit_depth == 2U ||
		       bit_depth == 4U || bit_depth == 8U ||
		       bit_depth == 16U;
	case 2U:
		return bit_depth == 8U || bit_depth == 16U;
	case 3U:
		return bit_depth == 1U || bit_depth == 2U ||
		       bit_depth == 4U || bit_depth == 8U;
	case 4U:
	case 6U:
		return bit_depth == 8U || bit_depth == 16U;
	default:
		return 0;
	}
}

static int zz9k_png_header_supported(const ZZ9KPngHeader *header)
{
	if (!header || header->width == 0U || header->height == 0U)
		return 0;
	if (header->compression != 0U || header->filter != 0U ||
	    header->interlace != 0U) {
		return 0;
	}
	return zz9k_png_bit_depth_supported(header->color_type,
	                                    header->bit_depth);
}

static int zz9k_png_fit_size_to_area(uint32_t src_w, uint32_t src_h,
                                     uint32_t area_w, uint32_t area_h,
                                     uint32_t *out_w, uint32_t *out_h)
{
	return zz9k_image_window_fit_size_to_area(
		src_w, src_h, area_w, area_h, out_w, out_h);
}

static int zz9k_png_choose_framebuffer_rect(
	const ZZ9KSurface *framebuffer,
	const ZZ9KPngInput *png_input,
	ZZ9KFbRect *rect)
{
	uint32_t width;
	uint32_t height;
	uint32_t bpp;

	if (!framebuffer || !png_input || !rect ||
	    framebuffer->width == 0U || framebuffer->height == 0U ||
	    png_input->width == 0U || png_input->height == 0U) {
		return 0;
	}

	width = png_input->width;
	height = png_input->height;
	if (png_input->fit_framebuffer) {
		if (width > framebuffer->width || height > framebuffer->height) {
			if (!zz9k_png_fit_size_to_area(
				    width, height, framebuffer->width,
				    framebuffer->height, &width, &height)) {
				return 0;
			}
		}
		rect->x = (framebuffer->width - width) / 2U;
		rect->y = (framebuffer->height - height) / 2U;
	} else {
		rect->x = 0U;
		rect->y = 0U;
	}
	rect->w = width;
	rect->h = height;

	bpp = zz9k_fb_bytes_per_pixel(framebuffer->format);
	return bpp != 0U && zz9k_fb_rect_fits(framebuffer, rect, bpp);
}

static int zz9k_png_build_framebuffer_scale_desc(
	ZZ9KScaleImageDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect)
{
	return zz9k_image_window_build_scale_desc(
		desc, source_handle, source_width, source_height, draw_rect,
		ZZ9K_SCALE_BILINEAR);
}

static int zz9k_png_choose_draw_rect_in_area(
	const ZZ9KFbRect *area,
	uint32_t src_width,
	uint32_t src_height,
	ZZ9KFbRect *rect)
{
	return zz9k_image_window_choose_draw_rect_in_area(
		area, src_width, src_height, rect);
}

static uint32_t zz9k_png_scale_slice_rows(void)
{
	return zz9k_image_window_scale_slice_rows();
}

static uint32_t zz9k_png_count_scale_slices(const ZZ9KFbRect *clip_rect)
{
	return zz9k_image_window_count_scale_slices(clip_rect);
}

static int zz9k_png_build_framebuffer_clipped_scale_desc_for_rect(
	ZZ9KScaleImageClippedDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect)
{
	return zz9k_image_window_build_clipped_scale_desc(
		desc, source_handle, source_width, source_height,
		draw_rect, clip_rect, ZZ9K_SCALE_BILINEAR);
}

static int zz9k_png_choose_window_origin(
	uint32_t screen_width,
	uint32_t screen_height,
	uint32_t window_width,
	uint32_t window_height,
	uint32_t *left,
	uint32_t *top)
{
	return zz9k_image_window_choose_origin(
		screen_width, screen_height, window_width, window_height,
		left, top);
}

static int zz9k_png_choose_window_max_extent(
	uint32_t framebuffer_width,
	uint32_t framebuffer_height,
	uint32_t left,
	uint32_t top,
	uint32_t min_width,
	uint32_t min_height,
	uint32_t *max_width,
	uint32_t *max_height)
{
	return zz9k_image_window_choose_max_extent(
		framebuffer_width, framebuffer_height, left, top,
		min_width, min_height, max_width, max_height);
}

static int zz9k_png_should_stop_after(const ZZ9KPngInput *input,
                                      uint32_t step)
{
	return input && input->stop_after_step != 0U &&
	       input->stop_after_step == step;
}

static void zz9k_png_flush(void)
{
	fflush(stdout);
}

static void zz9k_png_trace_step(const ZZ9KPngInput *input,
                                uint32_t step, const char *label)
{
	if (input && input->trace) {
		printf("zz9k-png: step %lu: %s\n",
		       (unsigned long)step, label);
		zz9k_png_flush();
	}
}

static void zz9k_png_trace_step_ok(const ZZ9KPngInput *input,
                                   uint32_t step)
{
	if (input && input->trace) {
		printf("zz9k-png: step %lu: ok\n", (unsigned long)step);
		zz9k_png_flush();
	}
}

static int zz9k_png_parse_u32(const char *text, uint32_t *value)
{
	uint32_t result = 0U;

	if (!text || !*text || !value)
		return 0;
	while (*text) {
		if (*text < '0' || *text > '9')
			return 0;
		if (result > ((0xffffffffU - (uint32_t)(*text - '0')) / 10U))
			return 0;
		result = (result * 10U) + (uint32_t)(*text - '0');
		text++;
	}
	*value = result;
	return 1;
}

static int zz9k_png_parse_hold_ticks(const char *text, uint32_t *value)
{
	uint32_t parsed;

	if (!value || !zz9k_png_parse_u32(text, &parsed))
		return 0;
	if (parsed > ZZ9K_PNG_MAX_FRAMEBUFFER_HOLD_TICKS)
		parsed = ZZ9K_PNG_MAX_FRAMEBUFFER_HOLD_TICKS;
	*value = parsed;
	return 1;
}

static int zz9k_png_parse_stop_after(const char *text, uint32_t *value)
{
	uint32_t parsed;

	if (!value || !zz9k_png_parse_u32(text, &parsed) || parsed == 0U)
		return 0;
	*value = parsed;
	return 1;
}

static int zz9k_png_parse_chunk_bytes(const char *text, uint32_t *value)
{
	uint32_t parsed;

	if (!value || !zz9k_png_parse_u32(text, &parsed) || parsed == 0U ||
	    parsed > ZZ9K_PNG_STAGING_BYTES) {
		return 0;
	}
	*value = parsed;
	return 1;
}

static int zz9k_png_read_file_exact(FILE *file, uint8_t *dst,
                                    uint32_t length)
{
	return fread(dst, 1U, length, file) == length;
}

static void zz9k_png_usage(void)
{
	printf("usage: zz9k-png [--view|--fb [--fit] [--window|--resize] "
	       "[--hold N] [--keep]] "
	       "[--trace] [--chunk N] [--stop-after N] file.png\n");
	printf("       --fit scales oversized framebuffer output with the ARM scaler\n");
	printf("       --window draws inside an Intuition window; --resize makes it resizable\n");
	printf("       --view opens a resizable layer-aware viewer window until close\n");
	printf("       interlaced PNG is not supported yet\n");
	printf("       default PNG stream chunk is 16 KiB\n");
	printf("       trace steps: 1=input alloc, 2=output setup, "
	       "3=session begin, 4=file open, 5=first feed\n");
}

static int zz9k_png_load_file(const char *path, ZZ9KPngInput *input)
{
	FILE *file;
	long file_size;
	uint8_t header_bytes[33];
	ZZ9KPngHeader header;

	if (!path || !input)
		return 0;

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-png: failed to open '%s'\n", path);
		return 0;
	}
	if (fseek(file, 0L, SEEK_END) != 0) {
		printf("zz9k-png: failed to seek '%s'\n", path);
		fclose(file);
		return 0;
	}
	file_size = ftell(file);
	if (file_size < (long)sizeof(header_bytes) ||
	    (unsigned long)file_size > 0xffffffffUL) {
		printf("zz9k-png: unsupported input size for '%s'\n", path);
		fclose(file);
		return 0;
	}
	if (fseek(file, 0L, SEEK_SET) != 0) {
		printf("zz9k-png: failed to rewind '%s'\n", path);
		fclose(file);
		return 0;
	}
	if (!zz9k_png_read_file_exact(file, header_bytes,
	                              (uint32_t)sizeof(header_bytes)) ||
	    !zz9k_png_read_header(header_bytes,
	                          (uint32_t)sizeof(header_bytes), &header)) {
		printf("zz9k-png: could not read PNG header from '%s'\n", path);
		fclose(file);
		return 0;
	}
	if (!zz9k_png_header_supported(&header)) {
		printf("zz9k-png: unsupported PNG header in '%s'\n", path);
		fclose(file);
		return 0;
	}
	fclose(file);

	input->path = path;
	input->length = (uint32_t)file_size;
	input->width = header.width;
	input->height = header.height;
	return 1;
}

static int zz9k_png_parse_args(int argc, char **argv, ZZ9KPngInput *input)
{
	int i;

	if (!input)
		return 0;

	memset(input, 0, sizeof(*input));
	input->hold_ticks = ZZ9K_PNG_DEFAULT_FRAMEBUFFER_HOLD_TICKS;
	input->chunk_bytes = ZZ9K_PNG_DEFAULT_CHUNK_BYTES;
	input->restore_framebuffer = 1;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fb") == 0) {
			input->use_framebuffer = 1;
		} else if (strcmp(argv[i], "--view") == 0) {
			input->use_framebuffer = 1;
			input->window_framebuffer = 1;
			input->resize_framebuffer = 1;
			input->view_framebuffer = 1;
		} else if (strcmp(argv[i], "--fit") == 0) {
			input->fit_framebuffer = 1;
		} else if (strcmp(argv[i], "--window") == 0) {
			input->window_framebuffer = 1;
		} else if (strcmp(argv[i], "--resize") == 0) {
			input->window_framebuffer = 1;
			input->resize_framebuffer = 1;
		} else if (strcmp(argv[i], "--keep") == 0) {
			input->restore_framebuffer = 0;
		} else if (strcmp(argv[i], "--hold") == 0) {
			if (++i >= argc ||
			    !zz9k_png_parse_hold_ticks(argv[i],
			                               &input->hold_ticks)) {
				zz9k_png_usage();
				return 0;
			}
		} else if (strcmp(argv[i], "--trace") == 0) {
			input->trace = 1;
		} else if (strcmp(argv[i], "--stop-after") == 0) {
			if (++i >= argc ||
			    !zz9k_png_parse_stop_after(
				    argv[i], &input->stop_after_step)) {
				zz9k_png_usage();
				return 0;
			}
		} else if (strcmp(argv[i], "--chunk") == 0) {
			if (++i >= argc ||
			    !zz9k_png_parse_chunk_bytes(
				    argv[i], &input->chunk_bytes)) {
				zz9k_png_usage();
				return 0;
			}
		} else if (strcmp(argv[i], "-h") == 0 ||
		           strcmp(argv[i], "--help") == 0) {
			zz9k_png_usage();
			return 0;
		} else if (argv[i][0] == '-') {
			zz9k_png_usage();
			return 0;
		} else if (!input->path) {
			if (!zz9k_png_load_file(argv[i], input))
				return 0;
		} else {
			zz9k_png_usage();
			return 0;
		}
	}
	if (!input->path) {
		zz9k_png_usage();
		return 0;
	}
	if (!input->use_framebuffer && !input->restore_framebuffer) {
		zz9k_png_usage();
		return 0;
	}
	if (input->fit_framebuffer && !input->use_framebuffer) {
		zz9k_png_usage();
		return 0;
	}
	if (input->window_framebuffer && !input->use_framebuffer) {
		zz9k_png_usage();
		return 0;
	}
	if (input->resize_framebuffer && !input->window_framebuffer) {
		zz9k_png_usage();
		return 0;
	}

	return 1;
}

static void zz9k_png_delay_ticks(uint32_t ticks)
{
#if ZZ9K_PNG_AMIGA
	if (ticks != 0U)
		Delay((LONG)ticks);
#else
	volatile uint32_t spin;
	uint32_t i;

	for (i = 0; i < ticks; i++) {
		for (spin = 0; spin < 1000000UL; spin++) {
		}
	}
#endif
}

static int zz9k_png_require_cap(uint32_t caps, uint32_t bit)
{
	const char *name;

	if ((caps & bit) != 0U)
		return 1;

	name = zz9k_capability_name(bit);
	printf("zz9k-png: missing required capability: %s\n",
	       name ? name : "unknown");
	return 0;
}

static int zz9k_png_require_stream_service(ZZ9KContext *ctx,
                                           int use_framebuffer)
{
	ZZ9KServiceInfo service;
	uint32_t required;
	int status;

	memset(&service, 0, sizeof(service));
	status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: image service query failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}

	if (!zz9k_image_stream_required_service_flags(
		    ZZ9K_IMAGE_CODEC_PNG,
		    use_framebuffer ? ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER :
		                      ZZ9K_IMAGE_OUTPUT_SURFACE,
		    &required)) {
		printf("zz9k-png: could not build required image service "
		       "flags\n");
		return 0;
	}
	if (!zz9k_has_service_flags(service.flags, required)) {
		printf("zz9k-png: firmware does not advertise PNG direct "
		       "streaming support\n");
		return 0;
	}

	return 1;
}

static int zz9k_png_require_clipped_scale_service(ZZ9KContext *ctx)
{
	ZZ9KServiceInfo service;
	int status;

	memset(&service, 0, sizeof(service));
	status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: image service query failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}
	if (!zz9k_image_service_supports_clipped_scale(
		    service.opcode_count, service.flags, ZZ9K_SCALE_BILINEAR)) {
		printf("zz9k-png: firmware does not advertise clipped "
		       "bilinear scale\n");
		return 0;
	}
	return 1;
}

static uint32_t zz9k_png_feed_capacity(const ZZ9KPngInput *png_input,
                                       const ZZ9KSharedBuffer *staging)
{
	uint32_t capacity;

	if (!png_input || !staging)
		return 0U;
	capacity = staging->length;
	if (png_input->chunk_bytes != 0U &&
	    png_input->chunk_bytes < capacity) {
		capacity = png_input->chunk_bytes;
	}
	return capacity;
}

static int zz9k_png_read_chunk_to_shared(FILE *file,
                                         ZZ9KSharedBuffer *staging,
                                         uint32_t offset,
                                         uint32_t length)
{
	uint8_t scratch[1024];
	uint32_t copied = 0U;

	if (!file || !staging)
		return 0;
	while (copied < length) {
		uint32_t want = length - copied;
		size_t bytes_read;

		if (want > sizeof(scratch))
			want = (uint32_t)sizeof(scratch);
		bytes_read = fread(scratch, 1U, want, file);
		if (bytes_read == 0U)
			return 0;
		if (!zz9k_shared_copy_to(staging, offset + copied, scratch,
		                         (uint32_t)bytes_read))
			return 0;
		copied += (uint32_t)bytes_read;
	}

	return 1;
}

static int zz9k_png_fill_staging(FILE *file,
                                 const ZZ9KPngInput *png_input,
                                 ZZ9KSharedBuffer *staging,
                                 uint32_t *file_offset,
                                 uint32_t *buffered)
{
	uint32_t remaining_file;
	uint32_t capacity;
	uint32_t space;
	uint32_t want;

	if (!file || !png_input || !staging || !file_offset || !buffered)
		return 0;
	capacity = zz9k_png_feed_capacity(png_input, staging);
	if (capacity == 0U || *file_offset > png_input->length ||
	    *buffered > capacity) {
		return 0;
	}

	remaining_file = png_input->length - *file_offset;
	space = capacity - *buffered;
	if (remaining_file == 0U || space == 0U)
		return 1;

	want = remaining_file < space ? remaining_file : space;
	if (!zz9k_png_read_chunk_to_shared(file, staging, *buffered, want)) {
		printf("zz9k-png: short read from '%s'\n", png_input->path);
		return 0;
	}

	*file_offset += want;
	*buffered += want;
	return 1;
}

static int zz9k_png_stream_result_made_progress(
	const ZZ9KImageSessionResult *result)
{
	if (!result)
		return 0;

	return result->bytes_consumed != 0U || result->bytes_written != 0U ||
	       result->state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE;
}

static int zz9k_png_stream_no_progress_is_fatal(
	const ZZ9KImageSessionResult *result, uint32_t buffered,
	uint32_t capacity, int eof)
{
	if (zz9k_png_stream_result_made_progress(result))
		return 0;
	if (!eof && buffered < capacity)
		return 0;
	return 1;
}

static int zz9k_png_feed_stream(ZZ9KContext *ctx, FILE *file,
                                const ZZ9KPngInput *png_input,
                                ZZ9KSharedBuffer *staging,
                                uint32_t session,
                                ZZ9KImageSessionResult *final_result,
                                int *stopped)
{
	uint32_t file_offset = 0U;
	uint32_t buffered = 0U;
	uint32_t empty_eof_feeds = 0U;
	uint32_t feeds = 0U;

	memset(final_result, 0, sizeof(*final_result));
	if (stopped)
		*stopped = 0;
	while (final_result->state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
		uint32_t consumed;
		uint32_t feed_capacity;
		int eof;

		if (!zz9k_png_fill_staging(file, png_input, staging,
		                           &file_offset, &buffered)) {
			return 0;
		}
		feed_capacity = zz9k_png_feed_capacity(png_input, staging);
		eof = file_offset == png_input->length;
		if (buffered == 0U && eof) {
			if (empty_eof_feeds > png_input->height + 8U) {
				printf("zz9k-png: stream exceeded EOF drain limit\n");
				return 0;
			}
			empty_eof_feeds++;
		}

		do {
			ZZ9KImageSessionFeedDesc feed;
			ZZ9KImageSessionResult result;
			int status;

			if (!zz9k_image_build_session_feed_desc(
				    &feed, session, staging->handle, 0U,
				    buffered,
				    eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U)) {
				printf("zz9k-png: could not build stream feed "
				       "descriptor\n");
				return 0;
			}

			memset(&result, 0, sizeof(result));
			if (feeds == 0U) {
				if (png_input->trace) {
					printf("zz9k-png: step %lu: submitting "
					       "first stream feed buffered=%lu "
					       "file=%lu/%lu eof=%lu chunk=%lu\n",
					       (unsigned long)
					       ZZ9K_PNG_STEP_FIRST_FEED,
					       (unsigned long)buffered,
					       (unsigned long)file_offset,
					       (unsigned long)png_input->length,
					       (unsigned long)eof,
					       (unsigned long)feed_capacity);
					zz9k_png_flush();
				}
			} else if (png_input->trace) {
				printf("zz9k-png: feed %lu: buffered=%lu "
				       "file=%lu/%lu eof=%lu chunk=%lu\n",
				       (unsigned long)(feeds + 1U),
				       (unsigned long)buffered,
				       (unsigned long)file_offset,
				       (unsigned long)png_input->length,
				       (unsigned long)eof,
				       (unsigned long)feed_capacity);
				zz9k_png_flush();
			}
			status = zz9k_image_session_feed(ctx, &feed, &result);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-png: stream feed failed: %s (%d)\n",
				       zz9k_status_name(status), status);
				return 0;
			}
			feeds++;
			if (feeds == 1U)
				zz9k_png_trace_step_ok(
					png_input, ZZ9K_PNG_STEP_FIRST_FEED);
			if (feeds == 1U &&
			    zz9k_png_should_stop_after(
				    png_input, ZZ9K_PNG_STEP_FIRST_FEED)) {
				*final_result = result;
				if (stopped)
					*stopped = 1;
				return 1;
			}
			if (result.bytes_consumed > buffered) {
				printf("zz9k-png: stream consumed beyond input chunk\n");
				return 0;
			}
			consumed = result.bytes_consumed;
			if (consumed != 0U) {
				buffered -= consumed;
				if (buffered != 0U) {
					if (!zz9k_shared_move(staging, 0U, consumed,
					                      buffered)) {
						printf("zz9k-png: stream input compaction "
						       "failed\n");
						return 0;
					}
				}
			}

			if (result.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
				*final_result = result;
				break;
			}
			if (result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT &&
			    result.state != ZZ9K_IMAGE_SESSION_STATE_HEADER_READY) {
				printf("zz9k-png: stream decode returned state %lu\n",
				       (unsigned long)result.state);
				return 0;
			}
			if (zz9k_png_stream_no_progress_is_fatal(
				    &result, buffered, feed_capacity, eof)) {
				printf("zz9k-png: stream made no input progress "
				       "(state=%lu consumed=%lu written=%lu "
				       "buffered=%lu file=%lu/%lu eof=%lu)\n",
				       (unsigned long)result.state,
				       (unsigned long)result.bytes_consumed,
				       (unsigned long)result.bytes_written,
				       (unsigned long)buffered,
				       (unsigned long)file_offset,
				       (unsigned long)png_input->length,
				       (unsigned long)eof);
				return 0;
			}

			if (buffered == 0U ||
			    (!zz9k_png_stream_result_made_progress(&result) &&
			     !eof)) {
				break;
			}
		} while (1);
	}

	return 1;
}

static int zz9k_png_build_backup_copy_descs(
	const ZZ9KSurface *backup_surface,
	const ZZ9KFbRect *rect,
	ZZ9KSurfaceCopyDesc *save_copy,
	ZZ9KSurfaceCopyDesc *restore_copy)
{
	return zz9k_fb_build_framebuffer_backup_copy_descs(
		backup_surface, rect, save_copy, restore_copy);
}

static void zz9k_png_framebuffer_backup_init(
	ZZ9KPngFramebufferBackup *backup)
{
	memset(backup, 0, sizeof(*backup));
}

static int zz9k_png_framebuffer_backup_prepare(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KFbRect *rect,
	ZZ9KPngFramebufferBackup *backup)
{
	uint32_t bpp;
	int status;

	if (!ctx || !framebuffer || !rect || !backup)
		return 0;

	bpp = zz9k_fb_bytes_per_pixel(framebuffer->format);
	backup->rect = *rect;
	if (bpp != 4U ||
	    !zz9k_fb_rect_fits(framebuffer, &backup->rect, bpp) ||
	    backup->rect.w > (0xffffffffU / bpp)) {
		printf("zz9k-png: invalid framebuffer backup rectangle\n");
		return 0;
	}

	memset(&backup->surface, 0, sizeof(backup->surface));
	status = zz9k_alloc_surface_ex(ctx, backup->rect.w, backup->rect.h,
	                               framebuffer->format,
	                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
	                               backup->rect.w * bpp,
	                               &backup->surface);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: framebuffer backup surface alloc failed: "
		       "%s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}
	backup->allocated = 1;
	if (!zz9k_png_build_backup_copy_descs(
		    &backup->surface, &backup->rect,
		    &backup->save_copy, &backup->restore_copy)) {
		printf("zz9k-png: framebuffer backup descriptor build failed\n");
		return 0;
	}

	status = zz9k_copy_surface(ctx, &backup->save_copy);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: framebuffer backup copy failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}
	backup->active = 1;
	return 1;
}

static int zz9k_png_framebuffer_restore(
	ZZ9KContext *ctx,
	ZZ9KPngFramebufferBackup *backup)
{
	int status;

	if (!ctx || !backup || !backup->active)
		return ZZ9K_STATUS_OK;
	status = zz9k_copy_surface(ctx, &backup->restore_copy);
	if (status == ZZ9K_STATUS_OK)
		backup->active = 0;
	return status;
}

static int zz9k_png_framebuffer_restore_visible(
	ZZ9KContext *ctx,
	const ZZ9KImageWindow *ui,
	ZZ9KPngFramebufferBackup *backup)
{
	ZZ9KFbRect clips[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
	uint32_t clip_count;
	uint32_t i;

	if (!ctx || !ui || !backup || !backup->active)
		return ZZ9K_STATUS_OK;
	if (!zz9k_image_window_visible_clips(
		    ui, &backup->rect, clips, ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS,
		    &clip_count)) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}
	for (i = 0U; i < clip_count; i++) {
		ZZ9KSurfaceCopyDesc restore_clip;
		int status;

		if (!zz9k_fb_build_framebuffer_backup_restore_clip_desc(
			    &backup->surface, &backup->rect, &clips[i],
			    &restore_clip)) {
			return ZZ9K_STATUS_BAD_REQUEST;
		}
		status = zz9k_copy_surface(ctx, &restore_clip);
		if (status != ZZ9K_STATUS_OK)
			return status;
	}
	backup->active = 0;
	return ZZ9K_STATUS_OK;
}

static void zz9k_png_framebuffer_backup_free(
	ZZ9KContext *ctx,
	ZZ9KPngFramebufferBackup *backup)
{
	if (!ctx || !backup)
		return;
	if (backup->allocated) {
		zz9k_free_surface(ctx, backup->surface.handle);
		backup->allocated = 0;
	}
	backup->active = 0;
}

static int zz9k_png_framebuffer_restore_and_free(
	ZZ9KContext *ctx,
	ZZ9KPngFramebufferBackup *backup)
{
	int status;

	if (!backup)
		return ZZ9K_STATUS_OK;
	status = zz9k_png_framebuffer_restore(ctx, backup);
	if (status != ZZ9K_STATUS_OK)
		return status;
	zz9k_png_framebuffer_backup_free(ctx, backup);
	return ZZ9K_STATUS_OK;
}

static int zz9k_png_framebuffer_restore_visible_and_free(
	ZZ9KContext *ctx,
	const ZZ9KImageWindow *ui,
	ZZ9KPngFramebufferBackup *backup)
{
	int status;

	if (!backup)
		return ZZ9K_STATUS_OK;
	status = zz9k_png_framebuffer_restore_visible(ctx, ui, backup);
	if (status != ZZ9K_STATUS_OK)
		return status;
	zz9k_png_framebuffer_backup_free(ctx, backup);
	return ZZ9K_STATUS_OK;
}

static int zz9k_png_open_window(const ZZ9KSurface *framebuffer,
                                const ZZ9KPngInput *png_input,
                                ZZ9KPngWindow *ui)
{
	ZZ9KImageWindowConfig config;

	if (!framebuffer || !png_input || !ui)
		return 0;
	zz9k_image_window_config_init(
		&config, "ZZ9000 SDK PNG", png_input->width, png_input->height,
		png_input->fit_framebuffer, png_input->resize_framebuffer,
		ZZ9K_PNG_WINDOW_MIN_WIDTH, ZZ9K_PNG_WINDOW_MIN_HEIGHT,
		ZZ9K_PNG_WINDOW_MARGIN_X, ZZ9K_PNG_WINDOW_MARGIN_Y);
	return zz9k_image_window_open(framebuffer, &config, ui);
}

static int zz9k_png_poll_window(ZZ9KPngWindow *ui,
                                const ZZ9KSurface *framebuffer,
                                int *changed,
                                int *closed)
{
	return zz9k_image_window_poll(ui, framebuffer, changed, closed);
}

static void zz9k_png_close_window(ZZ9KPngWindow *ui)
{
	zz9k_image_window_close(ui);
}

static int zz9k_png_scale_framebuffer_sliced(
	ZZ9KContext *ctx,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect)
{
	return zz9k_image_window_scale_sliced(
		ctx, source_handle, source_width, source_height,
		draw_rect, clip_rect, ZZ9K_SCALE_BILINEAR);
}

static int zz9k_png_render_source_to_area(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KPngInput *png_input,
	const ZZ9KSurface *source,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KPngWindow *ui,
	const ZZ9KFbRect *area,
	ZZ9KPngFramebufferBackup *backup)
{
	ZZ9KSurfaceFillDesc fill;
	ZZ9KFbRect clips[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
	ZZ9KFbRect draw_rect;
	uint32_t clip_count;
	uint32_t i;
	int status;

	if (!ctx || !framebuffer || !png_input || !source || !ui || !area ||
	    !backup || source_width == 0U || source_height == 0U ||
	    area->w == 0U || area->h == 0U) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}
	if (!zz9k_png_choose_draw_rect_in_area(area, source_width,
	                                       source_height, &draw_rect)) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}

	if (png_input->resize_framebuffer && backup->active) {
		zz9k_png_framebuffer_backup_free(ctx, backup);
	} else {
		status = zz9k_png_framebuffer_restore_visible_and_free(
			ctx, ui, backup);
		if (status != ZZ9K_STATUS_OK)
			return status;
	}
	if (png_input->restore_framebuffer &&
	    !zz9k_png_framebuffer_backup_prepare(ctx, framebuffer, area,
	                                         backup)) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}

	if (!zz9k_image_window_visible_clips(
		    ui, area, clips, ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS,
		    &clip_count)) {
		return ZZ9K_STATUS_INTERNAL_ERROR;
	}

	for (i = 0U; i < clip_count; i++) {
		if (!zz9k_image_window_build_framebuffer_fill_desc(
			    &fill, &clips[i], zz9k_surface_color_rgb(0U, 0U, 0U),
			    0U)) {
			return ZZ9K_STATUS_BAD_REQUEST;
		}
		status = zz9k_fill_surface(ctx, &fill);
		if (status != ZZ9K_STATUS_OK)
			return status;

		status = zz9k_png_scale_framebuffer_sliced(
			ctx, source->handle, source_width, source_height,
			&draw_rect, &clips[i]);
		if (status != ZZ9K_STATUS_OK)
			return status;
	}

	return ZZ9K_STATUS_OK;
}

static int zz9k_png_show_decoded_surface(
	ZZ9KContext *ctx,
	const ZZ9KSurface *framebuffer,
	const ZZ9KPngInput *png_input,
	const ZZ9KSurface *source,
	uint32_t source_width,
	uint32_t source_height,
	ZZ9KPngFramebufferBackup *backup)
{
	ZZ9KPngWindow ui;
	uint32_t tick;
	int status;
	int rc = 1;

	memset(&ui, 0, sizeof(ui));
	if (!zz9k_png_open_window(framebuffer, png_input, &ui))
		return 0;

	printf("zz9k-png: window area %lu,%lu %lu x %lu\n",
	       (unsigned long)ui.inner.x, (unsigned long)ui.inner.y,
	       (unsigned long)ui.inner.w, (unsigned long)ui.inner.h);
	status = zz9k_png_render_source_to_area(ctx, framebuffer, png_input,
	                                        source, source_width,
	                                        source_height, &ui,
	                                        &ui.inner,
	                                        backup);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: framebuffer scale failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}

	if (png_input->view_framebuffer) {
		printf("zz9k-png: showing until close using bilinear scale, "
		       "%lu scale slices\n",
		       (unsigned long)zz9k_png_count_scale_slices(&ui.inner));
	} else {
		printf("zz9k-png: showing for %lu ticks using bilinear scale, "
		       "%lu scale slices\n",
		       (unsigned long)png_input->hold_ticks,
		       (unsigned long)zz9k_png_count_scale_slices(&ui.inner));
	}
	tick = 0U;
	while (!zz9k_image_window_loop_done(png_input->view_framebuffer,
	                                    tick, png_input->hold_ticks, 0)) {
		int changed;
		int closed;

		if (!zz9k_png_poll_window(&ui, framebuffer, &changed, &closed)) {
			printf("zz9k-png: invalid PNG window after event\n");
			goto cleanup;
		}
		if (zz9k_image_window_loop_done(
			    png_input->view_framebuffer, tick,
			    png_input->hold_ticks, closed)) {
			break;
		}
		if (changed) {
			status = zz9k_png_render_source_to_area(
				ctx, framebuffer, png_input, source,
				source_width, source_height, &ui, &ui.inner,
				backup);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-png: framebuffer rescale failed: "
				       "%s (%d)\n",
				       zz9k_status_name(status), status);
				goto cleanup;
			}
			printf("zz9k-png: resized window area %lu,%lu %lu x %lu\n",
			       (unsigned long)ui.inner.x,
			       (unsigned long)ui.inner.y,
			       (unsigned long)ui.inner.w,
			       (unsigned long)ui.inner.h);
		}
		tick++;
		zz9k_png_delay_ticks(1U);
	}

	if (backup->active) {
		status = zz9k_png_framebuffer_restore_visible(ctx, &ui, backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-png: framebuffer restore failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			goto cleanup;
		}
		printf("zz9k-png: restored framebuffer window area using "
		       "ARM surface copy\n");
	}
	rc = 0;

cleanup:
	if (backup->active) {
		status = zz9k_png_framebuffer_restore_visible(ctx, &ui, backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-png: framebuffer restore failed during "
			       "cleanup: %s (%d)\n",
			       zz9k_status_name(status), status);
		}
	}
	zz9k_png_close_window(&ui);
	return rc == 0;
}

static int zz9k_png_prepare_output(ZZ9KContext *ctx,
                                   const ZZ9KPngInput *png_input,
                                   ZZ9KImageSessionBeginDesc *begin,
                                   ZZ9KSurface *framebuffer,
                                   ZZ9KSurface *surface,
                                   int *surface_allocated,
                                   ZZ9KFbRect *framebuffer_rect,
                                   ZZ9KPngFramebufferBackup *backup)
{
	ZZ9KRect output_rect;
	uint32_t output_bytes;
	uint32_t output_format;
	uint32_t output_pitch;
	int status;

	output_format = zz9k_surface_native_rtg_format();
	if (!zz9k_surface_layout(png_input->width, png_input->height,
	                         output_format, &output_pitch,
	                         &output_bytes)) {
		printf("zz9k-png: PNG output is too large\n");
		return 0;
	}

	output_rect.x = 0U;
	output_rect.y = 0U;
	output_rect.w = png_input->width;
	output_rect.h = png_input->height;

	if (png_input->use_framebuffer) {
		zz9k_png_trace_step(png_input, ZZ9K_PNG_STEP_PREPARE_OUTPUT,
		                     "mapping framebuffer surface");
		status = zz9k_map_framebuffer_surface(ctx, framebuffer);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-png: framebuffer map failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			return 0;
		}
		if (!zz9k_surface_is_native_rtg_format(framebuffer->format)) {
			printf("zz9k-png: framebuffer is not native RTG "
			       "(framebuffer %lu x %lu format=%lu)\n",
			       (unsigned long)framebuffer->width,
			       (unsigned long)framebuffer->height,
			       (unsigned long)framebuffer->format);
			return 0;
		}
		if (png_input->window_framebuffer) {
			status = zz9k_alloc_surface_ex(
				ctx, png_input->width, png_input->height,
				output_format, ZZ9K_SURFACE_FLAG_ARM_LOCAL,
				output_pitch, surface);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-png: surface alloc failed: "
				       "%s (%d)\n",
				       zz9k_status_name(status), status);
				return 0;
			}
			*surface_allocated = 1;
			if (!zz9k_image_build_surface_session_begin_desc(
				    begin, ZZ9K_IMAGE_CODEC_PNG, surface->handle,
				    &output_rect, output_format, 0U)) {
				printf("zz9k-png: could not build stream begin "
				       "descriptor\n");
				return 0;
			}
			zz9k_png_trace_step_ok(
				png_input, ZZ9K_PNG_STEP_PREPARE_OUTPUT);
			return 1;
		}
		if (!zz9k_png_choose_framebuffer_rect(
			    framebuffer, png_input, framebuffer_rect)) {
			if (!png_input->fit_framebuffer &&
			    (png_input->width > framebuffer->width ||
			     png_input->height > framebuffer->height)) {
				printf("zz9k-png: PNG is larger than framebuffer "
				       "(%lu x %lu > %lu x %lu); use --fit "
				       "to scale it\n",
				       (unsigned long)png_input->width,
				       (unsigned long)png_input->height,
				       (unsigned long)framebuffer->width,
				       (unsigned long)framebuffer->height);
			} else {
				printf("zz9k-png: invalid framebuffer draw "
				       "rectangle\n");
			}
			return 0;
		}
		if (png_input->restore_framebuffer &&
		    !zz9k_png_framebuffer_backup_prepare(
			    ctx, framebuffer, framebuffer_rect, backup)) {
			return 0;
		}

		if (png_input->fit_framebuffer) {
			status = zz9k_alloc_surface_ex(
				ctx, png_input->width, png_input->height,
				output_format, ZZ9K_SURFACE_FLAG_ARM_LOCAL,
				output_pitch, surface);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-png: surface alloc failed: "
				       "%s (%d)\n",
				       zz9k_status_name(status), status);
				return 0;
			}
			*surface_allocated = 1;
			if (!zz9k_image_build_surface_session_begin_desc(
				    begin, ZZ9K_IMAGE_CODEC_PNG, surface->handle,
				    &output_rect, output_format, 0U)) {
				printf("zz9k-png: could not build stream begin "
				       "descriptor\n");
				return 0;
			}
		} else {
			ZZ9KRect framebuffer_output_rect;

			framebuffer_output_rect.x = framebuffer_rect->x;
			framebuffer_output_rect.y = framebuffer_rect->y;
			framebuffer_output_rect.w = framebuffer_rect->w;
			framebuffer_output_rect.h = framebuffer_rect->h;
			if (!zz9k_image_build_framebuffer_session_begin_desc(
				    begin, ZZ9K_IMAGE_CODEC_PNG,
				    &framebuffer_output_rect,
				    output_format, 0U)) {
				printf("zz9k-png: could not build stream begin "
				       "descriptor\n");
				return 0;
			}
		}
	} else {
		zz9k_png_trace_step(png_input, ZZ9K_PNG_STEP_PREPARE_OUTPUT,
		                     "allocating ARM-local output surface");
		status = zz9k_alloc_surface_ex(ctx, png_input->width,
		                               png_input->height, output_format,
		                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
		                               output_pitch, surface);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-png: surface alloc failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			return 0;
		}
		*surface_allocated = 1;
		if (!zz9k_image_build_surface_session_begin_desc(
			    begin, ZZ9K_IMAGE_CODEC_PNG, surface->handle,
			    &output_rect, output_format, 0U)) {
			printf("zz9k-png: could not build stream begin "
			       "descriptor\n");
			return 0;
		}
	}
	zz9k_png_trace_step_ok(png_input, ZZ9K_PNG_STEP_PREPARE_OUTPUT);

	return 1;
}

static int zz9k_png_run_streaming(ZZ9KContext *ctx,
                                  const ZZ9KPngInput *png_input)
{
	ZZ9KSharedBuffer staging;
	ZZ9KSurface framebuffer;
	ZZ9KSurface surface;
	ZZ9KPngFramebufferBackup backup;
	ZZ9KImageSessionBeginDesc begin;
	ZZ9KImageSessionResult result;
	ZZ9KFbRect framebuffer_rect;
	FILE *file = 0;
	uint32_t session = 0U;
	uint32_t expected_output_bytes = 0U;
	uint32_t output_format;
	uint32_t expected_output_pitch;
	int staging_allocated = 0;
	int surface_allocated = 0;
	int session_open = 0;
	int status;
	int rc = 1;

	memset(&staging, 0, sizeof(staging));
	memset(&framebuffer, 0, sizeof(framebuffer));
	memset(&surface, 0, sizeof(surface));
	memset(&result, 0, sizeof(result));
	memset(&framebuffer_rect, 0, sizeof(framebuffer_rect));
	zz9k_png_framebuffer_backup_init(&backup);

	printf("PNG input bytes:      %lu\n", (unsigned long)png_input->length);
	printf("PNG dimensions:       %lu x %lu\n",
	       (unsigned long)png_input->width,
	       (unsigned long)png_input->height);
	zz9k_png_flush();
	output_format = zz9k_surface_native_rtg_format();

	zz9k_png_trace_step(png_input, ZZ9K_PNG_STEP_ALLOC_INPUT,
	                     "allocating shared input buffer");
	status = zz9k_alloc_shared(ctx, ZZ9K_PNG_STAGING_BYTES, 16U,
	                           ZZ9K_ALLOC_HOST_WINDOW, &staging);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: input alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	staging_allocated = 1;
	zz9k_png_trace_step_ok(png_input, ZZ9K_PNG_STEP_ALLOC_INPUT);
	if (zz9k_png_should_stop_after(png_input, ZZ9K_PNG_STEP_ALLOC_INPUT)) {
		rc = 0;
		goto cleanup;
	}

	if (!zz9k_png_prepare_output(ctx, png_input, &begin, &framebuffer,
	                             &surface, &surface_allocated,
	                             &framebuffer_rect, &backup)) {
		goto cleanup;
	}
	if (zz9k_png_should_stop_after(
		    png_input, ZZ9K_PNG_STEP_PREPARE_OUTPUT)) {
		rc = 0;
		goto cleanup;
	}

	zz9k_png_trace_step(png_input, ZZ9K_PNG_STEP_SESSION_BEGIN,
	                     "beginning image session");
	status = zz9k_image_session_begin(ctx, &begin, &result);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: stream begin failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	session = result.session;
	session_open = 1;
	if (session == 0U ||
	    result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
		printf("zz9k-png: unexpected stream begin result\n");
		goto cleanup;
	}
	zz9k_png_trace_step_ok(png_input, ZZ9K_PNG_STEP_SESSION_BEGIN);
	if (zz9k_png_should_stop_after(
		    png_input, ZZ9K_PNG_STEP_SESSION_BEGIN)) {
		rc = 0;
		goto cleanup;
	}

	zz9k_png_trace_step(png_input, ZZ9K_PNG_STEP_FILE_OPEN,
	                     "opening PNG file for streaming");
	file = fopen(png_input->path, "rb");
	if (!file) {
		printf("zz9k-png: failed to open '%s'\n", png_input->path);
		goto cleanup;
	}
	zz9k_png_trace_step_ok(png_input, ZZ9K_PNG_STEP_FILE_OPEN);
	if (zz9k_png_should_stop_after(png_input, ZZ9K_PNG_STEP_FILE_OPEN)) {
		rc = 0;
		goto cleanup;
	}
	{
		int stopped = 0;

		if (!zz9k_png_feed_stream(ctx, file, png_input, &staging,
		                          session, &result, &stopped)) {
			goto cleanup;
		}
		if (stopped) {
			rc = 0;
			goto cleanup;
		}
	}
	if (!zz9k_surface_layout(png_input->width, png_input->height,
	                         output_format, &expected_output_pitch,
	                         &expected_output_bytes)) {
		printf("zz9k-png: PNG output is too large\n");
		goto cleanup;
	}

	if (result.image_width != png_input->width ||
	    result.image_height != png_input->height ||
	    result.output_format != output_format ||
	    result.tile_width != png_input->width ||
	    result.tile_height != png_input->height ||
	    result.bytes_written != expected_output_bytes) {
		printf("zz9k-png: unexpected stream result %lu x %lu -> "
		       "%lu x %lu format=%lu bytes=%lu\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)result.tile_width,
		       (unsigned long)result.tile_height,
		       (unsigned long)result.output_format,
		       (unsigned long)result.bytes_written);
		goto cleanup;
	}

	if (png_input->use_framebuffer) {
		if (png_input->window_framebuffer) {
			printf("zz9k-png: file stream ok (%lu x %lu, %lu input "
			       "bytes, %lu output bytes, ARM surface for window)\n",
			       (unsigned long)result.image_width,
			       (unsigned long)result.image_height,
			       (unsigned long)png_input->length,
			       (unsigned long)result.bytes_written);
			if (!zz9k_png_show_decoded_surface(
				    ctx, &framebuffer, png_input, &surface,
				    result.image_width, result.image_height,
				    &backup)) {
				goto cleanup;
			}
		} else if (png_input->fit_framebuffer) {
			ZZ9KScaleImageDesc scale;

			if (!zz9k_png_build_framebuffer_scale_desc(
				    &scale, surface.handle, result.image_width,
				    result.image_height, &framebuffer_rect)) {
				printf("zz9k-png: framebuffer scale descriptor "
				       "build failed\n");
				goto cleanup;
			}
			status = zz9k_scale_image(ctx, &scale);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-png: framebuffer scale failed: "
				       "%s (%d)\n",
				       zz9k_status_name(status), status);
				goto cleanup;
			}
			printf("zz9k-png: file stream ok (%lu x %lu, %lu input "
			       "bytes, %lu output bytes, scaled to framebuffer "
			       "%lu x %lu at %lu,%lu)\n",
			       (unsigned long)result.image_width,
			       (unsigned long)result.image_height,
			       (unsigned long)png_input->length,
			       (unsigned long)result.bytes_written,
			       (unsigned long)framebuffer_rect.w,
			       (unsigned long)framebuffer_rect.h,
			       (unsigned long)framebuffer_rect.x,
			       (unsigned long)framebuffer_rect.y);
		} else {
			printf("zz9k-png: file stream ok (%lu x %lu, %lu input "
			       "bytes, %lu output bytes, framebuffer)\n",
			       (unsigned long)result.image_width,
			       (unsigned long)result.image_height,
			       (unsigned long)png_input->length,
			       (unsigned long)result.bytes_written);
		}
		if (png_input->restore_framebuffer) {
			printf("zz9k-png: showing for %lu ticks\n",
			       (unsigned long)png_input->hold_ticks);
			zz9k_png_delay_ticks(png_input->hold_ticks);
			status = zz9k_png_framebuffer_restore(ctx, &backup);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-png: framebuffer restore failed: "
				       "%s (%d)\n",
				       zz9k_status_name(status), status);
				goto cleanup;
			}
			printf("zz9k-png: restored framebuffer rectangle "
			       "%lu x %lu at %lu,%lu using ARM surface copy\n",
			       (unsigned long)backup.rect.w,
			       (unsigned long)backup.rect.h,
			       (unsigned long)backup.rect.x,
			       (unsigned long)backup.rect.y);
		}
	} else {
		printf("zz9k-png: file stream ok (%lu x %lu, %lu input bytes, "
		       "%lu output bytes, ARM surface)\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)png_input->length,
		       (unsigned long)result.bytes_written);
	}

	rc = 0;

cleanup:
	if (backup.active) {
		if (png_input->trace) {
			printf("zz9k-png: cleanup: restoring framebuffer\n");
			zz9k_png_flush();
		}
		status = zz9k_png_framebuffer_restore(ctx, &backup);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-png: framebuffer restore failed during "
			       "cleanup: %s (%d)\n",
			       zz9k_status_name(status), status);
			rc = 1;
		}
	}
	zz9k_png_framebuffer_backup_free(ctx, &backup);
	if (file)
		fclose(file);
	if (session_open) {
		if (png_input->trace) {
			printf("zz9k-png: cleanup: closing image session\n");
			zz9k_png_flush();
		}
		status = zz9k_image_session_close(ctx, session, 0U);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-png: stream close failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			rc = 1;
		}
	}
	if (surface_allocated)
		zz9k_free_surface(ctx, surface.handle);
	if (staging_allocated)
		zz9k_free_shared(ctx, staging.handle);
	return rc;
}

int zz9k_png_decode_viewer_image(ZZ9KContext *ctx,
                                 const ZZ9KSurface *framebuffer,
                                 const char *path,
                                 ZZ9KPictureViewerImage *image)
{
	ZZ9KPngInput input;
	ZZ9KSharedBuffer staging;
	ZZ9KSurface surface;
	ZZ9KImageSessionBeginDesc begin;
	ZZ9KImageSessionResult result;
	ZZ9KRect output_rect;
	FILE *file = 0;
	uint32_t output_format = 0U;
	uint32_t output_pitch = 0U;
	uint32_t expected_output_bytes = 0U;
	uint32_t session = 0U;
	int staging_allocated = 0;
	int surface_allocated = 0;
	int session_open = 0;
	int stopped = 0;
	int success = 0;
	int status;

	if (!image) {
		printf("zz9k-view: missing PNG viewer image output\n");
		return 0;
	}
	zz9k_picture_viewer_image_init(image);

	if (!ctx || !framebuffer || !path || path[0] == '\0' ||
	    framebuffer->width == 0U || framebuffer->height == 0U) {
		printf("zz9k-view: invalid PNG viewer decode request\n");
		return 0;
	}

	output_format = zz9k_picture_viewer_decode_format();

	memset(&input, 0, sizeof(input));
	memset(&staging, 0, sizeof(staging));
	memset(&surface, 0, sizeof(surface));
	memset(&begin, 0, sizeof(begin));
	memset(&result, 0, sizeof(result));

	if (!zz9k_png_load_file(path, &input)) {
		printf("zz9k-view: failed to load PNG metadata for '%s'\n", path);
		goto cleanup;
	}
	input.hold_ticks = ZZ9K_PNG_DEFAULT_FRAMEBUFFER_HOLD_TICKS;
	input.chunk_bytes = ZZ9K_PNG_DEFAULT_CHUNK_BYTES;
	input.restore_framebuffer = 0;

	if (!zz9k_png_require_stream_service(ctx, 0)) {
		printf("zz9k-view: PNG stream surface service is not available\n");
		goto cleanup;
	}

	status = zz9k_alloc_shared(ctx, ZZ9K_PNG_STAGING_BYTES, 16U,
	                           ZZ9K_ALLOC_HOST_WINDOW, &staging);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: PNG staging alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	staging_allocated = 1;

	if (!zz9k_surface_layout(input.width, input.height, output_format,
	                         &output_pitch, &expected_output_bytes)) {
		printf("zz9k-view: PNG output is too large\n");
		goto cleanup;
	}
	status = zz9k_alloc_surface_ex(ctx, input.width, input.height,
	                               output_format,
	                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
	                               output_pitch, &surface);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: PNG decode surface alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	surface_allocated = 1;

	output_rect.x = 0U;
	output_rect.y = 0U;
	output_rect.w = input.width;
	output_rect.h = input.height;
	if (!zz9k_image_build_surface_session_begin_desc(
		    &begin, ZZ9K_IMAGE_CODEC_PNG, surface.handle,
		    &output_rect, output_format, 0U)) {
		printf("zz9k-view: could not build PNG stream begin descriptor\n");
		goto cleanup;
	}

	status = zz9k_image_session_begin(ctx, &begin, &result);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: PNG stream begin failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	session = result.session;
	session_open = 1;
	if (session == 0U ||
	    result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
		printf("zz9k-view: unexpected PNG stream begin result\n");
		goto cleanup;
	}

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-view: failed to open '%s'\n", path);
		goto cleanup;
	}
	if (!zz9k_png_feed_stream(ctx, file, &input, &staging, session,
	                          &result, &stopped)) {
		printf("zz9k-view: PNG stream feed failed for '%s'\n", path);
		goto cleanup;
	}
	if (stopped) {
		printf("zz9k-view: PNG stream stopped before completion\n");
		goto cleanup;
	}

	if (result.image_width != input.width ||
	    result.image_height != input.height ||
	    result.output_format != output_format ||
	    result.tile_width != input.width ||
	    result.tile_height != input.height ||
	    result.bytes_written != expected_output_bytes) {
		printf("zz9k-view: unexpected PNG stream result %lu x %lu -> "
		       "%lu x %lu format=%lu bytes=%lu expected=%lu\n",
		       (unsigned long)result.image_width,
		       (unsigned long)result.image_height,
		       (unsigned long)result.tile_width,
		       (unsigned long)result.tile_height,
		       (unsigned long)result.output_format,
		       (unsigned long)result.bytes_written,
		       (unsigned long)expected_output_bytes);
		goto cleanup;
	}

	success = 1;

cleanup:
	if (file)
		fclose(file);
	if (session_open) {
		status = zz9k_image_session_close(ctx, session, 0U);
		if (status != ZZ9K_STATUS_OK) {
			printf("zz9k-view: PNG stream close failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			success = 0;
		}
	}
	if (success) {
		image->codec = ZZ9K_PICTURE_VIEWER_CODEC_PNG;
		image->path = path;
		image->width = input.width;
		image->height = input.height;
		image->surface = surface;
		image->surface_allocated = 1;
		surface_allocated = 0;
	}
	if (surface_allocated)
		zz9k_free_surface(ctx, surface.handle);
	if (staging_allocated)
		zz9k_free_shared(ctx, staging.handle);
	if (!success)
		zz9k_picture_viewer_image_init(image);
	return success;
}

#ifndef ZZ9K_PNG_NO_MAIN
int main(int argc, char **argv)
{
	ZZ9KContext *ctx = 0;
	ZZ9KCaps caps;
	ZZ9KPngInput png_input;
	int status;
	int rc = 1;

	if (!zz9k_png_parse_args(argc, argv, &png_input))
		return 1;

	printf("zz9k-png: opening SDK mailbox\n");
	status = zz9k_open(&ctx);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: open failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 10;
	}

	status = zz9k_query_caps(ctx, &caps);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-png: query caps failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	if (!zz9k_png_require_cap(caps.capability_bits,
	                          ZZ9K_CAP_SHARED_ALLOC) ||
	    !zz9k_png_require_cap(caps.capability_bits,
	                          ZZ9K_CAP_IMAGE_DECODE) ||
	    !zz9k_png_require_cap(caps.capability_bits,
	                          ZZ9K_CAP_SERVICE_DISCOVERY) ||
	    !zz9k_png_require_cap(caps.capability_bits,
	                          ZZ9K_CAP_SURFACES)) {
		goto cleanup;
	}
	if (png_input.use_framebuffer &&
	    (!zz9k_png_require_cap(caps.capability_bits,
	                           ZZ9K_CAP_FRAMEBUFFER_SURFACE) ||
	     ((png_input.fit_framebuffer || png_input.window_framebuffer) &&
	      !zz9k_png_require_cap(caps.capability_bits,
	                            ZZ9K_CAP_IMAGE_SCALE)) ||
	     ((png_input.restore_framebuffer || png_input.window_framebuffer) &&
	      !zz9k_png_require_cap(caps.capability_bits,
	                            ZZ9K_CAP_SURFACE_OPS)))) {
		goto cleanup;
	}
	if (png_input.window_framebuffer &&
	    !zz9k_png_require_clipped_scale_service(ctx)) {
		goto cleanup;
	}
	if (!zz9k_png_require_stream_service(
		    ctx, png_input.use_framebuffer && !png_input.fit_framebuffer &&
		    !png_input.window_framebuffer))
		goto cleanup;

	rc = zz9k_png_run_streaming(ctx, &png_input);

cleanup:
	zz9k_close(ctx);
	return rc;
}
#endif
