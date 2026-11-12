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

static int rects_overlap(const ZZPlayBrowseRect *a, const ZZPlayBrowseRect *b)
{
  if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0) {
    return 0;
  }
  return (a->x < b->x + b->w && a->x + a->w > b->x &&
          a->y < b->y + b->h && a->y + a->h > b->y);
}

static int verify_layout(const ZZPlayBrowseLayout *l,
                         const ZZPlayBrowseLayoutConfig *cfg,
                         int target_w, int target_h)
{
  ZZPlayBrowseRect rects[13];
  int i, j;

  if (!l || !cfg) return 1;
  if (l->inner_w < l->min_inner_w) return 2;
  if (l->inner_h < l->min_inner_h) return 3;
  if (target_w >= l->min_inner_w && l->inner_w != target_w) return 4;
  if (target_w < l->min_inner_w && l->inner_w != l->min_inner_w) return 5;
  if (target_h >= l->min_inner_h && l->inner_h != target_h) return 6;
  if (target_h < l->min_inner_h && l->inner_h != l->min_inner_h) return 7;

  rects[0] = l->service;
  rects[1] = l->search;
  rects[2] = l->list;
  for (i = 0; i < 4; i++) rects[3 + i] = l->nav_buttons[i];
  for (i = 0; i < 5; i++) rects[7 + i] = l->act_buttons[i];
  rects[12] = l->status;

  for (i = 0; i < 13; i++) {
    if (rects[i].w <= 0 || rects[i].h <= 0) return 10 + i;
    if (rects[i].x < BROWSE_MARGIN || rects[i].y < BROWSE_MARGIN) return 30 + i;
    if (rects[i].x + rects[i].w > l->inner_w - BROWSE_MARGIN) return 50 + i;
    if (rects[i].y + rects[i].h > l->inner_h) return 70 + i;
  }

  /* Status line stays clear of the size clearance */
  if (l->status.x + l->status.w > l->inner_w - BROWSE_MARGIN - cfg->size_clearance) {
    return 90;
  }
  /* Status line ends exactly at bottom of inner area */
  if (l->status.y + l->status.h != l->inner_h) return 91;

  /* Buttons in nav row share width evenly and do not overlap */
  for (i = 1; i < 4; i++) {
    if (l->nav_buttons[i].w != l->nav_buttons[0].w) return 92;
    if (l->nav_buttons[i].x != l->nav_buttons[i - 1].x + l->nav_buttons[0].w + BROWSE_SPACING) {
      return 93;
    }
  }

  /* Buttons in action row share width evenly and do not overlap */
  for (i = 1; i < 5; i++) {
    if (l->act_buttons[i].w != l->act_buttons[0].w) return 94;
    if (l->act_buttons[i].x != l->act_buttons[i - 1].x + l->act_buttons[0].w + BROWSE_SPACING) {
      return 95;
    }
  }

  /* No two controls overlap */
  for (i = 0; i < 13; i++) {
    for (j = i + 1; j < 13; j++) {
      if (rects_overlap(&rects[i], &rects[j])) {
        return 100 + i * 13 + j;
      }
    }
  }
  return 0;
}

static int test_compute_layout(void)
{
  static const int font_sizes[] = { 8, 11, 13, 16, 24 };
  static const int test_sizes[][2] = {
    { -10, -10 }, { 0, 0 }, { 50, 50 }, { 200, 100 },
    { 450, 200 }, { 500, 350 }, { 640, 256 }, { 800, 600 },
    { 1280, 720 }, { 1920, 1080 }
  };
  uint32_t f, s;

  for (f = 0U; f < sizeof(font_sizes) / sizeof(font_sizes[0]); f++) {
    int fh = font_sizes[f];
    ZZPlayBrowseLayoutConfig cfg;
    ZZPlayBrowseLayout layout;

    cfg.font_h = fh;
    cfg.cycle_w = fh * 12 + 32;
    cfg.search_label_w = fh * 6 + 8;
    cfg.search_min_w = fh * 8;
    cfg.nav_btn_min_w = fh * 7 + 16;
    cfg.act_btn_min_w = fh * 8 + 16;
    cfg.list_min_w = (fh + 4) + (fh * 6) + 3 * BROWSE_COLUMN_GAP + 100;
    cfg.list_min_rows = 3;
    cfg.size_clearance = fh > 12 ? fh + 8 : 20;

    for (s = 0U; s < sizeof(test_sizes) / sizeof(test_sizes[0]); s++) {
      int tw = test_sizes[s][0];
      int th = test_sizes[s][1];
      int err;

      zzplay_browse_compute_layout(tw, th, &cfg, &layout);
      err = verify_layout(&layout, &cfg, tw, th);
      if (err != 0) {
        printf("test_compute_layout failed: font=%d tw=%d th=%d err=%d\n",
               fh, tw, th, err);
        return err;
      }
    }
  }
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
  if (test_compute_layout() != 0) {
    failures++;
    printf("fail: compute layout\n");
  }
  if (failures == 0) {
    printf("zzplay_browse_rows_test: ok\n");
    return 0;
  }
  printf("zzplay_browse_rows_test: %d failure(s)\n", failures);
  return 1;
}
