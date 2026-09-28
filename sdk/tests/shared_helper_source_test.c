/*
 * Source guard for shared-buffer copy helper implementation.
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
  if (!file) return 0;
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
  if (strstr(source, needle)) return 1;
  printf("missing %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok = 1;

  if (argc != 2) {
    printf("usage: %s <include/zz9k/shared.h>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok &= expect_contains(source, "zz9k_shared_ptr_aligned");
  ok &= expect_contains(source, "volatile uint32_t *dst32");
  ok &= expect_contains(source, "volatile const uint32_t *src32");
  ok &= expect_contains(source, "volatile uint16_t *dst16");
  ok &= expect_contains(source, "volatile const uint16_t *src16");

  free(source);
  return ok ? 0 : 1;
}
