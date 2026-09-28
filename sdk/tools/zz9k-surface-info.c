/*
 * Read-only framebuffer surface metadata probe for SDK v2.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/surface.h"
#include "zz9k/text.h"
#include <stdint.h>
#include <stdio.h>

static void print_pointer_line(const char *label, volatile const void *ptr)
{
  uintptr_t value = (uintptr_t)ptr;

#if UINTPTR_MAX > 0xffffffffUL
  printf("%-22s0x%08lx%08lx\n", label,
         (unsigned long)((value >> 32) & 0xffffffffUL),
         (unsigned long)(value & 0xffffffffUL));
#else
  printf("%-22s0x%08lx\n", label, (unsigned long)value);
#endif
}

static void print_flag_name(int *first, const char *name)
{
  printf(" %s", name);
  *first = 0;
}

static void print_flags(uint32_t flags)
{
  uint32_t remaining;
  uint32_t count;
  uint32_t i;
  int first = 1;

  printf("Flags:                0x%08lx", (unsigned long)flags);
  remaining = flags;
  count = zz9k_known_surface_flag_count();
  for (i = 0; i < count; i++) {
    const char *name;
    uint32_t flag;

    flag = zz9k_known_surface_flag(i);
    if (flag == 0U || (flags & flag) == 0U) {
      continue;
    }

    name = zz9k_surface_flag_name(flag);
    if (name) {
      print_flag_name(&first, name);
      remaining &= ~flag;
    }
  }
  if (remaining != 0U) {
    printf(" 0x%08lx", (unsigned long)remaining);
    first = 0;
  }
  if (first) {
    printf(" none");
  }
  printf("\n");
}

static void print_service(ZZ9KContext *ctx, uint32_t service_id,
                          const char *fallback_name)
{
  ZZ9KServiceInfo service;
  const char *name;
  int status;

  status = zz9k_query_service(ctx, service_id, &service);
  if (status == ZZ9K_STATUS_NOT_FOUND ||
      status == ZZ9K_STATUS_UNSUPPORTED) {
    printf("Service %-10s unavailable: %s (%d)\n", fallback_name,
           zz9k_status_name(status), status);
    return;
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("Service %-10s query failed: %s (%d)\n", fallback_name,
           zz9k_status_name(status), status);
    return;
  }

  name = service.name[0] ? service.name : fallback_name;
  printf("Service %-10s version=%lu.%lu caps=0x%08lx flags=0x%08lx "
         "ops=0x%04lx+%lu\n",
         name,
         (unsigned long)((service.version >> 16) & 0xffffUL),
         (unsigned long)(service.version & 0xffffUL),
         (unsigned long)service.capability_bits,
         (unsigned long)service.flags,
         (unsigned long)service.opcode_base,
         (unsigned long)service.opcode_count);
}

static int require_cap(const ZZ9KCaps *caps, uint32_t bit)
{
  const char *name;

  if ((caps->capability_bits & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-surface-info: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

static int require_surface_caps(const ZZ9KCaps *caps)
{
  if (!require_cap(caps, ZZ9K_CAP_SURFACES) ||
      !require_cap(caps, ZZ9K_CAP_FRAMEBUFFER_SURFACE)) {
    return 0;
  }
  return 1;
}

int main(void)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KSurface framebuffer;
  const char *format;
  int status;

  printf("zz9k-surface-info: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surface-info: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }

  printf("zz9k-surface-info: querying capabilities\n");
  fflush(stdout);
  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surface-info: query caps failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }

  if (!require_surface_caps(&caps)) {
    zz9k_close(ctx);
    return 1;
  }

  printf("Capabilities:         0x%08lx\n",
         (unsigned long)caps.capability_bits);
  if (caps.capability_bits & ZZ9K_CAP_SERVICE_DISCOVERY) {
    print_service(ctx, ZZ9K_SERVICE_SURFACE, "surface");
    print_service(ctx, ZZ9K_SERVICE_IMAGE, "image");
  } else {
    printf("Service discovery:    unavailable\n");
  }

  printf("zz9k-surface-info: mapping framebuffer surface\n");
  fflush(stdout);
  status = zz9k_map_framebuffer_surface(ctx, &framebuffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-surface-info: map framebuffer failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }

  printf("Surface:              framebuffer\n");
  printf("Handle:               0x%08lx\n",
         (unsigned long)framebuffer.handle);
  printf("ARM address:          0x%08lx\n",
         (unsigned long)framebuffer.arm_addr);
  print_pointer_line("Host pointer:", framebuffer.data);
  printf("Size:                 %lu x %lu\n",
         (unsigned long)framebuffer.width,
         (unsigned long)framebuffer.height);
  printf("Pitch:                %lu bytes\n",
         (unsigned long)framebuffer.pitch);
  format = zz9k_surface_format_text(framebuffer.format);
  printf("Format:               %s (%lu)\n",
         format ? format : "unknown",
         (unsigned long)framebuffer.format);
  printf("Native RTG format:    %s\n",
         zz9k_surface_is_native_rtg_format(framebuffer.format) ?
         "yes" : "no");
  print_flags(framebuffer.flags);
  printf("Length:               %lu bytes\n",
         (unsigned long)framebuffer.length);

  zz9k_close(ctx);
  return 0;
}
