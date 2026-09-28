/*
 * Source guard for the public typed image decode example.
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

static int expect_not_contains(const char *source, const char *needle)
{
  if (!strstr(source, needle)) {
    return 1;
  }

  printf("unexpected %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <examples/amiga-typed-decode/"
           "zz9k-typed-decode-demo.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"proto/zz9k.h\"");
  ok &= expect_contains(source, "#include \"zz9k/caps.h\"");
  ok &= expect_contains(source, "#include \"zz9k/image.h\"");
  ok &= expect_contains(source, "#include \"zz9k/surface.h\"");
  ok &= expect_contains(source, "#include \"zz9k/text.h\"");
  ok &= expect_contains(source, "zz9k_status_text(status)");
  ok &= expect_contains(source, "zz9k_has_capabilities");
  ok &= expect_contains(source, "zz9k_missing_capabilities");
  ok &= expect_contains(source, "zz9k_known_capability_count");
  ok &= expect_contains(source, "zz9k_capability_name");
  ok &= expect_contains(source, "ZZ9K_LIBRARY_MIN_REVISION_TYPED_IMAGE_DECODE");
  ok &= expect_contains(source, "ZZ9K_CAP_IMAGE_DECODE");
  ok &= expect_contains(source, "ZZ9K_CAP_SHARED_ALLOC");
  ok &= expect_contains(source, "ZZ9K_CAP_SURFACES");
  ok &= expect_contains(source, "ZZ9KDecodeJpeg(&desc, &result)");
  ok &= expect_contains(source, "zz9k_image_build_decode_desc");
  ok &= expect_contains(source, "zz9k_surface_native_rtg_format()");
  ok &= expect_contains(source, "zz9k_surface_layout(image.width, image.height,");
  ok &= expect_contains(source, "expected_output_bytes");
  ok &= expect_contains(source, "result.output_format != output_format");
  ok &= expect_contains(source, "zz9k_surface_format_text(result.output_format)");
  ok &= expect_not_contains(source, "zz9k_surface_bytes_per_pixel(output_format)");
  ok &= expect_not_contains(source, "BGRA8888, %lu bytes on ARM surface");
  ok &= expect_contains(source, "ZZ9KAllocShared");
  ok &= expect_contains(source, "ZZ9KAllocSurface");
  ok &= expect_contains(source, "ZZ9KFreeShared");
  ok &= expect_contains(source, "ZZ9KFreeSurface");
  ok &= expect_not_contains(source, "ZZ9K_SURFACE_FORMAT_BGRA8888");
  ok &= expect_not_contains(source, "ZZ9KDecodeImage");

  free(source);
  return ok ? 0 : 1;
}
