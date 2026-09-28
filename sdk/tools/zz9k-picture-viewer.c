/*
 * Shared picture viewer helpers for ZZ9000 SDK tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-picture-viewer.h"

#include "zz9k/caps.h"
#include "zz9k/image_geometry.h"
#include <stdio.h>
#include <string.h>

#define ZZ9K_VIEW_RAWKEY_BACKSPACE 0x41U
#define ZZ9K_VIEW_RAWKEY_ESCAPE 0x45U
#define ZZ9K_VIEW_RAWKEY_UP 0x4cU
#define ZZ9K_VIEW_RAWKEY_DOWN 0x4dU
#define ZZ9K_VIEW_RAWKEY_RIGHT 0x4eU
#define ZZ9K_VIEW_RAWKEY_LEFT 0x4fU

ZZ9KPictureViewerCodec zz9k_picture_viewer_detect_codec(
	const uint8_t *bytes,
	uint32_t length)
{
	static const uint8_t png_signature[8] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
	};

	if (!bytes)
		return ZZ9K_PICTURE_VIEWER_CODEC_UNKNOWN;
	if (length >= 3U &&
	    bytes[0] == 0xffU && bytes[1] == 0xd8U && bytes[2] == 0xffU) {
		return ZZ9K_PICTURE_VIEWER_CODEC_JPEG;
	}
	if (length >= (uint32_t)sizeof(png_signature) &&
	    memcmp(bytes, png_signature, sizeof(png_signature)) == 0) {
		return ZZ9K_PICTURE_VIEWER_CODEC_PNG;
	}
	return ZZ9K_PICTURE_VIEWER_CODEC_UNKNOWN;
}

const char *zz9k_picture_viewer_codec_name(ZZ9KPictureViewerCodec codec)
{
	switch (codec) {
	case ZZ9K_PICTURE_VIEWER_CODEC_JPEG:
		return "JPEG";
	case ZZ9K_PICTURE_VIEWER_CODEC_PNG:
		return "PNG";
	default:
		return "unknown";
	}
}

int zz9k_picture_viewer_parse_args(int argc,
                                   char **argv,
                                   ZZ9KPictureViewerArgs *args)
{
	uint32_t i;

	if (!argv || !args || argc < 2)
		return 0;
	if ((uint64_t)(argc - 1) > 0xffffffffULL)
		return 0;

	for (i = 1U; i < (uint32_t)argc; i++) {
		if (!argv[i] || argv[i][0] == '\0' || argv[i][0] == '-')
			return 0;
	}

	args->files = (const char **)&argv[1];
	args->file_count = (uint32_t)(argc - 1);
	return 1;
}

const char *zz9k_picture_viewer_basename(const char *path)
{
	const char *base;
	const char *cursor;

	if (!path)
		return "";

	base = path;
	for (cursor = path; *cursor; cursor++) {
		if (*cursor == '/' || *cursor == '\\' || *cursor == ':')
			base = cursor + 1;
	}
	return base;
}

int zz9k_picture_viewer_format_title(
	char *title,
	size_t title_capacity,
	uint32_t index,
	uint32_t count,
	const ZZ9KPictureViewerImage *image)
{
	int needed;

	if (!title || title_capacity == 0U || !image)
		return 0;

	needed = snprintf(title, title_capacity,
	                  "ZZ9000 View %lu/%lu %s %lu x %lu - %s",
	                  (unsigned long)index,
	                  (unsigned long)count,
	                  zz9k_picture_viewer_codec_name(image->codec),
	                  (unsigned long)image->width,
	                  (unsigned long)image->height,
	                  zz9k_picture_viewer_basename(image->path));
	return needed > 0 && (size_t)needed < title_capacity;
}

uint32_t zz9k_picture_viewer_next_index(uint32_t index, uint32_t count)
{
	if (count <= 1U)
		return 0U;
	if (index >= count - 1U)
		return 0U;
	return index + 1U;
}

uint32_t zz9k_picture_viewer_previous_index(uint32_t index, uint32_t count)
{
	if (count <= 1U)
		return 0U;
	if (index == 0U || index >= count)
		return count - 1U;
	return index - 1U;
}

ZZ9KPictureViewerAction zz9k_picture_viewer_action_from_keys(
	uint32_t vanilla_key,
	uint32_t raw_key)
{
	if (vanilla_key == 'q' || vanilla_key == 'Q' ||
	    vanilla_key == 0x1bU || raw_key == ZZ9K_VIEW_RAWKEY_ESCAPE) {
		return ZZ9K_PICTURE_VIEWER_ACTION_QUIT;
	}
	if (vanilla_key == ' ' || raw_key == ZZ9K_VIEW_RAWKEY_DOWN ||
	    raw_key == ZZ9K_VIEW_RAWKEY_RIGHT) {
		return ZZ9K_PICTURE_VIEWER_ACTION_NEXT;
	}
	if (vanilla_key == 0x08U || raw_key == ZZ9K_VIEW_RAWKEY_BACKSPACE ||
	    raw_key == ZZ9K_VIEW_RAWKEY_UP ||
	    raw_key == ZZ9K_VIEW_RAWKEY_LEFT) {
		return ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS;
	}
	if (vanilla_key == 'r' || vanilla_key == 'R')
		return ZZ9K_PICTURE_VIEWER_ACTION_REDRAW;

	return ZZ9K_PICTURE_VIEWER_ACTION_NONE;
}

uint32_t zz9k_picture_viewer_decode_format(void)
{
	return zz9k_surface_native_rtg_format();
}

int zz9k_picture_viewer_framebuffer_format_supported(uint32_t format)
{
	return format == zz9k_picture_viewer_decode_format() ||
	       format == ZZ9K_SURFACE_FORMAT_RGB565 ||
	       format == ZZ9K_SURFACE_FORMAT_RGB555;
}

int zz9k_picture_viewer_framebuffer_requires_conversion(uint32_t format)
{
	return zz9k_picture_viewer_framebuffer_format_supported(format) &&
	       format != zz9k_picture_viewer_decode_format();
}

int zz9k_picture_viewer_image_service_supported(uint32_t opcode_count,
                                                uint32_t service_flags,
                                                uint32_t framebuffer_format)
{
	if (!zz9k_picture_viewer_framebuffer_format_supported(
		    framebuffer_format) ||
	    !zz9k_image_service_supports_clipped_scale(
		    opcode_count, service_flags, ZZ9K_SCALE_BILINEAR)) {
		return 0;
	}
	return !zz9k_picture_viewer_framebuffer_requires_conversion(
		       framebuffer_format) ||
	       zz9k_has_service_flag(
		       service_flags,
		       ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BGRA_TO_RGB555_RGB565);
}

void zz9k_picture_viewer_image_init(ZZ9KPictureViewerImage *image)
{
	if (image)
		memset(image, 0, sizeof(*image));
}

void zz9k_picture_viewer_image_free(ZZ9KContext *ctx,
                                    ZZ9KPictureViewerImage *image)
{
	if (!image)
		return;
	if (ctx && image->surface_allocated && image->surface.handle != 0U &&
	    image->surface.handle != ZZ9K_INVALID_HANDLE)
		zz9k_free_surface(ctx, image->surface.handle);
	zz9k_picture_viewer_image_init(image);
}

int zz9k_picture_viewer_render_image(ZZ9KContext *ctx,
                                     const ZZ9KSurface *framebuffer,
                                     const ZZ9KImageWindow *ui,
                                     const ZZ9KPictureViewerImage *image)
{
	ZZ9KSurfaceFillDesc fill;
	ZZ9KFbRect clips[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
	ZZ9KFbRect draw_rect;
	uint32_t clip_count;
	uint32_t i;
	int status;

	if (!ctx || !framebuffer || !ui || !image ||
	    framebuffer->width == 0U || framebuffer->height == 0U ||
	    ui->inner.w == 0U || ui->inner.h == 0U ||
	    image->width == 0U || image->height == 0U ||
	    image->surface.handle == 0U ||
	    image->surface.handle == ZZ9K_INVALID_HANDLE) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}
	if (!zz9k_image_window_choose_draw_rect_in_area(
		    &ui->inner, image->width, image->height, &draw_rect)) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}
	if (!zz9k_image_window_visible_clips(
		    ui, &ui->inner, clips, ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS,
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

		status = zz9k_image_window_scale_sliced(
			ctx, image->surface.handle, image->width,
			image->height, &draw_rect, &clips[i],
			ZZ9K_SCALE_BILINEAR);
		if (status != ZZ9K_STATUS_OK)
			return status;
	}

	return ZZ9K_STATUS_OK;
}
