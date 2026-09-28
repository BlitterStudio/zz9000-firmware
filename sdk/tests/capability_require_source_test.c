/*
 * Source guard for command-line capability requirement reporting.
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

static int source_has_capability_name(const char *path)
{
  char *source;
  int ok;

  source = read_file(path);
  if (!source) {
    printf("failed to read %s\n", path);
    return 0;
  }

  ok = strstr(source, "zz9k_capability_name") != 0;
  if (!ok) {
    printf("%s missing zz9k_capability_name\n", path);
  }
  free(source);
  return ok;
}

int main(int argc, char **argv)
{
  int ok;
  int i;

  if (argc < 2) {
    printf("usage: %s <source>...\n", argv[0]);
    return 2;
  }

  ok = 1;
  for (i = 1; i < argc; i++) {
    ok &= source_has_capability_name(argv[i]);
  }

  return ok ? 0 : 1;
}
