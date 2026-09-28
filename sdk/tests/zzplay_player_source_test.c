/*
 * Source guard for opening the P96 PIP window at first presentation.
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

int main(int argc, char **argv)
{
  char *source;
  char *retire;
  char *start_ensure;
  char *present_ensure;
  char *present;
  char *media_probe;
  char *mp3_run;
  char *mpeg_backend_select;
  char *board_probe;
  char *mp3_stop;
  char *mp3_stop_end;
  char *mp3_latched_check;
  char *mp3_raw_break;
  char *mp3_stop_latch;
  int ok;

  if (argc != 2) {
    return 2;
  }
  source = read_file(argv[1]);
  if (!source) {
    return 2;
  }
  retire = strstr(source, "static int zzplay_retire_held_frame(");
  start_ensure =
      retire ? strstr(retire, "zzplay_ensure_pip(runtime)") : 0;
  present_ensure =
      start_ensure
          ? strstr(start_ensure + 1, "zzplay_ensure_pip(runtime)")
          : 0;
  present = present_ensure
                ? strstr(present_ensure, "zz9k_media_session_present(")
                : 0;
  media_probe = strstr(source, "zzplay_probe_media_file(");
  mp3_run = media_probe ? strstr(media_probe, "zzplay_mp3_run(") : 0;
  mpeg_backend_select = mp3_run
                            ? strstr(mp3_run,
                                     "audio_decision = zzplay_audio_select(")
                            : 0;
  board_probe = mpeg_backend_select
                    ? strstr(mpeg_backend_select, "zz9k_find_board(")
                    : 0;
  mp3_stop = strstr(source, "static int zzplay_mp3_stop_requested(");
  mp3_stop_end = mp3_stop ? strstr(mp3_stop, "\n}\n") : 0;
  mp3_latched_check =
      mp3_stop
          ? strstr(mp3_stop, "if (zzplay_ctrl_c_requested != 0)")
          : 0;
  mp3_raw_break =
      mp3_stop ? strstr(mp3_stop,
                        "SetSignal(0L, SIGBREAKF_CTRL_C)")
               : 0;
  mp3_stop_latch =
      mp3_raw_break
          ? strstr(mp3_raw_break, "zzplay_ctrl_c_requested = 1;")
          : 0;
  ok = retire && start_ensure && present_ensure && present &&
       media_probe && mp3_run && mpeg_backend_select && board_probe &&
       mp3_stop && mp3_stop_end && mp3_latched_check &&
       mp3_raw_break && mp3_stop_latch &&
       mp3_latched_check < mp3_raw_break &&
       mp3_stop_latch < mp3_stop_end &&
       media_probe < mp3_run && mp3_run < mpeg_backend_select &&
       mpeg_backend_select < board_probe &&
       strstr(source, "runtime.video_info = info;") &&
       strstr(source, "signal(SIGINT, zzplay_sigint_handler)") &&
       strstr(source, "SetSignal(0L, SIGBREAKF_CTRL_C)") &&
       strstr(source, "IDCMP_VANILLAKEY") &&
       strstr(source, "zzplay_audio_pause(runtime)") &&
       strstr(source, "zzplay_audio_resume(runtime)") &&
       strstr(source, "zzplay_core_pause(&runtime->core)") &&
       strstr(source, "zzplay_core_resume(&runtime->core)") &&
       strstr(source, "zzplay_restart_session(&runtime") &&
       strstr(source, "zzplay_probe_media_file(") &&
       strstr(source, "zzplay_mp3_run(") &&
       strstr(source, "ZZPLAY_MEDIA_KIND_MP3") &&
       strstr(source, "ZZPLAY_Z2_INPUT_BYTES (24U * 1024U)") &&
       strstr(source, "ZZPLAY_Z2_PCM_BYTES (32U * 1024U)") &&
       strstr(source, "zz9k_query_aperture_layout(") &&
       strstr(source, "zzplay_video_z2_aperture_ready(") &&
       strstr(source, "zzplay_resource_release(") &&
       !strstr(source, "zzplay_open_pip(&info");
  if (!ok) {
    printf("P96 first-frame and lifecycle wiring is incomplete\n");
  }
  free(source);
  return ok ? 0 : 1;
}
