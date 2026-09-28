/*
 * Source guard for the zz9k-palette CLUT probe.
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
    printf("usage: %s <tools/zz9k-palette.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  /* The tool must gate on the advertised service flag rather than assuming
   * the op exists, so an old-firmware run reports a clear reason. */
  ok &= expect_contains(source, "ZZ9K_SERVICE_FLAG_SURFACE_PALETTE_QUERY");
  ok &= expect_contains(source, "zz9k_query_service(ctx, ZZ9K_SERVICE_SURFACE");
  ok &= expect_contains(source, "zz9k_query_palette(ctx, &desc)");
  /* Bounds come from the ABI, never from a local magic 256. */
  ok &= expect_contains(source, "ZZ9K_PALETTE_MAX_ENTRIES");
  ok &= expect_contains(source, "ZZ9K_PALETTE_ENTRY_BYTES");
  ok &= expect_not_contains(source, "256]");
  /* Poisoning the destination is what distinguishes "firmware wrote an
   * all-black palette" from "firmware wrote nothing at all". */
  ok &= expect_contains(source, "0xa5U");
  ok &= expect_contains(source, "non-zero reserved byte");
  /* Entries are big-endian on the wire; a host-order cast would silently
   * pass on a 68k and fail everywhere else. */
  ok &= expect_contains(source, "(uint32_t)p[0] << 24");
  ok &= expect_not_contains(source, "(uint32_t *)buffer.data");
  /* Every early return must give the shared buffer back. */
  ok &= expect_contains(source, "zz9k_free_shared(ctx, buffer.handle)");

  free(source);
  return ok ? 0 : 1;
}
