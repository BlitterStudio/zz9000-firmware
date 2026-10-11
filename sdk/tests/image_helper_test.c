/*
 * Unit checks for public image decode/session descriptor helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/image.h"

#include <stdint.h>
#include <string.h>

static int test_decode_descriptor(void)
{
  ZZ9KImageDecodeDesc desc;
  ZZ9KRect rect;

  rect.x = 3U;
  rect.y = 4U;
  rect.w = 320U;
  rect.h = 200U;
  memset(&desc, 0xff, sizeof(desc));
  if (!zz9k_image_build_decode_desc(&desc, 0x40000010UL, 0x20U,
                                    4096U, 0x40000020UL, &rect,
                                    ZZ9K_SURFACE_FORMAT_BGRA8888, 0x80U)) {
    return 1;
  }
  if (desc.src_handle != 0x40000010UL || desc.src_offset != 0x20U ||
      desc.src_length != 4096U) return 2;
  if (desc.dst_surface != 0x40000020UL ||
      desc.dst_x != 3U || desc.dst_y != 4U ||
      desc.dst_width != 320U || desc.dst_height != 200U) return 3;
  if (desc.output_format != ZZ9K_SURFACE_FORMAT_BGRA8888 ||
      desc.flags != 0x80U) return 4;
  if (zz9k_image_build_decode_desc(&desc, ZZ9K_INVALID_HANDLE, 0U,
                                   4096U, 0x40000020UL, &rect,
                                   ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 5;
  }
  rect.w = 0U;
  if (zz9k_image_build_decode_desc(&desc, 0x40000010UL, 0U, 4096U,
                                   0x40000020UL, &rect,
                                   ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 6;
  }
  return 0;
}

static int test_session_begin_descriptors(void)
{
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KRect rect;

  rect.x = 5U;
  rect.y = 6U;
  rect.w = 640U;
  rect.h = 480U;
  memset(&begin, 0xff, sizeof(begin));
  if (!zz9k_image_build_surface_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_PNG, 0x40000030UL, &rect,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0x10U)) {
    return 1;
  }
  if (begin.codec != ZZ9K_IMAGE_CODEC_PNG ||
      begin.output_mode != ZZ9K_IMAGE_OUTPUT_SURFACE ||
      begin.dst_surface != 0x40000030UL ||
      begin.dst_x != 5U || begin.dst_y != 6U ||
      begin.dst_width != 640U || begin.dst_height != 480U ||
      begin.output_format != ZZ9K_SURFACE_FORMAT_BGRA8888 ||
      begin.flags != 0x10U) return 2;

  if (!zz9k_image_build_framebuffer_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_JPEG, &rect,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 3;
  }
  if (begin.output_mode != ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER ||
      begin.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
      begin.dst_width != 640U || begin.dst_height != 480U) return 4;

  if (!zz9k_image_build_tile_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_GIF, 0x40000040UL, 2048U, 32U,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 5;
  }
  if (begin.output_mode != ZZ9K_IMAGE_OUTPUT_TILE_BUFFER ||
      begin.tile_handle != 0x40000040UL || begin.tile_stride != 2048U ||
      begin.tile_rows != 32U) return 6;
  if (!zz9k_image_build_tile_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_JPEG, 0x40000040UL, 1920U, 32U,
          ZZ9K_SURFACE_FORMAT_RGB888, 0U)) {
    return 10;
  }
  if (begin.output_mode != ZZ9K_IMAGE_OUTPUT_TILE_BUFFER ||
      begin.output_format != ZZ9K_SURFACE_FORMAT_RGB888 ||
      begin.tile_stride != 1920U) return 11;

  if (zz9k_image_build_surface_session_begin_desc(
          &begin, 0xffffffffUL, 0x40000030UL, &rect,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 7;
  }
  if (zz9k_image_build_surface_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_PNG, ZZ9K_INVALID_HANDLE, &rect,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 8;
  }
  if (zz9k_image_build_tile_session_begin_desc(
          &begin, ZZ9K_IMAGE_CODEC_PNG, 0x40000040UL, 0U, 32U,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 9;
  }

  return 0;
}

static int test_session_feed_descriptor(void)
{
  ZZ9KImageSessionFeedDesc feed;

  memset(&feed, 0xff, sizeof(feed));
  if (!zz9k_image_build_session_feed_desc(
          &feed, 7U, 0x40000050UL, 0x60U, 4096U, 0U)) {
    return 1;
  }
  if (feed.session != 7U || feed.src_handle != 0x40000050UL ||
      feed.src_offset != 0x60U || feed.src_length != 4096U ||
      feed.flags != 0U) return 2;
  if (!zz9k_image_build_session_feed_desc(
          &feed, 7U, 0x40000050UL, 0U, 0U,
          ZZ9K_IMAGE_SESSION_FEED_EOF)) {
    return 3;
  }
  if (feed.src_length != 0U ||
      feed.flags != ZZ9K_IMAGE_SESSION_FEED_EOF) return 4;
  if (zz9k_image_build_session_feed_desc(
          &feed, 0U, 0x40000050UL, 0U, 1U, 0U)) {
    return 5;
  }
  if (zz9k_image_build_session_feed_desc(
          &feed, 7U, ZZ9K_INVALID_HANDLE, 0U, 1U, 0U)) {
    return 6;
  }
  if (zz9k_image_build_session_feed_desc(
          &feed, 7U, 0x40000050UL, 0U, 0U, 0U)) {
    return 7;
  }
  if (zz9k_image_build_session_feed_desc(
          &feed, 7U, 0x40000050UL, 0U, 1U, 0x80000000UL)) {
    return 8;
  }
  return 0;
}

static int test_session_service_flags(void)
{
  uint32_t flags;

  flags = 0U;
  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_JPEG, ZZ9K_IMAGE_OUTPUT_TILE_BUFFER, &flags)) {
    return 1;
  }
  if (flags != (ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA |
                ZZ9K_SERVICE_FLAG_IMAGE_TILE_OUTPUT)) {
    return 2;
  }

  flags = 0U;
  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_PNG, ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER, &flags)) {
    return 3;
  }
  if (flags != (ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA |
                ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT)) {
    return 4;
  }

  flags = 0xffffffffUL;
  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_JPEG, ZZ9K_IMAGE_OUTPUT_SURFACE, &flags)) {
    return 5;
  }
  if (flags != (ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA)) {
    return 6;
  }

  flags = 0U;
  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_WEBP, ZZ9K_IMAGE_OUTPUT_TILE_BUFFER, &flags)) {
    return 10;
  }
  if (flags != (ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_WEBP |
                ZZ9K_SERVICE_FLAG_IMAGE_TILE_OUTPUT)) {
    return 11;
  }

  flags = 0U;
  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_WEBP, ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER, &flags)) {
    return 12;
  }
  if (flags != (ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_WEBP |
                ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT)) {
    return 13;
  }

  flags = 0xffffffffUL;
  if (!zz9k_image_stream_required_service_flags(
          ZZ9K_IMAGE_CODEC_WEBP, ZZ9K_IMAGE_OUTPUT_SURFACE, &flags)) {
    return 14;
  }
  if (flags != (ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_WEBP)) {
    return 15;
  }

  if (zz9k_image_stream_required_service_flags(
        ZZ9K_IMAGE_CODEC_GIF, ZZ9K_IMAGE_OUTPUT_TILE_BUFFER, &flags)) {
    return 7;
  }
  if (zz9k_image_stream_required_service_flags(
        ZZ9K_IMAGE_CODEC_JPEG, 0xffffffffUL, &flags)) {
    return 8;
  }
  if (zz9k_image_stream_required_service_flags(
        ZZ9K_IMAGE_CODEC_JPEG, ZZ9K_IMAGE_OUTPUT_TILE_BUFFER, 0)) {
    return 9;
  }

  return 0;
}

static int test_image_codec_known(void)
{
  if (!zz9k_image_codec_known(ZZ9K_IMAGE_CODEC_JPEG)) return 1;
  if (!zz9k_image_codec_known(ZZ9K_IMAGE_CODEC_PNG)) return 2;
  if (!zz9k_image_codec_known(ZZ9K_IMAGE_CODEC_GIF)) return 3;
  if (!zz9k_image_codec_known(ZZ9K_IMAGE_CODEC_WEBP)) return 4;
  if (zz9k_image_codec_known(0xffffffffUL)) return 5;
  if (zz9k_image_codec_known(0U)) return 6;
  return 0;
}

static int test_webp_header_parser(void)
{
  static const uint8_t webp_lossy[] = {
    0x52, 0x49, 0x46, 0x46, 0x64, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x20, 0x58, 0x00, 0x00, 0x00, 0xf0, 0x02, 0x00, 0x9d,
    0x01, 0x2a, 0x03, 0x00, 0x05, 0x00, 0x02, 0x00, 0x34, 0x25, 0xb0, 0x02
  };
  static const uint8_t webp_alpha[] = {
    0x52, 0x49, 0x46, 0x46, 0x62, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x4c, 0x55, 0x00, 0x00, 0x00, 0x2f, 0x02, 0xc0, 0x00,
    0x10, 0x57, 0x40, 0x20, 0x40, 0x91
  };
  static const uint8_t webp_anim[] = {
    0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00
  };
  static const uint8_t wave_riff[] = {
    0x52, 0x49, 0x46, 0x46, 0x24, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45,
    0x66, 0x6d, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00
  };
  static const uint8_t avi_riff[] = {
    0x52, 0x49, 0x46, 0x46, 0x24, 0x00, 0x00, 0x00, 0x41, 0x56, 0x49, 0x20,
    0x4c, 0x49, 0x53, 0x54, 0x10, 0x00, 0x00, 0x00
  };
  ZZ9KWebPHeader header;
  uint8_t corrupt[36];
  size_t i;

  /* 1. Lossy VP8 */
  if (zz9k_webp_parse_header(webp_lossy, sizeof(webp_lossy), &header) !=
      ZZ9K_WEBP_PARSE_READY) {
    return 1;
  }
  if (header.width != 3U || header.height != 5U ||
      header.format != ZZ9K_WEBP_FORMAT_LOSSY ||
      header.has_alpha != 0U || header.is_animated != 0U) {
    return 2;
  }

  /* 2. Lossless VP8L */
  if (zz9k_webp_parse_header(webp_alpha, sizeof(webp_alpha), &header) !=
      ZZ9K_WEBP_PARSE_READY) {
    return 3;
  }
  if (header.width != 3U || header.height != 4U ||
      header.format != ZZ9K_WEBP_FORMAT_LOSSLESS ||
      header.has_alpha != 1U || header.is_animated != 0U) {
    return 4;
  }

  /* 3. Extended VP8X (animated canvas preview) */
  if (zz9k_webp_parse_header(webp_anim, sizeof(webp_anim), &header) !=
      ZZ9K_WEBP_PARSE_READY) {
    return 5;
  }
  if (header.width != 6U || header.height != 4U ||
      header.format != ZZ9K_WEBP_FORMAT_EXTENDED ||
      header.has_alpha != 0U || header.is_animated != 1U) {
    return 6;
  }

  /* 4. Incomplete truncated buffers */
  for (i = 0U; i < 30U; ++i) {
    if (i < sizeof(webp_lossy)) {
      if (zz9k_webp_parse_header(webp_lossy, (uint32_t)i, &header) !=
          ZZ9K_WEBP_PARSE_INCOMPLETE) {
        return 10 + (int)i;
      }
    }
  }

  /* 5. Reject RIFF/WAVE and RIFF/AVI */
  if (zz9k_webp_parse_header(wave_riff, sizeof(wave_riff), &header) !=
      ZZ9K_WEBP_PARSE_INVALID) {
    return 41;
  }
  if (zz9k_webp_parse_header(avi_riff, sizeof(avi_riff), &header) !=
      ZZ9K_WEBP_PARSE_INVALID) {
    return 42;
  }

  /* 6. NULL checks */
  if (zz9k_webp_parse_header(0, 30U, &header) != ZZ9K_WEBP_PARSE_INVALID) {
    return 43;
  }
  /* NULL out_header is allowed and returns status */
  if (zz9k_webp_parse_header(webp_lossy, sizeof(webp_lossy), 0) !=
      ZZ9K_WEBP_PARSE_READY) {
    return 44;
  }

  /* 7. Corrupt start code in VP8 */
  memcpy(corrupt, webp_lossy, sizeof(webp_lossy));
  corrupt[23] = 0x00U;
  if (zz9k_webp_parse_header(corrupt, sizeof(corrupt), &header) !=
      ZZ9K_WEBP_PARSE_INVALID) {
    return 45;
  }

  /* 8. Corrupt signature in VP8L */
  memcpy(corrupt, webp_alpha, sizeof(webp_alpha));
  corrupt[20] = 0x00U;
  if (zz9k_webp_parse_header(corrupt, sizeof(corrupt), &header) !=
      ZZ9K_WEBP_PARSE_INVALID) {
    return 46;
  }

  /* 9. Corrupt reserved bits in VP8X */
  memcpy(corrupt, webp_anim, sizeof(webp_anim));
  corrupt[20] |= 0x80U;
  if (zz9k_webp_parse_header(corrupt, sizeof(corrupt), &header) !=
      ZZ9K_WEBP_PARSE_INVALID) {
    return 47;
  }

  return 0;
}
int main(void)
{
  int result;

  result = test_decode_descriptor();
  if (result) return 10 + result;

  result = test_session_begin_descriptors();
  if (result) return 30 + result;

  result = test_session_feed_descriptor();
  if (result) return 60 + result;

  result = test_session_service_flags();
  if (result) return 90 + result;

  result = test_image_codec_known();
  if (result) return 120 + result;

  result = test_webp_header_parser();
  if (result) return 150 + result;

  return 0;
}
