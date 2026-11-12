/* Behavioural tests for the Browse window's pure row helpers: duration
 * text, column widths and the clamp of a remembered window.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-browse-rows.h"

static int test_format_duration(void)
{
  char buf[16];

  /* Unknown stays empty */
  zzplay_browse_format_duration(0U, buf, sizeof(buf));
  if (strcmp(buf, "") != 0) return 1;

  /* Under a minute, padded seconds */
  zzplay_browse_format_duration(59000U, buf, sizeof(buf));
  if (strcmp(buf, "0:59") != 0) return 2;

  /* Rounded to the nearest second */
  zzplay_browse_format_duration(61400U, buf, sizeof(buf));
  if (strcmp(buf, "1:01") != 0) return 3;
  zzplay_browse_format_duration(60499U, buf, sizeof(buf));
  if (strcmp(buf, "1:00") != 0) return 4;

  zzplay_browse_format_duration(3599000U, buf, sizeof(buf));
  if (strcmp(buf, "59:59") != 0) return 5;

  zzplay_browse_format_duration(3600000U, buf, sizeof(buf));
  if (strcmp(buf, "1:00:00") != 0) return 6;

  zzplay_browse_format_duration(3661000U, buf, sizeof(buf));
  if (strcmp(buf, "1:01:01") != 0) return 7;

  /* Longer than 99:59:59 clamps instead of overflowing the column */
  zzplay_browse_format_duration(ZZPLAY_BROWSE_MS_MAX + 123456U, buf,
                                sizeof(buf));
  if (strcmp(buf, "99:59:59") != 0) return 8;

  /* A tiny buffer still yields a terminated string */
  zzplay_browse_format_duration(3661000U, buf, 4U);
  if (strlen(buf) >= 4U) return 9;

  return 0;
}

static int test_layout_columns(void)
{
  ZZPlayBrowseColumns cols;

  /* A comfortable row: length stays fixed at the right edge, the title
   * gets about 55 percent of the rest. */
  zzplay_browse_layout_columns(200, 12, 50, 6, &cols);
  if (cols.marker_x != 2) return 1;
  if (cols.len_x + cols.len_w != 198) return 2;
  if (cols.title_x != 2 + 12 + 6) return 3;
  {
    int rest = cols.len_x - 6 - cols.title_x;
    int share = (rest * 55) / 100;
    if (cols.title_w != share) return 4;
    if (cols.sub_x + cols.sub_w != cols.len_x - 6) return 5;
  }

  /* Narrower rows shrink both text cells, not the length cell */
  {
    ZZPlayBrowseColumns wide;
    zzplay_browse_layout_columns(400, 12, 50, 6, &wide);
    zzplay_browse_layout_columns(120, 12, 50, 6, &cols);
    if (cols.len_w != wide.len_w) return 6;
    if (cols.title_w >= wide.title_w) return 7;
    if (cols.title_w < 0 || cols.sub_w < 0) return 8;
  }

  /* A row too narrow for every cell clips without going negative or
   * overlapping the length cell. */
  zzplay_browse_layout_columns(30, 12, 50, 6, &cols);
  if (cols.title_w < 0 || cols.sub_w < 0 || cols.len_w < 0) return 9;
  if (cols.sub_x + cols.sub_w > cols.len_x) return 10;
  if (cols.len_x + cols.len_w > 28) return 11;

  return 0;
}

static int test_clamp_window(void)
{
  int left, top, w, h;

  /* A remembered window from a larger screen shrinks to this one and
   * comes back on it. */
  left = 800;
  top = 600;

  w = 640;
  h = 480;
  zzplay_browse_clamp_window(&left, &top, &w, &h, 640, 256, 200, 120);
  if (w != 640 || h != 256) return 1;
  if (left != 0 || top != 0) return 2;

  /* A size below the minimum grows to it */
  left = 10;
  top = 10;
  w = 80;
  h = 60;
  zzplay_browse_clamp_window(&left, &top, &w, &h, 640, 256, 200, 120);
  if (w != 200 || h != 120) return 3;

  /* A place off the right edge comes back so the whole window shows */
  left = 700;
  top = 10;
  w = 300;
  h = 120;
  zzplay_browse_clamp_window(&left, &top, &w, &h, 640, 256, 200, 120);
  if (left != 340) return 4;
  if (top != 10) return 5;

  /* Negative places move onto the screen */
  left = -50;
  top = -20;
  w = 300;
  h = 120;
  zzplay_browse_clamp_window(&left, &top, &w, &h, 640, 256, 200, 120);
  if (left != 0 || top != 0) return 6;

  /* A minimum larger than the screen loses to the screen */
  left = 0;
  top = 0;
  w = 800;
  h = 400;
  zzplay_browse_clamp_window(&left, &top, &w, &h, 640, 256, 900, 900);
  if (w != 640 || h != 256) return 7;

  /* A window that fits is not touched */
  left = 100;
  top = 50;
  w = 300;
  h = 150;
  zzplay_browse_clamp_window(&left, &top, &w, &h, 640, 256, 200, 120);
  if (left != 100 || top != 50 || w != 300 || h != 150) return 8;

  return 0;
}

static int test_count_playable(void)
{
  ZZPlayBrowseItem items[4];

  memset(items, 0, sizeof(items));
  snprintf(items[0].id, sizeof(items[0].id), "alb:1");
  items[0].kind = ZZPLAY_ITEM_FOLDER;
  items[0].playable = 1U;
  snprintf(items[1].id, sizeof(items[1].id), "201");
  items[1].kind = ZZPLAY_ITEM_TRACK;
  items[1].playable = 1U;
  snprintf(items[2].id, sizeof(items[2].id), "202");
  items[2].kind = ZZPLAY_ITEM_TRACK;
  items[2].playable = 0U;
  snprintf(items[3].id, sizeof(items[3].id), "radio:1");
  items[3].kind = ZZPLAY_ITEM_STATION;
  items[3].playable = 1U;

  if (zzplay_browse_count_playable(items, 4U) != 2U) return 1;
  if (zzplay_browse_count_playable(items, 0U) != 0U) return 2;
  if (zzplay_browse_count_playable(NULL, 4U) != 0U) return 3;

  return 0;
}


int main(void)
{
  int failures = 0;

  if (test_format_duration() != 0) {
    failures++;
    printf("fail: duration formatting\n");
  }
  if (test_layout_columns() != 0) {
    failures++;
    printf("fail: column layout\n");
  }
  if (test_clamp_window() != 0) {
    failures++;
    printf("fail: window clamp\n");
  }
  if (test_count_playable() != 0) {
    failures++;
    printf("fail: count playable\n");
  }
  if (failures == 0) {
    printf("zzplay_browse_rows_test: ok\n");
    return 0;
  }
  printf("zzplay_browse_rows_test: %d failure(s)\n", failures);
  return 1;
}
