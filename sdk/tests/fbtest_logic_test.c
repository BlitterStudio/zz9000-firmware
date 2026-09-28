/*
 * Unit checks for the reversible framebuffer write test helper logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../tools/zz9k-fb-common.h"

#include <stdint.h>
#include <string.h>

static int test_bytes_per_pixel_for_supported_formats(void)
{
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_ARGB8888) != 4) {
    return 1;
  }
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGBA8888) != 4) {
    return 2;
  }
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_BGRA8888) != 4) {
    return 7;
  }
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGB565) != 2) {
    return 3;
  }
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGB555) != 2) {
    return 4;
  }
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_INDEX8) != 1) {
    return 5;
  }
  if (zz9k_fb_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_PLANAR) != 0) {
    return 6;
  }
  return 0;
}

static int test_auto_rect_is_centered_and_bounded(void)
{
  ZZ9KSurface surface;
  ZZ9KFbRect rect;

  memset(&surface, 0, sizeof(surface));
  memset(&rect, 0, sizeof(rect));
  surface.width = 320;
  surface.height = 200;
  surface.pitch = 320 * 4;
  surface.format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  surface.flags = ZZ9K_SURFACE_FLAG_CPU_VISIBLE |
                  ZZ9K_SURFACE_FLAG_FRAMEBUFFER;
  surface.length = surface.pitch * surface.height;

  if (!zz9k_fb_choose_auto_rect(&surface, 4, &rect)) return 1;
  if (rect.w != 128) return 2;
  if (rect.h != 64) return 3;
  if (rect.x != 96) return 4;
  if (rect.y != 68) return 5;
  if (!zz9k_fb_rect_fits(&surface, &rect, 4)) return 6;

  surface.width = 80;
  surface.height = 40;
  surface.pitch = 80 * 4;
  surface.length = surface.pitch * surface.height;

  if (!zz9k_fb_choose_auto_rect(&surface, 4, &rect)) return 7;
  if (rect.w != 80) return 8;
  if (rect.h != 40) return 9;
  if (rect.x != 0 || rect.y != 0) return 10;
  if (!zz9k_fb_rect_fits(&surface, &rect, 4)) return 11;

  return 0;
}

static int test_copy_draw_and_restore_respects_pitch(void)
{
  uint8_t framebuffer[4 * 10];
  uint8_t backup[2 * 3];
  ZZ9KFbRect rect;
  unsigned int i;

  for (i = 0; i < sizeof(framebuffer); i++) {
    framebuffer[i] = (uint8_t)i;
  }
  memset(backup, 0, sizeof(backup));
  rect.x = 1;
  rect.y = 1;
  rect.w = 3;
  rect.h = 2;

  zz9k_fb_copy_rect_from_surface(backup, framebuffer, 10, &rect, 1);
  if (backup[0] != 11) return 1;
  if (backup[1] != 12) return 2;
  if (backup[2] != 13) return 3;
  if (backup[3] != 21) return 4;
  if (backup[4] != 22) return 5;
  if (backup[5] != 23) return 6;

  zz9k_fb_draw_rect(framebuffer, 10, &rect, 1,
                    ZZ9K_SURFACE_FORMAT_INDEX8);
  if (framebuffer[0] != 0) return 7;
  if (framebuffer[10] != 10) return 8;
  if (framebuffer[11] == 11) return 9;
  if (framebuffer[21] == 21) return 10;
  if (framebuffer[24] != 24) return 11;

  zz9k_fb_copy_rect_to_surface(framebuffer, backup, 10, &rect, 1);
  for (i = 0; i < sizeof(framebuffer); i++) {
    if (framebuffer[i] != (uint8_t)i) {
      return 20 + (int)i;
    }
  }

  return 0;
}

static int test_bgra_draw_writes_native_rtg_byte_order(void)
{
  uint8_t framebuffer[4];
  ZZ9KFbRect rect;

  memset(framebuffer, 0, sizeof(framebuffer));
  rect.x = 0;
  rect.y = 0;
  rect.w = 1;
  rect.h = 1;

  zz9k_fb_draw_rect(framebuffer, 4, &rect, 4,
                    ZZ9K_SURFACE_FORMAT_BGRA8888);

  if (framebuffer[0] != 0x20U) return 1;
  if (framebuffer[1] != 0x20U) return 2;
  if (framebuffer[2] != 0xffU) return 3;
  if (framebuffer[3] != 0xffU) return 4;

  return 0;
}

static int test_rect_rejects_short_surface_length(void)
{
  ZZ9KSurface surface;
  ZZ9KFbRect rect;

  memset(&surface, 0, sizeof(surface));
  surface.width = 20;
  surface.height = 20;
  surface.pitch = 80;
  surface.length = 80;
  rect.x = 0;
  rect.y = 1;
  rect.w = 20;
  rect.h = 2;

  if (zz9k_fb_rect_fits(&surface, &rect, 4)) return 1;
  return 0;
}

static int test_framebuffer_backup_descriptors_target_arm_surface(void)
{
  ZZ9KSurface backup;
  ZZ9KFbRect rect;
  ZZ9KFbRect clip;
  ZZ9KSurfaceCopyDesc save;
  ZZ9KSurfaceCopyDesc restore;
  ZZ9KSurfaceCopyDesc clipped_restore;

  memset(&backup, 0, sizeof(backup));
  backup.handle = 0x40003000UL;
  rect.x = 16U;
  rect.y = 24U;
  rect.w = 128U;
  rect.h = 64U;

  memset(&save, 0xff, sizeof(save));
  memset(&restore, 0xff, sizeof(restore));
  if (!zz9k_fb_build_framebuffer_backup_copy_descs(
        &backup, &rect, &save, &restore)) {
    return 1;
  }

  if (save.src_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 2;
  if (save.dst_surface != backup.handle) return 3;
  if (save.src_x != 16U || save.src_y != 24U) return 4;
  if (save.dst_x != 0U || save.dst_y != 0U) return 5;
  if (save.width != 128U || save.height != 64U) return 6;
  if (save.flags != 0U) return 7;

  if (restore.src_surface != backup.handle) return 8;
  if (restore.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 9;
  if (restore.src_x != 0U || restore.src_y != 0U) return 10;
  if (restore.dst_x != 16U || restore.dst_y != 24U) return 11;
  if (restore.width != 128U || restore.height != 64U) return 12;
  if (restore.flags != 0U) return 13;

  clip.x = 40U;
  clip.y = 48U;
  clip.w = 32U;
  clip.h = 12U;
  memset(&clipped_restore, 0xee, sizeof(clipped_restore));
  if (!zz9k_fb_build_framebuffer_backup_restore_clip_desc(
        &backup, &rect, &clip, &clipped_restore)) {
    return 14;
  }
  if (clipped_restore.src_surface != backup.handle) return 15;
  if (clipped_restore.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 16;
  if (clipped_restore.src_x != 24U || clipped_restore.src_y != 24U) return 17;
  if (clipped_restore.dst_x != 40U || clipped_restore.dst_y != 48U) return 18;
  if (clipped_restore.width != 32U || clipped_restore.height != 12U) return 19;
  if (clipped_restore.flags != 0U) return 20;

  clip.x = 10U;
  if (zz9k_fb_build_framebuffer_backup_restore_clip_desc(
        &backup, &rect, &clip, &clipped_restore)) {
    return 21;
  }

  return 0;
}

static int test_copy_backup_clip_to_surface_respects_backup_offset(void)
{
  uint8_t framebuffer[8 * 6];
  uint8_t backup[4 * 3];
  ZZ9KFbRect backup_rect;
  ZZ9KFbRect clip;
  int i;

  memset(framebuffer, 0, sizeof(framebuffer));
  for (i = 0; i < (int)sizeof(backup); i++) {
    backup[i] = (uint8_t)(0x80 + i);
  }

  backup_rect.x = 2U;
  backup_rect.y = 1U;
  backup_rect.w = 4U;
  backup_rect.h = 3U;
  clip.x = 3U;
  clip.y = 2U;
  clip.w = 2U;
  clip.h = 2U;

  if (!zz9k_fb_copy_backup_clip_to_surface(
        framebuffer, backup, 8U, &backup_rect, &clip, 1U)) {
    return 1;
  }
  if (framebuffer[(2U * 8U) + 3U] != backup[(1U * 4U) + 1U]) return 2;
  if (framebuffer[(2U * 8U) + 4U] != backup[(1U * 4U) + 2U]) return 3;
  if (framebuffer[(3U * 8U) + 3U] != backup[(2U * 4U) + 1U]) return 4;
  if (framebuffer[(3U * 8U) + 4U] != backup[(2U * 4U) + 2U]) return 5;
  if (framebuffer[(1U * 8U) + 2U] != 0U) return 6;

  clip.x = 1U;
  if (zz9k_fb_copy_backup_clip_to_surface(
        framebuffer, backup, 8U, &backup_rect, &clip, 1U)) {
    return 7;
  }

  return 0;
}

int main(void)
{
  int result;

  result = test_bytes_per_pixel_for_supported_formats();
  if (result) return 10 + result;

  result = test_auto_rect_is_centered_and_bounded();
  if (result) return 30 + result;

  result = test_copy_draw_and_restore_respects_pitch();
  if (result) return 60 + result;

  result = test_bgra_draw_writes_native_rtg_byte_order();
  if (result) return 90 + result;

  result = test_rect_rejects_short_surface_length();
  if (result) return 110 + result;

  result = test_framebuffer_backup_descriptors_target_arm_surface();
  if (result) return 130 + result;

  result = test_copy_backup_clip_to_surface_respects_backup_offset();
  if (result) return 160 + result;

  return 0;
}
