/*
 * Source guards for zz9k.library async diagnostic tools.
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

static int expect_contains(const char *label, const char *source,
                           const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("%s: missing %s\n", label, needle);
  return 0;
}

static int check_file(const char *path)
{
  char *source;
  int ok;

  source = read_file(path);
  if (!source) {
    printf("failed to read %s\n", path);
    return 0;
  }

  ok = 1;
  ok &= expect_contains(path, source, "#include \"proto/zz9k.h\"");
  ok &= expect_contains(path, source, "#include \"zz9k/sdk.h\"");
  ok &= expect_contains(path, source, "zz9k_request_ping");
  ok &= expect_contains(path, source, "zz9k_status_text(status)");
  ok &= expect_contains(path, source, "zz9k_status_text(");

  free(source);
  return ok;
}

int main(int argc, char **argv)
{
  int ok;
  int i;

  if (argc < 2) {
    printf("usage: %s <tool.c> [tool.c...]\n", argv[0]);
    return 2;
  }

  ok = 1;
  for (i = 1; i < argc; i++) {
    ok &= check_file(argv[i]);
  }

  return ok ? 0 : 1;
}
