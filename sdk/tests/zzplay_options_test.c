/*
 * R7 launch-surface parity: CLI, ToolTypes and Workbench arguments must
 * normalize to identical options.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-options.h"

/* Compare everything a launch path can set, except `launch` itself, which is
 * legitimately different between CLI and Workbench. */
static int same_options(const ZZPlayOptions *a, const ZZPlayOptions *b)
{
  unsigned k;

  if (a->audio_backend != b->audio_backend) return 0;
  if (a->loop_mode != b->loop_mode) return 0;
  if (a->loop_count != b->loop_count) return 0;
  if (a->show_fps != b->show_fps) return 0;
  if (a->uncapped != b->uncapped) return 0;
  if (a->fullscreen != b->fullscreen) return 0;
  if (a->audio_explicit != b->audio_explicit) return 0;
  if (a->quiet != b->quiet) return 0;
  if (a->player != b->player) return 0;
  if (a->ahi_unit != b->ahi_unit) return 0;
  if (a->volume != b->volume) return 0;
  if (!a->mhi_driver != !b->mhi_driver) return 0;
  if (a->mhi_driver && b->mhi_driver &&
      strcmp(a->mhi_driver, b->mhi_driver) != 0)
    return 0;
  if (!a->trace_path != !b->trace_path) return 0;
  if (a->trace_path && b->trace_path &&
      strcmp(a->trace_path, b->trace_path) != 0)
    return 0;
  if (a->path_count != b->path_count) return 0;
  for (k = 0U; k < a->path_count; k++) {
    if (strcmp(a->paths[k], b->paths[k]) != 0) return 0;
  }
  return 1;
}

static int test_cli_basics(void)
{
  ZZPlayOptions options;
  char *argv[] = { "zzplay", "--fps", "--loop=3", "--audio=ahi",
                   "--fullscreen", "movie.mpg" };

  if (zzplay_options_parse_cli(6, argv, &options) != ZZPLAY_OPTIONS_OK)
    return 1;
  if (!options.show_fps || options.uncapped) return 2;
  if (options.loop_mode != ZZPLAY_LOOP_FINITE || options.loop_count != 3U)
    return 3;
  if (options.audio_backend != ZZPLAY_AUDIO_AHI || !options.audio_explicit)
    return 4;
  if (!options.fullscreen) return 5;
  if (options.path_count != 1U || strcmp(options.paths[0], "movie.mpg") != 0)
    return 6;
  if (options.launch != ZZPLAY_LAUNCH_CLI) return 7;
  return 0;
}

static int test_cli_rejects_bad_input(void)
{
  ZZPlayOptions options;
  char *bad_backend[] = { "zzplay", "--audio=spdif", "m.mpg" };
  char *bad_loop[] = { "zzplay", "--loop=0", "m.mpg" };
  char *huge_loop[] = { "zzplay", "--loop=99999999999", "m.mpg" };
  char *bad_ahi[] = { "zzplay", "--ahiunit=4", "m.mpg" };
  char *bad_driver[] = { "zzplay", "--mhidriver=dir/bad", "m.mpg" };
  char *bad_volume[] = { "zzplay", "--volume=150", "m.mpg" };
  char *bad_player[] = { "zzplay", "--player=maybe", "m.mpg" };
  char *typo[] = { "zzplay", "--lop", "m.mpg" };
  char *help[] = { "zzplay", "--help" };

  if (zzplay_options_parse_cli(3, bad_backend, &options) !=
      ZZPLAY_OPTIONS_ERROR)
    return 1;
  if (zzplay_options_parse_cli(3, bad_loop, &options) != ZZPLAY_OPTIONS_ERROR)
    return 2;
  if (zzplay_options_parse_cli(3, huge_loop, &options) !=
      ZZPLAY_OPTIONS_ERROR)
    return 3;
  if (zzplay_options_parse_cli(3, bad_ahi, &options) != ZZPLAY_OPTIONS_ERROR)
    return 4;
  if (zzplay_options_parse_cli(3, bad_driver, &options) != ZZPLAY_OPTIONS_ERROR)
    return 5;
  if (zzplay_options_parse_cli(3, bad_volume, &options) != ZZPLAY_OPTIONS_ERROR)
    return 6;
  if (zzplay_options_parse_cli(3, bad_player, &options) != ZZPLAY_OPTIONS_ERROR)
    return 7;
  if (zzplay_options_parse_cli(3, typo, &options) != ZZPLAY_OPTIONS_ERROR)
    return 8;
  if (zzplay_options_parse_cli(2, help, &options) != ZZPLAY_OPTIONS_HELP)
    return 9;
  return 0;
}

static int test_cli_multi_path_and_empty(void)
{
  ZZPlayOptions options;
  char *no_path[] = { "zzplay", "--fps" };
  char *two_paths[] = { "zzplay", "a.mpg", "b.mpg" };

  /* No path is now valid (opens desktop player with empty playlist) */
  if (zzplay_options_parse_cli(2, no_path, &options) != ZZPLAY_OPTIONS_OK)
    return 1;
  if (options.path_count != 0U) return 2;
  if (!zzplay_options_wants_player(&options)) return 3;

  /* Multiple paths are now valid */
  if (zzplay_options_parse_cli(3, two_paths, &options) != ZZPLAY_OPTIONS_OK)
    return 4;
  if (options.path_count != 2U) return 5;
  if (strcmp(options.paths[0], "a.mpg") != 0 ||
      strcmp(options.paths[1], "b.mpg") != 0)
    return 6;

  return 0;
}

/* The core Stage A guarantee. */
static int test_tooltype_parity_with_cli(void)
{
  ZZPlayOptions cli;
  ZZPlayOptions wb;
  char *argv[] = { "zzplay", "--fps", "--loop=3", "--audio=mhi",
                   "--fullscreen", "--trace=RAM:t", "song.mp3" };
  static const char *tooltypes[] = {
    "FPS", "LOOP=3", "AUDIO=MHI", "FULLSCREEN", "TRACE=RAM:t"
  };
  unsigned i;

  if (zzplay_options_parse_cli(7, argv, &cli) != ZZPLAY_OPTIONS_OK)
    return 1;
  zzplay_options_init(&wb, ZZPLAY_LAUNCH_WORKBENCH);
  for (i = 0U; i < sizeof(tooltypes) / sizeof(tooltypes[0]); i++) {
    if (!zzplay_options_apply_tooltype(&wb, tooltypes[i])) return 2;
  }
  if (!zzplay_options_add_path(&wb, "song.mp3")) return 3;
  if (zzplay_options_finish(&wb) != ZZPLAY_OPTIONS_OK) return 3;
  if (!same_options(&cli, &wb)) return 4;
  if (wb.launch != ZZPLAY_LAUNCH_WORKBENCH) return 5;
  return 0;
}

static int test_tooltype_details(void)
{
  ZZPlayOptions options;

  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  /* Mixed case must work; icon editors do not normalize it. */
  if (!zzplay_options_apply_tooltype(&options, "Audio=Ahi")) return 1;
  if (options.audio_backend != ZZPLAY_AUDIO_AHI) return 2;
  /* A disabled ToolType is inert. */
  if (!zzplay_options_apply_tooltype(&options, "(AUDIO=NONE)")) return 3;
  if (options.audio_backend != ZZPLAY_AUDIO_AHI) return 4;
  /* Foreign ToolTypes are ignored, not errors. */
  if (!zzplay_options_apply_tooltype(&options, "DONOTWAIT")) return 5;
  if (!zzplay_options_apply_tooltype(&options, "")) return 6;
  /* Bare LOOP and "LOOP=" both mean forever. */
  if (!zzplay_options_apply_tooltype(&options, "LOOP")) return 7;
  if (options.loop_mode != ZZPLAY_LOOP_FOREVER) return 8;
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "LOOP=")) return 9;
  if (options.loop_mode != ZZPLAY_LOOP_FOREVER) return 10;
  /* A recognised key with a bad value IS an error. */
  if (zzplay_options_apply_tooltype(&options, "AUDIO=SPDIF")) return 11;
  if (zzplay_options_apply_tooltype(&options, "LOOP=nope")) return 12;
  /* An option that takes no value must reject one. */
  if (zzplay_options_apply_tooltype(&options, "FPS=1")) return 13;
  return 0;
}

/* --benchmark mutes audio, but only when the user named no backend. */
static int test_benchmark_audio_policy(void)
{
  ZZPlayOptions options;
  char *implicit[] = { "zzplay", "--benchmark", "m.mpg" };
  char *explicit_ahi[] = { "zzplay", "--benchmark", "--audio=ahi", "m.mpg" };
  char *reversed[] = { "zzplay", "--audio=ahi", "--benchmark", "m.mpg" };

  if (zzplay_options_parse_cli(3, implicit, &options) != ZZPLAY_OPTIONS_OK)
    return 1;
  if (options.audio_backend != ZZPLAY_AUDIO_NONE) return 2;
  if (!options.show_fps || !options.uncapped) return 3;
  if (zzplay_options_parse_cli(4, explicit_ahi, &options) !=
      ZZPLAY_OPTIONS_OK)
    return 4;
  if (options.audio_backend != ZZPLAY_AUDIO_AHI) return 5;
  /* Order must not matter. */
  if (zzplay_options_parse_cli(4, reversed, &options) != ZZPLAY_OPTIONS_OK)
    return 6;
  if (options.audio_backend != ZZPLAY_AUDIO_AHI) return 7;
  /* An explicit AUTO is still explicit and must survive benchmark. */
  {
    char *explicit_auto[] = { "zzplay", "--benchmark", "--audio=auto",
                              "m.mpg" };

    if (zzplay_options_parse_cli(4, explicit_auto, &options) !=
        ZZPLAY_OPTIONS_OK)
      return 8;
    if (options.audio_backend != ZZPLAY_AUDIO_AUTO) return 9;
  }
  return 0;
}

/* An ASL requester supplies only the path; every other option must already
 * be in place and must not be disturbed. */
static int test_path_supplied_late(void)
{
  ZZPlayOptions options;

  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "AUDIO=AX")) return 1;
  if (!zzplay_options_add_path(&options, "chosen.mpg")) return 2;
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 3;
  if (options.audio_backend != ZZPLAY_AUDIO_AX) return 4;
  if (options.path_count != 1U || strcmp(options.paths[0], "chosen.mpg") != 0)
    return 5;
  return 0;
}

/* A Workbench launch has no console; printing there makes AmigaDOS open an
 * output window that never closes, which is what the first bench round hit. */
static int test_quiet_defaults(void)
{
  ZZPlayOptions options;
  char *cli[] = { "zzplay", "m.mpg" };
  char *quiet[] = { "zzplay", "--quiet", "m.mpg" };

  if (zzplay_options_parse_cli(2, cli, &options) != ZZPLAY_OPTIONS_OK)
    return 1;
  if (options.quiet) return 2;

  if (zzplay_options_parse_cli(3, quiet, &options) != ZZPLAY_OPTIONS_OK)
    return 3;
  if (!options.quiet) return 4;

  /* Workbench defaults to quiet... */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_add_path(&options, "m.mpg")) return 5;
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 5;
  if (!options.quiet) return 6;

  /* ...but VERBOSE overrides it. */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "VERBOSE")) return 7;
  if (!zzplay_options_add_path(&options, "m.mpg")) return 8;
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 8;
  if (options.quiet) return 9;

  /* An explicit QUIET from the shell stays quiet. */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  if (!zzplay_options_apply_tooltype(&options, "QUIET")) return 10;
  if (!zzplay_options_add_path(&options, "m.mpg")) return 11;
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 11;
  if (!options.quiet) return 12;

  /* Benchmark output is the point of benchmarking, so it is not silenced
   * merely because it was started from Workbench. */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "BENCHMARK")) return 13;
  if (!zzplay_options_add_path(&options, "m.mpg")) return 14;
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 14;
  if (options.quiet) return 15;
  return 0;
}

static int test_defaults(void)
{
  ZZPlayOptions options;

  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  if (options.audio_backend != ZZPLAY_AUDIO_AUTO) return 1;
  if (options.loop_mode != ZZPLAY_LOOP_NONE) return 2;
  if (options.show_fps || options.uncapped || options.fullscreen) return 3;
  if (options.audio_explicit) return 4;
  if (options.path_count != 0U) return 5;
  if (options.player != -1 || options.ahi_unit != -1 || options.volume != -1)
    return 8;
  if (options.trace_path) return 7;
  if (options.quiet || options.quiet_explicit) return 6;
  return 0;
}

/* --trace defaults to a RAM-backed path and accepts an explicit one; the
 * ToolType spelling must reach the same option (R7). */
static int test_trace_option(void)
{
  ZZPlayOptions options;
  char *argv[] = { "zzplay", "--trace", "movie.mpg" };
  char *named[] = { "zzplay", "--trace=RAM:zz.trace", "movie.mpg" };
  char *empty[] = { "zzplay", "--trace=", "movie.mpg" };

  if (zzplay_options_parse_cli(3, argv, &options) != ZZPLAY_OPTIONS_OK)
    return 1;
  if (!options.trace_path ||
      strcmp(options.trace_path, "T:zzplay.trace") != 0)
    return 2;
  if (zzplay_options_parse_cli(3, named, &options) != ZZPLAY_OPTIONS_OK)
    return 3;
  if (!options.trace_path ||
      strcmp(options.trace_path, "RAM:zz.trace") != 0)
    return 4;
  if (zzplay_options_parse_cli(3, empty, &options) != ZZPLAY_OPTIONS_OK)
    return 5;
  if (!options.trace_path ||
      strcmp(options.trace_path, "T:zzplay.trace") != 0)
    return 6;
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "Trace=RAM:wb.trace"))
    return 7;
  if (!options.trace_path ||
      strcmp(options.trace_path, "RAM:wb.trace") != 0)
    return 8;
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "TRACE")) return 9;
  if (!options.trace_path ||
      strcmp(options.trace_path, "T:zzplay.trace") != 0)
    return 10;
  return 0;
}
static int test_new_options_and_wants_player(void)
{
  ZZPlayOptions options;
  char *cli[] = { "zzplay", "--player=yes", "--ahiunit=2",
                  "--mhidriver=test.library", "--volume=65", "x.mpg" };

  if (zzplay_options_parse_cli(6, cli, &options) != ZZPLAY_OPTIONS_OK)
    return 1;
  if (options.player != 1) return 2;
  if (options.ahi_unit != 2) return 3;
  if (!options.mhi_driver || strcmp(options.mhi_driver, "test.library") != 0)
    return 4;
  if (options.volume != 65) return 5;
  if (!zzplay_options_wants_player(&options)) return 6;

  /* ToolTypes parity */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "PLAYER=NO")) return 7;
  if (!zzplay_options_apply_tooltype(&options, "AHIUNIT=3")) return 8;
  if (!zzplay_options_apply_tooltype(&options, "MHIDRIVER=foo.library")) return 9;
  if (!zzplay_options_apply_tooltype(&options, "VOLUME=50")) return 10;
  if (options.player != 0 || options.ahi_unit != 3 || options.volume != 50)
    return 11;
  if (zzplay_options_wants_player(&options) != 0) return 12;

  /* Workbench wants player unless fullscreen */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  zzplay_options_add_path(&options, "x.mpg");
  if (!zzplay_options_wants_player(&options)) return 13;
  options.fullscreen = 1;
  if (zzplay_options_wants_player(&options)) return 14;

  /* CLI with files does not want player by default */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  zzplay_options_add_path(&options, "x.mpg");
  if (zzplay_options_wants_player(&options)) return 15;

  return 0;
}

int main(void)
{
  int rc;

  rc = test_defaults();
  if (rc != 0) { printf("defaults %d\n", rc); return 10 + rc; }
  rc = test_cli_basics();
  if (rc != 0) { printf("cli %d\n", rc); return 30 + rc; }
  rc = test_cli_rejects_bad_input();
  if (rc != 0) { printf("cli-bad %d\n", rc); return 50 + rc; }
  rc = test_tooltype_parity_with_cli();
  if (rc != 0) { printf("parity %d\n", rc); return 70 + rc; }
  rc = test_tooltype_details();
  if (rc != 0) { printf("tooltype %d\n", rc); return 90 + rc; }
  rc = test_benchmark_audio_policy();
  if (rc != 0) { printf("benchmark %d\n", rc); return 120 + rc; }
  rc = test_path_supplied_late();
  if (rc != 0) { printf("late-path %d\n", rc); return 150 + rc; }
  rc = test_trace_option();
  if (rc != 0) { printf("trace %d\n", rc); return 180 + rc; }
  rc = test_cli_multi_path_and_empty();
  if (rc != 0) { printf("multi-path %d\n", rc); return 200 + rc; }
  rc = test_new_options_and_wants_player();
  if (rc != 0) { printf("new-options %d\n", rc); return 220 + rc; }
  printf("zzplay_options_test: all checks passed\n");
  return 0;
}
