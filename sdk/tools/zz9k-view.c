/*
 * Standalone JPEG/PNG image viewer for the ZZ9000 SDK image service.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-jpeg-view.h"
#include "zz9k-picture-viewer.h"
#include "zz9k-png-view.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/image_geometry.h"
#include "zz9k/text.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
	defined(__VBCC__)
#define ZZ9K_VIEW_AMIGA 1
#include <exec/types.h>
#include <proto/dos.h>
#else
#define ZZ9K_VIEW_AMIGA 0
#endif

#define ZZ9K_VIEW_HEADER_BYTES 16U
#define ZZ9K_VIEW_WINDOW_MIN_WIDTH 180U
#define ZZ9K_VIEW_WINDOW_MIN_HEIGHT 120U
#define ZZ9K_VIEW_WINDOW_MARGIN_X 32U
#define ZZ9K_VIEW_WINDOW_MARGIN_Y 48U

typedef enum ZZ9KViewDirection {
	ZZ9K_VIEW_DIRECTION_NEXT = 0,
	ZZ9K_VIEW_DIRECTION_PREVIOUS
} ZZ9KViewDirection;

static void zz9k_view_usage(void)
{
	printf("usage: zz9k-view file.jpg|file.png [more-images...]\n");
	printf("       keys: Space/Right/Down next, Left/Up/Backspace previous, "
	       "r redraw, q/Esc quit\n");
}

static void zz9k_view_delay_tick(void)
{
#if ZZ9K_VIEW_AMIGA
	Delay(1L);
#else
	volatile uint32_t spin;

	for (spin = 0U; spin < 1000000UL; spin++) {
	}
#endif
}

static int zz9k_view_require_cap(uint32_t caps, uint32_t bit)
{
	const char *name;

	if (zz9k_has_capability(caps, bit))
		return 1;
	name = zz9k_capability_name(bit);
	printf("zz9k-view: missing required capability: %s\n",
	       name ? name : "unknown");
	return 0;
}

static int zz9k_view_require_caps(uint32_t caps)
{
	return zz9k_view_require_cap(caps, ZZ9K_CAP_SHARED_ALLOC) &&
	       zz9k_view_require_cap(caps, ZZ9K_CAP_SERVICE_DISCOVERY) &&
	       zz9k_view_require_cap(caps, ZZ9K_CAP_IMAGE_DECODE) &&
	       zz9k_view_require_cap(caps, ZZ9K_CAP_SURFACES) &&
	       zz9k_view_require_cap(caps, ZZ9K_CAP_FRAMEBUFFER_SURFACE) &&
	       zz9k_view_require_cap(caps, ZZ9K_CAP_IMAGE_SCALE) &&
	       zz9k_view_require_cap(caps, ZZ9K_CAP_SURFACE_OPS);
}

static int zz9k_view_require_image_service(ZZ9KContext *ctx,
                                           uint32_t framebuffer_format)
{
	ZZ9KServiceInfo service;
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
	if (!zz9k_picture_viewer_image_service_supported(
		    service.opcode_count, service.flags, framebuffer_format)) {
		printf("zz9k-view: image service does not support clipped "
		       "bilinear scaling for this framebuffer format\n");
		return 0;
	}
	return 1;
}

static int zz9k_view_read_header(const char *path,
                                 uint8_t *header,
                                 uint32_t header_capacity,
                                 uint32_t *bytes_read)
{
	FILE *file;
	size_t read_count;

	if (!path || !header || header_capacity == 0U || !bytes_read)
		return 0;

	file = fopen(path, "rb");
	if (!file) {
		printf("zz9k-view: failed to open '%s'\n", path);
		return 0;
	}
	read_count = fread(header, 1U, (size_t)header_capacity, file);
	if (ferror(file)) {
		printf("zz9k-view: failed to read '%s'\n", path);
		fclose(file);
		return 0;
	}
	fclose(file);
	*bytes_read = (uint32_t)read_count;
	return 1;
}

static int zz9k_view_decode_image(ZZ9KContext *ctx,
                                  const ZZ9KSurface *framebuffer,
                                  const char *path,
                                  ZZ9KPictureViewerImage *image)
{
	uint8_t header[ZZ9K_VIEW_HEADER_BYTES];
	uint32_t bytes_read;
	ZZ9KPictureViewerCodec codec;

	if (!image)
		return 0;
	zz9k_picture_viewer_image_init(image);

	if (!zz9k_view_read_header(path, header, ZZ9K_VIEW_HEADER_BYTES,
	                           &bytes_read)) {
		return 0;
	}
	codec = zz9k_picture_viewer_detect_codec(header, bytes_read);
	if (codec == ZZ9K_PICTURE_VIEWER_CODEC_JPEG) {
		return zz9k_jpeg_decode_viewer_image(ctx, framebuffer, path, image);
	}
	if (codec == ZZ9K_PICTURE_VIEWER_CODEC_PNG) {
		return zz9k_png_decode_viewer_image(ctx, framebuffer, path, image);
	}

	printf("zz9k-view: unsupported image type for '%s'\n", path);
	return 0;
}

static int zz9k_view_format_window_title(
	char *title,
	size_t title_capacity,
	uint32_t index,
	uint32_t count,
	const ZZ9KPictureViewerImage *image)
{
	const char *codec;
	const char *base;
	int needed;

	if (!title || title_capacity == 0U || !image)
		return 0;
	if (zz9k_picture_viewer_format_title(
		    title, title_capacity, index + 1U, count, image)) {
		return 1;
	}

	codec = zz9k_picture_viewer_codec_name(image->codec);
	base = zz9k_picture_viewer_basename(image->path);
	needed = snprintf(title, title_capacity, "ZZ9000 View %lu/%lu %s - %s",
	                  (unsigned long)(index + 1U),
	                  (unsigned long)count, codec, base);
	if (needed > 0 && (size_t)needed < title_capacity)
		return 1;

	needed = snprintf(title, title_capacity, "ZZ9000 View %lu/%lu %s",
	                  (unsigned long)(index + 1U),
	                  (unsigned long)count, codec);
	return needed > 0 && (size_t)needed < title_capacity;
}

static int zz9k_view_present_image(ZZ9KContext *ctx,
                                   const ZZ9KSurface *framebuffer,
                                   ZZ9KImageWindow *ui,
                                   const ZZ9KPictureViewerImage *image,
                                   uint32_t index,
                                   uint32_t count)
{
	char title[160];
	int status;

	if (!zz9k_view_format_window_title(
		    title, sizeof(title), index, count, image)) {
		printf("zz9k-view: could not format window title for '%s'\n",
		       image && image->path ? image->path : "");
		return 0;
	}
	zz9k_image_window_set_title(ui, title);
	status = zz9k_picture_viewer_render_image(ctx, framebuffer, ui, image);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: render failed for '%s': %s (%d)\n",
		       image && image->path ? image->path : "",
		       zz9k_status_name(status), status);
		return 0;
	}
	return 1;
}

static int zz9k_view_open_window(const ZZ9KSurface *framebuffer,
                                 const ZZ9KPictureViewerImage *image,
                                 ZZ9KImageWindow *ui);

static int zz9k_view_startup_image(ZZ9KContext *ctx,
                                   const ZZ9KSurface *framebuffer,
                                   const ZZ9KPictureViewerArgs *args,
                                   ZZ9KImageWindow *ui,
                                   int *window_open,
                                   ZZ9KPictureViewerImage *image,
                                   uint32_t *index)
{
	ZZ9KPictureViewerImage candidate;
	uint32_t i;

	if (!args || !ui || !window_open || !image || !index)
		return 0;

	for (i = 0U; i < args->file_count; i++) {
		zz9k_picture_viewer_image_init(&candidate);
		if (zz9k_view_decode_image(ctx, framebuffer, args->files[i],
		                           &candidate)) {
			if (!*window_open) {
				if (!zz9k_view_open_window(framebuffer, &candidate,
				                           ui)) {
					printf("zz9k-view: failed to open image "
					       "window for '%s'\n", args->files[i]);
					zz9k_picture_viewer_image_free(ctx,
					                               &candidate);
					continue;
				}
				*window_open = 1;
			}
			if (zz9k_view_present_image(ctx, framebuffer, ui,
			                            &candidate, i,
			                            args->file_count)) {
				*image = candidate;
				*index = i;
				return 1;
			}
			printf("zz9k-view: decoded '%s' but could not render it; "
			       "trying next image\n", args->files[i]);
		}
		zz9k_picture_viewer_image_free(ctx, &candidate);
	}

	printf("zz9k-view: no displayable images found\n");
	return 0;
}

static uint32_t zz9k_view_step_index(uint32_t index,
                                     uint32_t count,
                                     ZZ9KViewDirection direction)
{
	if (direction == ZZ9K_VIEW_DIRECTION_PREVIOUS)
		return zz9k_picture_viewer_previous_index(index, count);
	return zz9k_picture_viewer_next_index(index, count);
}

static int zz9k_view_navigate(ZZ9KContext *ctx,
                              const ZZ9KSurface *framebuffer,
                              ZZ9KImageWindow *ui,
                              const ZZ9KPictureViewerArgs *args,
                              ZZ9KPictureViewerImage *current_image,
                              uint32_t *current_index,
                              ZZ9KViewDirection direction)
{
	ZZ9KPictureViewerImage candidate;
	uint32_t candidate_index;
	uint32_t attempts;
	const char *direction_name;

	if (!args || !current_image || !current_index || args->file_count <= 1U) {
		printf("zz9k-view: no other displayable image; keeping '%s'\n",
		       current_image && current_image->path ?
		       current_image->path : "");
		return 0;
	}

	direction_name = direction == ZZ9K_VIEW_DIRECTION_PREVIOUS ?
	                 "previous" : "next";
	candidate_index = zz9k_view_step_index(*current_index, args->file_count,
	                                       direction);
	for (attempts = 0U; attempts < args->file_count - 1U; attempts++) {
		zz9k_picture_viewer_image_init(&candidate);
		if (zz9k_view_decode_image(ctx, framebuffer,
		                           args->files[candidate_index],
		                           &candidate)) {
			if (zz9k_view_present_image(ctx, framebuffer, ui, &candidate,
			                            candidate_index,
			                            args->file_count)) {
				zz9k_picture_viewer_image_free(ctx, current_image);
				*current_image = candidate;
				*current_index = candidate_index;
				return 1;
			}
			printf("zz9k-view: decoded '%s' but could not render it\n",
			       args->files[candidate_index]);
		}
		zz9k_picture_viewer_image_free(ctx, &candidate);
		candidate_index = zz9k_view_step_index(
			candidate_index, args->file_count, direction);
	}

	printf("zz9k-view: no other displayable image found going %s; "
	       "keeping '%s'\n",
	       direction_name,
	       current_image->path ? current_image->path : "");
	(void)zz9k_view_present_image(ctx, framebuffer, ui, current_image,
	                              *current_index, args->file_count);
	return 0;
}

static int zz9k_view_open_window(const ZZ9KSurface *framebuffer,
                                 const ZZ9KPictureViewerImage *image,
                                 ZZ9KImageWindow *ui)
{
	ZZ9KImageWindowConfig config;

	if (!framebuffer || !image || !ui)
		return 0;
	zz9k_image_window_config_init(
		&config, "ZZ9000 View", image->width, image->height, 1, 1,
		ZZ9K_VIEW_WINDOW_MIN_WIDTH, ZZ9K_VIEW_WINDOW_MIN_HEIGHT,
		ZZ9K_VIEW_WINDOW_MARGIN_X, ZZ9K_VIEW_WINDOW_MARGIN_Y);
	return zz9k_image_window_open(framebuffer, &config, ui);
}

int main(int argc, char **argv)
{
	ZZ9KPictureViewerArgs args;
	ZZ9KContext *ctx = 0;
	ZZ9KCaps caps;
	ZZ9KSurface framebuffer;
	ZZ9KPictureViewerImage image;
	ZZ9KImageWindow ui;
	uint32_t current_index = 0U;
	int window_open = 0;
	int status;
	int rc = 1;

	memset(&args, 0, sizeof(args));
	memset(&caps, 0, sizeof(caps));
	memset(&framebuffer, 0, sizeof(framebuffer));
	zz9k_picture_viewer_image_init(&image);
	memset(&ui, 0, sizeof(ui));

	if (!zz9k_picture_viewer_parse_args(argc, argv, &args)) {
		zz9k_view_usage();
		return 2;
	}

	printf("zz9k-view: opening SDK mailbox\n");
	status = zz9k_open(&ctx);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: open failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		return 10;
	}

	status = zz9k_query_caps(ctx, &caps);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: query caps failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	if (!zz9k_view_require_caps(caps.capability_bits)) {
		goto cleanup;
	}

	status = zz9k_map_framebuffer_surface(ctx, &framebuffer);
	if (status != ZZ9K_STATUS_OK) {
		printf("zz9k-view: framebuffer map failed: %s (%d)\n",
		       zz9k_status_name(status), status);
		goto cleanup;
	}
	if (!zz9k_picture_viewer_framebuffer_format_supported(
		    framebuffer.format)) {
		printf("zz9k-view: unsupported framebuffer format "
		       "(framebuffer %lu x %lu format=%lu %s)\n",
		       (unsigned long)framebuffer.width,
		       (unsigned long)framebuffer.height,
		       (unsigned long)framebuffer.format,
		       zz9k_surface_format_text(framebuffer.format));
		goto cleanup;
	}
	if (!zz9k_view_require_image_service(ctx, framebuffer.format))
		goto cleanup;

	if (!zz9k_view_startup_image(ctx, &framebuffer, &args, &ui,
	                             &window_open, &image, &current_index)) {
		goto cleanup;
	}

	rc = 0;
	for (;;) {
		ZZ9KImageWindowEvent event;
		ZZ9KPictureViewerAction action;

		if (!zz9k_image_window_poll_event(&ui, &framebuffer, &event)) {
			printf("zz9k-view: image window event poll failed\n");
			rc = 1;
			break;
		}
		action = zz9k_picture_viewer_action_from_keys(
			event.vanilla_key, event.raw_key);
		if (event.closed || action == ZZ9K_PICTURE_VIEWER_ACTION_QUIT)
			break;
		if (event.changed ||
		    action == ZZ9K_PICTURE_VIEWER_ACTION_REDRAW) {
			if (!zz9k_view_present_image(
				    ctx, &framebuffer, &ui, &image, current_index,
				    args.file_count)) {
				rc = 1;
				break;
			}
		} else if (action == ZZ9K_PICTURE_VIEWER_ACTION_NEXT) {
			(void)zz9k_view_navigate(
				ctx, &framebuffer, &ui, &args, &image,
				&current_index, ZZ9K_VIEW_DIRECTION_NEXT);
		} else if (action == ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS) {
			(void)zz9k_view_navigate(
				ctx, &framebuffer, &ui, &args, &image,
				&current_index, ZZ9K_VIEW_DIRECTION_PREVIOUS);
		}
		zz9k_view_delay_tick();
	}

cleanup:
	if (window_open)
		zz9k_image_window_close(&ui);
	zz9k_picture_viewer_image_free(ctx, &image);
	zz9k_close(ctx);
	return rc;
}
