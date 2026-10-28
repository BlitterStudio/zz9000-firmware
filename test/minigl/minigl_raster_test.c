/* Host-executable MiniGL raster behavioral test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_raster.h"
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/mw_types.h"

#define W 64U
#define H 48U
#define VSTRIDE MINIGL_RASTER_VERTEX_STRIDE
#define GL_POINTS 0x0000U
#define GL_LINES 0x0001U
#define GL_TRIANGLES 0x0004U
#define GL_QUADS 0x0007U
#define GL_TEXTURE_2D 0x0DE1U
#define GL_LESS 0x0201U
#define GL_LEQUAL 0x0203U
#define GL_ALWAYS 0x0207U
#define GL_GREATER 0x0204U
#define GL_NEVER 0x0200U
#define GL_LINEAR 0x2601U
#define GL_NEAREST 0x2600U
#define GL_MODULATE 0x2100U
#define GL_REPLACE 0x1e01U
#define GL_LINE 0x1B01U
#define GL_FILL 0x1B02U
#define GL_REPEAT 0x2901U
#define GL_FLAT 0x1d00U
#define GL_FOG_LINEAR 0x2601U
#define GL_SRC_ALPHA 0x0302U
#define GL_ONE_MINUS_SRC_ALPHA 0x0303U
#define GL_ZERO 0U
#define GL_ONE 1U
#define GL_SRC_ALPHA_SATURATE 0x0308U
#define GL_FUNC_ADD 0x8006U
#define GL_MIN 0x8007U
#define GL_MAX 0x8008U
#define GL_FUNC_SUBTRACT 0x800aU
#define GL_FUNC_REVERSE_SUBTRACT 0x800bU

int minigl_present_frame(const ZZ9KMiniGLPacketPresent *present, const uint32_t *pixels)
{ (void)present; (void)pixels; return 1; }
int minigl_present_frame_rgb565(const ZZ9KMiniGLPacketPresent *present, const uint16_t *pixels)
{ (void)present; (void)pixels; return 1; }

static uint32_t color[W * H];
static uint8_t depth[W * H * 3U];
static uint8_t vertices[400U * VSTRIDE];
static uint8_t staging[2048];
static uint8_t textures[8192];
static uint8_t fragmented_pool[32];
static uint8_t grown_pool[64];
static uint32_t grow_minimum;
static uint32_t grow_live_bytes;

static int grow_fragmented_pool(void *context, uint32_t minimum_bytes,
                                uint32_t live_bytes, uint8_t **pool,
                                uint32_t *pool_bytes)
{
    (void)context;
    grow_minimum = minimum_bytes;
    grow_live_bytes = live_bytes;
    memcpy(grown_pool, *pool, live_bytes);
    *pool = grown_pool;
    *pool_bytes = sizeof(grown_pool);
    return 1;
}

static void put16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; }
static void put32(uint8_t *p, uint32_t n) { p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16); p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n; }
static void putfloat(uint8_t *p, float f) { union { float f; uint32_t u; } n; n.f = f; put32(p, n.u); }
static void vertex(unsigned n, float x, float y, float z, uint32_t rgba, float u, float v)
{
    uint8_t *p = vertices + n * VSTRIDE;
    memset(p, 0, VSTRIDE);
    putfloat(p, x); putfloat(p + 4, y); putfloat(p + 8, z); put32(p + 12, rgba);
    putfloat(p + 16, u); putfloat(p + 20, v); putfloat(p + 24, u); putfloat(p + 28, v);
    putfloat(p + 44, 1.0f); putfloat(p + 48, 1.0f); putfloat(p + 52, 1.0f);
}
static ZZ9KMiniGLPacketSetState state(void)
{
    ZZ9KMiniGLPacketSetState s;
    memset(&s, 0, sizeof(s));
    s.depth_bits = 24; s.depth_func = GL_LESS; s.depth_near = 0; s.depth_far = 0x00ffffffU;
    s.viewport_w = W; s.viewport_h = H; s.color_mask = 0xfU; s.polygon_mode = GL_FILL;
    s.tex_env_mode[0] = GL_MODULATE; s.tex_env_mode[1] = GL_MODULATE;
    s.blend_src = ZZ9K_MGL_WIRE_ONE;
    s.blend_dst = ZZ9K_MGL_WIRE_ZERO;
    s.blend_src_alpha = ZZ9K_MGL_WIRE_ONE;
    s.blend_dst_alpha = ZZ9K_MGL_WIRE_ZERO;
    s.blend_equation = ZZ9K_MGL_WIRE_FUNC_ADD;
    return s;
}
static uint32_t pixel(unsigned x, unsigned y) { return color[y * W + x]; }

static float oracle_edge(const float x[3], const float y[3], unsigned a, unsigned b, float px, float py)
{
    return (x[b] - x[a]) * (py - y[a]) - (y[b] - y[a]) * (px - x[a]);
}

typedef struct RastPoint {
    int32_t x;
    int32_t y;
} RastPoint;

static inline RastPoint rast_to_fixed(float sx, float sy)
{
    RastPoint p;
    p.x = (int32_t)roundf(sx * 256.0f);
    p.y = (int32_t)roundf(sy * 256.0f);
    return p;
}

static inline int rast_top_left(RastPoint a, RastPoint b)
{
    int32_t dx = b.x - a.x;
    int32_t dy = b.y - a.y;
    return dy < 0 || (dy == 0 && dx > 0);
}

static inline int rast_edge_covers(RastPoint a, RastPoint b, int x, int y, int inclusive)
{
    int64_t px = ((int64_t)x << 8) + 128;
    int64_t py = ((int64_t)y << 8) + 128;
    int64_t e = (int64_t)(b.x - a.x) * (py - (int64_t)a.y) - (int64_t)(b.y - a.y) * (px - (int64_t)a.x);
    return e > 0 || (e == 0 && inclusive);
}

static void assert_scanline_coverage(void)
{
    uint32_t random = 0x13579bdfU;
    unsigned trial;
    ZZ9KMiniGLPacketSetState s = state();
    s.shade_model = GL_FLAT; s.depth_func = GL_LEQUAL;
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    minigl_raster_reset(); minigl_raster_set_state(&s);
    for (trial = 0U; trial < 128U; ++trial) {
        float sx[3], sy[3], area;
        unsigned i, x, y;
        for (i = 0U; i < 3U; ++i) {
            float vx, vy;
            random = random * 1664525U + 1013904223U;
            vx = (float)(random & 65535U) / 32768.0f - 1.0f;
            random = random * 1664525U + 1013904223U;
            vy = (float)(random & 65535U) / 32768.0f - 1.0f;
            vertex(i, vx, vy, 1.0f, 0xffffffffU, 0, 0);
            sx[i] = (vx + 1.0f) * 0.5f * W;
            sy[i] = H - (vy + 1.0f) * 0.5f * H;
        }
        area = oracle_edge(sx, sy, 0U, 1U, sx[2], sy[2]);
        if (area < 0.0f) {
            float swap = sx[1]; sx[1] = sx[2]; sx[2] = swap;
            swap = sy[1]; sy[1] = sy[2]; sy[2] = swap;
            area = -area;
        }
        minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
        minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
        RastPoint pa = rast_to_fixed(sx[0], sy[0]);
        RastPoint pb = rast_to_fixed(sx[1], sy[1]);
        RastPoint pc = rast_to_fixed(sx[2], sy[2]);
        int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
        int x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
        int y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
        int y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= (int)W) x1 = (int)W - 1;
        if (y1 >= (int)H) y1 = (int)H - 1;
        for (y = 0U; y < H; ++y) for (x = 0U; x < W; ++x) {
            int covered = (area != 0.0f) &&
                          (int)x >= x0 && (int)x <= x1 &&
                          (int)y >= y0 && (int)y <= y1 &&
                          rast_edge_covers(pb, pc, (int)x, (int)y, rast_top_left(pb, pc)) &&
                          rast_edge_covers(pc, pa, (int)x, (int)y, rast_top_left(pc, pa)) &&
                          rast_edge_covers(pa, pb, (int)x, (int)y, rast_top_left(pa, pb));
            unsigned di = (y * W + x) * 3U;
            assert(pixel(x, y) == (covered ? 0xffffffffU : 0U));
            assert(depth[di] == 255U && depth[di + 1U] == 255U && depth[di + 2U] == 255U);
        }
    }
}

static void assert_point_half_open_coverage(void)
{
    ZZ9KMiniGLPacketSetState s = state();

    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
    /* NDC (1/64, -1/48) projects to the half-integer (32.5, 24.5). */
    vertex(0U, 1.0f / 64.0f, -1.0f / 48.0f, 0.0f, 0xff0000ffU, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(32U, 24U) == 0xff0000ffU);
    assert(pixel(31U, 24U) == 0U && pixel(33U, 24U) == 0U);
    assert(pixel(32U, 23U) == 0U && pixel(32U, 25U) == 0U);

    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
    /* An integer centre belongs to the low half-open pixel square. */
    vertex(0U, 0.0f, 0.0f, 0.0f, 0x00ff00ffU, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    assert(pixel(32U, 23U) == 0U && pixel(31U, 24U) == 0U && pixel(32U, 24U) == 0U);
}

static void wire_vertex(float *v, float x, float y, uint32_t rgba)
{
    memset(v, 0, MW_OBJECT_BYTES);
    v[0] = x;
    v[1] = y;
    v[3] = 1.0f;
    v[4] = (float)(rgba >> 24) / 255.0f;
    v[5] = (float)((rgba >> 16) & 0xffU) / 255.0f;
    v[6] = (float)((rgba >> 8) & 0xffU) / 255.0f;
    v[7] = (float)(rgba & 0xffU) / 255.0f;
}

static void assert_large_lowered_outline(void)
{
    enum { OUTLINE_COUNT = 3444U };
    static float lowered[OUTLINE_COUNT * MW_OBJECT_WORDS];
    ZZ9KMiniGLPacketSetState s = state();
    unsigned i;

    for (i = 0U; i < OUTLINE_COUNT; i += 3U) {
        wire_vertex(lowered + (i + 0U) * MW_OBJECT_WORDS, 2.0f, 2.0f, 0xffffffffU);
        wire_vertex(lowered + (i + 1U) * MW_OBJECT_WORDS, 3.0f, 2.0f, 0xffffffffU);
        wire_vertex(lowered + (i + 2U) * MW_OBJECT_WORDS, 2.0f, 3.0f, 0xffffffffU);
    }
    wire_vertex(lowered, -0.75f, -0.75f, 0xff0000ffU);
    wire_vertex(lowered + MW_OBJECT_WORDS, -0.25f, -0.75f, 0xff0000ffU);
    wire_vertex(lowered + 2U * MW_OBJECT_WORDS, -0.50f, -0.25f, 0xff0000ffU);
    wire_vertex(lowered + (OUTLINE_COUNT - 3U) * MW_OBJECT_WORDS, 0.25f, -0.75f, 0x00ff00ffU);
    wire_vertex(lowered + (OUTLINE_COUNT - 2U) * MW_OBJECT_WORDS, 0.75f, -0.75f, 0x00ff00ffU);
    wire_vertex(lowered + (OUTLINE_COUNT - 1U) * MW_OBJECT_WORDS, 0.50f, -0.25f, 0x00ff00ffU);

    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
    minigl_raster_draw_wire_v9(GL_TRIANGLES, lowered, OUTLINE_COUNT,
                               2U << MW_LOWERED_FRONT_SHIFT, NULL);
    assert(pixel(16U, 41U) == 0xff0000ffU);
    assert(pixel(48U, 41U) == 0x00ff00ffU);
    assert(pixel(16U, 36U) == 0U);
    assert(pixel(48U, 36U) == 0U);
}

static void mip_level(uint32_t id, unsigned level, unsigned width, unsigned height, uint32_t value)
{
    unsigned i;
    for (i = 0U; i < width * height; ++i) put32(staging + i * 4U, value);
    minigl_raster_upload_texture(id, (uint16_t)level, (uint16_t)width, (uint16_t)height,
                                  ZZ9K_MGL_TEXFMT_RGBA8888, 0U, width * height * 4U);
    assert(minigl_raster_get_error() == 0U);
}

static void mip_quad(float extent, float repeat)
{
    vertex(0U, -extent, -extent, 0, 0xffffffffU, 0, 0);
    vertex(1U, extent, -extent, 0, 0xffffffffU, repeat, 0);
    vertex(2U, extent, extent, 0, 0xffffffffU, repeat, 0);
    vertex(3U, -extent, extent, 0, 0xffffffffU, 0, 0);
    minigl_raster_clear(0x4000U, 0U, 0U);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
}

static void assert_mipmaps(void)
{
    ZZ9KMiniGLPacketSetState s = state();
    unsigned filter;
    minigl_raster_reset();
    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; s.shade_model = GL_FLAT;
    minigl_raster_set_state(&s);
    mip_level(1U, 0U, 8U, 8U, 0xff0000ffU);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    minigl_raster_tex_params(1U, 0x2701U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.0f);
    assert(pixel(32U, 24U) == 0xff0000ffU);
    mip_level(1U, 1U, 4U, 4U, 0x00ff00ffU);
    mip_level(1U, 2U, 2U, 2U, 0x0000ffffU);
    mip_level(1U, 3U, 1U, 1U, 0xffffffffU);
    mip_quad(0.125f, 4.0f);
    assert(pixel(32U, 24U) == 0x0000ffffU);
    for (filter = 0x2700U; filter <= 0x2703U; ++filter) {
        minigl_raster_tex_params(1U, (uint16_t)filter, GL_NEAREST, GL_REPEAT, GL_REPEAT);
        mip_quad(0.125f, 4.0f);
        assert(pixel(32U, 24U) == 0x0000ffffU);
        mip_quad(0.5f, 1.0f);
        assert(pixel(32U, 24U) == 0xff0000ffU);
    }
    for (filter = 0x2702U; filter <= 0x2703U; ++filter) {
        uint32_t value;
        minigl_raster_tex_params(1U, (uint16_t)filter, GL_NEAREST, GL_REPEAT, GL_REPEAT);
        mip_quad(0.125f, 2.82842712475f);
        value = pixel(32U, 24U);
        assert((value >> 24) == 0U && ((value >> 16) & 255U) >= 127U &&
               ((value >> 16) & 255U) <= 128U && ((value >> 8) & 255U) >= 127U && ((value >> 8) & 255U) <= 128U);
    }
    minigl_raster_tex_params(1U, 0x2701U, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    put32(staging, 0xffff00ffU);
    minigl_raster_tex_subimage(1U, 1U, 0U, 0U, 1U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 4U);
    vertex(0U, -0.125f, -0.125f, 0, 0xffffffffU, 0, 0.125f);
    vertex(1U, 0.125f, -0.125f, 0, 0xffffffffU, 2, 0.125f);
    vertex(2U, 0.125f, 0.125f, 0, 0xffffffffU, 2, 0.125f);
    vertex(3U, -0.125f, 0.125f, 0, 0xffffffffU, 0, 0.125f);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
    assert(pixel(32U, 24U) == 0xffff00ffU);
    mip_level(1U, 1U, 2U, 2U, 0x00ff00ffU);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0xff0000ffU);
    mip_level(1U, 1U, 4U, 4U, 0x00ff00ffU);
    vertex(0U, -0.75f, -0.6666666667f, 0, 0xffffffffU, 0, 0);
    vertex(1U, 1.5f, -1.3333333333f, 0, 0xffffffffU, 64, 0);
    vertex(2U, -0.75f, 0.6666666667f, 0, 0xffffffffU, 0, 0);
    putfloat(vertices + VSTRIDE + 44U, 2.0f);
    putfloat(vertices + VSTRIDE + 48U, 8.0f);
    minigl_raster_clear(0x4000U, 0U, 0U);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(10U, 38U) == 0x0000ffffU && pixel(45U, 38U) == 0xff0000ffU);
    minigl_raster_delete_texture(1U);
    mip_level(1U, 0U, 8U, 8U, 0xff0000ffU);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    minigl_raster_tex_params(1U, 0x2701U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0xff0000ffU);
    minigl_raster_tex_params(1U, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0xff0000ffU);
    minigl_raster_upload_texture(1U, ZZ9K_MINIGL_MAX_MIP_LEVELS, 1U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 4U);
    assert(minigl_raster_get_error() != 0U);
    put32(staging, 0xff0000ffU); put32(staging + 4U, 0x0000ffffU);
    minigl_raster_upload_texture(2U, 0U, 2U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 8U);
    mip_level(2U, 1U, 1U, 1U, 0x00ff00ffU);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 2U);
    vertex(0U, 0, 0, 0, 0xffffffffU, 0.5f, 0);
    minigl_raster_tex_params(2U, 0x2700U, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x0000ffffU);
    minigl_raster_tex_params(2U, 0x2700U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x800080ffU);
    mip_quad(0.125f, 4.8f);
    {
        uint32_t value = pixel(32U, 24U);
        assert((value >> 24) > 0U && ((value >> 8) & 255U) > 0U && ((value >> 16) & 255U) == 0U);
    }
    mip_quad(0.125f, 6.4f);
    assert(pixel(32U, 24U) == 0x00ff00ffU);
    minigl_raster_tex_params(2U, GL_NEAREST, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.8f);
    assert(pixel(32U, 24U) == 0x0000ffffU);
    minigl_raster_tex_params(2U, GL_LINEAR, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.8f);
    {
        uint32_t value = pixel(32U, 24U);
        assert((value >> 24) > 0U && ((value >> 8) & 255U) > 0U);
    }
    put32(staging, 0xff0000ffU); put32(staging + 4U, 0x00ff00ffU);
    put32(staging + 8U, 0x0000ffffU); put32(staging + 12U, 0xffffffffU);
    minigl_raster_upload_texture(3U, 0U, 2U, 2U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 16U);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 3U);
    vertex(0U, 0, 0, 0, 0xffffffffU, -0.25f, 1.25f);
    minigl_raster_tex_params(3U, GL_NEAREST, GL_NEAREST, GL_REPEAT, 0x812fU);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0xffffffffU);
    minigl_raster_tex_params(3U, GL_NEAREST, GL_NEAREST, 0x812fU, GL_REPEAT);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0xff0000ffU);
    mip_level(4U, 3U, 1U, 1U, 0xffffffffU);
    mip_level(4U, 2U, 2U, 2U, 0x0000ffffU);
    mip_level(4U, 1U, 4U, 4U, 0x00ff00ffU);
    mip_level(4U, 0U, 8U, 8U, 0xff0000ffU);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 4U);
    minigl_raster_tex_params(4U, 0x2701U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0x0000ffffU);
    mip_level(4U, 0U, 16U, 16U, 0xff0000ffU);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0xff0000ffU);
    mip_level(4U, 1U, 8U, 8U, 0x00ff00ffU);
    mip_level(4U, 2U, 4U, 4U, 0x0000ffffU);
    mip_level(4U, 3U, 2U, 2U, 0xffff00ffU);
    mip_level(4U, 4U, 1U, 1U, 0xffffffffU);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0xffff00ffU);
}
static void assert_quake_charset_incomplete_mipmaps(void)
{
    ZZ9KMiniGLPacketSetState s = state();
    unsigned y, x;
    uint32_t val;
    minigl_raster_reset();

    for (y = 0U; y < 16U; ++y) {
        for (x = 0U; x < 16U; ++x) {
            uint32_t c = (x >= 4U && x < 12U && y >= 4U && y < 12U && (x == 8U || y == 8U))
                ? 0xd0c090aaU
                : 0x9f5b5300U;
            put32(staging + (y * 16U + x) * 4U, c);
        }
    }
    minigl_raster_upload_texture(1U, 0U, 16U, 16U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 16U * 16U * 4U);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    minigl_raster_tex_params(1U, 0x2701U, GL_LINEAR, GL_REPEAT, GL_REPEAT);

    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_ALPHA_TEST;
    s.shade_model = GL_FLAT;
    s.alpha_func = GL_GREATER;
    s.alpha_ref = 32767U;
    s.tex_env_mode[0] = GL_REPLACE;
    minigl_raster_set_state(&s);

    minigl_raster_clear(0x4000U, 0x000000ffU, 0U);
    vertex(0U, -1.0f, -1.0f, 0, 0xffffffffU, 0.0f, 0.0f);
    vertex(1U,  1.0f, -1.0f, 0, 0xffffffffU, 1.0f, 0.0f);
    vertex(2U,  1.0f,  1.0f, 0, 0xffffffffU, 1.0f, 1.0f);
    vertex(3U, -1.0f,  1.0f, 0, 0xffffffffU, 0.0f, 1.0f);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);

    /* LINEAR base filter: bilinear mix of glyph and transparent background.
     * Old strict-GL behaviour returned the untextured white vertex colour. */
    val = pixel(32U, 24U);
    assert((val >> 24) > 0x9fU && (val >> 24) < 0xffU);
    assert((val & 0xffU) > 0U && (val & 0xffU) < 0xffU);

    /* NEAREST base filter must hit the glyph texel exactly. */
    minigl_raster_tex_params(1U, 0x2700U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
    assert(pixel(32U, 24U) == 0xd0c090aaU);
}

static void assert_default_texture(void)
{
    ZZ9KMiniGLPacketSetState s = state();
    minigl_raster_reset();
    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; s.shade_model = GL_FLAT;
    minigl_raster_set_state(&s);
    minigl_raster_tex_params(0U, 0x2701U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    assert(minigl_raster_get_error() == 0U);
    mip_quad(0.125f, 8.0f); assert(pixel(32U, 24U) == 0xffffffffU);
    mip_level(0U, 0U, 2U, 2U, 0xff0000ffU);
    mip_quad(0.125f, 8.0f); assert(pixel(32U, 24U) == 0xff0000ffU);
    mip_level(0U, 1U, 1U, 1U, 0x0000ffffU);
    mip_quad(0.125f, 8.0f); assert(pixel(32U, 24U) == 0x0000ffffU);
    vertex(0U, 0, 0, 0, 0xffffffffU, 0.25f, 0.25f);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0xff0000ffU);
    put32(staging, 0x00ff00ffU);
    minigl_raster_tex_subimage(0U, 0U, 0U, 0U, 1U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 4U);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    minigl_raster_delete_texture(0U);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    mip_level(1U, 0U, 1U, 1U, 0xff0000ffU);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0xff0000ffU);
    minigl_raster_delete_texture(1U);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    minigl_raster_bind_texture(1U, GL_TEXTURE_2D, 0U);
    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_1; minigl_raster_set_state(&s);
    putfloat(vertices + 24U, 0.25f); putfloat(vertices + 28U, 0.25f);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    minigl_raster_reset(); minigl_raster_set_state(&s);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(31U, 23U) == 0xffffffffU);
}

static void assert_sampler_defaults(void)
{
    ZZ9KMiniGLPacketSetState s = state();
    minigl_raster_reset();
    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; s.shade_model = GL_FLAT;
    minigl_raster_set_state(&s);
    put32(staging, 0xff0000ffU); put32(staging + 4U, 0x00ff00ffU);
    minigl_raster_upload_texture(1U, 0U, 2U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 8U);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    vertex(0U, 0, 0, 0, 0xffffffffU, 0.5f, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0U, VSTRIDE, 0U, 1U, 0U);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    mip_level(1U, 1U, 1U, 1U, 0x0000ffffU);
    minigl_raster_draw_arrays(GL_POINTS, 0U, VSTRIDE, 0U, 1U, 0U);
    assert(pixel(31U, 23U) == 0x808000ffU);
    mip_quad(0.125f, 8.0f); assert(pixel(32U, 24U) == 0x0000ffffU);
    minigl_raster_tex_params(1U, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    minigl_raster_delete_texture(1U);
    put32(staging, 0xff0000ffU); put32(staging + 4U, 0x00ff00ffU);
    minigl_raster_upload_texture(1U, 0U, 2U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 8U);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    vertex(0U, 0, 0, 0, 0xffffffffU, 0.5f, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0U, VSTRIDE, 0U, 1U, 0U);
    assert(pixel(31U, 23U) == 0x00ff00ffU);
    mip_level(1U, 1U, 1U, 1U, 0x0000ffffU);
    minigl_raster_draw_arrays(GL_POINTS, 0U, VSTRIDE, 0U, 1U, 0U);
    assert(pixel(31U, 23U) == 0x808000ffU);
    assert(minigl_raster_get_error() == 0U);
}

static void assert_integer_vertices(void)
{
    ZZ9KMiniGLPacketSetState s = state();
    float matrix[16] = {0};
    uint32_t expected_color[W * H];
    uint8_t expected_depth[sizeof(depth)];
    const int32_t positions[3][3] = {{-120, -120, -40}, {120, -120, 40}, {0, 120, 0}};
    const uint8_t indices[12] = {0, 0, 0, 1, 0, 2, 0, 0, 0, 1, 0, 2};
    unsigned i;
    minigl_raster_reset();
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    s.depth_func = GL_LEQUAL; minigl_raster_set_state(&s);
    matrix[0] = matrix[5] = matrix[10] = 0.005f; matrix[15] = 1.0f;
    minigl_raster_load_matrix(0U, matrix);
    for (i = 0U; i < 3U; ++i)
        vertex(i, (float)positions[i][0], (float)positions[i][1], (float)positions[i][2], 0xaabbeeffU, 0, 0);
    putfloat(vertices + VSTRIDE + 44U, 2.0f);
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0U, VSTRIDE, 0U, 3U, 0U);
    assert(pixel(32U, 24U) == 0xaabbeeffU);
    memcpy(expected_color, color, sizeof(color)); memcpy(expected_depth, depth, sizeof(depth));
    for (i = 0U; i < 3U; ++i) {
        put32(vertices + i * VSTRIDE, (uint32_t)positions[i][0]);
        put32(vertices + i * VSTRIDE + 4U, (uint32_t)positions[i][1]);
        put32(vertices + i * VSTRIDE + 8U, (uint32_t)positions[i][2]);
    }
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, MINIGL_RASTER_ATTR_POSITION_INT32, VSTRIDE, 0U, 3U, 0U);
    assert(memcmp(expected_color, color, sizeof(color)) == 0);
    assert(memcmp(expected_depth, depth, sizeof(depth)) == 0);
    memcpy(vertices + 3U * VSTRIDE, indices, sizeof(indices));
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_elements(GL_TRIANGLES, MINIGL_RASTER_ATTR_POSITION_INT32, VSTRIDE, 6U, 0U, 3U * VSTRIDE);
    assert(memcmp(expected_color, color, sizeof(color)) == 0);
    assert(memcmp(expected_depth, depth, sizeof(depth)) == 0);
    for (i = 0U; i < 3U; ++i) {
        putfloat(vertices + i * VSTRIDE, (float)positions[i][0]);
        putfloat(vertices + i * VSTRIDE + 4U, (float)positions[i][1]);
        putfloat(vertices + i * VSTRIDE + 8U, (float)positions[i][2]);
    }
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_elements(GL_TRIANGLES, 0U, VSTRIDE, 6U, 0U, 3U * VSTRIDE);
    assert(memcmp(expected_color, color, sizeof(color)) == 0);
    assert(memcmp(expected_depth, depth, sizeof(depth)) == 0);
    assert(minigl_raster_get_error() == 0U);
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 2U, VSTRIDE, 0U, 3U, 0U);
    assert(minigl_raster_get_error() != 0U && pixel(32U, 24U) == 0U);
    minigl_raster_draw_elements(GL_TRIANGLES, 2U, VSTRIDE, 6U, 0U, 3U * VSTRIDE);
    assert(minigl_raster_get_error() != 0U && pixel(32U, 24U) == 0U);
    minigl_raster_reset(); s = state(); minigl_raster_set_state(&s);
    matrix[0] = matrix[5] = matrix[10] = 0x1p-32f;
    minigl_raster_load_matrix(0U, matrix);
    vertex(0U, 0, 0, 0, 0xff0000ffU, 0, 0); put32(vertices, 0x80000000U);
    vertex(1U, 0, 0, 0, 0x0000ffffU, 0, 0); put32(vertices + VSTRIDE, 0x7fffffffU);
    minigl_raster_clear(0x4000U, 0U, 0U);
    minigl_raster_draw_arrays(GL_POINTS, MINIGL_RASTER_ATTR_POSITION_INT32, VSTRIDE, 0U, 2U, 0U);
    assert(pixel(15U, 23U) == 0xff0000ffU && pixel(47U, 23U) == 0x0000ffffU);
}

static void assert_texture_compaction(void)
{
    MiniGLRasterBuffers buffers = {
        color, sizeof(color), depth, sizeof(depth), vertices, sizeof(vertices),
        staging, sizeof(staging), textures, 128U, W, H
    };
    ZZ9KMiniGLPacketSetState s = state();
    unsigned id;
    assert(minigl_raster_configure_buffers(&buffers)); minigl_raster_reset();
    for (id = 1U; id <= 4U; ++id) mip_level(id, 0U, 8U, 1U, (id << 24) | 0xffU);
    minigl_raster_delete_texture(2U); minigl_raster_delete_texture(4U);
    mip_level(5U, 0U, 16U, 1U, 0x050000ffU);
    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; minigl_raster_set_state(&s);
    vertex(0U, 0, 0, 0, 0xffffffffU, 0, 0);
    for (id = 1U; id <= 5U; id += 2U) {
        minigl_raster_bind_texture(0U, GL_TEXTURE_2D, id);
        minigl_raster_tex_params(id, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(31U, 23U) == ((id << 24) | 0xffU));
    }
    buffers.texture_pool_bytes = 512U;
    assert(minigl_raster_configure_buffers(&buffers)); minigl_raster_reset();
    mip_level(1U, 0U, 32U, 1U, 0xffffffffU);
    mip_level(2U, 0U, 8U, 8U, 0xff0000ffU);
    mip_level(2U, 1U, 4U, 4U, 0x00ff00ffU);
    mip_level(2U, 2U, 2U, 2U, 0x0000ffffU);
    mip_level(2U, 3U, 1U, 1U, 0xffffffffU);
    minigl_raster_delete_texture(1U);
    mip_level(3U, 0U, 40U, 1U, 0xffffffffU);
    minigl_raster_set_state(&s);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 2U);
    minigl_raster_tex_params(2U, 0x2701U, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0x0000ffffU);
    mip_quad(0.5f, 1.0f); assert(pixel(32U, 24U) == 0xff0000ffU);
    minigl_raster_delete_texture(3U);
    for (id = 0U; id < 64U; ++id) put32(staging + id * 4U, 0xffffffffU);
    minigl_raster_upload_texture(2U, 1U, 8U, 8U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 256U);
    assert(minigl_raster_get_error() != 0U);
    mip_quad(0.125f, 4.0f); assert(pixel(32U, 24U) == 0x0000ffffU);
    buffers.texture_pool_bytes = sizeof(textures);
    assert(minigl_raster_configure_buffers(&buffers)); minigl_raster_reset();
}

static void assert_fragmented_pool_growth(void)
{
    MiniGLRasterBuffers buffers = {
        color, sizeof(color), depth, sizeof(depth), vertices, sizeof(vertices),
        staging, sizeof(staging), fragmented_pool, sizeof(fragmented_pool), W, H
    };
    ZZ9KMiniGLPacketSetState s = state();
    uint32_t id;

    memset(fragmented_pool, 0, sizeof(fragmented_pool));
    memset(grown_pool, 0, sizeof(grown_pool));
    grow_minimum = 0U;
    grow_live_bytes = 0U;
    assert(minigl_raster_configure_buffers(&buffers));
    minigl_raster_set_texture_pool(fragmented_pool, sizeof(fragmented_pool),
                                   grow_fragmented_pool, NULL);
    minigl_raster_reset();
    for (id = 1U; id <= 4U; ++id)
        mip_level(id, 0U, 2U, 1U, (id << 24) | 0xffU);
    minigl_raster_delete_texture(1U);
    mip_level(5U, 0U, 4U, 1U, 0x050000ffU);
    assert(grow_minimum == 40U);
    assert(grow_live_bytes == 32U);

    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0;
    minigl_raster_set_state(&s);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 4U);
    minigl_raster_tex_params(4U, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    vertex(0U, 0.0f, 0.0f, 0.0f, 0xffffffffU, 0.0f, 0.0f);
    minigl_raster_clear(0x4000U, 0U, 0U);
    minigl_raster_draw_arrays(GL_POINTS, 0U, VSTRIDE, 0U, 1U, 0U);
    assert(pixel(31U, 23U) == 0x040000ffU);

    minigl_raster_set_texture_pool(textures, sizeof(textures), NULL, NULL);
    buffers.texture_pool = textures;
    buffers.texture_pool_bytes = sizeof(textures);
    assert(minigl_raster_configure_buffers(&buffers));
    minigl_raster_reset();
}

static void draw_point(uint32_t rgba)
{
    vertex(0U, 0.0f, 0.0f, 0.0f, rgba, 0.0f, 0.0f);
    minigl_raster_draw_arrays(GL_POINTS, 0U, VSTRIDE, 0U, 1U, 0U);
}

static void draw_depth_triangle(uint32_t rgba, float z)
{
    vertex(0U, -0.8f, -0.8f, z, rgba, 0.0f, 0.0f);
    vertex(1U, 0.8f, -0.8f, z, rgba, 0.0f, 0.0f);
    vertex(2U, 0.0f, 0.8f, z, rgba, 0.0f, 0.0f);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0U, VSTRIDE, 0U, 3U, 0U);
}

static void draw_sloped_depth_triangle(uint32_t rgba)
{
    vertex(0U, -0.8f, -0.8f, -0.25f, rgba, 0.0f, 0.0f);
    vertex(1U, 0.8f, -0.8f, 0.25f, rgba, 0.0f, 0.0f);
    vertex(2U, 0.0f, 0.8f, 0.0f, rgba, 0.0f, 0.0f);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0U, VSTRIDE, 0U, 3U, 0U);
}

static void assert_fragment_extensions(void)
{
    static const struct { uint16_t equation; uint32_t expected; } equations[] = {
        { GL_FUNC_ADD, 0xb0907080U }, { GL_MIN, 0x20401040U },
        { GL_MAX, 0x90506080U }, { GL_FUNC_SUBTRACT, 0x70100000U },
        { GL_FUNC_REVERSE_SUBTRACT, 0x00005080U }
    };
    ZZ9KMiniGLPacketSetState s;
    unsigned index;

    for (index = 0U; index < sizeof(equations) / sizeof(equations[0]); ++index) {
        s = state();
        s.enabled_flags = ZZ9K_MGL_STATE_BLEND;
        s.blend_src = equations[index].equation == GL_MIN || equations[index].equation == GL_MAX ?
            GL_ZERO : GL_ONE;
        s.blend_dst = GL_ONE;
        s.blend_src_alpha = GL_ZERO;
        s.blend_dst_alpha = GL_ONE;
        s.blend_equation = equations[index].equation;
        minigl_raster_set_state(&s);
        minigl_raster_clear(0x4000U, 0x20406080U, 0x00ffffffU);
        draw_point(0x90501040U);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == equations[index].expected);
    }

    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_BLEND;
    s.blend_src = GL_SRC_ALPHA; s.blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    s.blend_src_alpha = GL_ONE; s.blend_dst_alpha = GL_ZERO;
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0x20406080U, 0x00ffffffU);
    draw_point(0x90501040U);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x3c444c40U);

    s.blend_src = GL_SRC_ALPHA_SATURATE; s.blend_dst = GL_ONE;
    s.blend_src_alpha = GL_SRC_ALPHA_SATURATE; s.blend_dst_alpha = GL_ZERO;
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0x20406080U, 0x00ffffffU);
    draw_point(0x90501040U);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x44546440U);

    for (index = 0U; index < 4U; ++index) {
        s = state();
        s.depth_bits = index < 2U ? 16U : 24U;
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
        s.depth_func = GL_LESS; s.polygon_mode = GL_FILL;
        s.depth_near = (index & 1U) ? 0x00ffffffU : 0U;
        s.depth_far = (index & 1U) ? 0U : 0x00ffffffU;
        minigl_raster_set_state(&s);
        minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
        draw_sloped_depth_triangle(0xff0000ffU);
        s.enabled_flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
        s.polygon_offset_factor = -1.0f;
        s.polygon_offset_units = 0.0f;
        s.polygon_mode = 0U; /* Raster dispatch treats absent mode as fill. */
        minigl_raster_set_state(&s);
        draw_sloped_depth_triangle(0x00ff00ffU);
        assert(pixel(W / 2U, H / 2U) == 0x00ff00ffU);

        minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
        s.polygon_mode = GL_FILL;
        s.enabled_flags &= ~ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
        s.enabled_flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_LINE;
        minigl_raster_set_state(&s);
        draw_depth_triangle(0xff0000ffU, 0.0f);
        draw_depth_triangle(0x0000ffffU, 0.0f);
        assert(pixel(W / 2U, H / 2U) == 0xff0000ffU);

        minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE |
                          ZZ9K_MGL_STATE_POLYGON_OFFSET_POINT;
        s.polygon_offset_units = -64.0f;
        minigl_raster_set_state(&s);
        draw_point(0xff0000ffU);
        draw_point(0x0000ffffU);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0xff0000ffU);
    }
}

struct rix_raster_model {
    uint32_t csr[16];
    uint8_t command[MINIGL_RIX_COMMAND_BUFFERS][MINIGL_RIX_COMMAND_BYTES];
    uint8_t white[MINIGL_RIX_WHITE_TEXTURE_BYTES];
    unsigned csr_reads;
    unsigned cache_cleans;
    unsigned cache_invalidates;
    int complete;
};

static uint32_t rix_raster_read(void *context, uint32_t offset)
{
    struct rix_raster_model *model = context;

    ++model->csr_reads;
    return model->csr[offset / 4U];
}

static void rix_raster_write(void *context, uint32_t offset, uint32_t value)
{
    struct rix_raster_model *model = context;

    model->csr[offset / 4U] = value;
    if (offset == RIX_DRV_CSR_CMD_START && value != 0U) {
        ++model->csr[RIX_DRV_CSR_SUBMITTED / 4U];
        if (model->complete)
            model->csr[RIX_DRV_CSR_COMPLETED / 4U] =
                model->csr[RIX_DRV_CSR_SUBMITTED / 4U];
    }
}

static void rix_raster_cache_clean(void *context, uintptr_t address, size_t bytes)
{
    struct rix_raster_model *model = context;

    (void)address;
    (void)bytes;
    ++model->cache_cleans;
}

static void rix_raster_cache_invalidate(void *context, uintptr_t address, size_t bytes)
{
    struct rix_raster_model *model = context;

    (void)address;
    (void)bytes;
    ++model->cache_invalidates;
}

static void assert_rix_ownership_transitions(void)
{
    static struct rix_raster_model model;
    MiniGLRixPlatform platform;
    MiniGLRasterRixDiagnostics diagnostics;
    ZZ9KMiniGLPacketSetState s = state();
    unsigned reads_after_fail;
    memset(&model, 0, sizeof(model));
    model.complete = 1;
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.csr[RIX_DRV_CSR_CMD_START / 4U] = RIX_DRV_CMD_START_QUEUE_READY;
    memset(&platform, 0, sizeof(platform));
    platform.context = &model;
    platform.csr_read = rix_raster_read;
    platform.csr_write = rix_raster_write;
    platform.cache_clean = rix_raster_cache_clean;
    platform.cache_invalidate = rix_raster_cache_invalidate;
    platform.command_buffer[0] = model.command[0];
    platform.command_buffer[1] = model.command[1];
    platform.command_address[0] = 0x01000000U;
    platform.command_address[1] = 0x01010000U;
    platform.command_capacity = MINIGL_RIX_COMMAND_BYTES;
    platform.white_texture = model.white;
    platform.white_texture_address = 0x02000000U;
    platform.white_texture_bytes = sizeof(model.white);

    minigl_raster_set_rix_platform(&platform);
    minigl_raster_init();
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    minigl_raster_set_state(&s);
    memset(depth, 0, sizeof(depth));
    depth[0] = 0x12U;
    depth[1] = 0x34U;
    depth[2] = 0x56U;
    vertex(0U, -0.8f, -0.8f, 0.0f, 0xff0000ffU, 0.0f, 0.0f);
    vertex(1U, 0.8f, -0.8f, 0.0f, 0xff0000ffU, 0.0f, 0.0f);
    vertex(2U, 0.0f, 0.8f, 0.0f, 0xff0000ffU, 0.0f, 0.0f);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0U, VSTRIDE, 0U, 3U, 0U);
    assert(minigl_raster_finish());
    assert(depth[0] == 0x12U && depth[1] == 0x34U && depth[2] == 0U);
    assert(model.cache_cleans != 0U && model.cache_invalidates != 0U);

    s.scissor_x = W;
    s.scissor_w = 1U;
    s.scissor_h = H;
    s.enabled_flags |= ZZ9K_MGL_STATE_SCISSOR_TEST;
    minigl_raster_set_state(&s);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0U, VSTRIDE, 0U, 3U, 0U);
    assert(minigl_raster_hardware_available());
    assert(minigl_raster_finish());
    assert(minigl_raster_quiesce());
    assert(!minigl_raster_hardware_available());

    minigl_raster_set_rix_platform(&platform);
    minigl_raster_init();
    s = state();
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    minigl_raster_set_state(&s);
    assert(minigl_raster_get_rix_diagnostics(&diagnostics));
    model.complete = 0;
    minigl_raster_draw_arrays(GL_TRIANGLES, 0U, VSTRIDE, 0U, 3U, 0U);
    assert(!minigl_raster_finish());
    reads_after_fail = model.csr_reads;
    assert(minigl_raster_get_rix_diagnostics(&diagnostics));
    assert(diagnostics.active == 0U);
    assert(model.csr_reads == reads_after_fail);
    minigl_raster_set_rix_platform(NULL);
}

int main(void)
{
    ZZ9KMiniGLPacketSetState s;
    uint32_t changed = 0;
    unsigned x, y;

    {
        MiniGLRasterBuffers buffers = {
            color, sizeof(color), depth, sizeof(depth), vertices, sizeof(vertices),
            staging, sizeof(staging), textures, sizeof(textures), W, H
        };
        assert(minigl_raster_configure_buffers(&buffers));
    }
    minigl_raster_init();
    s = state(); s.depth_func = GL_LEQUAL;
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    vertex(0, 0.0f, 0.0f, 1.0f, 0xff0000ffU, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0xff0000ffU);
    {
        unsigned di = ((H / 2U - 1U) * W + W / 2U - 1U) * 3U;
        assert(depth[di] == 255U && depth[di + 1U] == 255U && depth[di + 2U] == 255U);
    }
    s.depth_bits = 16U; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0xff0000ffU);
    {
        unsigned di = ((H / 2U - 1U) * W + W / 2U - 1U) * 2U;
        assert(depth[di] == 255U && depth[di + 1U] == 255U);
    }
    s.depth_bits = 24U; s.depth_near = 0x00ffffffU; s.depth_far = 0U;
    minigl_raster_set_state(&s); minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    vertex(0, 0.0f, 0.0f, -1.0f, 0x00ff00ffU, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x00ff00ffU);
    {
        unsigned di = ((H / 2U - 1U) * W + W / 2U - 1U) * 3U;
        assert(depth[di] == 255U && depth[di + 1U] == 255U && depth[di + 2U] == 255U);
        vertex(0, 0, 0, 1.0f, 0x0000ffffU, 0, 0);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(depth[di] == 0U && depth[di + 1U] == 0U && depth[di + 2U] == 0U);
    }
    s.depth_near = 0U; s.depth_far = 0U;
    for (x = 0U; x < 6U; ++x) {
        unsigned bytes = x < 3U ? 2U : 3U;
        unsigned di = ((H / 2U - 1U) * W + W / 2U - 1U) * bytes;
        s.depth_bits = x < 3U ? 16U : 24U; minigl_raster_set_state(&s);
        minigl_raster_clear(0x4100U, 0U, 0U);
        vertex(0, 0, 0, (float)(x % 3U) - 1.0f, 0xff00ffffU, 0, 0);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0xff00ffffU);
        assert(depth[di] == 0U && depth[di + 1U] == 0U && (bytes == 2U || depth[di + 2U] == 0U));
    }

    /* A triangle crossing the left homogeneous clip plane must still rasterize. */
    vertex(0, -2.0f, -0.7f, 0.0f, 0xff0000ffU, 0, 0);
    vertex(1, 0.8f, -0.7f, 0.0f, 0xff0000ffU, 0, 0);
    vertex(2, 0.0f, 0.8f, 0.0f, 0xff0000ffU, 0, 0);
    s = state(); minigl_raster_set_state(&s); minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) changed += pixel(x, y) != 0;
    assert(changed > 100U);

    /* Points, lines and quads use distinct assembly paths. */
    vertex(0, -0.5f, 0.0f, 0.0f, 0x00ff00ffU, 0, 0);
    vertex(1, -0.9f, -0.9f, 0.0f, 0x0000ffffU, 0, 0);
    vertex(2, 0.9f, 0.9f, 0.0f, 0x0000ffffU, 0, 0);
    vertex(3, -0.2f, -0.2f, 0.0f, 0xffffffffU, 0, 0);
    vertex(4, 0.2f, -0.2f, 0.0f, 0xffffffffU, 1, 0);
    vertex(5, 0.2f, 0.2f, 0.0f, 0xffffffffU, 1, 1);
    vertex(6, -0.2f, 0.2f, 0.0f, 0xffffffffU, 0, 1);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    minigl_raster_draw_arrays(GL_LINES, 0, VSTRIDE, 1, 2, 0);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 3, 4, 0);
    assert(pixel(W / 4U - 1U, H / 2U - 1U) == 0x00ff00ffU);
    assert(pixel(W / 2U, H / 2U) == 0xffffffffU);

    /* Scissor/color masks, 24-bit depth, alpha blending and linear fog compose per pixel. */
    s = state(); minigl_raster_set_state(&s); minigl_raster_clear(0x4000U, 0x11223344U, 0x00ffffffU);
    s.scissor_x = W / 2U; s.scissor_y = 0; s.scissor_w = W / 2U; s.scissor_h = H;
    s.enabled_flags = ZZ9K_MGL_STATE_SCISSOR_TEST; s.color_mask = 0x1U; minigl_raster_set_state(&s);
    vertex(0, -0.9f, -0.9f, -0.8f, 0xaabbccffU, 0, 0); vertex(1, 0.9f, -0.9f, -0.8f, 0xaabbccffU, 0, 0); vertex(2, 0, 0.9f, -0.8f, 0xaabbccffU, 0, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 4U, H / 2U) == 0x11223344U);
    assert((pixel(W / 2U, H / 2U) & 0xff000000U) == 0xaa000000U);
    /* 24-bit depth rejects a later, farther fragment and standard alpha blending mixes channels. */
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U | 0x0100U, 0, 0x00ffffffU);
    vertex(0, -0.8f, -0.8f, 0.6f, 0xff0000ffU, 0, 0); vertex(1, 0.8f, -0.8f, 0.6f, 0xff0000ffU, 0, 0); vertex(2, 0, 0.8f, 0.6f, 0xff0000ffU, 0, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    vertex(0, -0.8f, -0.8f, -0.6f, 0x00ff00ffU, 0, 0); vertex(1, 0.8f, -0.8f, -0.6f, 0x00ff00ffU, 0, 0); vertex(2, 0, 0.8f, -0.6f, 0x00ff00ffU, 0, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    vertex(0, -0.8f, -0.8f, 0.5f, 0x0000ffffU, 0, 0); vertex(1, 0.8f, -0.8f, 0.5f, 0x0000ffffU, 0, 0); vertex(2, 0, 0.8f, 0.5f, 0x0000ffffU, 0, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0x00ff00ffU);
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_BLEND; s.blend_src = GL_SRC_ALPHA; s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0x0000ffffU, 0x00ffffffU);
    vertex(0, -0.8f, -0.8f, 0, 0xff000080U, 0, 0); vertex(1, 0.8f, -0.8f, 0, 0xff000080U, 0, 0); vertex(2, 0, 0.8f, 0, 0xff000080U, 0, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert((pixel(W / 2U, H / 2U) & 0xff00ff00U) != 0U);

    /* A depth-only draw still honors alpha rejection when every color channel is masked. */
    for (x = 0; x < 2U; ++x) {
        s = state();
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
        minigl_raster_set_state(&s);
        minigl_raster_clear(0x4100U, 0x11223344U, 0x00ffffffU);
        s.color_mask = 0U;
        s.enabled_flags |= ZZ9K_MGL_STATE_ALPHA_TEST | ZZ9K_MGL_STATE_BLEND;
        s.alpha_func = x == 0U ? GL_NEVER : GL_ALWAYS;
        s.blend_src = GL_SRC_ALPHA; s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
        minigl_raster_set_state(&s);
        vertex(0, 0.0f, 0.0f, -0.6f, 0xff000080U, 0, 0);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x11223344U);
        s.color_mask = 0xfU;
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
        minigl_raster_set_state(&s);
        vertex(0, 0.0f, 0.0f, 0.6f, 0x0000ffffU, 0, 0);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) ==
               (x == 0U ? 0x0000ffffU : 0x11223344U));
        s.color_mask = 0U; minigl_raster_set_state(&s);
        minigl_raster_clear(0x4100U, 0xffffffffU, 0x00ffffffU);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) ==
               (x == 0U ? 0x0000ffffU : 0x11223344U));
        s.color_mask = 0xfU; minigl_raster_set_state(&s);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x0000ffffU);
    }

    /* ARGB1555 upload expands, and P8 upload palette expansion feeds bilinear TMU0. */
    put16(staging, 0xfc00U); put16(staging + 2, 0x801fU);
    minigl_raster_upload_texture(1, 0U, 2, 1, ZZ9K_MGL_TEXFMT_ARGB1555, 0, 4);
    minigl_raster_tex_params(1, GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT); minigl_raster_bind_texture(0, GL_TEXTURE_2D, 1);
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; minigl_raster_set_state(&s);
    vertex(0, -0.8f, -0.8f, 0, 0xffffffffU, 0, 0); vertex(1, 0.8f, -0.8f, 0, 0xffffffffU, 1, 0); vertex(2, 0, 0.8f, 0, 0xffffffffU, 0.5f, 0);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU); minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert((pixel(W / 2U, H / 2U) & 0xff0000ffU) != 0U);
    memset(staging, 0, 1024); put32(staging + 4, 0x00ff00ffU); staging[1024] = 1;
    minigl_raster_upload_texture(2, 0U, 1, 1, ZZ9K_MGL_TEXFMT_P8, 0, 1025); minigl_raster_bind_texture(0, GL_TEXTURE_2D, 2);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU); minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0x00ff00ffU);

    /* Draw assembly must accept staging-sized batches without index copies. */
    s = state(); minigl_raster_set_state(&s); minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    for (x = 0; x < 300U; ++x)
        vertex(x, 0.0f, 0.0f, 0.0f, 0x123456ffU, 0, 0);
    assert(minigl_raster_get_error() == 0U);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 300, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x123456ffU);
    assert(minigl_raster_get_error() == 0U);

    /* GL_LINES pairs only adjacent vertices; point and line widths cover their stated areas. */
    s = state(); s.point_size = 3; s.line_width = 3; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    vertex(0, -0.8f, 0.0f, 0.0f, 0xff0000ffU, 0, 0);
    vertex(1, -0.2f, 0.0f, 0.0f, 0xff0000ffU, 0, 0);
    vertex(2, 0.0f, -0.8f, 0.0f, 0x00ff00ffU, 0, 0);
    vertex(3, 0.0f, -0.2f, 0.0f, 0x00ff00ffU, 0, 0);
    minigl_raster_draw_arrays(GL_LINES, 0, VSTRIDE, 0, 4, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 3, 1, 0);
    assert(pixel(W / 4U, H / 2U) == 0xff0000ffU);
    assert(pixel(W / 2U, H / 2U + 5U) == 0x00ff00ffU);

    /* Polygon line mode leaves the interior untouched, while fill blends each shared edge once. */
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_BLEND; s.blend_src = GL_SRC_ALPHA;
    s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA; s.polygon_mode = GL_LINE; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    vertex(0, -0.8f, -0.8f, 0, 0xff000080U, 0, 0);
    vertex(1, 0.8f, -0.8f, 0, 0xff000080U, 0, 0);
    vertex(2, 0.0f, 0.8f, 0, 0xff000080U, 0, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0U);
    s.polygon_mode = GL_FILL; minigl_raster_set_state(&s);
    vertex(0, -0.8f, -0.8f, 0, 0xff000080U, 0, 0);
    vertex(1, 0.8f, -0.8f, 0, 0xff000080U, 0, 0);
    vertex(2, 0.8f, 0.8f, 0, 0xff000080U, 0, 0);
    vertex(3, -0.8f, 0.8f, 0, 0xff000080U, 0, 0);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
    assert((pixel(W / 2U, H / 2U) >> 24) >= 126U && (pixel(W / 2U, H / 2U) >> 24) <= 129U);

    /* Fog tracks absolute eye-space z, not post-projection window depth. */
    {
        float projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1};
        s = state(); s.enabled_flags = ZZ9K_MGL_STATE_FOG; s.fog_color = 0x0000ffffU;
        s.fog_start = 0.0f; s.fog_end = 4.0f; minigl_raster_set_state(&s);
        minigl_raster_load_matrix(1, projection); minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
        vertex(0, -0.7f, -0.7f, -2.0f, 0xff0000ffU, 0, 0);
        vertex(1, 0.7f, -0.7f, -2.0f, 0xff0000ffU, 0, 0);
        vertex(2, 0.0f, 0.7f, -2.0f, 0xff0000ffU, 0, 0);
        minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
        assert((pixel(W / 2U, H / 2U) >> 24) > 120U && (pixel(W / 2U, H / 2U) >> 24) < 135U);
        projection[10] = 0.4f; minigl_raster_load_matrix(1, projection);
        minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
        minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
        assert((pixel(W / 2U, H / 2U) >> 24) > 120U && (pixel(W / 2U, H / 2U) >> 24) < 135U);
        projection[10] = 1.0f; minigl_raster_load_matrix(1, projection);
    }

    /* A rejected gigantic upload cannot replace an already sampled texture. */
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; minigl_raster_set_state(&s);
    minigl_raster_upload_texture(2, 0U, UINT16_MAX, UINT16_MAX, ZZ9K_MGL_TEXFMT_RGBA8888, 0, 0);
    assert(minigl_raster_get_error() != 0U);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    vertex(0, -0.8f, -0.8f, 0, 0xffffffffU, 0, 0);
    vertex(1, 0.8f, -0.8f, 0, 0xffffffffU, 1, 0);
    vertex(2, 0.0f, 0.8f, 0, 0xffffffffU, 0.5f, 0);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0x00ff00ffU);

    /* Each on-board upload format expands to the exact stored RGBA value. */
    put32(staging, 0x11223344U);
    minigl_raster_upload_texture(3, 0U, 1, 1, ZZ9K_MGL_TEXFMT_RGBA8888, 0, 4);
    minigl_raster_bind_texture(0, GL_TEXTURE_2D, 3);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0x11223344U);
    put16(staging, 0xf800U);
    minigl_raster_upload_texture(4, 0U, 1, 1, ZZ9K_MGL_TEXFMT_RGB565, 0, 2);
    minigl_raster_bind_texture(0, GL_TEXTURE_2D, 4);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0xff0000ffU);
    put16(staging, 0x801fU);
    minigl_raster_upload_texture(5, 0U, 1, 1, ZZ9K_MGL_TEXFMT_ARGB1555, 0, 2);
    minigl_raster_bind_texture(0, GL_TEXTURE_2D, 5);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0x0000ffffU);

    /* Both TMUs modulate sequentially; orthogonal colors must become black. */
    put32(staging, 0x00ff00ffU);
    minigl_raster_upload_texture(6, 0U, 1, 1, ZZ9K_MGL_TEXFMT_RGBA8888, 0, 4);
    put32(staging, 0xff0000ffU);
    minigl_raster_upload_texture(7, 0U, 1, 1, ZZ9K_MGL_TEXFMT_RGBA8888, 0, 4);
    minigl_raster_bind_texture(0, GL_TEXTURE_2D, 6);
    minigl_raster_bind_texture(1, GL_TEXTURE_2D, 7);
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1;
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(W / 2U, H / 2U) == 0x000000ffU);

    /* Points on the top boundary are outside the half-open raster domain. */
    s = state(); minigl_raster_set_state(&s); minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    vertex(0, 0.0f, -1.0f, 0.0f, 0xabcdef01U, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U, H - 1U) == 0U);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    vertex(0, 0.0f, 1.0f, 0.0f, 0xabcdef01U, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U, 0U) == 0U);

    /* Fractional stitched triangles must blend their diagonal exactly once. */
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_BLEND; s.blend_src = GL_SRC_ALPHA;
    s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
    vertex(0, -0.73f, -0.61f, 0, 0xff000080U, 0, 0);
    vertex(1, 0.68f, -0.47f, 0, 0xff000080U, 0, 0);
    vertex(2, 0.57f, 0.72f, 0, 0xff000080U, 0, 0);
    vertex(3, -0.64f, 0.53f, 0, 0xff000080U, 0, 0);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
    changed = 0U;
    for (y = 8U; y < H - 8U; ++y) for (x = 8U; x < W - 8U; ++x) {
        uint32_t value = pixel(x, y);
        if (value != 0U) { uint32_t r = value >> 24; if (r < 126U || r > 129U) fprintf(stderr, "double or crack at (%u,%u) val=%08x\n", x, y, value); assert(r >= 126U && r <= 129U); ++changed; }
    }
    assert(changed > 300U);

    /* 64-byte vertex stride: texture matrix R column contributes to S. */
    {
        static const float tex_mat[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.5f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        uint8_t *v64 = vertices;
        put32(staging, 0xff0000ffU);
        put32(staging + 4U, 0x0000ffffU);
        minigl_raster_upload_texture(8, 0U, 2, 1, ZZ9K_MGL_TEXFMT_RGBA8888, 0, 8);
        minigl_raster_bind_texture(0, GL_TEXTURE_2D, 8);
        minigl_raster_tex_params(8U, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
        minigl_raster_load_matrix(2, tex_mat);
        s = state(); s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; minigl_raster_set_state(&s);
        minigl_raster_clear(0x4000U, 0, 0x00ffffffU);
        memset(v64, 0, 64);
        putfloat(v64, 0.0f); putfloat(v64 + 4, 0.0f); putfloat(v64 + 8, 0.0f);
        put32(v64 + 12, 0xffffffffU);
        putfloat(v64 + 16, 0.25f); putfloat(v64 + 20, 0.5f);
        putfloat(v64 + 24, 0.0f);  putfloat(v64 + 28, 0.0f);
        putfloat(v64 + 32, 0.0f);  putfloat(v64 + 36, 0.0f); putfloat(v64 + 40, 1.0f);
        putfloat(v64 + 44, 1.0f);
        putfloat(v64 + 48, 1.0f); putfloat(v64 + 52, 1.0f);
        putfloat(v64 + 56, 1.0f); putfloat(v64 + 60, 0.0f);
        minigl_raster_draw_arrays(GL_POINTS, 0, 64U, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x0000ffffU);
        {
            float ident[16];
            memset(ident, 0, sizeof(ident));
            ident[0] = ident[5] = ident[10] = ident[15] = 1.0f;
            minigl_raster_load_matrix(2, ident);
        }
    }

    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    s.depth_offset = 0.2f;
    minigl_raster_set_state(&s); minigl_raster_clear(0x4100U, 0U, 0x00999999U);
    vertex(0, 0.0f, 0.0f, 0.0f, 0xff0000ffU, 0, 0);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0U);
    s.depth_offset = -0.2f; minigl_raster_set_state(&s);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0xff0000ffU);
    s.depth_offset = 2.0f; s.depth_func = GL_LEQUAL;
    minigl_raster_set_state(&s); minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0xff0000ffU);
    s = state(); s.min_triangle_area = 20.0f; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0U, 0U);
    vertex(0, -0.1f, -0.1f, 0, 0xff0000ffU, 0, 0);
    vertex(1, 0.1f, -0.1f, 0, 0xff0000ffU, 0, 0);
    vertex(2, 0.1f, 0.1f, 0, 0xff0000ffU, 0, 0);
    vertex(3, -0.1f, 0.1f, 0, 0xff0000ffU, 0, 0);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
    assert(pixel(W / 2U, H / 2U) != 0U);
    s.min_triangle_area = 40.0f; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0U, 0U);
    minigl_raster_draw_arrays(GL_QUADS, 0, VSTRIDE, 0, 4, 0);
    assert(pixel(W / 2U, H / 2U) == 0U);
    minigl_raster_reset();
    mip_level(0U, 0U, 1U, 1U, 0x5a6b7cffU);
    for (x = 0U; x < ZZ9K_MINIGL_MAX_TEXTURES; ++x) {
        put32(staging, ((x & 255U) << 24) | 0x00aabbffU);
        minigl_raster_upload_texture(10000U + x, 0U, 1U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 4U);
    }
    assert(minigl_raster_get_error() == 0U);
    s = state(); s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0; minigl_raster_set_state(&s);
    vertex(0, 0.0f, 0.0f, 0, 0xffffffffU, 0, 0);
    for (x = 0U; x < ZZ9K_MINIGL_MAX_TEXTURES; ++x) {
        minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 10000U + x);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == (((x & 255U) << 24) | 0x00aabbffU));
    }
    for (x = 0U; x < ZZ9K_MINIGL_MAX_TEXTURES; x += 2U)
        minigl_raster_delete_texture(10000U + x);
    for (x = 0U; x < ZZ9K_MINIGL_MAX_TEXTURES; x += 2U) {
        put32(staging, 0x123456ffU);
        minigl_raster_upload_texture(20000U + x, 0U, 1U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 4U);
        minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 20000U + x);
        minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
        assert(pixel(W / 2U - 1U, H / 2U - 1U) == 0x123456ffU);
    }
    assert(minigl_raster_get_error() == 0U);
    minigl_raster_reset();
    put32(staging, 0xff0000ffU); put32(staging + 4U, 0x0000ffffU);
    minigl_raster_upload_texture(1U, 0U, 2U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 8U);
    minigl_raster_bind_texture(0U, GL_TEXTURE_2D, 1U);
    minigl_raster_tex_params(1U, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    put32(staging, 0xffffffffU); put32(staging + 4U, 0x00ff00ffU);
    minigl_raster_upload_texture(2U, 0U, 2U, 1U, ZZ9K_MGL_TEXFMT_RGBA8888, 0U, 8U);
    minigl_raster_bind_texture(1U, GL_TEXTURE_2D, 2U);
    minigl_raster_tex_params(2U, GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
    s = state(); s.shade_model = GL_FLAT;
    s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1;
    minigl_raster_set_state(&s); minigl_raster_clear(0x4000U, 0U, 0U);
    vertex(0U, -0.8f, -0.8f, 0, 0xffffffffU, 0, 0);
    vertex(1U, 1.6f, -1.6f, 0, 0xffffffffU, 1, 0);
    vertex(2U, 0, 0.8f, 0, 0xffffffffU, 0, 0);
    putfloat(vertices + VSTRIDE + 44U, 2.0f);
    putfloat(vertices + VSTRIDE + 48U, 2.0f);
    putfloat(vertices + VSTRIDE + 52U, 4.0f);
    putfloat(vertices + VSTRIDE + 24U, 2.0f);
    minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, 0, 3, 0);
    assert(pixel(50U, 38U) == 0xff0000ffU);
    assert_scanline_coverage();
    assert_point_half_open_coverage();
    assert_large_lowered_outline();
    assert_fragment_extensions();
    assert_mipmaps();
    assert_quake_charset_incomplete_mipmaps();
    assert_default_texture();
    assert_sampler_defaults();
    assert_integer_vertices();
    assert_texture_compaction();
    assert_rix_ownership_transitions();
    assert_fragmented_pool_growth();
    minigl_raster_release_buffers();
    assert(minigl_raster_color_buffer() == NULL);
    assert(minigl_raster_width() == 0U && minigl_raster_height() == 0U);
    minigl_raster_draw_arrays(GL_POINTS, 0, VSTRIDE, 0, 1, 0);
    assert(minigl_raster_get_error() != 0U);
    {
        MiniGLRasterBuffers buffers = {
            color, sizeof(color), depth, sizeof(depth), vertices, sizeof(vertices),
            staging, sizeof(staging), textures, sizeof(textures), W, H
        };
        assert(minigl_raster_configure_buffers(&buffers));
        minigl_raster_init();
    }
    puts("MiniGL raster behavioral assertions passed");
    return 0;
}
