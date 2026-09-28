/* First-class standalone MP3 playback for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_MP3_H
#define ZZPLAY_MP3_H

#include "zzplay-options.h"
#include "zzplay-probe.h"

typedef int (*ZZPlayMP3StopRequested)(void *user);

/* Standalone MP3 has no video window, so U6 gives it a small status window
 * of its own; this is how that window's keys reach the playback loops.
 * Returning non-zero from `paused` holds playback without ending it. Both
 * callbacks are optional. */
typedef struct ZZPlayMP3Controls {
  ZZPlayMP3StopRequested stop_requested;
  int (*paused)(void *user);
  /* Called when the backend is known, so the window can show it. */
  void (*backend)(void *user, const char *name);
  /* Playback position. Exact on the AHI path (decoded output frames);
   * on MHI it is derived from bytes handed to the decoder and therefore
   * runs slightly ahead, which the UI marks as approximate. */
  void (*progress)(void *user, uint32_t elapsed_ms, int exact);
  void *user;
} ZZPlayMP3Controls;

int zzplay_mp3_run(const char *path,
                   const ZZPlayMP3Info *info,
                   const ZZPlayOptions *options,
                   const ZZPlayMP3Controls *controls);

#endif /* ZZPLAY_MP3_H */
