/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-geometry.h"

#include <string.h>

ZZPlayRect zzplay_geometry_fit(uint16_t src_w, uint16_t src_h,
                               uint16_t avail_w, uint16_t avail_h)
{
  ZZPlayRect rect;
  uint32_t width;
  uint32_t height;

  memset(&rect, 0, sizeof(rect));
  if (src_w == 0U || src_h == 0U || avail_w == 0U || avail_h == 0U) {
    return rect;
  }
  /* Compare src_w/src_h against avail_w/avail_h by cross-multiplying, which
   * keeps this exact in integers at any size. */
  if ((uint32_t)src_w * (uint32_t)avail_h >
      (uint32_t)avail_w * (uint32_t)src_h) {
    /* Source is relatively wider: width is the binding constraint. */
    width = avail_w;
    height = ((uint32_t)avail_w * (uint32_t)src_h) / (uint32_t)src_w;
  } else {
    height = avail_h;
    width = ((uint32_t)avail_h * (uint32_t)src_w) / (uint32_t)src_h;
  }
  /* Integer division can round a very thin fit to nothing; one pixel of a
   * wrong aspect beats an empty overlay rectangle. */
  if (width == 0U) {
    width = 1U;
  }
  if (height == 0U) {
    height = 1U;
  }
  if (width > avail_w) {
    width = avail_w;
  }
  if (height > avail_h) {
    height = avail_h;
  }
  rect.width = (uint16_t)width;
  rect.height = (uint16_t)height;
  rect.x = (int16_t)(((uint32_t)avail_w - width) / 2U);
  rect.y = (int16_t)(((uint32_t)avail_h - height) / 2U);
  return rect;
}

int zzplay_geometry_is_fitted(uint16_t width, uint16_t height,
                              uint16_t src_w, uint16_t src_h)
{
  uint32_t across = (uint32_t)width * (uint32_t)src_h;
  uint32_t down = (uint32_t)height * (uint32_t)src_w;
  uint32_t error = across > down ? across - down : down - across;

  /* width/height against src_w/src_h, cross-multiplied. A fit binds one
   * side and rounds the other down, which leaves an error below src_w
   * (width bound) or src_h (height bound): under one output pixel. */
  return error < (src_w > src_h ? (uint32_t)src_w : (uint32_t)src_h);
}

int zzplay_geometry_depth_is_supported(uint16_t depth)
{
  return depth == 15U || depth == 16U || depth == 32U;
}

int zzplay_geometry_is_doubled(uint16_t width, uint16_t height)
{
  return width < ZZPLAY_SCREEN_FLOOR_W || height < ZZPLAY_SCREEN_FLOOR_H;
}

void zzplay_geometry_fullscreen_plan(uint32_t wb_mode_id,
                                     uint16_t wb_width,
                                     uint16_t wb_height,
                                     uint16_t wb_depth,
                                     ZZPlayFullscreenPlan *plan)
{
  if (!plan) {
    return;
  }
  memset(plan, 0, sizeof(*plan));

  if (zzplay_geometry_depth_is_supported(wb_depth)) {
    plan->target_depth = wb_depth;
  } else {
    plan->target_depth = 16U;
  }

  if (zzplay_geometry_is_doubled(wb_width, wb_height) ||
      wb_mode_id == ZZPLAY_INVALID_MODE_ID) {
    plan->action = ZZPLAY_FS_FALLBACK_LADDER;
    return;
  }

  if (zzplay_geometry_depth_is_supported(wb_depth)) {
    plan->action = ZZPLAY_FS_REUSE_MODE;
    plan->mode_id = wb_mode_id;
    return;
  }

  plan->action = ZZPLAY_FS_MATCH_WB_RES;
  plan->width = wb_width;
  plan->height = wb_height;
}

static const uint16_t zzplay_screen_request_percent[4] = {100U, 133U, 166U,
                                                          200U};

int zzplay_geometry_screen_request(unsigned attempt,
                                   uint16_t display_w, uint16_t display_h,
                                   uint16_t src_w, uint16_t src_h,
                                   uint16_t *req_w, uint16_t *req_h)
{
  uint32_t w = display_w;
  uint32_t h = display_h;
  uint32_t percent;

  if (attempt >= 4U || req_w == 0 || req_h == 0) {
    return 0;
  }
  if (w == 0U || h == 0U) {
    w = src_w;
    h = src_h;
  }
  percent = zzplay_screen_request_percent[attempt];
  w = (w * percent) / 100U;
  h = (h * percent) / 100U;
  if (w < ZZPLAY_SCREEN_FLOOR_W) {
    w = ZZPLAY_SCREEN_FLOOR_W;
  }
  if (h < ZZPLAY_SCREEN_FLOOR_H) {
    h = ZZPLAY_SCREEN_FLOOR_H;
  }
  if (w > ZZPLAY_SCREEN_CAP_W) {
    w = ZZPLAY_SCREEN_CAP_W;
  }
  if (h > ZZPLAY_SCREEN_CAP_H) {
    h = ZZPLAY_SCREEN_CAP_H;
  }
  *req_w = (uint16_t)w;
  *req_h = (uint16_t)h;
  return 1;
}

int zzplay_geometry_is_exact(const ZZPlayRect *rect,
                             uint16_t src_w, uint16_t src_h)
{
  if (!rect) {
    return 0;
  }
  return rect->width == src_w && rect->height == src_h;
}

void zzplay_geometry_remember(ZZPlayWindowGeometry *saved,
                              const ZZPlayRect *window)
{
  if (!saved || !window) {
    return;
  }
  saved->window = *window;
  saved->valid = 1;
}

int zzplay_geometry_restore(const ZZPlayWindowGeometry *saved,
                            ZZPlayRect *window)
{
  if (!saved || !window || !saved->valid) {
    return 0;
  }
  *window = saved->window;
  return 1;
}
