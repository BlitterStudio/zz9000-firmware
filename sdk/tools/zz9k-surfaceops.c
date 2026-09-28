/*
 * Reversible ARM-side surface fill/copy test for RTG framebuffer surfaces.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k-fb-common.h"
#include "zz9k-image-window.h"
#include "zz9k/surface.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_SURFACEOPS_AMIGA 1
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#endif

#ifndef ZZ9K_SURFACEOPS_AMIGA
#define ZZ9K_SURFACEOPS_AMIGA 0
#include <time.h>
#endif

#define ZZ9K_SURFACEOPS_RESTORE_GUARD_PIXELS 8U
#define ZZ9K_SURFACEOPS_WINDOW_MIN_WIDTH 180U
#define ZZ9K_SURFACEOPS_WINDOW_MIN_HEIGHT 110U
#define ZZ9K_SURFACEOPS_WINDOW_SOURCE_WIDTH 280U
#define ZZ9K_SURFACEOPS_WINDOW_SOURCE_HEIGHT 90U
#define ZZ9K_SURFACEOPS_DEFAULT_HOLD_TICKS 80U
#define ZZ9K_SURFACEOPS_MAX_HOLD_TICKS 1000U
#define ZZ9K_SURFACEOPS_DEFAULT_LOOPS 1U
#define ZZ9K_SURFACEOPS_MAX_LOOPS 1000000U

typedef struct ZZ9KSurfaceopsOptions {
  uint32_t hold_ticks;
  uint32_t loops;
  uint32_t stats_interval;
  int stats;
} ZZ9KSurfaceopsOptions;

static void zz9k_surfaceops_options_init(ZZ9KSurfaceopsOptions *options)
{
  if (!options) {
    return;
  }
  options->hold_ticks = ZZ9K_SURFACEOPS_DEFAULT_HOLD_TICKS;
  options->loops = ZZ9K_SURFACEOPS_DEFAULT_LOOPS;
  options->stats_interval = 0U;
  options->stats = 0;
}

static int zz9k_surfaceops_parse_u32(const char *text, uint32_t *value)
{
  char *end;
  unsigned long parsed;

  if (!text || !text[0] || text[0] == '-' || !value) {
    return 0;
  }
  errno = 0;
  parsed = strtoul(text, &end, 0);
  if (errno == ERANGE || *end != '\0' || parsed > 0xffffffffUL) {
    return 0;
  }

  *value = (uint32_t)parsed;
  return 1;
}

static int zz9k_surfaceops_parse_options(int argc, char **argv,
                                         ZZ9KSurfaceopsOptions *options)
{
  int positional_hold = 0;
  int i;

  if (!options) {
    return 0;
  }
  zz9k_surfaceops_options_init(options);

  for (i = 1; i < argc; i++) {
    uint32_t value;

    if (strcmp(argv[i], "--stats") == 0) {
      options->stats = 1;
    } else if (strcmp(argv[i], "--hold-ticks") == 0) {
      if (++i >= argc ||
          !zz9k_surfaceops_parse_u32(argv[i], &value)) {
        return 0;
      }
      options->hold_ticks = value > ZZ9K_SURFACEOPS_MAX_HOLD_TICKS ?
                            ZZ9K_SURFACEOPS_MAX_HOLD_TICKS : value;
    } else if (strcmp(argv[i], "--loops") == 0) {
      if (++i >= argc ||
          !zz9k_surfaceops_parse_u32(argv[i], &value) ||
          value == 0U || value > ZZ9K_SURFACEOPS_MAX_LOOPS) {
        return 0;
      }
      options->loops = value;
    } else if (strcmp(argv[i], "--stats-interval") == 0) {
      if (++i >= argc ||
          !zz9k_surfaceops_parse_u32(argv[i], &value) ||
          value == 0U || value > ZZ9K_SURFACEOPS_MAX_LOOPS) {
        return 0;
      }
      options->stats_interval = value;
    } else if (argv[i][0] == '-') {
      return 0;
    } else {
      if (positional_hold ||
          !zz9k_surfaceops_parse_u32(argv[i], &value)) {
        return 0;
      }
      positional_hold = 1;
      options->hold_ticks = value > ZZ9K_SURFACEOPS_MAX_HOLD_TICKS ?
                            ZZ9K_SURFACEOPS_MAX_HOLD_TICKS : value;
    }
  }

  if (options->stats_interval != 0U && !options->stats) {
    return 0;
  }

  return 1;
}

static uint32_t zz9k_surfaceops_ticks_to_ms(uint64_t ticks,
                                            uint32_t ticks_per_second)
{
  uint64_t ms;

  if (ticks_per_second == 0U) {
    return 0U;
  }
  ms = (ticks * 1000ULL) / ticks_per_second;
  return ms > 0xffffffffULL ? 0xffffffffU : (uint32_t)ms;
}

static uint32_t zz9k_surfaceops_loops_per_second_x100(uint32_t loops,
                                                      uint32_t elapsed_ms)
{
  uint64_t rate;

  if (elapsed_ms == 0U) {
    return 0U;
  }
  rate = ((uint64_t)loops * 100000ULL) / (uint64_t)elapsed_ms;
  return rate > 0xffffffffULL ? 0xffffffffU : (uint32_t)rate;
}

static int zz9k_surfaceops_should_print_stats_bucket(
    uint32_t loops_completed, uint32_t stats_interval)
{
  return stats_interval != 0U && loops_completed != 0U &&
         (loops_completed % stats_interval) == 0U;
}

static void zz9k_surfaceops_window_config_init(
    ZZ9KImageWindowConfig *config)
{
  zz9k_image_window_config_init(
      config, "ZZ9000 SDK surfaceops",
      ZZ9K_SURFACEOPS_WINDOW_SOURCE_WIDTH,
      ZZ9K_SURFACEOPS_WINDOW_SOURCE_HEIGHT, 0, 0,
      ZZ9K_SURFACEOPS_WINDOW_MIN_WIDTH,
      ZZ9K_SURFACEOPS_WINDOW_MIN_HEIGHT, 0U, 0U);
  if (config) {
    config->close_gadget = 0;
  }
}

static int zz9k_surfaceops_inflate_rect(const ZZ9KSurface *framebuffer,
                                        const ZZ9KFbRect *inner,
                                        uint32_t guard,
                                        ZZ9KFbRect *outer)
{
  uint32_t left;
  uint32_t top;
  uint32_t right_room;
  uint32_t bottom_room;
  uint32_t right;
  uint32_t bottom;

  if (!framebuffer || !inner || !outer) {
    return 0;
  }

  left = inner->x < guard ? inner->x : guard;
  top = inner->y < guard ? inner->y : guard;
  right_room = framebuffer->width - (inner->x + inner->w);
  bottom_room = framebuffer->height - (inner->y + inner->h);
  right = right_room < guard ? right_room : guard;
  bottom = bottom_room < guard ? bottom_room : guard;

  outer->x = inner->x - left;
  outer->y = inner->y - top;
  outer->w = inner->w + left + right;
  outer->h = inner->h + top + bottom;
  return outer->w != 0U && outer->h != 0U;
}

static int zz9k_surfaceops_choose_rect_in_area(
    const ZZ9KSurface *framebuffer,
    const ZZ9KFbRect *area,
    uint32_t bpp,
    ZZ9KFbRect *draw_rect)
{
  uint32_t w;
  uint32_t h;

  if (!framebuffer || !area || !draw_rect || bpp == 0U ||
      area->w == 0U || area->h == 0U) {
    return 0;
  }
  if (!zz9k_fb_rect_fits(framebuffer, area, bpp)) {
    return 0;
  }

  w = area->w < 128U ? area->w : 128U;
  h = area->h < 64U ? area->h : 64U;
  draw_rect->w = w;
  draw_rect->h = h;
  draw_rect->x = area->x + ((area->w - w) / 2U);
  draw_rect->y = area->y + ((area->h - h) / 2U);
  return zz9k_fb_rect_fits(framebuffer, draw_rect, bpp);
}

static int zz9k_surfaceops_prepare_rects_in_area(
    const ZZ9KSurface *framebuffer,
    const ZZ9KFbRect *area,
    ZZ9KFbRect *draw_rect,
    ZZ9KFbRect *backup_rect,
    uint32_t *bpp,
    uint32_t *draw_row_bytes,
    uint32_t *backup_row_bytes)
{
  uint32_t local_bpp;

  if (!framebuffer || !area || !draw_rect || !backup_rect || !bpp ||
      !draw_row_bytes || !backup_row_bytes) {
    return 0;
  }
  local_bpp = zz9k_fb_bytes_per_pixel(framebuffer->format);
  if (local_bpp == 0U ||
      !zz9k_surfaceops_choose_rect_in_area(framebuffer, area, local_bpp,
                                           draw_rect) ||
      !zz9k_surfaceops_inflate_rect(framebuffer, draw_rect,
                                    ZZ9K_SURFACEOPS_RESTORE_GUARD_PIXELS,
                                    backup_rect) ||
      !zz9k_fb_rect_fits(framebuffer, backup_rect, local_bpp)) {
    return 0;
  }
  if (draw_rect->w > (UINT32_MAX / local_bpp) ||
      backup_rect->w > (UINT32_MAX / local_bpp)) {
    return 0;
  }

  *bpp = local_bpp;
  *draw_row_bytes = draw_rect->w * local_bpp;
  *backup_row_bytes = backup_rect->w * local_bpp;
  return 1;
}

static int zz9k_surfaceops_prepare_rects(const ZZ9KSurface *framebuffer,
                                         ZZ9KFbRect *draw_rect,
                                         ZZ9KFbRect *backup_rect,
                                         uint32_t *bpp,
                                         uint32_t *draw_row_bytes,
                                         uint32_t *backup_row_bytes)
{
  ZZ9KFbRect area;

  if (!framebuffer || !draw_rect || !backup_rect || !bpp ||
      !draw_row_bytes || !backup_row_bytes) {
    return 0;
  }

  area.x = 0U;
  area.y = 0U;
  area.w = framebuffer->width;
  area.h = framebuffer->height;
  return zz9k_surfaceops_prepare_rects_in_area(
      framebuffer, &area, draw_rect, backup_rect, bpp, draw_row_bytes,
      backup_row_bytes);
}

static int zz9k_surfaceops_build_backup_copy_descs(
    const ZZ9KSurface *backup_surface,
    const ZZ9KFbRect *rect,
    ZZ9KSurfaceCopyDesc *save_copy,
    ZZ9KSurfaceCopyDesc *restore_copy)
{
  return zz9k_fb_build_framebuffer_backup_copy_descs(
      backup_surface, rect, save_copy, restore_copy);
}

static void zz9k_surfaceops_rect_to_public(const ZZ9KFbRect *source,
                                           ZZ9KRect *target)
{
  target->x = source->x;
  target->y = source->y;
  target->w = source->w;
  target->h = source->h;
}

static int zz9k_surfaceops_build_source_copy_desc(ZZ9KSurfaceCopyDesc *copy,
                                                  uint32_t source_handle,
                                                  const ZZ9KFbRect *rect)
{
  ZZ9KRect source_rect;

  if (!rect) {
    return 0;
  }
  source_rect.x = 0U;
  source_rect.y = 0U;
  source_rect.w = rect->w;
  source_rect.h = rect->h;
  return zz9k_surface_build_copy_desc(copy, source_handle,
                                      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER,
                                      &source_rect, rect->x, rect->y, 0U);
}

static int zz9k_surfaceops_build_framebuffer_fill_desc(
    ZZ9KSurfaceFillDesc *fill,
    const ZZ9KFbRect *rect,
    uint32_t color)
{
  ZZ9KRect public_rect;

  if (!rect) {
    return 0;
  }
  zz9k_surfaceops_rect_to_public(rect, &public_rect);
  return zz9k_surface_build_framebuffer_fill_desc(fill, &public_rect,
                                                  color, 0U);
}

static int zz9k_surfaceops_build_surface_fill_desc(ZZ9KSurfaceFillDesc *fill,
                                                  uint32_t surface_handle,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t color)
{
  ZZ9KRect rect;

  rect.x = 0U;
  rect.y = 0U;
  rect.w = width;
  rect.h = height;
  return zz9k_surface_build_fill_desc(fill, surface_handle, &rect, color, 0U);
}

static uint32_t surface_color(uint32_t format,
                              uint8_t r, uint8_t g, uint8_t b)
{
  uint32_t color;

  if (!zz9k_surface_color_for_format(format, r, g, b, 0xffU, 0xffU,
                                     &color)) {
    return 0U;
  }
  return color;
}

#ifndef ZZ9K_SURFACEOPS_NO_MAIN
#if ZZ9K_SURFACEOPS_AMIGA
typedef ZZ9KImageWindow ZZ9KSurfaceopsWindow;

static void surfaceops_close_window(ZZ9KSurfaceopsWindow *ui)
{
  zz9k_image_window_close(ui);
}

static int surfaceops_open_window(const ZZ9KSurface *framebuffer,
                                  ZZ9KSurfaceopsWindow *ui)
{
  ZZ9KImageWindowConfig config;

  if (!framebuffer || !ui) {
    return 0;
  }
  zz9k_surfaceops_window_config_init(&config);
  return zz9k_image_window_open(framebuffer, &config, ui);
}
#endif

static void surfaceops_delay_ticks(uint32_t ticks)
{
#if ZZ9K_SURFACEOPS_AMIGA
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

static void surfaceops_usage(void)
{
  printf("usage: zz9k-surfaceops [hold-ticks]\n");
  printf("       zz9k-surfaceops [--hold-ticks <ticks>] "
         "[--loops <count>] [--stats] [--stats-interval <count>]\n");
  printf("       hold-ticks defaults to 80, capped at 1000\n");
  printf("       loops defaults to 1 and must be 1..1000000\n");
  printf("       stats-interval prints a bucket every N loops and requires "
         "--stats\n");
}

static uint64_t surfaceops_now_ticks(void)
{
#if ZZ9K_SURFACEOPS_AMIGA
  struct DateStamp stamp;
  uint32_t minutes;

  DateStamp(&stamp);
  minutes = ((uint32_t)stamp.ds_Days * 24U * 60U) +
            (uint32_t)stamp.ds_Minute;
  return ((uint64_t)minutes * 60ULL * 50ULL) + (uint32_t)stamp.ds_Tick;
#else
  return (uint64_t)clock();
#endif
}

static uint32_t surfaceops_ticks_per_second(void)
{
#if ZZ9K_SURFACEOPS_AMIGA
  return 50U;
#else
  return (uint32_t)CLOCKS_PER_SEC;
#endif
}

static void surfaceops_print_stats(uint32_t loops_completed,
                                   uint64_t start_ticks,
                                   uint64_t end_ticks)
{
  uint32_t elapsed_ms;
  uint32_t loops_x100;

  if (end_ticks < start_ticks) {
    end_ticks = start_ticks;
  }
  elapsed_ms = zz9k_surfaceops_ticks_to_ms(
      end_ticks - start_ticks, surfaceops_ticks_per_second());
  loops_x100 = zz9k_surfaceops_loops_per_second_x100(loops_completed,
                                                     elapsed_ms);
  printf("surfaceops stats: loops=%lu elapsed=%lu ms rate=%lu.%02lu loops/s\n",
         (unsigned long)loops_completed,
         (unsigned long)elapsed_ms,
         (unsigned long)(loops_x100 / 100U),
         (unsigned long)(loops_x100 % 100U));
}

static void surfaceops_print_stats_bucket(uint32_t first_loop,
                                          uint32_t last_loop,
                                          uint64_t start_ticks,
                                          uint64_t end_ticks)
{
  uint32_t count;
  uint32_t elapsed_ms;
  uint32_t loops_x100;

  if (last_loop < first_loop) {
    return;
  }
  if (end_ticks < start_ticks) {
    end_ticks = start_ticks;
  }
  count = (last_loop - first_loop) + 1U;
  elapsed_ms = zz9k_surfaceops_ticks_to_ms(
      end_ticks - start_ticks, surfaceops_ticks_per_second());
  loops_x100 = zz9k_surfaceops_loops_per_second_x100(count, elapsed_ms);
  printf("surfaceops stats interval: loops=%lu-%lu count=%lu "
         "elapsed=%lu ms rate=%lu.%02lu loops/s\n",
         (unsigned long)first_loop,
         (unsigned long)last_loop,
         (unsigned long)count,
         (unsigned long)elapsed_ms,
         (unsigned long)(loops_x100 / 100U),
         (unsigned long)(loops_x100 % 100U));
  fflush(stdout);
}

static int require_cap(uint32_t caps, uint32_t bit)
{
  const char *name;

  if ((caps & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-surfaceops: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

int main(int argc, char **argv)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KSurface framebuffer;
  ZZ9KSurface backup_surface;
  ZZ9KSurface source;
  ZZ9KFbRect target_area;
  ZZ9KFbRect draw_rect;
  ZZ9KFbRect backup_rect;
  ZZ9KSurfaceFillDesc fill;
  ZZ9KSurfaceCopyDesc save_copy;
  ZZ9KSurfaceCopyDesc restore_copy;
  ZZ9KSurfaceCopyDesc copy;
  ZZ9KSurfaceopsOptions options;
  uint32_t bpp;
  uint32_t draw_row_bytes;
  uint32_t backup_row_bytes;
  uint32_t ticks;
  uint32_t loop;
  uint32_t loops_completed = 0U;
  uint64_t stats_start = 0U;
  uint64_t stats_interval_start = 0U;
  uint64_t stats_end = 0U;
  int backup_allocated = 0;
  int source_allocated = 0;
  int drew = 0;
  int status;
  int rc = 1;
#if ZZ9K_SURFACEOPS_AMIGA
  ZZ9KSurfaceopsWindow ui;
#endif

  memset(&backup_surface, 0, sizeof(backup_surface));
  memset(&source, 0, sizeof(source));
#if ZZ9K_SURFACEOPS_AMIGA
  memset(&ui, 0, sizeof(ui));
#endif
  if (!zz9k_surfaceops_parse_options(argc, argv, &options)) {
    surfaceops_usage();
    return 1;
  }

  ticks = options.hold_ticks;

  printf("zz9k-surfaceops: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surfaceops: open failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    return 1;
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surfaceops: query caps failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  if (!require_cap(caps.capability_bits, ZZ9K_CAP_SURFACES) ||
      !require_cap(caps.capability_bits, ZZ9K_CAP_FRAMEBUFFER_SURFACE) ||
      !require_cap(caps.capability_bits, ZZ9K_CAP_SURFACE_OPS)) {
    goto cleanup;
  }

  status = zz9k_map_framebuffer_surface(ctx, &framebuffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surfaceops: map framebuffer failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
#if ZZ9K_SURFACEOPS_AMIGA
  if (!surfaceops_open_window(&framebuffer, &ui)) {
    goto cleanup;
  }
  target_area = ui.inner;
#else
  target_area.x = 0U;
  target_area.y = 0U;
  target_area.w = framebuffer.width;
  target_area.h = framebuffer.height;
#endif
  if (!zz9k_surfaceops_prepare_rects_in_area(
        &framebuffer, &target_area, &draw_rect, &backup_rect, &bpp,
        &draw_row_bytes, &backup_row_bytes)) {
    printf("zz9k-surfaceops: unsupported framebuffer geometry\n");
    goto cleanup;
  }

  status = zz9k_alloc_surface_ex(ctx, backup_rect.w, backup_rect.h,
                                 framebuffer.format,
                                 ZZ9K_SURFACE_FLAG_ARM_LOCAL,
                                 backup_row_bytes, &backup_surface);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surfaceops: backup surface alloc failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  backup_allocated = 1;
  if (!zz9k_surfaceops_build_backup_copy_descs(&backup_surface, &backup_rect,
                                               &save_copy, &restore_copy)) {
    printf("zz9k-surfaceops: backup descriptor build failed\n");
    goto cleanup;
  }
  status = zz9k_copy_surface(ctx, &save_copy);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surfaceops: backup surface copy failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }

  printf("Framebuffer:          %lu x %lu, pitch=%lu, format=%s (%lu)\n",
         (unsigned long)framebuffer.width,
         (unsigned long)framebuffer.height,
         (unsigned long)framebuffer.pitch,
         zz9k_fb_format_name(framebuffer.format),
         (unsigned long)framebuffer.format);
  printf("Window area:          x=%lu y=%lu w=%lu h=%lu\n",
         (unsigned long)target_area.x, (unsigned long)target_area.y,
         (unsigned long)target_area.w, (unsigned long)target_area.h);
  printf("Bytes per pixel:      %lu\n", (unsigned long)bpp);
  printf("Test rectangle:       x=%lu y=%lu w=%lu h=%lu\n",
         (unsigned long)draw_rect.x, (unsigned long)draw_rect.y,
         (unsigned long)draw_rect.w, (unsigned long)draw_rect.h);
  printf("Restore rectangle:    x=%lu y=%lu w=%lu h=%lu\n",
         (unsigned long)backup_rect.x, (unsigned long)backup_rect.y,
         (unsigned long)backup_rect.w, (unsigned long)backup_rect.h);
  printf("Backup:               guarded ARM-local surface copy\n");
  printf("Hold:                 %lu ticks\n", (unsigned long)ticks);
  printf("Loops:                %lu\n", (unsigned long)options.loops);
  if (options.stats_interval != 0U) {
    printf("Stats interval:       %lu loops\n",
           (unsigned long)options.stats_interval);
  }
  fflush(stdout);

  status = zz9k_alloc_surface_ex(ctx, draw_rect.w, draw_rect.h,
                                 framebuffer.format,
                                 ZZ9K_SURFACE_FLAG_ARM_LOCAL, draw_row_bytes,
                                 &source);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surfaceops: source alloc failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  source_allocated = 1;

  if (!zz9k_surfaceops_build_source_copy_desc(&copy, source.handle,
                                              &draw_rect)) {
    printf("zz9k-surfaceops: copy descriptor build failed\n");
    goto cleanup;
  }

  if (options.stats) {
    stats_start = surfaceops_now_ticks();
    stats_interval_start = stats_start;
  }

  for (loop = 0U; loop < options.loops; loop++) {
    uint8_t frame_r = (uint8_t)(0x20U + (loop & 0x3fU));
    uint8_t source_g = (uint8_t)(0x60U + (loop & 0x7fU));

    if (!zz9k_surfaceops_build_framebuffer_fill_desc(
          &fill, &draw_rect,
          surface_color(framebuffer.format, frame_r, 0x70U, 0xffU))) {
      printf("zz9k-surfaceops: fill descriptor build failed\n");
      goto cleanup;
    }
    status = zz9k_fill_surface(ctx, &fill);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-surfaceops: fill framebuffer failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
      goto cleanup;
    }
    drew = 1;
    if (options.loops == 1U) {
      printf("Surface fill:         ok\n");
    }
    surfaceops_delay_ticks(ticks);

    if (!zz9k_surfaceops_build_surface_fill_desc(
          &fill, source.handle, draw_rect.w, draw_rect.h,
          surface_color(framebuffer.format, 0xffU, source_g, 0x20U))) {
      printf("zz9k-surfaceops: source fill descriptor build failed\n");
      goto cleanup;
    }
    status = zz9k_fill_surface(ctx, &fill);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-surfaceops: fill source failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
      goto cleanup;
    }

    status = zz9k_copy_surface(ctx, &copy);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-surfaceops: copy to framebuffer failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
      goto cleanup;
    }
    if (options.loops == 1U) {
      printf("Surface copy:         ok\n");
    }
    surfaceops_delay_ticks(ticks);

    status = zz9k_copy_surface(ctx, &restore_copy);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-surfaceops: restore framebuffer failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
      goto cleanup;
    }
    drew = 0;
    loops_completed++;
    if (options.stats &&
        zz9k_surfaceops_should_print_stats_bucket(
            loops_completed, options.stats_interval)) {
      uint64_t stats_interval_end = surfaceops_now_ticks();
      surfaceops_print_stats_bucket(
          loops_completed - options.stats_interval + 1U, loops_completed,
          stats_interval_start, stats_interval_end);
      stats_interval_start = stats_interval_end;
    }
  }

  if (options.stats) {
    stats_end = surfaceops_now_ticks();
    surfaceops_print_stats(loops_completed, stats_start, stats_end);
  }
  printf("zz9k-surfaceops: restored framebuffer rectangle "
         "using ARM surface copy\n");
  if (options.loops > 1U) {
    printf("zz9k-surfaceops: completed %lu surface stress loops\n",
           (unsigned long)loops_completed);
  }
  rc = 0;

cleanup:
  if (drew && backup_allocated) {
    status = zz9k_copy_surface(ctx, &restore_copy);
    if (status == ZZ9K_STATUS_OK) {
      printf("zz9k-surfaceops: cleanup restored framebuffer rectangle "
             "using ARM surface copy\n");
    } else {
      printf("zz9k-surfaceops: cleanup restore failed: %s (%d)\n",
             zz9k_fb_status_name(status), status);
    }
  }
  if (source_allocated) {
    zz9k_free_surface(ctx, source.handle);
  }
  if (backup_allocated) {
    zz9k_free_surface(ctx, backup_surface.handle);
  }
#if ZZ9K_SURFACEOPS_AMIGA
  surfaceops_close_window(&ui);
#endif
  zz9k_close(ctx);
  return rc;
}
#endif
