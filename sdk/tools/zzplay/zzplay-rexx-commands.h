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
/* SEARCH returns no more than this many rows. */
#define ZZPLAY_REXX_RESULT_LINES_MAX 100U
#define ZZPLAY_REXX_COLLECTION_MARKER " (collection)"
/* 100 maximum-size provider ids and titles, markers and line separators. */
#define ZZPLAY_REXX_RESULT_MAX 20501U

typedef struct ZZPlayRexxResultBuilder {
  char *text;
  uint32_t size;
  uint32_t length;
  uint32_t lines;
} ZZPlayRexxResultBuilder;

void zzplay_rexx_result_init(ZZPlayRexxResultBuilder *result, char *text,
                             uint32_t size);
/* Add "<id> <title>", with the documented marker for a collection.
 * Newlines inside either field become spaces. Returns 0 at 100 lines or
 * when the line does not fit. */
int zzplay_rexx_result_add(ZZPlayRexxResultBuilder *result, const char *id,
                           const char *title, int collection);

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
  /* Open or front the service browser. */
  int (*browse)(void *user, char *reason, uint32_t reason_size);
  /* Load and list installed services into `result`. */
  int (*services)(void *user, ZZPlayRexxResultBuilder *result, char *reason,
                  uint32_t reason_size);
  /* Search one installed service. */
  int (*search)(void *user, const char *service, const char *query,
                ZZPlayRexxResultBuilder *result, char *reason,
                uint32_t reason_size);
  /* Gather a collection into an otherwise empty playlist. */
  int (*collection)(void *user, const char *service, const char *id,
                    uint32_t limit, ZZPlayPlaylist *list, char *reason,
                    uint32_t reason_size);
} ZZPlayRexxHost;

typedef struct ZZPlayRexxReply {
  int rc;
  /* The command produced a RESULT string. */
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
