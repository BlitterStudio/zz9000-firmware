/*
 * ZZ9000 SDK codec decompression smoke/file tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/compression.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t selftest_plain[] =
  "ZZ9000 SDK codec service self-test payload. raw/zlib/gzip should all "
  "inflate to this exact string.";

static const uint8_t selftest_raw[] = {
  0x0d, 0xc5, 0x3d, 0x12, 0x40, 0x40, 0x0c, 0x06, 0xd0, 0xab, 0x7c, 0x17,
  0xc0, 0xb6, 0x7a, 0x9d, 0x52, 0xa7, 0x8b, 0xdd, 0x20, 0x33, 0x19, 0x6b,
  0x36, 0xf1, 0x7b, 0x7a, 0x5e, 0xf3, 0xc6, 0xb1, 0x0d, 0x21, 0x60, 0xe8,
  0x7a, 0xc4, 0x9c, 0x38, 0xc2, 0xb8, 0x9c, 0x12, 0xf9, 0x5f, 0xe7, 0xca,
  0xd9, 0x1c, 0x3b, 0x3d, 0x9a, 0x29, 0xd5, 0x28, 0x74, 0x35, 0xaf, 0xca,
  0xd4, 0x2c, 0xaf, 0xec, 0xb0, 0x35, 0x1f, 0x9a, 0x40, 0xaa, 0x90, 0x6d,
  0x56, 0x72, 0x86, 0x67, 0xf8, 0x2a, 0x06, 0xbe, 0x29, 0x3a, 0xcc, 0x8b,
  0x6c, 0x4b, 0xfd, 0x01
};

static const uint8_t selftest_zlib[] = {
  0x78, 0xda, 0x0d, 0xc5, 0x3d, 0x12, 0x40, 0x40, 0x0c, 0x06, 0xd0, 0xab,
  0x7c, 0x17, 0xc0, 0xb6, 0x7a, 0x9d, 0x52, 0xa7, 0x8b, 0xdd, 0x20, 0x33,
  0x19, 0x6b, 0x36, 0xf1, 0x7b, 0x7a, 0x5e, 0xf3, 0xc6, 0xb1, 0x0d, 0x21,
  0x60, 0xe8, 0x7a, 0xc4, 0x9c, 0x38, 0xc2, 0xb8, 0x9c, 0x12, 0xf9, 0x5f,
  0xe7, 0xca, 0xd9, 0x1c, 0x3b, 0x3d, 0x9a, 0x29, 0xd5, 0x28, 0x74, 0x35,
  0xaf, 0xca, 0xd4, 0x2c, 0xaf, 0xec, 0xb0, 0x35, 0x1f, 0x9a, 0x40, 0xaa,
  0x90, 0x6d, 0x56, 0x72, 0x86, 0x67, 0xf8, 0x2a, 0x06, 0xbe, 0x29, 0x3a,
  0xcc, 0x8b, 0x6c, 0x4b, 0xfd, 0x01, 0x83, 0x89, 0x22, 0xd1
};

static const uint8_t selftest_gzip[] = {
  0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x0d, 0xc5,
  0x3d, 0x12, 0x40, 0x40, 0x0c, 0x06, 0xd0, 0xab, 0x7c, 0x17, 0xc0, 0xb6,
  0x7a, 0x9d, 0x52, 0xa7, 0x8b, 0xdd, 0x20, 0x33, 0x19, 0x6b, 0x36, 0xf1,
  0x7b, 0x7a, 0x5e, 0xf3, 0xc6, 0xb1, 0x0d, 0x21, 0x60, 0xe8, 0x7a, 0xc4,
  0x9c, 0x38, 0xc2, 0xb8, 0x9c, 0x12, 0xf9, 0x5f, 0xe7, 0xca, 0xd9, 0x1c,
  0x3b, 0x3d, 0x9a, 0x29, 0xd5, 0x28, 0x74, 0x35, 0xaf, 0xca, 0xd4, 0x2c,
  0xaf, 0xec, 0xb0, 0x35, 0x1f, 0x9a, 0x40, 0xaa, 0x90, 0x6d, 0x56, 0x72,
  0x86, 0x67, 0xf8, 0x2a, 0x06, 0xbe, 0x29, 0x3a, 0xcc, 0x8b, 0x6c, 0x4b,
  0xfd, 0x01, 0xbf, 0x51, 0xe2, 0xbf, 0x62, 0x00, 0x00, 0x00
};

static const uint32_t selftest_crc32 = 0xbfe251bfUL;

static const uint8_t selftest_lzma_plain[] =
  "ZZ9000 SDK codec service: repeatable LZMA-alone inflate path "
  "for 7z payloads";

static const uint8_t selftest_lzma[] = {
  0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x00, 0x2d, 0x60, 0xe0, 0x61, 0xa0, 0x20, 0x2b, 0x96, 0xa5, 0x04,
  0xea, 0xea, 0x6c, 0x79, 0xa1, 0x71, 0xc5, 0x2a, 0x91, 0x5a, 0xa9, 0xd5,
  0x57, 0x05, 0xc0, 0x51, 0xec, 0xf7, 0x03, 0x03, 0x05, 0x1b, 0x33, 0x8c,
  0xc9, 0x84, 0x3b, 0x62, 0x0e, 0x29, 0xf7, 0xd2, 0xb6, 0x4f, 0x44, 0x3e,
  0xe5, 0xc7, 0x20, 0x5a, 0xd2, 0xcd, 0x75, 0xe0, 0xa4, 0x1f, 0x05, 0xbb,
  0x04, 0xa0, 0xeb, 0x52, 0x7f, 0xd7, 0x6f, 0x88, 0xed, 0x5e, 0xbf, 0x21,
  0x17, 0x61, 0x29, 0x42, 0x23, 0xff, 0xff, 0x4a, 0x35, 0x40, 0x00
};

static const uint32_t selftest_lzma_crc32 = 0x08f2a7d0UL;

static const uint8_t selftest_lzma2_plain[] =
  "ZZ9000 SDK codec service: repeatable LZMA2 inflate path "
  "for ordinary 7z payloads";

static const uint8_t selftest_lzma2[] = {
  0x10, 0x01, 0x00, 0x4f, 0x5a, 0x5a, 0x39, 0x30, 0x30, 0x30, 0x20, 0x53,
  0x44, 0x4b, 0x20, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x20, 0x73, 0x65, 0x72,
  0x76, 0x69, 0x63, 0x65, 0x3a, 0x20, 0x72, 0x65, 0x70, 0x65, 0x61, 0x74,
  0x61, 0x62, 0x6c, 0x65, 0x20, 0x4c, 0x5a, 0x4d, 0x41, 0x32, 0x20, 0x69,
  0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x70, 0x61, 0x74, 0x68, 0x20,
  0x66, 0x6f, 0x72, 0x20, 0x6f, 0x72, 0x64, 0x69, 0x6e, 0x61, 0x72, 0x79,
  0x20, 0x37, 0x7a, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x73,
  0x00
};

static const uint32_t selftest_lzma2_crc32 = 0x2b0dec5aUL;

static const char *algorithm_name(uint32_t algorithm)
{
  switch (algorithm) {
    case ZZ9K_COMPRESSION_DEFLATE_RAW:
      return "raw";
    case ZZ9K_COMPRESSION_ZLIB:
      return "zlib";
    case ZZ9K_COMPRESSION_GZIP:
      return "gzip";
    case ZZ9K_COMPRESSION_LZMA_ALONE:
      return "lzma";
    case ZZ9K_COMPRESSION_LZMA2:
      return "lzma2";
    default:
      return "unknown";
  }
}

static uint32_t algorithm_required_flag(uint32_t algorithm)
{
  switch (algorithm) {
    case ZZ9K_COMPRESSION_DEFLATE_RAW:
      return ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_RAW;
    case ZZ9K_COMPRESSION_ZLIB:
      return ZZ9K_SERVICE_FLAG_CODEC_ZLIB;
    case ZZ9K_COMPRESSION_GZIP:
      return ZZ9K_SERVICE_FLAG_CODEC_GZIP;
    case ZZ9K_COMPRESSION_LZMA_ALONE:
      return ZZ9K_SERVICE_FLAG_CODEC_LZMA_ALONE;
    case ZZ9K_COMPRESSION_LZMA2:
      return ZZ9K_SERVICE_FLAG_CODEC_LZMA2;
    default:
      return 0U;
  }
}

static int parse_algorithm(const char *name, uint32_t *algorithm)
{
  if (strcmp(name, "raw") == 0) {
    *algorithm = ZZ9K_COMPRESSION_DEFLATE_RAW;
    return 1;
  }
  if (strcmp(name, "zlib") == 0) {
    *algorithm = ZZ9K_COMPRESSION_ZLIB;
    return 1;
  }
  if (strcmp(name, "gzip") == 0) {
    *algorithm = ZZ9K_COMPRESSION_GZIP;
    return 1;
  }
  if (strcmp(name, "lzma") == 0) {
    *algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
    return 1;
  }
  if (strcmp(name, "lzma2") == 0) {
    *algorithm = ZZ9K_COMPRESSION_LZMA2;
    return 1;
  }
  return 0;
}

static void usage(const char *name)
{
  printf("usage: %s [--selftest]\n", name);
  printf("       %s --alg raw|zlib|gzip|lzma|lzma2 --capacity <bytes> "
         "[--out <file>] <input>\n", name);
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
  }
  return 1;
}

static int require_codec_service(ZZ9KContext *ctx, ZZ9KServiceInfo *service)
{
  ZZ9KCaps caps;
  int status;

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_name(status), status);
    return 0;
  }
  if ((caps.capability_bits & ZZ9K_CAP_COMPRESSION) == 0U) {
    printf("%s capability not advertised\n",
           zz9k_capability_name(ZZ9K_CAP_COMPRESSION));
    return 0;
  }

  status = zz9k_query_service(ctx, ZZ9K_SERVICE_CODEC, service);
  if (status != ZZ9K_STATUS_OK) {
    printf("query codec service failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 0;
  }
  return 1;
}

static int service_supports_algorithm(const ZZ9KServiceInfo *service,
                                      uint32_t algorithm)
{
  uint32_t flag = algorithm_required_flag(algorithm);
  return service && flag != 0U && (service->flags & flag) != 0U;
}

static int read_file(const char *path, uint8_t **data, uint32_t *length)
{
  FILE *file;
  long size;
  uint8_t *bytes;

  file = fopen(path, "rb");
  if (!file) {
    printf("open input failed: %s\n", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  size = ftell(file);
  if (size <= 0 || size > 0x7fffffffL) {
    fclose(file);
    printf("unsupported input size\n");
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  bytes = (uint8_t *)malloc((size_t)size);
  if (!bytes) {
    fclose(file);
    printf("input memory allocation failed\n");
    return 0;
  }
  if (fread(bytes, 1U, (size_t)size, file) != (size_t)size) {
    free(bytes);
    fclose(file);
    printf("input read failed\n");
    return 0;
  }
  fclose(file);

  *data = bytes;
  *length = (uint32_t)size;
  return 1;
}

static int write_file(const char *path, const uint8_t *data, uint32_t length)
{
  FILE *file;

  file = fopen(path, "wb");
  if (!file) {
    printf("open output failed: %s\n", path);
    return 0;
  }
  if (fwrite(data, 1U, length, file) != length) {
    fclose(file);
    printf("output write failed\n");
    return 0;
  }
  fclose(file);
  return 1;
}

static int run_decompress(ZZ9KContext *ctx, uint32_t algorithm,
                          const uint8_t *compressed,
                          uint32_t compressed_length,
                          uint8_t *output,
                          uint32_t output_capacity,
                          ZZ9KDecompressResult *result)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer decoded;
  ZZ9KDecompressDesc desc;
  int status;
  int ok = 0;

  memset(&input, 0, sizeof(input));
  memset(&decoded, 0, sizeof(decoded));

  status = zz9k_alloc_shared(ctx, compressed_length, 16U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc input failed: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }
  status = zz9k_alloc_shared(ctx, output_capacity, 16U, 0U, &decoded);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc output failed: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }
  if (!zz9k_shared_copy_to(&input, 0U, compressed, compressed_length)) {
    printf("input copy failed\n");
    goto out;
  }
  if (!zz9k_compression_build_decompress_desc(
          &desc, algorithm, input.handle, 0U, compressed_length,
          decoded.handle, 0U, output_capacity,
          ZZ9K_DECOMPRESS_FLAG_EXPECT_END)) {
    printf("could not build decompression descriptor\n");
    goto out;
  }

  memset(result, 0, sizeof(*result));
  status = zz9k_decompress(ctx, &desc, result);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s decompress failed: %s (%d)\n",
           algorithm_name(algorithm), zz9k_status_name(status), status);
    goto out;
  }
  if (result->bytes_written > output_capacity) {
    printf("invalid decompressed byte count\n");
    goto out;
  }
  if (!zz9k_shared_copy_from(output, &decoded, 0U,
                             result->bytes_written)) {
    printf("output copy failed\n");
    goto out;
  }
  ok = 1;

out:
  if (decoded.handle != 0U && decoded.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, decoded.handle);
  }
  if (input.handle != 0U && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  return ok;
}

static int run_one_selftest(ZZ9KContext *ctx, const ZZ9KServiceInfo *service,
                            uint32_t algorithm,
                            const uint8_t *compressed,
                            uint32_t compressed_length,
                            const uint8_t *plain,
                            uint32_t plain_length,
                            uint32_t expected_crc32)
{
  uint8_t output[128];
  ZZ9KDecompressResult result;

  if (!service_supports_algorithm(service, algorithm)) {
    printf("%s not advertised by codec service\n",
           algorithm_name(algorithm));
    return 0;
  }

  if (plain_length > sizeof(output)) {
    printf("%s self-test output buffer too small\n",
           algorithm_name(algorithm));
    return 0;
  }
  memset(output, 0, sizeof(output));
  if (!run_decompress(ctx, algorithm, compressed, compressed_length,
                      output, (uint32_t)sizeof(output), &result)) {
    return 0;
  }

  if (result.bytes_consumed != compressed_length ||
      result.bytes_written != plain_length ||
      result.algorithm != algorithm ||
      (result.flags & ZZ9K_DECOMPRESS_RESULT_STREAM_END) == 0U ||
      (result.flags & ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U ||
      result.checksum != expected_crc32 ||
      !bytes_equal(output, plain, plain_length)) {
    printf("%s self-test verification failed\n",
           algorithm_name(algorithm));
    return 0;
  }

  printf("%s self-test ok: in=%lu out=%lu crc32=0x%08lx\n",
         algorithm_name(algorithm),
         (unsigned long)result.bytes_consumed,
         (unsigned long)result.bytes_written,
         (unsigned long)result.checksum);
  return 1;
}

static int run_selftest(ZZ9KContext *ctx, const ZZ9KServiceInfo *service)
{
  int ok = 1;

  ok &= run_one_selftest(ctx, service, ZZ9K_COMPRESSION_DEFLATE_RAW,
                         selftest_raw, (uint32_t)sizeof(selftest_raw),
                         selftest_plain,
                         (uint32_t)sizeof(selftest_plain) - 1U,
                         selftest_crc32);
  ok &= run_one_selftest(ctx, service, ZZ9K_COMPRESSION_ZLIB,
                         selftest_zlib, (uint32_t)sizeof(selftest_zlib),
                         selftest_plain,
                         (uint32_t)sizeof(selftest_plain) - 1U,
                         selftest_crc32);
  ok &= run_one_selftest(ctx, service, ZZ9K_COMPRESSION_GZIP,
                         selftest_gzip, (uint32_t)sizeof(selftest_gzip),
                         selftest_plain,
                         (uint32_t)sizeof(selftest_plain) - 1U,
                         selftest_crc32);
  ok &= run_one_selftest(ctx, service, ZZ9K_COMPRESSION_LZMA_ALONE,
                         selftest_lzma, (uint32_t)sizeof(selftest_lzma),
                         selftest_lzma_plain,
                         (uint32_t)sizeof(selftest_lzma_plain) - 1U,
                         selftest_lzma_crc32);
  ok &= run_one_selftest(ctx, service, ZZ9K_COMPRESSION_LZMA2,
                         selftest_lzma2, (uint32_t)sizeof(selftest_lzma2),
                         selftest_lzma2_plain,
                         (uint32_t)sizeof(selftest_lzma2_plain) - 1U,
                         selftest_lzma2_crc32);
  return ok;
}

static int run_file_mode(ZZ9KContext *ctx, const ZZ9KServiceInfo *service,
                         uint32_t algorithm, const char *input_path,
                         const char *output_path, uint32_t capacity)
{
  uint8_t *compressed = 0;
  uint8_t *output = 0;
  uint32_t compressed_length = 0;
  ZZ9KDecompressResult result;
  int ok = 0;

  if (!service_supports_algorithm(service, algorithm)) {
    printf("%s not advertised by codec service\n", algorithm_name(algorithm));
    return 0;
  }
  if (!read_file(input_path, &compressed, &compressed_length)) {
    return 0;
  }
  output = (uint8_t *)malloc((size_t)capacity);
  if (!output) {
    printf("output memory allocation failed\n");
    goto out;
  }

  if (!run_decompress(ctx, algorithm, compressed, compressed_length,
                      output, capacity, &result)) {
    goto out;
  }
  if (output_path && !write_file(output_path, output, result.bytes_written)) {
    goto out;
  }

  printf("%s inflate ok: in=%lu out=%lu crc32=0x%08lx%s\n",
         algorithm_name(algorithm),
         (unsigned long)result.bytes_consumed,
         (unsigned long)result.bytes_written,
         (unsigned long)result.checksum,
         output_path ? " written" : "");
  ok = 1;

out:
  free(output);
  free(compressed);
  return ok;
}

int main(int argc, char **argv)
{
  ZZ9KContext *ctx = 0;
  ZZ9KServiceInfo service;
  uint32_t algorithm = ZZ9K_COMPRESSION_ZLIB;
  uint32_t capacity = 0U;
  const char *input_path = 0;
  const char *output_path = 0;
  int selftest = 0;
  int status;
  int arg;
  int rc = 1;

  if (argc == 1) {
    selftest = 1;
  }

  arg = 1;
  while (arg < argc) {
    if (strcmp(argv[arg], "--selftest") == 0) {
      selftest = 1;
      arg++;
    } else if (strcmp(argv[arg], "--alg") == 0) {
      arg++;
      if (arg >= argc || !parse_algorithm(argv[arg], &algorithm)) {
        usage(argv[0]);
        return 2;
      }
      arg++;
    } else if (strcmp(argv[arg], "--capacity") == 0) {
      char *end = 0;
      unsigned long parsed;

      arg++;
      if (arg >= argc) {
        usage(argv[0]);
        return 2;
      }
      parsed = strtoul(argv[arg], &end, 0);
      if (!end || *end != '\0' || parsed == 0UL ||
          parsed > 0x7fffffffUL) {
        usage(argv[0]);
        return 2;
      }
      capacity = (uint32_t)parsed;
      arg++;
    } else if (strcmp(argv[arg], "--out") == 0) {
      arg++;
      if (arg >= argc) {
        usage(argv[0]);
        return 2;
      }
      output_path = argv[arg++];
    } else {
      if (input_path) {
        usage(argv[0]);
        return 2;
      }
      input_path = argv[arg++];
    }
  }

  if (selftest && (input_path || output_path || capacity != 0U)) {
    usage(argv[0]);
    return 2;
  }
  if (!selftest && (!input_path || capacity == 0U)) {
    usage(argv[0]);
    return 2;
  }

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("open failed: %s (%d)\n", zz9k_status_name(status), status);
    return 10;
  }
  if (!zz9k_shared_heap_board_visible(ctx)) {
    printf("note: this diagnostic stages its buffers in the default shared\n"
           "heap, which is not CPU-visible on Zorro II; its accelerated\n"
           "sections cannot run here (docs/zz9k-zorro2-services.md)\n");
  }

  if (!require_codec_service(ctx, &service)) {
    rc = 3;
    goto out;
  }

  if (selftest) {
    rc = run_selftest(ctx, &service) ? 0 : 1;
  } else {
    rc = run_file_mode(ctx, &service, algorithm, input_path, output_path,
                       capacity) ? 0 : 1;
  }

out:
  zz9k_close(ctx);
  return rc;
}
