/*
 * Compile checks for the public application SDK umbrella header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/sdk.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
  ZZ9KRect rect;
  ZZ9KSurfaceFillDesc fill;
  ZZ9KImageDecodeDesc decode;
  ZZ9KAudioDecodeDesc audio_decode;
  ZZ9KCryptoHashDesc hash;
  ZZ9KDecompressDesc decompress;
  ZZ9KDecompressTestDesc decompress_test;
  ZZ9KDecompressStreamBeginDesc stream_begin;
  ZZ9KDecompressStreamReadDesc stream_read;
  ZZ9KDecompressStreamFeedDesc stream_feed;
  ZZ9KSharedBuffer buffer;
  uint8_t bytes[8];

  rect.x = 1U;
  rect.y = 2U;
  rect.w = 3U;
  rect.h = 4U;
  if (!zz9k_surface_build_framebuffer_fill_desc(
          &fill, &rect, zz9k_surface_color_rgb(1U, 2U, 3U), 0U)) {
    return 1;
  }

  if (!zz9k_image_build_decode_desc(
          &decode, 0x40000010UL, 0U, 128U, 0x40000020UL, &rect,
          ZZ9K_SURFACE_FORMAT_BGRA8888, 0U)) {
    return 2;
  }

  if (!zz9k_audio_build_decode_desc(
          &audio_decode, 0x40000021UL, 0U, 512U, 0x40000022UL, 0U,
          4096U, 0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE, 0U)) {
    return 12;
  }

  if (!zz9k_crypto_build_hash_desc(
          &hash, ZZ9K_CRYPTO_HASH_SHA256, 0x40000030UL, 0U, 3U,
          0x40000040UL, 0U)) {
    return 3;
  }

  if (!zz9k_compression_build_decompress_desc(
          &decompress, ZZ9K_COMPRESSION_GZIP, 0x40000060UL, 0U, 16U,
          0x40000070UL, 0U, 64U, ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    return 4;
  }

  if (!zz9k_compression_build_decompress_test_desc(
          &decompress_test, ZZ9K_COMPRESSION_LZMA_ALONE, 0x40000080UL,
          0U, 128U, 4096U, ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    return 7;
  }

  if (!zz9k_compression_build_decompress_stream_begin_desc(
          &stream_begin, ZZ9K_COMPRESSION_LZMA_ALONE, 0x40000090UL,
          0U, 128U, 4096U, ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    return 8;
  }
  if (!zz9k_compression_build_decompress_stream_begin_desc(
          &stream_begin, ZZ9K_COMPRESSION_LZMA2, ZZ9K_INVALID_HANDLE,
          0U, 0U, 4096U,
          ZZ9K_DECOMPRESS_FLAG_EXPECT_END |
          ZZ9K_DECOMPRESS_FLAG_FEED_INPUT)) {
    return 11;
  }

  if (!zz9k_compression_build_decompress_stream_read_desc(
          &stream_read, 1U, 0x400000a0UL, 0U, 32768U, 0U)) {
    return 9;
  }

  if (!zz9k_compression_build_decompress_stream_feed_desc(
          &stream_feed, 1U, 0x400000b0UL, 0U, 32768U,
          ZZ9K_DECOMPRESS_STREAM_FEED_EOF)) {
    return 10;
  }

  memset(bytes, 0, sizeof(bytes));
  memset(&buffer, 0, sizeof(buffer));
  buffer.handle = 0x40000050UL;
  buffer.data = bytes;
  buffer.length = sizeof(bytes);
  if (!zz9k_shared_set(&buffer, 0U, 0x5aU, sizeof(bytes))) {
    return 5;
  }
  if (zz9k_status_text(ZZ9K_STATUS_OK)[0] == '\0') {
    return 6;
  }

  return 0;
}
