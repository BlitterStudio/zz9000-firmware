/*
 * Header-only image geometry helpers for SDK callers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_IMAGE_GEOMETRY_H
#define ZZ9K_IMAGE_GEOMETRY_H

#include "zz9k/abi.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZZ9K_IMAGE_SCALE_DEFAULT_SLICE_ROWS 96U

typedef struct ZZ9KRect {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
} ZZ9KRect;

static inline int zz9k_rect_is_empty(const ZZ9KRect *rect)
{
  return !rect || rect->w == 0U || rect->h == 0U;
}

static inline uint32_t zz9k_image_muldiv_floor_u32(uint32_t value,
                                                   uint32_t multiplier,
                                                   uint32_t divisor)
{
  uint64_t product;

  if (divisor == 0U) {
    return 0U;
  }
  product = (uint64_t)value * (uint64_t)multiplier;
  return (uint32_t)(product / divisor);
}

static inline int zz9k_image_fit_size_to_area(uint32_t src_w,
                                              uint32_t src_h,
                                              uint32_t area_w,
                                              uint32_t area_h,
                                              uint32_t *out_w,
                                              uint32_t *out_h)
{
  uint64_t width_limited;
  uint32_t width;
  uint32_t height;

  if (src_w == 0U || src_h == 0U || area_w == 0U || area_h == 0U ||
      !out_w || !out_h) {
    return 0;
  }

  width_limited = (uint64_t)src_w * (uint64_t)area_h;
  if (width_limited > ((uint64_t)src_h * (uint64_t)area_w)) {
    width = area_w;
    height = zz9k_image_muldiv_floor_u32(src_h, area_w, src_w);
    if (height == 0U) {
      height = 1U;
    }
  } else {
    height = area_h;
    width = zz9k_image_muldiv_floor_u32(src_w, area_h, src_h);
    if (width == 0U) {
      width = 1U;
    }
  }

  *out_w = width;
  *out_h = height;
  return 1;
}

static inline int zz9k_image_choose_draw_rect_in_area(const ZZ9KRect *area,
                                                      uint32_t src_width,
                                                      uint32_t src_height,
                                                      ZZ9KRect *rect)
{
  uint32_t width;
  uint32_t height;

  if (!area || !rect || area->w == 0U || area->h == 0U ||
      src_width == 0U || src_height == 0U) {
    return 0;
  }
  if (!zz9k_image_fit_size_to_area(src_width, src_height, area->w,
                                   area->h, &width, &height)) {
    return 0;
  }

  rect->x = area->x + ((area->w - width) / 2U);
  rect->y = area->y + ((area->h - height) / 2U);
  rect->w = width;
  rect->h = height;
  return 1;
}

static inline uint32_t zz9k_image_count_scale_slices(const ZZ9KRect *clip_rect,
                                                     uint32_t slice_rows)
{
  if (zz9k_rect_is_empty(clip_rect) || slice_rows == 0U) {
    return 0U;
  }
  return (clip_rect->h + slice_rows - 1U) / slice_rows;
}

static inline int zz9k_image_scale_filter_supported_by_service(
    uint32_t service_flags,
    uint32_t filter)
{
  switch (filter) {
  case ZZ9K_SCALE_NEAREST:
    return 1;
  case ZZ9K_SCALE_BILINEAR:
    return (service_flags & ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BILINEAR) != 0U;
  default:
    return 0;
  }
}

static inline int zz9k_image_service_supports_clipped_scale(
    uint32_t opcode_count,
    uint32_t service_flags,
    uint32_t filter)
{
  if (opcode_count < 8U ||
      (service_flags & ZZ9K_SERVICE_FLAG_IMAGE_SCALE_CLIPPED) == 0U) {
    return 0;
  }
  return zz9k_image_scale_filter_supported_by_service(service_flags, filter);
}

static inline int zz9k_image_build_surface_scale_desc(
    ZZ9KScaleImageDesc *desc,
    uint32_t source_handle,
    uint32_t destination_handle,
    uint32_t source_width,
    uint32_t source_height,
    const ZZ9KRect *draw_rect,
    uint32_t filter)
{
  if (!desc || zz9k_rect_is_empty(draw_rect) ||
      source_handle == ZZ9K_INVALID_HANDLE ||
      destination_handle == ZZ9K_INVALID_HANDLE ||
      source_width == 0U || source_height == 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->src_surface = source_handle;
  desc->dst_surface = destination_handle;
  desc->src_w = source_width;
  desc->src_h = source_height;
  desc->dst_x = draw_rect->x;
  desc->dst_y = draw_rect->y;
  desc->dst_w = draw_rect->w;
  desc->dst_h = draw_rect->h;
  desc->filter = filter;
  return 1;
}

static inline int zz9k_image_build_framebuffer_scale_desc(
    ZZ9KScaleImageDesc *desc,
    uint32_t source_handle,
    uint32_t source_width,
    uint32_t source_height,
    const ZZ9KRect *draw_rect,
    uint32_t filter)
{
  return zz9k_image_build_surface_scale_desc(
      desc, source_handle, ZZ9K_SURFACE_HANDLE_FRAMEBUFFER,
      source_width, source_height, draw_rect, filter);
}

static inline int zz9k_image_build_surface_clipped_scale_desc(
    ZZ9KScaleImageClippedDesc *desc,
    uint32_t source_handle,
    uint32_t destination_handle,
    uint32_t source_width,
    uint32_t source_height,
    const ZZ9KRect *draw_rect,
    const ZZ9KRect *clip_rect,
    uint32_t filter)
{
  if (!desc || zz9k_rect_is_empty(draw_rect) ||
      zz9k_rect_is_empty(clip_rect) ||
      source_handle == ZZ9K_INVALID_HANDLE ||
      destination_handle == ZZ9K_INVALID_HANDLE ||
      source_width == 0U || source_height == 0U) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->src_surface = source_handle;
  desc->dst_surface = destination_handle;
  desc->src_w = source_width;
  desc->src_h = source_height;
  desc->dst_x = draw_rect->x;
  desc->dst_y = draw_rect->y;
  desc->dst_w = draw_rect->w;
  desc->dst_h = draw_rect->h;
  desc->clip_x = clip_rect->x;
  desc->clip_y = clip_rect->y;
  desc->clip_w = clip_rect->w;
  desc->clip_h = clip_rect->h;
  desc->filter = filter;
  return 1;
}

static inline int zz9k_image_build_clipped_scale_desc(
    ZZ9KScaleImageClippedDesc *desc,
    uint32_t source_handle,
    uint32_t source_width,
    uint32_t source_height,
    const ZZ9KRect *draw_rect,
    const ZZ9KRect *clip_rect,
    uint32_t filter)
{
  return zz9k_image_build_surface_clipped_scale_desc(
      desc, source_handle, ZZ9K_SURFACE_HANDLE_FRAMEBUFFER,
      source_width, source_height, draw_rect, clip_rect, filter);
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_IMAGE_GEOMETRY_H */
