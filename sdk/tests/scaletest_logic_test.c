/*
 * Unit checks for the ARM scaler-to-framebuffer demo helper logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_SCALETEST_NO_MAIN
#include "../tools/zz9k-scaletest.c"

#include <stdint.h>
#include <string.h>

static int test_scale_rects_are_centered_and_halved(void)
{
  ZZ9KSurface framebuffer;
  ZZ9KFbRect src;
  ZZ9KFbRect dst;

  memset(&framebuffer, 0, sizeof(framebuffer));
  framebuffer.width = 320;
  framebuffer.height = 200;
  framebuffer.pitch = 320 * 4;
  framebuffer.format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  framebuffer.flags = ZZ9K_SURFACE_FLAG_CPU_VISIBLE |
                      ZZ9K_SURFACE_FLAG_FRAMEBUFFER;
  framebuffer.length = framebuffer.pitch * framebuffer.height;

  if (!zz9k_scaletest_choose_rects(&framebuffer, 4, &src, &dst)) return 1;
  if (dst.x != 96 || dst.y != 68) return 2;
  if (dst.w != 128 || dst.h != 64) return 3;
  if (src.x != 0 || src.y != 0) return 4;
  if (src.w != 64 || src.h != 32) return 5;

  framebuffer.width = 80;
  framebuffer.height = 40;
  framebuffer.pitch = 80 * 4;
  framebuffer.length = framebuffer.pitch * framebuffer.height;

  if (!zz9k_scaletest_choose_rects(&framebuffer, 4, &src, &dst)) return 6;
  if (dst.x != 0 || dst.y != 0) return 7;
  if (dst.w != 80 || dst.h != 40) return 8;
  if (src.w != 40 || src.h != 20) return 9;

  return 0;
}

static int test_scale_descriptor_targets_framebuffer(void)
{
  ZZ9KScaleImageDesc desc;
  ZZ9KScaleImageClippedDesc clipped;
  ZZ9KFbRect src;
  ZZ9KFbRect dst;
  ZZ9KFbRect clip;

  memset(&desc, 0, sizeof(desc));
  src.x = 0;
  src.y = 0;
  src.w = 64;
  src.h = 32;
  dst.x = 96;
  dst.y = 68;
  dst.w = 128;
  dst.h = 64;

  if (!zz9k_scaletest_build_desc(&desc, 0x40000011UL, &src, &dst,
                                 ZZ9K_SCALE_BILINEAR)) {
    return 1;
  }

  if (desc.src_surface != 0x40000011UL) return 2;
  if (desc.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 3;
  if (desc.src_x != 0 || desc.src_y != 0) return 4;
  if (desc.src_w != 64 || desc.src_h != 32) return 5;
  if (desc.dst_x != 96 || desc.dst_y != 68) return 6;
  if (desc.dst_w != 128 || desc.dst_h != 64) return 7;
  if (desc.filter != ZZ9K_SCALE_BILINEAR) return 8;
  if (desc.flags != 0) return 9;
  if (zz9k_scaletest_build_desc(&desc, ZZ9K_INVALID_HANDLE, &src,
                                &dst, ZZ9K_SCALE_BILINEAR)) {
    return 10;
  }

  if (!zz9k_scaletest_choose_clip_rect(&dst, &clip)) return 11;
  if (clip.x != 128 || clip.y != 84) return 12;
  if (clip.w != 64 || clip.h != 32) return 13;

  memset(&clipped, 0, sizeof(clipped));
  if (!zz9k_scaletest_build_clipped_desc(&clipped, 0x40000011UL, &src,
                                         &dst, &clip,
                                         ZZ9K_SCALE_BILINEAR)) {
    return 14;
  }

  if (clipped.src_surface != 0x40000011UL) return 15;
  if (clipped.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 16;
  if (clipped.src_w != 64 || clipped.src_h != 32) return 17;
  if (clipped.dst_x != 96 || clipped.dst_y != 68) return 18;
  if (clipped.dst_w != 128 || clipped.dst_h != 64) return 19;
  if (clipped.clip_x != 128 || clipped.clip_y != 84) return 20;
  if (clipped.clip_w != 64 || clipped.clip_h != 32) return 21;
  if (clipped.filter != ZZ9K_SCALE_BILINEAR) return 22;
  if (clipped.flags != 0) return 23;
  if (zz9k_scaletest_build_clipped_desc(&clipped, ZZ9K_INVALID_HANDLE,
                                        &src, &dst, &clip,
                                        ZZ9K_SCALE_BILINEAR)) {
    return 24;
  }

  return 0;
}

static int test_clear_descriptor_targets_framebuffer(void)
{
  ZZ9KSurfaceFillDesc fill;
  ZZ9KFbRect rect;

  memset(&fill, 0xa5, sizeof(fill));
  rect.x = 11;
  rect.y = 17;
  rect.w = 64;
  rect.h = 32;

  if (!zz9k_scaletest_build_framebuffer_fill_desc(&fill, &rect)) return 1;
  if (fill.surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 2;
  if (fill.x != 11 || fill.y != 17) return 3;
  if (fill.width != 64 || fill.height != 32) return 4;
  if (fill.color != 0xff000000UL) return 5;
  if (fill.flags != 0) return 6;

  rect.w = 0;
  if (zz9k_scaletest_build_framebuffer_fill_desc(&fill, &rect)) return 7;

  return 0;
}

int main(void)
{
  int result;

  result = test_scale_rects_are_centered_and_halved();
  if (result) return 10 + result;

  result = test_scale_descriptor_targets_framebuffer();
  if (result) return 40 + result;

  result = test_clear_descriptor_targets_framebuffer();
  if (result) return 80 + result;

  return 0;
}
