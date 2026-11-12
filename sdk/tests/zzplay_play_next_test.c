/* Tests for Browse's Play next: inserting entries right after the playing
 * one (zzplay_playlist_insert_after, zzplay_playlist_next_anchor).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-playlist.h"

static void check_permutation(const ZZPlayPlaylist *list)
{
  uint8_t *seen;
  uint32_t i;

  if (list->count == 0U) {
    return;
  }
  seen = (uint8_t *)calloc(list->count, 1U);
  assert(seen != NULL);
  for (i = 0U; i < list->count; i++) {
    assert(list->order[i] < list->count);
    assert(!seen[list->order[i]]);
    seen[list->order[i]] = 1U;
  }
  free(seen);
}

static void fill(ZZPlayPlaylist *list, const char *const *paths, uint32_t n)
{
  uint32_t i;

  for (i = 0U; i < n; i++) {
    assert(zzplay_playlist_add(list, paths[i]) == (int32_t)i);
  }
}

static const char *path_at(const ZZPlayPlaylist *list, int32_t index)
{
  assert(index >= 0 && (uint32_t)index < list->count);
  return list->entries[index].path;
}

/* An album queued behind the playing track keeps its order, comes next,
 * and the playing track keeps its index. */
static void test_insert_after_playing(void)
{
  static const char *const paths[] = {"a", "b", "c"};
  ZZPlayPlaylist list;
  int32_t anchor;
  int32_t idx;

  zzplay_playlist_init(&list);
  fill(&list, paths, 3U);
  assert(zzplay_playlist_set_current(&list, 1));

  anchor = zzplay_playlist_next_anchor(&list, 1);
  assert(anchor == 1);
  idx = zzplay_playlist_insert_after(&list, anchor, "x");
  assert(idx == 2);
  idx = zzplay_playlist_insert_after(&list, idx, "y");
  assert(idx == 3);

  assert(list.count == 5U);
  assert(list.current == 1);
  assert(strcmp(path_at(&list, 0), "a") == 0);
  assert(strcmp(path_at(&list, 1), "b") == 0);
  assert(strcmp(path_at(&list, 2), "x") == 0);
  assert(strcmp(path_at(&list, 3), "y") == 0);
  assert(strcmp(path_at(&list, 4), "c") == 0);
  /* Unshuffled play order stays the entry order. */
  for (idx = 0; idx < 5; idx++) {
    assert(list.order[idx] == (uint32_t)idx);
  }
  assert(zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) == 2);
  zzplay_playlist_free(&list);
}

/* Nothing playing: the entries go to the start; a current entry that is
 * only remembered keeps pointing at the same entry. */
static void test_insert_when_nothing_plays(void)
{
  static const char *const paths[] = {"a", "b"};
  ZZPlayPlaylist list;
  int32_t idx;

  zzplay_playlist_init(&list);
  fill(&list, paths, 2U);
  assert(zzplay_playlist_set_current(&list, 1));

  assert(zzplay_playlist_next_anchor(&list, 0) == -1);
  idx = zzplay_playlist_insert_after(&list, -1, "x");
  assert(idx == 0);
  idx = zzplay_playlist_insert_after(&list, idx, "y");
  assert(idx == 1);
  assert(strcmp(path_at(&list, 0), "x") == 0);
  assert(strcmp(path_at(&list, 1), "y") == 0);
  assert(strcmp(path_at(&list, 2), "a") == 0);
  assert(list.current == 3);
  assert(strcmp(path_at(&list, list.current), "b") == 0);
  check_permutation(&list);

  /* An empty list takes the entry as its first. */
  zzplay_playlist_clear(&list);
  assert(zzplay_playlist_next_anchor(&list, 1) == -1);
  assert(zzplay_playlist_insert_after(&list, -1, "z") == 0);
  assert(list.count == 1U && list.current == -1);
  zzplay_playlist_free(&list);
}

/* Shuffled: the new entries play straight after the current one in play
 * order, whatever the shuffle drew. */
static void test_insert_shuffled(void)
{
  static const char *const paths[] = {"a", "b", "c", "d", "e", "f"};
  ZZPlayPlaylist list;
  int32_t cur;
  int32_t x;
  int32_t y;
  uint32_t k;
  uint32_t pos = 0U;

  zzplay_playlist_init(&list);
  fill(&list, paths, 6U);
  zzplay_playlist_set_shuffle(&list, 1, 77U);
  cur = (int32_t)list.order[2];
  assert(zzplay_playlist_set_current(&list, cur));

  x = zzplay_playlist_insert_after(&list, zzplay_playlist_next_anchor(&list, 1),
                                   "x");
  y = zzplay_playlist_insert_after(&list, x, "y");
  assert(x >= 0 && y >= 0);
  check_permutation(&list);
  assert(strcmp(path_at(&list, list.current), paths[cur]) == 0);
  for (k = 0U; k < list.count; k++) {
    if (list.order[k] == (uint32_t)list.current) {
      pos = k;
    }
  }
  assert(list.order[pos + 1U] == (uint32_t)x);
  assert(list.order[pos + 2U] == (uint32_t)y);
  assert(zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) == x);
  zzplay_playlist_free(&list);
}

/* The playing entry was removed from the list while it plays: Play next
 * queues behind the place play continues from, so the new entry is
 * what plays when the removed one ends. */
static void test_insert_after_removed_playing_entry(void)
{
  static const char *const paths[] = {"a", "b", "c"};
  ZZPlayPlaylist list;
  int32_t anchor;
  int32_t idx;
  int32_t next;

  zzplay_playlist_init(&list);
  fill(&list, paths, 3U);
  assert(zzplay_playlist_set_current(&list, 1));
  assert(zzplay_playlist_remove(&list, 1U));
  assert(list.current == -1);

  anchor = zzplay_playlist_next_anchor(&list, 1);
  assert(anchor == 0);
  idx = zzplay_playlist_insert_after(&list, anchor, "x");
  assert(idx == 1);
  next = zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO);
  assert(next == idx);

  /* Removed when it was first: the new entry goes to the start and still
   * plays next. */
  zzplay_playlist_clear(&list);
  fill(&list, paths, 3U);
  assert(zzplay_playlist_set_current(&list, 0));
  assert(zzplay_playlist_remove(&list, 0U));
  assert(zzplay_playlist_next_anchor(&list, 1) == -1);
  idx = zzplay_playlist_insert_after(&list, -1, "x");
  assert(idx == 0);
  assert(zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) == 0);
  zzplay_playlist_free(&list);
}

/* A full list takes nothing more and stays as it was; bad anchors are
 * refused. */
static void test_capacity_and_bad_anchor(void)
{
  ZZPlayPlaylist list;
  char path[16];
  uint32_t i;

  zzplay_playlist_init(&list);
  for (i = 0U; i + 1U < ZZPLAY_PLAYLIST_MAX_ENTRIES; i++) {
    (void)snprintf(path, sizeof(path), "t%lu", (unsigned long)i);
    assert(zzplay_playlist_add(&list, path) >= 0);
  }
  assert(zzplay_playlist_set_current(&list, 5));
  assert(zzplay_playlist_insert_after(&list, 5, "last") == 6);
  assert(list.count == ZZPLAY_PLAYLIST_MAX_ENTRIES);
  assert(zzplay_playlist_insert_after(&list, 6, "over") == -1);
  assert(list.count == ZZPLAY_PLAYLIST_MAX_ENTRIES);
  assert(list.current == 5);
  assert(strcmp(path_at(&list, 6), "last") == 0);
  assert(strcmp(path_at(&list, 7), "t6") == 0);

  zzplay_playlist_clear(&list);
  assert(zzplay_playlist_add(&list, "a") == 0);
  assert(zzplay_playlist_insert_after(&list, 1, "x") == -1);
  assert(zzplay_playlist_insert_after(&list, -2, "x") == -1);
  assert(zzplay_playlist_insert_after(&list, 0, "") == -1);
  assert(list.count == 1U);
  zzplay_playlist_free(&list);
}

int main(void)
{
  test_insert_after_playing();
  test_insert_when_nothing_plays();
  test_insert_shuffled();
  test_insert_after_removed_playing_entry();
  test_capacity_and_bad_anchor();
  printf("zzplay_play_next_test: all tests passed\n");
  return 0;
}
