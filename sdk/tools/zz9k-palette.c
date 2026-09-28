/*
 * Primary display CLUT probe for SDK v2.
 *
 * Reads the INDEX8 palette back through ZZ9K_OP_QUERY_PALETTE and prints it,
 * so a hardware run can be compared against the palette the Amiga side
 * actually set. Reading the CLUT says nothing about whether 8-bit overlay
 * composition is enabled; this tool only reports what firmware has shadowed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zz9k/text.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_BUFFER_BYTES \
  ((uint32_t)ZZ9K_PALETTE_MAX_ENTRIES * (uint32_t)ZZ9K_PALETTE_ENTRY_BYTES)

struct options {
  uint32_t start;
  uint32_t count;
  int quiet;
};

static void usage(void)
{
  printf("usage: zz9k-palette [START [COUNT]] [-q]\n");
  printf("  START   first palette index (default 0)\n");
  printf("  COUNT   number of entries (default: to the end of the table)\n");
  printf("  -q      print only the summary, not every entry\n");
}

static int parse_index(const char *text, uint32_t *out)
{
  char *end;
  unsigned long value;

  if (!text || !*text) {
    return 0;
  }

  end = 0;
  value = strtoul(text, &end, 0);
  if (!end || *end != '\0' || value > (unsigned long)ZZ9K_PALETTE_MAX_ENTRIES) {
    return 0;
  }

  *out = (uint32_t)value;
  return 1;
}

static int parse_options(int argc, char **argv, struct options *opts)
{
  int positional = 0;
  int i;

  opts->start = 0U;
  opts->count = 0U;
  opts->quiet = 0;

  for (i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "-q") == 0) {
      opts->quiet = 1;
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      return 0;
    }

    if (positional == 0) {
      if (!parse_index(arg, &opts->start)) {
        return 0;
      }
      positional = 1;
    } else if (positional == 1) {
      if (!parse_index(arg, &opts->count)) {
        return 0;
      }
      positional = 2;
    } else {
      return 0;
    }
  }

  if (opts->count == 0U) {
    opts->count = (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES - opts->start;
  }
  if (opts->count == 0U ||
      opts->start + opts->count > (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES) {
    return 0;
  }

  return 1;
}

/* The reply is a bulk write into the shared buffer, so the entries are read
 * back as big-endian 0x00RRGGBB words regardless of host byte order. */
static uint32_t read_entry(volatile const unsigned char *base, uint32_t index)
{
  volatile const unsigned char *p = base + (index * 4U);

  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int require_palette_service(ZZ9KContext *ctx, const ZZ9KCaps *caps)
{
  ZZ9KServiceInfo service;
  int status;

  if ((caps->capability_bits & ZZ9K_CAP_SERVICE_DISCOVERY) == 0U) {
    /* Without discovery there is no flag to check; let the op itself answer. */
    printf("Service discovery:    unavailable, attempting the query anyway\n");
    return 1;
  }

  status = zz9k_query_service(ctx, ZZ9K_SERVICE_SURFACE, &service);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-palette: surface service query failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 0;
  }

  if ((service.flags & ZZ9K_SERVICE_FLAG_SURFACE_PALETTE_QUERY) == 0U) {
    printf("zz9k-palette: firmware does not advertise "
           "SURFACE_PALETTE_QUERY\n");
    printf("              (surface service flags 0x%08lx)\n",
           (unsigned long)service.flags);
    return 0;
  }

  printf("Surface service:      palette query supported "
         "(flags 0x%08lx)\n", (unsigned long)service.flags);
  return 1;
}

int main(int argc, char **argv)
{
  struct options opts;
  ZZ9KContext *ctx;
  ZZ9KCaps caps;
  ZZ9KSharedBuffer buffer;
  ZZ9KPaletteQueryDesc desc;
  volatile const unsigned char *entries;
  uint32_t nonzero = 0U;
  uint32_t i;
  int status;

  if (!parse_options(argc, argv, &opts)) {
    usage();
    return 2;
  }

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-palette: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-palette: query caps failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }

  if (!require_palette_service(ctx, &caps)) {
    zz9k_close(ctx);
    return 1;
  }

  memset(&buffer, 0, sizeof(buffer));
  status = zz9k_alloc_shared(ctx, PALETTE_BUFFER_BYTES, 16U, 0U, &buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-palette: shared alloc failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }
  if (!buffer.data) {
    /* An unmapped buffer is a valid allocation, but this tool has to read
     * the reply with the 68k, so it cannot use one. */
    printf("zz9k-palette: shared buffer is not host-addressable\n");
    zz9k_free_shared(ctx, buffer.handle);
    zz9k_close(ctx);
    return 1;
  }

  /* Poison the destination so a firmware that silently writes nothing cannot
   * be mistaken for one that returned an all-black palette. */
  for (i = 0; i < PALETTE_BUFFER_BYTES; i++) {
    ((volatile unsigned char *)buffer.data)[i] = 0xa5U;
  }

  memset(&desc, 0, sizeof(desc));
  desc.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.start = opts.start;
  desc.count = opts.count;
  desc.dst_handle = buffer.handle;
  desc.dst_offset = 0U;
  desc.flags = 0U;

  status = zz9k_query_palette(ctx, &desc);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-palette: query failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_free_shared(ctx, buffer.handle);
    zz9k_close(ctx);
    return 1;
  }

  entries = (volatile const unsigned char *)buffer.data;
  printf("Palette entries:      %lu..%lu\n",
         (unsigned long)opts.start,
         (unsigned long)(opts.start + opts.count - 1U));

  for (i = 0; i < opts.count; i++) {
    uint32_t value = read_entry(entries, i);

    if (value != 0U) {
      nonzero++;
    }
    if (!opts.quiet) {
      printf("  %3lu  0x%06lx  r=%3lu g=%3lu b=%3lu\n",
             (unsigned long)(opts.start + i),
             (unsigned long)(value & 0x00ffffffUL),
             (unsigned long)((value >> 16) & 0xffUL),
             (unsigned long)((value >> 8) & 0xffUL),
             (unsigned long)(value & 0xffUL));
    }
  }

  printf("Non-black entries:    %lu of %lu\n",
         (unsigned long)nonzero, (unsigned long)opts.count);

  /* The high byte is reserved and must read back as zero; a non-zero one
   * means the destination was not fully written. */
  for (i = 0; i < opts.count; i++) {
    if ((read_entry(entries, i) & 0xff000000UL) != 0U) {
      printf("zz9k-palette: entry %lu has a non-zero reserved byte\n",
             (unsigned long)(opts.start + i));
      zz9k_free_shared(ctx, buffer.handle);
      zz9k_close(ctx);
      return 1;
    }
  }

  printf("zz9k-palette: ok\n");
  zz9k_free_shared(ctx, buffer.handle);
  zz9k_close(ctx);
  return 0;
}
