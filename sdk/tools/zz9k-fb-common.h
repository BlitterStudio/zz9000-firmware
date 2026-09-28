/*
 * Shared helpers for ZZ9000 SDK framebuffer demo tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_FB_COMMON_H
#define ZZ9K_FB_COMMON_H

#include "zz9k/host.h"
#include <stdint.h>

typedef struct ZZ9KFbRect {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
} ZZ9KFbRect;

const char *zz9k_fb_status_name(int status);
const char *zz9k_fb_format_name(uint32_t format);
uint32_t zz9k_fb_bytes_per_pixel(uint32_t format);
int zz9k_fb_rect_fits(const ZZ9KSurface *surface, const ZZ9KFbRect *rect,
                      uint32_t bpp);
int zz9k_fb_choose_auto_rect(const ZZ9KSurface *surface, uint32_t bpp,
                             ZZ9KFbRect *rect);
int zz9k_fb_build_framebuffer_backup_copy_descs(
    const ZZ9KSurface *backup_surface,
    const ZZ9KFbRect *rect,
    ZZ9KSurfaceCopyDesc *save_copy,
    ZZ9KSurfaceCopyDesc *restore_copy);
int zz9k_fb_build_framebuffer_backup_restore_clip_desc(
    const ZZ9KSurface *backup_surface,
    const ZZ9KFbRect *backup_rect,
    const ZZ9KFbRect *clip_rect,
    ZZ9KSurfaceCopyDesc *restore_copy);
void zz9k_fb_copy_rect_from_surface(uint8_t *dst,
                                    const volatile uint8_t *src,
                                    uint32_t pitch, const ZZ9KFbRect *rect,
                                    uint32_t bpp);
void zz9k_fb_copy_rect_to_surface(volatile uint8_t *dst, const uint8_t *src,
                                  uint32_t pitch, const ZZ9KFbRect *rect,
                                  uint32_t bpp);
int zz9k_fb_copy_backup_clip_to_surface(volatile uint8_t *dst,
                                        const uint8_t *src,
                                        uint32_t dst_pitch,
                                        const ZZ9KFbRect *backup_rect,
                                        const ZZ9KFbRect *clip_rect,
                                        uint32_t bpp);
void zz9k_fb_draw_rect(volatile uint8_t *surface_data, uint32_t pitch,
                       const ZZ9KFbRect *rect, uint32_t bpp,
                       uint32_t format);
int zz9k_fb_fill_surface_pattern(const ZZ9KSurface *surface);

#endif /* ZZ9K_FB_COMMON_H */
