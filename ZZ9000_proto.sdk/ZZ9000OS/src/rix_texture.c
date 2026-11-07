/*
 * RasterIX external texture upload conversion for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_texture.h"

static uint16_t rix_texture_pixel(uint32_t rgba, enum RixTextureFormat format)
{
    uint8_t r = (uint8_t)(rgba >> 24);
    uint8_t g = (uint8_t)(rgba >> 16);
    uint8_t b = (uint8_t)(rgba >> 8);
    uint8_t a = (uint8_t)rgba;
    switch (format) {
    case RIX_TEXTURE_RGB565:
        return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                          ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
    case RIX_TEXTURE_RGBA4444:
        return (uint16_t)(((uint16_t)(r >> 4) << 12) |
                          ((uint16_t)(g >> 4) << 8) |
                          ((uint16_t)(b >> 4) << 4) | (uint16_t)(a >> 4));
    case RIX_TEXTURE_RGBA5551:
        return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                          ((uint16_t)(g >> 3) << 6) |
                          ((uint16_t)(b >> 3) << 1) | (uint16_t)(a >> 7));
    default:
        return 0U;
    }
}

int rix_texture_convert_rgba8_le(uint8_t *destination, size_t destination_size,
                                 const uint32_t *rgba, size_t texel_count,
                                 enum RixTextureFormat format)
{
    size_t i;
    if (destination == NULL || rgba == NULL || texel_count > destination_size / 2U ||
        format > RIX_TEXTURE_RGB565) {
        return 0;
    }
    for (i = 0U; i < texel_count; ++i) {
        uint16_t pixel = rix_texture_pixel(rgba[i], format);
        destination[i * 2U] = (uint8_t)pixel;
        destination[i * 2U + 1U] = (uint8_t)(pixel >> 8);
    }
    return 1;
}

uint32_t rix_texture_mip_offset(uint8_t width_log2, uint8_t height_log2,
                                uint8_t level)
{
    uint32_t width_mask;
    uint32_t height_mask;
    uint32_t layout_mask = 0U;
    uint32_t active_width;
    uint32_t active_height;
    uint32_t index;

    if (width_log2 > 8U || height_log2 > 8U ||
        level > (width_log2 > height_log2 ? width_log2 : height_log2))
        return UINT32_MAX;
    width_mask = (1U << width_log2) - 1U;
    height_mask = (1U << height_log2) - 1U;
    for (index = 0U; index < 8U; ++index) {
        layout_mask |= (((width_mask >> index) ^ (height_mask >> index)) & 1U) <<
                       (16U - 2U * index);
        layout_mask |= (((width_mask >> index) | (height_mask >> index)) & 1U) <<
                       (17U - 2U * index);
    }
    layout_mask >>= 18U - ((uint32_t)width_log2 + height_log2 + 1U);
    active_width = level < width_log2 ? (uint32_t)width_log2 - (uint32_t)level : 0U;
    active_height = level < height_log2 ? (uint32_t)height_log2 - (uint32_t)level : 0U;
    return layout_mask & ~((1U << (active_width + active_height + 1U)) - 1U);
}
