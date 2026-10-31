/*
 * ZZ9000 MiniGL Wire Protocol v9 Types and Protocol Constants
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_TYPES_H
#define MW_TYPES_H

#include <stdint.h>
#include <string.h>

#define MW_MAGIC                    0x414d3344UL /* 'AM3D' */
#define MW_VERSION                  9UL
#define MW_MAX_PACKET_BYTES         (256UL * 1024UL)
#define MW_MAX_COMMANDS             4096UL
#define MW_MAX_VERTICES             8192UL
#define MW_MAX_DIMENSION            4096UL
#define MW_VERTEX_WORDS             18UL
#define MW_VERTEX_BYTES             (MW_VERTEX_WORDS * 4UL)
#define MW_OBJECT_WORDS             19UL
#define MW_OBJECT_BYTES             (MW_OBJECT_WORDS * 4UL)
#define MW_PLUGIN_VERSION_BYTES     64UL

/* Command opcodes */
#define MW_CLEAR                    1UL
#define MW_UPLOAD                   3UL
#define MW_STATE                    4UL
#define MW_TEXTURE_DELETE           7UL
#define MW_TEXTURE_PARAM            8UL
#define MW_TEXTURE_COPY             9UL
#define MW_SYNC                     10UL
#define MW_TARGET                   11UL
#define MW_PRESENT                  12UL
#define MW_DRAW_OBJECT              13UL
#define MW_DRAW_PACKED              14UL
#define MW_DRAW_LOWERED             15UL
#define MW_TEXTURE_IMAGE_RAW        16UL
#define MW_TEXTURE_SUB_RAW          17UL
#define MW_DRAW_ARRAY_RAW           18UL
#define MW_COLOR_TABLE              20UL
/* Capability-gated direct-card presentation. Its payload is an MW_BLIT
 * request followed by the board-relative destination offset and span bytes,
 * so old v9 implementations reject it and the library keeps BRIDGE_BLIT. */
#define MW_PRESENT_SPAN             51UL
/* Custom raw texture format tokens */
#define MW_RAW_ARGB_4444            0x4d574001UL
#define MW_RAW_UBYTE_ARGB           0x4d574002UL
#define MW_RAW_UBYTE_BGRA           0x4d574003UL

/* Lowered draw flags */
#define MW_LOWERED_FLAT             1UL
#define MW_LOWERED_EDGE_MASK        8UL
#define MW_LOWERED_FRONT_SHIFT      4UL
#define MW_LOWERED_BACK_SHIFT       6UL

/* Packed draw bitfield shifts */
#define MW_PACK_POSITION            0UL
#define MW_PACK_COLOR               2UL
#define MW_PACK_TEX0                4UL
#define MW_PACK_TEX1                6UL
#define MW_PACK_NORMAL              8UL
#define MW_PACK_GROUP               9UL

/* State keys 1..50 */
#define MW_DEPTH_TEST               1UL
#define MW_DEPTH_FUNC               2UL
#define MW_DEPTH_MASK               3UL
#define MW_DEPTH_RANGE              4UL
#define MW_BLEND                    5UL
#define MW_BLEND_FACTORS            6UL
#define MW_BLEND_EQUATION           7UL
#define MW_ALPHA_TEST               8UL
#define MW_ALPHA_FUNC               9UL
#define MW_CULL                     10UL
#define MW_CULL_FACE                11UL
#define MW_FRONT_FACE               12UL
#define MW_SCISSOR                  13UL
#define MW_SCISSOR_BOX              14UL
#define MW_COLOR_MASK               15UL
#define MW_POLYGON_OFFSET           17UL
#define MW_OFFSET_FILL              18UL
#define MW_OFFSET_LINE              19UL
#define MW_OFFSET_POINT             20UL
#define MW_LINE_WIDTH               21UL
#define MW_POINT_SIZE               22UL
#define MW_SHADE_MODEL              23UL
#define MW_FOG                      24UL
#define MW_FOG_MODE                 25UL
#define MW_FOG_PARAMS               26UL
#define MW_FOG_COLOR                27UL
#define MW_TEXTURE_STAGE            28UL
#define MW_DRAW_BUFFER              29UL
#define MW_READ_BUFFER              30UL
#define MW_DITHER                   31UL
#define MW_POINT_SMOOTH             32UL
#define MW_VIEWPORT                 33UL
#define MW_Z_OFFSET                 34UL
#define MW_PERSPECTIVE              35UL
#define MW_MODELVIEW                36UL
#define MW_PROJECTION               37UL
/* Key 38 retired */
#define MW_TEXTURE_MATRIX0          39UL
#define MW_TEXTURE_MATRIX1          40UL
#define MW_TEXGEN0                  41UL
#define MW_TEXGEN1                  42UL
/* Key 43 retired */
#define MW_LIGHTING                 44UL
#define MW_LIGHT                    45UL
#define MW_LIGHT_MODEL              46UL
#define MW_MATERIAL                 47UL
#define MW_COLOR_MATERIAL           48UL
#define MW_NORMALIZE                49UL
#define MW_TEXTURE_ENV_COLOR        50UL

/* Synchronous requests */
#define MW_READ_PIXELS              1UL
#define MW_BLIT                     2UL
#define MW_CAPTURE                  3UL
#define MW_PLUGIN_VERSION           4UL
#define MW_BLIT_MAX_RECTS           256UL

/* Bridge return status codes */
#define MW_STATUS_OK                0x00000000U
#define MW_STATUS_UNAVAILABLE       0xffffffffU
#define MW_STATUS_INVALID           0xfffffffeU
#define MW_STATUS_CAPACITY          0xfffffffdU
#define MW_STATUS_CONTEXT_LOST      0xfffffffcU
#define MW_STATUS_FAILED            0xfffffffbU

/* Big-endian helpers */
static inline uint32_t mw_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           (uint32_t)p[3];
}

static inline void mw_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline float mw_bef(const uint8_t *p)
{
    union { uint32_t u; float f; } val;
    val.u = mw_be32(p);
    return val.f;
}

static inline void mw_put_bef(uint8_t *p, float f)
{
    union { uint32_t u; float f; } val;
    val.f = f;
    mw_put_be32(p, val.u);
}

static inline int mw_packed_layout(uint32_t format, uint32_t *constants, uint32_t *vertex_words)
{
    uint32_t position = (format >> MW_PACK_POSITION) & 3U;
    uint32_t color    = (format >> MW_PACK_COLOR) & 3U;
    uint32_t tex0     = (format >> MW_PACK_TEX0) & 3U;
    uint32_t tex1     = (format >> MW_PACK_TEX1) & 3U;
    uint32_t normal   = (format >> MW_PACK_NORMAL) & 1U;
    uint32_t group    = (format >> MW_PACK_GROUP) & 3U;

    if ((format >> 11) || position == 3U || color == 3U ||
        tex0 == 3U || tex1 == 3U || group == 3U)
        return 0;

    if (vertex_words)
        *vertex_words = position + 2U +
                        (color == 1U ? 1U : color == 2U ? 4U : 0U) +
                        tex0 * 2U + tex1 * 2U + normal * 3U;

    if (constants)
        *constants = (color ? 0U : 4U) +
                     (tex0 ? 0U : 4U) +
                     (tex1 ? 0U : 4U) +
                     (normal ? 0U : 3U);

    return 1;
}

#endif /* MW_TYPES_H */
