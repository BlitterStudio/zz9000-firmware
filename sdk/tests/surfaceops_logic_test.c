/*
 * Unit checks for the ARM surfaceops framebuffer smoke-test helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_SURFACEOPS_NO_MAIN
#include "../tools/zz9k-surfaceops.c"

#include <stdint.h>
#include <string.h>

static int test_surfaceops_rect_setup_does_not_require_cpu_mapping(void)
{
  ZZ9KSurface framebuffer;
  ZZ9KFbRect draw_rect;
  ZZ9KFbRect backup_rect;
  uint32_t bpp;
  uint32_t draw_row_bytes;
  uint32_t backup_row_bytes;

  memset(&framebuffer, 0, sizeof(framebuffer));
  framebuffer.width = 640U;
  framebuffer.height = 480U;
  framebuffer.pitch = 640U * 4U;
  framebuffer.length = framebuffer.pitch * framebuffer.height;
  framebuffer.format = ZZ9K_SURFACE_FORMAT_BGRA8888;
  framebuffer.flags = ZZ9K_SURFACE_FLAG_FRAMEBUFFER;
  framebuffer.data = 0;

  if (!zz9k_surfaceops_prepare_rects(&framebuffer, &draw_rect,
                                     &backup_rect, &bpp,
                                     &draw_row_bytes,
                                     &backup_row_bytes)) {
    return 1;
  }
  if (bpp != 4U) return 2;
  if (draw_row_bytes != draw_rect.w * bpp) return 3;
  if (backup_row_bytes != backup_rect.w * bpp) return 4;
  if (draw_rect.x != 256U || draw_rect.y != 208U) return 5;
  if (draw_rect.w != 128U || draw_rect.h != 64U) return 6;
  if (backup_rect.x != 248U || backup_rect.y != 200U) return 7;
  if (backup_rect.w != 144U || backup_rect.h != 80U) return 8;

  return 0;
}

static int test_surfaceops_rect_setup_stays_inside_window_area(void)
{
  ZZ9KSurface framebuffer;
  ZZ9KFbRect window_area;
  ZZ9KFbRect draw_rect;
  ZZ9KFbRect backup_rect;
  uint32_t bpp;
  uint32_t draw_row_bytes;
  uint32_t backup_row_bytes;

  memset(&framebuffer, 0, sizeof(framebuffer));
  framebuffer.width = 640U;
  framebuffer.height = 480U;
  framebuffer.pitch = 640U * 4U;
  framebuffer.length = framebuffer.pitch * framebuffer.height;
  framebuffer.format = ZZ9K_SURFACE_FORMAT_BGRA8888;
  framebuffer.flags = ZZ9K_SURFACE_FLAG_FRAMEBUFFER;

  window_area.x = 50U;
  window_area.y = 40U;
  window_area.w = 240U;
  window_area.h = 120U;

  if (!zz9k_surfaceops_prepare_rects_in_area(
        &framebuffer, &window_area, &draw_rect, &backup_rect, &bpp,
        &draw_row_bytes, &backup_row_bytes)) {
    return 1;
  }
  if (draw_rect.x != 106U || draw_rect.y != 68U) return 2;
  if (draw_rect.w != 128U || draw_rect.h != 64U) return 3;
  if (backup_rect.x != 98U || backup_rect.y != 60U) return 4;
  if (backup_rect.w != 144U || backup_rect.h != 80U) return 5;
  if (draw_row_bytes != 512U) return 6;
  if (backup_row_bytes != 576U) return 7;

  return 0;
}

static int test_surfaceops_backup_descriptors_stay_arm_side(void)
{
  ZZ9KSurface backup;
  ZZ9KFbRect rect;
  ZZ9KSurfaceCopyDesc save;
  ZZ9KSurfaceCopyDesc restore;

  memset(&backup, 0, sizeof(backup));
  backup.handle = 0x40003000UL;
  backup.flags = ZZ9K_SURFACE_FLAG_ARM_LOCAL;
  rect.x = 16U;
  rect.y = 24U;
  rect.w = 128U;
  rect.h = 64U;

  memset(&save, 0xff, sizeof(save));
  memset(&restore, 0xff, sizeof(restore));
  if (!zz9k_surfaceops_build_backup_copy_descs(&backup, &rect, &save,
                                               &restore)) {
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

  return 0;
}

static int test_surfaceops_source_copy_targets_framebuffer(void)
{
  ZZ9KFbRect rect;
  ZZ9KSurfaceCopyDesc copy;

  rect.x = 32U;
  rect.y = 40U;
  rect.w = 96U;
  rect.h = 48U;

  memset(&copy, 0xff, sizeof(copy));
  if (!zz9k_surfaceops_build_source_copy_desc(&copy, 0x40004000UL,
                                              &rect)) {
    return 1;
  }

  if (copy.src_surface != 0x40004000UL) return 2;
  if (copy.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 3;
  if (copy.src_x != 0U || copy.src_y != 0U) return 4;
  if (copy.dst_x != 32U || copy.dst_y != 40U) return 5;
  if (copy.width != 96U || copy.height != 48U) return 6;
  if (copy.flags != 0U) return 7;
  if (zz9k_surfaceops_build_source_copy_desc(&copy, ZZ9K_INVALID_HANDLE,
                                             &rect)) {
    return 8;
  }
  rect.w = 0U;
  if (zz9k_surfaceops_build_source_copy_desc(&copy, 0x40004000UL,
                                             &rect)) {
    return 9;
  }

  return 0;
}

static int test_surfaceops_window_config_uses_shared_helper(void)
{
  ZZ9KSurface framebuffer;
  ZZ9KImageWindowConfig config;
  uint32_t width;
  uint32_t height;

  memset(&framebuffer, 0, sizeof(framebuffer));
  framebuffer.width = 1280U;
  framebuffer.height = 720U;
  framebuffer.pitch = 1280U * 4U;
  framebuffer.length = framebuffer.pitch * framebuffer.height;
  framebuffer.format = ZZ9K_SURFACE_FORMAT_BGRA8888;

  zz9k_surfaceops_window_config_init(&config);
  if (config.resizable) return 1;
  if (config.close_gadget) return 2;
  if (!zz9k_image_window_choose_size(&framebuffer, &config,
                                     &width, &height)) {
    return 3;
  }
  if (width != 360U || height != 180U) return 4;

  framebuffer.width = 320U;
  framebuffer.height = 160U;
  if (!zz9k_image_window_choose_size(&framebuffer, &config,
                                     &width, &height)) {
    return 5;
  }
  if (width != 320U || height != 160U) return 6;

  return 0;
}

static int test_surfaceops_parse_legacy_and_loop_options(void)
{
  ZZ9KSurfaceopsOptions options;
  char *legacy[] = {"zz9k-surfaceops", "25"};
  char *looped[] = {
    "zz9k-surfaceops", "--hold-ticks", "0", "--loops", "17", "--stats"
  };
  char *bucketed[] = {
    "zz9k-surfaceops", "--hold-ticks", "0", "--loops", "1000",
    "--stats", "--stats-interval", "100"
  };
  char *too_many_positionals[] = {"zz9k-surfaceops", "5", "6"};
  char *bad_loop_count[] = {"zz9k-surfaceops", "--loops", "0"};
  char *negative_hold[] = {"zz9k-surfaceops", "--hold-ticks", "-1"};
  char *bad_stats_interval[] = {"zz9k-surfaceops", "--stats-interval", "0"};
  char *interval_without_stats[] = {
    "zz9k-surfaceops", "--loops", "1000", "--stats-interval", "100"
  };

  if (!zz9k_surfaceops_parse_options(2, legacy, &options)) return 1;
  if (options.hold_ticks != 25U) return 2;
  if (options.loops != 1U) return 3;
  if (options.stats) return 4;

  if (!zz9k_surfaceops_parse_options(6, looped, &options)) return 5;
  if (options.hold_ticks != 0U) return 6;
  if (options.loops != 17U) return 7;
  if (!options.stats) return 8;
  if (options.stats_interval != 0U) return 9;

  if (!zz9k_surfaceops_parse_options(8, bucketed, &options)) return 10;
  if (options.hold_ticks != 0U) return 11;
  if (options.loops != 1000U) return 12;
  if (!options.stats) return 13;
  if (options.stats_interval != 100U) return 14;

  if (zz9k_surfaceops_parse_options(3, too_many_positionals, &options)) {
    return 15;
  }
  if (zz9k_surfaceops_parse_options(3, bad_loop_count, &options)) {
    return 16;
  }
  if (zz9k_surfaceops_parse_options(3, negative_hold, &options)) {
    return 17;
  }
  if (zz9k_surfaceops_parse_options(3, bad_stats_interval, &options)) {
    return 18;
  }
  if (zz9k_surfaceops_parse_options(5, interval_without_stats, &options)) {
    return 19;
  }

  return 0;
}

static int test_surfaceops_stats_math_is_stable(void)
{
  if (zz9k_surfaceops_ticks_to_ms(125ULL, 50U) != 2500U) return 1;
  if (zz9k_surfaceops_ticks_to_ms(1ULL, 0U) != 0U) return 2;
  if (zz9k_surfaceops_loops_per_second_x100(17U, 2000U) != 850U) return 3;
  if (zz9k_surfaceops_loops_per_second_x100(17U, 0U) != 0U) return 4;

  return 0;
}

static int test_surfaceops_stats_bucket_selection(void)
{
  if (zz9k_surfaceops_should_print_stats_bucket(0U, 100U)) return 1;
  if (zz9k_surfaceops_should_print_stats_bucket(99U, 100U)) return 2;
  if (!zz9k_surfaceops_should_print_stats_bucket(100U, 100U)) return 3;
  if (!zz9k_surfaceops_should_print_stats_bucket(200U, 100U)) return 4;
  if (zz9k_surfaceops_should_print_stats_bucket(200U, 0U)) return 5;

  return 0;
}

int main(void)
{
  int result;

  result = test_surfaceops_rect_setup_does_not_require_cpu_mapping();
  if (result) return 10 + result;

  result = test_surfaceops_rect_setup_stays_inside_window_area();
  if (result) return 25 + result;

  result = test_surfaceops_backup_descriptors_stay_arm_side();
  if (result) return 40 + result;

  result = test_surfaceops_source_copy_targets_framebuffer();
  if (result) return 70 + result;

  result = test_surfaceops_window_config_uses_shared_helper();
  if (result) return 90 + result;

  result = test_surfaceops_parse_legacy_and_loop_options();
  if (result) return 110 + result;

  result = test_surfaceops_stats_math_is_stable();
  if (result) return 130 + result;

  result = test_surfaceops_stats_bucket_selection();
  if (result) return 145 + result;

  return 0;
}
