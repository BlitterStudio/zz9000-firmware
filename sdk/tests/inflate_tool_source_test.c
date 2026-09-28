/*
 * Source guard for zz9k-inflate decompression smoke tool.
 *
 * SPDX-License-Identifier: MIT
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
    printf("usage: %s <tools/zz9k-inflate.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"zz9k/compression.h\"");
  ok &= expect_contains(source, "ZZ9K_CAP_COMPRESSION");
  ok &= expect_contains(source, "ZZ9K_SERVICE_CODEC");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_ZLIB");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_GZIP");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_RAW");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_LZMA_ALONE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_LZMA2");
  ok &= expect_contains(source, "zz9k_compression_build_decompress_desc");
  ok &= expect_contains(source, "zz9k_decompress");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_ZLIB");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_GZIP");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_DEFLATE_RAW");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_LZMA_ALONE");
  ok &= expect_contains(source, "ZZ9K_COMPRESSION_LZMA2");
  ok &= expect_contains(source, "0xbfe251bfUL");
  ok &= expect_contains(source, "0x08f2a7d0UL");
  ok &= expect_contains(source, "self-test");
  ok &= expect_contains(source, "--alg raw|zlib|gzip|lzma|lzma2");

  free(source);
  return ok ? 0 : 1;
}
