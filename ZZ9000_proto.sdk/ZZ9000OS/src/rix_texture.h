/*
 * RasterIX external texture upload conversion for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RIX_TEXTURE_H
#define RIX_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "rix_cmd.h"

/* Converts RRGGBBAA client texels to the RasterIX device format.  The output
 * is explicitly little-endian, so it is suitable for the ARM's DDR upload. */
int rix_texture_convert_rgba8_le(uint8_t *destination, size_t destination_size,
                                 const uint32_t *rgba, size_t texel_count,
                                 enum RixTextureFormat format);
/* TextureSampler's sparse mip layout, in texels, for power-of-two dimensions
 * no larger than 256.  A full virtual texture is 128 2 KiB pages. */
uint32_t rix_texture_mip_offset(uint8_t width_log2, uint8_t height_log2,
                                uint8_t level);

#endif
