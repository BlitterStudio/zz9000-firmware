/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-options.h"
#include "zzplay-core.h"
#include "zzplay-prefs.h"

#include <stdint.h>
#include <string.h>

static char zzplay_lower(char c)
{
  if (c >= 'A' && c <= 'Z') {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

static int zzplay_parse_uint(const char *value, uint32_t *out, uint32_t max_val)
{
  uint32_t parsed = 0U;

  if (!value || !*value || !out) {
    return 0;
  }
  while (*value) {
    uint32_t digit;

    if (*value < '0' || *value > '9') {
      return 0;
    }
    digit = (uint32_t)(*value - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) {
      return 0;
    }
    parsed = parsed * 10U + digit;
    value++;
  }
  if (parsed > max_val) {
    return 0;
  }
  *out = parsed;
  return 1;
}

static int zzplay_parse_loop_count(const char *value, uint32_t *count)
{
  uint32_t parsed = 0U;

  if (!zzplay_parse_uint(value, &parsed, UINT32_MAX) || parsed == 0U) {
    return 0;
  }
  *count = parsed;
  return 1;
}

static int zzplay_parse_audio_backend(const char *value,
                                      ZZPlayAudioBackend *backend)
{
  if (!value || !backend) {
    return 0;
  }
  if (zzplay_ascii_equal_fold(value, "auto")) {
    *backend = ZZPLAY_AUDIO_AUTO;
  } else if (zzplay_ascii_equal_fold(value, "ahi")) {
    *backend = ZZPLAY_AUDIO_AHI;
  } else if (zzplay_ascii_equal_fold(value, "mhi")) {
    *backend = ZZPLAY_AUDIO_MHI;
  } else if (zzplay_ascii_equal_fold(value, "ax")) {
    *backend = ZZPLAY_AUDIO_AX;
  } else if (zzplay_ascii_equal_fold(value, "none")) {
    *backend = ZZPLAY_AUDIO_NONE;
  } else {
    return 0;
  }
  return 1;
}

void zzplay_options_init(ZZPlayOptions *options, ZZPlayLaunchSource launch)
{
  if (!options) {
    return;
  }
  memset(options, 0, sizeof(*options));
  options->player = -1;
  options->ahi_unit = -1;
  options->volume = -1;
  options->audio_backend = ZZPLAY_AUDIO_AUTO;
  options->launch = launch;
}

int zzplay_options_apply(ZZPlayOptions *options, ZZPlayOptionKey key,
                         const char *value)
{
  if (!options) {
    return 0;
  }
  switch (key) {
  case ZZPLAY_OPT_FPS:
    if (value) {
      return 0;
    }
    options->show_fps = 1;
    return 1;
  case ZZPLAY_OPT_BENCHMARK:
    if (value) {
      return 0;
    }
    options->show_fps = 1;
    options->uncapped = 1;
    return 1;
  case ZZPLAY_OPT_FULLSCREEN:
    if (value) {
      return 0;
    }
    options->fullscreen = 1;
    return 1;
  case ZZPLAY_OPT_QUIET:
    if (value) {
      return 0;
    }
    options->quiet = 1;
    options->quiet_explicit = 1;
    return 1;
  case ZZPLAY_OPT_VERBOSE:
    if (value) {
      return 0;
    }
    options->quiet = 0;
    options->quiet_explicit = 1;
    return 1;
  case ZZPLAY_OPT_TRACE:
    if (!value || !*value) {
      options->trace_path = "T:zzplay.trace";
      return 1;
    }
    options->trace_path = value;
    return 1;
  case ZZPLAY_OPT_LOOP:
    if (!value) {
      options->loop_mode = ZZPLAY_LOOP_FOREVER;
      options->loop_count = 0U;
      return 1;
    }
    if (!zzplay_parse_loop_count(value, &options->loop_count)) {
      return 0;
    }
    options->loop_mode = ZZPLAY_LOOP_FINITE;
    return 1;
  case ZZPLAY_OPT_AUDIO:
    if (!zzplay_parse_audio_backend(value, &options->audio_backend)) {
      return 0;
    }
    options->audio_explicit = 1;
    return 1;
  case ZZPLAY_OPT_PLAYER:
    if (!value) {
      options->player = 1;
      return 1;
    }
    if (zzplay_ascii_equal_fold(value, "yes") ||
        zzplay_ascii_equal_fold(value, "1") ||
        zzplay_ascii_equal_fold(value, "true")) {
      options->player = 1;
      return 1;
    }
    if (zzplay_ascii_equal_fold(value, "no") ||
        zzplay_ascii_equal_fold(value, "0") ||
        zzplay_ascii_equal_fold(value, "false")) {
      options->player = 0;
      return 1;
    }
    return 0;
  case ZZPLAY_OPT_AHI_UNIT:
    {
      uint32_t unit = 0U;
      if (!zzplay_parse_uint(value, &unit, 3U)) {
        return 0;
      }
      options->ahi_unit = (int32_t)unit;
      return 1;
    }
  case ZZPLAY_OPT_MHI_DRIVER:
    if (!zzplay_prefs_valid_driver(value)) {
      return 0;
    }
    options->mhi_driver = value;
    return 1;
  case ZZPLAY_OPT_VOLUME:
    {
      uint32_t vol = 0U;
      if (!zzplay_parse_uint(value, &vol, 100U)) {
        return 0;
      }
      options->volume = (int32_t)vol;
      return 1;
    }
  case ZZPLAY_OPT_HELP:
  case ZZPLAY_OPT_NONE:
  default:
    return 0;
  }
}

ZZPlayOptionKey zzplay_options_key_from_cli(const char *token,
                                            const char **value)
{
  if (value) {
    *value = 0;
  }
  if (!token || token[0] != '-' || token[1] != '-') {
    return ZZPLAY_OPT_NONE;
  }
  token += 2;
  if (strcmp(token, "fps") == 0) {
    return ZZPLAY_OPT_FPS;
  }
  if (strcmp(token, "benchmark") == 0) {
    return ZZPLAY_OPT_BENCHMARK;
  }
  if (strcmp(token, "fullscreen") == 0) {
    return ZZPLAY_OPT_FULLSCREEN;
  }
  if (strcmp(token, "quiet") == 0) {
    return ZZPLAY_OPT_QUIET;
  }
  if (strcmp(token, "verbose") == 0) {
    return ZZPLAY_OPT_VERBOSE;
  }
  if (strcmp(token, "help") == 0) {
    return ZZPLAY_OPT_HELP;
  }
  if (strcmp(token, "trace") == 0) {
    return ZZPLAY_OPT_TRACE;
  }
  if (strncmp(token, "trace=", 6U) == 0) {
    if (value) {
      *value = token + 6;
    }
    return ZZPLAY_OPT_TRACE;
  }
  if (strcmp(token, "loop") == 0) {
    return ZZPLAY_OPT_LOOP;
  }
  if (strncmp(token, "loop=", 5U) == 0) {
    if (value) {
      *value = token + 5;
    }
    return ZZPLAY_OPT_LOOP;
  }
  if (strncmp(token, "audio=", 6U) == 0) {
    if (value) {
      *value = token + 6;
    }
    return ZZPLAY_OPT_AUDIO;
  }
  if (strcmp(token, "player") == 0) {
    return ZZPLAY_OPT_PLAYER;
  }
  if (strncmp(token, "player=", 7U) == 0) {
    if (value) {
      *value = token + 7;
    }
    return ZZPLAY_OPT_PLAYER;
  }
  if (strncmp(token, "ahiunit=", 8U) == 0) {
    if (value) {
      *value = token + 8;
    }
    return ZZPLAY_OPT_AHI_UNIT;
  }
  if (strncmp(token, "mhidriver=", 10U) == 0) {
    if (value) {
      *value = token + 10;
    }
    return ZZPLAY_OPT_MHI_DRIVER;
  }
  if (strncmp(token, "volume=", 7U) == 0) {
    if (value) {
      *value = token + 7;
    }
    return ZZPLAY_OPT_VOLUME;
  }
  return ZZPLAY_OPT_NONE;
}

ZZPlayOptionKey zzplay_options_key_from_tooltype(const char *tooltype,
                                                 const char **value)
{
  const char *equals;
  size_t name_length;

  if (value) {
    *value = 0;
  }
  if (!tooltype || !*tooltype) {
    return ZZPLAY_OPT_NONE;
  }
  /* Workbench keeps disabled ToolTypes in the icon wrapped in parentheses. */
  if (tooltype[0] == '(') {
    return ZZPLAY_OPT_NONE;
  }
  equals = strchr(tooltype, '=');
  name_length = equals ? (size_t)(equals - tooltype) : strlen(tooltype);
  if (equals && value) {
    *value = equals + 1;
  }
  {
    static const struct {
      const char *name;
      ZZPlayOptionKey key;
    } table[] = {
      { "FPS", ZZPLAY_OPT_FPS },
      { "BENCHMARK", ZZPLAY_OPT_BENCHMARK },
      { "FULLSCREEN", ZZPLAY_OPT_FULLSCREEN },
      { "QUIET", ZZPLAY_OPT_QUIET },
      { "VERBOSE", ZZPLAY_OPT_VERBOSE },
      { "TRACE", ZZPLAY_OPT_TRACE },
      { "LOOP", ZZPLAY_OPT_LOOP },
      { "AUDIO", ZZPLAY_OPT_AUDIO },
      { "PLAYER", ZZPLAY_OPT_PLAYER },
      { "AHIUNIT", ZZPLAY_OPT_AHI_UNIT },
      { "MHIDRIVER", ZZPLAY_OPT_MHI_DRIVER },
      { "VOLUME", ZZPLAY_OPT_VOLUME },
    };
    unsigned i;

    /* Compared case-insensitively: icon editors do not enforce case, and the
     * conventional upper-case ToolType must reach the same option as the
     * lower-case CLI spelling. */
    for (i = 0U; i < sizeof(table) / sizeof(table[0]); i++) {
      size_t j;
      int match;

      if (strlen(table[i].name) != name_length) {
        continue;
      }
      match = 1;
      for (j = 0U; match && j < name_length; j++) {
        if (zzplay_lower(tooltype[j]) != zzplay_lower(table[i].name[j])) {
          match = 0;
        }
      }
      if (match) {
        return table[i].key;
      }
    }
  }
  return ZZPLAY_OPT_NONE;
}

int zzplay_options_apply_tooltype(ZZPlayOptions *options,
                                  const char *tooltype)
{
  const char *value = 0;
  ZZPlayOptionKey key;

  if (!options) {
    return 0;
  }
  key = zzplay_options_key_from_tooltype(tooltype, &value);
  if (key == ZZPLAY_OPT_NONE) {
    /* Unknown ToolTypes are not an error: icons routinely carry entries for
     * other programs and for Workbench itself. */
    return 1;
  }
  /* "LOOP=" with nothing after it means the bare option. */
  if (value && *value == '\0') {
    value = 0;
  }
  return zzplay_options_apply(options, key, value);
}

int zzplay_options_add_path(ZZPlayOptions *options, const char *path)
{
  if (!options || !path || options->path_count >= ZZPLAY_OPTIONS_MAX_PATHS) {
    return 0;
  }
  options->paths[options->path_count++] = path;
  return 1;
}

ZZPlayOptionsResult zzplay_options_finish(ZZPlayOptions *options)
{
  if (!options) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  /* Benchmarking measures the pipeline, not the mixer, so it mutes audio —
   * but never overrides a backend the user actually asked for. */
  if (options->uncapped && !options->audio_explicit) {
    options->audio_backend = ZZPLAY_AUDIO_NONE;
  }
  /* A Workbench launch has nowhere to print, so stay quiet unless the user
   * said otherwise. --benchmark is about measuring, so its numbers are
   * worth a console even from Workbench. */
  if (!options->quiet_explicit) {
    options->quiet = (options->launch == ZZPLAY_LAUNCH_WORKBENCH &&
                      !options->show_fps)
                         ? 1
                         : 0;
  }
  return ZZPLAY_OPTIONS_OK;
}

int zzplay_options_wants_player(const ZZPlayOptions *options)
{
  if (!options) {
    return 0;
  }
  if (options->player >= 0) {
    return options->player != 0;
  }
  if (options->path_count == 0U) {
    return 1;
  }
  if (options->launch == ZZPLAY_LAUNCH_WORKBENCH) {
    return !options->fullscreen;
  }
  return 0;
}

ZZPlayOptionsResult zzplay_options_parse_cli(
    int argc, char **argv, ZZPlayOptions *options)
{
  int i;

  if (!options || argc < 1 || !argv) {
    return ZZPLAY_OPTIONS_ERROR;
  }
  zzplay_options_init(options, ZZPLAY_LAUNCH_CLI);
  for (i = 1; i < argc; i++) {
    const char *value = 0;
    ZZPlayOptionKey key = zzplay_options_key_from_cli(argv[i], &value);

    if (key == ZZPLAY_OPT_HELP) {
      return ZZPLAY_OPTIONS_HELP;
    }
    if (key != ZZPLAY_OPT_NONE) {
      if (!zzplay_options_apply(options, key, value)) {
        return ZZPLAY_OPTIONS_ERROR;
      }
      continue;
    }
    /* An unrecognised dashed token is a mistake, not a filename: silently
     * treating "--lop" as a path would produce a confusing open failure. */
    if (argv[i][0] == '-' && argv[i][1] == '-') {
      return ZZPLAY_OPTIONS_ERROR;
    }
    if (!zzplay_options_add_path(options, argv[i])) {
      return ZZPLAY_OPTIONS_ERROR;
    }
  }
  return zzplay_options_finish(options);
}
