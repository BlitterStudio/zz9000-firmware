/*
 * zz9k-mediaout: option parsing, MPEG header probing, picture geometry, the
 * checksum and the conversion of a finished picture to 32-bit pixels for the
 * optional window. Header-only so the host suite can test it without a card.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_MEDIAOUT_LOGIC_H
#define ZZ9K_MEDIAOUT_LOGIC_H

#include "zz9k/abi.h"
#include "zz9k/media_output.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZ9K_MEDIAOUT_DEFAULT_COUNT 50UL
#define ZZ9K_MEDIAOUT_MAX_COUNT 100000UL

typedef struct ZZ9KMediaOutOptions {
  const char *path;
  uint32_t format;      /* ZZ9K_SURFACE_FORMAT_* */
  uint32_t width;       /* output size; 0 takes the size of the picture */
  uint32_t height;
  uint32_t depth;       /* INDEX8 and PLANAR bits, 1..8 */
  uint32_t flags;       /* ZZ9KMediaOutputFlags without the depth bits */
  uint32_t count;       /* pictures to present */
  int single_slot;
  int window;
  int checksum;
} ZZ9KMediaOutOptions;

typedef struct ZZ9KMediaOutMpegInfo {
  uint32_t width;
  uint32_t height;
  uint32_t video_codec;   /* ZZ9K_VIDEO_CODEC_MPEG1 or _MPEG2 */
} ZZ9KMediaOutMpegInfo;

typedef struct ZZ9KMediaOutPlan {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t pitch;
  uint32_t flags;         /* including the depth bits */
  uint32_t depth;         /* 0 for RGB */
  uint32_t picture_bytes;
  uint32_t slot_stride;   /* 0 = one slot */
  uint32_t slots;
  uint32_t buffer_bytes;  /* both slots */
} ZZ9KMediaOutPlan;

static inline void zz9k_mediaout_usage(void)
{
  printf("usage: zz9k-mediaout FILE.mpg [options]\n");
  printf("Plays an MPEG-1 or MPEG-2 program stream through a media session\n");
  printf("whose output is a shared buffer, and prints a checksum, the slot\n");
  printf("and the timing of every picture.\n");
  printf("  -f FORMAT  rgb565 (default), rgb555, index8 or planar\n");
  printf("  -s WxH     output size (default: the size of the stream)\n");
  printf("  -d DEPTH   bits of index8 or planar, 1..8 (default 8)\n");
  printf("  -i         interleaved planes (planar)\n");
  printf("  -o         ordered dither\n");
  printf("  -n         nearest-neighbour scaling (default: bilinear)\n");
  printf("  -H         read a gray ramp as the host palette (index8, planar)\n");
  printf("  -1         one slot instead of two\n");
  printf("  -c N       pictures to present (default %lu)\n",
         (unsigned long)ZZ9K_MEDIAOUT_DEFAULT_COUNT);
  printf("  -x         skip the checksum (times the card only)\n");
  printf("  -w         show every picture in a window (cybergraphics)\n");
}

static inline int zz9k_mediaout_parse_size(const char *text, uint32_t *width,
                                           uint32_t *height)
{
  char *end = 0;
  unsigned long w;
  unsigned long h;

  if (!text || !*text) {
    return 0;
  }
  w = strtoul(text, &end, 10);
  if (!end || (*end != 'x' && *end != 'X') || w == 0UL ||
      w > ZZ9K_MEDIA_OUTPUT_MAX_WIDTH) {
    return 0;
  }
  text = end + 1;
  h = strtoul(text, &end, 10);
  if (!end || *end != '\0' || end == text || h == 0UL ||
      h > ZZ9K_MEDIA_OUTPUT_MAX_HEIGHT) {
    return 0;
  }
  *width = (uint32_t)w;
  *height = (uint32_t)h;
  return 1;
}

static inline int zz9k_mediaout_parse_format(const char *name,
                                             uint32_t *format)
{
  if (!name) {
    return 0;
  }
  if (strcmp(name, "rgb565") == 0) {
    *format = ZZ9K_SURFACE_FORMAT_RGB565;
  } else if (strcmp(name, "rgb555") == 0) {
    *format = ZZ9K_SURFACE_FORMAT_RGB555;
  } else if (strcmp(name, "index8") == 0) {
    *format = ZZ9K_SURFACE_FORMAT_INDEX8;
  } else if (strcmp(name, "planar") == 0) {
    *format = ZZ9K_SURFACE_FORMAT_PLANAR;
  } else {
    return 0;
  }
  return 1;
}

static inline const char *zz9k_mediaout_format_name(uint32_t format)
{
  switch (format) {
  case ZZ9K_SURFACE_FORMAT_RGB565:
    return "rgb565";
  case ZZ9K_SURFACE_FORMAT_RGB555:
    return "rgb555";
  case ZZ9K_SURFACE_FORMAT_INDEX8:
    return "index8";
  case ZZ9K_SURFACE_FORMAT_PLANAR:
    return "planar";
  default:
    return "?";
  }
}

/* Returns 1 for a usable command line, 0 for anything the usage text should
 * answer (including -h). */
static inline int zz9k_mediaout_parse(int argc, char **argv,
                                      ZZ9KMediaOutOptions *o)
{
  int i;

  memset(o, 0, sizeof(*o));
  o->format = ZZ9K_SURFACE_FORMAT_RGB565;
  o->depth = 8U;
  o->count = (uint32_t)ZZ9K_MEDIAOUT_DEFAULT_COUNT;
  o->checksum = 1;
  for (i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (arg[0] != '-' || arg[1] == '\0') {
      if (o->path) {
        return 0;
      }
      o->path = arg;
      continue;
    }
    if (arg[2] != '\0') {
      return 0;
    }
    switch (arg[1]) {
    case 'f':
      if (++i >= argc || !zz9k_mediaout_parse_format(argv[i], &o->format)) {
        return 0;
      }
      break;
    case 's':
      if (++i >= argc ||
          !zz9k_mediaout_parse_size(argv[i], &o->width, &o->height)) {
        return 0;
      }
      break;
    case 'd': {
      char *end = 0;
      unsigned long depth;

      if (++i >= argc) {
        return 0;
      }
      depth = strtoul(argv[i], &end, 10);
      if (!end || *end != '\0' || depth < 1UL || depth > 8UL) {
        return 0;
      }
      o->depth = (uint32_t)depth;
      break;
    }
    case 'c': {
      char *end = 0;
      unsigned long count;

      if (++i >= argc) {
        return 0;
      }
      count = strtoul(argv[i], &end, 10);
      if (!end || *end != '\0' || count < 1UL ||
          count > ZZ9K_MEDIAOUT_MAX_COUNT) {
        return 0;
      }
      o->count = (uint32_t)count;
      break;
    }
    case 'i':
      o->flags |= ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED;
      break;
    case 'o':
      o->flags |= ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED;
      break;
    case 'n':
      o->flags |= ZZ9K_MEDIA_OUTPUT_SCALE_NEAREST;
      break;
    case 'H':
      o->flags |= ZZ9K_MEDIA_OUTPUT_PALETTE_HOST;
      break;
    case '1':
      o->single_slot = 1;
      break;
    case 'x':
      o->checksum = 0;
      break;
    case 'w':
      o->window = 1;
      break;
    default:
      return 0;
    }
  }
  if (!o->path) {
    return 0;
  }
  /* options that only make sense for some formats */
  if (o->format != ZZ9K_SURFACE_FORMAT_PLANAR &&
      (o->flags & ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0U) {
    return 0;
  }
  if ((o->format == ZZ9K_SURFACE_FORMAT_RGB565 ||
       o->format == ZZ9K_SURFACE_FORMAT_RGB555) &&
      (o->flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) != 0U) {
    return 0;
  }
  return 1;
}

/* Finds the first MPEG sequence header in the data and reads the picture
 * size from it. A sequence extension right after it makes the stream MPEG-2.
 * Returns 0 when the data holds no sequence header. */
static inline int zz9k_mediaout_mpeg_info(const uint8_t *data,
                                          uint32_t length,
                                          ZZ9KMediaOutMpegInfo *info)
{
  uint32_t i;

  if (!data || !info || length < 12U) {
    return 0;
  }
  for (i = 0U; i + 12U <= length; i++) {
    uint32_t j;

    if (data[i] != 0U || data[i + 1U] != 0U || data[i + 2U] != 1U ||
        data[i + 3U] != 0xb3U) {
      continue;
    }
    info->width = ((uint32_t)data[i + 4U] << 4) | (data[i + 5U] >> 4);
    info->height = ((uint32_t)(data[i + 5U] & 0x0fU) << 8) | data[i + 6U];
    info->video_codec = ZZ9K_VIDEO_CODEC_MPEG1;
    /* the extension follows the header, after at most two quantiser
     * matrices of 64 bytes */
    for (j = i + 12U; j + 5U <= length && j < i + 12U + 128U; j++) {
      if (data[j] == 0U && data[j + 1U] == 0U && data[j + 2U] == 1U &&
          data[j + 3U] == 0xb5U && (data[j + 4U] >> 4) == 1U) {
        info->video_codec = ZZ9K_VIDEO_CODEC_MPEG2;
        break;
      }
    }
    return info->width != 0U && info->height != 0U;
  }
  return 0;
}

/* A program stream opens with a pack header. */
static inline int zz9k_mediaout_is_program_stream(const uint8_t *data,
                                                  uint32_t length)
{
  return data && length >= 4U && data[0] == 0U && data[1] == 0U &&
         data[2] == 1U && data[3] == 0xbaU;
}

/* FNV-1a over bytes, so a run is comparable with another one that prints the
 * same picture size. */
static inline uint32_t zz9k_mediaout_checksum(uint32_t seed,
                                              const uint8_t *data,
                                              uint32_t length)
{
  uint32_t hash = seed;
  uint32_t i;

  for (i = 0U; i < length; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

#define ZZ9K_MEDIAOUT_CHECKSUM_SEED 2166136261UL

/* Checksum of the bytes a picture really holds: the active bytes of every
 * row (every plane row for PLANAR). The bytes between the end of a row and
 * the pitch are never written by the card, so they are left out; they hold
 * whatever the allocation held. `slot` may be card memory; `row` is scratch
 * for one row, ZZ9K_MEDIAOUT_ROW_SCRATCH bytes (kept off the small default
 * Amiga stack by the caller). */
#define ZZ9K_MEDIAOUT_ROW_SCRATCH (ZZ9K_MEDIA_OUTPUT_MAX_WIDTH * 2U)
static inline uint32_t zz9k_mediaout_checksum_picture(
    const ZZ9KMediaOutPlan *plan, const volatile uint8_t *slot, uint32_t hash,
    uint8_t *row)
{
  uint32_t row_bytes = zz9k_media_output_row_bytes(plan->format, plan->width);
  uint32_t planes = plan->format == ZZ9K_SURFACE_FORMAT_PLANAR
                        ? plan->depth : 1U;
  uint32_t plane;
  uint32_t y;

  for (plane = 0U; plane < planes; plane++) {
    for (y = 0U; y < plan->height; y++) {
      uint32_t offset = plan->format == ZZ9K_SURFACE_FORMAT_PLANAR
          ? zz9k_media_output_plane_row_offset(plan->flags, plan->height,
                                               plan->pitch, plane, y)
          : y * plan->pitch;

      memcpy(row, (const void *)(slot + offset), row_bytes);
      hash = zz9k_mediaout_checksum(hash, row, row_bytes);
    }
  }
  return hash;
}

/* Pictures per second times 100, from a count and the time it took. The
 * products are 64-bit: unsigned long is 32 bits on the 68k. */
static inline uint64_t zz9k_mediaout_rate_x100(uint32_t count,
                                               uint32_t elapsed_us)
{
  if (elapsed_us == 0U) {
    return 0U;
  }
  return ((uint64_t)count * 100000000ULL) / elapsed_us;
}

/* The next smaller input chunk to try when the host window cannot take the
 * one asked for, or 0 below the smallest useful size. */
static inline uint32_t zz9k_mediaout_smaller_chunk(uint32_t chunk)
{
  return chunk > 2048U ? chunk / 2U : 0U;
}

/* Geometry of the output for a stream of src_w x src_h: size, pitch, slot
 * stride and buffer size. Returns a ZZ9K_STATUS_* code from the same check
 * the card applies. Pitches are rounded up to an even number of bytes so a
 * planar picture can be copied into a BitMap row by row. */
static inline int zz9k_mediaout_plan(const ZZ9KMediaOutOptions *o,
                                     uint32_t src_w, uint32_t src_h,
                                     ZZ9KMediaOutPlan *plan)
{
  ZZ9KMediaSessionOutputDesc desc;
  ZZ9KMediaOutputLayout layout;
  int status;

  memset(plan, 0, sizeof(*plan));
  plan->width = o->width != 0U ? o->width : src_w;
  plan->height = o->height != 0U ? o->height : src_h;
  plan->format = o->format;
  plan->flags = o->flags;
  if (o->format == ZZ9K_SURFACE_FORMAT_INDEX8 ||
      o->format == ZZ9K_SURFACE_FORMAT_PLANAR) {
    plan->depth = o->depth;
    plan->flags |= ZZ9K_MEDIA_OUTPUT_DEPTH(o->depth);
  }
  plan->pitch = (zz9k_media_output_row_bytes(o->format, plan->width) + 1U) &
                ~1U;

  memset(&desc, 0, sizeof(desc));
  desc.session = 1U;
  desc.buffer_handle = 1U;
  desc.width = plan->width;
  desc.height = plan->height;
  desc.format = plan->format;
  desc.pitch = plan->pitch;
  desc.flags = plan->flags;
  desc.palette_handle = (plan->flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) != 0U
                            ? 1U : 0U;
  status = zz9k_media_output_check(&desc, &layout);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  plan->picture_bytes = layout.picture_bytes;
  plan->slots = o->single_slot ? 1U : 2U;
  plan->slot_stride = o->single_slot
                          ? 0U : ((layout.picture_bytes + 3U) & ~3U);
  plan->buffer_bytes = plan->slots == 2U
                           ? plan->slot_stride + layout.picture_bytes
                           : layout.picture_bytes;
  return ZZ9K_STATUS_OK;
}

/* The host palette the -H option provides: a gray ramp over the 1 << depth
 * colours in use, as 256 big-endian 0x00RRGGBB words. */
static inline void zz9k_mediaout_gray_palette(uint32_t depth,
                                              uint8_t bytes[1024])
{
  uint32_t n = 1U << depth;
  uint32_t i;

  memset(bytes, 0, 1024U);
  for (i = 0U; i < n && i < 256U; i++) {
    uint8_t v = (uint8_t)(n > 1U ? (i * 255U) / (n - 1U) : 0U);

    bytes[i * 4U + 1U] = v;
    bytes[i * 4U + 2U] = v;
    bytes[i * 4U + 3U] = v;
  }
}

/* Big-endian word `index` of a palette byte table. */
static inline uint32_t zz9k_mediaout_palette_word(const uint8_t *bytes,
                                                  uint32_t index)
{
  const uint8_t *p = bytes + index * 4U;

  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

/* Converts row `y` of a finished picture in `slot` to 0x00RRGGBB words.
 * `palette` is the 256-word table of an INDEX8 or PLANAR picture. */
static inline void zz9k_mediaout_row_to_rgb(const ZZ9KMediaOutPlan *plan,
                                            const uint8_t *slot, uint32_t y,
                                            const uint32_t *palette,
                                            uint32_t *out)
{
  uint32_t x;

  if (plan->format == ZZ9K_SURFACE_FORMAT_RGB565 ||
      plan->format == ZZ9K_SURFACE_FORMAT_RGB555) {
    const uint8_t *row = slot + (size_t)y * plan->pitch;

    for (x = 0U; x < plan->width; x++) {
      uint32_t v = ((uint32_t)row[2U * x] << 8) | row[2U * x + 1U];
      uint32_t r, g, b;

      if (plan->format == ZZ9K_SURFACE_FORMAT_RGB565) {
        r = (v >> 11) & 31U;
        g = (v >> 5) & 63U;
        b = v & 31U;
        g = (g << 2) | (g >> 4);
      } else {
        r = (v >> 10) & 31U;
        g = (v >> 5) & 31U;
        b = v & 31U;
        g = (g << 3) | (g >> 2);
      }
      r = (r << 3) | (r >> 2);
      b = (b << 3) | (b >> 2);
      out[x] = (r << 16) | (g << 8) | b;
    }
    return;
  }
  if (plan->format == ZZ9K_SURFACE_FORMAT_INDEX8) {
    const uint8_t *row = slot + (size_t)y * plan->pitch;

    for (x = 0U; x < plan->width; x++) {
      out[x] = palette[row[x]];
    }
    return;
  }
  for (x = 0U; x < plan->width; x++) {
    uint32_t index = 0U;
    uint32_t p;

    for (p = 0U; p < plan->depth; p++) {
      const uint8_t *row = slot + (size_t)zz9k_media_output_plane_row_offset(
          plan->flags, plan->height, plan->pitch, p, y);

      index |= (uint32_t)((row[x >> 3] >> (7U - (x & 7U))) & 1U) << p;
    }
    out[x] = palette[index];
  }
}

#endif /* ZZ9K_MEDIAOUT_LOGIC_H */
