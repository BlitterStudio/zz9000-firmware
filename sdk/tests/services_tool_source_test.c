/*
 * Source guard for zz9k-services service-specific flag names.
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
    printf("usage: %s <tools/zz9k-services.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "zz9k_known_service_flag_count");
  ok &= expect_contains(source, "zz9k_known_service_flag");
  ok &= expect_contains(source, "zz9k_service_flag_name");
  ok &= expect_contains(source, "print_service_flags");
  ok &= expect_contains(source, "usage: zz9k-services [--all|--check-release]");
  ok &= expect_contains(source, "ZZ9K_SERVICES_MODE_CHECK_RELEASE");
  ok &= expect_contains(source, "ReleaseServiceRequirement");
  ok &= expect_contains(source, "check_release_services");
  ok &= expect_contains(source, "zz9k_find_board");
  ok &= expect_contains(source, "board.zorro_version");
  ok &= expect_contains(source, "--check-release");
  ok &= expect_contains(source, "release check ok");
  ok &= expect_contains(source, "release check failed");
  ok &= expect_contains(source, "ZZ9K_CAP_SERVICE_DISCOVERY");
  ok &= expect_contains(source, "ZZ9K_CAP_APERTURE_LAYOUT");
  ok &= expect_contains(source, "ZZ9K_SERVICE_CORE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_MEMORY");
  ok &= expect_contains(source, "ZZ9K_SERVICE_SURFACE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_IMAGE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_CODEC");
  ok &= expect_contains(source, "ZZ9K_SERVICE_AUDIO");
  ok &= expect_contains(source, "ZZ9K_SERVICE_CRYPTO");
  ok &= expect_contains(source, "ZZ9K_SERVICE_DIAG");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_JPEG_PROGRESSIVE");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_JPEG_SCALING");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_IMAGE_RGB888_OUTPUT");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CODEC_LZMA2");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM");
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_CRYPTO_X25519");

  free(source);
  return ok ? 0 : 1;
}
