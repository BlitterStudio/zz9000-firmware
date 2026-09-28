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

  return 0;
}
