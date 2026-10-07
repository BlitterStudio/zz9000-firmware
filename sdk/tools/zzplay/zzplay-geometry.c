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
