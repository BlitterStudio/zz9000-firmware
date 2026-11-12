/* Pure layout helpers for the Browse window's column rows and its
 * remembered geometry. No Amiga includes, so the host suite builds them.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_BROWSE_ROWS_H
#define ZZPLAY_BROWSE_ROWS_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-browse-model.h"

/* The length column is sized once from this text ("88:88:88"), not per
 * row, so every known length aligns at the same right edge. */
#define ZZPLAY_BROWSE_LEN_TEXT "88:88:88"

/* Longest duration a row shows (99:59:59); longer ones clamp to it. */
#define ZZPLAY_BROWSE_MS_MAX 359999000U

/* Inner margins and control spacing in the Browse window. */
#define BROWSE_MARGIN 6
#define BROWSE_SPACING 4
/* Space between the marker, title, subtitle and length cells. */
#define BROWSE_COLUMN_GAP 6

/* Share of the width left to title and subtitle that the title gets. */
#define ZZPLAY_BROWSE_TITLE_SHARE 55

typedef struct ZZPlayBrowseRect {
  int x;
  int y;
  int w;
  int h;
} ZZPlayBrowseRect;

/* Button row 1: Navigation row (4 buttons) */
#define ZZPLAY_BROWSE_NAV_BACK 0
#define ZZPLAY_BROWSE_NAV_RELOAD 1
#define ZZPLAY_BROWSE_NAV_STOP 2
#define ZZPLAY_BROWSE_NAV_SETTINGS 3
#define ZZPLAY_BROWSE_NAV_COUNT 4

/* Button row 2: Action row (5 buttons) */
#define ZZPLAY_BROWSE_ACT_PLAY 0
#define ZZPLAY_BROWSE_ACT_ADD 1
#define ZZPLAY_BROWSE_ACT_PLAY_ALL 2
#define ZZPLAY_BROWSE_ACT_ADD_ALL 3
#define ZZPLAY_BROWSE_ACT_FAVOURITE 4
#define ZZPLAY_BROWSE_ACT_COUNT 5

typedef struct ZZPlayBrowseLayoutConfig {
  int font_h;          /* Screen font height (YSize) */
  int cycle_w;         /* Measured width of the service cycle gadget */
  int search_label_w;  /* Measured width of "Search:" label + padding */
  int search_min_w;    /* Minimum width for search string field */
  int nav_btn_min_w;   /* Minimum button width in navigation row */
  int act_btn_min_w;   /* Minimum button width in action row */
  int list_min_w;      /* Sensible minimum width for listview */
  int list_min_rows;   /* Minimum visible rows in listview */
  int size_clearance;  /* Space at right of status line for sizing gadget */
} ZZPlayBrowseLayoutConfig;

typedef struct ZZPlayBrowseLayout {
  int min_inner_w;
  int min_inner_h;
  int inner_w;
  int inner_h;

  ZZPlayBrowseRect service;
  ZZPlayBrowseRect search;
  ZZPlayBrowseRect list;
  ZZPlayBrowseRect nav_buttons[ZZPLAY_BROWSE_NAV_COUNT];
  ZZPlayBrowseRect act_buttons[ZZPLAY_BROWSE_ACT_COUNT];
  ZZPlayBrowseRect status;
} ZZPlayBrowseLayout;
typedef struct ZZPlayBrowseColumns {
  int marker_x, marker_w; /* the row's kind marker */
  int title_x, title_w;
  int sub_x, sub_w;       /* artist and album, or the module's details */
  int len_x, len_w;       /* length, right-aligned inside the cell */
} ZZPlayBrowseColumns;

/* "m:ss" or "h:mm:ss" for a duration in milliseconds, rounded to the
 * nearest second; 0 (unknown) gives the empty string. */
void zzplay_browse_format_duration(uint32_t duration_ms, char *buf,
                                   size_t size);

/* Split a row's pixel width into marker, title, subtitle and length
 * cells. The length cell keeps `len_w` at the right edge; the title gets
 * about 55 percent of what is left after the marker and the gaps. No
 * width is ever negative, so a very narrow row simply clips its cells. */
void zzplay_browse_layout_columns(int avail_w, int marker_w, int len_w,
                                  int gap, ZZPlayBrowseColumns *out);

/* Clamp a remembered window to the screen it is opened on: size into
 * [min_w, screen_w] and [min_h, screen_h], then position it so the whole
 * window is visible. */
void zzplay_browse_clamp_window(int *left, int *top, int *width, int *height,
                                int screen_w, int screen_h,
                                int min_w, int min_h);

/* Rows a gather would add to the playlist: playable, not folders. */
uint32_t zzplay_browse_count_playable(const ZZPlayBrowseItem *items,
                                      uint32_t count);

/* Compute the pure geometry for the Browse window controls with two
 * button rows (navigation and action). If target_inner_w or
 * target_inner_h are smaller than the computed minimums, they are
 * clamped up to min_inner_w and min_inner_h. Controls never overlap,
 * and the status line stays clear of size_clearance. */
void zzplay_browse_compute_layout(int target_inner_w, int target_inner_h,
                                  const ZZPlayBrowseLayoutConfig *cfg,
                                  ZZPlayBrowseLayout *out);

#endif /* ZZPLAY_BROWSE_ROWS_H */
