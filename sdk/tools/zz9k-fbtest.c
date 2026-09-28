/*
 * Reversible direct-to-RTG-framebuffer write test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-fb-common.h"
#include "zz9k/caps.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_FBTEST_AMIGA 1
#include <exec/types.h>
#include <proto/dos.h>
#endif

#ifndef ZZ9K_FBTEST_NO_MAIN
static void fbtest_delay_ticks(uint32_t ticks)
{
#if ZZ9K_FBTEST_AMIGA
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

static uint32_t parse_ticks(int argc, char **argv)
{
  unsigned long value;

  if (argc < 2) {
    return 100U;
  }
  value = strtoul(argv[1], 0, 0);
  if (value > 1000UL) {
    value = 1000UL;
  }
  return (uint32_t)value;
}

int main(int argc, char **argv)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KSurface surface;
  ZZ9KFbRect rect;
  volatile uint8_t *surface_data;
  uint8_t *backup = 0;
  uint32_t bpp;
  uint32_t row_bytes;
  uint32_t backup_len;
  uint32_t ticks;
  int status;
  int drew = 0;
  int rc = 1;

  if (argc > 2) {
    printf("usage: zz9k-fbtest [hold-ticks]\n");
    printf("       hold-ticks defaults to 100, capped at 1000\n");
    return 1;
  }

  ticks = parse_ticks(argc, argv);

  printf("zz9k-fbtest: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-fbtest: open failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    return 1;
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-fbtest: query caps failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  if ((caps.capability_bits & ZZ9K_CAP_FRAMEBUFFER_SURFACE) == 0) {
    printf("zz9k-fbtest: missing required capability: %s\n",
           zz9k_capability_name(ZZ9K_CAP_FRAMEBUFFER_SURFACE));
    goto cleanup;
  }

  status = zz9k_map_framebuffer_surface(ctx, &surface);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-fbtest: map framebuffer failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto cleanup;
  }
  if ((surface.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0) {
    printf("zz9k-fbtest: framebuffer is not CPU-visible\n");
    goto cleanup;
  }
  if (!surface.data) {
    printf("zz9k-fbtest: framebuffer is not CPU-mapped by the host\n");
    goto cleanup;
  }

  bpp = zz9k_fb_bytes_per_pixel(surface.format);
  if (bpp == 0) {
    printf("zz9k-fbtest: unsupported framebuffer format: %lu\n",
           (unsigned long)surface.format);
    goto cleanup;
  }
  if (!zz9k_fb_choose_auto_rect(&surface, bpp, &rect)) {
    printf("zz9k-fbtest: could not choose a safe test rectangle\n");
    goto cleanup;
  }

  row_bytes = rect.w * bpp;
  backup_len = row_bytes * rect.h;
  backup = (uint8_t *)malloc(backup_len);
  if (!backup) {
    printf("zz9k-fbtest: backup allocation failed: %lu bytes\n",
           (unsigned long)backup_len);
    goto cleanup;
  }

  printf("Framebuffer:          %lu x %lu, pitch=%lu, format=%s (%lu)\n",
         (unsigned long)surface.width,
         (unsigned long)surface.height,
         (unsigned long)surface.pitch,
         zz9k_fb_format_name(surface.format),
         (unsigned long)surface.format);
  printf("Test rectangle:       x=%lu y=%lu w=%lu h=%lu\n",
         (unsigned long)rect.x,
         (unsigned long)rect.y,
         (unsigned long)rect.w,
         (unsigned long)rect.h);
  printf("Backup:               %lu bytes\n", (unsigned long)backup_len);
  printf("Hold:                 %lu ticks\n", (unsigned long)ticks);
  fflush(stdout);

  surface_data = (volatile uint8_t *)surface.data;
  zz9k_fb_copy_rect_from_surface(backup, surface_data, surface.pitch,
                                 &rect, bpp);
  zz9k_fb_draw_rect(surface_data, surface.pitch, &rect, bpp, surface.format);
  drew = 1;
  fbtest_delay_ticks(ticks);
  zz9k_fb_copy_rect_to_surface(surface_data, backup, surface.pitch,
                               &rect, bpp);
  drew = 0;

  printf("zz9k-fbtest: restored framebuffer rectangle\n");
  rc = 0;

cleanup:
  if (drew && backup) {
    zz9k_fb_copy_rect_to_surface((volatile uint8_t *)surface.data,
                                 backup, surface.pitch, &rect, bpp);
  }
  free(backup);
  zz9k_close(ctx);
  return rc;
}
#endif
