/*
 * Source guard for the completion-derived ahi.device clock.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *source;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0L, SEEK_END) != 0 ||
      (length = ftell(file)) < 0L ||
      fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  source = (char *)malloc((size_t)length + 1U);
  if (!source) {
    fclose(file);
    return 0;
  }
  if (fread(source, 1U, (size_t)length, file) !=
      (size_t)length) {
    free(source);
    fclose(file);
    return 0;
  }
  source[length] = '\0';
  fclose(file);
  return source;
}

static int contains(const char *source, const char *needle)
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
  int ok = 1;

  if (argc != 2) {
    return 2;
  }
  source = read_file(argv[1]);
  if (!source) {
    return 2;
  }
  ok &= contains(source, "CheckIO((struct IORequest *)request)");
  ok &= contains(source, "(void)WaitIO((struct IORequest *)request)");
  ok &= contains(source, "request->ahir_Std.io_Actual /");
  ok &= contains(source, "zzplay_audio_clock_complete(");
  ok &= contains(source, "zzplay_audio_clock_retract_underrun(");
  ok &= contains(source, "sink->end_of_stream = 1U");
  ok &= contains(source, "CMD_STOP");
  ok &= contains(source, "CMD_START");
  ok &= contains(source, "AbortIO((struct IORequest *)buffer->request)");
  ok &= contains(source, "ahir_Link = zzplay_ahi_predecessor");
  free(source);
  return ok ? 0 : 1;
}
