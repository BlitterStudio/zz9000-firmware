/*
 * Standalone WebP decode bridge for the shared ZZ9000 picture viewer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-webp-view.h"
#include "zz9k-picture-viewer.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/image.h"
#include "zz9k/image_geometry.h"
#include "zz9k/surface.h"
#include "zz9k/shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZ9K_WEBP_STAGING_BYTES (32UL * 1024UL)

typedef struct ZZ9KWebPInput {
	const char *path;
	uint32_t width;
	uint32_t height;
	uint32_t file_length;
	uint32_t format;
	uint8_t has_alpha;
	uint8_t is_animated;
} ZZ9KWebPInput;

static int zz9k_webp_load_file(const char *path, ZZ9KWebPInput *input)
{
	uint8_t header[ZZ9K_WEBP_HEADER_MIN_BYTES];
	ZZ9KWebPHeader parsed;
	ZZ9KWebPParseStatus status;
	FILE *file;
	long file_size;
	size_t read_bytes;

	if (!path || !input)
		return 0;
	memset(input, 0, sizeof(*input));

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-view: failed to open '%s'\n", path);
		return 0;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return 0;
	}
	file_size = ftell(file);
	if (file_size < (long)ZZ9K_WEBP_HEADER_MIN_BYTES) {
		fclose(file);
		printf("zz9k-view: WebP file '%s' is too small (%ld bytes)\n",
		       path, file_size);
		return 0;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 0;
	}
	read_bytes = fread(header, 1U, sizeof(header), file);
	fclose(file);
	if (read_bytes != sizeof(header)) {
		printf("zz9k-view: failed to read WebP header from '%s'\n", path);
		return 0;
	}

	status = zz9k_webp_parse_header(header, (uint32_t)read_bytes, &parsed);
	if (status != ZZ9K_WEBP_PARSE_READY) {
		printf("zz9k-view: invalid WebP header in '%s'\n", path);
		return 0;
	}

	input->path = path;
	input->width = parsed.width;
	input->height = parsed.height;
	input->file_length = (uint32_t)file_size;
	input->format = parsed.format;
	input->has_alpha = parsed.has_alpha;
	input->is_animated = parsed.is_animated;
	return 1;
}

static int zz9k_webp_require_stream_service(ZZ9KContext *ctx)
{
	ZZ9KServiceInfo service;
	uint32_t required_flags = 0U;
	int status;

	if (!ctx)
		return 0;
	memset(&service, 0, sizeof(service));
	status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: image service query failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 0;
	}

	if (!zz9k_image_stream_required_service_flags(
		    ZZ9K_IMAGE_CODEC_WEBP,
		    ZZ9K_IMAGE_OUTPUT_SURFACE,
		    &required_flags)) {
		printf("zz9k-view: could not determine required WebP service flags\n");
		return 0;
	}
	if (!zz9k_has_service_flags(service.flags, required_flags)) {
		printf("zz9k-view: firmware does not advertise WebP streaming support\n");
		return 0;
	}
	return 1;
}

static int zz9k_webp_feed_stream(ZZ9KContext *ctx, FILE *file,
                                 const ZZ9KWebPInput *webp_input,
                                 ZZ9KSharedBuffer *staging,
                                 uint32_t session,
                                 ZZ9KImageSessionResult *final_result)
{
	uint32_t file_offset = 0U;
	uint32_t buffered = 0U;
	uint32_t empty_eof_feeds = 0U;

	memset(final_result, 0, sizeof(*final_result));
	while (final_result->state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
		uint32_t consumed;
		uint32_t to_read;
		size_t bytes_read;
		int eof;

		if (staging->length > buffered && file_offset < webp_input->file_length) {
			to_read = staging->length - buffered;
			if (to_read > webp_input->file_length - file_offset)
				to_read = webp_input->file_length - file_offset;
			bytes_read = fread((uint8_t *)staging->data + buffered, 1U, (size_t)to_read, file);
			if (bytes_read == 0U && ferror(file)) {
				printf("zz9k-view: failed reading WebP stream for '%s'\n",
				       webp_input->path);
				return 0;
			}
			buffered += (uint32_t)bytes_read;
			file_offset += (uint32_t)bytes_read;
		}

		eof = file_offset == webp_input->file_length;
		if (buffered == 0U && eof) {
			if (empty_eof_feeds > webp_input->height + 8U) {
				printf("zz9k-view: WebP stream exceeded EOF drain limit\n");
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
				printf("zz9k-view: could not build WebP stream feed descriptor\n");
				return 0;
			}

			memset(&result, 0, sizeof(result));
			status = zz9k_image_session_feed(ctx, &feed, &result);
			if (status != ZZ9K_STATUS_OK) {
				printf("zz9k-view: WebP stream feed failed: %s (%d)\n",
				       zz9k_status_name(status), status);
				return 0;
			}

			if (result.bytes_consumed > buffered) {
				printf("zz9k-view: WebP stream consumed beyond input chunk\n");
				return 0;
			}
			consumed = result.bytes_consumed;
			if (consumed != 0U) {
				buffered -= consumed;
				if (buffered != 0U) {
					if (!zz9k_shared_move(staging, 0U, consumed, buffered)) {
						printf("zz9k-view: WebP stream compaction failed\n");
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
				printf("zz9k-view: WebP stream returned unexpected state %lu\n",
				       (unsigned long)result.state);
				return 0;
			}

			if (consumed == 0U && result.bytes_written == 0U) {
				if (buffered == staging->length || eof) {
					printf("zz9k-view: WebP stream made no progress (eof=%d buffered=%lu)\n",
					       eof, (unsigned long)buffered);
					return 0;
				}
			}

			if (buffered == 0U || (consumed == 0U && result.bytes_written == 0U && !eof)) {
				break;
			}
		} while (1);
	}

	return 1;
}

int zz9k_webp_decode_viewer_image(ZZ9KContext *ctx,
                                  const ZZ9KSurface *framebuffer,
                                  const char *path,
                                  ZZ9KPictureViewerImage *image)
{
	ZZ9KWebPInput input;
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
	int success = 0;
	int status;

	if (!image) {
		printf("zz9k-view: missing WebP viewer image output\n");
		return 0;
	}
	zz9k_picture_viewer_image_init(image);

	if (!ctx || !framebuffer || !path || path[0] == '\0' ||
	    framebuffer->width == 0U || framebuffer->height == 0U) {
		printf("zz9k-view: invalid WebP viewer decode request\n");
		return 0;
	}

	output_format = zz9k_picture_viewer_decode_format();

	memset(&input, 0, sizeof(input));
	memset(&staging, 0, sizeof(staging));
	memset(&surface, 0, sizeof(surface));
	memset(&begin, 0, sizeof(begin));
	memset(&result, 0, sizeof(result));

	if (!zz9k_webp_load_file(path, &input)) {
		goto cleanup;
	}

	if (!zz9k_webp_require_stream_service(ctx)) {
		printf("zz9k-view: WebP stream surface service is not available\n");
		goto cleanup;
	}

	status = zz9k_alloc_shared(ctx, ZZ9K_WEBP_STAGING_BYTES, 16U,
	                           ZZ9K_ALLOC_HOST_WINDOW, &staging);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: WebP staging alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	staging_allocated = 1;

	/*
	 * Animated WebP input decodes the first fully composited canvas as a
	 * documented still preview. Surface dimensions are canvas dimensions.
	 */
	if (!zz9k_surface_layout(input.width, input.height, output_format,
	                         &output_pitch, &expected_output_bytes)) {
		printf("zz9k-view: WebP output is too large\n");
		goto cleanup;
	}
	status = zz9k_alloc_surface_ex(ctx, input.width, input.height,
	                               output_format,
	                               ZZ9K_SURFACE_FLAG_ARM_LOCAL,
	                               output_pitch, &surface);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: WebP decode surface alloc failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	surface_allocated = 1;

	output_rect.x = 0U;
	output_rect.y = 0U;
	output_rect.w = input.width;
	output_rect.h = input.height;
	if (!zz9k_image_build_surface_session_begin_desc(
		    &begin, ZZ9K_IMAGE_CODEC_WEBP, surface.handle,
		    &output_rect, output_format, 0U)) {
		printf("zz9k-view: could not build WebP stream begin descriptor\n");
		goto cleanup;
	}

	status = zz9k_image_session_begin(ctx, &begin, &result);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: WebP stream begin failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	session = result.session;
	session_open = 1;
	if (session == 0U ||
	    result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
		printf("zz9k-view: unexpected WebP stream begin result\n");
		goto cleanup;
	}

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-view: failed to open '%s'\n", path);
		goto cleanup;
	}
	if (!zz9k_webp_feed_stream(ctx, file, &input, &staging, session,
	                           &result)) {
		printf("zz9k-view: WebP stream feed failed for '%s'\n", path);
		goto cleanup;
	}

	if (result.image_width != input.width ||
	    result.image_height != input.height ||
	    result.output_format != output_format ||
	    result.tile_width != input.width ||
	    result.tile_height != input.height ||
	    result.bytes_written != expected_output_bytes) {
		printf("zz9k-view: unexpected WebP stream result %lu x %lu -> "
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
			printf("zz9k-view: WebP stream close failed: %s (%d)\n",
			       zz9k_status_name(status), status);
			success = 0;
		}
	}
	if (success) {
		image->codec = ZZ9K_PICTURE_VIEWER_CODEC_WEBP;
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
	return success;
}

#ifndef ZZ9K_WEBP_NO_MAIN
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("zz9k-webp: standalone tool not supported; use zz9k-view\n");
	return 1;
}
#endif
