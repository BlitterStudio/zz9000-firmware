/*
 * Tests for public ZZ9000 SDK image geometry helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/image_geometry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
  ZZ9KRect area;
  ZZ9KRect rect;
  ZZ9KScaleImageDesc scale;
  ZZ9KScaleImageClippedDesc clipped;
  uint32_t width;
  uint32_t height;

  if (!zz9k_image_fit_size_to_area(720U, 960U, 1280U, 720U,
                                   &width, &height) ||
      width != 540U || height != 720U) {
    printf("did not fit portrait image to framebuffer area\n");
    return 1;
  }

  area.x = 50U;
  area.y = 40U;
  area.w = 240U;
  area.h = 120U;
  if (!zz9k_image_choose_draw_rect_in_area(&area, 720U, 960U, &rect) ||
      rect.x != 125U || rect.y != 40U ||
      rect.w != 90U || rect.h != 120U) {
    printf("did not choose centered image draw rectangle\n");
    return 2;
  }

  if (zz9k_image_count_scale_slices(
        &area, ZZ9K_IMAGE_SCALE_DEFAULT_SLICE_ROWS) != 2U ||
      zz9k_image_count_scale_slices(
        &area, 50U) != 3U) {
    printf("did not count clipped scale slices\n");
    return 3;
  }

  area.h = 240U;
  if (zz9k_image_count_scale_slices(
        &area, ZZ9K_IMAGE_SCALE_DEFAULT_SLICE_ROWS) != 3U) {
    printf("did not round clipped scale slices up\n");
    return 4;
  }
  area.h = 0U;
  if (zz9k_image_count_scale_slices(
        &area, ZZ9K_IMAGE_SCALE_DEFAULT_SLICE_ROWS) != 0U ||
      zz9k_image_count_scale_slices(&area, 0U) != 0U ||
      zz9k_image_count_scale_slices(0, ZZ9K_IMAGE_SCALE_DEFAULT_SLICE_ROWS) !=
        0U) {
    printf("did not reject empty clipped scale slices\n");
    return 5;
  }

  area.x = 50U;
  area.y = 40U;
  area.w = 240U;
  area.h = 120U;
  memset(&clipped, 0, sizeof(clipped));
  if (!zz9k_image_build_clipped_scale_desc(
        &clipped, 0x40000055UL, 720U, 960U, &rect, &area,
        ZZ9K_SCALE_BILINEAR)) {
    printf("did not build clipped scale descriptor\n");
    return 6;
  }
  if (clipped.src_surface != 0x40000055UL ||
      clipped.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
      clipped.src_x != 0U || clipped.src_y != 0U ||
      clipped.src_w != 720U || clipped.src_h != 960U ||
      clipped.dst_x != 125U || clipped.dst_y != 40U ||
      clipped.dst_w != 90U || clipped.dst_h != 120U ||
      clipped.clip_x != 50U || clipped.clip_y != 40U ||
      clipped.clip_w != 240U || clipped.clip_h != 120U ||
      clipped.filter != ZZ9K_SCALE_BILINEAR ||
      clipped.flags != 0U) {
    printf("incorrect clipped scale descriptor\n");
    return 7;
  }

  if (zz9k_image_fit_size_to_area(0U, 960U, 1280U, 720U,
                                  &width, &height) ||
      zz9k_image_fit_size_to_area(720U, 960U, 1280U, 720U,
                                  0, &height) ||
      zz9k_image_choose_draw_rect_in_area(0, 720U, 960U, &rect) ||
      zz9k_image_choose_draw_rect_in_area(&area, 0U, 960U, &rect) ||
      zz9k_image_build_clipped_scale_desc(
        &clipped, ZZ9K_INVALID_HANDLE, 720U, 960U, &rect, &area,
        ZZ9K_SCALE_NEAREST) ||
      zz9k_image_build_clipped_scale_desc(
        0, 0x40000055UL, 720U, 960U, &rect, &area,
        ZZ9K_SCALE_NEAREST)) {
    printf("did not reject invalid public image geometry inputs\n");
    return 8;
  }

  memset(&scale, 0, sizeof(scale));
  if (!zz9k_image_build_surface_scale_desc(
        &scale, 0x40000055UL, 0x40000077UL, 720U, 960U, &rect,
        ZZ9K_SCALE_NEAREST)) {
    printf("did not build surface scale descriptor\n");
    return 9;
  }
  if (scale.src_surface != 0x40000055UL ||
      scale.dst_surface != 0x40000077UL ||
      scale.src_x != 0U || scale.src_y != 0U ||
      scale.src_w != 720U || scale.src_h != 960U ||
      scale.dst_x != 125U || scale.dst_y != 40U ||
      scale.dst_w != 90U || scale.dst_h != 120U ||
      scale.filter != ZZ9K_SCALE_NEAREST ||
      scale.flags != 0U) {
    printf("incorrect surface scale descriptor\n");
    return 10;
  }

  memset(&scale, 0, sizeof(scale));
  if (!zz9k_image_build_framebuffer_scale_desc(
        &scale, 0x40000055UL, 720U, 960U, &rect, ZZ9K_SCALE_BILINEAR) ||
      scale.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
      scale.filter != ZZ9K_SCALE_BILINEAR) {
    printf("did not build framebuffer scale descriptor\n");
    return 11;
  }

  memset(&clipped, 0, sizeof(clipped));
  if (!zz9k_image_build_surface_clipped_scale_desc(
        &clipped, 0x40000055UL, 0x40000077UL, 720U, 960U, &rect,
        &area, ZZ9K_SCALE_BILINEAR) ||
      clipped.dst_surface != 0x40000077UL ||
      clipped.clip_x != 50U || clipped.clip_y != 40U) {
    printf("did not build surface clipped scale descriptor\n");
    return 12;
  }

  if (zz9k_image_build_surface_scale_desc(
        &scale, 0x40000055UL, ZZ9K_INVALID_HANDLE, 720U, 960U,
        &rect, ZZ9K_SCALE_NEAREST) ||
      zz9k_image_build_surface_clipped_scale_desc(
        &clipped, 0x40000055UL, ZZ9K_INVALID_HANDLE, 720U, 960U,
        &rect, &area, ZZ9K_SCALE_NEAREST)) {
    printf("did not reject invalid destination handles\n");
    return 13;
  }

  {
    uint32_t opcode_count = 8U;
    uint32_t service_flags = ZZ9K_SERVICE_FLAG_IMAGE_SCALE_CLIPPED;

    if (!zz9k_image_scale_filter_supported_by_service(
          service_flags, ZZ9K_SCALE_NEAREST) ||
        zz9k_image_scale_filter_supported_by_service(
          service_flags, ZZ9K_SCALE_BILINEAR) ||
        !zz9k_image_service_supports_clipped_scale(
          opcode_count, service_flags, ZZ9K_SCALE_NEAREST)) {
      printf("did not handle nearest clipped-scale service flags\n");
      return 14;
    }

    service_flags |= ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BILINEAR;
    if (!zz9k_image_scale_filter_supported_by_service(
          service_flags, ZZ9K_SCALE_BILINEAR) ||
        !zz9k_image_service_supports_clipped_scale(
          opcode_count, service_flags, ZZ9K_SCALE_BILINEAR)) {
      printf("did not handle bilinear clipped-scale service flags\n");
      return 15;
    }

    opcode_count = 7U;
    if (zz9k_image_service_supports_clipped_scale(
          opcode_count, service_flags, ZZ9K_SCALE_BILINEAR) ||
        zz9k_image_scale_filter_supported_by_service(
          service_flags, ZZ9K_SCALE_BICUBIC) ||
        zz9k_image_service_supports_clipped_scale(
          8U, service_flags, ZZ9K_SCALE_BICUBIC)) {
      printf("did not reject invalid clipped-scale service inputs\n");
      return 16;
    }
  }

  return 0;
}
