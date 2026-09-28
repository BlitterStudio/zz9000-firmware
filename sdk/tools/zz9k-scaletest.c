/*
 * ARM scaler to RTG framebuffer demo.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k-fb-common.h"
#include "zz9k/image_geometry.h"
#include "zz9k/surface.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_SCALETEST_AMIGA 1
#include <exec/types.h>
#include <proto/dos.h>
#endif

static int zz9k_scaletest_choose_rects(const ZZ9KSurface *framebuffer,
                                       uint32_t bpp, ZZ9KFbRect *src,
                                       ZZ9KFbRect *dst)
{
  if (!src || !dst ||
      !zz9k_fb_choose_auto_rect(framebuffer, bpp, dst)) {
    return 0;
  }

  src->x = 0;
  src->y = 0;
  src->w = dst->w > 1U ? dst->w / 2U : 1U;
  src->h = dst->h > 1U ? dst->h / 2U : 1U;
  return src->w != 0 && src->h != 0;
}

static void zz9k_scaletest_fb_rect_to_rect(const ZZ9KFbRect *source,
                                           ZZ9KRect *target)
{
  target->x = source->x;
  target->y = source->y;
  target->w = source->w;
  target->h = source->h;
}

static int zz9k_scaletest_build_framebuffer_fill_desc(
    ZZ9KSurfaceFillDesc *fill,
    const ZZ9KFbRect *rect)
{
  ZZ9KRect public_rect;

  if (!rect) {
    return 0;
  }

  zz9k_scaletest_fb_rect_to_rect(rect, &public_rect);
  return zz9k_surface_build_framebuffer_fill_desc(
      fill, &public_rect, zz9k_surface_color_rgb(0U, 0U, 0U), 0U);
}

static int zz9k_scaletest_build_desc(ZZ9KScaleImageDesc *desc,
                                     uint32_t src_handle,
                                     const ZZ9KFbRect *src,
                                     const ZZ9KFbRect *dst,
                                     uint32_t filter)
{
  ZZ9KRect dst_rect;

  if (!src || !dst || src->w == 0U || src->h == 0U) {
    return 0;
  }

  zz9k_scaletest_fb_rect_to_rect(dst, &dst_rect);
  if (!zz9k_image_build_framebuffer_scale_desc(desc, src_handle, src->w,
                                               src->h, &dst_rect, filter)) {
    return 0;
  }
  desc->src_x = src->x;
  desc->src_y = src->y;
  return 1;
}

static int zz9k_scaletest_choose_clip_rect(const ZZ9KFbRect *dst,
                                           ZZ9KFbRect *clip)
{
  if (!dst || !clip || dst->w == 0U || dst->h == 0U) {
    return 0;
  }

  clip->w = dst->w > 1U ? dst->w / 2U : 1U;
  clip->h = dst->h > 1U ? dst->h / 2U : 1U;
  clip->x = dst->x + ((dst->w - clip->w) / 2U);
  clip->y = dst->y + ((dst->h - clip->h) / 2U);
  return 1;
}

static int zz9k_scaletest_build_clipped_desc(
    ZZ9KScaleImageClippedDesc *desc,
    uint32_t src_handle,
    const ZZ9KFbRect *src,
    const ZZ9KFbRect *dst,
    const ZZ9KFbRect *clip,
    uint32_t filter)
{
  ZZ9KRect dst_rect;
  ZZ9KRect clip_rect;

  if (!src || !dst || !clip || src->w == 0U || src->h == 0U) {
    return 0;
  }

  zz9k_scaletest_fb_rect_to_rect(dst, &dst_rect);
  zz9k_scaletest_fb_rect_to_rect(clip, &clip_rect);
  if (!zz9k_image_build_clipped_scale_desc(desc, src_handle, src->w,
                                           src->h, &dst_rect, &clip_rect,
                                           filter)) {
    return 0;
  }
  desc->src_x = src->x;
  desc->src_y = src->y;
  return 1;
}

#ifndef ZZ9K_SCALETEST_NO_MAIN
static const char *zz9k_scaletest_filter_name(uint32_t filter)
{
  switch (filter) {
  case ZZ9K_SCALE_NEAREST:
    return "nearest";
  case ZZ9K_SCALE_BILINEAR:
    return "bilinear";
  default:
    return "unknown";
  }
}

static void scaletest_delay_ticks(uint32_t ticks)
{
#if ZZ9K_SCALETEST_AMIGA
  Delay((LONG)ticks);
#else
  volatile uint32_t spin;
  uint32_t i;

  for (i = 0; i < ticks; i++) {
    for (spin = 0; spin < 1000000UL; spin++) {
    }
  }
#endif
}

static int parse_tick_text(const char *text, uint32_t *ticks)
{
  char *end;
  unsigned long value;

  if (!text || !*text || !ticks)
    return 0;
  value = strtoul(text, &end, 0);
  if (*end != '\0')
    return 0;
  if (value > 1000UL) {
    value = 1000UL;
  }
  *ticks = (uint32_t)value;
  return 1;
}

static void zz9k_scaletest_usage(void)
{
  printf("usage: zz9k-scaletest [--nearest|--bilinear] [--clip] [hold-ticks]\n");
  printf("       hold-ticks defaults to 100, capped at 1000\n");
}

static int zz9k_scaletest_parse_args(int argc, char **argv,
                                     uint32_t *ticks, uint32_t *filter,
                                     int *clipped)
{
  int ticks_set = 0;
  int i;

  if (!ticks || !filter || !clipped)
    return 0;
  *ticks = 100U;
  *filter = ZZ9K_SCALE_NEAREST;
  *clipped = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--nearest") == 0) {
      *filter = ZZ9K_SCALE_NEAREST;
    } else if (strcmp(argv[i], "--bilinear") == 0) {
      *filter = ZZ9K_SCALE_BILINEAR;
    } else if (strcmp(argv[i], "--clip") == 0) {
      *clipped = 1;
    } else if (strcmp(argv[i], "-h") == 0 ||
               strcmp(argv[i], "--help") == 0) {
      zz9k_scaletest_usage();
      return 0;
    } else if (!ticks_set && parse_tick_text(argv[i], ticks)) {
      ticks_set = 1;
    } else {
      zz9k_scaletest_usage();
      return 0;
    }
  }

  return 1;
}

static int require_cap(uint32_t caps, uint32_t bit)
{
  const char *name;

  if ((caps & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-scaletest: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

int main(int argc, char **argv)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KServiceInfo image_service;
  ZZ9KSurface framebuffer;
  ZZ9KSurface source;
  ZZ9KFbRect src_rect;
  ZZ9KFbRect dst_rect;
  ZZ9KFbRect clip_rect;
  ZZ9KScaleImageDesc scale;
  ZZ9KScaleImageClippedDesc clipped_scale;
  ZZ9KSurfaceFillDesc fill;
  volatile uint8_t *framebuffer_data;
  uint8_t *backup = 0;
  uint32_t bpp;
  uint32_t row_bytes;
  uint32_t backup_len;
  uint32_t ticks;
  uint32_t filter;
  int clipped;
  int status;
  int source_allocated = 0;
  int drew = 0;
  int rc = 1;

  memset(&source, 0, sizeof(source));
  memset(&image_service, 0, sizeof(image_service));
  if (!zz9k_scaletest_parse_args(argc, argv, &ticks, &filter, &clipped)) {
    return 1;
  }

  printf("zz9k-scaletest: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-scaletest: open failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    return 1;
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-scaletest: query caps failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  if (!require_cap(caps.capability_bits, ZZ9K_CAP_SURFACES) ||
      !require_cap(caps.capability_bits, ZZ9K_CAP_FRAMEBUFFER_SURFACE) ||
      !require_cap(caps.capability_bits, ZZ9K_CAP_IMAGE_SCALE)) {
    goto cleanup;
  }
  if (clipped &&
      !require_cap(caps.capability_bits, ZZ9K_CAP_SURFACE_OPS)) {
    goto cleanup;
  }
  if (clipped) {
    status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &image_service);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-scaletest: image service query failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
      goto cleanup;
    }
    if (!zz9k_image_service_supports_clipped_scale(
          image_service.opcode_count, image_service.flags, filter)) {
      printf("zz9k-scaletest: firmware does not advertise clipped scale\n");
      goto cleanup;
    }
  }

  status = zz9k_map_framebuffer_surface(ctx, &framebuffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-scaletest: map framebuffer failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  if ((framebuffer.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0) {
    printf("zz9k-scaletest: framebuffer is not CPU-visible\n");
    goto cleanup;
  }
  if (!framebuffer.data) {
    printf("zz9k-scaletest: framebuffer is not CPU-mapped by the host\n");
    goto cleanup;
  }

  bpp = zz9k_fb_bytes_per_pixel(framebuffer.format);
  if (bpp == 0) {
    printf("zz9k-scaletest: unsupported framebuffer format: %lu\n",
           (unsigned long)framebuffer.format);
    goto cleanup;
  }
  if (filter == ZZ9K_SCALE_BILINEAR && bpp != 4U) {
    printf("zz9k-scaletest: bilinear currently requires a 32-bit "
           "framebuffer format\n");
    goto cleanup;
  }
  if (!zz9k_scaletest_choose_rects(&framebuffer, bpp, &src_rect,
                                   &dst_rect)) {
    printf("zz9k-scaletest: could not choose safe scale rectangles\n");
    goto cleanup;
  }
  if (clipped &&
      !zz9k_scaletest_choose_clip_rect(&dst_rect, &clip_rect)) {
    printf("zz9k-scaletest: could not choose clipped scale rectangle\n");
    goto cleanup;
  }

  status = zz9k_alloc_surface_ex(ctx, src_rect.w, src_rect.h,
                                 framebuffer.format, 0, src_rect.w * bpp,
                                 &source);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-scaletest: source surface alloc failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  source_allocated = 1;

  if ((source.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0 ||
      !zz9k_fb_fill_surface_pattern(&source)) {
    printf("zz9k-scaletest: could not fill source surface\n");
    goto cleanup;
  }

  row_bytes = dst_rect.w * bpp;
  backup_len = row_bytes * dst_rect.h;
  backup = (uint8_t *)malloc(backup_len);
  if (!backup) {
    printf("zz9k-scaletest: backup allocation failed: %lu bytes\n",
           (unsigned long)backup_len);
    goto cleanup;
  }

  printf("Framebuffer:          %lu x %lu, pitch=%lu, format=%s (%lu)\n",
         (unsigned long)framebuffer.width,
         (unsigned long)framebuffer.height,
         (unsigned long)framebuffer.pitch,
         zz9k_fb_format_name(framebuffer.format),
         (unsigned long)framebuffer.format);
  printf("Source surface:       handle=0x%08lx, %lu x %lu\n",
         (unsigned long)source.handle,
         (unsigned long)source.width,
         (unsigned long)source.height);
  printf("Scale destination:    x=%lu y=%lu w=%lu h=%lu\n",
         (unsigned long)dst_rect.x,
         (unsigned long)dst_rect.y,
         (unsigned long)dst_rect.w,
         (unsigned long)dst_rect.h);
  if (clipped) {
    printf("Clip rectangle:       x=%lu y=%lu w=%lu h=%lu\n",
           (unsigned long)clip_rect.x,
           (unsigned long)clip_rect.y,
           (unsigned long)clip_rect.w,
           (unsigned long)clip_rect.h);
  }
  printf("Backup:               %lu bytes\n", (unsigned long)backup_len);
  printf("Filter:               %s\n", zz9k_scaletest_filter_name(filter));
  printf("Hold:                 %lu ticks\n", (unsigned long)ticks);
  fflush(stdout);

  framebuffer_data = (volatile uint8_t *)framebuffer.data;
  zz9k_fb_copy_rect_from_surface(backup, framebuffer_data,
                                 framebuffer.pitch, &dst_rect, bpp);
  if (clipped) {
    if (!zz9k_scaletest_build_framebuffer_fill_desc(&fill, &dst_rect)) {
      printf("zz9k-scaletest: could not build clear descriptor\n");
      goto cleanup;
    }
    status = zz9k_fill_surface(ctx, &fill);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-scaletest: ARM clear failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
      goto cleanup;
    }
    drew = 1;
    if (!zz9k_scaletest_build_clipped_desc(&clipped_scale, source.handle,
                                           &src_rect, &dst_rect,
                                           &clip_rect, filter)) {
      printf("zz9k-scaletest: could not build clipped scale descriptor\n");
      goto cleanup;
    }
    status = zz9k_scale_image_clipped(ctx, &clipped_scale);
  } else {
    if (!zz9k_scaletest_build_desc(&scale, source.handle, &src_rect,
                                   &dst_rect, filter)) {
      printf("zz9k-scaletest: could not build scale descriptor\n");
      goto cleanup;
    }
    status = zz9k_scale_image(ctx, &scale);
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-scaletest: ARM scale failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  drew = 1;
  scaletest_delay_ticks(ticks);
  zz9k_fb_copy_rect_to_surface(framebuffer_data, backup, framebuffer.pitch,
                               &dst_rect, bpp);
  drew = 0;

  printf("zz9k-scaletest: restored framebuffer rectangle\n");
  rc = 0;

cleanup:
  if (drew && backup) {
    zz9k_fb_copy_rect_to_surface((volatile uint8_t *)framebuffer.data,
                                 backup, framebuffer.pitch, &dst_rect, bpp);
  }
  free(backup);
  if (source_allocated) {
    zz9k_free_surface(ctx, source.handle);
  }
  zz9k_close(ctx);
  return rc;
}
#endif
