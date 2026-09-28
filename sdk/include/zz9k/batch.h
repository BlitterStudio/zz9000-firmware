/*
 * Header-only helpers for the batched LZH decode arena
 * (ZZ9K_OP_DECOMPRESS_BATCH). The tool uses the writers to build the
 * arena image, the firmware mirrors the parse/validation logic, and the
 * host tests use both sides as an executable specification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_BATCH_H
#define ZZ9K_BATCH_H

#include "zz9k/abi.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZZ9KBatchLayout {
  uint32_t mode;
  uint32_t member_capacity;
  uint32_t desc_offset;
  uint32_t blob_offset;
  uint32_t blob_capacity;
  uint32_t output_offset;   /* 0 in TEST mode */
  uint32_t output_capacity; /* 0 in TEST mode */
  uint32_t result_offset;
  uint32_t total_size;
} ZZ9KBatchLayout;

typedef struct ZZ9KBatchParsedHeader {
  uint32_t mode;
  uint32_t member_count;
  uint32_t desc_offset;
  uint32_t blob_offset;
  uint32_t blob_length;
  uint32_t output_offset;
  uint32_t output_capacity;
  uint32_t result_offset;
} ZZ9KBatchParsedHeader;

static inline uint32_t zz9k_batch_align16(uint32_t value)
{
  return (value + 15U) & ~(uint32_t)15U;
}

/* offset/length window fits within total (overflow-safe). */
static inline int zz9k_batch_range_ok(uint32_t total, uint32_t offset,
                                      uint32_t length)
{
  return offset <= total && length <= total - offset;
}

/* True iff [a_off, a_off+a_len) and [b_off, b_off+b_len) overlap. The two
   regions being compared have each already passed zz9k_batch_range_ok
   against arena_length, but their *sum* (offset+length) can still reach up
   to 2 * arena_length -- which would wrap a 32-bit accumulator if
   arena_length exceeds 2^31 (zz9k_batch_layout_init caps its own budgets
   well below that, but this parser also validates arenas built by a
   foreign/untrusted producer, so it cannot rely on that cap). Compute the
   end-of-range sums in 64 bits to stay exact regardless. */
static inline int zz9k_batch_ranges_overlap(uint32_t a_off, uint32_t a_len,
                                            uint32_t b_off, uint32_t b_len)
{
  uint64_t a_end = (uint64_t)a_off + (uint64_t)a_len;
  uint64_t b_end = (uint64_t)b_off + (uint64_t)b_len;

  return !(a_end <= (uint64_t)b_off || b_end <= (uint64_t)a_off);
}

/* Compute the arena layout for a mode and budgets. Returns 1 on success,
   0 on invalid arguments. Budgets are capped at 256 MB each so the
   offset arithmetic cannot wrap 32 bits. */
static inline int zz9k_batch_layout_init(ZZ9KBatchLayout *layout,
                                         uint32_t mode,
                                         uint32_t member_capacity,
                                         uint32_t blob_capacity,
                                         uint32_t output_capacity)
{
  uint32_t cursor;

  if (!layout || member_capacity == 0U ||
      member_capacity > ZZ9K_BATCH_MEMBER_LIMIT || blob_capacity == 0U ||
      blob_capacity > 0x10000000UL || output_capacity > 0x10000000UL ||
      (mode != ZZ9K_BATCH_MODE_TEST && mode != ZZ9K_BATCH_MODE_EXTRACT) ||
      (mode == ZZ9K_BATCH_MODE_TEST && output_capacity != 0U) ||
      (mode == ZZ9K_BATCH_MODE_EXTRACT && output_capacity == 0U)) {
    return 0;
  }

  layout->mode = mode;
  layout->member_capacity = member_capacity;
  layout->desc_offset = ZZ9K_BATCH_HEADER_SIZE;
  cursor = zz9k_batch_align16(layout->desc_offset +
                              member_capacity * ZZ9K_BATCH_DESC_SIZE);
  layout->blob_offset = cursor;
  layout->blob_capacity = blob_capacity;
  cursor = zz9k_batch_align16(cursor + blob_capacity);
  if (mode == ZZ9K_BATCH_MODE_EXTRACT) {
    layout->output_offset = cursor;
    layout->output_capacity = output_capacity;
    cursor = zz9k_batch_align16(cursor + output_capacity);
  } else {
    layout->output_offset = 0U;
    layout->output_capacity = 0U;
  }
  layout->result_offset = cursor;
  layout->total_size = cursor + member_capacity * ZZ9K_BATCH_RESULT_SIZE;
  return 1;
}

/* Serialize the arena header (ZZ9K_BATCH_HEADER_SIZE bytes, big-endian).
   member_count/blob_length describe the CURRENT chunk and must not exceed
   the layout's capacities. */
static inline void zz9k_batch_write_header(uint8_t *header,
                                           const ZZ9KBatchLayout *layout,
                                           uint32_t member_count,
                                           uint32_t blob_length)
{
  memset(header, 0, ZZ9K_BATCH_HEADER_SIZE);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_MAGIC, ZZ9K_BATCH_ARENA_MAGIC);
  zz9k_put_be16(header + ZZ9K_BATCH_HDR_VERSION, ZZ9K_BATCH_ARENA_VERSION);
  zz9k_put_be16(header + ZZ9K_BATCH_HDR_MODE, (uint16_t)layout->mode);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_MEMBER_COUNT, member_count);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_DESC_OFFSET, layout->desc_offset);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_BLOB_OFFSET, layout->blob_offset);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_BLOB_LENGTH, blob_length);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_OUTPUT_OFFSET, layout->output_offset);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_OUTPUT_CAPACITY,
                layout->output_capacity);
  zz9k_put_be32(header + ZZ9K_BATCH_HDR_RESULT_OFFSET, layout->result_offset);
}

static inline void zz9k_batch_write_desc(uint8_t *desc,
                                         const ZZ9KBatchMemberDesc *m)
{
  memset(desc, 0, ZZ9K_BATCH_DESC_SIZE);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_ALGORITHM, m->algorithm);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_SRC_OFFSET, m->src_offset);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_SRC_LENGTH, m->src_length);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_DST_OFFSET, m->dst_offset);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_UNCOMPRESSED_SIZE,
                m->uncompressed_size);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_EXPECTED_CRC, m->expected_crc);
  zz9k_put_be32(desc + ZZ9K_BATCH_DESC_FLAGS, m->flags);
}

static inline void zz9k_batch_read_desc(const uint8_t *desc,
                                        ZZ9KBatchMemberDesc *m)
{
  m->algorithm = zz9k_get_be32(desc + ZZ9K_BATCH_DESC_ALGORITHM);
  m->src_offset = zz9k_get_be32(desc + ZZ9K_BATCH_DESC_SRC_OFFSET);
  m->src_length = zz9k_get_be32(desc + ZZ9K_BATCH_DESC_SRC_LENGTH);
  m->dst_offset = zz9k_get_be32(desc + ZZ9K_BATCH_DESC_DST_OFFSET);
  m->uncompressed_size =
      zz9k_get_be32(desc + ZZ9K_BATCH_DESC_UNCOMPRESSED_SIZE);
  m->expected_crc = zz9k_get_be32(desc + ZZ9K_BATCH_DESC_EXPECTED_CRC);
  m->flags = zz9k_get_be32(desc + ZZ9K_BATCH_DESC_FLAGS);
}

static inline void zz9k_batch_write_result(uint8_t *res,
                                           const ZZ9KBatchMemberResult *r)
{
  memset(res, 0, ZZ9K_BATCH_RESULT_SIZE);
  zz9k_put_be32(res + ZZ9K_BATCH_RESULT_STATUS, r->status);
  zz9k_put_be32(res + ZZ9K_BATCH_RESULT_BYTES_WRITTEN, r->bytes_written);
  zz9k_put_be32(res + ZZ9K_BATCH_RESULT_CHECKSUM, r->checksum);
}

static inline void zz9k_batch_read_result(const uint8_t *res,
                                          ZZ9KBatchMemberResult *r)
{
  r->status = zz9k_get_be32(res + ZZ9K_BATCH_RESULT_STATUS);
  r->bytes_written = zz9k_get_be32(res + ZZ9K_BATCH_RESULT_BYTES_WRITTEN);
  r->checksum = zz9k_get_be32(res + ZZ9K_BATCH_RESULT_CHECKSUM);
}

/* Parse + validate a header image against the arena length. Mirrors the
   firmware-side validation (this is the executable specification the
   firmware handler must match). Returns ZZ9K_STATUS_OK,
   ZZ9K_STATUS_BAD_REQUEST, or ZZ9K_STATUS_UNSUPPORTED (future version). */
static inline int zz9k_batch_parse_header(const uint8_t *arena,
                                          uint32_t arena_length,
                                          ZZ9KBatchParsedHeader *h)
{
  if (!arena || !h || arena_length < ZZ9K_BATCH_HEADER_SIZE) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (zz9k_get_be32(arena + ZZ9K_BATCH_HDR_MAGIC) != ZZ9K_BATCH_ARENA_MAGIC) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (zz9k_get_be16(arena + ZZ9K_BATCH_HDR_VERSION) !=
      ZZ9K_BATCH_ARENA_VERSION) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  h->mode = zz9k_get_be16(arena + ZZ9K_BATCH_HDR_MODE);
  h->member_count = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_MEMBER_COUNT);
  h->desc_offset = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_DESC_OFFSET);
  h->blob_offset = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_BLOB_OFFSET);
  h->blob_length = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_BLOB_LENGTH);
  h->output_offset = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_OUTPUT_OFFSET);
  h->output_capacity = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_OUTPUT_CAPACITY);
  h->result_offset = zz9k_get_be32(arena + ZZ9K_BATCH_HDR_RESULT_OFFSET);

  if (h->mode != ZZ9K_BATCH_MODE_TEST && h->mode != ZZ9K_BATCH_MODE_EXTRACT) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (h->member_count == 0U || h->member_count > ZZ9K_BATCH_MEMBER_LIMIT) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!zz9k_batch_range_ok(arena_length, h->desc_offset,
                           h->member_count * ZZ9K_BATCH_DESC_SIZE) ||
      !zz9k_batch_range_ok(arena_length, h->blob_offset, h->blob_length) ||
      !zz9k_batch_range_ok(arena_length, h->result_offset,
                           h->member_count * ZZ9K_BATCH_RESULT_SIZE)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (h->mode == ZZ9K_BATCH_MODE_EXTRACT &&
      !zz9k_batch_range_ok(arena_length, h->output_offset,
                           h->output_capacity)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  /* Every arena region must be pairwise disjoint: overlapping regions would
     let, e.g., writing a decode result corrupt the descriptor table or the
     decoded output of another member. Mirrors the firmware's parser. */
  {
    uint32_t desc_len = h->member_count * ZZ9K_BATCH_DESC_SIZE;
    uint32_t result_len = h->member_count * ZZ9K_BATCH_RESULT_SIZE;
    int has_output = (h->mode == ZZ9K_BATCH_MODE_EXTRACT);

    if (zz9k_batch_ranges_overlap(0U, ZZ9K_BATCH_HEADER_SIZE, h->desc_offset,
                                  desc_len) ||
        zz9k_batch_ranges_overlap(0U, ZZ9K_BATCH_HEADER_SIZE, h->blob_offset,
                                  h->blob_length) ||
        zz9k_batch_ranges_overlap(0U, ZZ9K_BATCH_HEADER_SIZE,
                                  h->result_offset, result_len) ||
        zz9k_batch_ranges_overlap(h->desc_offset, desc_len, h->blob_offset,
                                  h->blob_length) ||
        zz9k_batch_ranges_overlap(h->desc_offset, desc_len, h->result_offset,
                                  result_len) ||
        zz9k_batch_ranges_overlap(h->blob_offset, h->blob_length,
                                  h->result_offset, result_len) ||
        (has_output &&
         (zz9k_batch_ranges_overlap(0U, ZZ9K_BATCH_HEADER_SIZE,
                                    h->output_offset, h->output_capacity) ||
          zz9k_batch_ranges_overlap(h->desc_offset, desc_len,
                                    h->output_offset, h->output_capacity) ||
          zz9k_batch_ranges_overlap(h->blob_offset, h->blob_length,
                                    h->output_offset, h->output_capacity) ||
          zz9k_batch_ranges_overlap(h->result_offset, result_len,
                                    h->output_offset, h->output_capacity)))) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
  }
  return ZZ9K_STATUS_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_BATCH_H */
