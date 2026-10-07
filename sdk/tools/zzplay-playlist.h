/* Playlist model for zzplay: ordered entries, repeat/shuffle navigation and
 * M3U import/export.
 *
 * Host-testable: plain C plus stdio, no AmigaOS calls. Paths are stored as
 * given (AmigaDOS syntax on the target); the only path arithmetic here is
 * resolving a relative M3U entry against the playlist's own drawer.
 *
 * Navigation never touches the file system: whether an entry can actually
 * be played is the player's problem, which marks failures with
 * zzplay_playlist_mark_failed() so a run of unplayable files cannot spin.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_PLAYLIST_H
#define ZZPLAY_PLAYLIST_H

#include <stddef.h>
#include <stdint.h>

#define ZZPLAY_PLAYLIST_MAX_ENTRIES 4096U
#define ZZPLAY_PLAYLIST_PATH_MAX 256U
#define ZZPLAY_PLAYLIST_TITLE_MAX 96U

typedef enum ZZPlayRepeat {
  ZZPLAY_REPEAT_OFF = 0,
  ZZPLAY_REPEAT_ONE,
  ZZPLAY_REPEAT_ALL
} ZZPlayRepeat;

typedef struct ZZPlayPlaylistEntry {
  char *path;               /* owned, NUL-terminated */
  /* Display title, "" until known (ID3 tag or #EXTINF). The list shows the
   * file name when empty. */
  char title[ZZPLAY_PLAYLIST_TITLE_MAX];
  uint32_t duration_ms;     /* 0 when unknown */
  uint8_t failed;           /* last attempt could not be played */
} ZZPlayPlaylistEntry;

typedef struct ZZPlayPlaylist {
  ZZPlayPlaylistEntry *entries;
  /* order[k] is the entry index played k-th. Identity unless shuffled. */
  uint32_t *order;
  uint32_t count;
  uint32_t capacity;
  /* Index into `entries` of the current item, -1 when none. */
  int32_t current;
  /* Play-order position to continue from after the current entry was
   * removed (its successor now sits there), -1 when not applicable.
   * Cleared by set_current, clear and reshuffling. */
  int32_t resume_pos;
  ZZPlayRepeat repeat;
  int shuffle;
  uint32_t rng;             /* xorshift32 state, never 0 */
} ZZPlayPlaylist;

void zzplay_playlist_init(ZZPlayPlaylist *list);
void zzplay_playlist_free(ZZPlayPlaylist *list);
void zzplay_playlist_clear(ZZPlayPlaylist *list);

/* Append one path. Returns the new entry index, or -1 on allocation failure,
 * an empty/over-long path, or a full list. Duplicates are allowed. When
 * shuffled, the new entry is placed at a random position after the current
 * one in the play order. */
int32_t zzplay_playlist_add(ZZPlayPlaylist *list, const char *path);

/* Remove entry `index`. Keeps `current` pointing at the same entry, or -1
 * when the current entry itself was removed, in which case next() and
 * previous() continue from the removed entry's place in the play order.
 * Returns 0 for a bad index. */
int zzplay_playlist_remove(ZZPlayPlaylist *list, uint32_t index);

/* Move entry `from` to position `to` in entry order (not play order). */
int zzplay_playlist_move(ZZPlayPlaylist *list, uint32_t from, uint32_t to);

int zzplay_playlist_set_current(ZZPlayPlaylist *list, int32_t index);
void zzplay_playlist_set_repeat(ZZPlayPlaylist *list, ZZPlayRepeat repeat);

/* Turning shuffle on draws a fresh permutation with the current entry
 * first; turning it off restores entry order. `seed` 0 keeps the existing
 * generator state. */
void zzplay_playlist_set_shuffle(ZZPlayPlaylist *list, int shuffle,
                                 uint32_t seed);

typedef enum ZZPlayAdvance {
  /* The current item ended on its own (end of file). Honours REPEAT_ONE by
   * returning the same entry. */
  ZZPLAY_ADVANCE_AUTO = 0,
  /* The user asked for next/previous: REPEAT_ONE does not hold them on the
   * same entry. */
  ZZPLAY_ADVANCE_USER
} ZZPlayAdvance;

/* Entry index that follows the current one, or -1 when playback should
 * stop (end of list without REPEAT_ALL, or empty list). Does not modify
 * the playlist except that wrapping a shuffled list under REPEAT_ALL
 * reshuffles (keeping the wrapped-to entry different from the one that
 * just played when count > 1). With no current entry, returns the
 * successor of a removed current entry (see remove), else the first
 * entry in play order. Entries marked failed are skipped; if every entry
 * is failed, returns -1. */
int32_t zzplay_playlist_next(ZZPlayPlaylist *list, ZZPlayAdvance how);

/* Entry index before the current one in play order. At the start: the last
 * entry under REPEAT_ALL, otherwise the current entry again (restart).
 * -1 for an empty list or when every entry is failed. Failed entries are
 * skipped like next(). */
int32_t zzplay_playlist_previous(ZZPlayPlaylist *list);

void zzplay_playlist_mark_failed(ZZPlayPlaylist *list, uint32_t index,
                                 int failed);
/* Clears every failed mark (e.g. after the user changes settings). */
void zzplay_playlist_clear_failed(ZZPlayPlaylist *list);

int zzplay_playlist_set_title(ZZPlayPlaylist *list, uint32_t index,
                              const char *title);

/* Final component of an AmigaDOS path ("Work:Music/a.mp3" -> "a.mp3"). */
const char *zzplay_playlist_basename(const char *path);

/* Text shown for an entry: its title, else its file name. */
const char *zzplay_playlist_display_name(const ZZPlayPlaylist *list,
                                         uint32_t index);

/* Join an AmigaDOS drawer and a name the way dos.library AddPart() does:
 * "Work:" + "a" -> "Work:a", "Work:M" + "a" -> "Work:M/a", "" + "a" -> "a".
 * A name that is already absolute (contains ':') is copied unchanged.
 * Returns 0 if the result does not fit. */
int zzplay_playlist_join(char *out, size_t capacity, const char *drawer,
                         const char *name);

/* Drawer part of a path ("Work:M/a.m3u" -> "Work:M", "Work:a" -> "Work:"). */
void zzplay_playlist_drawer(char *out, size_t capacity, const char *path);

/* Parse M3U / extended M3U text and append its entries. Blank lines and
 * comments are skipped; "#EXTINF:<seconds>,<title>" sets the title and
 * duration of the entry that follows. Relative entries are resolved against
 * `base_drawer`. Accepts LF, CRLF and CR line ends and a UTF-8 BOM.
 * Returns the number of entries added. */
uint32_t zzplay_playlist_parse_m3u(ZZPlayPlaylist *list, const char *text,
                                   size_t length, const char *base_drawer);

/* Write the list in entry order as extended M3U ("#EXTM3U" header, an
 * "#EXTINF" line for entries with a known title or duration, absolute
 * paths). Returns the number of bytes required, excluding the NUL; output
 * is truncated (still NUL-terminated) when `capacity` is too small. */
size_t zzplay_playlist_format_m3u(const ZZPlayPlaylist *list, char *out,
                                  size_t capacity);

/* stdio wrappers around parse/format. Return entries added / 1 on success,
 * 0 on any I/O failure. */
uint32_t zzplay_playlist_load_m3u(ZZPlayPlaylist *list, const char *path);
int zzplay_playlist_save_m3u(const ZZPlayPlaylist *list, const char *path);

/* True when the path names a playlist file (.m3u / .m3u8, any case). */
int zzplay_playlist_is_playlist_path(const char *path);

#endif /* ZZPLAY_PLAYLIST_H */
