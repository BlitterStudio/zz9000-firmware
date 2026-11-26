/*
 * Header-only helpers for the shared-buffer output of a media session:
 * the geometry of a picture, the fixed palettes, and a check that mirrors
 * what the firmware accepts in ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG.
 *
 * Copyright (C) 2024-2026 Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#ifndef ZZ9K_MEDIA_OUTPUT_H
#define ZZ9K_MEDIA_OUTPUT_H

#include "zz9k/abi.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a valid output configuration occupies in the buffer. */
typedef struct ZZ9KMediaOutputLayout {
  uint32_t depth;         /* 1..8 for INDEX8 and PLANAR, 0 for RGB */
  uint32_t row_bytes;     /* smallest legal pitch */
  uint32_t picture_bytes; /* one picture */
  uint32_t slots;         /* 1 or 2 */
  uint32_t span_bytes;    /* from slot 0 to the end of the last slot */
} ZZ9KMediaOutputLayout;

/* The bit depth a flags word asks for: 0 means 8. */
static inline uint32_t zz9k_media_output_depth(uint32_t flags)
{
  uint32_t depth = ZZ9K_MEDIA_OUTPUT_DEPTH(flags);

  return depth == 0U ? 8U : depth;
}

/* Bytes in one row of a picture of `width` pixels: one plane for PLANAR. */
static inline uint32_t zz9k_media_output_row_bytes(uint32_t format,
                                                   uint32_t width)
{
  if (format == ZZ9K_SURFACE_FORMAT_INDEX8) {
    return width;
  }
  if (format == ZZ9K_SURFACE_FORMAT_PLANAR) {
    return (width + 7U) / 8U;
  }
  return width * 2U;
}

/* Checks a configuration the way the firmware does, short of the buffer
 * itself: returns ZZ9K_STATUS_OK, ZZ9K_STATUS_BAD_REQUEST or
 * ZZ9K_STATUS_UNSUPPORTED (a format or flag the firmware lacks: today,
 * ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION). `layout` is filled on success. */
static inline int zz9k_media_output_check(const ZZ9KMediaSessionOutputDesc *desc,
                                          ZZ9KMediaOutputLayout *layout)
{
  ZZ9KMediaOutputLayout out;
  uint64_t picture;
  uint64_t span;
  uint32_t flags;

  if (!desc || !layout) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  flags = desc->flags;
  if ((flags & ~(ZZ9K_MEDIA_OUTPUT_DEPTH_MASK |
                 ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED |
                 ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED |
                 ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION |
                 ZZ9K_MEDIA_OUTPUT_PALETTE_HOST |
                 ZZ9K_MEDIA_OUTPUT_SCALE_NEAREST)) != 0U ||
      desc->width == 0U || desc->width > ZZ9K_MEDIA_OUTPUT_MAX_WIDTH ||
      desc->height == 0U || desc->height > ZZ9K_MEDIA_OUTPUT_MAX_HEIGHT ||
      (desc->buffer_offset & 3U) != 0U || (desc->slot_stride & 3U) != 0U ||
      ((flags & ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED) != 0U &&
       (flags & ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION) != 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(&out, 0, sizeof(out));
  switch (desc->format) {
  case ZZ9K_SURFACE_FORMAT_INDEX8:
  case ZZ9K_SURFACE_FORMAT_PLANAR:
    out.depth = zz9k_media_output_depth(flags);
    if (out.depth > 8U ||
        (desc->format == ZZ9K_SURFACE_FORMAT_INDEX8 &&
         (flags & ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0U)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    break;
  case ZZ9K_SURFACE_FORMAT_RGB565:
  case ZZ9K_SURFACE_FORMAT_RGB555:
    if ((flags & (ZZ9K_MEDIA_OUTPUT_DEPTH_MASK |
                  ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED |
                  ZZ9K_MEDIA_OUTPUT_PALETTE_HOST)) != 0U) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    break;
  default:
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if ((flags & ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION) != 0U) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (((flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) != 0U) !=
      (desc->palette_handle != 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if ((flags & ZZ9K_MEDIA_OUTPUT_PALETTE_HOST) == 0U &&
      desc->palette_offset != 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  out.row_bytes = zz9k_media_output_row_bytes(desc->format, desc->width);
  if (desc->pitch < out.row_bytes) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  picture = (uint64_t)desc->pitch * desc->height;
  if (desc->format == ZZ9K_SURFACE_FORMAT_PLANAR) {
    picture *= out.depth;
  }
  out.slots = desc->slot_stride != 0U ? 2U : 1U;
  span = picture;
  if (out.slots == 2U) {
    if (desc->slot_stride < picture) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    span += desc->slot_stride;
  }
  if (span > 0xffffffffULL) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  out.picture_bytes = (uint32_t)picture;
  out.span_bytes = (uint32_t)span;
  *layout = out;
  return ZZ9K_STATUS_OK;
}

/* Byte offset, from the start of a slot, of row `row` of plane `plane` of a
 * PLANAR picture. Separate planes (the default) are a normal BitMap: plane
 * p, row y at (p * height + y) * pitch. Interleaved planes follow
 * BMF_INTERLEAVED: row y, plane p at (y * depth + p) * pitch. */
static inline uint32_t zz9k_media_output_plane_row_offset(
    uint32_t flags, uint32_t height, uint32_t pitch, uint32_t plane,
    uint32_t row)
{
  if ((flags & ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED) != 0U) {
    return (row * zz9k_media_output_depth(flags) + plane) * pitch;
  }
  return (plane * height + row) * pitch;
}

/* The fixed palette of an INDEX8 or PLANAR output of `depth` bits (1..8),
 * used when ZZ9K_MEDIA_OUTPUT_PALETTE_HOST is clear: 256 words of
 * 0x00RRGGBB, the entries beyond the colours in use zero. Load it into the
 * screen before showing the pictures. Returns the number of colours in use,
 * or 0 for a depth outside 1..8.
 *
 * Depths 1 and 2 are gray ramps of 2 and 4 levels. Depths 3 to 8 are colour
 * cubes of lr x lg x lb levels, colour i = (r * lg + g) * lb + b:
 *   3: 2x2x2   4: 2x4x2   5: 4x4x2   6: 4x4x4   7: 5x5x5   8: 6x7x6 (252)
 * A channel with L levels has level k at round(k * 255 / (L - 1)). */
static inline uint32_t zz9k_media_output_fixed_palette(uint32_t depth,
                                                       uint32_t out[256])
{
  static const uint8_t dims[9][4] = {
    { 0, 0, 0, 0 }, { 2, 2, 2, 1 }, { 4, 4, 4, 1 }, { 2, 2, 2, 0 },
    { 2, 4, 2, 0 }, { 4, 4, 2, 0 }, { 4, 4, 4, 0 }, { 5, 5, 5, 0 },
    { 6, 7, 6, 0 }
  };
  uint32_t i;
  uint32_t used;

  memset(out, 0, 256U * sizeof(out[0]));
  if (depth < 1U || depth > 8U) {
    return 0U;
  }
  if (dims[depth][3] != 0U) {
    used = 1U << depth;
    for (i = 0U; i < used; i++) {
      uint32_t v = (i * 255U + (used - 1U) / 2U) / (used - 1U);

      out[i] = (v << 16) | (v << 8) | v;
    }
    return used;
  }
  used = (uint32_t)dims[depth][0] * dims[depth][1] * dims[depth][2];
  for (i = 0U; i < used; i++) {
    uint32_t lr = dims[depth][0];
    uint32_t lg = dims[depth][1];
    uint32_t lb = dims[depth][2];
    uint32_t b = i % lb;
    uint32_t g = (i / lb) % lg;
    uint32_t r = i / (lb * lg);

    out[i] = (((r * 255U + (lr - 1U) / 2U) / (lr - 1U)) << 16) |
             (((g * 255U + (lg - 1U) / 2U) / (lg - 1U)) << 8) |
             ((b * 255U + (lb - 1U) / 2U) / (lb - 1U));
  }
  return used;
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_MEDIA_OUTPUT_H */
