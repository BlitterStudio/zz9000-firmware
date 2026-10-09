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
         codec == ZZ9K_IMAGE_CODEC_GIF ||
         codec == ZZ9K_IMAGE_CODEC_WEBP;
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
  case ZZ9K_IMAGE_CODEC_WEBP:
    codec_flag = ZZ9K_SERVICE_FLAG_IMAGE_WEBP;
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

#define ZZ9K_WEBP_HEADER_MIN_BYTES 30U

typedef enum ZZ9KWebPFormat {
  ZZ9K_WEBP_FORMAT_UNKNOWN = 0,
  ZZ9K_WEBP_FORMAT_LOSSY = 1,
  ZZ9K_WEBP_FORMAT_LOSSLESS = 2,
  ZZ9K_WEBP_FORMAT_EXTENDED = 3
} ZZ9KWebPFormat;

typedef enum ZZ9KWebPParseStatus {
  ZZ9K_WEBP_PARSE_INVALID = 0,
  ZZ9K_WEBP_PARSE_INCOMPLETE = 1,
  ZZ9K_WEBP_PARSE_READY = 2
} ZZ9KWebPParseStatus;

typedef struct ZZ9KWebPHeader {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint8_t has_alpha;
  uint8_t is_animated;
} ZZ9KWebPHeader;

static inline uint32_t zz9k_webp_read_le24(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16);
}

static inline uint32_t zz9k_webp_read_le32(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline ZZ9KWebPParseStatus zz9k_webp_parse_header(
    const uint8_t *data,
    uint32_t length,
    ZZ9KWebPHeader *out_header)
{
  uint32_t riff_size;
  uint32_t chunk_size;
  int is_vp8;
  int is_vp8l;
  int is_vp8x;

  if (out_header) {
    memset(out_header, 0, sizeof(*out_header));
  }
  if (!data) {
    return ZZ9K_WEBP_PARSE_INVALID;
  }

  if (length < 4U) {
    if (length >= 1U && data[0] != 'R') return ZZ9K_WEBP_PARSE_INVALID;
    if (length >= 2U && (data[0] != 'R' || data[1] != 'I')) return ZZ9K_WEBP_PARSE_INVALID;
    if (length >= 3U && (data[0] != 'R' || data[1] != 'I' || data[2] != 'F')) return ZZ9K_WEBP_PARSE_INVALID;
    return ZZ9K_WEBP_PARSE_INCOMPLETE;
  }
  if (memcmp(data, "RIFF", 4U) != 0) {
    return ZZ9K_WEBP_PARSE_INVALID;
  }

  if (length < 8U) {
    return ZZ9K_WEBP_PARSE_INCOMPLETE;
  }
  riff_size = zz9k_webp_read_le32(data + 4U);
  if (riff_size < 4U) {
    return ZZ9K_WEBP_PARSE_INVALID;
  }

  if (length < 12U) {
    if (length >= 9U && data[8] != 'W') return ZZ9K_WEBP_PARSE_INVALID;
    if (length >= 10U && (data[8] != 'W' || data[9] != 'E')) return ZZ9K_WEBP_PARSE_INVALID;
    if (length >= 11U && (data[8] != 'W' || data[9] != 'E' || data[10] != 'B')) return ZZ9K_WEBP_PARSE_INVALID;
    return ZZ9K_WEBP_PARSE_INCOMPLETE;
  }
  if (memcmp(data + 8U, "WEBP", 4U) != 0) {
    return ZZ9K_WEBP_PARSE_INVALID;
  }

  if (length < 16U) {
    return ZZ9K_WEBP_PARSE_INCOMPLETE;
  }

  is_vp8 = (memcmp(data + 12U, "VP8 ", 4U) == 0);
  is_vp8l = (memcmp(data + 12U, "VP8L", 4U) == 0);
  is_vp8x = (memcmp(data + 12U, "VP8X", 4U) == 0);

  if (!is_vp8 && !is_vp8l && !is_vp8x) {
    return ZZ9K_WEBP_PARSE_INVALID;
  }

  if (length < 20U) {
    return ZZ9K_WEBP_PARSE_INCOMPLETE;
  }
  chunk_size = zz9k_webp_read_le32(data + 16U);
  if (riff_size < chunk_size + 12U) {
    return ZZ9K_WEBP_PARSE_INVALID;
  }

  if (is_vp8) {
    uint16_t raw_w;
    uint16_t raw_h;
    uint32_t w;
    uint32_t h;

    if (chunk_size < 10U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (length < 30U) {
      return ZZ9K_WEBP_PARSE_INCOMPLETE;
    }
    if ((data[20] & 1U) != 0U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (data[23] != 0x9DU || data[24] != 0x01U || data[25] != 0x2AU) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    raw_w = (uint16_t)data[26] | ((uint16_t)data[27] << 8);
    raw_h = (uint16_t)data[28] | ((uint16_t)data[29] << 8);
    w = (uint32_t)(raw_w & 0x3FFFU);
    h = (uint32_t)(raw_h & 0x3FFFU);
    if (w == 0U || h == 0U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (out_header) {
      out_header->width = w;
      out_header->height = h;
      out_header->format = ZZ9K_WEBP_FORMAT_LOSSY;
      out_header->has_alpha = 0U;
      out_header->is_animated = 0U;
    }
    return ZZ9K_WEBP_PARSE_READY;
  }

  if (is_vp8l) {
    uint32_t val;
    uint32_t w;
    uint32_t h;
    uint8_t alpha;
    uint8_t version;

    if (chunk_size < 5U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (length < 25U) {
      return ZZ9K_WEBP_PARSE_INCOMPLETE;
    }
    if (data[20] != 0x2FU) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    val = zz9k_webp_read_le32(data + 21U);
    w = (val & 0x3FFFU) + 1U;
    h = ((val >> 14) & 0x3FFFU) + 1U;
    alpha = (uint8_t)((val >> 28) & 1U);
    version = (uint8_t)((val >> 29) & 7U);
    if (version != 0U || w == 0U || h == 0U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (out_header) {
      out_header->width = w;
      out_header->height = h;
      out_header->format = ZZ9K_WEBP_FORMAT_LOSSLESS;
      out_header->has_alpha = alpha;
      out_header->is_animated = 0U;
    }
    return ZZ9K_WEBP_PARSE_READY;
  }

  /* is_vp8x */
  {
    uint8_t is_anim;
    uint8_t has_alpha;
    uint32_t w;
    uint32_t h;

    if (chunk_size < 10U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (length < 30U) {
      return ZZ9K_WEBP_PARSE_INCOMPLETE;
    }
    if ((data[20] & 0xC1U) != 0U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    is_anim = (data[20] & 0x02U) != 0U ? 1U : 0U;
    has_alpha = (data[20] & 0x10U) != 0U ? 1U : 0U;
    if (data[21] != 0U || data[22] != 0U || data[23] != 0U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    w = 1U + zz9k_webp_read_le24(data + 24U);
    h = 1U + zz9k_webp_read_le24(data + 27U);
    if (w == 0U || h == 0U) {
      return ZZ9K_WEBP_PARSE_INVALID;
    }
    if (out_header) {
      out_header->width = w;
      out_header->height = h;
      out_header->format = ZZ9K_WEBP_FORMAT_EXTENDED;
      out_header->has_alpha = has_alpha;
      out_header->is_animated = is_anim;
    }
    return ZZ9K_WEBP_PARSE_READY;
  }
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_IMAGE_H */
