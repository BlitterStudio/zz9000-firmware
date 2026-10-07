/* Playlist model and M3U handling for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-playlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t zzplay_xorshift32(uint32_t *state)
{
  uint32_t x = *state;

  if (x == 0U) {
    x = 0x12345678U;
  }
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static void zzplay_fisher_yates(uint32_t *arr, uint32_t start, uint32_t count,
                                uint32_t *rng)
{
  uint32_t i;

  if (count <= 1U || start >= count) {
    return;
  }
  for (i = count - 1U; i > start; i--) {
    uint32_t j = start + (zzplay_xorshift32(rng) % (i - start + 1U));
    uint32_t tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
  }
}

void zzplay_playlist_init(ZZPlayPlaylist *list)
{
  if (!list) {
    return;
  }
  memset(list, 0, sizeof(*list));
  list->current = -1;
  list->resume_pos = -1;
  list->repeat = ZZPLAY_REPEAT_OFF;
  list->rng = 0x12345678U;
}

void zzplay_playlist_clear(ZZPlayPlaylist *list)
{
  uint32_t i;

  if (!list) {
    return;
  }
  for (i = 0U; i < list->count; i++) {
    free(list->entries[i].path);
    list->entries[i].path = NULL;
  }
  list->count = 0U;
  list->current = -1;
  list->resume_pos = -1;
}

void zzplay_playlist_free(ZZPlayPlaylist *list)
{
  if (!list) {
    return;
  }
  zzplay_playlist_clear(list);
  free(list->entries);
  free(list->order);
  list->entries = NULL;
  list->order = NULL;
  list->capacity = 0U;
}

int32_t zzplay_playlist_add(ZZPlayPlaylist *list, const char *path)
{
  char *copy;
  uint32_t idx;
  size_t len;

  if (!list || !path || !*path) {
    return -1;
  }
  len = strlen(path);
  if (len >= ZZPLAY_PLAYLIST_PATH_MAX || list->count >= ZZPLAY_PLAYLIST_MAX_ENTRIES) {
    return -1;
  }
  if (list->count >= list->capacity) {
    uint32_t new_cap = (list->capacity == 0U) ? 16U : (list->capacity * 2U);
    ZZPlayPlaylistEntry *new_entries;
    uint32_t *new_order;

    if (new_cap > ZZPLAY_PLAYLIST_MAX_ENTRIES) {
      new_cap = ZZPLAY_PLAYLIST_MAX_ENTRIES;
    }
    new_entries = (ZZPlayPlaylistEntry *)realloc(
        list->entries, new_cap * sizeof(ZZPlayPlaylistEntry));
    if (!new_entries) {
      return -1;
    }
    list->entries = new_entries;
    new_order = (uint32_t *)realloc(list->order, new_cap * sizeof(uint32_t));
    if (!new_order) {
      return -1;
    }
    list->order = new_order;
    list->capacity = new_cap;
  }

  copy = (char *)malloc(len + 1U);
  if (!copy) {
    return -1;
  }
  memcpy(copy, path, len + 1U);

  idx = list->count;
  list->entries[idx].path = copy;
  list->entries[idx].title[0] = '\0';
  list->entries[idx].duration_ms = 0U;
  list->entries[idx].failed = 0U;

  if (!list->shuffle) {
    list->order[idx] = idx;
  } else {
    /* Insert at random position after current in play order. */
    uint32_t pos;
    int32_t cur_pos = -1;

    if (list->current >= 0) {
      uint32_t k;
      for (k = 0U; k < list->count; k++) {
        if (list->order[k] == (uint32_t)list->current) {
          cur_pos = (int32_t)k;
          break;
        }
      }
    }
    if (cur_pos < 0 || (uint32_t)cur_pos >= list->count) {
      /* Any position 0..count */
      pos = zzplay_xorshift32(&list->rng) % (list->count + 1U);
    } else {
      /* Position in cur_pos + 1 .. count */
      uint32_t slots = list->count - (uint32_t)cur_pos;
      pos = (uint32_t)cur_pos + 1U + (zzplay_xorshift32(&list->rng) % slots);
    }
    if (pos < list->count) {
      memmove(&list->order[pos + 1U], &list->order[pos],
              (list->count - pos) * sizeof(uint32_t));
    }
    list->order[pos] = idx;
    if (list->resume_pos >= 0 && pos <= (uint32_t)list->resume_pos) {
      list->resume_pos++;
    }
  }

  list->count++;
  return (int32_t)idx;
}

int zzplay_playlist_remove(ZZPlayPlaylist *list, uint32_t index)
{
  uint32_t k;
  uint32_t order_pos = 0U;

  if (!list || index >= list->count) {
    return 0;
  }
  free(list->entries[index].path);

  if (list->current == (int32_t)index) {
    list->current = -1;
    /* Navigation continues from where the removed entry stood: after the
     * removal its play-order successor occupies its position. */
    list->resume_pos = -2; /* resolved below, once order_pos is known */
  } else if (list->current > (int32_t)index) {
    list->current--;
  }

  if (index + 1U < list->count) {
    memmove(&list->entries[index], &list->entries[index + 1U],
            (list->count - index - 1U) * sizeof(ZZPlayPlaylistEntry));
  }

  for (k = 0U; k < list->count; k++) {
    if (list->order[k] == index) {
      order_pos = k;
      break;
    }
  }
  if (order_pos + 1U < list->count) {
    memmove(&list->order[order_pos], &list->order[order_pos + 1U],
            (list->count - order_pos - 1U) * sizeof(uint32_t));
  }
  for (k = 0U; k < list->count - 1U; k++) {
    if (list->order[k] > index) {
      list->order[k]--;
    }
  }
  if (list->resume_pos == -2) {
    list->resume_pos = (int32_t)order_pos;
  } else if (list->resume_pos > (int32_t)order_pos) {
    list->resume_pos--;
  }

  list->count--;
  return 1;
}

int zzplay_playlist_move(ZZPlayPlaylist *list, uint32_t from, uint32_t to)
{
  ZZPlayPlaylistEntry saved;
  uint32_t k;

  if (!list || from >= list->count || to >= list->count) {
    return 0;
  }
  if (from == to) {
    return 1;
  }

  saved = list->entries[from];
  if (from < to) {
    memmove(&list->entries[from], &list->entries[from + 1U],
            (to - from) * sizeof(ZZPlayPlaylistEntry));
  } else {
    memmove(&list->entries[to + 1U], &list->entries[to],
            (from - to) * sizeof(ZZPlayPlaylistEntry));
  }
  list->entries[to] = saved;

  if (list->current == (int32_t)from) {
    list->current = (int32_t)to;
  } else if (from < to && list->current > (int32_t)from &&
             list->current <= (int32_t)to) {
    list->current--;
  } else if (from > to && list->current >= (int32_t)to &&
             list->current < (int32_t)from) {
    list->current++;
  }

  for (k = 0U; k < list->count; k++) {
    uint32_t v = list->order[k];
    if (v == from) {
      list->order[k] = to;
    } else if (from < to && v > from && v <= to) {
      list->order[k] = v - 1U;
    } else if (from > to && v >= to && v < from) {
      list->order[k] = v + 1U;
    }
  }
  return 1;
}

int zzplay_playlist_set_current(ZZPlayPlaylist *list, int32_t index)
{
  if (!list) {
    return 0;
  }
  if (index < 0) {
    list->current = -1;
    list->resume_pos = -1;
    return 1;
  }
  if ((uint32_t)index >= list->count) {
    return 0;
  }
  list->current = index;
  list->resume_pos = -1;
  return 1;
}

void zzplay_playlist_set_repeat(ZZPlayPlaylist *list, ZZPlayRepeat repeat)
{
  if (!list) {
    return;
  }
  list->repeat = repeat;
}

void zzplay_playlist_set_shuffle(ZZPlayPlaylist *list, int shuffle,
                                 uint32_t seed)
{
  uint32_t k;
  int norm;

  if (!list) {
    return;
  }
  if (seed != 0U) {
    list->rng = seed;
  }
  norm = shuffle ? 1 : 0;
  if (list->shuffle == norm && (seed == 0U || !norm)) {
    return;
  }
  list->shuffle = norm;
  /* A new order invalidates a remembered resume position. */
  list->resume_pos = -1;

  if (!norm) {
    for (k = 0U; k < list->count; k++) {
      list->order[k] = k;
    }
    return;
  }

  if (list->count == 0U) {
    return;
  }

  if (list->current >= 0 && (uint32_t)list->current < list->count) {
    uint32_t fill = 1U;
    list->order[0] = (uint32_t)list->current;
    for (k = 0U; k < list->count; k++) {
      if (k != (uint32_t)list->current) {
        list->order[fill++] = k;
      }
    }
    zzplay_fisher_yates(list->order, 1U, list->count, &list->rng);
  } else {
    for (k = 0U; k < list->count; k++) {
      list->order[k] = k;
    }
    zzplay_fisher_yates(list->order, 0U, list->count, &list->rng);
  }
}

int32_t zzplay_playlist_next(ZZPlayPlaylist *list, ZZPlayAdvance how)
{
  uint32_t i;
  int any_good = 0;
  int32_t cur_pos = -1;
  uint32_t step;

  if (!list || list->count == 0U) {
    return -1;
  }

  for (i = 0U; i < list->count; i++) {
    if (!list->entries[i].failed) {
      any_good = 1;
      break;
    }
  }
  if (!any_good) {
    return -1;
  }

  if (list->current < 0) {
    uint32_t start = list->resume_pos >= 0 ? (uint32_t)list->resume_pos
                                           : 0U;

    /* After the playing entry was removed, carry on with its successor;
     * otherwise start from the top. The end of the list stops playback
     * unless REPEAT_ALL wraps it. */
    for (step = 0U; step < list->count; step++) {
      uint32_t pos = start + step;
      uint32_t idx;

      if (pos >= list->count) {
        if (list->repeat != ZZPLAY_REPEAT_ALL) {
          return -1;
        }
        pos -= list->count;
      }
      idx = list->order[pos];
      if (!list->entries[idx].failed) {
        return (int32_t)idx;
      }
    }
    return -1;
  }

  if (how == ZZPLAY_ADVANCE_AUTO && list->repeat == ZZPLAY_REPEAT_ONE) {
    if (!list->entries[list->current].failed) {
      return list->current;
    }
  }

  for (i = 0U; i < list->count; i++) {
    if (list->order[i] == (uint32_t)list->current) {
      cur_pos = (int32_t)i;
      break;
    }
  }
  if (cur_pos < 0) {
    cur_pos = 0;
  }

  if ((uint32_t)cur_pos + 1U >= list->count) {
    if (list->repeat != ZZPLAY_REPEAT_ALL) {
      return -1;
    }
    if (list->shuffle && list->count > 1U) {
      uint32_t just_played = (uint32_t)list->current;
      zzplay_fisher_yates(list->order, 0U, list->count, &list->rng);
      if (list->order[0] == just_played) {
        uint32_t other = 1U + (zzplay_xorshift32(&list->rng) % (list->count - 1U));
        uint32_t tmp = list->order[0];
        list->order[0] = list->order[other];
        list->order[other] = tmp;
      }
    }
    cur_pos = -1;
  }

  for (step = 1U; step <= list->count; step++) {
    uint32_t next_pos = (uint32_t)(cur_pos + (int32_t)step);
    uint32_t idx;

    if (next_pos >= list->count) {
      if (list->repeat != ZZPLAY_REPEAT_ALL) {
        return -1;
      }
      next_pos = next_pos % list->count;
    }
    idx = list->order[next_pos];
    if (!list->entries[idx].failed) {
      return (int32_t)idx;
    }
  }
  return -1;
}

int32_t zzplay_playlist_previous(ZZPlayPlaylist *list)
{
  uint32_t i;
  int any_good = 0;
  int32_t cur_pos = -1;
  uint32_t step;

  if (!list || list->count == 0U) {
    return -1;
  }

  for (i = 0U; i < list->count; i++) {
    if (!list->entries[i].failed) {
      any_good = 1;
      break;
    }
  }
  if (!any_good) {
    return -1;
  }

  if (list->current < 0) {
    if (list->resume_pos > 0) {
      /* The playing entry was removed: previous is the entry before the
       * position it occupied. */
      for (step = 1U; step <= (uint32_t)list->resume_pos; step++) {
        uint32_t idx = list->order[(uint32_t)list->resume_pos - step];
        if (!list->entries[idx].failed) {
          return (int32_t)idx;
        }
      }
    }
    for (i = 0U; i < list->count; i++) {
      uint32_t idx = list->order[i];
      if (!list->entries[idx].failed) {
        return (int32_t)idx;
      }
    }
    return -1;
  }

  for (i = 0U; i < list->count; i++) {
    if (list->order[i] == (uint32_t)list->current) {
      cur_pos = (int32_t)i;
      break;
    }
  }
  if (cur_pos < 0) {
    cur_pos = 0;
  }

  if (cur_pos == 0) {
    if (list->repeat == ZZPLAY_REPEAT_ALL) {
      for (step = 1U; step <= list->count; step++) {
        uint32_t prev_pos = list->count - step;
        uint32_t idx = list->order[prev_pos];
        if (!list->entries[idx].failed) {
          return (int32_t)idx;
        }
      }
      return -1;
    }
    if (!list->entries[list->current].failed) {
      return list->current;
    }
    for (i = 0U; i < list->count; i++) {
      uint32_t idx = list->order[i];
      if (!list->entries[idx].failed) {
        return (int32_t)idx;
      }
    }
    return -1;
  }

  for (step = 1U; step <= list->count; step++) {
    int32_t prev_pos = cur_pos - (int32_t)step;
    uint32_t idx;

    if (prev_pos < 0) {
      if (list->repeat != ZZPLAY_REPEAT_ALL) {
        break;
      }
      prev_pos = (int32_t)list->count + prev_pos;
    }
    idx = list->order[prev_pos];
    if (!list->entries[idx].failed) {
      return (int32_t)idx;
    }
  }

  /* Nothing playable before the current entry: restart it, or, if it is
   * itself failed, fall forward to the first playable entry so -1 stays
   * reserved for an empty (or wholly failed) list. */
  if (!list->entries[list->current].failed) {
    return list->current;
  }
  for (i = 0U; i < list->count; i++) {
    uint32_t idx = list->order[i];
    if (!list->entries[idx].failed) {
      return (int32_t)idx;
    }
  }
  return -1;
}

void zzplay_playlist_mark_failed(ZZPlayPlaylist *list, uint32_t index,
                                 int failed)
{
  if (!list || index >= list->count) {
    return;
  }
  list->entries[index].failed = failed ? 1U : 0U;
}

void zzplay_playlist_clear_failed(ZZPlayPlaylist *list)
{
  uint32_t i;

  if (!list) {
    return;
  }
  for (i = 0U; i < list->count; i++) {
    list->entries[i].failed = 0U;
  }
}

int zzplay_playlist_set_title(ZZPlayPlaylist *list, uint32_t index,
                              const char *title)
{
  if (!list || index >= list->count) {
    return 0;
  }
  if (!title) {
    list->entries[index].title[0] = '\0';
    return 1;
  }
  strncpy(list->entries[index].title, title,
          sizeof(list->entries[index].title) - 1U);
  list->entries[index].title[sizeof(list->entries[index].title) - 1U] = '\0';
  return 1;
}

const char *zzplay_playlist_basename(const char *path)
{
  const char *colon;
  const char *slash;
  const char *sep;

  if (!path) {
    return "";
  }
  colon = strrchr(path, ':');
  slash = strrchr(path, '/');
  sep = (colon > slash) ? colon : slash;
  return sep ? (sep + 1) : path;
}

const char *zzplay_playlist_display_name(const ZZPlayPlaylist *list,
                                         uint32_t index)
{
  if (!list || index >= list->count) {
    return "";
  }
  if (list->entries[index].title[0] != '\0') {
    return list->entries[index].title;
  }
  return zzplay_playlist_basename(list->entries[index].path);
}

int zzplay_playlist_join(char *out, size_t capacity, const char *drawer,
                         const char *name)
{
  size_t dlen;
  size_t nlen;
  int need_sep = 0;
  size_t total;

  if (!out || capacity == 0U || !name) {
    return 0;
  }
  if (strchr(name, ':') != NULL) {
    nlen = strlen(name);
    if (nlen >= capacity) {
      return 0;
    }
    memcpy(out, name, nlen + 1U);
    return 1;
  }
  if (!drawer || drawer[0] == '\0') {
    nlen = strlen(name);
    if (nlen >= capacity) {
      return 0;
    }
    memcpy(out, name, nlen + 1U);
    return 1;
  }

  dlen = strlen(drawer);
  nlen = strlen(name);
  if (drawer[dlen - 1U] != ':' && drawer[dlen - 1U] != '/') {
    need_sep = 1;
  }
  total = dlen + (need_sep ? 1U : 0U) + nlen;
  if (total >= capacity) {
    return 0;
  }

  memcpy(out, drawer, dlen);
  if (need_sep) {
    out[dlen] = '/';
    memcpy(out + dlen + 1U, name, nlen + 1U);
  } else {
    memcpy(out + dlen, name, nlen + 1U);
  }
  return 1;
}

void zzplay_playlist_drawer(char *out, size_t capacity, const char *path)
{
  const char *colon;
  const char *slash;
  const char *sep;
  size_t len;

  if (!out || capacity == 0U) {
    return;
  }
  if (!path || !*path) {
    out[0] = '\0';
    return;
  }
  colon = strrchr(path, ':');
  slash = strrchr(path, '/');
  sep = (colon > slash) ? colon : slash;
  if (!sep) {
    out[0] = '\0';
    return;
  }

  if (sep == colon) {
    len = (size_t)(sep - path) + 1U;
  } else {
    len = (size_t)(sep - path);
  }
  if (len >= capacity) {
    len = capacity - 1U;
  }
  memcpy(out, path, len);
  out[len] = '\0';
}

uint32_t zzplay_playlist_parse_m3u(ZZPlayPlaylist *list, const char *text,
                                   size_t length, const char *base_drawer)
{
  const char *p;
  const char *end;
  uint32_t added = 0U;
  int pending_extinf = 0;
  char pending_title[ZZPLAY_PLAYLIST_TITLE_MAX];
  uint32_t pending_duration_ms = 0U;

  if (!list || !text || length == 0U) {
    return 0U;
  }

  p = text;
  end = text + length;

  /* Skip UTF-8 BOM if present. */
  if (length >= 3U && (uint8_t)p[0] == 0xEFU && (uint8_t)p[1] == 0xBBU &&
      (uint8_t)p[2] == 0xBFU) {
    p += 3;
  }

  pending_title[0] = '\0';

  while (p < end) {
    const char *line_start = p;
    const char *line_end = p;
    size_t line_len;
    char line_buf[ZZPLAY_PLAYLIST_PATH_MAX];

    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
      line_end++;
    }
    p = line_end;
    if (p < end && *p == '\r') {
      p++;
    }
    if (p < end && *p == '\n') {
      p++;
    }

    line_len = (size_t)(line_end - line_start);
    while (line_len > 0U && (line_start[line_len - 1U] == ' ' ||
                             line_start[line_len - 1U] == '\t')) {
      line_len--;
    }
    while (line_len > 0U && (*line_start == ' ' || *line_start == '\t')) {
      line_start++;
      line_len--;
    }
    if (line_len == 0U) {
      continue;
    }

    if (line_start[0] == '#') {
      if (line_len >= 8U && strncmp(line_start, "#EXTINF:", 8U) == 0) {
        /* Parse inside the line only: the input is (text, length) and need
         * not be NUL-terminated, so strtol() could run past its end. */
        const char *line_stop = line_start + line_len;
        const char *scan = line_start + 8;
        uint32_t sec = 0U;
        int negative = 0;
        int digits = 0;

        if (scan < line_stop && *scan == '-') {
          negative = 1;
          scan++;
        }
        while (scan < line_stop && *scan >= '0' && *scan <= '9') {
          uint32_t digit = (uint32_t)(*scan - '0');

          /* Saturate at the largest whole second a uint32 ms holds. */
          sec = sec > (UINT32_MAX / 1000U - digit) / 10U
                    ? UINT32_MAX / 1000U
                    : sec * 10U + digit;
          digits = 1;
          scan++;
        }
        if (scan < line_stop && *scan == ',') {
          const char *title_str = scan + 1;
          size_t tlen = (size_t)(line_stop - title_str);
          if (tlen >= sizeof(pending_title)) {
            tlen = sizeof(pending_title) - 1U;
          }
          memcpy(pending_title, title_str, tlen);
          pending_title[tlen] = '\0';
        } else {
          pending_title[0] = '\0';
        }
        pending_duration_ms =
            (digits && !negative && sec > 0U) ? sec * 1000U : 0U;
        pending_extinf = 1;
      }
      continue;
    }

    if (line_len >= sizeof(line_buf)) {
      line_len = sizeof(line_buf) - 1U;
    }
    memcpy(line_buf, line_start, line_len);
    line_buf[line_len] = '\0';

    {
      char resolved[ZZPLAY_PLAYLIST_PATH_MAX];
      int32_t idx;

      if (!zzplay_playlist_join(resolved, sizeof(resolved), base_drawer,
                                line_buf)) {
        pending_extinf = 0;
        pending_title[0] = '\0';
        pending_duration_ms = 0U;
        continue;
      }

      idx = zzplay_playlist_add(list, resolved);
      if (idx >= 0) {
        if (pending_extinf) {
          if (pending_title[0] != '\0') {
            zzplay_playlist_set_title(list, (uint32_t)idx, pending_title);
          }
          list->entries[idx].duration_ms = pending_duration_ms;
        }
        added++;
      }
    }

    pending_extinf = 0;
    pending_title[0] = '\0';
    pending_duration_ms = 0U;
  }

  return added;
}

size_t zzplay_playlist_format_m3u(const ZZPlayPlaylist *list, char *out,
                                  size_t capacity)
{
  size_t needed = 0U;
  size_t written = 0U;
  uint32_t i;

  if (!list) {
    if (out && capacity > 0U) {
      out[0] = '\0';
    }
    return 0U;
  }

  {
    static const char header[] = "#EXTM3U\n";
    size_t hlen = sizeof(header) - 1U;
    needed += hlen;
    if (out && capacity > written) {
      size_t copy = (capacity - written > hlen) ? hlen : (capacity - written - 1U);
      memcpy(out + written, header, copy);
      written += copy;
    }
  }

  for (i = 0U; i < list->count; i++) {
    const ZZPlayPlaylistEntry *e = &list->entries[i];
    int has_title = (e->title[0] != '\0');
    int has_dur = (e->duration_ms > 0U);

    if (has_title || has_dur) {
      char extinf[ZZPLAY_PLAYLIST_TITLE_MAX + 32U];
      int sec = has_dur ? (int)(e->duration_ms / 1000U) : -1;
      int elen = snprintf(extinf, sizeof(extinf), "#EXTINF:%d,%s\n", sec,
                          e->title);
      if (elen > 0) {
        size_t ulen = (size_t)elen;
        needed += ulen;
        if (out && capacity > written + 1U) {
          size_t avail = capacity - written - 1U;
          size_t copy = (avail < ulen) ? avail : ulen;
          memcpy(out + written, extinf, copy);
          written += copy;
        }
      }
    }

    {
      size_t plen = strlen(e->path);
      needed += plen + 1U;
      if (out && capacity > written + 1U) {
        size_t avail = capacity - written - 1U;
        size_t copy = (avail < plen) ? avail : plen;
        memcpy(out + written, e->path, copy);
        written += copy;
        if (written + 1U < capacity) {
          out[written++] = '\n';
        }
      }
    }
  }

  if (out && capacity > 0U) {
    out[written] = '\0';
  }
  return needed;
}

uint32_t zzplay_playlist_load_m3u(ZZPlayPlaylist *list, const char *path)
{
  FILE *f;
  long sz;
  size_t len;
  char *buf;
  char drawer[ZZPLAY_PLAYLIST_PATH_MAX];
  uint32_t added;

  if (!list || !path) {
    return 0U;
  }
  f = fopen(path, "rb");
  if (!f) {
    return 0U;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0U;
  }
  sz = ftell(f);
  if (sz <= 0 || sz > 10 * 1024 * 1024) {
    fclose(f);
    return 0U;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return 0U;
  }
  len = (size_t)sz;
  buf = (char *)malloc(len + 1U);
  if (!buf) {
    fclose(f);
    return 0U;
  }
  if (fread(buf, 1U, len, f) != len) {
    free(buf);
    fclose(f);
    return 0U;
  }
  fclose(f);
  buf[len] = '\0';

  zzplay_playlist_drawer(drawer, sizeof(drawer), path);
  added = zzplay_playlist_parse_m3u(list, buf, len, drawer);
  free(buf);
  return added;
}

int zzplay_playlist_save_m3u(const ZZPlayPlaylist *list, const char *path)
{
  size_t needed;
  char *buf;
  FILE *f;
  size_t written;

  if (!list || !path) {
    return 0;
  }
  needed = zzplay_playlist_format_m3u(list, NULL, 0U);
  buf = (char *)malloc(needed + 1U);
  if (!buf) {
    return 0;
  }
  zzplay_playlist_format_m3u(list, buf, needed + 1U);

  f = fopen(path, "wb");
  if (!f) {
    free(buf);
    return 0;
  }
  written = fwrite(buf, 1U, needed, f);
  fclose(f);
  free(buf);
  return (written == needed) ? 1 : 0;
}

int zzplay_playlist_is_playlist_path(const char *path)
{
  const char *dot;

  if (!path) {
    return 0;
  }
  dot = strrchr(path, '.');
  if (!dot) {
    return 0;
  }
  dot++;
  if ((dot[0] == 'm' || dot[0] == 'M') &&
      (dot[1] == '3' || dot[1] == '3') &&
      (dot[2] == 'u' || dot[2] == 'U')) {
    if (dot[3] == '\0') {
      return 1;
    }
    if ((dot[3] == '8' || dot[3] == '8') && dot[4] == '\0') {
      return 1;
    }
  }
  return 0;
}
