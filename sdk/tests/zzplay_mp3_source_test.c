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

int main(int argc, char **argv)
{
  char *source;
  char *accelerated;
  char *ahi_claim;
  char *session_begin;
  char *mhi;
  char *mhi_claim;
  char *mhi_play;
  int native_geometry;
  int ok;

  if (argc != 2 || !(source = read_file(argv[1]))) return 2;
  accelerated = strstr(source, "static int zzplay_mp3_accelerated(");
  ahi_claim = accelerated
                  ? strstr(accelerated, "zzplay_ahi_prepare(")
                  : 0;
  session_begin = ahi_claim
                      ? strstr(ahi_claim, "zzplay_mp3_decode_once(")
                      : 0;
  mhi = strstr(source, "static int zzplay_mp3_mhi(");
  mhi_claim = mhi ? strstr(mhi, "zzplay_mhi_acquire(") : 0;
  mhi_play = mhi_claim
                 ? strstr(mhi_claim, "zzplay_mhi_play_file(")
                 : 0;
  native_geometry = begin_requests_native_geometry(source);
  if (!native_geometry) {
    printf("stream begin must request the file's native rate/channels; "
           "the firmware rejects a non-zero output geometry\n");
  }
  ok = native_geometry && accelerated && ahi_claim && session_begin &&
       ahi_claim < session_begin && mhi && mhi_claim && mhi_play &&
       mhi_claim < mhi_play &&
       strstr(source, "zz9k_audio_stream_begin(") &&
       strstr(source, "zz9k_audio_stream_feed(") &&
       strstr(source, "zz9k_audio_stream_read(") &&
       strstr(source, "zz9k_audio_stream_close(") &&
       strstr(source, "zzplay_ahi_begin_drain(") &&
       strstr(source, "ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE") &&
       strstr(source, "ZZPLAY_MP3_Z2_PCM_CAPACITY (32UL * 1024UL)") &&
       strstr(source, "ZZPLAY_MP3_Z2_STAGING_CAPACITY (16UL * 1024UL)") &&
       strstr(source, "ZZ9K_ALLOC_CARD_ONLY, &decode->compressed") &&
       strstr(source, "host_flags = ZZ9K_ALLOC_HOST_WINDOW") &&
       strstr(source, "AUTO falling back to accelerated decode + AHI") &&
       strstr(source, "direct AX is not a standalone MP3 backend") &&
       strstr(source, "mhi_status == ZZPLAY_MHI_STOPPED") &&
       strstr(source, "MP3 MHI loop") &&
       !strstr(source, "zzplay_mp3_load_public") &&
       !strstr(source, "mhilib.h") && !strstr(source, "mhizz9000.h");
  if (!ok) printf("standalone MP3 policy/ownership wiring is incomplete\n");
  free(source);
  return ok ? 0 : 1;
}
