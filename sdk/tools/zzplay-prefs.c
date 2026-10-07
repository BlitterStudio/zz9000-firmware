/* Persistent preferences for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char zzplay_prefs_lower(char c)
{
  if (c >= 'A' && c <= 'Z') {
    return (char)(c + ('a' - 'A'));
  }
  return c;
}

static int zzplay_prefs_case_eq(const char *a, const char *b)
{
  while (*a && *b) {
    if (zzplay_prefs_lower(*a) != zzplay_prefs_lower(*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

void zzplay_prefs_defaults(ZZPlayPrefs *prefs)
{
  if (!prefs) {
    return;
  }
  memset(prefs, 0, sizeof(*prefs));
  prefs->mp3_output = ZZPLAY_AUDIO_AUTO;
  prefs->video_audio = ZZPLAY_AUDIO_AUTO;
  prefs->ahi_unit = 0U;
  strncpy(prefs->mhi_driver, ZZPLAY_PREFS_DEFAULT_MHI_DRIVER,
          sizeof(prefs->mhi_driver) - 1U);
  prefs->mhi_driver[sizeof(prefs->mhi_driver) - 1U] = '\0';
  prefs->volume = 100U;
  prefs->repeat = ZZPLAY_REPEAT_OFF;
  prefs->shuffle = 0;
  prefs->window_left = -1;
  prefs->window_top = -1;
  prefs->last_drawer[0] = '\0';
}

int zzplay_prefs_valid_driver(const char *name)
{
  size_t len;

  if (!name || !*name) {
    return 0;
  }
  len = strlen(name);
  if (len >= ZZPLAY_PREFS_DRIVER_MAX) {
    return 0;
  }
  if (strchr(name, ':') != NULL || strchr(name, '/') != NULL) {
    return 0;
  }
  return 1;
}

static int zzplay_parse_int(const char *str, int *val)
{
  char *end = NULL;
  long v;

  if (!str || !*str || !val) {
    return 0;
  }
  v = strtol(str, &end, 10);
  if (!end || *end != '\0') {
    return 0;
  }
  *val = (int)v;
  return 1;
}

uint32_t zzplay_prefs_parse(ZZPlayPrefs *prefs, const char *text,
                            size_t length)
{
  const char *p;
  const char *end;
  uint32_t accepted = 0U;

  if (!prefs || !text || length == 0U) {
    return 0U;
  }

  p = text;
  end = text + length;

  while (p < end) {
    const char *line_start = p;
    const char *line_end = p;
    const char *eq;
    char key[64];
    char val[ZZPLAY_PREFS_DRAWER_MAX];
    size_t klen;
    size_t vlen;

    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
      line_end++;
    }
    p = line_end;
    if (p < end && *p == '\r') p++;
    if (p < end && *p == '\n') p++;

    /* Skip leading whitespace */
    while (line_start < line_end && (*line_start == ' ' || *line_start == '\t')) {
      line_start++;
    }
    /* Skip trailing whitespace */
    while (line_end > line_start &&
           (line_end[-1] == ' ' || line_end[-1] == '\t')) {
      line_end--;
    }

    if (line_start >= line_end) {
      continue;
    }
    if (*line_start == '#' || *line_start == ';') {
      continue;
    }

    eq = line_start;
    while (eq < line_end && *eq != '=') {
      eq++;
    }
    if (eq >= line_end) {
      continue;
    }

    /* Key */
    {
      const char *ks = line_start;
      const char *ke = eq;
      while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) {
        ke--;
      }
      klen = (size_t)(ke - ks);
      if (klen == 0U || klen >= sizeof(key)) {
        continue;
      }
      memcpy(key, ks, klen);
      key[klen] = '\0';
    }

    /* Value */
    {
      const char *vs = eq + 1;
      const char *ve = line_end;
      while (vs < ve && (*vs == ' ' || *vs == '\t')) {
        vs++;
      }
      while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) {
        ve--;
      }
      vlen = (size_t)(ve - vs);
      if (vlen >= sizeof(val)) {
        vlen = sizeof(val) - 1U;
      }
      memcpy(val, vs, vlen);
      val[vlen] = '\0';
    }

    if (zzplay_prefs_case_eq(key, "MP3OUTPUT")) {
      if (zzplay_prefs_case_eq(val, "AUTO")) {
        prefs->mp3_output = ZZPLAY_AUDIO_AUTO;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "MHI")) {
        prefs->mp3_output = ZZPLAY_AUDIO_MHI;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "AHI")) {
        prefs->mp3_output = ZZPLAY_AUDIO_AHI;
        accepted++;
      }
      /* No NONE: muted MP3 decode is unpaced (a benchmark), so as a saved
       * preference it would race silently through the playlist. AUDIO=none
       * remains available per launch. */
    } else if (zzplay_prefs_case_eq(key, "VIDEOAUDIO")) {
      if (zzplay_prefs_case_eq(val, "AUTO")) {
        prefs->video_audio = ZZPLAY_AUDIO_AUTO;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "AHI")) {
        prefs->video_audio = ZZPLAY_AUDIO_AHI;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "AX")) {
        prefs->video_audio = ZZPLAY_AUDIO_AX;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "NONE")) {
        prefs->video_audio = ZZPLAY_AUDIO_NONE;
        accepted++;
      }
    } else if (zzplay_prefs_case_eq(key, "AHIUNIT")) {
      int u;
      if (zzplay_parse_int(val, &u) && u >= 0 && u <= 3) {
        prefs->ahi_unit = (uint32_t)u;
        accepted++;
      }
    } else if (zzplay_prefs_case_eq(key, "MHIDRIVER")) {
      if (zzplay_prefs_valid_driver(val)) {
        strncpy(prefs->mhi_driver, val, sizeof(prefs->mhi_driver) - 1U);
        prefs->mhi_driver[sizeof(prefs->mhi_driver) - 1U] = '\0';
        accepted++;
      }
    } else if (zzplay_prefs_case_eq(key, "VOLUME")) {
      int vol;
      if (zzplay_parse_int(val, &vol) && vol >= 0 && vol <= 100) {
        prefs->volume = (uint32_t)vol;
        accepted++;
      }
    } else if (zzplay_prefs_case_eq(key, "REPEAT")) {
      if (zzplay_prefs_case_eq(val, "OFF")) {
        prefs->repeat = ZZPLAY_REPEAT_OFF;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "ONE")) {
        prefs->repeat = ZZPLAY_REPEAT_ONE;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "ALL")) {
        prefs->repeat = ZZPLAY_REPEAT_ALL;
        accepted++;
      }
    } else if (zzplay_prefs_case_eq(key, "SHUFFLE")) {
      if (zzplay_prefs_case_eq(val, "YES") || zzplay_prefs_case_eq(val, "1") ||
          zzplay_prefs_case_eq(val, "TRUE")) {
        prefs->shuffle = 1;
        accepted++;
      } else if (zzplay_prefs_case_eq(val, "NO") ||
                 zzplay_prefs_case_eq(val, "0") ||
                 zzplay_prefs_case_eq(val, "FALSE")) {
        prefs->shuffle = 0;
        accepted++;
      }
    } else if (zzplay_prefs_case_eq(key, "WINDOW")) {
      const char *comma = strchr(val, ',');
      if (comma) {
        char x_buf[32];
        int x, y;
        size_t x_len = (size_t)(comma - val);
        if (x_len < sizeof(x_buf)) {
          memcpy(x_buf, val, x_len);
          x_buf[x_len] = '\0';
          if (zzplay_parse_int(x_buf, &x) && zzplay_parse_int(comma + 1, &y)) {
            prefs->window_left = (int16_t)x;
            prefs->window_top = (int16_t)y;
            accepted++;
          }
        }
      }
    } else if (zzplay_prefs_case_eq(key, "DRAWER")) {
      if (vlen < sizeof(prefs->last_drawer)) {
        strncpy(prefs->last_drawer, val, sizeof(prefs->last_drawer) - 1U);
        prefs->last_drawer[sizeof(prefs->last_drawer) - 1U] = '\0';
        accepted++;
      }
    }
  }

  return accepted;
}

static const char *zzplay_prefs_backend_name(ZZPlayAudioBackend b)
{
  switch (b) {
  case ZZPLAY_AUDIO_AHI: return "AHI";
  case ZZPLAY_AUDIO_MHI: return "MHI";
  case ZZPLAY_AUDIO_AX: return "AX";
  case ZZPLAY_AUDIO_NONE: return "NONE";
  case ZZPLAY_AUDIO_AUTO:
  default:
    return "AUTO";
  }
}

static const char *zzplay_prefs_repeat_name(ZZPlayRepeat r)
{
  switch (r) {
  case ZZPLAY_REPEAT_ONE: return "ONE";
  case ZZPLAY_REPEAT_ALL: return "ALL";
  case ZZPLAY_REPEAT_OFF:
  default:
    return "OFF";
  }
}

size_t zzplay_prefs_format(const ZZPlayPrefs *prefs, char *out,
                           size_t capacity)
{
  static char buf[ZZPLAY_PREFS_TEXT_MAX];
  int written;
  size_t needed;

  if (!prefs) {
    if (out && capacity > 0U) {
      out[0] = '\0';
    }
    return 0U;
  }

  written = snprintf(
      buf, sizeof(buf),
      "# ZZPlay settings\n"
      "MP3OUTPUT=%s\n"
      "VIDEOAUDIO=%s\n"
      "AHIUNIT=%u\n"
      "MHIDRIVER=%s\n"
      "VOLUME=%u\n"
      "REPEAT=%s\n"
      "SHUFFLE=%s\n"
      "WINDOW=%d,%d\n"
      "DRAWER=%s\n",
      zzplay_prefs_backend_name(prefs->mp3_output),
      zzplay_prefs_backend_name(prefs->video_audio),
      prefs->ahi_unit,
      prefs->mhi_driver,
      prefs->volume,
      zzplay_prefs_repeat_name(prefs->repeat),
      prefs->shuffle ? "YES" : "NO",
      (int)prefs->window_left, (int)prefs->window_top,
      prefs->last_drawer);

  if (written < 0) {
    if (out && capacity > 0U) {
      out[0] = '\0';
    }
    return 0U;
  }

  needed = (size_t)written;
  if (out && capacity > 0U) {
    size_t copy = (capacity - 1U < needed) ? (capacity - 1U) : needed;
    memcpy(out, buf, copy);
    out[copy] = '\0';
  }
  return needed;
}

int zzplay_prefs_load(ZZPlayPrefs *prefs, const char *path)
{
  FILE *f;
  long sz;
  char *buf;
  size_t len;

  if (!prefs || !path) {
    return 0;
  }
  zzplay_prefs_defaults(prefs);

  f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  sz = ftell(f);
  if (sz <= 0 || sz > 1024 * 1024) {
    fclose(f);
    return 1;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }
  len = (size_t)sz;
  buf = (char *)malloc(len + 1U);
  if (!buf) {
    fclose(f);
    return 0;
  }
  if (fread(buf, 1U, len, f) != len) {
    free(buf);
    fclose(f);
    return 0;
  }
  fclose(f);
  buf[len] = '\0';

  zzplay_prefs_parse(prefs, buf, len);
  free(buf);
  return 1;
}

int zzplay_prefs_save(const ZZPlayPrefs *prefs, const char *path)
{
  static char buf[ZZPLAY_PREFS_TEXT_MAX];
  size_t needed;
  FILE *f;
  size_t written;

  if (!prefs || !path) {
    return 0;
  }
  needed = zzplay_prefs_format(prefs, buf, sizeof(buf));
  if (needed >= sizeof(buf)) {
    return 0;
  }
  f = fopen(path, "wb");
  if (!f) {
    return 0;
  }
  written = fwrite(buf, 1U, needed, f);
  fclose(f);
  return (written == needed) ? 1 : 0;
}

void zzplay_prefs_copy_session(ZZPlayPrefs *to, const ZZPlayPrefs *from)
{
  if (!to || !from) {
    return;
  }
  to->volume = from->volume;
  to->repeat = from->repeat;
  to->shuffle = from->shuffle;
  to->window_left = from->window_left;
  to->window_top = from->window_top;
  strncpy(to->last_drawer, from->last_drawer, sizeof(to->last_drawer) - 1U);
  to->last_drawer[sizeof(to->last_drawer) - 1U] = '\0';
}

int zzplay_prefs_save_session(const ZZPlayPrefs *current, const char *path)
{
  ZZPlayPrefs saved;

  if (!current || !path) {
    return 0;
  }
  zzplay_prefs_load(&saved, path);
  zzplay_prefs_copy_session(&saved, current);
  return zzplay_prefs_save(&saved, path);
}

void zzplay_prefs_apply_options(ZZPlayPrefs *prefs,
                                const ZZPlayOptions *options)
{
  if (!prefs || !options) {
    return;
  }
  if (options->ahi_unit >= 0 && options->ahi_unit <= 3) {
    prefs->ahi_unit = (uint32_t)options->ahi_unit;
  }
  if (options->mhi_driver && zzplay_prefs_valid_driver(options->mhi_driver)) {
    strncpy(prefs->mhi_driver, options->mhi_driver,
            sizeof(prefs->mhi_driver) - 1U);
    prefs->mhi_driver[sizeof(prefs->mhi_driver) - 1U] = '\0';
  }
  if (options->volume >= 0 && options->volume <= 100) {
    prefs->volume = (uint32_t)options->volume;
  }
}

ZZPlayAudioBackend zzplay_prefs_requested_backend(
    const ZZPlayPrefs *prefs, const ZZPlayOptions *options,
    ZZPlayMediaAudio media, int *strict)
{
  if (options && (options->audio_explicit || options->uncapped)) {
    if (strict) {
      *strict = 1;
    }
    return options->audio_backend;
  }
  if (strict) {
    *strict = 0;
  }
  if (!prefs) {
    return ZZPLAY_AUDIO_AUTO;
  }
  if (media == ZZPLAY_MEDIA_AUDIO_MP3) {
    return prefs->mp3_output;
  }
  if (media == ZZPLAY_MEDIA_AUDIO_MP2) {
    return prefs->video_audio;
  }
  return ZZPLAY_AUDIO_AUTO;
}
