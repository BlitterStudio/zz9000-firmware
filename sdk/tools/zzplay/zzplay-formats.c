/* Media format registry for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-formats.h"
#include "zzplay-core.h"

#include <string.h>

static const char *const zzplay_mpeg_ps_extensions[] = {
  "mpg", "mpeg", NULL
};

static const char *const zzplay_webm_extensions[] = {
  "webm", "mkv", NULL
};

static const char *const zzplay_mp3_extensions[] = {
  "mp3", NULL
};
static const char *const zzplay_webp_extensions[] = {
  "webp", NULL
};
static const char *const zzplay_flac_extensions[] = {
  "flac", NULL
};
static const char *const zzplay_vorbis_extensions[] = {
  "ogg", "oga", NULL
};


static const ZZPlayFormat zzplay_formats_table[] = {
  {
    ZZPLAY_MEDIA_KIND_MPEG_PS,
    "MPEG-1 Program Stream",
    zzplay_mpeg_ps_extensions,
    ZZPLAY_FORMAT_HAS_VIDEO
  },
  {
    ZZPLAY_MEDIA_KIND_WEBM,
    "WebM",
    zzplay_webm_extensions,
    ZZPLAY_FORMAT_HAS_VIDEO
  },
  {
    ZZPLAY_MEDIA_KIND_MP3,
    "MPEG Layer III",
    zzplay_mp3_extensions,
    0U
  },
  {
    ZZPLAY_MEDIA_KIND_WEBP,
    "Animated WebP",
    zzplay_webp_extensions,
    ZZPLAY_FORMAT_HAS_VIDEO
  },
  {
    ZZPLAY_MEDIA_KIND_FLAC,
    "FLAC",
    zzplay_flac_extensions,
    0U
  },
  {
    ZZPLAY_MEDIA_KIND_VORBIS,
    "Ogg Vorbis",
    zzplay_vorbis_extensions,
    0U
  }
};

static const size_t zzplay_formats_count =
    sizeof(zzplay_formats_table) / sizeof(zzplay_formats_table[0]);

size_t zzplay_format_count(void)
{
  return zzplay_formats_count;
}

const ZZPlayFormat *zzplay_format_at(size_t index)
{
  if (index >= zzplay_formats_count) {
    return NULL;
  }
  return &zzplay_formats_table[index];
}

const ZZPlayFormat *zzplay_format_for_kind(ZZPlayMediaKind kind)
{
  size_t i;

  if (kind == ZZPLAY_MEDIA_KIND_UNSUPPORTED) {
    return NULL;
  }
  for (i = 0U; i < zzplay_formats_count; i++) {
    if (zzplay_formats_table[i].kind == kind) {
      return &zzplay_formats_table[i];
    }
  }
  return NULL;
}


const ZZPlayFormat *zzplay_format_for_path(const char *path)
{
  const char *dot;
  size_t i;

  if (!path) {
    return NULL;
  }
  dot = strrchr(path, '.');
  if (!dot) {
    return NULL;
  }
  dot++; /* skip dot */
  for (i = 0U; i < zzplay_formats_count; i++) {
    const char *const *ext = zzplay_formats_table[i].extensions;
    while (*ext) {
      if (zzplay_ascii_equal_fold(dot, *ext)) {
        return &zzplay_formats_table[i];
      }
      ext++;
    }
  }
  return NULL;
}

int zzplay_formats_pattern(char *out, size_t capacity,
                           int include_playlists)
{
  static const char prefix[] = "#?.(";
  static const char *const playlists[] = { "m3u", "m3u8" };
  size_t required = sizeof(prefix);
  size_t i;
  unsigned extension_count = 0U;

  for (i = 0U; i < zzplay_formats_count; i++) {
    const char *const *ext = zzplay_formats_table[i].extensions;

    while (*ext) {
      required += strlen(*ext) + (extension_count != 0U ? 1U : 0U);
      extension_count++;
      ext++;
    }
  }
  if (include_playlists) {
    for (i = 0U; i < sizeof(playlists) / sizeof(playlists[0]); i++) {
      required += strlen(playlists[i]) + (extension_count != 0U ? 1U : 0U);
      extension_count++;
    }
  }
  required++; /* closing ')' */
  if (!out || required > capacity) {
    return 0;
  }

  memcpy(out, prefix, sizeof(prefix) - 1U);
  required = sizeof(prefix) - 1U;
  extension_count = 0U;
  for (i = 0U; i < zzplay_formats_count; i++) {
    const char *const *ext = zzplay_formats_table[i].extensions;

    while (*ext) {
      if (extension_count != 0U) {
        out[required++] = '|';
      }
      memcpy(out + required, *ext, strlen(*ext));
      required += strlen(*ext);
      extension_count++;
      ext++;
    }
  }
  if (include_playlists) {
    for (i = 0U; i < sizeof(playlists) / sizeof(playlists[0]); i++) {
      out[required++] = '|';
      memcpy(out + required, playlists[i], strlen(playlists[i]));
      required += strlen(playlists[i]);
    }
  }
  out[required++] = ')';
  out[required] = '\0';
  return 1;
}
