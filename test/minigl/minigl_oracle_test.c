/* Deterministic reference vs candidate oracle test for MiniGL rasterizer.
 * Verifies the accuracy contract:
 * - Depth values written and coverage must be BIT-EXACT vs frozen reference.
 * - Color max absolute difference <= 2 per channel (rare outliers <= 0.05% of pixels).
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "minigl_raster_ref.h"
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_raster.h"
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_lighting.h"
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_present.h"
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/memorymap.h"

#define W 320U
#define H 240U
#define VSTRIDE MINIGL_RASTER_VERTEX_STRIDE
#define VERTS 16384U
#define TEX 64U
#define HUD_TEX 32U

#define GL_TRIANGLES 0x0004U
#define GL_TRIANGLE_FAN 0x0006U
#define GL_POLYGON 0x0009U
#define GL_TEXTURE_2D 0x0DE1U
#define GL_LEQUAL 0x0203U
#define GL_GREATER 0x0204U
#define GL_NEAREST 0x2600U
#define GL_LINEAR 0x2601U
#define GL_MODULATE 0x2100U
#define GL_REPLACE 0x1E01U
#define GL_REPEAT 0x2901U
#define GL_CLAMP 0x2900U
#define GL_FLAT 0x1D00U
#define GL_SMOOTH 0x1D01U
#define GL_FILL 0x1B02U
#define GL_SRC_COLOR 0x0300U
#define GL_SRC_ALPHA 0x0302U
#define GL_ONE_MINUS_SRC_ALPHA 0x0303U
#define GL_ZERO 0U
#define GL_ONE 1U
#define GL_COLOR_BUFFER_BIT 0x00004000U
#define GL_DEPTH_BUFFER_BIT 0x00000100U
#define GL_BACK 0x0405U
#define GL_CCW 0x0901U
#define GL_FRONT 0x0404U

#define CLEAR_RGBA 0x0b0d12ffU
#define DEPTH_FAR 0x00ffffffU
#define TEX_DIFFUSE 1U
#define TEX_LIGHT_RGBA 2U
#define TEX_HUD 3U
#define TEX_LIGHT_1555 4U
#define TEX_LIGHT_LUM 5U
#define TEX_MIPMAP 6U
#define GL_LINEAR_MIPMAP_NEAREST  0x2701U

static uint32_t g_cand_color[W * H];
static uint8_t  g_cand_depth[W * H * 3U];
static uint32_t g_ref_color[W * H];
static uint8_t  g_ref_depth[W * H * 3U];

static uint8_t g_vertices[VERTS * VSTRIDE];
static uint8_t g_staging[TEX * TEX * 4U];
static uint8_t g_cand_pool[512U * 1024U];
static uint8_t g_ref_pool[512U * 1024U];

static unsigned g_world_verts;
static unsigned g_alias_first, g_alias_count;
static unsigned g_hud_first, g_hud_count;
static unsigned g_add_first, g_add_count;
static unsigned g_lit_first, g_lit_count, g_lit_batches, g_lit_batch;
static unsigned g_gear_first[3], g_gear_count[3];
static unsigned g_mip_first;

static void put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}

static void putfloat(uint8_t *p, float f)
{
    union { float f; uint32_t u; } n;
    n.f = f;
    put32(p, n.u);
}

static void vertex_at(unsigned n, float x, float y, float z, uint32_t rgba,
                      float u0, float v0, float u1, float v1,
                      float nx, float ny, float nz, float q)
{
    uint8_t *p = g_vertices + (size_t)n * VSTRIDE;
    memset(p, 0, VSTRIDE);
    putfloat(p, x);
    putfloat(p + 4, y);
    putfloat(p + 8, z);
    put32(p + 12, rgba);
    putfloat(p + 16, u0);
    putfloat(p + 20, v0);
    putfloat(p + 24, u1);
    putfloat(p + 28, v1);
    putfloat(p + 32, nx);
    putfloat(p + 36, ny);
    putfloat(p + 40, nz);
    putfloat(p + 44, q);
    putfloat(p + 48, q);
    putfloat(p + 52, 1.0f);
}

static void quad(unsigned base, float x0, float y0, float x1, float y1, float z,
                 uint32_t c, float u0, float v0, float u1, float v1,
                 float lu0, float lv0, float lu1, float lv1)
{
    vertex_at(base + 0U, x0, y0, z, c, u0, v0, lu0, lv0, 0, 0, 1.0f, 1.0f);
    vertex_at(base + 1U, x1, y0, z, c, u1, v0, lu1, lv0, 0, 0, 1.0f, 1.0f);
    vertex_at(base + 2U, x1, y1, z, c, u1, v1, lu1, lv1, 0, 0, 1.0f, 1.0f);
    vertex_at(base + 3U, x0, y1, z, c, u0, v1, lu0, lv1, 0, 0, 1.0f, 1.0f);
}

static ZZ9KMiniGLPacketSetState base_state(void)
{
    ZZ9KMiniGLPacketSetState s;
    memset(&s, 0, sizeof(s));
    s.depth_bits = 24;
    s.depth_func = GL_LEQUAL;
    s.depth_near = 0;
    s.depth_far = DEPTH_FAR;
    s.viewport_w = (uint16_t)W;
    s.viewport_h = (uint16_t)H;
    s.color_mask = 0xfU;
    s.polygon_mode = GL_FILL;
    s.cull_face_mode = GL_BACK;
    s.front_face = GL_CCW;
    s.shade_model = GL_SMOOTH;
    s.point_size = 1;
    s.line_width = 1;
    s.tex_env_mode[0] = GL_MODULATE;
    s.tex_env_mode[1] = GL_MODULATE;
    s.blend_src = GL_ONE;
    s.blend_dst = GL_ZERO;
    s.blend_src_alpha = GL_ONE;
    s.blend_dst_alpha = GL_ZERO;
    s.blend_equation = ZZ9K_MGL_WIRE_FUNC_ADD;
    return s;
}

static void identity(float m[16])
{
    memset(m, 0, 16U * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mul_mat(float out[16], const float a[16], const float b[16])
{
    float t[16];
    int r, c, k;
    for (r = 0; r < 4; ++r) {
        for (c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (k = 0; k < 4; ++k)
                sum += a[r + k * 4] * b[k + c * 4];
            t[r + c * 4] = sum;
        }
    }
    memcpy(out, t, sizeof(t));
}

static void load_frustum_to(int is_ref)
{
    float m[16];
    float l = -0.55f, r = 0.55f, b = -0.4125f, t = 0.4125f, n = 1.0f, f = 100.0f;
    memset(m, 0, sizeof(m));
    m[0] = (2.0f * n) / (r - l);
    m[5] = (2.0f * n) / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -(2.0f * f * n) / (f - n);
    if (is_ref)
        ref_minigl_raster_load_matrix(1, m);
    else
        minigl_raster_load_matrix(1, m);
}

static void gear_modelview_to(int is_ref, float angle, float tx, float ty, float tz)
{
    float root[16], spin[16], m[16];
    float c = cosf(angle), s = sinf(angle);
    identity(root);
    root[12] = tx; root[13] = ty; root[14] = tz;
    identity(spin);
    spin[0] = c; spin[1] = s;
    spin[4] = -s; spin[5] = c;
    mul_mat(m, root, spin);
    if (is_ref)
        ref_minigl_raster_load_matrix(0, m);
    else
        minigl_raster_load_matrix(0, m);
}

static void load_identities_to(int is_ref)
{
    float m[16];
    identity(m);
    if (is_ref) {
        ref_minigl_raster_load_matrix(0, m);
        ref_minigl_raster_load_matrix(1, m);
        ref_minigl_raster_load_matrix(2, m);
        ref_minigl_raster_load_matrix(3, m);
    } else {
        minigl_raster_load_matrix(0, m);
        minigl_raster_load_matrix(1, m);
        minigl_raster_load_matrix(2, m);
        minigl_raster_load_matrix(3, m);
    }
}

static void build_scene_geometry(void)
{
    unsigned base = 0;
    int col, row;
    /* World */
    for (row = 0; row < 12; ++row) {
        for (col = 0; col < 16; ++col) {
            float x0 = -1.0f + (float)col * (2.0f / 16.0f);
            float x1 = x0 + (2.0f / 16.0f);
            float y0 = -1.0f + (float)row * (2.0f / 12.0f);
            float y1 = y0 + (2.0f / 12.0f);
            float z  = 0.15f + 0.65f * (float)(col + row) / 28.0f;
            quad(base, x0, y0, x1, y1, z, 0xffffffffU,
                 (float)col * 0.25f, (float)row * 0.25f,
                 (float)(col + 1) * 0.25f, (float)(row + 1) * 0.25f,
                 (float)col / 16.0f, (float)row / 12.0f,
                 (float)(col + 1) / 16.0f, (float)(row + 1) / 12.0f);
            base += 4U;
        }
    }
    g_world_verts = base;

    /* Alias */
    g_alias_first = base;
    for (row = 0; row < 8; ++row) {
        for (col = 0; col < 8; ++col) {
            float cx = -0.7f + (float)col * 0.2f;
            float cy = -0.7f + (float)row * 0.2f;
            float z  = 0.35f + 0.3f * (float)(row ^ col) / 8.0f;
            vertex_at(base + 0U, cx, cy + 0.08f, z, 0xffd090ffU, 0.5f, 0.1f, 0, 0, 0, 0, 1, 1);
            vertex_at(base + 1U, cx - 0.07f, cy - 0.06f, z, 0x90ffd0ffU, 0.1f, 0.9f, 0, 0, 0, 0, 1, 1);
            vertex_at(base + 2U, cx + 0.07f, cy - 0.06f, z, 0xd090ffffU, 0.9f, 0.9f, 0, 0, 0, 0, 1, 1);
            base += 3U;
        }
    }
    g_alias_count = base - g_alias_first;

    /* HUD */
    g_hud_first = base;
    for (col = 0; col < 8; ++col) {
        float x0 = -0.9f + (float)col * 0.22f;
        float x1 = x0 + 0.18f;
        float y0 = 0.70f;
        float y1 = 0.92f;
        quad(base, x0, y0, x1, y1, 0.0f, 0xffffffffU,
             (float)col / 8.0f, 0.0f, (float)(col + 1) / 8.0f, 1.0f,
             0, 0, 0, 0);
        base += 4U;
    }
    g_hud_count = base - g_hud_first;

    /* Additive */
    g_add_first = base;
    {
        unsigned p;
        for (p = 0; p < 6U; ++p) {
            float cx = -0.6f + (float)p * 0.24f;
            float cy = -0.2f + 0.15f * (float)(p & 1U);
            float z = 0.2f;
            unsigned i;
            vertex_at(base, cx, cy, z, 0xffffffffU, 0.5f, 0.5f, 0, 0, 0, 0, 1, 1);
            for (i = 0; i < 8U; ++i) {
                float a = (float)i * (2.0f * 3.14159265f / 8.0f);
                float r = 0.12f;
                vertex_at(base + 1U + i, cx + cosf(a) * r, cy + sinf(a) * r, z,
                          0xff602000U, 0.5f + cosf(a) * 0.5f, 0.5f + sinf(a) * 0.5f,
                          0, 0, 0, 0, 1, 1);
            }
            base += 9U;
        }
    }
    g_add_count = base - g_add_first;

    /* Lit */
    g_lit_first = base;
    g_lit_batches = 4U;
    g_lit_batch = 12U;
    {
        unsigned b, i;
        for (b = 0; b < g_lit_batches; ++b) {
            for (i = 0; i < g_lit_batch; ++i) {
                float x = -0.6f + (float)i * 0.11f;
                float y = -0.4f + (float)b * 0.25f;
                float z = 0.5f;
                vertex_at(base + 0U, x, y + 0.08f, z, 0xffffffffU, 0, 0, 0, 0, 0, 0, 1, 1);
                vertex_at(base + 1U, x - 0.05f, y - 0.05f, z, 0xffffffffU, 0, 0, 0, 0, 0, 0, 1, 1);
                vertex_at(base + 2U, x + 0.05f, y - 0.05f, z, 0xffffffffU, 0, 0, 0, 0, 0, 0, 1, 1);
                base += 3U;
            }
        }
    }
    g_lit_count = base - g_lit_first;

    /* Gears */
    {
        unsigned g;
        for (g = 0; g < 3U; ++g) {
            unsigned teeth = 10U + g * 4U;
            float r0 = 0.6f + (float)g * 0.2f;
            float r1 = r0 + 0.3f;
            unsigned t;
            g_gear_first[g] = base;
            for (t = 0; t < teeth; ++t) {
                float a0 = (float)t * 2.0f * 3.14159265f / (float)teeth;
                float a1 = a0 + 3.14159265f / (float)teeth;
                vertex_at(base + 0U, cosf(a0) * r0, sinf(a0) * r0, 0.0f, 0xffffffffU, 0, 0, 0, 0, 0, 0, 1, 1);
                vertex_at(base + 1U, cosf(a0) * r1, sinf(a0) * r1, 0.0f, 0xffffffffU, 0, 0, 0, 0, 0, 0, 1, 1);
                vertex_at(base + 2U, cosf(a1) * r0, sinf(a1) * r0, 0.0f, 0xffffffffU, 0, 0, 0, 0, 0, 0, 1, 1);
                base += 3U;
            }
            g_gear_count[g] = base - g_gear_first[g];
        }
    }

    /* Mip quad */
    g_mip_first = base;
    quad(base, -0.90f, -0.80f, 0.90f, -0.20f, 0.10f, 0xffffffffU, 0, 0, 4.0f, 4.0f, 0, 0, 0, 0);
    base += 4U;
    quad(base, -0.90f,  0.20f, 0.90f,  0.80f, 0.90f, 0xffffffffU, 0, 0, 64.0f, 64.0f, 0, 0, 0, 0);
    base += 4U;
}

static void upload_textures_to(int is_ref)
{
    uint8_t rgb[TEX * TEX * 3U];
    uint32_t rgba[TEX * TEX];
    uint16_t argb1555[TEX * TEX];
    uint8_t lum[TEX * TEX];
    uint32_t hud[HUD_TEX * HUD_TEX];
    unsigned x, y;

    /* Diffuse checker */
    for (y = 0; y < TEX; ++y) {
        for (x = 0; x < TEX; ++x) {
            int c = ((x >> 3) ^ (y >> 3)) & 1;
            rgb[(y * TEX + x) * 3 + 0] = c ? 220 : 40;
            rgb[(y * TEX + x) * 3 + 1] = c ? 200 : 70;
            rgb[(y * TEX + x) * 3 + 2] = c ? 160 : 120;
        }
    }
    memcpy(g_staging, rgb, sizeof(rgb));
    if (is_ref) {
        ref_minigl_raster_upload_texture(TEX_DIFFUSE, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_RGB888, 0, sizeof(rgb));
        ref_minigl_raster_tex_params(TEX_DIFFUSE, GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    } else {
        minigl_raster_upload_texture(TEX_DIFFUSE, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_RGB888, 0, sizeof(rgb));
        minigl_raster_tex_params(TEX_DIFFUSE, GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    }

    /* Lightmap RGBA */
    for (y = 0; y < TEX; ++y) {
        for (x = 0; x < TEX; ++x) {
            float dx = (float)x - 31.5f, dy = (float)y - 31.5f;
            float r = sqrtf(dx * dx + dy * dy) / 32.0f;
            uint8_t v = (uint8_t)(r >= 1.0f ? 20.0f : (1.0f - r) * 235.0f + 20.0f);
            rgba[y * TEX + x] = ((uint32_t)v << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | 255U;
            argb1555[y * TEX + x] = (uint16_t)(0x8000U | ((v >> 3) << 10) | ((v >> 3) << 5) | (v >> 3));
            lum[y * TEX + x] = v;
        }
    }
    memcpy(g_staging, rgba, sizeof(rgba));
    if (is_ref) {
        ref_minigl_raster_upload_texture(TEX_LIGHT_RGBA, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_RGBA8888, 0, sizeof(rgba));
        ref_minigl_raster_tex_params(TEX_LIGHT_RGBA, GL_LINEAR, GL_LINEAR, GL_CLAMP, GL_CLAMP);
    } else {
        minigl_raster_upload_texture(TEX_LIGHT_RGBA, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_RGBA8888, 0, sizeof(rgba));
        minigl_raster_tex_params(TEX_LIGHT_RGBA, GL_LINEAR, GL_LINEAR, GL_CLAMP, GL_CLAMP);
    }

    memcpy(g_staging, argb1555, sizeof(argb1555));
    if (is_ref) {
        ref_minigl_raster_upload_texture(TEX_LIGHT_1555, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_ARGB1555, 0, sizeof(argb1555));
        ref_minigl_raster_tex_params(TEX_LIGHT_1555, GL_LINEAR, GL_LINEAR, GL_CLAMP, GL_CLAMP);
    } else {
        minigl_raster_upload_texture(TEX_LIGHT_1555, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_ARGB1555, 0, sizeof(argb1555));
        minigl_raster_tex_params(TEX_LIGHT_1555, GL_LINEAR, GL_LINEAR, GL_CLAMP, GL_CLAMP);
    }

    memcpy(g_staging, lum, sizeof(lum));
    if (is_ref) {
        ref_minigl_raster_upload_texture(TEX_LIGHT_LUM, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_LUM8, 0, sizeof(lum));
        ref_minigl_raster_tex_params(TEX_LIGHT_LUM, GL_LINEAR, GL_LINEAR, GL_CLAMP, GL_CLAMP);
    } else {
        minigl_raster_upload_texture(TEX_LIGHT_LUM, 0U, TEX, TEX, ZZ9K_MGL_TEXFMT_LUM8, 0, sizeof(lum));
        minigl_raster_tex_params(TEX_LIGHT_LUM, GL_LINEAR, GL_LINEAR, GL_CLAMP, GL_CLAMP);
    }

    /* HUD */
    for (y = 0; y < HUD_TEX; ++y) {
        for (x = 0; x < HUD_TEX; ++x) {
            int border = (x < 2 || x >= HUD_TEX - 2 || y < 2 || y >= HUD_TEX - 2);
            hud[y * HUD_TEX + x] = border ? 0xffffffffU : 0x00000000U;
        }
    }
    memcpy(g_staging, hud, sizeof(hud));
    if (is_ref) {
        ref_minigl_raster_upload_texture(TEX_HUD, 0U, HUD_TEX, HUD_TEX, ZZ9K_MGL_TEXFMT_RGBA8888, 0, sizeof(hud));
        ref_minigl_raster_tex_params(TEX_HUD, GL_NEAREST, GL_NEAREST, GL_CLAMP, GL_CLAMP);
    } else {
        minigl_raster_upload_texture(TEX_HUD, 0U, HUD_TEX, HUD_TEX, ZZ9K_MGL_TEXFMT_RGBA8888, 0, sizeof(hud));
        minigl_raster_tex_params(TEX_HUD, GL_NEAREST, GL_NEAREST, GL_CLAMP, GL_CLAMP);
    }

    /* Mipmaps */
    {
        static const uint32_t colors[4] = {
            0xff0000ffU, 0x00ff00ffU, 0x0000ffffU, 0xffff00ffU
        };
        unsigned level;
        for (level = 0; level < 4U; ++level) {
            unsigned dim = 64U >> level;
            unsigned texels = dim * dim;
            unsigned i;
            for (i = 0; i < texels; ++i)
                put32(g_staging + i * 4U, colors[level]);
            if (is_ref)
                ref_minigl_raster_upload_texture(TEX_MIPMAP, (uint16_t)level, (uint16_t)dim, (uint16_t)dim,
                                                 ZZ9K_MGL_TEXFMT_RGBA8888, 0, texels * 4U);
            else
                minigl_raster_upload_texture(TEX_MIPMAP, (uint16_t)level, (uint16_t)dim, (uint16_t)dim,
                                             ZZ9K_MGL_TEXFMT_RGBA8888, 0, texels * 4U);
        }
        if (is_ref)
            ref_minigl_raster_tex_params(TEX_MIPMAP, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR, GL_REPEAT, GL_REPEAT);
        else
            minigl_raster_tex_params(TEX_MIPMAP, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR, GL_REPEAT, GL_REPEAT);
    }
}

static void run_scenario(const char *name, int is_ref)
{
    ZZ9KMiniGLPacketSetState s = base_state();
    load_identities_to(is_ref);

    if (strcmp(name, "clear") == 0) {
        if (is_ref) {
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
        } else {
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
        }
    } else if (strcmp(name, "world_opaque") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_TEXTURE_2D_0;
        s.tex_env_mode[0] = GL_REPLACE;
        s.shade_model = GL_FLAT;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        }
    } else if (strcmp(name, "lightmap_multitex") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE |
                          ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1;
        s.tex_env_mode[0] = GL_REPLACE;
        s.tex_env_mode[1] = GL_MODULATE;
        s.shade_model = GL_FLAT;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            ref_minigl_raster_bind_texture(1, GL_TEXTURE_2D, TEX_LIGHT_RGBA);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            minigl_raster_bind_texture(1, GL_TEXTURE_2D, TEX_LIGHT_RGBA);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        }
    } else if (strcmp(name, "lightmap_blend_4444") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_TEXTURE_2D_0;
        s.tex_env_mode[0] = GL_REPLACE;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
            s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_BLEND;
            s.blend_src = GL_SRC_ALPHA;
            s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
            s.tex_env_mode[0] = GL_MODULATE;
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_LIGHT_RGBA);
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
            s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_BLEND;
            s.blend_src = GL_SRC_ALPHA;
            s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
            s.tex_env_mode[0] = GL_MODULATE;
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_LIGHT_RGBA);
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        }
    } else if (strcmp(name, "lightmap_blend_1555") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_TEXTURE_2D_0;
        s.tex_env_mode[0] = GL_REPLACE;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
            s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_BLEND;
            s.blend_src = GL_SRC_ALPHA;
            s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
            s.tex_env_mode[0] = GL_MODULATE;
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_LIGHT_1555);
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
            s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_BLEND;
            s.blend_src = GL_SRC_ALPHA;
            s.blend_dst = GL_ONE_MINUS_SRC_ALPHA; s.blend_src_alpha = GL_SRC_ALPHA; s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
            s.tex_env_mode[0] = GL_MODULATE;
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_LIGHT_1555);
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        }
    } else if (strcmp(name, "lightmap_blend_lum") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_TEXTURE_2D_0;
        s.tex_env_mode[0] = GL_REPLACE;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
            s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_BLEND;
            s.blend_src = GL_ZERO;
            s.blend_dst = GL_SRC_COLOR; s.blend_src_alpha = GL_ZERO; s.blend_dst_alpha = GL_SRC_COLOR;
            s.tex_env_mode[0] = GL_REPLACE;
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_LIGHT_LUM);
            for (n = 0; n < g_world_verts; n += 4U)
                ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned n;
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
            s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_BLEND;
            s.blend_src = GL_ZERO;
            s.blend_dst = GL_SRC_COLOR; s.blend_src_alpha = GL_ZERO; s.blend_dst_alpha = GL_SRC_COLOR;
            s.tex_env_mode[0] = GL_REPLACE;
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_LIGHT_LUM);
            for (n = 0; n < g_world_verts; n += 4U)
                minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, n, 4, 0);
        }
    } else if (strcmp(name, "alias_tris") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_TEXTURE_2D_0;
        s.tex_env_mode[0] = GL_MODULATE;
        s.shade_model = GL_FLAT;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            ref_minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, g_alias_first, g_alias_count, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_DIFFUSE);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, g_alias_first, g_alias_count, 0);
        }
    } else if (strcmp(name, "hud_fan") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_ALPHA_TEST;
        s.tex_env_mode[0] = GL_REPLACE;
        s.alpha_func = GL_GREATER;
        s.alpha_ref = 32768U;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_HUD);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned i;
            for (i = 0; i < 8U; ++i)
                ref_minigl_raster_draw_arrays(GL_TRIANGLE_FAN, 0, VSTRIDE, g_hud_first + i * 4U, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_HUD);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned i;
            for (i = 0; i < 8U; ++i)
                minigl_raster_draw_arrays(GL_TRIANGLE_FAN, 0, VSTRIDE, g_hud_first + i * 4U, 4, 0);
        }
    } else if (strcmp(name, "additive") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_BLEND;
        s.blend_src = GL_ONE;
        s.blend_dst = GL_ONE; s.blend_src_alpha = GL_ONE; s.blend_dst_alpha = GL_ONE;
        s.shade_model = GL_SMOOTH;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned i;
            for (i = 0; i < 6U; ++i)
                ref_minigl_raster_draw_arrays(GL_TRIANGLE_FAN, 0, VSTRIDE, g_add_first + i * 9U, 9, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            unsigned i;
            for (i = 0; i < 6U; ++i)
                minigl_raster_draw_arrays(GL_TRIANGLE_FAN, 0, VSTRIDE, g_add_first + i * 9U, 9, 0);
        }
    } else if (strcmp(name, "quake_mipmapped") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_TEXTURE_2D_0;
        s.tex_env_mode[0] = GL_REPLACE;
        s.shade_model = GL_FLAT;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_MIPMAP);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, g_mip_first, 4, 0);
            ref_minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, g_mip_first + 4, 4, 0);
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_bind_texture(0, GL_TEXTURE_2D, TEX_MIPMAP);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, g_mip_first, 4, 0);
            minigl_raster_draw_arrays(GL_POLYGON, 0, VSTRIDE, g_mip_first + 4, 4, 0);
        }
    } else if (strcmp(name, "gears_positional") == 0) {
        s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE | ZZ9K_MGL_STATE_CULL_FACE;
        s.shade_model = GL_SMOOTH;
        static const float tx[3] = { -2.35f, 2.05f, -0.15f };
        static const float ty[3] = { -0.50f, 0.65f, 2.00f };
        static const float tz[3] = { -8.0f, -8.0f, -8.30f };
        static const float scale_spin[3] = { 1.0f, -2.0f, 3.0f };
        unsigned gear;
        if (is_ref) {
            ref_minigl_raster_set_state(&s);
            ref_minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            load_frustum_to(1);
            for (gear = 0; gear < 3U; ++gear) {
                gear_modelview_to(1, scale_spin[gear] * 0.02f, tx[gear], ty[gear], tz[gear]);
                ref_minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, g_gear_first[gear], g_gear_count[gear], 0);
            }
        } else {
            minigl_raster_set_state(&s);
            minigl_raster_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, CLEAR_RGBA, DEPTH_FAR);
            load_frustum_to(0);
            for (gear = 0; gear < 3U; ++gear) {
                gear_modelview_to(0, scale_spin[gear] * 0.02f, tx[gear], ty[gear], tz[gear]);
                minigl_raster_draw_arrays(GL_TRIANGLES, 0, VSTRIDE, g_gear_first[gear], g_gear_count[gear], 0);
            }
        }
    }
}

int main(void)
{
    static const char *scenarios[] = {
        "clear",
        "world_opaque",
        "lightmap_multitex",
        "lightmap_blend_4444",
        "lightmap_blend_1555",
        "lightmap_blend_lum",
        "alias_tris",
        "hud_fan",
        "additive",
        "quake_mipmapped",
        "gears_positional"
    };
    const size_t num_scenarios = sizeof(scenarios) / sizeof(scenarios[0]);
    size_t s_idx;

    MiniGLRasterBuffers cand_buf = {
        g_cand_color, sizeof(g_cand_color),
        g_cand_depth, sizeof(g_cand_depth),
        g_vertices, sizeof(g_vertices),
        g_staging, sizeof(g_staging),
        g_cand_pool, sizeof(g_cand_pool),
        (uint16_t)W, (uint16_t)H
    };
    MiniGLRasterBuffers ref_buf = {
        g_ref_color, sizeof(g_ref_color),
        g_ref_depth, sizeof(g_ref_depth),
        g_vertices, sizeof(g_vertices),
        g_staging, sizeof(g_staging),
        g_ref_pool, sizeof(g_ref_pool),
        (uint16_t)W, (uint16_t)H
    };

    build_scene_geometry();

    printf("=== MiniGL Rasterizer Oracle Verification against Frozen Baseline ===\n");
    printf("%-22s | %-11s | %-12s | %-14s | %-8s\n",
           "Scenario", "Depth Exact", "Max Diff", "Pixels > 2/255", "Status");
    printf("-----------------------------------------------------------------------------\n");

    int all_ok = 1;

    for (s_idx = 0; s_idx < num_scenarios; ++s_idx) {
        const char *name = scenarios[s_idx];

        /* Configure candidate */
        minigl_raster_configure_buffers(&cand_buf);
        minigl_raster_init();
        minigl_raster_configure_buffers(&cand_buf);
        upload_textures_to(0);

        /* Configure reference */
        ref_minigl_raster_configure_buffers(&ref_buf);
        ref_minigl_raster_init();
        ref_minigl_raster_configure_buffers(&ref_buf);
        upload_textures_to(1);

        /* Run both */
        run_scenario(name, 1);
        run_scenario(name, 0);

        /* 1. Depth exactness check */
        int depth_exact = (memcmp(g_cand_depth, g_ref_depth, sizeof(g_cand_depth)) == 0);

        /* 2. Coverage & color check */
        unsigned max_color_diff = 0;
        unsigned pixels_over_2 = 0;
        unsigned written_pixels = 0;
        unsigned i;

        for (i = 0; i < W * H; ++i) {
            uint32_t c_cand = g_cand_color[i];
            uint32_t c_ref  = g_ref_color[i];

            int r_cand = (c_cand >> 24) & 0xff;
            int g_cand = (c_cand >> 16) & 0xff;
            int b_cand = (c_cand >> 8)  & 0xff;
            int a_cand = c_cand & 0xff;

            int r_ref = (c_ref >> 24) & 0xff;
            int g_ref = (c_ref >> 16) & 0xff;
            int b_ref = (c_ref >> 8)  & 0xff;
            int a_ref = c_ref & 0xff;

            int dr = abs(r_cand - r_ref);
            int dg = abs(g_cand - g_ref);
            int db = abs(b_cand - b_ref);
            int da = abs(a_cand - a_ref);

            int d = dr;
            if (dg > d) d = dg;
            if (db > d) d = db;
            if (da > d) d = da;

            if ((unsigned)d > max_color_diff)
                max_color_diff = (unsigned)d;

            if (c_ref != CLEAR_RGBA || (g_ref_depth[i * 3] != 0xff || g_ref_depth[i * 3 + 1] != 0xff || g_ref_depth[i * 3 + 2] != 0xff))
                written_pixels++;

            if (d > 2)
                pixels_over_2++;
        }

        double pct_over_2 = written_pixels > 0 ? (100.0 * (double)pixels_over_2 / (double)written_pixels) : 0.0;
        int pass = depth_exact && (max_color_diff <= 2 || pct_over_2 <= 0.05);

        printf("%-22s | %-11s | %-12u | %5u (%.3f%%) | %-8s\n",
               name,
               depth_exact ? "BIT-EXACT" : "MISMATCH",
               max_color_diff,
               pixels_over_2,
               pct_over_2,
               pass ? "PASS" : "FAIL");

        if (!pass) {
            all_ok = 0;
            fprintf(stderr, "ORACLE FAILURE in scenario '%s': depth_exact=%d max_diff=%u pct_over_2=%.4f%%\n",
                    name, depth_exact, max_color_diff, pct_over_2);
        }
    }

    if (!all_ok) {
        fprintf(stderr, "MiniGL oracle tests FAILED\n");
        return 1;
    }

    printf("All MiniGL raster oracle scenarios PASSED.\n");
    return 0;
}
