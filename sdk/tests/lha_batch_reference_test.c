/*
 * End-to-end batch-arena reference test: builds a multi-member arena from
 * the real LHA fixtures, decodes it with a reference implementation of the
 * firmware batch loop (driving the vendored LHa-for-UNIX core), and checks
 * per-member results. This is the executable specification for the
 * firmware's handle_decompress_batch.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/batch.h"
#include "lha_real_fixtures.h"
#include "zz9k_lha_unix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int method_from_algorithm(uint32_t algorithm)
{
  switch (algorithm) {
  case ZZ9K_COMPRESSION_LH1: return 1;
  case ZZ9K_COMPRESSION_LH5: return 5;
  case ZZ9K_COMPRESSION_LH6: return 6;
  case ZZ9K_COMPRESSION_LH7: return 7;
  default: return -1;
  }
}

/* Reference implementation of the firmware batch loop: parse, decode each
   member in order, write per-member results, never abort the batch on a
   member failure. Returns 1 unless the arena itself is malformed. */
static int reference_batch_decode(uint8_t *arena, uint32_t arena_length)
{
  ZZ9KBatchParsedHeader h;
  uint32_t i;

  if (zz9k_batch_parse_header(arena, arena_length, &h) != ZZ9K_STATUS_OK) {
    return 0;
  }
  for (i = 0U; i < h.member_count; i++) {
    ZZ9KBatchMemberDesc desc;
    ZZ9KBatchMemberResult result;
    FILE *in;
    FILE *out = 0;
    uint16_t crc = 0U;
    int method;
    int ok = 0;

    zz9k_batch_read_desc(arena + h.desc_offset + i * ZZ9K_BATCH_DESC_SIZE,
                         &desc);
    memset(&result, 0, sizeof(result));

    if (h.mode == ZZ9K_BATCH_MODE_TEST &&
        desc.uncompressed_size > ZZ9K_BATCH_TEST_MAX_EXPECTED) {
      /* Mirrors the firmware: TEST-mode decode-and-discard has no arena
         region to bound uncompressed_size, so a row claiming more than the
         cap is rejected outright rather than attempted. */
      result.status = ZZ9K_STATUS_BAD_REQUEST;
      zz9k_batch_write_result(
          arena + h.result_offset + i * ZZ9K_BATCH_RESULT_SIZE, &result);
      continue;
    }

    method = method_from_algorithm(desc.algorithm);

    if (method > 0 && desc.src_length != 0U &&
        desc.uncompressed_size != 0U &&
        zz9k_batch_range_ok(h.blob_length, desc.src_offset,
                            desc.src_length) &&
        (h.mode != ZZ9K_BATCH_MODE_EXTRACT ||
         zz9k_batch_range_ok(h.output_capacity, desc.dst_offset,
                             desc.uncompressed_size))) {
      in = tmpfile();
      if (!in) {
        return 0;
      }
      if (desc.src_length != 0U &&
          fwrite(arena + h.blob_offset + desc.src_offset, 1U,
                 desc.src_length, in) != desc.src_length) {
        fclose(in);
        return 0;
      }
      fseek(in, 0L, SEEK_SET);
      if (h.mode == ZZ9K_BATCH_MODE_EXTRACT) {
        out = tmpfile();
        if (!out) {
          fclose(in);
          return 0;
        }
      }
      ok = zz9k_lha_unix_decode_method(
          in, out, desc.uncompressed_size, desc.src_length,
          (uint16_t)desc.expected_crc,
          (desc.flags & ZZ9K_BATCH_MEMBER_FLAG_HAVE_CRC) != 0U, &crc,
          method);
      if (ok && h.mode == ZZ9K_BATCH_MODE_EXTRACT) {
        fseek(out, 0L, SEEK_SET);
        if (fread(arena + h.output_offset + desc.dst_offset, 1U,
                  desc.uncompressed_size, out) != desc.uncompressed_size) {
          ok = 0;
        }
      }
      fclose(in);
      if (out) {
        fclose(out);
      }
    }

    result.status = ok ? ZZ9K_STATUS_OK : ZZ9K_STATUS_BAD_REQUEST;
    result.bytes_written = ok ? desc.uncompressed_size : 0U;
    result.checksum = crc;
    zz9k_batch_write_result(
        arena + h.result_offset + i * ZZ9K_BATCH_RESULT_SIZE, &result);
  }
  return 1;
}

/* Build an arena holding all fixtures. Returns the arena (caller frees)
   and fills layout. mode is ZZ9K_BATCH_MODE_TEST or _EXTRACT. */
static uint8_t *build_fixture_arena(uint32_t mode, ZZ9KBatchLayout *layout)
{
  uint32_t blob_total = 0U;
  uint32_t out_total = 0U;
  uint32_t blob = 0U;
  uint32_t out = 0U;
  uint8_t *arena;
  uint32_t i;

  for (i = 0U; i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    blob_total += zz9k_lha_fixtures[i].compressed_size;
    out_total += zz9k_lha_fixtures[i].uncompressed_size;
  }
  if (!zz9k_batch_layout_init(layout, mode, ZZ9K_LHA_FIXTURE_COUNT,
                              blob_total,
                              mode == ZZ9K_BATCH_MODE_EXTRACT ? out_total
                                                              : 0U)) {
    return 0;
  }
  arena = (uint8_t *)calloc(1U, layout->total_size);
  if (!arena) {
    return 0;
  }
  zz9k_batch_write_header(arena, layout, ZZ9K_LHA_FIXTURE_COUNT, blob_total);
  for (i = 0U; i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    ZZ9KBatchMemberDesc desc;

    desc.algorithm = ZZ9K_COMPRESSION_LH5; /* all fixtures are -lh5- */
    desc.src_offset = blob;
    desc.src_length = zz9k_lha_fixtures[i].compressed_size;
    desc.dst_offset = out;
    desc.uncompressed_size = zz9k_lha_fixtures[i].uncompressed_size;
    desc.expected_crc = zz9k_lha_fixtures[i].crc16;
    desc.flags = ZZ9K_BATCH_MEMBER_FLAG_HAVE_CRC;
    zz9k_batch_write_desc(
        arena + layout->desc_offset + i * ZZ9K_BATCH_DESC_SIZE, &desc);
    memcpy(arena + layout->blob_offset + blob,
           zz9k_lha_fixtures[i].compressed,
           zz9k_lha_fixtures[i].compressed_size);
    blob += zz9k_lha_fixtures[i].compressed_size;
    out += zz9k_lha_fixtures[i].uncompressed_size;
  }
  return arena;
}

static int test_batch_test_mode_all_members_verify(void)
{
  ZZ9KBatchLayout layout;
  uint8_t *arena = build_fixture_arena(ZZ9K_BATCH_MODE_TEST, &layout);
  uint32_t i;
  int rc = 0;

  if (!arena) {
    return 1;
  }
  if (!reference_batch_decode(arena, layout.total_size)) {
    rc = 2;
  }
  for (i = 0U; rc == 0 && i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    ZZ9KBatchMemberResult result;

    zz9k_batch_read_result(
        arena + layout.result_offset + i * ZZ9K_BATCH_RESULT_SIZE, &result);
    if (result.status != ZZ9K_STATUS_OK) rc = 10 + (int)i;
    else if (result.bytes_written != zz9k_lha_fixtures[i].uncompressed_size)
      rc = 20 + (int)i;
    else if ((uint16_t)result.checksum != zz9k_lha_fixtures[i].crc16)
      rc = 30 + (int)i;
  }
  free(arena);
  return rc;
}

static int test_batch_extract_mode_is_byte_correct(void)
{
  ZZ9KBatchLayout layout;
  uint8_t *arena = build_fixture_arena(ZZ9K_BATCH_MODE_EXTRACT, &layout);
  uint32_t out = 0U;
  uint32_t i;
  int rc = 0;

  if (!arena) {
    return 1;
  }
  if (!reference_batch_decode(arena, layout.total_size)) {
    rc = 2;
  }
  for (i = 0U; rc == 0 && i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    ZZ9KBatchMemberResult result;

    zz9k_batch_read_result(
        arena + layout.result_offset + i * ZZ9K_BATCH_RESULT_SIZE, &result);
    if (result.status != ZZ9K_STATUS_OK) {
      rc = 10 + (int)i;
    } else if (memcmp(arena + layout.output_offset + out,
                      zz9k_lha_fixtures[i].expected,
                      zz9k_lha_fixtures[i].uncompressed_size) != 0) {
      rc = 20 + (int)i;
    }
    out += zz9k_lha_fixtures[i].uncompressed_size;
  }
  free(arena);
  return rc;
}

static int test_corrupt_member_fails_alone(void)
{
  ZZ9KBatchLayout layout;
  uint8_t *arena = build_fixture_arena(ZZ9K_BATCH_MODE_TEST, &layout);
  ZZ9KBatchMemberDesc desc;
  uint32_t i;
  int rc = 0;

  if (!arena) {
    return 1;
  }
  /* Corrupt the middle of member 1's compressed stream. */
  zz9k_batch_read_desc(arena + layout.desc_offset + 1U * ZZ9K_BATCH_DESC_SIZE,
                       &desc);
  arena[layout.blob_offset + desc.src_offset + desc.src_length / 2U] ^= 0xffU;

  if (!reference_batch_decode(arena, layout.total_size)) {
    rc = 2;
  }
  for (i = 0U; rc == 0 && i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    ZZ9KBatchMemberResult result;

    zz9k_batch_read_result(
        arena + layout.result_offset + i * ZZ9K_BATCH_RESULT_SIZE, &result);
    if (i == 1U) {
      /* Either the decoder aborted or the CRC must miss. */
      if (result.status == ZZ9K_STATUS_OK &&
          (uint16_t)result.checksum == zz9k_lha_fixtures[i].crc16) {
        rc = 10;
      }
    } else if (result.status != ZZ9K_STATUS_OK ||
               (uint16_t)result.checksum != zz9k_lha_fixtures[i].crc16) {
      rc = 20 + (int)i; /* neighbors must be untouched */
    }
  }
  free(arena);
  return rc;
}

static int test_test_mode_oversize_expected_rejected(void)
{
  ZZ9KBatchLayout layout;
  uint8_t *arena = build_fixture_arena(ZZ9K_BATCH_MODE_TEST, &layout);
  ZZ9KBatchMemberDesc desc;
  uint32_t i;
  int rc = 0;

  if (!arena) {
    return 1;
  }
  /* Claim an expected size just over the TEST-mode cap for member 1; the
     cap has no bearing on the other members. */
  zz9k_batch_read_desc(arena + layout.desc_offset + 1U * ZZ9K_BATCH_DESC_SIZE,
                       &desc);
  desc.uncompressed_size = ZZ9K_BATCH_TEST_MAX_EXPECTED + 1U;
  zz9k_batch_write_desc(
      arena + layout.desc_offset + 1U * ZZ9K_BATCH_DESC_SIZE, &desc);

  if (!reference_batch_decode(arena, layout.total_size)) {
    rc = 2;
  }
  for (i = 0U; rc == 0 && i < ZZ9K_LHA_FIXTURE_COUNT; i++) {
    ZZ9KBatchMemberResult result;

    zz9k_batch_read_result(
        arena + layout.result_offset + i * ZZ9K_BATCH_RESULT_SIZE, &result);
    if (i == 1U) {
      if (result.status != ZZ9K_STATUS_BAD_REQUEST) rc = 10;
    } else if (result.status != ZZ9K_STATUS_OK ||
               (uint16_t)result.checksum != zz9k_lha_fixtures[i].crc16) {
      rc = 20 + (int)i; /* neighbors must be untouched */
    }
  }
  free(arena);
  return rc;
}

int main(void)
{
  int rc;

  if ((rc = test_batch_test_mode_all_members_verify()) != 0) return 100 + rc;
  if ((rc = test_batch_extract_mode_is_byte_correct()) != 0) return 200 + rc;
  if ((rc = test_corrupt_member_fails_alone()) != 0) return 300 + rc;
  if ((rc = test_test_mode_oversize_expected_rejected()) != 0) return 400 + rc;
  return 0;
}
