/*
 * Source guard for the ARM app echo hardware smoke client.
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
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    if (file) fclose(file);
    return 0;
  }
  data = (char *)malloc((size_t)length + 1U);
  if (!data || fread(data, 1U, (size_t)length, file) != (size_t)length) {
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
  int ok;
  const char *revision_check;
  const char *app_open;

  if (argc != 2) return 2;
  source = read_file(argv[1]);
  if (!source) return 2;
  revision_check = strstr(source,
      "ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_APP_CLIENT");
  app_open = strstr(source, "ZZ9KAppOpen(&app, port)");
  ok = revision_check && app_open && revision_check < app_open &&
       expect_contains(source, "uint32_t count = 300U;") &&
       expect_contains(source, "memcmp(sent.bytes, received.bytes") &&
       expect_contains(source, "sizeof(sent.bytes)") &&
       expect_contains(source, "ZZ9KAppStatus(app, &final_status)") &&
       expect_contains(source, "final_status.state == ZZ9K_APP_STATE_IDLE");
  free(source);
  return ok ? 0 : 1;
}
