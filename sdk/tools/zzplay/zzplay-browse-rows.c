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
  int list_min_rows;
  int list_min_h;
  int cycle_w;
  int search_label_w;
  int search_min_w;
  int nav_btn_min_w;
  int act_btn_min_w;
  int list_min_w;
  int size_clearance;
  int top_row_min;
  int nav_row_min;
  int act_row_min;
  int content_min_w;
  int min_inner_w;
  int min_inner_h;
  int inner_w;
  int inner_h;
  int content_w;
  int cur_y;
  int list_h;
  int below_list;
  int nav_btn_w;
  int act_btn_w;
  int status_w;
  int i;

  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!cfg) {
    return;
  }

  font_h = cfg->font_h > 4 ? cfg->font_h : 8;
  btn_h = font_h + 6;
  list_min_rows = cfg->list_min_rows > 1 ? cfg->list_min_rows : 3;
  list_min_h = list_min_rows * font_h + 4;

  cycle_w = cfg->cycle_w > 20 ? cfg->cycle_w : 80;
  search_label_w = cfg->search_label_w > 10 ? cfg->search_label_w : 50;
  search_min_w = cfg->search_min_w > 20 ? cfg->search_min_w : 60;
  nav_btn_min_w = cfg->nav_btn_min_w > 20 ? cfg->nav_btn_min_w : 50;
  act_btn_min_w = cfg->act_btn_min_w > 20 ? cfg->act_btn_min_w : 50;
  list_min_w = cfg->list_min_w > 50 ? cfg->list_min_w : 150;
  size_clearance = cfg->size_clearance >= 0 ? cfg->size_clearance : 20;

  /* Minimum content width needed by each row */
  top_row_min = cycle_w + BROWSE_SPACING + search_label_w + search_min_w;
  nav_row_min = ZZPLAY_BROWSE_NAV_COUNT * nav_btn_min_w +
                (ZZPLAY_BROWSE_NAV_COUNT - 1) * BROWSE_SPACING;
  act_row_min = ZZPLAY_BROWSE_ACT_COUNT * act_btn_min_w +
                (ZZPLAY_BROWSE_ACT_COUNT - 1) * BROWSE_SPACING;

  content_min_w = top_row_min;
  if (nav_row_min > content_min_w) {
    content_min_w = nav_row_min;
  }
  if (act_row_min > content_min_w) {
    content_min_w = act_row_min;
  }
  if (list_min_w > content_min_w) {
    content_min_w = list_min_w;
  }

  min_inner_w = content_min_w + 2 * BROWSE_MARGIN;
  min_inner_h = BROWSE_MARGIN + btn_h + BROWSE_SPACING + list_min_h +
                BROWSE_SPACING + btn_h + BROWSE_SPACING + btn_h +
                BROWSE_SPACING + btn_h;

  inner_w = target_inner_w < min_inner_w ? min_inner_w : target_inner_w;
  inner_h = target_inner_h < min_inner_h ? min_inner_h : target_inner_h;

  out->min_inner_w = min_inner_w;
  out->min_inner_h = min_inner_h;
  out->inner_w = inner_w;
  out->inner_h = inner_h;

  content_w = inner_w - 2 * BROWSE_MARGIN;

  /* 1. Top row: Service cycle + Search field */
  cur_y = BROWSE_MARGIN;
  out->service.x = BROWSE_MARGIN;
  out->service.y = cur_y;
  out->service.w = cycle_w;
  out->service.h = btn_h;

  out->search.x = BROWSE_MARGIN + cycle_w + BROWSE_SPACING + search_label_w;
  out->search.y = cur_y;
  out->search.w = content_w - cycle_w - BROWSE_SPACING - search_label_w;
  if (out->search.w < search_min_w) {
    out->search.w = search_min_w;
  }
  out->search.h = btn_h;

  cur_y += btn_h + BROWSE_SPACING;

  /* 2. List view */
  below_list = 3 * BROWSE_SPACING + 3 * btn_h;
  list_h = inner_h - cur_y - below_list;
  if (list_h < list_min_h) {
    list_h = list_min_h;
  }
  out->list.x = BROWSE_MARGIN;
  out->list.y = cur_y;
  out->list.w = content_w;
  out->list.h = list_h;

  cur_y += list_h + BROWSE_SPACING;

  /* 3. Navigation row (4 buttons: Back, Reload, Stop, Settings) */
  nav_btn_w = (content_w - (ZZPLAY_BROWSE_NAV_COUNT - 1) * BROWSE_SPACING) /
              ZZPLAY_BROWSE_NAV_COUNT;
  if (nav_btn_w < nav_btn_min_w) {
    nav_btn_w = nav_btn_min_w;
  }
  for (i = 0; i < ZZPLAY_BROWSE_NAV_COUNT; i++) {
    out->nav_buttons[i].x = BROWSE_MARGIN + i * (nav_btn_w + BROWSE_SPACING);
    out->nav_buttons[i].y = cur_y;
    out->nav_buttons[i].w = nav_btn_w;
    out->nav_buttons[i].h = btn_h;
  }

  cur_y += btn_h + BROWSE_SPACING;

  /* 4. Action row (5 buttons: Play, Add, Play all, Add all, Favourite) */
  act_btn_w = (content_w - (ZZPLAY_BROWSE_ACT_COUNT - 1) * BROWSE_SPACING) /
              ZZPLAY_BROWSE_ACT_COUNT;
  if (act_btn_w < act_btn_min_w) {
    act_btn_w = act_btn_min_w;
  }
  for (i = 0; i < ZZPLAY_BROWSE_ACT_COUNT; i++) {
    out->act_buttons[i].x = BROWSE_MARGIN + i * (act_btn_w + BROWSE_SPACING);
    out->act_buttons[i].y = cur_y;
    out->act_buttons[i].w = act_btn_w;
    out->act_buttons[i].h = btn_h;
  }

  cur_y += btn_h + BROWSE_SPACING;

  /* 5. Status line */
  out->status.x = BROWSE_MARGIN;
  out->status.y = cur_y;
  status_w = content_w - size_clearance;
  if (status_w < 40) {
    status_w = 40;
  }
  out->status.w = status_w;
  out->status.h = btn_h;
}
