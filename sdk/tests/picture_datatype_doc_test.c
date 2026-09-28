/*
 * Source guard for the ZZ9000 picture DataType documentation.
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
    printf("usage: %s <docs/zz9k-picture-datatype.md>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "zz9k-picture.datatype 42.151");
  ok &= expect_contains(source, "PNGdt44-compatible LUT8 path");
  ok &= expect_contains(source, "- `mskHasTransparentColor`");
  ok &= expect_contains(source, "GitHub issue #73");
  ok &= expect_contains(source, "`GM_RENDER` lock-safe");
  ok &= expect_contains(source, "legacy `ENV:ZZ9K_PICTURE_RENDER_MODE`");
  ok &= expect_contains(source, "validated SDK v2 DataType");
  ok &= expect_contains(source, "OS3.1");
  ok &= expect_contains(source, "picture.datatype v39-v42");
  ok &= expect_contains(source, "v43` and `v47`");
  ok &= expect_contains(source, "JPEG and");
  ok &= expect_contains(source, "both use the validated");
  ok &= expect_contains(source, "legacy 8-bit remapped");
  ok &= expect_contains(source, "real transparent PNGs");
  ok &= expect_contains(source, "MultiView and a");
  ok &= expect_contains(source, "browser client");
  ok &= expect_contains(source, "`PBPAFMT_RGBA`");
  ok &= expect_contains(source, "`bmh_Masking = mskHasAlpha`");
  ok &= expect_contains(source, "8-, 15-, and 16-bit screens");
  ok &= expect_contains(source, "32-bit screens retain");
  ok &= expect_contains(source, "per-pixel alpha.");
  ok &= expect_contains(source, "packaged inactive under `Storage/DataTypes`");
  ok &= expect_contains(
      source, "copy Storage/DataTypes/ZZ9000-JPEG#? TO DEVS:DataTypes/");
  ok &= expect_contains(
      source, "copy Storage/DataTypes/ZZ9000-PNG#? TO DEVS:DataTypes/");
  ok &= expect_contains(source, "AddDataTypes DEVS:DataTypes/ZZ9000-JPEG");
  ok &= expect_contains(source, "AddDataTypes DEVS:DataTypes/ZZ9000-PNG");
  ok &= expect_contains(source, "AddDataTypes LIST");

  free(source);
  return ok ? 0 : 1;
}
