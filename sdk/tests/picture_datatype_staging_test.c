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

  return 0;
}
