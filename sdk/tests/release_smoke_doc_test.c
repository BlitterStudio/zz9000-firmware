/*
 * Source guard for the packaged SDK v2 release smoke checklist.
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
    printf("usage: %s <docs/zz9k-release-smoke.md>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "# ZZ9000 SDK v2 Release Smoke Checklist");
  ok &= expect_contains(source, "zz9k-services --check-release");
  ok &= expect_contains(source, "zz9k-info");
  ok &= expect_contains(source, "zz9k-smoke");
  ok &= expect_contains(source, "zz9k-surface-info");
  ok &= expect_contains(
      source,
      "zz9k-surfaceops --hold-ticks 0 --loops 1000 --stats --stats-interval 100");
  ok &= expect_contains(source, "zz9k-hash --alg sha256");
  ok &= expect_contains(source, "zz9k-chacha");
  ok &= expect_contains(source, "zz9k-aead");
  ok &= expect_contains(source, "zz9k-inflate");
  ok &= expect_contains(source, "zz9k-archive");
  ok &= expect_contains(source, "Archives/split-deflate.7z");
  ok &= expect_contains(source, "Archives/split-lzma.7z");
  ok &= expect_contains(source, "Archives/split-lzma2.7z");
  ok &= expect_contains(source, "zz9k-archive l Archives/split-deflate.7z");
  ok &= expect_contains(source, "zz9k-archive t Archives/split-lzma.7z");
  ok &= expect_contains(
      source,
      "zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-lzma2.7z");
  ok &= expect_contains(source, "zz9k-jpeg");
  ok &= expect_contains(source, "zz9k-png");
  ok &= expect_contains(source, "zz9k-view");
  ok &= expect_contains(source,
                        "zz9k-view Work:Pictures/test.jpg Work:Pictures/test.png");
  ok &= expect_contains(source,
                        "next/previous keys navigate between the images");
  ok &= expect_contains(source,
                        "RGB555 (15-bit), RGB565 (16-bit), and BGRA8888");
  ok &= expect_contains(source,
                        "resize and occlusion redraw through visible clips");
  ok &= expect_contains(source, "zz9k-dtprobe");
  ok &= expect_contains(source, "MultiView");
  ok &= expect_contains(source, "copy Storage/DataTypes/ZZ9000-JPEG#? TO DEVS:DataTypes/");
  ok &= expect_contains(source, "copy Storage/DataTypes/ZZ9000-PNG#? TO DEVS:DataTypes/");
  ok &= expect_contains(source, "AddDataTypes DEVS:DataTypes/ZZ9000-JPEG");
  ok &= expect_contains(source, "AddDataTypes DEVS:DataTypes/ZZ9000-PNG");
  ok &= expect_contains(source, "AddDataTypes LIST");
  ok &= expect_contains(source, "DataType descriptors are activated from `Storage/DataTypes`");
  ok &= expect_contains(source, "zz9k-mp3 --stats");
  ok &= expect_contains(source,
                        "ZZPlay --audio=ahi Work:Audio/test.mp3");
  ok &= expect_contains(source,
                        "ZZPlay --audio=mhi Work:Audio/test.mp3");
  ok &= expect_contains(source, "ZZPlay --audio=none --benchmark");
  ok &= expect_contains(source, "zz9k-mpega-smoke --trace --null-api-check");
  ok &= expect_contains(source, "Expected pass signal");
  ok &= expect_contains(source, "Failure routing");

  free(source);
  return ok ? 0 : 1;
}
