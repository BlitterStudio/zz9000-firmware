/* Pure layout helpers for the Browse window; see zzplay-browse-rows.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse-rows.h"

#include <stdio.h>
#include <string.h>

#include "zzplay-browse-model.h"

void zzplay_browse_format_duration(uint32_t duration_ms, char *buf,
                                   size_t size)
{
  uint32_t secs;
  uint32_t hours;
  uint32_t minutes;

  if (!buf || size == 0U) {
    return;
  }
  if (duration_ms == 0U) {
    buf[0] = '\0';
    return;
  }
  if (duration_ms > ZZPLAY_BROWSE_MS_MAX) {
    duration_ms = ZZPLAY_BROWSE_MS_MAX;
  }
  secs = (duration_ms + 500U) / 1000U;
  hours = secs / 3600U;
  minutes = (secs / 60U) % 60U;
  secs %= 60U;
  if (hours > 0U) {
    (void)snprintf(buf, size, "%lu:%02lu:%02lu", (unsigned long)hours,
                   (unsigned long)minutes, (unsigned long)secs);
  } else {
    (void)snprintf(buf, size, "%lu:%02lu", (unsigned long)minutes,
                   (unsigned long)secs);
  }
}

uint32_t zzplay_browse_count_playable(const ZZPlayBrowseItem *items,
                                      uint32_t count)
{
  uint32_t playable = 0U;
  uint32_t i;

  if (!items) {
    return 0U;
  }
  for (i = 0U; i < count; i++) {
    if (items[i].kind != ZZPLAY_ITEM_FOLDER && items[i].playable != 0U) {
      playable++;
    }
  }
  return playable;
}
void zzplay_browse_layout_columns(int avail_w, int marker_w, int len_w,
                                  int gap, ZZPlayBrowseColumns *out)
{
  int rest;
  int title_w;

  if (!out) {
    return;
  }
  out->marker_x = 0;
  out->marker_w = 0;
  out->title_x = 0;
  out->title_w = 0;
  out->sub_x = 0;
  out->sub_w = 0;
  out->len_x = 0;
  out->len_w = 0;
  if (avail_w < 1 || marker_w < 0 || len_w < 0 || gap < 0) {
    return;
  }

  /* Two pixels of padding on each side, like GadTools' own row text. */
  if (len_w + 4 > avail_w) {
    len_w = avail_w - 4;
    if (len_w < 0) {
      len_w = 0;
    }
  }
  out->marker_x = 2;
  out->marker_w = marker_w;
  out->len_w = len_w;
  out->len_x = avail_w - 2 - len_w;
  out->title_x = 2 + marker_w + gap;

  /* What is left between the marker and the length cell is shared by
   * the title and the subtitle: a gap before the length cell, one
   * between them, and the title gets the larger share. */
  rest = out->len_x - gap - out->title_x;
  if (rest < 0) {
    rest = 0;
  }
  title_w = (rest * ZZPLAY_BROWSE_TITLE_SHARE) / 100;
  out->title_w = title_w;
  out->sub_x = out->title_x + title_w + gap;
  out->sub_w = rest - title_w - gap;
  if (out->sub_w < 0) {
    out->sub_w = 0;
  }
  if (out->sub_x > out->len_x) {
    out->sub_x = out->len_x;
  }
}

void zzplay_browse_clamp_window(int *left, int *top, int *width, int *height,
                                int screen_w, int screen_h,
                                int min_w, int min_h)
{
  if (!left || !top || !width || !height) {
    return;
  }
  if (min_w < 0) {
    min_w = 0;
  }
  if (min_h < 0) {
    min_h = 0;
  }
  if (*width < min_w) {
    *width = min_w;
  }
  if (*height < min_h) {
    *height = min_h;
  }
  /* A minimum larger than the screen loses to the screen: the window
   * must still open on it. */
  if (*width > screen_w) {
    *width = screen_w;
  }
  if (*height > screen_h) {
    *height = screen_h;
  }
  if (*left > screen_w - *width) {
    *left = screen_w - *width;
  }
  if (*top > screen_h - *height) {
    *top = screen_h - *height;
  }
  if (*left < 0) {
    *left = 0;
  }
  if (*top < 0) {
    *top = 0;
  }
}

void zzplay_browse_compute_layout(int target_inner_w, int target_inner_h,
                                  const ZZPlayBrowseLayoutConfig *cfg,
                                  ZZPlayBrowseLayout *out)
{
  int font_h;
  int btn_h;
  int list_min_h;
  int cycle_w;
  int search_label_w;
  int search_min_w;
  int btn_min_w;
  int size_clearance;
  int content_min_w;
  int grid_min_w;
  int content_w;
  int cur_y;
  int list_h;
  int below_list;
  int cell_w;
  int extra;
  int status_w;
  int row;
  int col;

  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!cfg) {
    return;
  }

  font_h = cfg->font_h > 4 ? cfg->font_h : 8;
  btn_h = font_h + 6;
  list_min_h = (cfg->list_min_rows > 1 ? cfg->list_min_rows : 3) * font_h + 4;
  cycle_w = cfg->cycle_w > 20 ? cfg->cycle_w : 80;
  search_label_w = cfg->search_label_w > 10 ? cfg->search_label_w : 50;
  search_min_w = cfg->search_min_w > 20 ? cfg->search_min_w : 60;
  btn_min_w = cfg->btn_min_w > 20 ? cfg->btn_min_w : 50;
  size_clearance = cfg->size_clearance >= 0 ? cfg->size_clearance : 20;

  /* The widest of the search line, the button grid and the list. */
  content_min_w = cycle_w + BROWSE_SPACING + search_label_w + search_min_w;
  grid_min_w = ZZPLAY_BROWSE_BUTTON_COLS * btn_min_w +
               (ZZPLAY_BROWSE_BUTTON_COLS - 1) * BROWSE_SPACING;
  if (grid_min_w > content_min_w) {
    content_min_w = grid_min_w;
  }
  if (cfg->list_min_w > content_min_w) {
    content_min_w = cfg->list_min_w;
  }
  if (size_clearance + 40 > content_min_w) {
    content_min_w = size_clearance + 40;
  }

  /* Search line, list, button rows, status line. */
  below_list = ZZPLAY_BROWSE_BUTTON_ROWS * (BROWSE_SPACING + btn_h) +
               BROWSE_SPACING + btn_h;
  out->min_inner_w = content_min_w + 2 * BROWSE_MARGIN;
  out->min_inner_h = BROWSE_MARGIN + btn_h + BROWSE_SPACING + list_min_h +
                     below_list;
  out->inner_w = target_inner_w < out->min_inner_w ? out->min_inner_w
                                                   : target_inner_w;
  out->inner_h = target_inner_h < out->min_inner_h ? out->min_inner_h
                                                   : target_inner_h;
  content_w = out->inner_w - 2 * BROWSE_MARGIN;

  cur_y = BROWSE_MARGIN;
  out->service.x = BROWSE_MARGIN;
  out->service.y = cur_y;
  out->service.w = cycle_w;
  out->service.h = btn_h;
  out->search.x = BROWSE_MARGIN + cycle_w + BROWSE_SPACING + search_label_w;
  out->search.y = cur_y;
  out->search.w = content_w - cycle_w - BROWSE_SPACING - search_label_w;
  out->search.h = btn_h;
  cur_y += btn_h + BROWSE_SPACING;

  list_h = out->inner_h - cur_y - below_list;
  out->list.x = BROWSE_MARGIN;
  out->list.y = cur_y;
  out->list.w = content_w;
  out->list.h = list_h;
  cur_y += list_h;

  /* Equal cells; the pixels left over from the division go one each to
   * the first columns, the same in every row, so the columns line up
   * and the grid ends flush with the list. */
  cell_w = (content_w - (ZZPLAY_BROWSE_BUTTON_COLS - 1) * BROWSE_SPACING) /
           ZZPLAY_BROWSE_BUTTON_COLS;
  extra = content_w - (ZZPLAY_BROWSE_BUTTON_COLS - 1) * BROWSE_SPACING -
          cell_w * ZZPLAY_BROWSE_BUTTON_COLS;
  for (row = 0; row < ZZPLAY_BROWSE_BUTTON_ROWS; row++) {
    int x = BROWSE_MARGIN;

    cur_y += BROWSE_SPACING;
    for (col = 0; col < ZZPLAY_BROWSE_BUTTON_COLS; col++) {
      ZZPlayBrowseRect *r =
          &out->buttons[row * ZZPLAY_BROWSE_BUTTON_COLS + col];

      r->x = x;
      r->y = cur_y;
      r->w = cell_w + (col < extra ? 1 : 0);
      r->h = btn_h;
      x += r->w + BROWSE_SPACING;
    }
    cur_y += btn_h;
  }
  cur_y += BROWSE_SPACING;

  status_w = content_w - size_clearance;
  out->status.x = BROWSE_MARGIN;
  out->status.y = cur_y;
  out->status.w = status_w;
  out->status.h = btn_h;
}
