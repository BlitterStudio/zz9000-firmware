/* ARexx command language for ZZPlay.
 *
 * Parses one command string and applies it to the controller and the
 * playlist, the same state the player window drives. Nothing here touches
 * AmigaOS: the port (zzplay-rexx.c) receives the messages, and file system
 * work comes in through the host callbacks, so the whole command set is
 * host-testable.
 *
 * Commands are case-insensitive. Arguments are separated by blanks; a
 * double-quoted argument may contain blanks. Playlist positions are
 * 1-based. Times are whole seconds.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_REXX_COMMANDS_H
#define ZZPLAY_REXX_COMMANDS_H

#include <stdint.h>

#include "zzplay-controller.h"
#include "zzplay-playlist.h"

/* ARexx return codes (rexx/errors.h RC_OK, RC_WARN, RC_ERROR). */
#define ZZPLAY_REXX_RC_OK 0
#define ZZPLAY_REXX_RC_WARN 5
#define ZZPLAY_REXX_RC_ERROR 10

/* Longest command string the port accepts, including the NUL. */
#define ZZPLAY_REXX_LINE_MAX 1024U
/* Room for the longest result: a playlist path, or the HELP list. */
#define ZZPLAY_REXX_RESULT_MAX 320U

typedef struct ZZPlayRexxHost {
  ZZPlayController *ctl;
  ZZPlayPlaylist *playlist;
  /* The playing item's own copy of its path; the playlist entry may be
   * removed or replaced while it plays. NULL or "" when unknown. */
  const char *item_path;
  /* "ZZPlay 0.6 (07.10.2026)". */
  const char *version;
  void *user;
  /* Expand a file, drawer, pattern or playlist into `list` (zzplay-files.h).
   * Returns the number of entries added. */
  uint32_t (*add_path)(void *user, ZZPlayPlaylist *list, const char *path);
  /* Write `list` as M3U. Returns 0 on failure. */
  int (*save_playlist)(void *user, const ZZPlayPlaylist *list,
                       const char *path);
  /* Nonzero while ZZPlay cannot quit (a file requester is open). */
  int (*quit_blocked)(void *user);
  /* Bring the player window to the front. Returns 0 without a window. */
  int (*show)(void *user);
} ZZPlayRexxHost;

typedef struct ZZPlayRexxReply {
  int rc;
  /* The command produced a RESULT string (only for rc ZZPLAY_REXX_RC_OK). */
  int has_result;
  char result[ZZPLAY_REXX_RESULT_MAX];
} ZZPlayRexxReply;

/* Nonzero when the command expands paths on the file system (ADD, OPEN).
 * Such a command can block for as long as a drawer scan takes, so the port
 * runs it behind the controller's modal gate, like a Workbench drop. */
int zzplay_rexx_needs_gate(const char *line);

/* Execute one command. `line` is used as scratch space and is modified. */
void zzplay_rexx_execute(const ZZPlayRexxHost *host, char *line,
                         ZZPlayRexxReply *reply);

#endif /* ZZPLAY_REXX_COMMANDS_H */
