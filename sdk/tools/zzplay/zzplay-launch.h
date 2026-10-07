/* Workbench and CLI launch handling for zzplay.
 *
 * AmigaOS-only. Under libnix a Workbench launch arrives with argc == 0 and
 * argv pointing at the WBStartup message; there is no console, so failures
 * must reach the user through a requester. All launch paths funnel into
 * the same validated ZZPlayOptions via zzplay-options.c, which is where
 * the host-testable logic lives.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_LAUNCH_H
#define ZZPLAY_LAUNCH_H

#include "zzplay-options.h"

typedef struct ZZPlayController ZZPlayController;

#define ZZPLAY_LAUNCH_PATH_MAX 256U

/* Populated for a Workbench launch so cleanup can undo exactly what was
 * acquired, in reverse order. */
typedef struct ZZPlayLaunch {
  void *startup;        /* struct WBStartup * */
  long old_directory;   /* CurrentDir() result to restore, -1 if unchanged */
  /* Full paths resolved from the Workbench arguments, owned here and
   * freed by zzplay_launch_end(); the options' path pointers borrow this
   * storage for as long as the launch lives. */
  char *paths[ZZPLAY_OPTIONS_MAX_PATHS];
  uint32_t path_count;
} ZZPlayLaunch;

/* Normalize a launch into `options`. `argc`/`argv` come straight from
 * main(). A CLI launch parses every non-option token as a file argument;
 * a Workbench launch applies the tool's and then the first project's
 * ToolTypes and adds every dropped argument as a path. An empty launch is
 * valid: the player opens with an empty playlist. Opens icon.library only
 * as needed. On ZZPLAY_OPTIONS_ERROR the caller reports through
 * zzplay_launch_report() and then cleans up. */
ZZPlayOptionsResult zzplay_launch_begin(int argc, char **argv,
                                        ZZPlayOptions *options,
                                        ZZPlayLaunch *launch);

/* Release everything zzplay_launch_begin() acquired. Safe to call twice and
 * safe on a failed begin. */
void zzplay_launch_end(ZZPlayLaunch *launch);

/* Report a message to whichever surface the user can actually see: stderr
 * for a CLI launch, an Intuition requester for a Workbench launch. */
void zzplay_launch_report(const ZZPlayOptions *options, const char *message);

/* The same, but the player window's message line wins whenever the GUI is
 * open; the requester/stderr routing is the headless fallback. */
void zzplay_launch_report_surface(const ZZPlayOptions *options,
                                  const char *message);
/* Formats once in static storage, updates the controller message then routes
 * it to the visible surface. */
void zzplay_launch_reportf(ZZPlayController *controller,
                           const ZZPlayOptions *options,
                           const char *format, ...);

#endif /* ZZPLAY_LAUNCH_H */
