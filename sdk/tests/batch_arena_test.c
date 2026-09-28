/*
 * Batch arena layout + serialization round-trip checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/batch.h"
#include <string.h>

static int test_layout_test_mode(void)
{
  ZZ9KBatchLayout layout;

  if (!zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 64U,
                              1024U * 1024U, 0U)) {
    return 1;
  }
  if (layout.desc_offset != ZZ9K_BATCH_HEADER_SIZE) return 2;
  if (layout.desc_offset % 16U != 0U) return 3;
  if (layout.blob_offset < layout.desc_offset + 64U * ZZ9K_BATCH_DESC_SIZE) {
    return 4;
  }
  if (layout.blob_offset % 16U != 0U) return 5;
  if (layout.output_offset != 0U || layout.output_capacity != 0U) return 6;
  if (layout.result_offset < layout.blob_offset + 1024U * 1024U) return 7;
  if (layout.result_offset % 16U != 0U) return 8;
  if (layout.total_size !=
      layout.result_offset + 64U * ZZ9K_BATCH_RESULT_SIZE) {
    return 9;
  }
  return 0;
}

static int test_layout_extract_mode(void)
{
  ZZ9KBatchLayout layout;

  if (!zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_EXTRACT, 8U, 4096U,
                              8192U)) {
    return 1;
  }
  if (layout.output_offset < layout.blob_offset + 4096U) return 2;
  if (layout.output_offset % 16U != 0U) return 3;
  if (layout.output_capacity != 8192U) return 4;
  if (layout.result_offset < layout.output_offset + 8192U) return 5;
  return 0;
}

static int test_layout_rejects_bad_args(void)
{
  ZZ9KBatchLayout layout;

  if (zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 0U, 4096U, 0U)) {
    return 1;
  }
  if (zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST,
                             ZZ9K_BATCH_MEMBER_LIMIT + 1U, 4096U, 0U)) {
    return 2;
  }
  if (zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 8U, 0U, 0U)) {
    return 3;
  }
  if (zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 8U, 4096U,
                             4096U)) {
    return 4; /* TEST must have no output region */
  }
  if (zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_EXTRACT, 8U, 4096U,
                             0U)) {
    return 5; /* EXTRACT must have an output region */
  }
  if (zz9k_batch_layout_init(&layout, 2U, 8U, 4096U, 0U)) {
    return 6; /* unknown mode */
  }
  if (zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 8U,
                             0x10000001UL, 0U)) {
    return 7; /* budget cap */
  }
  return 0;
}

static int test_header_round_trip(void)
{
  ZZ9KBatchLayout layout;
  ZZ9KBatchParsedHeader parsed;
  uint8_t arena[4096];

  memset(arena, 0, sizeof(arena));
  if (!zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 4U, 512U, 0U)) {
    return 1;
  }
  if (layout.total_size > sizeof(arena)) return 2;
  zz9k_batch_write_header(arena, &layout, 3U, 400U);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) !=
      ZZ9K_STATUS_OK) {
    return 3;
  }
  if (parsed.mode != ZZ9K_BATCH_MODE_TEST) return 4;
  if (parsed.member_count != 3U) return 5;
  if (parsed.desc_offset != layout.desc_offset) return 6;
  if (parsed.blob_offset != layout.blob_offset) return 7;
  if (parsed.blob_length != 400U) return 8;
  if (parsed.output_offset != 0U || parsed.output_capacity != 0U) return 9;
  if (parsed.result_offset != layout.result_offset) return 10;
  return 0;
}

static int test_parse_rejects_malformed(void)
{
  ZZ9KBatchLayout layout;
  ZZ9KBatchParsedHeader parsed;
  uint8_t arena[4096];

  memset(arena, 0, sizeof(arena));
  if (!zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_TEST, 4U, 512U, 0U)) {
    return 1;
  }
  zz9k_batch_write_header(arena, &layout, 3U, 400U);

  /* bad magic */
  arena[0] ^= 0xffU;
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) ==
      ZZ9K_STATUS_OK) {
    return 2;
  }
  arena[0] ^= 0xffU;

  /* future version -> UNSUPPORTED */
  zz9k_put_be16(arena + ZZ9K_BATCH_HDR_VERSION, 2U);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) !=
      ZZ9K_STATUS_UNSUPPORTED) {
    return 3;
  }
  zz9k_put_be16(arena + ZZ9K_BATCH_HDR_VERSION, ZZ9K_BATCH_ARENA_VERSION);

  /* member_count 0 and over-limit */
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_MEMBER_COUNT, 0U);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) ==
      ZZ9K_STATUS_OK) {
    return 4;
  }
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_MEMBER_COUNT,
                ZZ9K_BATCH_MEMBER_LIMIT + 1U);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) ==
      ZZ9K_STATUS_OK) {
    return 5;
  }
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_MEMBER_COUNT, 3U);

  /* blob range escaping the arena */
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_BLOB_LENGTH, layout.total_size);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) ==
      ZZ9K_STATUS_OK) {
    return 6;
  }
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_BLOB_LENGTH, 400U);

  /* truncated arena */
  if (zz9k_batch_parse_header(arena, ZZ9K_BATCH_HEADER_SIZE - 1U, &parsed) ==
      ZZ9K_STATUS_OK) {
    return 7;
  }

  /* overlapping regions: point result_offset INTO the blob region -- each
     region individually still fits the arena, but they now alias. */
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_RESULT_OFFSET, layout.blob_offset);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) ==
      ZZ9K_STATUS_OK) {
    return 8;
  }
  zz9k_put_be32(arena + ZZ9K_BATCH_HDR_RESULT_OFFSET, layout.result_offset);
  if (zz9k_batch_parse_header(arena, layout.total_size, &parsed) !=
      ZZ9K_STATUS_OK) {
    return 9;
  }
  return 0;
}

static int test_desc_and_result_round_trip(void)
{
  ZZ9KBatchMemberDesc desc_in;
  ZZ9KBatchMemberDesc desc_out;
  ZZ9KBatchMemberResult res_in;
  ZZ9KBatchMemberResult res_out;
  uint8_t desc_bytes[ZZ9K_BATCH_DESC_SIZE];
  uint8_t res_bytes[ZZ9K_BATCH_RESULT_SIZE];

  desc_in.algorithm = ZZ9K_COMPRESSION_LH5;
  desc_in.src_offset = 0x100U;
  desc_in.src_length = 0x200U;
  desc_in.dst_offset = 0x300U;
  desc_in.uncompressed_size = 0x400U;
  desc_in.expected_crc = 0xf21cU;
  desc_in.flags = ZZ9K_BATCH_MEMBER_FLAG_HAVE_CRC;
  zz9k_batch_write_desc(desc_bytes, &desc_in);
  zz9k_batch_read_desc(desc_bytes, &desc_out);
  if (memcmp(&desc_in, &desc_out, sizeof(desc_in)) != 0) return 1;
  if (zz9k_get_be32(desc_bytes + ZZ9K_BATCH_DESC_ALGORITHM) !=
      ZZ9K_COMPRESSION_LH5) {
    return 2; /* big-endian on the wire */
  }

  res_in.status = ZZ9K_STATUS_OK;
  res_in.bytes_written = 0x400U;
  res_in.checksum = 0xf21cU;
  zz9k_batch_write_result(res_bytes, &res_in);
  zz9k_batch_read_result(res_bytes, &res_out);
  if (memcmp(&res_in, &res_out, sizeof(res_in)) != 0) return 3;
  return 0;
}

int main(void)
{
  int rc;

  if ((rc = test_layout_test_mode()) != 0) return 100 + rc;
  if ((rc = test_layout_extract_mode()) != 0) return 200 + rc;
  if ((rc = test_layout_rejects_bad_args()) != 0) return 300 + rc;
  if ((rc = test_header_round_trip()) != 0) return 400 + rc;
  if ((rc = test_parse_rejects_malformed()) != 0) return 500 + rc;
  if ((rc = test_desc_and_result_round_trip()) != 0) return 600 + rc;
  return 0;
}
