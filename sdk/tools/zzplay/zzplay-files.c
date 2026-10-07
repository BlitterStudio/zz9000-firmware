/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-files.h"

#include <dos/dos.h>
#include <dos/dosasl.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdlib.h>
#include <string.h>

#include "zzplay-formats.h"

typedef struct NameArray {
  char **items;
  uint32_t count;
  uint32_t capacity;
} NameArray;

static void name_array_init(NameArray *arr)
{
  arr->items = 0;
  arr->count = 0U;
  arr->capacity = 0U;
}

static int name_array_push(NameArray *arr, const char *name)
{
  char *copy;

  if (!name) {
    return 0;
  }

  if (arr->count >= arr->capacity) {
    uint32_t new_cap = arr->capacity ? (arr->capacity * 2U) : 16U;
    char **new_items = (char **)realloc(arr->items,
                                        (size_t)new_cap * sizeof(char *));
    if (!new_items) {
      return 0;
    }
    arr->items = new_items;
    arr->capacity = new_cap;
  }

  copy = strdup(name);
  if (!copy) {
    return 0;
  }
  arr->items[arr->count++] = copy;
  return 1;
}

static void name_array_free(NameArray *arr)
{
  uint32_t i;

  if (arr->items) {
    for (i = 0U; i < arr->count; i++) {
      free(arr->items[i]);
    }
    free(arr->items);
    arr->items = 0;
  }
  arr->count = 0U;
  arr->capacity = 0U;
}

static int compare_str_ptrs(const void *a, const void *b)
{
  return strcasecmp(*(const char * const *)a, *(const char * const *)b);
}

/* Returns 1 if path contains AmigaDOS wildcard characters. */
static int is_wildcard_pattern(const char *path)
{
  size_t len = strlen(path);
  LONG buf_len = (LONG)(len * 2U + 4U);
  UBYTE *pat_buf;
  LONG res;

  if (buf_len < 128) {
    buf_len = 128;
  }
  pat_buf = (UBYTE *)AllocVec((ULONG)buf_len, MEMF_PUBLIC | MEMF_CLEAR);
  if (!pat_buf) {
    return 0;
  }
  res = ParsePatternNoCase((CONST_STRPTR)path, pat_buf, buf_len);
  FreeVec(pat_buf);
  return (res == 1) ? 1 : 0;
}

static uint32_t add_pattern(ZZPlayPlaylist *list, const char *pattern)
{
  uint32_t added = 0U;
  size_t path_max = ZZPLAY_PLAYLIST_PATH_MAX;
  size_t anchor_size = sizeof(struct AnchorPath) + path_max;
  struct AnchorPath *anchor;

  if (!list || list->count >= ZZPLAY_PLAYLIST_MAX_ENTRIES) {
    return 0U;
  }
  anchor = (struct AnchorPath *)AllocVec(
      (ULONG)anchor_size, MEMF_PUBLIC | MEMF_CLEAR);
  if (!anchor) {
    return 0U;
  }

  anchor->ap_Strlen = (WORD)path_max;
  anchor->ap_Flags = APF_DOWILD;

  if (MatchFirst((CONST_STRPTR)pattern, anchor) == 0) {
    do {
      if (list->count >= ZZPLAY_PLAYLIST_MAX_ENTRIES) {
        break;
      }
      if (anchor->ap_Info.fib_DirEntryType < 0) {
        const char *match_path = (const char *)anchor->ap_Buf;
        if (!match_path || match_path[0] == '\0') {
          match_path = (const char *)anchor->ap_Info.fib_FileName;
        }
        /* A match behaves like the same path named directly: a playlist
         * contributes its entries, anything else is one entry. */
        if (match_path && match_path[0] != '\0') {
          if (zzplay_playlist_is_playlist_path(match_path)) {
            added += zzplay_playlist_load_m3u(list, match_path);
          } else if (zzplay_playlist_add(list, match_path) >= 0) {
            added++;
          }
        }
      } else {
        /* Do not enter directory nodes while matching files. */
        anchor->ap_Flags &= ~APF_DODIR;
      }
    } while (MatchNext(anchor) == 0);
    MatchEnd(anchor);
  }

  FreeVec(anchor);
  return added;
}

static uint32_t add_drawer(ZZPlayPlaylist *list, BPTR dir_lock,
                           const char *dir_name, uint32_t depth)
{
  char base_path[ZZPLAY_PLAYLIST_PATH_MAX];
  struct FileInfoBlock *fib;
  NameArray files;
  NameArray subdrawers;
  uint32_t added = 0U;
  uint32_t i;

  if (!list || depth >= ZZPLAY_FILES_MAX_DEPTH ||
      list->count >= ZZPLAY_PLAYLIST_MAX_ENTRIES) {
    return 0U;
  }

  base_path[0] = '\0';
  if (!NameFromLock(dir_lock, (STRPTR)base_path, sizeof(base_path))) {
    if (dir_name) {
      strncpy(base_path, dir_name, sizeof(base_path) - 1U);
      base_path[sizeof(base_path) - 1U] = '\0';
    }
  }

  fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
  if (!fib) {
    return 0U;
  }

  if (!Examine(dir_lock, fib)) {
    FreeDosObject(DOS_FIB, fib);
    return 0U;
  }

  name_array_init(&files);
  name_array_init(&subdrawers);

  while (ExNext(dir_lock, fib)) {
    const char *name = (const char *)fib->fib_FileName;
    if (fib->fib_DirEntryType < 0) {
      if (zzplay_format_for_path(name) != 0) {
        name_array_push(&files, name);
      }
    } else if (fib->fib_DirEntryType > 0) {
      if (depth + 1U < ZZPLAY_FILES_MAX_DEPTH) {
        name_array_push(&subdrawers, name);
      }
    }
  }

  FreeDosObject(DOS_FIB, fib);

  if (files.count > 1U) {
    qsort(files.items, (size_t)files.count, sizeof(char *), compare_str_ptrs);
  }

  for (i = 0U; i < files.count &&
       list->count < ZZPLAY_PLAYLIST_MAX_ENTRIES; i++) {
    char full_path[ZZPLAY_PLAYLIST_PATH_MAX];
    strncpy(full_path, base_path, sizeof(full_path) - 1U);
    full_path[sizeof(full_path) - 1U] = '\0';
    AddPart((STRPTR)full_path, (CONST_STRPTR)files.items[i], sizeof(full_path));
    if (zzplay_playlist_add(list, full_path) >= 0) {
      added++;
    }
  }
  name_array_free(&files);

  if (subdrawers.count > 1U) {
    qsort(subdrawers.items, (size_t)subdrawers.count, sizeof(char *),
          compare_str_ptrs);
  }

  for (i = 0U; i < subdrawers.count &&
       list->count < ZZPLAY_PLAYLIST_MAX_ENTRIES; i++) {
    char sub_path[ZZPLAY_PLAYLIST_PATH_MAX];
    BPTR sub_lock;

    strncpy(sub_path, base_path, sizeof(sub_path) - 1U);
    sub_path[sizeof(sub_path) - 1U] = '\0';
    AddPart((STRPTR)sub_path, (CONST_STRPTR)subdrawers.items[i],
            sizeof(sub_path));

    sub_lock = Lock((CONST_STRPTR)sub_path, ACCESS_READ);
    if (sub_lock) {
      added += add_drawer(list, sub_lock, sub_path, depth + 1U);
      UnLock(sub_lock);
    }
  }
  name_array_free(&subdrawers);

  return added;
}

uint32_t zzplay_files_add(ZZPlayPlaylist *list, const char *path)
{
  BPTR lock;
  struct FileInfoBlock *fib;
  int is_drawer = 0;

  if (!list || !path || path[0] == '\0' ||
      list->count >= ZZPLAY_PLAYLIST_MAX_ENTRIES) {
    return 0U;
  }

  if (is_wildcard_pattern(path)) {
    return add_pattern(list, path);
  }

  lock = Lock((CONST_STRPTR)path, ACCESS_READ);
  if (lock) {
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
    if (fib) {
      if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        is_drawer = 1;
      }
      FreeDosObject(DOS_FIB, fib);
    }
    if (is_drawer) {
      uint32_t added = add_drawer(list, lock, path, 0U);
      UnLock(lock);
      return added;
    }
    UnLock(lock);
  }

  if (zzplay_playlist_is_playlist_path(path)) {
    return zzplay_playlist_load_m3u(list, path);
  }

  return (zzplay_playlist_add(list, path) >= 0) ? 1U : 0U;
}

uint32_t zzplay_files_add_lock(ZZPlayPlaylist *list, long lock,
                               const char *name)
{
  char path_buf[ZZPLAY_PLAYLIST_PATH_MAX];

  if (!list) {
    return 0U;
  }

  path_buf[0] = '\0';
  if (lock) {
    if (!NameFromLock((BPTR)lock, (STRPTR)path_buf, sizeof(path_buf))) {
      path_buf[0] = '\0';
    }
  }

  if (name && name[0] != '\0') {
    if (path_buf[0] != '\0') {
      AddPart((STRPTR)path_buf, (CONST_STRPTR)name, sizeof(path_buf));
    } else {
      strncpy(path_buf, name, sizeof(path_buf) - 1U);
      path_buf[sizeof(path_buf) - 1U] = '\0';
    }
  }

  if (path_buf[0] == '\0') {
    return 0U;
  }

  return zzplay_files_add(list, path_buf);
}
