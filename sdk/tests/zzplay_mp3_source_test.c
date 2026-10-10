/* Standalone MP3 pipeline ordering/source guard.
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

/* The firmware's AUDIO_STREAM_BEGIN rejects a non-zero output_hz or
 * output_channels with UNSUPPORTED: the stream decoder has no rate or
 * channel conversion and always emits the file's native geometry. Asking
 * for the probed rate/channels there aborts standalone MP3 playback before
 * a single sample is decoded, so the request must stay zeroed. */
static int begin_requests_native_geometry(const char *source)
{
  const char *call =
      strstr(source, "zz9k_audio_build_stream_begin_desc(");
  const char *cursor;
  unsigned depth = 0U;

  if (!call) {
    return 0;
  }
  for (cursor = strchr(call, '('); cursor && *cursor; cursor++) {
    if (*cursor == '(') {
      depth++;
      continue;
    }
    if (*cursor == ')') {
      depth--;
      if (depth == 0U) {
        return 1;
      }
      continue;
    }
    if (depth == 1U &&
        (strncmp(cursor, "sample_rate", 11U) == 0 ||
         strncmp(cursor, "->channels", 10U) == 0)) {
      return 0;
    }
  }
  return 0;
}

/* argv[1] is zzplay-mp3.c (MHI bridge and backend policy), argv[2]
 * zzplay-codec.c (the shared accelerated decode + AHI engine). */
int main(int argc, char **argv)
{
  char *source;
  char *codec;
  int native_geometry;
  int ok;

  if (argc != 3 || !(source = read_file(argv[1]))) return 2;
  if (!(codec = read_file(argv[2]))) {
    free(source);
    return 2;
  }
  native_geometry = begin_requests_native_geometry(codec);
  if (!native_geometry) {
    printf("stream begin must request the file's native rate/channels; "
           "the firmware rejects a non-zero output geometry\n");
  }
  /* The function-signature and ordering assertions that used to run here
   * pinned the controls-callback structure the controller-driven rewrite
   * removed, and the ring-size/host-flag text pinned the fixed Zorro II
   * allocation the shrink ladder replaced; per the test policy they were
   * deleted, not re-pinned. */
  ok = native_geometry &&
       strstr(codec, "zz9k_audio_stream_begin(") &&
       strstr(codec, "zz9k_audio_stream_feed(") &&
       strstr(codec, "zz9k_audio_stream_read(") &&
       strstr(codec, "zz9k_audio_stream_close(") &&
       strstr(codec, "zzplay_ahi_begin_drain(") &&
       strstr(source, "ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE") &&
       strstr(source, "AUTO falling back to accelerated decode + AHI") &&
       strstr(source, "direct AX is not a standalone MP3 backend") &&
       strstr(source, "zzplay_mhi_acquire(") &&
       strstr(source, "zzplay_mhi_play_file(") &&
       strstr(source, "MP3 MHI loop") &&
       !strstr(source, "zzplay_mp3_load_public") &&
       !strstr(source, "mhilib.h") && !strstr(source, "mhizz9000.h") &&
       !strstr(codec, "zzplay_mp3_load_public") &&
       !strstr(codec, "mhilib.h") && !strstr(codec, "mhizz9000.h");
  if (!ok) printf("standalone MP3 policy/ownership wiring is incomplete\n");
  free(codec);
  free(source);
  return ok ? 0 : 1;
}
