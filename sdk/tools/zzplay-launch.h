/* Workbench and CLI launch handling for zzplay (R7).
 *
 * AmigaOS-only. Under libnix a Workbench launch arrives with argc == 0 and
 * argv pointing at the WBStartup message; there is no console, so failures
 * must reach the user through a requester. All four launch paths funnel into
 * the same validated ZZPlayOptions via zzplay-options.c, which is where the
 * host-testable logic lives.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_LAUNCH_H
#define ZZPLAY_LAUNCH_H

#include "zzplay-options.h"

/* Populated for a Workbench launch so cleanup can undo exactly what was
 * acquired, in reverse order. */
typedef struct ZZPlayLaunch {
  void *startup;        /* struct WBStartup * */
  void *disk_object;    /* struct DiskObject * from the tool or project icon */
  long old_directory;   /* CurrentDir() result to restore, -1 if unchanged */
  char path[256];       /* resolved file, owned here */
  int have_path;
} ZZPlayLaunch;

/* Normalize a launch into `options`. `argc`/`argv` come straight from main().
 * Opens icon.library/asl.library only as needed. On ZZPLAY_OPTIONS_ERROR the
 * caller reports through zzplay_launch_report() and then cleans up. */
ZZPlayOptionsResult zzplay_launch_begin(int argc, char **argv,
                                        ZZPlayOptions *options,
                                        ZZPlayLaunch *launch);

/* Release everything zzplay_launch_begin() acquired. Safe to call twice and
 * safe on a failed begin. */
void zzplay_launch_end(ZZPlayLaunch *launch);

/* Report a message to whichever surface the user can actually see: stderr
 * for a CLI launch, an Intuition requester for a Workbench launch. */
void zzplay_launch_report(const ZZPlayOptions *options, const char *message);

/* Ask for a file with the ASL requester. Returns 0 if cancelled or if
 * asl.library is unavailable. */
int zzplay_launch_request_file(ZZPlayLaunch *launch);

#endif /* ZZPLAY_LAUNCH_H */
