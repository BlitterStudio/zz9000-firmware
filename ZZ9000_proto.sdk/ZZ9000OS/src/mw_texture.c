/*
 * ZZ9000 MiniGL Wire Protocol v9 Texture Manager Implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mw_texture.h"
#include <stdlib.h>
#include <string.h>

static uint32_t mw_tex_raw_scalar_bytes(uint32_t type)
{
    return (type == 0x1400 || type == 0x1401) ? 1U :
           (type == 0x1402 || type == 0x1403) ? 2U : 4U;
}

static uint32_t mw_tex_raw_components(uint32_t format)
{
    switch (format) {
    case 0x1900:
    case 0x1903:
    case 0x1906:
    case 0x1909:
    case 0x8049:
        return 1U;
    case 0x190a:
        return 2U;
    case 0x1907: /* GL_RGB */
        return 3U;
    default:
        return 4U;
    }
}

static uint32_t mw_tex_raw_word(const uint8_t *p, uint32_t bytes, int swap)
{
    uint32_t value = 0;
    uint32_t i;
    for (i = 0; i < bytes; ++i)
        value = (value << 8) | p[swap ? (bytes - 1 - i) : i];
    return value;
}

static uint8_t mw_tex_raw_scalar(const uint8_t *p, uint32_t type, int swap)
{
    switch (type) {
    case 0x1401: /* GL_UNSIGNED_BYTE */
        return *p;
    case 0x1400: { /* GL_BYTE */
        int8_t v = (int8_t)*p;
        return v < 0 ? 0U : (uint8_t)(((uint32_t)v * 255U) / 127U);
    }
    case 0x1403: /* GL_UNSIGNED_SHORT */
        return (uint8_t)(mw_tex_raw_word(p, 2, swap) >> 8);
    case 0x1402: { /* GL_SHORT */
        int16_t v = (int16_t)mw_tex_raw_word(p, 2, swap);
        return v < 0 ? 0U : (uint8_t)(((uint32_t)v * 255U) / 32767U);
    }
    case 0x1405: /* GL_UNSIGNED_INT */
        return (uint8_t)(mw_tex_raw_word(p, 4, swap) >> 24);
    case 0x1404: { /* GL_INT */
        int32_t v = (int32_t)mw_tex_raw_word(p, 4, swap);
        return v < 0 ? 0U : (uint8_t)((uint32_t)v / 8421504U);
    }
    case 0x1406: { /* GL_FLOAT */
        uint32_t bits = mw_tex_raw_word(p, 4, swap);
        float val;
        memcpy(&val, &bits, sizeof(val));
        if (val <= 0.0f) return 0U;
        return val >= 1.0f ? 255U : (uint8_t)(val * 255.0f + 0.5f);
    }
    default:
        return 0U;
    }
}

static uint32_t mw_tex_raw_pixel(const uint8_t *p, uint32_t format, uint32_t type, int swap)
{
    uint8_t r, g, b, a;

    if (format == MW_RAW_ARGB_4444) {
        uint32_t val = mw_tex_raw_word(p, 2, swap);
        r = (uint8_t)(((val >> 8) & 15U) * 17U);
        g = (uint8_t)(((val >> 4) & 15U) * 17U);
        b = (uint8_t)((val & 15U) * 17U);
        a = (uint8_t)(((val >> 12) & 15U) * 17U);
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    }

    if (type == 0x8363) { /* GL_UNSIGNED_SHORT_5_6_5 */
        uint32_t val = mw_tex_raw_word(p, 2, swap);
        r = (uint8_t)((((val >> 11) & 31U) * 255U) / 31U);
        g = (uint8_t)((((val >> 5) & 63U) * 255U) / 63U);
        b = (uint8_t)(((val & 31U) * 255U) / 31U);
        a = 255U;
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    }

    if (type == 0x8033) { /* GL_UNSIGNED_SHORT_4_4_4_4 */
        uint32_t val = mw_tex_raw_word(p, 2, swap);
        r = (uint8_t)(((val >> 12) & 15U) * 17U);
        g = (uint8_t)(((val >> 8) & 15U) * 17U);
        b = (uint8_t)(((val >> 4) & 15U) * 17U);
        a = (uint8_t)((val & 15U) * 17U);
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    }

    if (type == 0x8034) { /* GL_UNSIGNED_SHORT_5_5_5_1 */
        uint32_t val = mw_tex_raw_word(p, 2, swap);
        r = (uint8_t)((((val >> 11) & 31U) * 255U) / 31U);
        g = (uint8_t)((((val >> 6) & 31U) * 255U) / 31U);
        b = (uint8_t)((((val >> 1) & 31U) * 255U) / 31U);
        a = (val & 1U) ? 255U : 0U;
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    }

    {
        uint8_t q[4] = {0, 0, 0, 255};
        uint32_t comps = mw_tex_raw_components(format);
        uint32_t stride = mw_tex_raw_scalar_bytes(type);
        uint32_t i;
        for (i = 0; i < comps; ++i)
            q[i] = mw_tex_raw_scalar(p + i * stride, type, swap);

        switch (format) {
        case 0x1906: /* GL_ALPHA: (255, 255, 255, A) */
            r = 255U; g = 255U; b = 255U; a = q[0];
            break;
        case 0x1909: /* GL_LUMINANCE: (L, L, L, 255) */
            r = q[0]; g = q[0]; b = q[0]; a = 255U;
            break;
        case 0x190a: /* GL_LUMINANCE_ALPHA: (L, L, L, A) */
            r = q[0]; g = q[0]; b = q[0]; a = q[1];
            break;
        case 0x8049: /* GL_INTENSITY: (I, I, I, I) */
            r = q[0]; g = q[0]; b = q[0]; a = q[0];
            break;
        case 0x80e1: /* GL_BGRA */
            r = q[2]; g = q[1]; b = q[0]; a = q[3];
            break;
        case MW_RAW_UBYTE_ARGB:
            r = q[1]; g = q[2]; b = q[3]; a = q[0];
            break;
        case 0x1903: /* GL_RED */
            r = q[0]; g = 0U; b = 0U; a = 255U;
            break;
        default: /* GL_RGB / GL_RGBA */
            r = q[0]; g = q[1]; b = q[2]; a = q[3];
            break;
        }
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    }
}

void mw_texmgr_init(mw_texture_manager *tm)
{
    tm->capacity = 16;
    tm->count = 0;
    tm->textures = (mw_texture *)calloc(tm->capacity, sizeof(mw_texture));
}

void mw_texmgr_destroy(mw_texture_manager *tm)
{
    uint32_t i, lvl;
    if (!tm || !tm->textures)
        return;

    for (i = 0; i < tm->capacity; ++i) {
        if (tm->textures[i].valid) {
            for (lvl = 0; lvl < MW_MAX_MIP_LEVELS; ++lvl) {
                if (tm->textures[i].pixels[lvl]) {
                    free(tm->textures[i].pixels[lvl]);
                    tm->textures[i].pixels[lvl] = NULL;
                }
                if (tm->textures[i].indices[lvl]) {
                    free(tm->textures[i].indices[lvl]);
                    tm->textures[i].indices[lvl] = NULL;
                }
            }
        }
    }
    free(tm->textures);
    tm->textures = NULL;
    tm->capacity = 0;
    tm->count = 0;
}

mw_texture *mw_texmgr_find(mw_texture_manager *tm, uint32_t id)
{
    uint32_t i;
    if (!tm || !tm->textures)
        return NULL;
    for (i = 0; i < tm->capacity; ++i) {
        if (tm->textures[i].valid && tm->textures[i].id == id)
            return &tm->textures[i];
    }
    return NULL;
}

mw_texture *mw_texmgr_get_or_create(mw_texture_manager *tm, uint32_t id)
{
    mw_texture *t;
    uint32_t i;

    t = mw_texmgr_find(tm, id);
    if (t)
        return t;

    /* Find free slot */
    for (i = 0; i < tm->capacity; ++i) {
        if (!tm->textures[i].valid) {
            t = &tm->textures[i];
            break;
        }
    }

    if (!t) {
        /* Grow capacity */
        uint32_t new_cap = tm->capacity * 2;
        mw_texture *new_textures = (mw_texture *)realloc(tm->textures, new_cap * sizeof(mw_texture));
        if (!new_textures)
            return NULL;
        memset(new_textures + tm->capacity, 0, (new_cap - tm->capacity) * sizeof(mw_texture));
        tm->textures = new_textures;
        t = &tm->textures[tm->capacity];
        tm->capacity = new_cap;
    }

    memset(t, 0, sizeof(*t));
    t->id = id;
    t->valid = 1;
    t->min_filter = 0x2701; /* GL_LINEAR_MIPMAP_NEAREST */
    t->mag_filter = 0x2601; /* GL_LINEAR */
    t->wrap_s = 0x2901;     /* GL_REPEAT */
    t->wrap_t = 0x2901;     /* GL_REPEAT */
    tm->count++;
    return t;
}

void mw_texmgr_delete(mw_texture_manager *tm, uint32_t id)
{
    mw_texture *t = mw_texmgr_find(tm, id);
    uint32_t lvl;
    if (!t)
        return;

    for (lvl = 0; lvl < MW_MAX_MIP_LEVELS; ++lvl) {
        if (t->pixels[lvl]) {
            free(t->pixels[lvl]);
            t->pixels[lvl] = NULL;
        }
        if (t->indices[lvl]) {
            free(t->indices[lvl]);
            t->indices[lvl] = NULL;
        }
    }
    memset(t, 0, sizeof(*t));
    tm->count--;
}

void mw_texmgr_generate_mips(mw_texture *t)
{
    uint32_t lvl;
    if (!t || !t->pixels[0] || !t->widths[0] || !t->heights[0])
        return;

    for (lvl = 1; lvl < MW_MAX_MIP_LEVELS; ++lvl) {
        uint32_t pw = t->widths[lvl - 1];
        uint32_t ph = t->heights[lvl - 1];
        uint32_t w, h;
        uint32_t *src = t->pixels[lvl - 1];
        uint32_t *dst;
        uint32_t y, x, rem;

        if (!src || (pw <= 1 && ph <= 1)) {
            for (rem = lvl; rem < MW_MAX_MIP_LEVELS; ++rem) {
                if (t->pixels[rem]) {
                    free(t->pixels[rem]);
                    t->pixels[rem] = NULL;
                }
                if (t->indices[rem]) {
                    free(t->indices[rem]);
                    t->indices[rem] = NULL;
                }
                t->widths[rem] = 0;
                t->heights[rem] = 0;
            }
            break;
        }

        w = pw > 1 ? pw / 2 : 1;
        h = ph > 1 ? ph / 2 : 1;

        if (t->widths[lvl] != w || t->heights[lvl] != h || !t->pixels[lvl]) {
            if (t->pixels[lvl]) free(t->pixels[lvl]);
            t->pixels[lvl] = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
            if (!t->pixels[lvl]) return;
            t->widths[lvl] = w;
            t->heights[lvl] = h;
        }
        dst = t->pixels[lvl];
        t->internal_rgb[lvl] = t->internal_rgb[0];

        /* 2x2 box downsampling */
        for (y = 0; y < h; ++y) {
            uint32_t sy0 = y * 2;
            uint32_t sy1 = (sy0 + 1 < ph) ? sy0 + 1 : sy0;
            for (x = 0; x < w; ++x) {
                uint32_t sx0 = x * 2;
                uint32_t sx1 = (sx0 + 1 < pw) ? sx0 + 1 : sx0;

                uint32_t p00 = src[sy0 * pw + sx0];
                uint32_t p10 = src[sy0 * pw + sx1];
                uint32_t p01 = src[sy1 * pw + sx0];
                uint32_t p11 = src[sy1 * pw + sx1];

                uint32_t r = (((p00 >> 24) & 0xff) + ((p10 >> 24) & 0xff) +
                              ((p01 >> 24) & 0xff) + ((p11 >> 24) & 0xff) + 2) / 4;
                uint32_t g = (((p00 >> 16) & 0xff) + ((p10 >> 16) & 0xff) +
                              ((p01 >> 16) & 0xff) + ((p11 >> 16) & 0xff) + 2) / 4;
                uint32_t b = (((p00 >> 8) & 0xff) + ((p10 >> 8) & 0xff) +
                              ((p01 >> 8) & 0xff) + ((p11 >> 8) & 0xff) + 2) / 4;
                uint32_t a = ((p00 & 0xff) + (p10 & 0xff) +
                              (p01 & 0xff) + (p11 & 0xff) + 2) / 4;

                dst[y * w + x] = (r << 24) | (g << 16) | (b << 8) | a;
            }
        }
        t->max_level = (uint8_t)lvl;
    }
    t->stale = 0;
}

int mw_texmgr_image_raw(mw_texture_manager *tm, const uint8_t *payload, size_t bytes,
                        const uint8_t palette[256][4], uint32_t palette_width)
{
    uint32_t name, level, internal_format, width, height, format, type;
    uint32_t alignment, row_length, skip_rows, skip_pixels, swap, source;
    uint32_t pixel, source_width, row;
    const uint8_t *src;
    mw_texture *t;
    uint32_t *dst;
    uint32_t y, x;

    if (bytes < 56)
        return 0;

    name = mw_be32(payload);
    level = mw_be32(payload + 4);
    internal_format = mw_be32(payload + 8);
    width = mw_be32(payload + 12);
    height = mw_be32(payload + 16);
    format = mw_be32(payload + 20);
    type = mw_be32(payload + 24);
    alignment = mw_be32(payload + 28);
    row_length = mw_be32(payload + 32);
    skip_rows = mw_be32(payload + 36);
    skip_pixels = mw_be32(payload + 40);
    swap = mw_be32(payload + 44);
    source = mw_be32(payload + 52);

    if (level >= MW_MAX_MIP_LEVELS)
        return 0;

    t = mw_texmgr_get_or_create(tm, name);
    if (!t)
        return 0;

    if (t->widths[level] != width || t->heights[level] != height || !t->pixels[level]) {
        if (t->pixels[level]) free(t->pixels[level]);
        t->pixels[level] = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
        if (!t->pixels[level]) return 0;
        t->widths[level] = width;
        t->heights[level] = height;
    }
    t->base_format[level] = format;
    t->internal_rgb[level] = (internal_format == 3 || internal_format == 0x1907 ||
                              internal_format == 0x8050 || internal_format == 0x8051);
    dst = t->pixels[level];
    if (format == 0x1900) {
        t->indexed = 1;
        t->internal_rgb[level] = 0;
        if (!t->indices[level] || t->widths[level] != width || t->heights[level] != height) {
            if (t->indices[level]) free(t->indices[level]);
            t->indices[level] = (uint8_t *)malloc((size_t)width * height);
            if (!t->indices[level]) return 0;
        }
        if (source == 0) {
            memset(t->indices[level], 0, (size_t)width * height);
            memset(dst, 0, (size_t)width * height * sizeof(uint32_t));
            return 1;
        }
        source_width = row_length ? row_length : width;
        row = ((source_width + alignment - 1) / alignment) * alignment;
        src = payload + 56 + (size_t)skip_rows * row + (size_t)skip_pixels;
        for (y = 0; y < height; ++y) {
            const uint8_t *src_row = src + (size_t)y * row;
            uint32_t *dst_row = dst + (size_t)y * width;
            uint8_t *idx_row = t->indices[level] + (size_t)y * width;
            for (x = 0; x < width; ++x) {
                uint8_t idx = src_row[x];
                idx_row[x] = idx;
                if (palette && idx < palette_width) {
                    dst_row[x] = ((uint32_t)palette[idx][0] << 24) |
                                 ((uint32_t)palette[idx][1] << 16) |
                                 ((uint32_t)palette[idx][2] << 8)  |
                                 (uint32_t)palette[idx][3];
                } else {
                    dst_row[x] = 0x000000ffU;
                }
            }
        }
    } else {
        t->indexed = 0;
        if (t->indices[level]) {
            free(t->indices[level]);
            t->indices[level] = NULL;
        }
        pixel = (type == 0x8363 || type == 0x8033 || format == MW_RAW_ARGB_4444) ? 2U :
                mw_tex_raw_components(format) * mw_tex_raw_scalar_bytes(type);
        source_width = row_length ? row_length : width;
        row = ((source_width * pixel + alignment - 1) / alignment) * alignment;
        src = payload + 56 + (size_t)skip_rows * row + (size_t)skip_pixels * pixel;

        for (y = 0; y < height; ++y) {
            const uint8_t *src_row = src + (size_t)y * row;
            uint32_t *dst_row = dst + (size_t)y * width;
            for (x = 0; x < width; ++x)
                dst_row[x] = mw_tex_raw_pixel(src_row + (size_t)x * pixel, format, type, swap);
        }
    }

    if (level == 0 && t->generate_mipmap)
        mw_texmgr_generate_mips(t);

    return 1;
}

int mw_texmgr_sub_raw(mw_texture_manager *tm, const uint8_t *payload, size_t bytes,
                      const uint8_t palette[256][4], uint32_t palette_width)
{
    uint32_t name, level, xoffset, yoffset, width, height, format, type;
    uint32_t alignment, row_length, skip_rows, skip_pixels, swap, source;
    uint32_t pixel, source_width, row;
    const uint8_t *src;
    mw_texture *t;
    uint32_t *dst;
    uint32_t y, x;

    if (bytes < 60)
        return 0;

    name = mw_be32(payload);
    level = mw_be32(payload + 4);
    xoffset = mw_be32(payload + 8);
    yoffset = mw_be32(payload + 12);
    width = mw_be32(payload + 16);
    height = mw_be32(payload + 20);
    format = mw_be32(payload + 24);
    type = mw_be32(payload + 28);
    alignment = mw_be32(payload + 32);
    row_length = mw_be32(payload + 36);
    skip_rows = mw_be32(payload + 40);
    skip_pixels = mw_be32(payload + 44);
    swap = mw_be32(payload + 48);
    source = mw_be32(payload + 56);
    (void)source;
    if (level >= MW_MAX_MIP_LEVELS)
        return 0;

    t = mw_texmgr_find(tm, name);
    if (!t || !t->pixels[level])
        return 0;

    if (xoffset + width > t->widths[level] || yoffset + height > t->heights[level])
        return 0;

    dst = t->pixels[level];
    if (t->indexed && format == 0x1900) {
        source_width = row_length ? row_length : width;
        row = ((source_width + alignment - 1) / alignment) * alignment;
        src = payload + 60 + (size_t)skip_rows * row + (size_t)skip_pixels;
        for (y = 0; y < height; ++y) {
            const uint8_t *src_row = src + (size_t)y * row;
            uint32_t *dst_row = dst + (size_t)(yoffset + y) * t->widths[level] + xoffset;
            uint8_t *idx_row = t->indices[level] + (size_t)(yoffset + y) * t->widths[level] + xoffset;
            for (x = 0; x < width; ++x) {
                uint8_t idx = src_row[x];
                idx_row[x] = idx;
                if (palette && idx < palette_width) {
                    dst_row[x] = ((uint32_t)palette[idx][0] << 24) |
                                 ((uint32_t)palette[idx][1] << 16) |
                                 ((uint32_t)palette[idx][2] << 8)  |
                                 (uint32_t)palette[idx][3];
                } else {
                    dst_row[x] = 0x000000ffU;
                }
            }
        }
    } else {
        pixel = (type == 0x8363 || type == 0x8033 || format == MW_RAW_ARGB_4444) ? 2U :
                mw_tex_raw_components(format) * mw_tex_raw_scalar_bytes(type);
        source_width = row_length ? row_length : width;
        row = ((source_width * pixel + alignment - 1) / alignment) * alignment;
        src = payload + 60 + (size_t)skip_rows * row + (size_t)skip_pixels * pixel;

        for (y = 0; y < height; ++y) {
            const uint8_t *src_row = src + (size_t)y * row;
            uint32_t *dst_row = dst + (size_t)(yoffset + y) * t->widths[level] + xoffset;
            for (x = 0; x < width; ++x)
                dst_row[x] = mw_tex_raw_pixel(src_row + (size_t)x * pixel, format, type, swap);
        }
    }
    if (level == 0 && t->generate_mipmap)
        mw_texmgr_generate_mips(t);

    return 1;
}

int mw_texmgr_param(mw_texture_manager *tm, uint32_t id, uint32_t pname, uint32_t value, float fvalue)
{
    mw_texture *t = mw_texmgr_get_or_create(tm, id);
    if (!t)
        return 0;

    (void)fvalue;
    switch (pname) {
    case 0x2800: /* GL_TEXTURE_MAG_FILTER */
        t->mag_filter = (uint16_t)value;
        break;
    case 0x2801: /* GL_TEXTURE_MIN_FILTER */
        t->min_filter = (uint16_t)value;
        break;
    case 0x2802: /* GL_TEXTURE_WRAP_S */
        t->wrap_s = (uint16_t)value;
        break;
    case 0x2803: /* GL_TEXTURE_WRAP_T */
        t->wrap_t = (uint16_t)value;
        break;
    case 0x8191: /* GL_GENERATE_MIPMAP */
        t->generate_mipmap = (value != 0);
        if (t->generate_mipmap && t->pixels[0])
            mw_texmgr_generate_mips(t);
        break;
    default:
        break;
    }
    return 1;
}
