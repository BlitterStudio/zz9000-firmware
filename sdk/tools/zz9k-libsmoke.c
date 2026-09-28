/*
 * Real zz9k.library smoke test for shared memory, surfaces, framebuffer
 * mapping, scaler, and diagnostics.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/caps.h"
#include "zz9k-fb-common.h"
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZ9K_LIBSMOKE_HOLD_TICKS 50UL

struct Library *ZZ9KBase;

static int require_cap(const ZZ9KCaps *caps, uint32_t bit)
{
  const char *name;

  if ((caps->capability_bits & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-libsmoke: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

static int check_pattern(const ZZ9KSharedBuffer *buffer, uint8_t value)
{
  volatile const uint8_t *data;
  uint32_t middle;
  uint32_t last;

  if (!buffer || !buffer->data || buffer->length == 0) {
    return 0;
  }

  data = (volatile const uint8_t *)buffer->data;
  middle = buffer->length / 2U;
  last = buffer->length - 1U;
  return data[0] == value && data[middle] == value && data[last] == value;
}

static int choose_scale_rects(const ZZ9KSurface *framebuffer, uint32_t bpp,
                              ZZ9KFbRect *src, ZZ9KFbRect *dst)
{
  if (!src || !dst || !zz9k_fb_choose_auto_rect(framebuffer, bpp, dst)) {
    return 0;
  }

  src->x = 0;
  src->y = 0;
  src->w = dst->w > 1U ? dst->w / 2U : 1U;
  src->h = dst->h > 1U ? dst->h / 2U : 1U;
  return src->w != 0 && src->h != 0;
}

static void build_scale_desc(ZZ9KScaleImageDesc *desc, uint32_t src_handle,
                             const ZZ9KFbRect *src, const ZZ9KFbRect *dst)
{
  memset(desc, 0, sizeof(*desc));
  desc->src_surface = src_handle;
  desc->dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc->src_x = src->x;
  desc->src_y = src->y;
  desc->src_w = src->w;
  desc->src_h = src->h;
  desc->dst_x = dst->x;
  desc->dst_y = dst->y;
  desc->dst_w = dst->w;
  desc->dst_h = dst->h;
  desc->filter = ZZ9K_SCALE_NEAREST;
}

static void print_diag(void)
{
  ZZ9KDiagInfo diag;
  int status;

  memset(&diag, 0, sizeof(diag));
  status = ZZ9KReadDiag(&diag);
  if (status != ZZ9K_STATUS_OK) {
    printf("diag: read failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    return;
  }

  printf("diag: completed=%lu failed=%lu last_status=%s (%lu) "
         "heap_free=%lu largest=%lu\n",
         (unsigned long)diag.requests_completed,
         (unsigned long)diag.requests_failed,
         zz9k_fb_status_name((int)diag.last_status),
         (unsigned long)diag.last_status,
         (unsigned long)diag.shared_heap_free,
         (unsigned long)diag.shared_heap_largest_free);
}

int main(void)
{
  ZZ9KCaps caps;
  ZZ9KSharedBuffer src_buffer;
  ZZ9KSharedBuffer dst_buffer;
  ZZ9KSurface framebuffer;
  ZZ9KSurface source;
  ZZ9KScaleImageDesc scale;
  ZZ9KFbRect src_rect;
  ZZ9KFbRect dst_rect;
  volatile uint8_t *framebuffer_data;
  uint8_t *backup;
  uint32_t bpp;
  uint32_t backup_len;
  int source_allocated;
  int drew;
  int status;
  int rc;

  memset(&caps, 0, sizeof(caps));
  memset(&src_buffer, 0, sizeof(src_buffer));
  memset(&dst_buffer, 0, sizeof(dst_buffer));
  memset(&framebuffer, 0, sizeof(framebuffer));
  memset(&source, 0, sizeof(source));
  memset(&src_rect, 0, sizeof(src_rect));
  memset(&dst_rect, 0, sizeof(dst_rect));
  backup = 0;
  source_allocated = 0;
  drew = 0;
  rc = 1;

  printf("zz9k-libsmoke: opening zz9k.library\n");
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("open failed\n");
    return 20;
  }

  printf("open ok base=0x%08lx version=%u revision=%u\n",
         (unsigned long)ZZ9KBase, ZZ9KBase->lib_Version,
         ZZ9KBase->lib_Revision);

  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }

  printf("SDK ABI %u.%u caps=0x%08lx\n",
         (unsigned)caps.abi_major, (unsigned)caps.abi_minor,
         (unsigned long)caps.capability_bits);

  if (!require_cap(&caps, ZZ9K_CAP_SHARED_ALLOC) ||
      !require_cap(&caps, ZZ9K_CAP_MEMORY_OPS) ||
      !require_cap(&caps, ZZ9K_CAP_SURFACES) ||
      !require_cap(&caps, ZZ9K_CAP_FRAMEBUFFER_SURFACE) ||
      !require_cap(&caps, ZZ9K_CAP_IMAGE_SCALE) ||
      !require_cap(&caps, ZZ9K_CAP_DIAGNOSTICS)) {
    goto out;
  }

  status = ZZ9KAllocShared(4096U, 64U, 0, &src_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("shared src alloc failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }

  status = ZZ9KAllocShared(4096U, 64U, 0, &dst_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("shared dst alloc failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }

  status = ZZ9KMemFill(src_buffer.handle, 0, src_buffer.length, 0x6bU);
  if (status != ZZ9K_STATUS_OK) {
    printf("mem fill failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }

  status = ZZ9KMemCopy(dst_buffer.handle, 0, src_buffer.handle, 0,
                       src_buffer.length);
  if (status != ZZ9K_STATUS_OK) {
    printf("mem copy failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }
  if (!check_pattern(&dst_buffer, 0x6bU)) {
    printf("shared memory verification failed\n");
    goto out;
  }
  printf("shared memory: ok\n");

  status = ZZ9KMapFramebufferSurface(&framebuffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("map framebuffer failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }
  if ((framebuffer.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0) {
    printf("framebuffer is not CPU-visible\n");
    goto out;
  }
  if (!framebuffer.data) {
    printf("framebuffer is not CPU-mapped by the host\n");
    goto out;
  }

  bpp = zz9k_fb_bytes_per_pixel(framebuffer.format);
  if (bpp == 0) {
    printf("unsupported framebuffer format: %lu\n",
           (unsigned long)framebuffer.format);
    goto out;
  }
  if (!choose_scale_rects(&framebuffer, bpp, &src_rect, &dst_rect)) {
    printf("could not choose safe framebuffer scale rectangles\n");
    goto out;
  }

  printf("framebuffer: %lu x %lu pitch=%lu format=%s (%lu)\n",
         (unsigned long)framebuffer.width,
         (unsigned long)framebuffer.height,
         (unsigned long)framebuffer.pitch,
         zz9k_fb_format_name(framebuffer.format),
         (unsigned long)framebuffer.format);

  status = ZZ9KAllocSurfaceEx(src_rect.w, src_rect.h, framebuffer.format, 0,
                              src_rect.w * bpp, &source);
  if (status != ZZ9K_STATUS_OK) {
    printf("source surface alloc failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }
  source_allocated = 1;

  printf("source surface: handle=0x%08lx %lu x %lu pitch=%lu\n",
         (unsigned long)source.handle,
         (unsigned long)source.width,
         (unsigned long)source.height,
         (unsigned long)source.pitch);

  if ((source.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0 ||
      !zz9k_fb_fill_surface_pattern(&source)) {
    printf("source surface fill failed\n");
    goto out;
  }
  printf("source surface fill: ok\n");

  backup_len = dst_rect.w * bpp * dst_rect.h;
  backup = (uint8_t *)malloc(backup_len);
  if (!backup) {
    printf("backup allocation failed: %lu bytes\n",
           (unsigned long)backup_len);
    goto out;
  }

  framebuffer_data = (volatile uint8_t *)framebuffer.data;
  zz9k_fb_copy_rect_from_surface(backup, framebuffer_data,
                                 framebuffer.pitch, &dst_rect, bpp);
  build_scale_desc(&scale, source.handle, &src_rect, &dst_rect);

  status = ZZ9KScaleImage(&scale);
  if (status != ZZ9K_STATUS_OK) {
    printf("scale image failed: %s (%d)\n",
           zz9k_fb_status_name(status), status);
    goto out;
  }
  drew = 1;
  printf("framebuffer scale: ok, showing for %lu ticks\n",
         (unsigned long)ZZ9K_LIBSMOKE_HOLD_TICKS);
  Delay((LONG)ZZ9K_LIBSMOKE_HOLD_TICKS);
  zz9k_fb_copy_rect_to_surface(framebuffer_data, backup, framebuffer.pitch,
                               &dst_rect, bpp);
  drew = 0;
  printf("framebuffer restore: ok\n");

  print_diag();
  rc = 0;

out:
  if (drew && backup) {
    zz9k_fb_copy_rect_to_surface((volatile uint8_t *)framebuffer.data,
                                 backup, framebuffer.pitch, &dst_rect, bpp);
  }
  free(backup);
  if (source_allocated) {
    ZZ9KFreeSurface(source.handle);
  }
  if (dst_buffer.handle != 0 && dst_buffer.handle != ZZ9K_INVALID_HANDLE) {
    ZZ9KFreeShared(dst_buffer.handle);
  }
  if (src_buffer.handle != 0 && src_buffer.handle != ZZ9K_INVALID_HANDLE) {
    ZZ9KFreeShared(src_buffer.handle);
  }
  CloseLibrary(ZZ9KBase);
  return rc;
}
