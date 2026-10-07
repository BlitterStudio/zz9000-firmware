/* File-system side of the playlist: expanding what the user hands ZZPlay
 * into playlist entries.
 *
 * AmigaOS-only (dos.library, asl.library). The playlist model itself is
 * host-testable in zzplay-playlist.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_FILES_H
#define ZZPLAY_FILES_H

#include <stdint.h>

#include "zzplay-playlist.h"

/* Add whatever `path` names:
 *   - an AmigaDOS pattern ("Work:Music/#?.mp3"): every matching file, each
 *     treated as if it had been named on its own (a playlist expands);
 *   - a drawer or volume: its files whose extension is a registered media
 *     format (zzplay-formats.h), sorted by name, recursing into sub-drawers
 *     up to ZZPLAY_FILES_MAX_DEPTH levels;
 *   - a playlist file (.m3u/.m3u8): its entries;
 *   - anything else: added as one entry without probing (playback decides).
 * Returns the number of entries added. */
#define ZZPLAY_FILES_MAX_DEPTH 4U
uint32_t zzplay_files_add(ZZPlayPlaylist *list, const char *path);

/* The same for a Workbench argument (WBArg / AppMessage): `lock` is a BPTR
 * to the containing drawer and `name` the object in it; an empty name means
 * the lock is the object itself (a dropped drawer or volume). */
uint32_t zzplay_files_add_lock(ZZPlayPlaylist *list, long lock,
                               const char *name);

/* The full path of a Workbench argument (as above) in `path`, without
 * touching what it names. Returns 0 when it cannot be resolved. */
int zzplay_files_lock_path(long lock, const char *name, char *path,
                           uint32_t size);

#endif /* ZZPLAY_FILES_H */
