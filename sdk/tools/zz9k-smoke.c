/*
 * Real-card smoke test for SDK v2 mailbox services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/image_geometry.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int require_cap(const ZZ9KCaps *caps, uint32_t bit)
{
  const char *name;

  if ((caps->capability_bits & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-smoke: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

static int check_pattern(const ZZ9KSharedBuffer *buffer, uint8_t value)
{
  volatile const uint8_t *data = (volatile const uint8_t *)buffer->data;
  uint32_t middle = buffer->length / 2U;
  uint32_t last = buffer->length - 1U;

  return data[0] == value && data[middle] == value && data[last] == value;
}

static void fill_bgra_gradient(const ZZ9KSurface *surface)
{
  uint32_t y;
  uint32_t x;

  for (y = 0; y < surface->height; y++) {
    volatile uint8_t *row =
      (volatile uint8_t *)surface->data + (y * surface->pitch);

    for (x = 0; x < surface->width; x++) {
      volatile uint8_t *pixel = row + (x * 4U);

      pixel[0] = (uint8_t)(x * 4U);
      pixel[1] = (uint8_t)(y * 4U);
      pixel[2] = (uint8_t)((x + y) * 2U);
      pixel[3] = 0xffU;
    }
  }
}

static int check_scaled_pixel(const ZZ9KSurface *surface)
{
  volatile const uint8_t *data = (volatile const uint8_t *)surface->data;

  return data[3] == 0xffU;
}

static void print_diag(ZZ9KContext *ctx)
{
  ZZ9KDiagInfo diag;

  if (zz9k_read_diag(ctx, &diag) != ZZ9K_STATUS_OK) {
    return;
  }

  printf("diag: completed=%lu failed=%lu last_status=%s (%lu) "
         "heap_free=%lu largest=%lu\n",
         (unsigned long)diag.requests_completed,
         (unsigned long)diag.requests_failed,
         zz9k_status_name((int)diag.last_status),
         (unsigned long)diag.last_status,
         (unsigned long)diag.shared_heap_free,
         (unsigned long)diag.shared_heap_largest_free);
}

static int zz9k_smoke_build_scale_desc(ZZ9KScaleImageDesc *desc,
                                       const ZZ9KSurface *src,
                                       const ZZ9KSurface *dst,
                                       uint32_t filter)
{
  ZZ9KRect dst_rect;

  if (!src || !dst) {
    return 0;
  }

  dst_rect.x = 0U;
  dst_rect.y = 0U;
  dst_rect.w = dst->width;
  dst_rect.h = dst->height;
  return zz9k_image_build_surface_scale_desc(
      desc, src->handle, dst->handle, src->width, src->height,
      &dst_rect, filter);
}

#ifndef ZZ9K_SMOKE_NO_MAIN
int main(void)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KSharedBuffer src_buffer;
  ZZ9KSharedBuffer dst_buffer;
  ZZ9KSurface src_surface;
  ZZ9KSurface dst_surface;
  ZZ9KScaleImageDesc scale;
  int status;
  int rc = 1;

  memset(&src_buffer, 0, sizeof(src_buffer));
  memset(&dst_buffer, 0, sizeof(dst_buffer));
  memset(&src_surface, 0, sizeof(src_surface));
  memset(&dst_surface, 0, sizeof(dst_surface));

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-smoke: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }
  if (!zz9k_shared_heap_board_visible(ctx)) {
    printf("note: this diagnostic stages its buffers in the default shared\n"
           "heap, which is not CPU-visible on Zorro II; its accelerated\n"
           "sections cannot run here (docs/zz9k-zorro2-services.md)\n");
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-smoke: query caps failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  printf("SDK ABI %u.%u caps=0x%08lx\n",
         (unsigned)caps.abi_major, (unsigned)caps.abi_minor,
         (unsigned long)caps.capability_bits);

  if (!require_cap(&caps, ZZ9K_CAP_SHARED_ALLOC) ||
      !require_cap(&caps, ZZ9K_CAP_MEMORY_OPS) ||
      !require_cap(&caps, ZZ9K_CAP_SURFACES) ||
      !require_cap(&caps, ZZ9K_CAP_IMAGE_SCALE)) {
    goto out;
  }

  status = zz9k_alloc_shared(ctx, 4096U, 64U, 0, &src_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("shared src alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_shared(ctx, 4096U, 64U, 0, &dst_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("shared dst alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_mem_fill(ctx, src_buffer.handle, 0, src_buffer.length, 0x5aU);
  if (status != ZZ9K_STATUS_OK) {
    printf("mem fill: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_mem_copy(ctx, dst_buffer.handle, 0, src_buffer.handle, 0,
                         src_buffer.length);
  if (status != ZZ9K_STATUS_OK) {
    printf("mem copy: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  if (!check_pattern(&dst_buffer, 0x5aU)) {
    printf("mem copy: pattern verification failed\n");
    goto out;
  }
  printf("shared memory: ok\n");

  status = zz9k_alloc_surface_ex(ctx, 64U, 64U, ZZ9K_SURFACE_FORMAT_BGRA8888,
                                 0, 64U * 4U, &src_surface);
  if (status != ZZ9K_STATUS_OK) {
    printf("src surface alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_surface_ex(ctx, 128U, 128U, ZZ9K_SURFACE_FORMAT_BGRA8888,
                                 0, 128U * 4U, &dst_surface);
  if (status != ZZ9K_STATUS_OK) {
    printf("dst surface alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  fill_bgra_gradient(&src_surface);

  if (!zz9k_smoke_build_scale_desc(&scale, &src_surface, &dst_surface,
                                   ZZ9K_SCALE_NEAREST)) {
    printf("scale image: could not build descriptor\n");
    goto out;
  }

  status = zz9k_scale_image(ctx, &scale);
  if (status != ZZ9K_STATUS_OK) {
    printf("scale image: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  if (!check_scaled_pixel(&dst_surface)) {
    printf("scale image: destination verification failed\n");
    goto out;
  }
  printf("surface scale: ok\n");

  print_diag(ctx);
  rc = 0;

out:
  if (dst_surface.handle != 0 && dst_surface.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_surface(ctx, dst_surface.handle);
  }
  if (src_surface.handle != 0 && src_surface.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_surface(ctx, src_surface.handle);
  }
  if (dst_buffer.handle != 0 && dst_buffer.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, dst_buffer.handle);
  }
  if (src_buffer.handle != 0 && src_buffer.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, src_buffer.handle);
  }
  zz9k_close(ctx);
  return rc;
}
#endif
