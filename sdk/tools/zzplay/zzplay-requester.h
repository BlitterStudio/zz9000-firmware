/* Asynchronous ASL and About requesters for ZZPlay.
 *
 * The player task polls the result while this module's child process owns the
 * requester. The child only collects paths; playlist and controller changes
 * remain in the player task.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_REQUESTER_H
#define ZZPLAY_REQUESTER_H

#include <stdint.h>

#include "zzplay-controller.h"
#include "zzplay-playlist.h"

struct Screen;

typedef struct ZZPlayRequesterResult {
  ZZPlayModal op;
  int accepted;
  int saved;
  char error[96];
  char drawer[ZZPLAY_PLAYLIST_PATH_MAX];
  char **paths;
  uint32_t path_count;
  /* Fully expanded media entries, prepared by the requester task. */
  ZZPlayPlaylist prepared;
} ZZPlayRequesterResult;

/* Start a requester in a child process. Only one requester may be open.
 * The screen is used as the requester's public screen, never as a sleeping
 * parent window. Returns 0 if it could not be started. */
int zzplay_requester_start(ZZPlayModal op, const char *initial_drawer,
                           const char *version, const ZZPlayPlaylist *playlist,
                           struct Screen *screen);
int zzplay_requester_active(void);
uint32_t zzplay_requester_signal_mask(void);

/* Transfer one completed result to the caller. The caller owns it and must
 * release it with zzplay_requester_result_release(). */
int zzplay_requester_take(ZZPlayRequesterResult *result);
void zzplay_requester_result_release(ZZPlayRequesterResult *result);

/* Wait for an open requester to close before the player task exits. */
void zzplay_requester_close(void);

#endif /* ZZPLAY_REQUESTER_H */
