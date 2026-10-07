/* Command-line, ToolType and Workbench options for zzplay.
 *
 * R7 requires CLI arguments, Workbench project arguments, ToolTypes and an
 * ASL requester to produce the same validated options. They therefore share
 * one applier (`zzplay_options_apply`); the CLI and ToolType front ends are
 * thin adapters over it, so parity is structural rather than something two
 * parsers have to agree on by hand.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_OPTIONS_H
#define ZZPLAY_OPTIONS_H

#include "zzplay-audio.h"

typedef enum ZZPlayOptionsResult {
  ZZPLAY_OPTIONS_OK = 0,
  ZZPLAY_OPTIONS_HELP,
  ZZPLAY_OPTIONS_ERROR
} ZZPlayOptionsResult;

typedef enum ZZPlayLoopMode {
  ZZPLAY_LOOP_NONE = 0,
  ZZPLAY_LOOP_FINITE,
  ZZPLAY_LOOP_FOREVER
} ZZPlayLoopMode;

/* How the player was started. Workbench has no console, so failures there
 * must go to a requester instead of stderr (R7). */
typedef enum ZZPlayLaunchSource {
  ZZPLAY_LAUNCH_CLI = 0,
  ZZPLAY_LAUNCH_WORKBENCH
} ZZPlayLaunchSource;

/* One option, independent of the syntax it arrived in. */
typedef enum ZZPlayOptionKey {
  ZZPLAY_OPT_NONE = 0,
  ZZPLAY_OPT_FPS,
  ZZPLAY_OPT_BENCHMARK,
  ZZPLAY_OPT_LOOP,
  ZZPLAY_OPT_AUDIO,
  ZZPLAY_OPT_FULLSCREEN,
  ZZPLAY_OPT_QUIET,
  ZZPLAY_OPT_VERBOSE,
  ZZPLAY_OPT_TRACE,
  ZZPLAY_OPT_HELP,
  /* PLAYER (stay open as a desktop player) / PLAYER=NO. */
  ZZPLAY_OPT_PLAYER,
  /* AHIUNIT=0..3: ahi.device unit for this launch. */
  ZZPLAY_OPT_AHI_UNIT,
  /* MHIDRIVER=name: driver file in LIBS:MHI/ for this launch. */
  ZZPLAY_OPT_MHI_DRIVER,
  /* VOLUME=0..100: starting volume for this launch. */
  ZZPLAY_OPT_VOLUME
} ZZPlayOptionKey;

/* Files named on the command line or selected in Workbench. More than this
 * is an error that suggests a pattern or a playlist instead. */
#define ZZPLAY_OPTIONS_MAX_PATHS 64U

typedef struct ZZPlayOptions {
  /* Media files, patterns, drawers or playlists, in the order given. The
   * pointers are borrowed (argv or ZZPlayLaunch storage). */
  const char *paths[ZZPLAY_OPTIONS_MAX_PATHS];
  uint32_t path_count;
  /* -1 automatic (see zzplay_options_wants_player), 0 off, 1 on. */
  int player;
  /* -1 when not given for this launch; the saved settings apply. */
  int32_t ahi_unit;
  int32_t volume;
  const char *mhi_driver;    /* NULL when not given */
  ZZPlayAudioBackend audio_backend;
  uint32_t loop_count;
  ZZPlayLoopMode loop_mode;
  int show_fps;
  int uncapped;
  int fullscreen;
  /* Suppress informational output. Defaults on for a Workbench launch,
   * which has no console: printing there makes AmigaDOS open an output
   * window that never closes. */
  int quiet;
  int quiet_explicit;
  /* Set when the user named a backend. Only an unspecified backend may be
   * silently replaced by --benchmark, and only AUTO may fall back (R4). */
  int audio_explicit;
  /* Per-frame playback trace destination (diagnostics); NULL when off. */
  const char *trace_path;
  ZZPlayLaunchSource launch;
} ZZPlayOptions;

/* Reset to documented defaults. Always call before applying anything. */
void zzplay_options_init(ZZPlayOptions *options, ZZPlayLaunchSource launch);

/* Apply one option. `value` is the text after '=', or NULL when the option
 * takes none. Returns 0 on a malformed or unexpected value. */
int zzplay_options_apply(ZZPlayOptions *options, ZZPlayOptionKey key,
                         const char *value);

/* Map one CLI token ("--loop=3") to a key plus its value. Returns
 * ZZPLAY_OPT_NONE for anything that is not an option token. */
ZZPlayOptionKey zzplay_options_key_from_cli(const char *token,
                                            const char **value);

/* Map one Workbench ToolType ("LOOP=3", case-insensitive) the same way. */
ZZPlayOptionKey zzplay_options_key_from_tooltype(const char *tooltype,
                                                 const char **value);

/* Apply one ToolType string; ignores empty and commented-out entries the way
 * Workbench does. Returns 0 only for a recognised key with a bad value. */
int zzplay_options_apply_tooltype(ZZPlayOptions *options,
                                  const char *tooltype);

/* Append one file argument. Returns 0 when the list is full. */
int zzplay_options_add_path(ZZPlayOptions *options, const char *path);

/* Final cross-option validation, shared by every launch path. A launch
 * without files is valid: it opens the player with an empty playlist. */
ZZPlayOptionsResult zzplay_options_finish(ZZPlayOptions *options);

/* Whether to run as a desktop player that stays open after the playlist
 * ends. PLAYER / PLAYER=NO decide when given. Otherwise: always when no file
 * was named (there is nothing else to do), and for a Workbench launch
 * unless FULLSCREEN asks for a chrome-free video sequence. A shell launch
 * with files plays them and exits, as it always has. */
int zzplay_options_wants_player(const ZZPlayOptions *options);

/* CLI: options and file arguments may be interleaved; every non-option
 * token is a file argument. */
ZZPlayOptionsResult zzplay_options_parse_cli(
    int argc, char **argv, ZZPlayOptions *options);

#endif /* ZZPLAY_OPTIONS_H */
