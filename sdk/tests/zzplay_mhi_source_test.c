/* Public-API/runtime-boundary guard for the optional zzplay MHI adapter.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file = fopen(path, "rb");
  long length;
  char *source;

  if (!file || fseek(file, 0L, SEEK_END) != 0 ||
      (length = ftell(file)) < 0L || fseek(file, 0L, SEEK_SET) != 0) {
    if (file) fclose(file);
    return 0;
  }
  source = (char *)malloc((size_t)length + 1U);
  if (!source || fread(source, 1U, (size_t)length, file) !=
                     (size_t)length) {
    free(source);
    fclose(file);
    return 0;
  }
  source[length] = '\0';
  fclose(file);
  return source;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2 || !(source = read_file(argv[1]))) return 2;
  ok = strstr(source, "#include <libraries/mhi.h>") &&
       strstr(source, "#include <proto/mhi.h>") &&
       strstr(source,
              "#define ZZPLAY_MHI_LIBRARY_PATH \"MHI/mhizz9000.library\"") &&
       strstr(source,
              "OpenLibrary((CONST_STRPTR)ZZPLAY_MHI_LIBRARY_PATH") &&
       !strstr(source,
               "OpenLibrary((CONST_STRPTR)\"mhizz9000.library\"") &&
       strstr(source, "struct Library *MHIBase = 0;") &&
       !strstr(source, "struct Library *MHIBase;") &&
       strstr(source, "MHIQuery(MHIQ_LAYER3)") &&
       strstr(source, "MHIAllocDecoder(") &&
       strstr(source, "MHIQueueBuffer(") &&
       strstr(source, "int eof_announced = 0;") &&
       strstr(source,
              "if (eof && queued == 0U && !eof_announced) {") &&
       strstr(source,
              "MHIQueueBuffer(sink->decoder, sink->buffers[0], 0U)") &&
       strstr(source, "MHIGetEmpty(") &&
       strstr(source, "MHIGetStatus(") &&
       strstr(source, "MHIF_OUT_OF_DATA") &&
       strstr(source, "ZZPLAY_MHI_BUFFER_COUNT") &&
       strstr(source, "ZZPLAY_MHI_BUFFER_BYTES") &&
       strstr(source, "sink->buffers[") &&
       strstr(source, "FreeVec(sink->buffers[") &&
       strstr(source, "input_bytes") &&
       strstr(source, "fread(") &&
       strstr(source, "MHIPlay(") && strstr(source, "MHIPause(") &&
       strstr(source,
              "if (MHIGetStatus(sink->decoder) != MHIF_PAUSED) {\n"
              "    return 0;\n"
              "  }\n"
              "  sink->paused = 1U;") &&
       strstr(source,
              "if (MHIGetStatus(sink->decoder) != MHIF_PLAYING) {\n"
              "    return 0;\n"
              "  }\n"
              "  sink->paused = 0U;") &&
       strstr(source, "MHIStop(") && strstr(source, "MHIFreeDecoder(") &&
       !strstr(source, "mhilib.h") && !strstr(source, "mhizz9000.h");
  if (!ok) {
    printf("zzplay MHI adapter lacks public bounded/drained playback\n");
  }
  free(source);
  return ok ? 0 : 1;
}
