/*
 * Behavioral checks for ZZ9000 picture DataType memory staging.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Include the class so this m68k-only harness invokes its static staging
 * helpers rather than a duplicate model.
 */
#include "../amiga/datatypes/zz9k_picture_datatype.c"

#include <string.h>

static int test_memory_staging_preserves_bytes_and_position(void)
{
  static const uint8_t source_bytes[] = {
      0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U};
  uint8_t staged_bytes[8];
  ZZ9KPictureSource source;
  ZZ9KSharedBuffer staging;
  uint32_t copied;
  int eof;

  memset(staged_bytes, 0xa5, sizeof(staged_bytes));
  memset(&source, 0, sizeof(source));
  memset(&staging, 0, sizeof(staging));
  source.memory = source_bytes;
  source.size = sizeof(source_bytes);
  source.position = 2U;
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  staging.handle = 1U;
  staging.data = staged_bytes;
  staging.length = sizeof(staged_bytes);
  copied = 0U;
  eof = 0;

  if (!zz9k_picture_read_chunk_to_shared(
          &source, &staging, 0, 0U, 1U, 5U, &copied, &eof, 0)) {
    return 1;
  }
  if (copied != 5U || eof || source.position != 7U ||
      memcmp(staged_bytes + 1U, source_bytes + 2U, copied) != 0) {
    return 2;
  }
  if (staged_bytes[0] != 0xa5U || staged_bytes[6] != 0xa5U ||
      staged_bytes[7] != 0xa5U) {
    return 3;
  }

  copied = 1U;
  if (!zz9k_picture_read_chunk_to_shared(
          &source, &staging, 0, 0U, 6U, 2U, &copied, &eof, 0) ||
      copied != 0U || !eof || source.position != 7U) {
    return 4;
  }

  return 0;
}

static int test_memory_staging_short_read_sets_eof(void)
{
  static const uint8_t source_bytes[] = {0x31U, 0x32U, 0x33U};
  uint8_t staged_bytes[5];
  ZZ9KPictureSource source;
  ZZ9KSharedBuffer staging;
  uint32_t copied;
  int eof;

  memset(staged_bytes, 0, sizeof(staged_bytes));
  memset(&source, 0, sizeof(source));
  memset(&staging, 0, sizeof(staging));
  source.memory = source_bytes;
  source.size = sizeof(source_bytes);
  source.position = 2U;
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  staging.handle = 1U;
  staging.data = staged_bytes;
  staging.length = sizeof(staged_bytes);
  copied = 0U;
  eof = 0;

  if (!zz9k_picture_read_chunk_to_shared(
          &source, &staging, 0, 0U, 0U, sizeof(staged_bytes), &copied,
          &eof, 0)) {
    return 1;
  }
  if (copied != 1U || !eof || source.position != source.size ||
      staged_bytes[0] != source_bytes[2]) {
    return 2;
  }

  return 0;
}

static int test_memory_staging_does_not_advance_on_copy_failure(void)
{
  static const uint8_t source_bytes[] = {0x11U, 0x22U};
  uint8_t staged_bytes[2];
  ZZ9KPictureSource source;
  ZZ9KSharedBuffer staging;
  uint32_t copied;
  int eof;

  memset(&source, 0, sizeof(source));
  memset(&staging, 0, sizeof(staging));
  source.memory = source_bytes;
  source.size = sizeof(source_bytes);
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  staging.handle = 1U;
  staging.data = staged_bytes;
  staging.length = sizeof(staged_bytes);
  copied = 0U;
  eof = 0;
  if (zz9k_picture_read_chunk_to_shared(
          &source, &staging, 0, 0U, staging.length, 1U, &copied, &eof,
          0) ||
      source.position != 0U) {
    return 1;
  }

  staging.handle = ZZ9K_INVALID_HANDLE;
  staging.data = staged_bytes;
  staging.length = sizeof(staged_bytes);
  copied = 0U;
  eof = 0;

  if (zz9k_picture_read_chunk_to_shared(
          &source, &staging, 0, 0U, 0U, sizeof(staged_bytes), &copied,
          &eof, 0) ||
      source.position != 0U) {
    return 2;
  }

  return 0;
}

static int test_null_scratch_requires_memory_source(void)
{
  uint8_t staged_bytes[2];
  ZZ9KPictureSource source;
  ZZ9KSharedBuffer staging;
  uint32_t copied;
  int eof;

  memset(&source, 0, sizeof(source));
  memset(&staging, 0, sizeof(staging));
  source.position = 1U;
  source.type = ZZ9K_PICTURE_SOURCE_FILE;
  staging.handle = 1U;
  staging.data = staged_bytes;
  staging.length = sizeof(staged_bytes);
  copied = 0U;
  eof = 0;

  if (zz9k_picture_read_chunk_to_shared(
          &source, &staging, 0, 0U, 0U, staging.length, &copied, &eof,
          0) ||
      source.position != 1U) {
    return 1;
  }

  return 0;
}
static int test_webp_dimension_reader_and_detection(void)
{
  static const uint8_t webp_test_lossy[] = {
    0x52, 0x49, 0x46, 0x46, 0x64, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x20, 0x58, 0x00, 0x00, 0x00, 0xf0, 0x02, 0x00, 0x9d,
    0x01, 0x2a, 0x03, 0x00, 0x05, 0x00, 0x02, 0x00, 0x34, 0x25, 0xb0, 0x02
  };
  static const uint8_t webp_test_alpha[] = {
    0x52, 0x49, 0x46, 0x46, 0x62, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x4c, 0x55, 0x00, 0x00, 0x00, 0x2f, 0x02, 0xc0, 0x00,
    0x10, 0x57, 0x40, 0x20, 0x40, 0x91
  };
  static const uint8_t webp_test_anim[] = {
    0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00
  };
  static const uint8_t non_webp_wave[] = {
    0x52, 0x49, 0x46, 0x46, 0x24, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45,
    0x66, 0x6d, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00
  };
  ZZ9KPictureSource source;
  ZZ9KPictureCodec codec;
  ZZ9KPicturePngPalette palette;
  uint32_t width;
  uint32_t height;
  int has_alpha;
  uint8_t interlace;

  /* 1. Lossy WebP detection */
  memset(&source, 0, sizeof(source));
  source.memory = webp_test_lossy;
  source.size = sizeof(webp_test_lossy);
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  codec = ZZ9K_PICTURE_CODEC_UNKNOWN;
  width = height = 0U;
  has_alpha = -1;
  interlace = 0xffU;
  if (!zz9k_picture_read_dimensions(
          &source, &codec, &width, &height, &has_alpha, &interlace, &palette)) {
    return 1;
  }
  if (codec != ZZ9K_PICTURE_CODEC_WEBP || width != 3U || height != 5U ||
      has_alpha != 0 || source.position != 0U) {
    return 2;
  }

  /* 2. Lossless WebP detection with alpha */
  memset(&source, 0, sizeof(source));
  source.memory = webp_test_alpha;
  source.size = sizeof(webp_test_alpha);
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  codec = ZZ9K_PICTURE_CODEC_UNKNOWN;
  width = height = 0U;
  has_alpha = -1;
  if (!zz9k_picture_read_dimensions(
          &source, &codec, &width, &height, &has_alpha, &interlace, &palette)) {
    return 3;
  }
  if (codec != ZZ9K_PICTURE_CODEC_WEBP || width != 3U || height != 4U ||
      has_alpha != 1 || source.position != 0U) {
    return 4;
  }

  /* 3. Extended WebP detection (animated canvas geometry) */
  memset(&source, 0, sizeof(source));
  source.memory = webp_test_anim;
  source.size = sizeof(webp_test_anim);
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  codec = ZZ9K_PICTURE_CODEC_UNKNOWN;
  width = height = 0U;
  has_alpha = -1;
  if (!zz9k_picture_read_dimensions(
          &source, &codec, &width, &height, &has_alpha, &interlace, &palette)) {
    return 5;
  }
  if (codec != ZZ9K_PICTURE_CODEC_WEBP || width != 6U || height != 4U ||
      has_alpha != 0 || source.position != 0U) {
    return 6;
  }

  /* 4. Non-WebP RIFF/WAVE rejection and position restoration */
  memset(&source, 0, sizeof(source));
  source.memory = non_webp_wave;
  source.size = sizeof(non_webp_wave);
  source.type = ZZ9K_PICTURE_SOURCE_MEMORY;
  codec = ZZ9K_PICTURE_CODEC_UNKNOWN;
  if (zz9k_picture_read_dimensions(
          &source, &codec, &width, &height, &has_alpha, &interlace, &palette)) {
    return 7;
  }
  if (source.position != 0U) {
    return 8;
  }

  return 0;
}

static int test_webp_codec_mappings_and_names(void)
{
  if (zz9k_picture_image_codec(ZZ9K_PICTURE_CODEC_WEBP) !=
      ZZ9K_IMAGE_CODEC_WEBP) {
    return 1;
  }
  if (strcmp(zz9k_picture_object_name(ZZ9K_PICTURE_CODEC_WEBP),
             "ZZ9000 WebP") != 0) {
    return 2;
  }
  return 0;
}


int main(void)
{
  int result;

  result = test_memory_staging_preserves_bytes_and_position();
  if (result != 0) {
    return result;
  }
  result = test_memory_staging_short_read_sets_eof();
  if (result != 0) {
    return 10 + result;
  }
  result = test_memory_staging_does_not_advance_on_copy_failure();
  if (result != 0) {
    return 20 + result;
  }
  result = test_null_scratch_requires_memory_source();
  if (result != 0) {
    return 30 + result;
  }
  result = test_webp_dimension_reader_and_detection();
  if (result != 0) {
    return 40 + result;
  }
  result = test_webp_codec_mappings_and_names();
  if (result != 0) {
    return 50 + result;
  }

  return 0;
}
