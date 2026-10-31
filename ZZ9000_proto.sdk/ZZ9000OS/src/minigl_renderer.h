/*
 * Shared MiniGL renderer model used internally by the Wire-v9 service.
 *
 * This is deliberately firmware-private: it describes renderer state and
 * presentation arguments, not a host-visible command ABI.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_RENDERER_H
#define MINIGL_RENDERER_H

#include <stdint.h>

#define ZZ9K_MINIGL_MAX_WIDTH 1024U
#define ZZ9K_MINIGL_MAX_HEIGHT 768U
#define MINIGL_RASTER_STAGING_SLOT_BYTES (2U * 1024U * 1024U)
#define ZZ9K_MINIGL_MAX_TEXTURES 1024U
#define ZZ9K_MINIGL_MAX_MIP_LEVELS 16U


/* Renderer-internal normalized vertex record, independent of any guest ABI. */
#define MINIGL_RASTER_VERTEX_STRIDE 64U
#define MINIGL_RASTER_ATTR_POSITION_INT32 (1U << 0)
#define ZZ9K_MGL_TEX_INTERNAL_RGB (1U << 15)
#define ZZ9K_MGL_TEX_STORAGE_MASK 0x7fffU

#define ZZ9K_MGL_STATE_DEPTH_TEST (1U << 0)
#define ZZ9K_MGL_STATE_ALPHA_TEST (1U << 1)
#define ZZ9K_MGL_STATE_BLEND (1U << 2)
#define ZZ9K_MGL_STATE_CULL_FACE (1U << 3)
#define ZZ9K_MGL_STATE_FOG (1U << 4)
#define ZZ9K_MGL_STATE_TEXTURE_2D_0 (1U << 5)
#define ZZ9K_MGL_STATE_TEXTURE_2D_1 (1U << 6)
#define ZZ9K_MGL_STATE_DEPTH_WRITE (1U << 7)
#define ZZ9K_MGL_STATE_SCISSOR_TEST (1U << 8)
#define ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL (1U << 9)
#define ZZ9K_MGL_STATE_POLYGON_OFFSET_LINE (1U << 10)
#define ZZ9K_MGL_STATE_POLYGON_OFFSET_POINT (1U << 11)
#define ZZ9K_MGL_STATE_FLAG_FORCE_CPU (1U << 0)
#define ZZ9K_MGL_STATE_FLAGS_MASK ZZ9K_MGL_STATE_FLAG_FORCE_CPU

enum {
    ZZ9K_MGL_FALLBACK_NONE = 0,
    ZZ9K_MGL_FALLBACK_DEPTH_BITS = 1,
    ZZ9K_MGL_FALLBACK_FOG = 2,
    ZZ9K_MGL_FALLBACK_MULTITEXTURE = 3,
    ZZ9K_MGL_FALLBACK_POLYGON_OFFSET = 4,
    ZZ9K_MGL_FALLBACK_POLYGON_MODE = 5,
    ZZ9K_MGL_FALLBACK_BLEND_EQUATION = 6,
    ZZ9K_MGL_FALLBACK_TEXTURE_FILTER = 7,
    ZZ9K_MGL_FALLBACK_RESIDENCY_EXHAUSTED = 8,
    ZZ9K_MGL_FALLBACK_BACKEND_DISABLED_OR_ABSENT = 9,
    ZZ9K_MGL_FALLBACK_FORCED_CPU = 10,
    ZZ9K_MGL_FALLBACK_DRAW_REJECTED = 11,
    ZZ9K_MGL_FALLBACK_MAX = 12
};

#define ZZ9K_MGL_WIRE_TEXTURE_2D 0x0DE1U
#define ZZ9K_MGL_WIRE_NEVER 0x0200U
#define ZZ9K_MGL_WIRE_LESS 0x0201U
#define ZZ9K_MGL_WIRE_EQUAL 0x0202U
#define ZZ9K_MGL_WIRE_LEQUAL 0x0203U
#define ZZ9K_MGL_WIRE_GREATER 0x0204U
#define ZZ9K_MGL_WIRE_NOTEQUAL 0x0205U
#define ZZ9K_MGL_WIRE_GEQUAL 0x0206U
#define ZZ9K_MGL_WIRE_ALWAYS 0x0207U
#define ZZ9K_MGL_WIRE_ZERO 0U
#define ZZ9K_MGL_WIRE_ONE 1U
#define ZZ9K_MGL_WIRE_SRC_COLOR 0x0300U
#define ZZ9K_MGL_WIRE_ONE_MINUS_SRC_COLOR 0x0301U
#define ZZ9K_MGL_WIRE_SRC_ALPHA 0x0302U
#define ZZ9K_MGL_WIRE_ONE_MINUS_SRC_ALPHA 0x0303U
#define ZZ9K_MGL_WIRE_DST_ALPHA 0x0304U
#define ZZ9K_MGL_WIRE_ONE_MINUS_DST_ALPHA 0x0305U
#define ZZ9K_MGL_WIRE_DST_COLOR 0x0306U
#define ZZ9K_MGL_WIRE_ONE_MINUS_DST_COLOR 0x0307U
#define ZZ9K_MGL_WIRE_SRC_ALPHA_SATURATE 0x0308U
#define ZZ9K_MGL_WIRE_FRONT 0x0404U
#define ZZ9K_MGL_WIRE_BACK 0x0405U
#define ZZ9K_MGL_WIRE_FRONT_AND_BACK 0x0408U
#define ZZ9K_MGL_WIRE_CW 0x0900U
#define ZZ9K_MGL_WIRE_CCW 0x0901U
#define ZZ9K_MGL_WIRE_FLAT 0x1D00U
#define ZZ9K_MGL_WIRE_SMOOTH 0x1D01U
#define ZZ9K_MGL_WIRE_POINT 0x1B00U
#define ZZ9K_MGL_WIRE_LINE 0x1B01U
#define ZZ9K_MGL_WIRE_FILL 0x1B02U
#define ZZ9K_MGL_WIRE_EXP 0x0800U
#define ZZ9K_MGL_WIRE_EXP2 0x0801U
#define ZZ9K_MGL_WIRE_LINEAR 0x2601U
#define ZZ9K_MGL_WIRE_NEAREST 0x2600U
#define ZZ9K_MGL_WIRE_NEAREST_MIPMAP_NEAREST 0x2700U
#define ZZ9K_MGL_WIRE_LINEAR_MIPMAP_NEAREST 0x2701U
#define ZZ9K_MGL_WIRE_NEAREST_MIPMAP_LINEAR 0x2702U
#define ZZ9K_MGL_WIRE_LINEAR_MIPMAP_LINEAR 0x2703U
#define ZZ9K_MGL_WIRE_REPEAT 0x2901U
#define ZZ9K_MGL_WIRE_CLAMP 0x2900U
#define ZZ9K_MGL_WIRE_CLAMP_TO_EDGE 0x812FU
#define ZZ9K_MGL_WIRE_MODULATE 0x2100U
#define ZZ9K_MGL_WIRE_DECAL 0x2101U
#define ZZ9K_MGL_WIRE_REPLACE 0x1E01U
#define ZZ9K_MGL_WIRE_ADD 0x0104U
#define ZZ9K_MGL_WIRE_BLEND 0x0BE2U
#define ZZ9K_MGL_WIRE_FUNC_ADD 0x8006U
#define ZZ9K_MGL_WIRE_MIN 0x8007U
#define ZZ9K_MGL_WIRE_MAX 0x8008U
#define ZZ9K_MGL_WIRE_FUNC_SUBTRACT 0x800AU
#define ZZ9K_MGL_WIRE_FUNC_REVERSE_SUBTRACT 0x800BU

typedef struct ZZ9KMiniGLPacketSetState {
    uint32_t enabled_flags;
    uint16_t blend_src, blend_dst, alpha_func, alpha_ref, depth_func, depth_bits;
    uint32_t depth_near, depth_far;
    uint16_t cull_face_mode, front_face, shade_model, polygon_mode, point_size, line_width;
    int16_t viewport_x, viewport_y;
    uint16_t viewport_w, viewport_h;
    int16_t scissor_x, scissor_y;
    uint16_t scissor_w, scissor_h;
    uint32_t color_mask, fog_color, fog_mode;
    float fog_start, fog_end, fog_density;
    uint16_t tex_filter, tex_wrap;
    uint16_t tex_env_mode[2];
    uint32_t tex_env_color[2];
    float min_triangle_area, depth_offset;
    uint16_t blend_src_alpha, blend_dst_alpha, blend_equation, reserved;
    float polygon_offset_factor, polygon_offset_units;
} ZZ9KMiniGLPacketSetState;

#define ZZ9K_MGL_TEXFMT_RGBA8888 0U
#define ZZ9K_MGL_TEXFMT_RGB565 1U
#define ZZ9K_MGL_TEXFMT_ARGB1555 2U
#define ZZ9K_MGL_TEXFMT_RGB888 3U
#define ZZ9K_MGL_TEXFMT_LUM8 4U
#define ZZ9K_MGL_TEXFMT_LUM_A8 5U
#define ZZ9K_MGL_TEXFMT_P8 6U

#define ZZ9K_MGL_LIGHTING_ENABLED (1U << 0)
#define ZZ9K_MGL_LIGHTING_NORMALIZE (1U << 1)
#define ZZ9K_MGL_LIGHTING_COLOR_MATERIAL (1U << 2)
#define ZZ9K_MGL_LIGHTING_TWO_SIDE (1U << 3)
#define ZZ9K_MGL_LIGHTING_LOCAL_VIEWER (1U << 4)
#define ZZ9K_MGL_LIGHTING_RESCALE_NORMAL (1U << 5)
typedef struct ZZ9KMiniGLMaterial {
    float ambient[4], diffuse[4], specular[4], emission[4], shininess;
} ZZ9KMiniGLMaterial;
typedef struct ZZ9KMiniGLLight {
    uint32_t enabled;
    float position[4], ambient[4], diffuse[4], specular[4];
    float spot_direction[3], spot_exponent, spot_cutoff;
    float attenuation[3];
} ZZ9KMiniGLLight;
typedef struct ZZ9KMiniGLPacketLighting {
    uint32_t flags;
    uint16_t color_material_face, color_material_mode;
    float global_ambient[4];
    ZZ9KMiniGLMaterial material[2];
    ZZ9KMiniGLLight light[8];
} ZZ9KMiniGLPacketLighting;

typedef struct ZZ9KMiniGLPacketPresent {
    uint32_t target_width, target_height, window_w, window_h, window_x, window_y;
    uint32_t reserved, fence_id;
    uint32_t dest_offset, dest_pitch, dest_format, source_x, source_y;
} ZZ9KMiniGLPacketPresent;
#define ZZ9K_MGL_PIXFMT_RGBA32 0U
#define ZZ9K_MGL_PIXFMT_BGRA32 1U
#define ZZ9K_MGL_PIXFMT_ARGB32 2U
#define ZZ9K_MGL_PIXFMT_RGB565_BE 3U
#define ZZ9K_MGL_PIXFMT_RGB565_LE 4U
#define ZZ9K_MGL_PIXFMT_RGB24 5U
#define ZZ9K_MGL_PIXFMT_BGR24 6U
#define ZZ9K_MGL_PIXFMT_ABGR32 7U

#endif /* MINIGL_RENDERER_H */
