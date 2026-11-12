/* Pure layout helpers for the Browse window; see zzplay-browse-rows.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-browse-rows.h"

#include <stdio.h>

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
