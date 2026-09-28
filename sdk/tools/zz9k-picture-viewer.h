/*
 * Shared picture viewer helpers for ZZ9000 SDK tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_PICTURE_VIEWER_H
#define ZZ9K_PICTURE_VIEWER_H

#include "zz9k-image-window.h"
#include "zz9k/host.h"
#include "zz9k/surface.h"
#include <stddef.h>
#include <stdint.h>

typedef enum ZZ9KPictureViewerCodec {
	ZZ9K_PICTURE_VIEWER_CODEC_UNKNOWN = 0,
	ZZ9K_PICTURE_VIEWER_CODEC_JPEG,
	ZZ9K_PICTURE_VIEWER_CODEC_PNG
} ZZ9KPictureViewerCodec;

typedef enum ZZ9KPictureViewerAction {
	ZZ9K_PICTURE_VIEWER_ACTION_NONE = 0,
	ZZ9K_PICTURE_VIEWER_ACTION_QUIT,
	ZZ9K_PICTURE_VIEWER_ACTION_NEXT,
	ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS,
	ZZ9K_PICTURE_VIEWER_ACTION_REDRAW
} ZZ9KPictureViewerAction;

typedef struct ZZ9KPictureViewerArgs {
	const char **files;
	uint32_t file_count;
} ZZ9KPictureViewerArgs;

typedef struct ZZ9KPictureViewerImage {
	ZZ9KPictureViewerCodec codec;
	const char *path;
	uint32_t width;
	uint32_t height;
	ZZ9KSurface surface;
	int surface_allocated;
} ZZ9KPictureViewerImage;

ZZ9KPictureViewerCodec zz9k_picture_viewer_detect_codec(
	const uint8_t *bytes,
	uint32_t length);
const char *zz9k_picture_viewer_codec_name(ZZ9KPictureViewerCodec codec);
int zz9k_picture_viewer_parse_args(int argc,
                                   char **argv,
                                   ZZ9KPictureViewerArgs *args);
const char *zz9k_picture_viewer_basename(const char *path);
int zz9k_picture_viewer_format_title(
	char *title,
	size_t title_capacity,
	uint32_t index,
	uint32_t count,
	const ZZ9KPictureViewerImage *image);
uint32_t zz9k_picture_viewer_next_index(uint32_t index, uint32_t count);
uint32_t zz9k_picture_viewer_previous_index(uint32_t index, uint32_t count);
ZZ9KPictureViewerAction zz9k_picture_viewer_action_from_keys(
	uint32_t vanilla_key,
	uint32_t raw_key);
uint32_t zz9k_picture_viewer_decode_format(void);
int zz9k_picture_viewer_framebuffer_format_supported(uint32_t format);
int zz9k_picture_viewer_framebuffer_requires_conversion(uint32_t format);
int zz9k_picture_viewer_image_service_supported(uint32_t opcode_count,
                                                uint32_t service_flags,
                                                uint32_t framebuffer_format);
void zz9k_picture_viewer_image_init(ZZ9KPictureViewerImage *image);
void zz9k_picture_viewer_image_free(ZZ9KContext *ctx,
                                    ZZ9KPictureViewerImage *image);
int zz9k_picture_viewer_render_image(ZZ9KContext *ctx,
                                     const ZZ9KSurface *framebuffer,
                                     const ZZ9KImageWindow *ui,
                                     const ZZ9KPictureViewerImage *image);

#endif
