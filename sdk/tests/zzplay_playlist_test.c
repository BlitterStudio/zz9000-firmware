/* Behavioural tests for playlist model and M3U (zzplay-playlist.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-playlist.h"

static int check_permutation(const ZZPlayPlaylist *list)
{
  uint32_t i;
  uint8_t *seen;

  if (list->count == 0U) {
    return 1;
  }
  seen = (uint8_t *)calloc(list->count, 1U);
  if (!seen) {
    return 0;
  }
  for (i = 0U; i < list->count; i++) {
    uint32_t v = list->order[i];
    if (v >= list->count || seen[v]) {
      free(seen);
      return 0;
    }
    seen[v] = 1U;
  }
  free(seen);
  return 1;
}

static int test_navigation_boundaries(void)
{
  ZZPlayPlaylist list;

  zzplay_playlist_init(&list);
  zzplay_playlist_add(&list, "track1.mp3");
  zzplay_playlist_add(&list, "track2.mp3");
  zzplay_playlist_add(&list, "track3.mp3");

  /* --- REPEAT_OFF --- */
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_OFF);
  zzplay_playlist_set_current(&list, 0);

  /* next: 0 -> 1 -> 2 -> -1 */
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 1) return 1;
  zzplay_playlist_set_current(&list, 1);
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 2) return 2;
  zzplay_playlist_set_current(&list, 2);
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != -1) return 3;

  /* prev: 2 -> 1 -> 0 -> 0 (restart) */
  if (zzplay_playlist_previous(&list) != 1) return 4;
  zzplay_playlist_set_current(&list, 1);
  if (zzplay_playlist_previous(&list) != 0) return 5;
  zzplay_playlist_set_current(&list, 0);
  if (zzplay_playlist_previous(&list) != 0) return 6;

  /* --- REPEAT_ONE --- */
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_ONE);
  zzplay_playlist_set_current(&list, 1);
  /* AUTO advance returns the same entry */
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 1) return 7;
  /* USER advance ignores REPEAT_ONE and goes to next */
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_USER) != 2) return 8;

  /* --- REPEAT_ALL --- */
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_ALL);
  zzplay_playlist_set_current(&list, 2);
  /* Wraps to 0 at end */
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 0) return 9;
  zzplay_playlist_set_current(&list, 0);
  /* Prev at start wraps to last (2) */
  if (zzplay_playlist_previous(&list) != 2) return 10;

  zzplay_playlist_free(&list);
  return 0;
}

/* Removing the entry that is playing must not send auto-advance back to
 * the top of the list: playback continues with the removed entry's
 * successor, previous goes to its predecessor, and the end still stops
 * unless repeat-all wraps. */
static int test_remove_playing_entry(void)
{
  static const char *const names[] = { "a", "b", "c", "d" };
  ZZPlayPlaylist list;
  uint32_t i;

  zzplay_playlist_init(&list);
  for (i = 0U; i < 4U; i++) {
    zzplay_playlist_add(&list, names[i]);
  }

  /* Middle: playing "b", remove it -> next is "c", previous is "a". */
  zzplay_playlist_set_current(&list, 1);
  if (!zzplay_playlist_remove(&list, 1U)) return 1;
  if (list.current != -1) return 2;
  if (strcmp(list.entries[zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO)]
                 .path, "c") != 0) return 3;
  if (strcmp(list.entries[zzplay_playlist_previous(&list)].path, "a") != 0)
    return 4;
  /* Removing an earlier entry keeps the resume point on "c". */
  if (!zzplay_playlist_remove(&list, 0U)) return 5;
  if (strcmp(list.entries[zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO)]
                 .path, "c") != 0) return 6;

  /* First: list is now c,d; playing "c" removed -> next is "d". */
  zzplay_playlist_set_current(&list, 0);
  zzplay_playlist_remove(&list, 0U);
  if (strcmp(list.entries[zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO)]
                 .path, "d") != 0) return 7;

  /* Last: playing the last entry removed -> stop, or wrap under ALL. */
  zzplay_playlist_clear(&list);
  for (i = 0U; i < 4U; i++) {
    zzplay_playlist_add(&list, names[i]);
  }
  zzplay_playlist_set_current(&list, 3);
  zzplay_playlist_remove(&list, 3U);
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != -1) return 8;
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_ALL);
  if (strcmp(list.entries[zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO)]
                 .path, "a") != 0) return 9;

  /* Shuffled: the successor is the next entry in play order. */
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_OFF);
  zzplay_playlist_set_shuffle(&list, 1, 0xC0FFEEU);
  {
    uint32_t playing = list.order[1];
    uint32_t successor = list.order[2];
    const char *expect = list.entries[successor].path;

    zzplay_playlist_set_current(&list, (int32_t)playing);
    zzplay_playlist_remove(&list, playing);
    if (!check_permutation(&list)) return 10;
    if (strcmp(list.entries[zzplay_playlist_next(&list,
                                                 ZZPLAY_ADVANCE_AUTO)].path,
               expect) != 0) return 11;
  }

  zzplay_playlist_free(&list);
  return 0;
}

static int test_shuffle_permutations(void)
{
  ZZPlayPlaylist list;
  uint32_t i;
  int32_t cur_val;

  zzplay_playlist_init(&list);
  zzplay_playlist_add(&list, "a.mp3");
  zzplay_playlist_add(&list, "b.mp3");
  zzplay_playlist_add(&list, "c.mp3");
  zzplay_playlist_add(&list, "d.mp3");
  zzplay_playlist_add(&list, "e.mp3");

  zzplay_playlist_set_current(&list, 2); /* 'c.mp3' */

  /* Turn shuffle on with seed */
  zzplay_playlist_set_shuffle(&list, 1, 0xABCDEF12U);
  if (!check_permutation(&list)) return 1;
  /* Current entry must be first in play order */
  if (list.order[0] != 2U) return 2;

  /* Add entry while shuffled: must be inserted after current in play order */
  cur_val = list.current;
  zzplay_playlist_add(&list, "f.mp3"); /* index 5 */
  if (!check_permutation(&list)) return 3;
  if (list.current != cur_val) return 4;
  /* 'f' (index 5) must appear after current (index 2) in order */
  {
    int seen_cur = 0;
    int f_after_cur = 0;
    for (i = 0U; i < list.count; i++) {
      if (list.order[i] == (uint32_t)cur_val) seen_cur = 1;
      if (seen_cur && list.order[i] == 5U) f_after_cur = 1;
    }
    if (!f_after_cur) return 5;
  }

  /* Remove an entry (index 1: 'b.mp3') */
  if (!zzplay_playlist_remove(&list, 1)) return 6;
  if (!check_permutation(&list)) return 7;
  /* current was 2, now 1 since 1 < 2 was removed */
  if (list.current != 1) return 8;

  /* Move an entry (from 0 to 3) */
  if (!zzplay_playlist_move(&list, 0, 3)) return 9;
  if (!check_permutation(&list)) return 10;

  /* Test wrap under REPEAT_ALL: reshuffles and avoids immediate repeat of last played */
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_ALL);
  /* Set current to the last entry in play order */
  zzplay_playlist_set_current(&list, (int32_t)list.order[list.count - 1U]);
  {
    int32_t last_played = list.current;
    int32_t wrapped_to = zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO);
    if (wrapped_to < 0) return 11;
    if (wrapped_to == last_played) return 12; /* Avoid immediate repeat */
    if (!check_permutation(&list)) return 13;
  }

  /* Turning shuffle off restores entry order */
  zzplay_playlist_set_shuffle(&list, 0, 0);
  for (i = 0U; i < list.count; i++) {
    if (list.order[i] != i) return 14;
  }

  zzplay_playlist_free(&list);
  return 0;
}

static int test_failed_skipping(void)
{
  ZZPlayPlaylist list;

  zzplay_playlist_init(&list);
  zzplay_playlist_add(&list, "1.mp3");
  zzplay_playlist_add(&list, "2.mp3");
  zzplay_playlist_add(&list, "3.mp3");

  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_OFF);
  zzplay_playlist_set_current(&list, 0);

  /* Mark track 2 (index 1) as failed */
  zzplay_playlist_mark_failed(&list, 1, 1);
  /* Next from 0 should skip 1 and return 2 */
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 2) return 1;

  /* Mark track 3 (index 2) as failed */
  zzplay_playlist_mark_failed(&list, 2, 1);
  /* Next from 0 under REPEAT_OFF should stop (-1) because 1 and 2 are failed */
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != -1) return 2;

  /* Next from 0 under REPEAT_ALL should wrap back to 0 */
  zzplay_playlist_set_repeat(&list, ZZPLAY_REPEAT_ALL);
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 0) return 3;

  /* Mark all as failed -> returns -1 */
  zzplay_playlist_mark_failed(&list, 0, 1);
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != -1) return 4;
  if (zzplay_playlist_previous(&list) != -1) return 5;

  /* Clear failed */
  zzplay_playlist_clear_failed(&list);
  if (zzplay_playlist_next(&list, ZZPLAY_ADVANCE_AUTO) != 1) return 6;

  zzplay_playlist_free(&list);
  return 0;
}

static int test_join_and_drawer(void)
{
  char buf[128];

  /* zzplay_playlist_join */
  if (!zzplay_playlist_join(buf, sizeof(buf), "Work:", "a") ||
      strcmp(buf, "Work:a") != 0) return 1;

  if (!zzplay_playlist_join(buf, sizeof(buf), "Work:M", "a") ||
      strcmp(buf, "Work:M/a") != 0) return 2;

  if (!zzplay_playlist_join(buf, sizeof(buf), "Work:M/", "a") ||
      strcmp(buf, "Work:M/a") != 0) return 3;

  if (!zzplay_playlist_join(buf, sizeof(buf), "", "a") ||
      strcmp(buf, "a") != 0) return 4;

  if (!zzplay_playlist_join(buf, sizeof(buf), "Work:M", "DH0:Music/b") ||
      strcmp(buf, "DH0:Music/b") != 0) return 5;

  /* Buffer capacity failure */
  if (zzplay_playlist_join(buf, 4, "Work:", "song.mp3") != 0) return 6;

  /* zzplay_playlist_drawer */
  zzplay_playlist_drawer(buf, sizeof(buf), "Work:M/a.m3u");
  if (strcmp(buf, "Work:M") != 0) return 7;

  zzplay_playlist_drawer(buf, sizeof(buf), "Work:a");
  if (strcmp(buf, "Work:") != 0) return 8;

  zzplay_playlist_drawer(buf, sizeof(buf), "M/a");
  if (strcmp(buf, "M") != 0) return 9;

  zzplay_playlist_drawer(buf, sizeof(buf), "a");
  if (strcmp(buf, "") != 0) return 10;

  /* Basename */
  if (strcmp(zzplay_playlist_basename("Work:Music/song.mp3"), "song.mp3") != 0)
    return 11;
  if (strcmp(zzplay_playlist_basename("Work:song.mp3"), "song.mp3") != 0)
    return 12;

  return 0;
}

static int test_m3u_parse_and_format(void)
{
  static const char m3u_text[] =
      "\xEF\xBB\xBF" /* UTF-8 BOM */
      "#EXTM3U\r\n"
      "# A comment line\n"
      "\n"
      "#EXTINF:185,Artist Name - Track One\r\n"
      "track1.mp3\r\n"
      "#EXTINF:-1,Track Two\n"
      "sub/track2.mp3\n"
      "# Track without EXTINF\r"
      "DH0:Absolute/track3.mp3\r\n";

  ZZPlayPlaylist list;
  char out_buf[1024];
  size_t needed;

  zzplay_playlist_init(&list);
  if (zzplay_playlist_parse_m3u(&list, m3u_text, sizeof(m3u_text) - 1U,
                               "Work:Music") != 3U)
    return 1;

  if (list.count != 3U) return 2;

  /* Track 1 */
  if (strcmp(list.entries[0].path, "Work:Music/track1.mp3") != 0) return 3;
  if (strcmp(list.entries[0].title, "Artist Name - Track One") != 0) return 4;
  if (list.entries[0].duration_ms != 185000U) return 5;

  /* Track 2 */
  if (strcmp(list.entries[1].path, "Work:Music/sub/track2.mp3") != 0) return 6;
  if (strcmp(list.entries[1].title, "Track Two") != 0) return 7;
  if (list.entries[1].duration_ms != 0U) return 8;

  /* Track 3 */
  if (strcmp(list.entries[2].path, "DH0:Absolute/track3.mp3") != 0) return 9;
  if (list.entries[2].title[0] != '\0') return 10;

  /* Display names */
  if (strcmp(zzplay_playlist_display_name(&list, 0),
             "Artist Name - Track One") != 0) return 11;
  if (strcmp(zzplay_playlist_display_name(&list, 2), "track3.mp3") != 0)
    return 12;

  /* Format M3U */
  needed = zzplay_playlist_format_m3u(&list, out_buf, sizeof(out_buf));
  if (needed == 0U) return 13;
  if (strstr(out_buf, "#EXTM3U\n") == NULL) return 14;
  if (strstr(out_buf, "#EXTINF:185,Artist Name - Track One\n") == NULL)
    return 15;
  if (strstr(out_buf, "#EXTINF:-1,Track Two\n") == NULL) return 16;
  if (strstr(out_buf, "DH0:Absolute/track3.mp3\n") == NULL) return 17;

  /* Round trip parse of formatted output */
  {
    ZZPlayPlaylist list2;
    zzplay_playlist_init(&list2);
    if (zzplay_playlist_parse_m3u(&list2, out_buf, strlen(out_buf), "") != 3U)
      return 18;
    if (strcmp(list2.entries[0].title, "Artist Name - Track One") != 0)
      return 19;
    if (list2.entries[0].duration_ms != 185000U) return 20;
    zzplay_playlist_free(&list2);
  }

  /* Playlist path check */
  if (!zzplay_playlist_is_playlist_path("list.m3u")) return 21;
  if (!zzplay_playlist_is_playlist_path("LIST.M3U8")) return 22;
  if (zzplay_playlist_is_playlist_path("song.mp3")) return 23;

  zzplay_playlist_free(&list);
  return 0;
}

/* EXTINF values beyond what a uint32 millisecond count holds saturate
 * instead of wrapping to a small bogus duration. */
static int test_m3u_extinf_saturates(void)
{
  static const char huge[] = "#EXTINF:5000000,Long\nlong.mp3\n";
  ZZPlayPlaylist list;

  zzplay_playlist_init(&list);
  if (zzplay_playlist_parse_m3u(&list, huge, sizeof(huge) - 1U, "Work:") !=
      1U) {
    return 1;
  }
  if (list.entries[0].duration_ms != (UINT32_MAX / 1000U) * 1000U) return 2;
  if (strcmp(list.entries[0].title, "Long") != 0) return 3;
  zzplay_playlist_free(&list);
  return 0;
}

int main(void)
{
  int rc;

  rc = test_navigation_boundaries();
  if (rc) {
    fprintf(stderr, "test_navigation_boundaries failed: %d\n", rc);
    return 1;
  }
  rc = test_remove_playing_entry();
  if (rc) {
    fprintf(stderr, "test_remove_playing_entry failed: %d\n", rc);
    return 7;
  }
  rc = test_shuffle_permutations();
  if (rc) {
    fprintf(stderr, "test_shuffle_permutations failed: %d\n", rc);
    return 2;
  }
  rc = test_failed_skipping();
  if (rc) {
    fprintf(stderr, "test_failed_skipping failed: %d\n", rc);
    return 3;
  }
  rc = test_join_and_drawer();
  if (rc) {
    fprintf(stderr, "test_join_and_drawer failed: %d\n", rc);
    return 4;
  }
  rc = test_m3u_parse_and_format();
  if (rc) {
    fprintf(stderr, "test_m3u_parse_and_format failed: %d\n", rc);
    return 5;
  }
  rc = test_m3u_extinf_saturates();
  if (rc) {
    fprintf(stderr, "test_m3u_extinf_saturates failed: %d\n", rc);
    return 6;
  }

  printf("zzplay_playlist_test: all tests passed\n");
  return 0;
}
