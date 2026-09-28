/*
 * R7 launch-surface parity: CLI, ToolTypes and Workbench arguments must
 * normalize to identical options.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay-options.h"

/* Compare everything a launch path can set, except `launch` itself, which is
 * legitimately different between CLI and Workbench. */
static int same_options(const ZZPlayOptions *a, const ZZPlayOptions *b)
{
  if (a->audio_backend != b->audio_backend) return 0;
  if (a->loop_mode != b->loop_mode) return 0;
  if (a->loop_count != b->loop_count) return 0;
  if (a->show_fps != b->show_fps) return 0;
  if (a->uncapped != b->uncapped) return 0;
  if (a->fullscreen != b->fullscreen) return 0;
  if (a->audio_explicit != b->audio_explicit) return 0;
  if (a->quiet != b->quiet) return 0;
  if (!a->trace_path != !b->trace_path) return 0;
  if (a->trace_path && b->trace_path &&
      strcmp(a->trace_path, b->trace_path) != 0)
    return 0;
  if (!a->path != !b->path) return 0;
  if (a->path && b->path && strcmp(a->path, b->path) != 0) return 0;
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
  if (!options.path || strcmp(options.path, "movie.mpg") != 0) return 6;
  if (options.launch != ZZPLAY_LAUNCH_CLI) return 7;
  return 0;
}

static int test_cli_rejects_bad_input(void)
{
  ZZPlayOptions options;
  char *no_path[] = { "zzplay", "--fps" };
  char *bad_backend[] = { "zzplay", "--audio=spdif", "m.mpg" };
  char *bad_loop[] = { "zzplay", "--loop=0", "m.mpg" };
  char *huge_loop[] = { "zzplay", "--loop=99999999999", "m.mpg" };
  char *two_paths[] = { "zzplay", "a.mpg", "b.mpg" };
  /* A typo must not be silently accepted as a filename. */
  char *typo[] = { "zzplay", "--lop", "m.mpg" };
  char *help[] = { "zzplay", "--help" };

  if (zzplay_options_parse_cli(2, no_path, &options) != ZZPLAY_OPTIONS_ERROR)
    return 1;
  if (zzplay_options_parse_cli(3, bad_backend, &options) !=
      ZZPLAY_OPTIONS_ERROR)
    return 2;
  if (zzplay_options_parse_cli(3, bad_loop, &options) != ZZPLAY_OPTIONS_ERROR)
    return 3;
  if (zzplay_options_parse_cli(3, huge_loop, &options) !=
      ZZPLAY_OPTIONS_ERROR)
    return 4;
  if (zzplay_options_parse_cli(3, two_paths, &options) !=
      ZZPLAY_OPTIONS_ERROR)
    return 5;
  if (zzplay_options_parse_cli(3, typo, &options) != ZZPLAY_OPTIONS_ERROR)
    return 6;
  if (zzplay_options_parse_cli(2, help, &options) != ZZPLAY_OPTIONS_HELP)
    return 7;
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
  wb.path = "song.mp3";
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
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_ERROR) return 2;
  options.path = "chosen.mpg";
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 3;
  if (options.audio_backend != ZZPLAY_AUDIO_AX) return 4;
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
  options.path = "m.mpg";
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 5;
  if (!options.quiet) return 6;

  /* ...but VERBOSE overrides it. */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "VERBOSE")) return 7;
  options.path = "m.mpg";
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 8;
  if (options.quiet) return 9;

  /* An explicit QUIET from the shell stays quiet. */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  if (!zzplay_options_apply_tooltype(&options, "QUIET")) return 10;
  options.path = "m.mpg";
  if (zzplay_options_finish(&options) != ZZPLAY_OPTIONS_OK) return 11;
  if (!options.quiet) return 12;

  /* Benchmark output is the point of benchmarking, so it is not silenced
   * merely because it was started from Workbench. */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_WORKBENCH);
  if (!zzplay_options_apply_tooltype(&options, "BENCHMARK")) return 13;
  options.path = "m.mpg";
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
  if (options.path) return 5;
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
  printf("zzplay_options_test: all checks passed\n");
  return 0;
}
