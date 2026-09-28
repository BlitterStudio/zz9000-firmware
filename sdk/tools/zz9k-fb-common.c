/*
 * Shared helpers for ZZ9000 SDK framebuffer demo tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-fb-common.h"
#include "zz9k/surface.h"

#include <string.h>

const char *zz9k_fb_status_name(int status)
{
  switch (status) {
  case ZZ9K_STATUS_OK:
    return "ok";
  case ZZ9K_STATUS_QUEUED:
    return "queued";
  case ZZ9K_STATUS_BUSY:
    return "busy";
  case ZZ9K_STATUS_UNSUPPORTED:
    return "unsupported";
  case ZZ9K_STATUS_BAD_REQUEST:
    return "bad-request";
  case ZZ9K_STATUS_BAD_HANDLE:
    return "bad-handle";
  case ZZ9K_STATUS_NO_MEMORY:
    return "no-memory";
  case ZZ9K_STATUS_TIMEOUT:
    return "timeout";
  case ZZ9K_STATUS_CANCELLED:
    return "cancelled";
  case ZZ9K_STATUS_IO_ERROR:
    return "io-error";
  case ZZ9K_STATUS_NOT_FOUND:
    return "not-found";
  case ZZ9K_STATUS_INTERNAL_ERROR:
    return "internal-error";
  default:
    return "error";
  }
}

const char *zz9k_fb_format_name(uint32_t format)
{
  switch (format) {
  case ZZ9K_SURFACE_FORMAT_RGB565:
    return "rgb565";
  case ZZ9K_SURFACE_FORMAT_ARGB8888:
    return "argb8888";
  case ZZ9K_SURFACE_FORMAT_RGBA8888:
    return "rgba8888";
  case ZZ9K_SURFACE_FORMAT_BGRA8888:
    return "bgra8888";
  case ZZ9K_SURFACE_FORMAT_INDEX8:
    return "index8";
  case ZZ9K_SURFACE_FORMAT_RGB555:
    return "rgb555";
  default:
    return "unsupported";
  }
}

uint32_t zz9k_fb_bytes_per_pixel(uint32_t format)
{
  return zz9k_surface_bytes_per_pixel(format);
}

int zz9k_fb_rect_fits(const ZZ9KSurface *surface, const ZZ9KFbRect *rect,
                      uint32_t bpp)
{
  ZZ9KRect public_rect;

  if (!surface || !rect) {
    return 0;
  }
  public_rect.x = rect->x;
  public_rect.y = rect->y;
  public_rect.w = rect->w;
  public_rect.h = rect->h;
  return zz9k_surface_rect_fits_bpp(surface->width, surface->height,
                                    surface->pitch, surface->length,
                                    &public_rect, bpp);
}

int zz9k_fb_choose_auto_rect(const ZZ9KSurface *surface, uint32_t bpp,
                             ZZ9KFbRect *rect)
{
  uint32_t w;
  uint32_t h;

  if (!surface || !rect || surface->width == 0 || surface->height == 0) {
    return 0;
  }

  w = surface->width < 128U ? surface->width : 128U;
  h = surface->height < 64U ? surface->height : 64U;
  rect->w = w;
  rect->h = h;
  rect->x = (surface->width - w) / 2U;
  rect->y = (surface->height - h) / 2U;

  return zz9k_fb_rect_fits(surface, rect, bpp);
}

int zz9k_fb_build_framebuffer_backup_copy_descs(
    const ZZ9KSurface *backup_surface,
    const ZZ9KFbRect *rect,
    ZZ9KSurfaceCopyDesc *save_copy,
    ZZ9KSurfaceCopyDesc *restore_copy)
{
  ZZ9KRect public_rect;

  if (!backup_surface || !rect || !save_copy || !restore_copy ||
      backup_surface->handle == 0U || rect->w == 0U || rect->h == 0U) {
    return 0;
  }

  public_rect.x = rect->x;
  public_rect.y = rect->y;
  public_rect.w = rect->w;
  public_rect.h = rect->h;
  return zz9k_surface_build_framebuffer_backup_copy_descs(
      save_copy, restore_copy, backup_surface->handle, &public_rect);
}

static int zz9k_fb_rect_extents(const ZZ9KFbRect *rect,
                                uint32_t *right,
                                uint32_t *bottom)
{
  if (!rect || !right || !bottom ||
      rect->w == 0U || rect->h == 0U ||
      rect->x > (0xffffffffU - rect->w) ||
      rect->y > (0xffffffffU - rect->h)) {
    return 0;
  }
  *right = rect->x + rect->w;
  *bottom = rect->y + rect->h;
  return 1;
}

static int zz9k_fb_rect_contains_rect(const ZZ9KFbRect *outer,
                                      const ZZ9KFbRect *inner)
{
  uint32_t outer_right;
  uint32_t outer_bottom;
  uint32_t inner_right;
  uint32_t inner_bottom;

  if (!zz9k_fb_rect_extents(outer, &outer_right, &outer_bottom) ||
      !zz9k_fb_rect_extents(inner, &inner_right, &inner_bottom)) {
    return 0;
  }
  return inner->x >= outer->x && inner->y >= outer->y &&
         inner_right <= outer_right && inner_bottom <= outer_bottom;
}

int zz9k_fb_build_framebuffer_backup_restore_clip_desc(
    const ZZ9KSurface *backup_surface,
    const ZZ9KFbRect *backup_rect,
    const ZZ9KFbRect *clip_rect,
    ZZ9KSurfaceCopyDesc *restore_copy)
{
  ZZ9KRect source_rect;

  if (!backup_surface || backup_surface->handle == 0U ||
      !backup_rect || !clip_rect || !restore_copy ||
      !zz9k_fb_rect_contains_rect(backup_rect, clip_rect)) {
    return 0;
  }

  source_rect.x = clip_rect->x - backup_rect->x;
  source_rect.y = clip_rect->y - backup_rect->y;
  source_rect.w = clip_rect->w;
  source_rect.h = clip_rect->h;
  return zz9k_surface_build_copy_desc(
      restore_copy, backup_surface->handle, ZZ9K_SURFACE_HANDLE_FRAMEBUFFER,
      &source_rect, clip_rect->x, clip_rect->y, 0U);
}

void zz9k_fb_copy_rect_from_surface(uint8_t *dst,
                                    const volatile uint8_t *src,
                                    uint32_t pitch,
                                    const ZZ9KFbRect *rect, uint32_t bpp)
{
  uint32_t row;
  uint32_t col;
  uint32_t row_bytes = rect->w * bpp;

  for (row = 0; row < rect->h; row++) {
    const volatile uint8_t *src_row =
        src + ((rect->y + row) * pitch) + (rect->x * bpp);
    uint8_t *dst_row = dst + (row * row_bytes);

    for (col = 0; col < row_bytes; col++) {
      dst_row[col] = src_row[col];
    }
  }
}

void zz9k_fb_copy_rect_to_surface(volatile uint8_t *dst, const uint8_t *src,
                                  uint32_t pitch, const ZZ9KFbRect *rect,
                                  uint32_t bpp)
{
  uint32_t row;
  uint32_t col;
  uint32_t row_bytes = rect->w * bpp;

  for (row = 0; row < rect->h; row++) {
    volatile uint8_t *dst_row =
        dst + ((rect->y + row) * pitch) + (rect->x * bpp);
    const uint8_t *src_row = src + (row * row_bytes);

    for (col = 0; col < row_bytes; col++) {
      dst_row[col] = src_row[col];
    }
  }
}

int zz9k_fb_copy_backup_clip_to_surface(volatile uint8_t *dst,
                                        const uint8_t *src,
                                        uint32_t dst_pitch,
                                        const ZZ9KFbRect *backup_rect,
                                        const ZZ9KFbRect *clip_rect,
                                        uint32_t bpp)
{
  uint32_t row;
  uint32_t col;
  uint32_t src_pitch;
  uint32_t row_bytes;
  uint32_t src_x;
  uint32_t src_y;

  if (!dst || !src || bpp == 0U ||
      !zz9k_fb_rect_contains_rect(backup_rect, clip_rect) ||
      backup_rect->w > (0xffffffffU / bpp) ||
      clip_rect->w > (0xffffffffU / bpp)) {
    return 0;
  }

  src_pitch = backup_rect->w * bpp;
  row_bytes = clip_rect->w * bpp;
  src_x = clip_rect->x - backup_rect->x;
  src_y = clip_rect->y - backup_rect->y;
  for (row = 0; row < clip_rect->h; row++) {
    const uint8_t *src_row =
        src + ((src_y + row) * src_pitch) + (src_x * bpp);
    volatile uint8_t *dst_row =
        dst + ((clip_rect->y + row) * dst_pitch) + (clip_rect->x * bpp);

    for (col = 0; col < row_bytes; col++) {
      dst_row[col] = src_row[col];
    }
  }
  return 1;
}

static void zz9k_fb_color(uint32_t x, uint32_t y,
                          uint8_t *r, uint8_t *g, uint8_t *b)
{
  uint32_t block = ((x / 16U) + (y / 16U)) & 3U;

  switch (block) {
  case 0:
    *r = 0xffU;
    *g = 0x20U;
    *b = 0x20U;
    break;
  case 1:
    *r = 0x20U;
    *g = 0xd0U;
    *b = 0x40U;
    break;
  case 2:
    *r = 0x20U;
    *g = 0x60U;
    *b = 0xffU;
    break;
  default:
    *r = 0xffU;
    *g = 0xffU;
    *b = 0xffU;
    break;
  }
}

static void zz9k_fb_write_pixel(volatile uint8_t *pixel, uint32_t format,
                                uint32_t x, uint32_t y)
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint16_t value;

  zz9k_fb_color(x, y, &r, &g, &b);

  switch (format) {
  case ZZ9K_SURFACE_FORMAT_ARGB8888:
    pixel[0] = 0xffU;
    pixel[1] = r;
    pixel[2] = g;
    pixel[3] = b;
    break;
  case ZZ9K_SURFACE_FORMAT_RGBA8888:
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = 0xffU;
    break;
  case ZZ9K_SURFACE_FORMAT_BGRA8888:
    pixel[0] = b;
    pixel[1] = g;
    pixel[2] = r;
    pixel[3] = 0xffU;
    break;
  case ZZ9K_SURFACE_FORMAT_RGB565:
    value = (uint16_t)(((uint16_t)(r & 0xf8U) << 8) |
                       ((uint16_t)(g & 0xfcU) << 3) |
                       ((uint16_t)b >> 3));
    pixel[0] = (uint8_t)(value >> 8);
    pixel[1] = (uint8_t)(value & 0xffU);
    break;
  case ZZ9K_SURFACE_FORMAT_RGB555:
    value = (uint16_t)(((uint16_t)(r & 0xf8U) << 7) |
                       ((uint16_t)(g & 0xf8U) << 2) |
                       ((uint16_t)b >> 3));
    pixel[0] = (uint8_t)(value >> 8);
    pixel[1] = (uint8_t)(value & 0xffU);
    break;
  case ZZ9K_SURFACE_FORMAT_INDEX8:
    pixel[0] = (uint8_t)((((x / 16U) + (y / 16U)) & 1U) ? 0xffU : 0x00U);
    break;
  default:
    break;
  }
}

void zz9k_fb_draw_rect(volatile uint8_t *surface_data, uint32_t pitch,
                       const ZZ9KFbRect *rect, uint32_t bpp,
                       uint32_t format)
{
  uint32_t y;
  uint32_t x;

  for (y = 0; y < rect->h; y++) {
    volatile uint8_t *row =
        surface_data + ((rect->y + y) * pitch) + (rect->x * bpp);

    for (x = 0; x < rect->w; x++) {
      zz9k_fb_write_pixel(row + (x * bpp), format, x, y);
    }
  }
}

int zz9k_fb_fill_surface_pattern(const ZZ9KSurface *surface)
{
  ZZ9KFbRect rect;
  uint32_t bpp;

  if (!surface || !surface->data) {
    return 0;
  }

  bpp = zz9k_fb_bytes_per_pixel(surface->format);
  rect.x = 0;
  rect.y = 0;
  rect.w = surface->width;
  rect.h = surface->height;
  if (!zz9k_fb_rect_fits(surface, &rect, bpp)) {
    return 0;
  }

  zz9k_fb_draw_rect((volatile uint8_t *)surface->data, surface->pitch, &rect,
                    bpp, surface->format);
  return 1;
}
