/*
 * ZZ9000 MiniGL Wire Protocol v9 Texture Manager
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_TEXTURE_H
#define MW_TEXTURE_H

#include <stdint.h>
#include <stddef.h>
#include "mw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MW_MAX_MIP_LEVELS 16U

typedef struct mw_texture {
    uint32_t id;
    uint16_t min_filter;
    uint16_t mag_filter;
    uint16_t wrap_s;
    uint16_t wrap_t;
    uint8_t  generate_mipmap;
    uint8_t  stale;
    uint8_t  valid;
    uint8_t  max_level;

    uint32_t widths[MW_MAX_MIP_LEVELS];
    uint32_t heights[MW_MAX_MIP_LEVELS];
    uint32_t *pixels[MW_MAX_MIP_LEVELS];
    uint8_t  internal_rgb[MW_MAX_MIP_LEVELS];
    uint32_t base_format[MW_MAX_MIP_LEVELS];
    uint8_t  indexed;
    uint8_t  *indices[MW_MAX_MIP_LEVELS];
    uint32_t pixel_addresses[MW_MAX_MIP_LEVELS];
    uint32_t index_addresses[MW_MAX_MIP_LEVELS];
} mw_texture;

typedef struct mw_texture_manager {
    mw_texture *textures;
    uint32_t capacity;
    uint32_t count;
    uint32_t card_pool_owner;
    uint32_t textures_address;
    uint8_t *arena;
    uint32_t arena_address;
    uint32_t arena_bytes;
    uint32_t arena_free;
} mw_texture_manager;

void mw_texmgr_init(mw_texture_manager *tm);
void mw_texmgr_init_owned(mw_texture_manager *tm, uint32_t card_pool_owner);
void mw_texmgr_destroy(mw_texture_manager *tm);

mw_texture *mw_texmgr_get_or_create(mw_texture_manager *tm, uint32_t id);
mw_texture *mw_texmgr_find(mw_texture_manager *tm, uint32_t id);
void mw_texmgr_delete(mw_texture_manager *tm, uint32_t id);

int mw_texmgr_allocate_level(mw_texture_manager *tm, mw_texture *t,
                             uint32_t level, uint32_t width, uint32_t height);
int mw_texmgr_image_raw(mw_texture_manager *tm, const uint8_t *payload, size_t bytes,
                        const uint8_t palette[256][4], uint32_t palette_width);
int mw_texmgr_sub_raw(mw_texture_manager *tm, const uint8_t *payload, size_t bytes,
                      const uint8_t palette[256][4], uint32_t palette_width);
int mw_texmgr_param(mw_texture_manager *tm, uint32_t id, uint32_t pname, uint32_t value, float fvalue);
int mw_texmgr_generate_mips(mw_texture_manager *tm, mw_texture *t);

#ifdef __cplusplus
}
#endif

#endif /* MW_TEXTURE_H */
