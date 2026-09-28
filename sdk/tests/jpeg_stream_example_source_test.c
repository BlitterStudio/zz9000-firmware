/*
 * Source guard for the public JPEG image-session example.
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
    printf("usage: %s <examples/amiga-jpeg-stream/"
           "zz9k-jpeg-stream-demo.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "#include \"zz9k/caps.h\"");
  ok &= expect_contains(source, "#include \"zz9k/image.h\"");
  ok &= expect_contains(source, "#include \"zz9k/surface.h\"");
  ok &= expect_contains(source, "#include \"zz9k/text.h\"");
  ok &= expect_contains(source, "zz9k_has_capabilities");
  ok &= expect_contains(source, "zz9k_missing_capabilities");
  ok &= expect_contains(source, "zz9k_known_capability_count");
  ok &= expect_contains(source, "zz9k_capability_name");
  ok &= expect_contains(source, "zz9k_has_service_flags");
  ok &= expect_contains(source, "zz9k_missing_service_flags");
  ok &= expect_contains(source, "zz9k_known_service_flag_count");
  ok &= expect_contains(source, "zz9k_service_flag_name");
  ok &= expect_contains(source, "zz9k_image_stream_required_service_flags");
  ok &= expect_contains(source, "stream feed failed: %s (%d)");
  ok &= expect_contains(source, "staging allocation failed: %s (%d)");
  ok &= expect_contains(source, "framebuffer map failed: %s (%d)");
  ok &= expect_contains(source, "tile allocation failed: %s (%d)");
  ok &= expect_contains(source, "image session begin failed: %s (%d)");
  ok &= expect_contains(source, "zz9k_image_build_session_feed_desc");
  ok &= expect_contains(source,
                        "zz9k_image_build_framebuffer_session_begin_desc");
  ok &= expect_contains(source,
                        "zz9k_image_build_tile_session_begin_desc");
  ok &= expect_contains(source, "zz9k_surface_native_rtg_format()");
  ok &= expect_contains(source,
                        "zz9k_surface_is_native_rtg_format(framebuffer.format)");
  ok &= expect_contains(source,
                        "zz9k_surface_min_pitch(width, output_format, &row_bytes)");
  ok &= expect_contains(source,
                        "choose_tile_rows(input->width, output_format,");
  ok &= expect_contains(source, "output_pitch");
  ok &= expect_not_contains(source, "uint32_t bytes_per_pixel");
  ok &= expect_not_contains(source, "ZZ9K_SURFACE_FORMAT_BGRA8888");

  free(source);
  return ok ? 0 : 1;
}
