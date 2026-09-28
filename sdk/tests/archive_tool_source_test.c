/*
 * Source guard for the zz9k-archive CLI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *data;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0) {
    fclose(file);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = (char *)malloc((size_t)length + 1U);
  if (!data) {
    fclose(file);
    return 0;
  }
  if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return 0;
  }

  data[length] = '\0';
  fclose(file);
  return data;
}

static int expect_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("missing %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <tools/zz9k-archive.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"zz9k/compression.h\"");
  ok &= expect_contains(source, "ZZ9K_SERVICE_CODEC");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_DEFLATE_RAW");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_FEED");
  ok &= expect_contains(source, "zz9k_compression_required_feed_service_flags");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_GZIP");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_LZMA_ALONE");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_LZMA2");
  ok &= expect_contains(source, "zz9k_compression_build_decompress_desc");
  ok &= expect_contains(source, "zz9k_decompress");
  ok &= expect_contains(source, "zz9k_decompress_test");
  ok &= expect_contains(source, "zz9k_archive_decompress_test_to_result");
  ok &= expect_contains(source, "zz9k_decompress_stream_begin");
  ok &= expect_contains(source, "zz9k_decompress_stream_feed");
  ok &= expect_contains(source, "zz9k_decompress_stream_read");
  ok &= expect_contains(source, "zz9k_decompress_stream_close");
  ok &= expect_contains(source,
                        "#define ZZ9K_ARCHIVE_STREAM_HOST_BUDGET (48U * 1024U)");
  ok &= expect_contains(source, ": ZZ9K_ALLOC_CARD_ONLY");
  ok &= expect_contains(source, "zz9k_archive_alloc_stream_pair(ctx, ZZ9K_ALLOC_HOST_WINDOW,");
  ok &= expect_contains(source, "ZZ9K_ALLOC_HOST_WINDOW, input");
  ok &= expect_contains(source, "zz9k_archive_stream_budget");
  ok &= expect_contains(source, "zz9k_archive_stream_budget_chunk");
  ok &= expect_contains(source, "zz9k_archive_alloc_shrink_retry");
  ok &= expect_contains(source, "caps.host_window_heap_size");
  ok &= expect_contains(source, "zz9k_archive_print_window_overflow");
  ok &= expect_contains(source, "exceeds the negotiated Zorro II host window");
  ok &= expect_contains(source, "zz9k_archive_alloc_stream_pair");
  ok &= expect_contains(source, "zz9k_archive_pair_shrink_retry");
  ok &= expect_contains(source,
                        "alloc stream buffer pair failed");
  ok &= expect_contains(source,
                        "alloc file stream buffer pair failed");
  ok &= expect_contains(source, "Host window total");
  ok &= expect_contains(source, "zz9k_archive_decompress_stream_to_file");
  ok &= expect_contains(source, "zz9k_archive_decompress_feed_stream_to_file");
  ok &= expect_contains(source,
                        "zz9k_archive_decompress_feed_stream_parts_to_file");
  ok &= expect_contains(source,
                        "zz9k_archive_decompress_feed_file_parts_to_file");
  ok &= expect_contains(source,
                        "zz9k_archive_decompress_feed_file_parts_to_result");
  ok &= expect_contains(source,
                        "zz9k_archive_decompress_feed_file_to_callback");
  ok &= expect_contains(source, "zz9k_archive_decompress_feed_file_to_file");
  ok &= expect_contains(source, "zz9k_archive_decompress_feed_file_to_result");
  ok &= expect_contains(source, "zz9k_archive_probe_file");
  ok &= expect_contains(source, "zz9k_archive_path_exists");
  ok &= expect_contains(source, "zz9k_archive_path_is_dir");
  ok &= expect_contains(source, "zz9k_archive_overwrite_outputs");
  ok &= expect_contains(source, "zz9k_archive_skip_existing_outputs");
  ok &= expect_contains(source, "zz9k_archive_dry_run_outputs");
  ok &= expect_contains(source, "zz9k_archive_last_output_skipped");
  ok &= expect_contains(source, "zz9k_archive_last_output_dry_run");
  ok &= expect_contains(source, "zz9k_archive_match_filter");
  ok &= expect_contains(source, "zz9k_archive_entry_matches_filter");
  ok &= expect_contains(source, "zz9k_archive_strip_components");
  ok &= expect_contains(source, "zz9k_archive_output_entry");
  ok &= expect_contains(source, "zz9k_archive_trim_trailing_separators");
  ok &= expect_contains(source, "--overwrite");
  ok &= expect_contains(source, "--skip-existing");
  ok &= expect_contains(source, "--dry-run");
  ok &= expect_contains(source, "--match");
  ok &= expect_contains(source, "--strip-components");
  ok &= expect_contains(source, "output exists, use --overwrite");
  ok &= expect_contains(source, "output path is a directory");
  ok &= expect_contains(source, "output path is a file");
  ok &= expect_contains(source, "zz9k_archive_read_file_range");
  ok &= expect_contains(source, "zz9k_archive_write_file_range_entry");
  ok &= expect_contains(source, "decompress-feed");
  ok &= expect_contains(source, "deflate-feed");
  ok &= expect_contains(source, "ZZ9K_DECOMPRESS_RESULT_NEED_INPUT");
  ok &= expect_contains(source, "decompress-stream");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_FORMAT_ZIP");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_FORMAT_TAR");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_FORMAT_GZIP");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_FORMAT_7Z");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_FORMAT_LZMA_ALONE");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_FORMAT_LHA");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_METHOD_LH0");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_METHOD_LH1");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_METHOD_LHD");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_METHOD_LH5");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_METHOD_LH6");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_METHOD_LH7");
  ok &= expect_contains(source, "zz9k_lha_unix_decode_method");
  ok &= expect_contains(source, "zz9k_archive_lha_decode_method_to_file");
  ok &= expect_contains(source, "zz9k_archive_extract_lha_lh5");
  ok &= expect_contains(source, "zz9k_archive_lha_list");
  ok &= expect_contains(source, "zz9k_archive_lha_list_file");
  ok &= expect_contains(source, "zz9k_archive_lha_parse_header");
  ok &= expect_contains(source, "zz9k_archive_handle_lha_file");
  ok &= expect_contains(source, "ZZ9KLhaSource");
  ok &= expect_contains(source, "zz9k_archive_lha_batch_run_src");
  ok &= expect_contains(source, "zz9k_archive_lha_command_loop");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_LHA_PARSE_NEEDS_WINDOW");
  ok &= expect_contains(source, "board.zorro_version == 2U");
  ok &= expect_contains(source, "zz9k_archive_lha_join_dir_name");
  ok &= expect_contains(source, "ext_size");
  ok &= expect_contains(source, "lha method unsupported");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_ZIP_EXTRA_ZIP64");
  ok &= expect_contains(source, "zz9k_archive_zip_parse_zip64_extra");
  ok &= expect_contains(source, "zz9k_archive_zip_read_eocd");
  ok &= expect_contains(source, "zz9k_archive_zip_read_eocd_from_file");
  ok &= expect_contains(source, "zz9k_archive_zip_external_attrs_is_dir");
  ok &= expect_contains(source, "zz9k_archive_strip_current_dir_prefix");
  ok &= expect_contains(source, "zz9k_archive_copy_zip_name");
  ok &= expect_contains(source, "zz9k_archive_name_ends_with_slash");
  ok &= expect_contains(source, "zz9k_archive_zip_entry_is_root_metadata");
  ok &= expect_contains(source, "zz9k_archive_7z_entry_is_root_metadata");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_7Z_METHOD_COPY");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_7Z_METHOD_DEFLATE");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_7Z_METHOD_LZMA");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_7Z_METHOD_LZMA2");
  ok &= expect_contains(source, "zz9k_archive_zip_read_directory_from_file");
  ok &= expect_contains(source, "zz9k_archive_zip_data_offset_checked");
  ok &= expect_contains(source,
                        "zz9k_archive_zip_data_offset_from_file_checked");
  ok &= expect_contains(source, "zz9k_archive_zip_data_offset_from_file");
  ok &= expect_contains(source, "zz9k_archive_zip_list_file");
  ok &= expect_contains(source, "zz9k_archive_zip_file_can_test_entries");
  ok &= expect_contains(source, "zz9k_archive_zip_test_deflate_entry");
  ok &= expect_contains(source, "zz9k_archive_zip_stored_entry_crc_matches");
  ok &= expect_contains(source, "zz9k_archive_zip_result_matches_entry");
  ok &= expect_contains(source, "zip entry crc mismatch");
  ok &= expect_contains(source, "zip deflate feed test failed");
  ok &= expect_contains(source, "zz9k_archive_zip_extract_deflate_entry");
  ok &= expect_contains(source, "zip deflate feed extract failed");
  ok &= expect_contains(source, "zip deflate-feed not advertised; using legacy extract path");
  ok &= expect_contains(source, "zip deflate legacy decode failed");
  ok &= expect_contains(source, "zz9k_archive_handle_zip_file");
  ok &= expect_contains(source, "zz9k_archive_zip_list");
  ok &= expect_contains(source, "zz9k_archive_detect_tar");
  ok &= expect_contains(source, "zz9k_archive_tar_list");
  ok &= expect_contains(source, "ZZ9KArchiveTarStream");
  ok &= expect_contains(source, "ZZ9KArchiveTarPaxInfo");
  ok &= expect_contains(source, "zz9k_archive_tar_stream_prepare_pax");
  ok &= expect_contains(source, "pax_capacity");
  ok &= expect_contains(source, "zz9k_archive_tar_parse_pax_info");
  ok &= expect_contains(source, "zz9k_archive_parse_decimal");
  ok &= expect_contains(source, "zz9k_archive_parse_base256");
  ok &= expect_contains(source, "zz9k_archive_parse_tar_number");
  ok &= expect_contains(source, "zz9k_archive_tar_stream_consume");
  ok &= expect_contains(source, "zz9k_archive_handle_tar_gzip_feed");
  ok &= expect_contains(source, "tar.gz feed failed");
  ok &= expect_contains(source, "zz9k_archive_gzip_info");
  ok &= expect_contains(source, "zz9k_archive_gzip_info_from_file");
  ok &= expect_contains(source, "zz9k_archive_gzip_result_matches_footer");
  ok &= expect_contains(source, "flags & 0x02U");
  ok &= expect_contains(source, "gzip crc mismatch");
  ok &= expect_contains(source, "zz9k_archive_handle_gzip_file");
  ok &= expect_contains(source, "gzip-feed");
  ok &= expect_contains(source, "gzip feed test failed");
  ok &= expect_contains(source, "gzip feed extract failed");
  ok &= expect_contains(source, "zz9k_archive_lzma_info");
  ok &= expect_contains(source, "zz9k_archive_lzma_info_from_header");
  ok &= expect_contains(source, "zz9k_archive_lzma_output_capacity");
  ok &= expect_contains(source, "zz9k_archive_7z_read_number");
  ok &= expect_contains(source, "zz9k_archive_7z_skip_digests");
  ok &= expect_contains(source, "zz9k_archive_7z_start_header_from_prefix");
  ok &= expect_contains(source, "zz9k_archive_7z_start_header");
  ok &= expect_contains(source, "zz9k_archive_7z_copy_encoded_header");
  ok &= expect_contains(source, "zz9k_archive_7z_decode_encoded_header_from_file");
  ok &= expect_contains(source, "7z encoded header LZMA decode failed");
  ok &= expect_contains(source, "7z encoded header crc mismatch");
  ok &= expect_contains(source, "zz9k_archive_7z_files_info_property_supported");
  ok &= expect_contains(source, "7z header crc mismatch");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_ENTRY_FLAG_CRC32");
  ok &= expect_contains(source, "ZZ9K_ARCHIVE_ENTRY_FLAG_7Z_UNSUPPORTED_SPLIT");
  ok &= expect_contains(source, "decoded_offset");
  ok &= expect_contains(source, "zz9k_archive_entry_has_crc32");
  ok &= expect_contains(source, "zz9k_archive_7z_entry_has_unsupported_split");
  ok &= expect_contains(source, "zz9k_archive_7z_entry_has_split_substream");
  ok &= expect_contains(source, "zz9k_archive_7z_read_digests");
  ok &= expect_contains(source, "zz9k_archive_7z_streams_flatten_substreams");
  ok &= expect_contains(source, "zz9k_archive_7z_split_group_count");
  ok &= expect_contains(source, "ZZ9KArchive7zSplitWriter");
  ok &= expect_contains(source, "zz9k_archive_7z_split_writer_chunk");
  ok &= expect_contains(source, "zz9k_archive_handle_7z_split_group_file");
  ok &= expect_contains(source, "7z split group not contiguous");
  ok &= expect_contains(source, "zz9k_archive_7z_copy_file_crc_matches");
  ok &= expect_contains(source, "zz9k_archive_7z_result_matches_entry");
  ok &= expect_contains(source, "7z entry crc mismatch");
  ok &= expect_contains(source, "7z LZMA crc mismatch");
  ok &= expect_contains(source, "zz9k_archive_7z_read_header_from_file");
  ok &= expect_contains(source, "zz9k_archive_7z_list_from_header");
  ok &= expect_contains(source, "zz9k_archive_handle_7z_file");
  ok &= expect_contains(source, "zz9k_archive_handle_7z_feed_file");
  ok &= expect_contains(source, "zz9k_archive_7z_parse_streams_info");
  ok &= expect_contains(source, "zz9k_archive_7z_parse_substreams_info");
  ok &= expect_contains(source, "zz9k_archive_7z_parse_diagnostic");
  ok &= expect_contains(source,
                        "7z multi-coder/filter-chain folders unsupported");
  ok &= expect_contains(source,
                        "7z multiple input/output streams unsupported");
  ok &= expect_contains(source, "7z folder method unsupported");
  ok &= expect_contains(source, "7z unsupported layout: %s");
  ok &= expect_contains(source, "zz9k_archive_7z_lzma_alone_header");
  ok &= expect_contains(source, "zz9k_archive_7z_build_lzma_alone_payload");
  ok &= expect_contains(source, "zz9k_archive_7z_build_lzma2_payload");
  ok &= expect_contains(source, "zz9k_archive_7z_lzma2_fallback_file_range");
  ok &= expect_contains(source, "zz9k_archive_7z_lzma2_feed_output_limit");
  ok &= expect_contains(source, "zz9k_archive_lzma_props_dict_size");
  ok &= expect_contains(source, "zz9k_archive_lzma2_prop_dict_size");
  ok &= expect_contains(source, "zz9k_archive_print_shared_diag");
  ok &= expect_contains(source, "zz9k_archive_7z_list");
  ok &= expect_contains(source, "zz9k_archive_path_is_safe");
  ok &= expect_contains(source, "usage: %s l|t|x");
  ok &= expect_contains(source, "--capacity bytes");
  ok &= expect_contains(source, "lzma-alone");
  ok &= expect_contains(source, "7z LZMA");
  ok &= expect_contains(source, "7z Deflate");
  ok &= expect_contains(source, "7z LZMA2");
  ok &= expect_contains(source, "LZMA2 dictionary");
  ok &= expect_contains(source, "7z LZMA diagnostics");
  ok &= expect_contains(source, "streamed test path");
  ok &= expect_contains(source, "Largest free block");
  ok &= expect_contains(source, "unsupported encoded 7z header");
  ok &= expect_contains(source, "7z non-Copy multi-substream unsupported");
  ok &= expect_contains(source, "7z compressed multi-substream");
  ok &= expect_contains(source, "7z list ok");

  free(source);
  return ok ? 0 : 1;
}
