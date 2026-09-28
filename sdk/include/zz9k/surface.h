/*
 * Header-only surface format and layout helpers for SDK callers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_SURFACE_H
#define ZZ9K_SURFACE_H

#include "zz9k/abi.h"
#include "zz9k/image_geometry.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t zz9k_surface_bytes_per_pixel(uint32_t format)
{
  switch (format) {
  case ZZ9K_SURFACE_FORMAT_ARGB8888:
  case ZZ9K_SURFACE_FORMAT_RGBA8888:
  case ZZ9K_SURFACE_FORMAT_BGRA8888:
    return 4U;
  case ZZ9K_SURFACE_FORMAT_RGB565:
  case ZZ9K_SURFACE_FORMAT_RGB555:
    return 2U;
  case ZZ9K_SURFACE_FORMAT_RGB888:
    return 3U;
  case ZZ9K_SURFACE_FORMAT_INDEX8:
    return 1U;
  default:
    return 0U;
  }
}

static inline uint32_t zz9k_surface_native_rtg_format(void)
{
  return ZZ9K_SURFACE_FORMAT_BGRA8888;
}

static inline int zz9k_surface_is_native_rtg_format(uint32_t format)
{
  return format == zz9k_surface_native_rtg_format();
}

static inline uint32_t zz9k_known_surface_flag_count(void)
{
  return 5U;
}

static inline uint32_t zz9k_known_surface_flag(uint32_t index)
{
  switch (index) {
  case 0:
    return ZZ9K_SURFACE_FLAG_CPU_VISIBLE;
  case 1:
    return ZZ9K_SURFACE_FLAG_FRAMEBUFFER;
  case 2:
    return ZZ9K_SURFACE_FLAG_DISPLAYED;
  case 3:
    return ZZ9K_SURFACE_FLAG_SHARED_BUFFER;
  case 4:
    return ZZ9K_SURFACE_FLAG_ARM_LOCAL;
  default:
    return 0U;
  }
}

static inline const char *zz9k_surface_flag_name(uint32_t flag)
{
  switch (flag) {
  case ZZ9K_SURFACE_FLAG_CPU_VISIBLE:
    return "cpu-visible";
  case ZZ9K_SURFACE_FLAG_FRAMEBUFFER:
    return "framebuffer";
  case ZZ9K_SURFACE_FLAG_DISPLAYED:
    return "displayed";
  case ZZ9K_SURFACE_FLAG_SHARED_BUFFER:
    return "shared-buffer";
  case ZZ9K_SURFACE_FLAG_ARM_LOCAL:
    return "arm-local";
  default:
    return 0;
  }
}

static inline int zz9k_surface_min_pitch(uint32_t width,
                                         uint32_t format,
                                         uint32_t *pitch)
{
  uint32_t bpp;

  if (!pitch || width == 0U) {
    return 0;
  }
  bpp = zz9k_surface_bytes_per_pixel(format);
  if (bpp == 0U || width > (0xffffffffUL / bpp)) {
    return 0;
  }
  *pitch = width * bpp;
  return 1;
}

static inline int zz9k_surface_length_for_pitch(uint32_t height,
                                                uint32_t pitch,
                                                uint32_t *length)
{
  if (!length || height == 0U || pitch == 0U ||
      height > (0xffffffffUL / pitch)) {
    return 0;
  }
  *length = height * pitch;
  return 1;
}

static inline int zz9k_surface_layout(uint32_t width,
                                      uint32_t height,
                                      uint32_t format,
                                      uint32_t *pitch,
                                      uint32_t *length)
{
  uint32_t computed_pitch;
  uint32_t computed_length;

  if (!pitch || !length) {
    return 0;
  }
  if (!zz9k_surface_min_pitch(width, format, &computed_pitch) ||
      !zz9k_surface_length_for_pitch(height, computed_pitch,
                                     &computed_length)) {
    return 0;
  }
  *pitch = computed_pitch;
  *length = computed_length;
  return 1;
}

static inline int zz9k_surface_rect_fits_bpp(uint32_t width,
                                             uint32_t height,
                                             uint32_t pitch,
                                             uint32_t length,
                                             const ZZ9KRect *rect,
                                             uint32_t bpp)
{
  uint32_t x_offset;
  uint32_t row_bytes;
  uint32_t last_y;
  uint32_t last_row;
  uint32_t last_offset;

  if (!rect || bpp == 0U || rect->w == 0U || rect->h == 0U ||
      width == 0U || height == 0U || pitch == 0U || length == 0U) {
    return 0;
  }
  if (rect->x >= width || rect->y >= height) {
    return 0;
  }
  if (rect->w > (width - rect->x) || rect->h > (height - rect->y)) {
    return 0;
  }
  if (rect->x > (0xffffffffUL / bpp) ||
      rect->w > (0xffffffffUL / bpp)) {
    return 0;
  }

  x_offset = rect->x * bpp;
  row_bytes = rect->w * bpp;
  if (x_offset > pitch || row_bytes > (pitch - x_offset)) {
    return 0;
  }

  last_y = rect->y + rect->h - 1U;
  if (last_y > (0xffffffffUL / pitch)) {
    return 0;
  }
  last_row = last_y * pitch;
  if (last_row > (0xffffffffUL - x_offset)) {
    return 0;
  }
  last_offset = last_row + x_offset;
  if (last_offset > (0xffffffffUL - row_bytes)) {
    return 0;
  }
  if ((last_offset + row_bytes) > length) {
    return 0;
  }

  return 1;
}

static inline int zz9k_surface_rect_fits(uint32_t width,
                                         uint32_t height,
                                         uint32_t pitch,
                                         uint32_t length,
                                         const ZZ9KRect *rect,
                                         uint32_t format)
{
  return zz9k_surface_rect_fits_bpp(
      width, height, pitch, length, rect,
      zz9k_surface_bytes_per_pixel(format));
}

static inline int zz9k_surface_build_fill_desc(ZZ9KSurfaceFillDesc *desc,
                                               uint32_t surface_handle,
                                               const ZZ9KRect *rect,
                                               uint32_t color,
                                               uint32_t flags)
{
  if (!desc || zz9k_rect_is_empty(rect) ||
      surface_handle == ZZ9K_INVALID_HANDLE) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->surface = surface_handle;
  desc->x = rect->x;
  desc->y = rect->y;
  desc->width = rect->w;
  desc->height = rect->h;
  desc->color = color;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_surface_build_framebuffer_fill_desc(
    ZZ9KSurfaceFillDesc *desc,
    const ZZ9KRect *rect,
    uint32_t color,
    uint32_t flags)
{
  return zz9k_surface_build_fill_desc(
      desc, ZZ9K_SURFACE_HANDLE_FRAMEBUFFER, rect, color, flags);
}

static inline int zz9k_surface_build_copy_desc(ZZ9KSurfaceCopyDesc *desc,
                                               uint32_t source_handle,
                                               uint32_t destination_handle,
                                               const ZZ9KRect *source_rect,
                                               uint32_t destination_x,
                                               uint32_t destination_y,
                                               uint32_t flags)
{
  if (!desc || zz9k_rect_is_empty(source_rect) ||
      source_handle == ZZ9K_INVALID_HANDLE ||
      destination_handle == ZZ9K_INVALID_HANDLE) {
    return 0;
  }

  memset(desc, 0, sizeof(*desc));
  desc->src_surface = source_handle;
  desc->dst_surface = destination_handle;
  desc->src_x = source_rect->x;
  desc->src_y = source_rect->y;
  desc->dst_x = destination_x;
  desc->dst_y = destination_y;
  desc->width = source_rect->w;
  desc->height = source_rect->h;
  desc->flags = flags;
  return 1;
}

static inline int zz9k_surface_build_framebuffer_backup_copy_descs(
    ZZ9KSurfaceCopyDesc *save_copy,
    ZZ9KSurfaceCopyDesc *restore_copy,
    uint32_t backup_surface_handle,
    const ZZ9KRect *framebuffer_rect)
{
  ZZ9KRect backup_rect;

  if (!save_copy || !restore_copy || zz9k_rect_is_empty(framebuffer_rect) ||
      backup_surface_handle == ZZ9K_INVALID_HANDLE) {
    return 0;
  }

  if (!zz9k_surface_build_copy_desc(save_copy,
                                    ZZ9K_SURFACE_HANDLE_FRAMEBUFFER,
                                    backup_surface_handle,
                                    framebuffer_rect, 0U, 0U, 0U)) {
    return 0;
  }

  backup_rect.x = 0U;
  backup_rect.y = 0U;
  backup_rect.w = framebuffer_rect->w;
  backup_rect.h = framebuffer_rect->h;
  return zz9k_surface_build_copy_desc(restore_copy, backup_surface_handle,
                                      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER,
                                      &backup_rect, framebuffer_rect->x,
                                      framebuffer_rect->y, 0U);
}

static inline uint32_t zz9k_surface_color_argb(uint8_t a,
                                               uint8_t r,
                                               uint8_t g,
                                               uint8_t b)
{
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
         ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t zz9k_surface_color_rgb(uint8_t r,
                                              uint8_t g,
                                              uint8_t b)
{
  return zz9k_surface_color_argb(0xffU, r, g, b);
}

static inline uint32_t zz9k_surface_color_rgb565(uint8_t r,
                                                 uint8_t g,
                                                 uint8_t b)
{
  return (uint32_t)((((uint16_t)(r & 0xf8U)) << 8) |
                    (((uint16_t)(g & 0xfcU)) << 3) |
                    ((uint16_t)b >> 3));
}

static inline uint32_t zz9k_surface_color_rgb555(uint8_t r,
                                                 uint8_t g,
                                                 uint8_t b)
{
  return (uint32_t)((((uint16_t)(r & 0xf8U)) << 7) |
                    (((uint16_t)(g & 0xf8U)) << 2) |
                    ((uint16_t)b >> 3));
}

static inline uint32_t zz9k_surface_color_index8(uint8_t index)
{
  return (uint32_t)index;
}

static inline int zz9k_surface_color_for_format(uint32_t format,
                                                uint8_t r,
                                                uint8_t g,
                                                uint8_t b,
                                                uint8_t a,
                                                uint8_t index,
                                                uint32_t *color)
{
  if (!color) {
    return 0;
  }

  switch (format) {
  case ZZ9K_SURFACE_FORMAT_ARGB8888:
  case ZZ9K_SURFACE_FORMAT_RGBA8888:
  case ZZ9K_SURFACE_FORMAT_BGRA8888:
  case ZZ9K_SURFACE_FORMAT_RGB888:
    *color = zz9k_surface_color_argb(a, r, g, b);
    return 1;
  case ZZ9K_SURFACE_FORMAT_RGB565:
    *color = zz9k_surface_color_rgb565(r, g, b);
    return 1;
  case ZZ9K_SURFACE_FORMAT_RGB555:
    *color = zz9k_surface_color_rgb555(r, g, b);
    return 1;
  case ZZ9K_SURFACE_FORMAT_INDEX8:
    *color = zz9k_surface_color_index8(index);
    return 1;
  default:
    return 0;
  }
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_SURFACE_H */
