/*
 * Unit tests for the zz9k-archive LHA batch-offload driver internals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_ARCHIVE_NO_MAIN 1
#include "../tools/zz9k-archive.c"

#include <stdio.h>
#include <string.h>

static void make_entry(ZZ9KArchiveEntry *entry, uint32_t method,
                       uint32_t compressed, uint32_t uncompressed)
{
  memset(entry, 0, sizeof(*entry));
  strcpy(entry->name, "file.bin");
  entry->method = method;
  entry->compressed_size = compressed;
  entry->uncompressed_size = uncompressed;
  entry->data_offset = 0U;
  entry->crc32 = 0x1234U;
  entry->flags = ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32;
}

static int test_eligibility(void)
{
  ZZ9KArchiveEntry entry;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 200U);
  if (!zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 1;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH0, 100U, 100U);
  if (zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 2;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 200U);
  entry.is_dir = 1U;
  if (zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 3;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 0U, 200U);
  if (zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 4;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 0U);
  if (zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 5;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 200U);
  entry.data_offset = 950U; /* runs past the archive */
  if (zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 6;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 200U);
  strcpy(entry.name, "../evil");
  if (zz9k_archive_lha_batch_member_eligible(&entry, 1000U)) return 7;

  return 0;
}

static int test_plan_chunk_packs_by_budget_and_cap(void)
{
  ZZ9KArchiveEntry entries[5];
  uint32_t members[5] = {0U, 1U, 2U, 3U, 4U};
  ZZ9KLhaBatchChunk chunk;
  uint32_t i;

  for (i = 0U; i < 5U; i++) {
    make_entry(&entries[i], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  }

  /* blob budget fits 2.5 members -> chunk of 2 */
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 5U, 0U,
                                        ZZ9K_BATCH_MODE_TEST, 64U, 250U, 0U,
                                        &chunk) != 2U) {
    return 1;
  }
  if (chunk.first != 0U || chunk.count != 2U || chunk.blob_length != 200U) {
    return 2;
  }

  /* member cap of 3 binds before the budget */
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 5U, 0U,
                                        ZZ9K_BATCH_MODE_TEST, 3U, 10000U, 0U,
                                        &chunk) != 3U) {
    return 3;
  }

  /* extract: output budget fits 2 members */
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 5U, 0U,
                                        ZZ9K_BATCH_MODE_EXTRACT, 64U, 10000U,
                                        700U, &chunk) != 2U) {
    return 4;
  }
  if (chunk.output_length != 600U) return 5;

  /* resume mid-list */
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 5U, 3U,
                                        ZZ9K_BATCH_MODE_TEST, 64U, 10000U, 0U,
                                        &chunk) != 2U) {
    return 6;
  }
  if (chunk.first != 3U) return 7;

  /* an oversize member packs nothing (caller advances past it) */
  entries[0].compressed_size = 999U;
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 5U, 0U,
                                        ZZ9K_BATCH_MODE_TEST, 64U, 500U, 0U,
                                        &chunk) != 0U) {
    return 8;
  }
  return 0;
}

static int test_plan_chunk_test_mode_uncompressed_cap(void)
{
  ZZ9KArchiveEntry entries[4];
  uint32_t members[4] = {0U, 1U, 2U, 3U};
  ZZ9KLhaBatchChunk chunk;
  uint32_t i;

  for (i = 0U; i < 4U; i++) {
    make_entry(&entries[i], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  }

  /* cap fits 2.5 members' uncompressed output -> chunk of 2 */
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 4U, 0U,
                                        ZZ9K_BATCH_MODE_TEST, 64U, 10000U,
                                        750U, &chunk) != 2U) {
    return 1;
  }
  if (chunk.output_length != 600U) return 2;

  /* 0 = unbounded (existing TEST behavior preserved) */
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 4U, 0U,
                                        ZZ9K_BATCH_MODE_TEST, 64U, 10000U,
                                        0U, &chunk) != 4U) {
    return 3;
  }

  /* an oversize member alone packs nothing (caller advances) */
  entries[0].uncompressed_size = 999U;
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 4U, 0U,
                                        ZZ9K_BATCH_MODE_TEST, 64U, 10000U,
                                        750U, &chunk) != 0U) {
    return 4;
  }
  return 0;
}

static int test_write_tables_lays_out_descriptors(void)
{
  ZZ9KArchiveEntry entries[2];
  uint32_t members[2] = {0U, 1U};
  ZZ9KLhaBatchChunk chunk;
  ZZ9KBatchLayout layout;
  ZZ9KBatchMemberDesc desc;
  uint8_t image[ZZ9K_BATCH_HEADER_SIZE + 2U * ZZ9K_BATCH_DESC_SIZE];
  ZZ9KBatchParsedHeader parsed;

  make_entry(&entries[0], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  make_entry(&entries[1], ZZ9K_ARCHIVE_LHA_METHOD_LH7, 50U, 200U);
  entries[1].flags = 0U; /* no stored CRC */

  if (!zz9k_batch_layout_init(&layout, ZZ9K_BATCH_MODE_EXTRACT, 2U, 150U,
                              500U)) {
    return 1;
  }
  if (zz9k_archive_lha_batch_plan_chunk(entries, members, 2U, 0U,
                                        ZZ9K_BATCH_MODE_EXTRACT, 2U, 150U,
                                        500U, &chunk) != 2U) {
    return 2;
  }
  zz9k_archive_lha_batch_write_tables(entries, members, &chunk, &layout,
                                      image);

  if (zz9k_batch_parse_header(image, layout.total_size, &parsed) !=
      ZZ9K_STATUS_OK) {
    return 3;
  }
  if (parsed.member_count != 2U || parsed.blob_length != 150U) return 4;

  zz9k_batch_read_desc(image + ZZ9K_BATCH_HEADER_SIZE, &desc);
  if (desc.algorithm != ZZ9K_COMPRESSION_LH5) return 5;
  if (desc.src_offset != 0U || desc.src_length != 100U) return 6;
  if (desc.dst_offset != 0U || desc.uncompressed_size != 300U) return 7;
  if (desc.expected_crc != 0x1234U ||
      desc.flags != ZZ9K_BATCH_MEMBER_FLAG_HAVE_CRC) {
    return 8;
  }

  zz9k_batch_read_desc(image + ZZ9K_BATCH_HEADER_SIZE + ZZ9K_BATCH_DESC_SIZE,
                       &desc);
  if (desc.algorithm != ZZ9K_COMPRESSION_LH7) return 9;
  if (desc.src_offset != 100U || desc.src_length != 50U) return 10;
  if (desc.dst_offset != 300U || desc.uncompressed_size != 200U) return 11;
  if (desc.flags != 0U) return 12;
  return 0;
}

static int test_judge_maps_results_to_states(void)
{
  ZZ9KArchiveEntry entry;
  ZZ9KBatchMemberResult result;

  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  entry.crc32 = 0xbeefU;
  zz9k_lha_diag_reset();

  result.status = ZZ9K_STATUS_OK;
  result.bytes_written = 300U;
  result.checksum = 0xbeefU;
  if (zz9k_archive_lha_batch_judge(&entry, &result) != ZZ9K_LHA_BATCH_DONE) {
    return 1;
  }
  if (zz9k_lha_diag_batched != 1UL) return 2;

  result.checksum = 0xdeadU; /* CRC miss */
  if (zz9k_archive_lha_batch_judge(&entry, &result) != ZZ9K_LHA_BATCH_SW) {
    return 3;
  }
  if (zz9k_lha_diag_sw_crc_miss != 1UL) return 4;

  result.checksum = 0xbeefU;
  result.bytes_written = 299U; /* size miss */
  if (zz9k_archive_lha_batch_judge(&entry, &result) != ZZ9K_LHA_BATCH_SW) {
    return 5;
  }
  if (zz9k_lha_diag_sw_crc_miss != 2UL) return 6;

  result.bytes_written = 300U;
  result.status = ZZ9K_STATUS_BAD_REQUEST; /* member decode failed */
  if (zz9k_archive_lha_batch_judge(&entry, &result) != ZZ9K_LHA_BATCH_SW) {
    return 7;
  }
  if (zz9k_lha_diag_sw_codec_fail != 1UL) return 8;

  /* no stored CRC: only the size is judged */
  entry.flags = 0U;
  result.status = ZZ9K_STATUS_OK;
  result.checksum = 0x0U;
  if (zz9k_archive_lha_batch_judge(&entry, &result) != ZZ9K_LHA_BATCH_DONE) {
    return 9;
  }
  return 0;
}

static int test_offload_fits_predicate(void)
{
  ZZ9KArchiveEntry entry;
  ZZ9KDiagInfo diag;

  memset(&diag, 0, sizeof(diag));
  diag.shared_heap_free = 10000U;
  diag.shared_heap_largest_free = 6000U;

  /* fits: small member, plenty of heap */
  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 1000U, 2000U);
  if (!zz9k_archive_lha_offload_fits(&diag, &entry)) return 1;

  /* compressed input alone exceeds the largest free block */
  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 7000U, 2000U);
  if (zz9k_archive_lha_offload_fits(&diag, &entry)) return 2;

  /* decoded output alone exceeds the largest free block */
  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 1000U, 7000U);
  if (zz9k_archive_lha_offload_fits(&diag, &entry)) return 3;

  /* both fit the largest block individually, but their sum exceeds the
     total free space (this is the real-world case: two ~3.5MB members
     against a ~2.4MB-free shared heap) */
  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 5000U, 5500U);
  if (zz9k_archive_lha_offload_fits(&diag, &entry)) return 4;

  /* no diag available: try the board as before */
  make_entry(&entry, ZZ9K_ARCHIVE_LHA_METHOD_LH5, 5000U, 5500U);
  if (!zz9k_archive_lha_offload_fits(0, &entry)) return 5;

  return 0;
}

static int test_batch_run_no_service_leaves_all_none(void)
{
  ZZ9KArchiveEntry entries[2];
  uint8_t data[256];
  uint8_t state[2] = {9U, 9U};
  ZZ9KServiceInfo service;
  ZZ9KLhaSource src;

  memset(data, 0, sizeof(data));
  make_entry(&entries[0], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  make_entry(&entries[1], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  memset(&service, 0, sizeof(service));
  zz9k_archive_lha_source_init_mem(&src, data, (uint32_t)sizeof(data));

  /* no context */
  state[0] = state[1] = 9U;
  zz9k_archive_lha_batch_run_src(0, &service, &src, entries, 2U,
                                 "t", 0, state);
  if (state[0] != 9U || state[1] != 9U) return 1; /* untouched */

  /* context but no batch capability advertised */
  service.flags = ZZ9K_SERVICE_FLAG_CODEC_LZH;
  zz9k_archive_lha_batch_run_src((ZZ9KContext *)&service /* non-NULL dummy */,
                                 &service, &src, entries, 2U, "t",
                                 0, state);
  if (state[0] != 9U || state[1] != 9U) return 2;

  /* list command never batches */
  service.flags = ZZ9K_SERVICE_FLAG_CODEC_LZH |
                  ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_BATCH;
  zz9k_archive_lha_batch_run_src((ZZ9KContext *)&service, &service, &src,
                                 entries, 2U, "l", 0, state);
  if (state[0] != 9U || state[1] != 9U) return 3;

  return 0;
}

static int test_extract_excludes_duplicate_paths(void)
{
  ZZ9KArchiveEntry entries[3];
  uint8_t collides[3];
  uint32_t hash0, hash1, hash2;

  /* two entries sharing the same output name hash identically and both
     get marked; a third, uniquely-named entry hashes differently and is
     marked in neither run. */
  make_entry(&entries[0], ZZ9K_ARCHIVE_LHA_METHOD_LH0, 100U, 100U);
  strcpy(entries[0].name, "dup.bin");
  make_entry(&entries[1], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  strcpy(entries[1].name, "dup.bin");
  make_entry(&entries[2], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  strcpy(entries[2].name, "unique.bin");

  hash0 = zz9k_archive_lha_output_name_hash(&entries[0]);
  hash1 = zz9k_archive_lha_output_name_hash(&entries[1]);
  hash2 = zz9k_archive_lha_output_name_hash(&entries[2]);
  if (hash0 == 0U || hash0 != hash1) return 1;
  if (hash2 == 0U || hash2 == hash0) return 2;

  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    return 3;
  }
  if (!collides[0] || !collides[1] || collides[2]) return 4;

  /* case-differing duplicate hashes the same (ASCII fold) and still
     collides */
  strcpy(entries[2].name, "DUP.BIN");
  if (zz9k_archive_lha_output_name_hash(&entries[2]) != hash0) return 5;
  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    return 6;
  }
  if (!collides[0] || !collides[1] || !collides[2]) return 7;

  /* --strip-components: distinct raw names that resolve to the same
     OUTPUT name must collide (a/foo vs b/foo with 1 component stripped);
     distinct post-strip names (b/bar) must not. Restore the global
     afterwards. */
  strcpy(entries[0].name, "a/foo");
  strcpy(entries[1].name, "b/foo");
  strcpy(entries[2].name, "b/bar");
  zz9k_archive_strip_components = 1U;
  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    zz9k_archive_strip_components = 0U;
    return 8;
  }
  if (!collides[0] || !collides[1] || collides[2]) {
    zz9k_archive_strip_components = 0U;
    return 9;
  }
  zz9k_archive_strip_components = 0U;

  /* without stripping, the same raw names (a/foo vs b/foo) never collide */
  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    return 10;
  }
  if (collides[0] || collides[1] || collides[2]) return 11;

  /* Entries that produce no output at all never collide, even with each
     other: over-stripping a single-path-component name yields "" from
     zz9k_archive_strip_entry_name, so zz9k_archive_output_entry fails and
     the hash is 0 (verified against the real transform, not assumed). */
  strcpy(entries[0].name, "top.bin");
  strcpy(entries[1].name, "top.bin"); /* same raw name, also over-stripped */
  strcpy(entries[2].name, "unique.bin");
  zz9k_archive_strip_components = 2U;
  if (zz9k_archive_lha_output_name_hash(&entries[0]) != 0U) {
    zz9k_archive_strip_components = 0U;
    return 12;
  }
  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    zz9k_archive_strip_components = 0U;
    return 13;
  }
  if (collides[0] || collides[1] || collides[2]) {
    zz9k_archive_strip_components = 0U;
    return 14;
  }
  zz9k_archive_strip_components = 0U;

  /* Parent-file/child conflicts must also stay ordered. A batched child
     would create the parent directory before an earlier non-batched file
     parent is processed. */
  make_entry(&entries[0], ZZ9K_ARCHIVE_LHA_METHOD_LH0, 100U, 100U);
  strcpy(entries[0].name, "a");
  make_entry(&entries[1], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  strcpy(entries[1].name, "a/b");
  make_entry(&entries[2], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  strcpy(entries[2].name, "ab/c");
  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    return 15;
  }
  if (!collides[0] || !collides[1] || collides[2]) return 16;

  {
    ZZ9KArchiveEntry prefix_entries[4];
    uint8_t prefix_collides[4];

    make_entry(&prefix_entries[0], ZZ9K_ARCHIVE_LHA_METHOD_LH0, 100U, 100U);
    strcpy(prefix_entries[0].name, "a");
    make_entry(&prefix_entries[1], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
    strcpy(prefix_entries[1].name, "a-irrelevant");
    make_entry(&prefix_entries[2], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
    strcpy(prefix_entries[2].name, "a/b");
    make_entry(&prefix_entries[3], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
    strcpy(prefix_entries[3].name, "z");
    memset(prefix_collides, 0, sizeof(prefix_collides));
    if (!zz9k_archive_lha_batch_mark_collisions(
            prefix_entries, 4U, prefix_collides)) {
      return 19;
    }
    if (!prefix_collides[0] || prefix_collides[1] ||
        !prefix_collides[2] || prefix_collides[3]) {
      return 20;
    }
  }

  /* File-vs-directory conflicts differ only by a trailing slash after
     output-name normalization and must be excluded too. */
  make_entry(&entries[0], ZZ9K_ARCHIVE_LHA_METHOD_LH0, 0U, 0U);
  strcpy(entries[0].name, "dir/");
  entries[0].is_dir = 1U;
  make_entry(&entries[1], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  strcpy(entries[1].name, "dir");
  make_entry(&entries[2], ZZ9K_ARCHIVE_LHA_METHOD_LH5, 100U, 300U);
  strcpy(entries[2].name, "dir2");
  memset(collides, 0, sizeof(collides));
  if (!zz9k_archive_lha_batch_mark_collisions(entries, 3U, collides)) {
    return 17;
  }
  if (!collides[0] || !collides[1] || collides[2]) return 18;

  return 0;
}

int main(void)
{
  int rc;

  if ((rc = test_eligibility()) != 0) return 100 + rc;
  if ((rc = test_plan_chunk_packs_by_budget_and_cap()) != 0) return 200 + rc;
  if ((rc = test_write_tables_lays_out_descriptors()) != 0) return 300 + rc;
  if ((rc = test_judge_maps_results_to_states()) != 0) return 400 + rc;
  if ((rc = test_batch_run_no_service_leaves_all_none()) != 0) return 500 + rc;
  if ((rc = test_plan_chunk_test_mode_uncompressed_cap()) != 0) {
    return 600 + rc;
  }
  if ((rc = test_offload_fits_predicate()) != 0) return 700 + rc;
  if ((rc = test_extract_excludes_duplicate_paths()) != 0) return 800 + rc;
  return 0;
}
