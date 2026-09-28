/*
 * Header-only image decode/session descriptor helpers for SDK callers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_IMAGE_H
#define ZZ9K_IMAGE_H

#include "zz9k/image_geometry.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int zz9k_image_codec_known(uint32_t codec)
{
  return codec == ZZ9K_IMAGE_CODEC_JPEG ||
         codec == ZZ9K_IMAGE_CODEC_PNG ||
         codec == ZZ9K_IMAGE_CODEC_GIF;
}

static inline int zz9k_image_output_format_known(uint32_t format)
{
  return format != ZZ9K_SURFACE_FORMAT_UNKNOWN;
}

static inline int zz9k_image_stream_required_service_flags(
    uint32_t codec,
    uint32_t output_mode,
    uint32_t *required_flags)
{
  uint32_t codec_flag;
  uint32_t output_flag;

  if (!required_flags) {
    return 0;
  }

  switch (codec) {
  case ZZ9K_IMAGE_CODEC_JPEG:
    codec_flag = ZZ9K_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA;
    break;
  case ZZ9K_IMAGE_CODEC_PNG:
    codec_flag = ZZ9K_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA;
    break;
  default:
    return 0;
  }

  switch (output_mode) {
  case ZZ9K_IMAGE_OUTPUT_SURFACE:
    output_flag = 0U;
    break;
  case ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER:
    output_flag = ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT;
    break;
  case ZZ9K_IMAGE_OUTPUT_TILE_BUFFER:
    output_flag = ZZ9K_SERVICE_FLAG_IMAGE_TILE_OUTPUT;
    break;
  default:
    return 0;
  }

  *required_flags = ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                    codec_flag |
                    output_flag;
  return 1;
}

static inline int zz9k_image_build_decode_desc(
    ZZ9KImageDecodeDesc *desc,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t dst_surface,
    const ZZ9KRect *dst_rect,
    uint32_t output_format,
    uint32_t flags)
{
  if (!desc || src_handle == ZZ9K_INVALID_HANDLE || src_length == 0U ||
      dst_surface == ZZ9K_INVALID_HANDLE || zz9k_rect_is_empty(dst_rect) ||
      !zz9k_image_output_format_known(output_format)) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->src_handle = src_handle;
  desc->src_offset = src_offset;
  desc->src_length = src_length;
  desc->dst_surface = dst_surface;
  desc->dst_x = dst_rect->x;
  desc->dst_y = dst_rect->y;
  desc->dst_width = dst_rect->w;
  desc->dst_height = dst_rect->h;
  desc->output_format = output_format;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_image_build_surface_session_begin_desc(
    ZZ9KImageSessionBeginDesc *desc,
    uint32_t codec,
    uint32_t dst_surface,
    const ZZ9KRect *dst_rect,
    uint32_t output_format,
    uint32_t flags)
{
  if (!desc || !zz9k_image_codec_known(codec) ||
      dst_surface == ZZ9K_INVALID_HANDLE || zz9k_rect_is_empty(dst_rect) ||
      !zz9k_image_output_format_known(output_format)) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->codec = codec;
  desc->output_mode = ZZ9K_IMAGE_OUTPUT_SURFACE;
  desc->dst_surface = dst_surface;
  desc->dst_x = dst_rect->x;
  desc->dst_y = dst_rect->y;
  desc->dst_width = dst_rect->w;
  desc->dst_height = dst_rect->h;
  desc->output_format = output_format;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_image_build_framebuffer_session_begin_desc(
    ZZ9KImageSessionBeginDesc *desc,
    uint32_t codec,
    const ZZ9KRect *dst_rect,
    uint32_t output_format,
    uint32_t flags)
{
  if (!zz9k_image_build_surface_session_begin_desc(
          desc, codec, ZZ9K_SURFACE_HANDLE_FRAMEBUFFER, dst_rect,
          output_format, flags)) {
    return 0;
  }
  desc->output_mode = ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER;
  return 1;
}

static inline int zz9k_image_build_tile_session_begin_desc(
    ZZ9KImageSessionBeginDesc *desc,
    uint32_t codec,
    uint32_t tile_handle,
    uint32_t tile_stride,
    uint32_t tile_rows,
    uint32_t output_format,
    uint32_t flags)
{
  if (!desc || !zz9k_image_codec_known(codec) ||
      tile_handle == ZZ9K_INVALID_HANDLE || tile_stride == 0U ||
      tile_rows == 0U || !zz9k_image_output_format_known(output_format)) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->codec = codec;
  desc->output_mode = ZZ9K_IMAGE_OUTPUT_TILE_BUFFER;
  desc->output_format = output_format;
  desc->tile_handle = tile_handle;
  desc->tile_stride = tile_stride;
  desc->tile_rows = tile_rows;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_image_build_session_feed_desc(
    ZZ9KImageSessionFeedDesc *desc,
    uint32_t session,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t flags)
{
  if (!desc || session == 0U || src_handle == ZZ9K_INVALID_HANDLE ||
      (flags & ~ZZ9K_IMAGE_SESSION_FEED_EOF) != 0U ||
      (src_length == 0U &&
       (flags & ZZ9K_IMAGE_SESSION_FEED_EOF) == 0U)) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->session = session;
  desc->src_handle = src_handle;
  desc->src_offset = src_offset;
  desc->src_length = src_length;
  desc->flags = flags;
  return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_IMAGE_H */
