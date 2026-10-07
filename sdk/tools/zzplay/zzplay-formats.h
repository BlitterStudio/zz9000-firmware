/* Media format registry for zzplay.
 *
 * Every playable format is one row in a table: its display name, the file
 * extensions used for drawer scans and the ASL pattern, the probe result
 * kind that identifies it, and whether it has video (a one-shot run opens
 * the player window only for audio-only items). Per-item capabilities such
 * as seeking and volume are reported by the engine that plays the item,
 * because they depend on the engine and output, not the format. The format
 * actually played is always decided by
 * probing the file contents (zzplay-probe.h), never by extension alone;
 * extensions only decide what a drawer scan or file requester offers.
 *
 * Adding a format means: teach zzplay_probe_media_file() a new
 * ZZPlayMediaKind, add a row here, and register a playback engine for the
 * kind in zzplay.c (zzplay_engines[]).
 *
 * Host-testable. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_FORMATS_H
#define ZZPLAY_FORMATS_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-probe.h"

#define ZZPLAY_FORMAT_HAS_VIDEO 0x0001U

typedef struct ZZPlayFormat {
  ZZPlayMediaKind kind;
  const char *name;         /* "MPEG-1 Program Stream" */
  /* Lower-case extensions without the dot, NULL-terminated. */
  const char *const *extensions;
  uint32_t flags;
} ZZPlayFormat;

size_t zzplay_format_count(void);
const ZZPlayFormat *zzplay_format_at(size_t index);

/* The row for a probed kind, or NULL for ZZPLAY_MEDIA_KIND_UNSUPPORTED or
 * an unregistered kind. */
const ZZPlayFormat *zzplay_format_for_kind(ZZPlayMediaKind kind);

/* The row whose extension list contains the path's extension (case-
 * insensitive), or NULL. Used for drawer scans only. */
const ZZPlayFormat *zzplay_format_for_path(const char *path);

/* AmigaDOS pattern matching every registered media extension and, when
 * `include_playlists` is set, playlist files: "#?.(mpg|mpeg|mp3|m3u|m3u8)".
 * Returns 0 if it does not fit. */
int zzplay_formats_pattern(char *out, size_t capacity,
                           int include_playlists);

#endif /* ZZPLAY_FORMATS_H */
