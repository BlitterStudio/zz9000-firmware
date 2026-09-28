/*
 * Tests for public ZZ9000 SDK surface helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/surface.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect_text(const char *actual, const char *expected)
{
  return actual && strcmp(actual, expected) == 0;
}

static int test_bytes_per_pixel_for_supported_formats(void)
{
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_ARGB8888) != 4U) {
    return 1;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGBA8888) != 4U) {
    return 2;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_BGRA8888) != 4U) {
    return 3;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGB565) != 2U) {
    return 4;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGB555) != 2U) {
    return 5;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_INDEX8) != 1U) {
    return 6;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_RGB888) != 3U) {
    return 8;
  }
  if (zz9k_surface_bytes_per_pixel(ZZ9K_SURFACE_FORMAT_PLANAR) != 0U) {
    return 7;
  }
  return 0;
}

static int test_native_rtg_format_helpers(void)
{
  if (zz9k_surface_native_rtg_format() != ZZ9K_SURFACE_FORMAT_BGRA8888) {
    return 1;
  }
  if (!zz9k_surface_is_native_rtg_format(ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    return 2;
  }
  if (zz9k_surface_is_native_rtg_format(ZZ9K_SURFACE_FORMAT_ARGB8888)) {
    return 3;
  }
  if (zz9k_surface_is_native_rtg_format(ZZ9K_SURFACE_FORMAT_UNKNOWN)) {
    return 4;
  }
  return 0;
}

static int test_surface_pitch_and_length_are_checked(void)
{
  uint32_t pitch;
  uint32_t length;

  if (!zz9k_surface_min_pitch(1280U, ZZ9K_SURFACE_FORMAT_BGRA8888,
                              &pitch) ||
      pitch != 5120U) {
    return 1;
  }
  if (!zz9k_surface_length_for_pitch(720U, pitch, &length) ||
      length != 3686400U) {
    return 2;
  }
  if (!zz9k_surface_layout(320U, 200U, ZZ9K_SURFACE_FORMAT_RGB565,
                           &pitch, &length) ||
      pitch != 640U || length != 128000U) {
    return 3;
  }
  if (!zz9k_surface_layout(320U, 200U, ZZ9K_SURFACE_FORMAT_INDEX8,
                           &pitch, &length) ||
      pitch != 320U || length != 64000U) {
    return 4;
  }
  if (!zz9k_surface_layout(10U, 2U, ZZ9K_SURFACE_FORMAT_RGB888,
                           &pitch, &length) ||
      pitch != 30U || length != 60U) {
    return 8;
  }
  if (zz9k_surface_min_pitch(0U, ZZ9K_SURFACE_FORMAT_BGRA8888, &pitch) ||
      zz9k_surface_min_pitch(0x40000000UL, ZZ9K_SURFACE_FORMAT_BGRA8888,
                             &pitch) ||
      zz9k_surface_min_pitch(320U, ZZ9K_SURFACE_FORMAT_PLANAR, &pitch) ||
      zz9k_surface_min_pitch(320U, ZZ9K_SURFACE_FORMAT_BGRA8888, 0)) {
    return 5;
  }
  if (zz9k_surface_length_for_pitch(0U, pitch, &length) ||
      zz9k_surface_length_for_pitch(0x40000000UL, 8U, &length) ||
      zz9k_surface_length_for_pitch(200U, 0U, &length) ||
      zz9k_surface_length_for_pitch(200U, 320U, 0)) {
    return 6;
  }
  if (zz9k_surface_layout(320U, 0U, ZZ9K_SURFACE_FORMAT_BGRA8888,
                          &pitch, &length) ||
      zz9k_surface_layout(320U, 200U, ZZ9K_SURFACE_FORMAT_PLANAR,
                          &pitch, &length) ||
      zz9k_surface_layout(320U, 200U, ZZ9K_SURFACE_FORMAT_BGRA8888,
                          0, &length) ||
      zz9k_surface_layout(320U, 200U, ZZ9K_SURFACE_FORMAT_BGRA8888,
                          &pitch, 0)) {
    return 7;
  }

  return 0;
}

static int test_surface_flag_names(void)
{
  if (zz9k_known_surface_flag_count() != 5U) return 1;
  if (zz9k_known_surface_flag(0) != ZZ9K_SURFACE_FLAG_CPU_VISIBLE) return 2;
  if (zz9k_known_surface_flag(4) != ZZ9K_SURFACE_FLAG_ARM_LOCAL) return 3;
  if (zz9k_known_surface_flag(5) != 0U) return 4;
  if (!expect_text(zz9k_surface_flag_name(ZZ9K_SURFACE_FLAG_CPU_VISIBLE),
                   "cpu-visible")) return 5;
  if (!expect_text(zz9k_surface_flag_name(ZZ9K_SURFACE_FLAG_ARM_LOCAL),
                   "arm-local")) return 6;
  if (zz9k_surface_flag_name(0x80000000UL) != 0) return 7;
  return 0;
}

static int test_surface_rect_fits_layout(void)
{
  ZZ9KRect rect;

  rect.x = 1U;
  rect.y = 2U;
  rect.w = 3U;
  rect.h = 4U;

  if (!zz9k_surface_rect_fits_bpp(20U, 20U, 80U, 1600U, &rect, 4U)) {
    return 1;
  }
  if (!zz9k_surface_rect_fits(20U, 20U, 80U, 1600U, &rect,
                              ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    return 2;
  }

  rect.x = 19U;
  rect.y = 0U;
  rect.w = 2U;
  rect.h = 1U;
  if (zz9k_surface_rect_fits(20U, 20U, 80U, 1600U, &rect,
                             ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    return 3;
  }

  rect.x = 0U;
  rect.y = 1U;
  rect.w = 20U;
  rect.h = 2U;
  if (zz9k_surface_rect_fits(20U, 20U, 80U, 80U, &rect,
                             ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    return 4;
  }
  if (zz9k_surface_rect_fits(20U, 20U, 40U, 1600U, &rect,
                             ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    return 5;
  }
  if (zz9k_surface_rect_fits(20U, 20U, 80U, 1600U, &rect,
                             ZZ9K_SURFACE_FORMAT_PLANAR) ||
      zz9k_surface_rect_fits_bpp(20U, 20U, 80U, 1600U, &rect, 0U) ||
      zz9k_surface_rect_fits(20U, 20U, 80U, 1600U, 0,
                             ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    return 6;
  }

  return 0;
}

static int test_surface_operation_descriptors(void)
{
  ZZ9KRect rect;
  ZZ9KSurfaceFillDesc fill;
  ZZ9KSurfaceCopyDesc copy;
  ZZ9KSurfaceCopyDesc save;
  ZZ9KSurfaceCopyDesc restore;

  rect.x = 16U;
  rect.y = 24U;
  rect.w = 128U;
  rect.h = 64U;

  if (!zz9k_surface_build_fill_desc(&fill, 0x40000022UL, &rect,
                                    0xff336699UL, 0x40U)) {
    return 1;
  }
  if (fill.surface != 0x40000022UL ||
      fill.x != 16U || fill.y != 24U ||
      fill.width != 128U || fill.height != 64U ||
      fill.color != 0xff336699UL || fill.flags != 0x40U) {
    return 2;
  }

  if (!zz9k_surface_build_framebuffer_fill_desc(
        &fill, &rect, 0xff000000UL, 0U) ||
      fill.surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
      fill.color != 0xff000000UL) {
    return 3;
  }

  if (!zz9k_surface_build_copy_desc(&copy, 0x40000011UL,
                                    0x40000022UL, &rect, 3U, 4U,
                                    0x80U)) {
    return 4;
  }
  if (copy.src_surface != 0x40000011UL ||
      copy.dst_surface != 0x40000022UL ||
      copy.src_x != 16U || copy.src_y != 24U ||
      copy.dst_x != 3U || copy.dst_y != 4U ||
      copy.width != 128U || copy.height != 64U ||
      copy.flags != 0x80U) {
    return 5;
  }

  if (!zz9k_surface_build_framebuffer_backup_copy_descs(
        &save, &restore, 0x40003000UL, &rect)) {
    return 6;
  }
  if (save.src_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
      save.dst_surface != 0x40003000UL ||
      save.src_x != 16U || save.src_y != 24U ||
      save.dst_x != 0U || save.dst_y != 0U ||
      save.width != 128U || save.height != 64U ||
      save.flags != 0U) {
    return 7;
  }
  if (restore.src_surface != 0x40003000UL ||
      restore.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
      restore.src_x != 0U || restore.src_y != 0U ||
      restore.dst_x != 16U || restore.dst_y != 24U ||
      restore.width != 128U || restore.height != 64U ||
      restore.flags != 0U) {
    return 8;
  }

  rect.w = 0U;
  if (zz9k_surface_build_fill_desc(&fill, 0x40000022UL, &rect,
                                   0U, 0U) ||
      zz9k_surface_build_copy_desc(&copy, 0x40000011UL,
                                   0x40000022UL, &rect, 0U, 0U,
                                   0U) ||
      zz9k_surface_build_framebuffer_backup_copy_descs(
        &save, &restore, 0x40003000UL, &rect)) {
    return 9;
  }
  rect.w = 128U;
  if (zz9k_surface_build_fill_desc(&fill, ZZ9K_INVALID_HANDLE, &rect,
                                   0U, 0U) ||
      zz9k_surface_build_copy_desc(&copy, ZZ9K_INVALID_HANDLE,
                                   0x40000022UL, &rect, 0U, 0U,
                                   0U) ||
      zz9k_surface_build_copy_desc(&copy, 0x40000011UL,
                                   ZZ9K_INVALID_HANDLE, &rect, 0U, 0U,
                                   0U) ||
      zz9k_surface_build_framebuffer_backup_copy_descs(
        &save, &restore, ZZ9K_INVALID_HANDLE, &rect)) {
    return 10;
  }

  return 0;
}

static int test_surface_color_helpers(void)
{
  uint32_t color;

  if (zz9k_surface_color_argb(0x12U, 0x34U, 0x56U, 0x78U) !=
      0x12345678UL) {
    return 1;
  }
  if (zz9k_surface_color_rgb(0x34U, 0x56U, 0x78U) != 0xff345678UL) {
    return 2;
  }
  if (zz9k_surface_color_rgb565(0xffU, 0x80U, 0x20U) != 0xfc04U) {
    return 3;
  }
  if (zz9k_surface_color_rgb555(0xffU, 0x80U, 0x20U) != 0x7e04U) {
    return 4;
  }
  if (zz9k_surface_color_index8(0x5aU) != 0x5aU) {
    return 5;
  }
  if (!zz9k_surface_color_for_format(ZZ9K_SURFACE_FORMAT_BGRA8888,
                                     0x20U, 0x70U, 0xffU, 0xffU,
                                     0U, &color) ||
      color != 0xff2070ffUL) {
    return 6;
  }
  if (!zz9k_surface_color_for_format(ZZ9K_SURFACE_FORMAT_RGB888,
                                     0x20U, 0x70U, 0xffU, 0xffU,
                                     0U, &color) ||
      color != 0xff2070ffUL) {
    return 10;
  }
  if (!zz9k_surface_color_for_format(ZZ9K_SURFACE_FORMAT_RGB565,
                                     0xffU, 0x80U, 0x20U, 0xffU,
                                     0U, &color) ||
      color != 0xfc04U) {
    return 7;
  }
  if (!zz9k_surface_color_for_format(ZZ9K_SURFACE_FORMAT_INDEX8,
                                     0U, 0U, 0U, 0xffU, 0x5aU,
                                     &color) ||
      color != 0x5aU) {
    return 8;
  }
  if (zz9k_surface_color_for_format(ZZ9K_SURFACE_FORMAT_PLANAR,
                                    0U, 0U, 0U, 0xffU, 0U,
                                    &color) ||
      zz9k_surface_color_for_format(ZZ9K_SURFACE_FORMAT_BGRA8888,
                                    0U, 0U, 0U, 0xffU, 0U, 0)) {
    return 9;
  }

  return 0;
}

int main(void)
{
  int result;

  result = test_bytes_per_pixel_for_supported_formats();
  if (result) {
    printf("bytes-per-pixel test failed: %d\n", result);
    return 10 + result;
  }

  result = test_native_rtg_format_helpers();
  if (result) {
    printf("native RTG format helper test failed: %d\n", result);
    return 25 + result;
  }

  result = test_surface_pitch_and_length_are_checked();
  if (result) {
    printf("surface layout test failed: %d\n", result);
    return 30 + result;
  }

  result = test_surface_flag_names();
  if (result) {
    printf("surface flag helper test failed: %d\n", result);
    return 40 + result;
  }

  result = test_surface_rect_fits_layout();
  if (result) {
    printf("surface rect fit test failed: %d\n", result);
    return 50 + result;
  }

  result = test_surface_operation_descriptors();
  if (result) {
    printf("surface operation descriptor test failed: %d\n", result);
    return 70 + result;
  }

  result = test_surface_color_helpers();
  if (result) {
    printf("surface color helper test failed: %d\n", result);
    return 90 + result;
  }

  return 0;
}
