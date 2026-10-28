/*
 * Unit and integration tests for wire-v9 to RasterIX mapper and fallback dispatch.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mw_host.h"
#include "mw_types.h"
#include "mw_ctx.h"
#include "minigl_raster.h"
#include "minigl_rix_backend.h"
extern void (*g_rix_test_log)(const char *fmt, ...);
#include <stdarg.h>
static void test_log_wrapper(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#define WIDTH 64U
#define HEIGHT 48U

/* Mock platform for capture & hardware path routing */
struct mock_platform {
    uint32_t csr[16];
    uint8_t cmd_buf[2][64 * 1024];
    uint8_t white_tex[MINIGL_RIX_WHITE_TEXTURE_BYTES];
    uint8_t tex_pool[MINIGL_RIX_TEXTURE_POOL_BYTES];
    unsigned csr_reads;
    unsigned submissions;
};
static struct mock_platform s_mock;


static uint32_t mock_csr_read(void *ctx, uint32_t offset)
{
    struct mock_platform *p = (struct mock_platform *)ctx;
    ++p->csr_reads;
    return p->csr[offset / 4U];
}

static void mock_csr_write(void *ctx, uint32_t offset, uint32_t value)
{
    struct mock_platform *p = (struct mock_platform *)ctx;
    p->csr[offset / 4U] = value;
    if (offset == RIX_DRV_CSR_CMD_START && value != 0U) {
        ++p->submissions;
        ++p->csr[RIX_DRV_CSR_SUBMITTED / 4U];
        p->csr[RIX_DRV_CSR_COMPLETED / 4U] = p->csr[RIX_DRV_CSR_SUBMITTED / 4U];
        p->csr[RIX_DRV_CSR_CMD_START / 4U] = RIX_DRV_CMD_START_QUEUE_READY;
    }
}

static void mock_cache_noop(void *ctx, uintptr_t addr, size_t bytes)
{
    (void)ctx;
    (void)addr;
    (void)bytes;
}

static void setup_mock_platform(struct mock_platform *p, MiniGLRixPlatform *out)
{
    memset(p, 0, sizeof(*p));
    memset(out, 0, sizeof(*out));
    p->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    p->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    p->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    p->csr[RIX_DRV_CSR_CMD_START / 4U] = RIX_DRV_CMD_START_QUEUE_READY;
    out->context = p;
    out->csr_read = mock_csr_read;
    out->csr_write = mock_csr_write;
    out->cache_clean = mock_cache_noop;
    out->cache_invalidate = mock_cache_noop;
    out->command_buffer[0] = p->cmd_buf[0];
    out->command_buffer[1] = p->cmd_buf[1];
    out->command_address[0] = 0x100000;
    out->command_address[1] = 0x110000;
    out->command_capacity = sizeof(p->cmd_buf[0]);
    out->white_texture = p->white_tex;
    out->white_texture_address = 0x200000;
    out->white_texture_bytes = sizeof(p->white_tex);
    out->texture_memory = p->tex_pool;
    out->texture_address = 0x300000;
    out->texture_bytes = sizeof(p->tex_pool);
}

static void test_platform_probe_guard(void)
{
    MiniGLRixPlatform platform;

    setup_mock_platform(&s_mock, &platform);
    s_mock.csr[RIX_DRV_CSR_IDENTITY / 4U] = 0U;
    assert(!minigl_rix_backend_probe_platform(&platform));
    assert(s_mock.csr_reads == 1U);

    setup_mock_platform(&s_mock, &platform);
    s_mock.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE + 1U;
    assert(!minigl_rix_backend_probe_platform(&platform));
    assert(s_mock.csr_reads == 2U);

    setup_mock_platform(&s_mock, &platform);
    assert(minigl_rix_backend_probe_platform(&platform));
    assert(s_mock.csr_reads == 2U);
    printf("PASS: test_platform_probe_guard\n");
}

/* 1. Direct state mapper test */
static void test_state_mapper_fallback_rules(void)
{
    MiniGLRixBackend backend;
    MiniGLRixPlatform platform;
    ZZ9KMiniGLPacketSetState state;

    setup_mock_platform(&s_mock, &platform);
    minigl_rix_backend_init(&backend, &platform);
    assert(minigl_rix_backend_available(&backend));

    memset(&state, 0, sizeof(state));
    state.depth_bits = 24U;
    state.blend_equation = 0x8006U; /* GL_FUNC_ADD */
    state.depth_func = 0x0201U; /* GL_LESS */
    state.alpha_func = 0x0207U; /* GL_ALWAYS */
    state.blend_src = 0x0302U;  /* GL_SRC_ALPHA */
    state.blend_dst = 0x0303U;  /* GL_ONE_MINUS_SRC_ALPHA */

    /* Baseline valid state */
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    /* 1a. Forced CPU */
    state.reserved = ZZ9K_MGL_STATE_FLAG_FORCE_CPU;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_FORCED_CPU);
    state.reserved = 0U;

    /* 1b. Z16 backend accepts MiniGL's 24/32-bit requests. */
    state.depth_bits = 32U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);
    state.depth_bits = 16U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);
    state.depth_bits = 24U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    /* 1c. Fog */
    state.enabled_flags |= ZZ9K_MGL_STATE_FOG;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_FOG);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_FOG;

    /* 1d. Multitexture */
    state.enabled_flags |= ZZ9K_MGL_STATE_TEXTURE_2D_1;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_TEXTURE_2D_1;

    /* 1e. Polygon offset */
    state.enabled_flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_POLYGON_OFFSET);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;

    state.polygon_offset_factor = 1.0f;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_POLYGON_OFFSET);
    state.polygon_offset_factor = 0.0f;

    /* 1f. Polygon mode */
    state.polygon_mode = 0x1b01U; /* GL_LINE */
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_POLYGON_MODE);
    state.polygon_mode = 0U;

    /* 1g. Blend equation */
    state.blend_equation = 0x8007U; /* GL_FUNC_SUBTRACT */
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_BLEND_EQUATION);
    state.blend_equation = 0x8006U; /* GL_FUNC_ADD */

    /* 1h. Verify all fallback strings */
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_NONE), "none") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_DEPTH_BITS), "depth bits") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_FOG), "fog") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_MULTITEXTURE), "multitexture") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_POLYGON_OFFSET), "polygon offset") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_POLYGON_MODE), "polygon mode") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_BLEND_EQUATION), "blend equation") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_TEXTURE_FILTER), "texture filter") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_RESIDENCY_EXHAUSTED), "residency exhausted") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_BACKEND_DISABLED_OR_ABSENT), "backend disabled/absent") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_FORCED_CPU), "forced CPU") == 0);

    printf("PASS: test_state_mapper_fallback_rules\n");
}

/* 2. Texture environment mode mapping: REPLACE, MODULATE, ADD, DECAL, BLEND */
static void test_texture_env_modes(void)
{
    MiniGLRixBackend backend;
    MiniGLRixPlatform platform;
    ZZ9KMiniGLPacketSetState state;
    uint32_t texels[64 * 64];

    setup_mock_platform(&s_mock, &platform);
    minigl_rix_backend_init(&backend, &platform);

    memset(texels, 0xff, sizeof(texels));
    assert(minigl_rix_backend_upload_texture(&backend, 1, 0, texels, 64, 64, 0));
    minigl_rix_backend_bind_texture(&backend, 1);
    minigl_rix_backend_tex_params(&backend, 1, 0x2600U, 0x2600U, 0x2901U, 0x2901U);

    memset(&state, 0, sizeof(state));
    state.depth_bits = 24U;
    state.blend_equation = 0x8006U;
    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0;

    /* GL_REPLACE (0x1e01) */
    state.tex_env_mode[0] = 0x1e01U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    /* GL_MODULATE (0x2100) */
    state.tex_env_mode[0] = 0x2100U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    /* GL_ADD (0x0104) */
    state.tex_env_mode[0] = 0x0104U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    /* GL_DECAL (0x2101) */
    state.tex_env_mode[0] = 0x2101U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    /* GL_BLEND (0x0be2) */
    state.tex_env_mode[0] = 0x0be2U;
    state.tex_env_color[0] = 0x808080ffU;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    printf("PASS: test_texture_env_modes\n");
}

/* 3. Integration: wire-v9 draws route to RasterIX and fallback cleanly */
static void test_wire_v9_draw_routing_and_fallback(void)
{
    MiniGLRixPlatform platform;
    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);

    mw_ctx *c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL);

    /* Construct wire-v9 packet with a single packed triangle */
    uint8_t packet[512];
    memset(packet, 0, sizeof(packet));

    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 1); /* 1 command: MW_DRAW_OBJECT */

    uint32_t obj_payload = 8 + 3 * 76;
    mw_put_be32(packet + 16, MW_DRAW_OBJECT);
    mw_put_be32(packet + 20, obj_payload);
    mw_put_be32(packet + 24, 4);  /* GL_TRIANGLES */
    mw_put_be32(packet + 28, 3);  /* count 3 */

    uint8_t *q = packet + 32;
    /* Vert 0: (0, 0.8, 0, 1), white */
    mw_put_bef(q, 0.0f); mw_put_bef(q + 4, 0.8f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f);
    q += 76;
    /* Vert 1: (-0.8, -0.8, 0, 1), white */
    mw_put_bef(q, -0.8f); mw_put_bef(q + 4, -0.8f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f);
    q += 76;
    /* Vert 2: (0.8, -0.8, 0, 1), white */
    mw_put_bef(q, 0.8f); mw_put_bef(q + 4, -0.8f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f);
    q += 76;

    uint32_t packet_bytes = 16 + 8 + obj_payload;
    mw_put_be32(packet + 8, packet_bytes);
    /* Submit: should route to hardware! */
    uint32_t status = mw_host_submit(c, packet, packet_bytes);
    assert(status == MW_STATUS_OK);
    assert(minigl_raster_draws_hardware() >= 1U);

    /* Test fallback on fog: send MW_FOG = 1 */
    uint8_t fog_packet[64];
    memset(fog_packet, 0, sizeof(fog_packet));
    mw_put_be32(fog_packet, MW_MAGIC);
    mw_put_be32(fog_packet + 4, MW_VERSION);
    mw_put_be32(fog_packet + 12, 1);
    mw_put_be32(fog_packet + 16, MW_STATE);
    mw_put_be32(fog_packet + 20, 12);
    mw_put_be32(fog_packet + 24, MW_FOG);
    mw_put_be32(fog_packet + 28, 1);
    mw_put_be32(fog_packet + 32, 1); /* fog = 1 */
    uint32_t fog_bytes = 16 + 8 + 12;
    mw_put_be32(fog_packet + 8, fog_bytes);

    status = mw_host_submit(c, fog_packet, fog_bytes);
    assert(status == MW_STATUS_OK);

    uint32_t sw_before = minigl_raster_draws_software();
    status = mw_host_submit(c, packet, packet_bytes);
    assert(status == MW_STATUS_OK);
    assert(minigl_raster_draws_software() > sw_before);
    assert(minigl_raster_first_fallback_reason() == ZZ9K_MGL_FALLBACK_FOG);
    minigl_raster_reset_frame_counters();

    /* Test forced CPU */
    c->state.force_cpu = 1;
    status = mw_host_submit(c, packet, packet_bytes);
    assert(status == MW_STATUS_OK);
    assert(minigl_raster_first_fallback_reason() == ZZ9K_MGL_FALLBACK_FORCED_CPU);
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);

    printf("PASS: test_wire_v9_draw_routing_and_fallback\n");
}

/* 4. Watertight tessellated grid test */
static void test_watertight_tessellation(void)
{
    #define GRID_N 8
    static uint8_t hit_counts[640 * 480];
    RixVertex grid[GRID_N + 1][GRID_N + 1];
    unsigned i, j;

    memset(hit_counts, 0, sizeof(hit_counts));

    /* Build grid from (100, 100) to (260, 260) with 20px step */
    for (j = 0; j <= GRID_N; ++j) {
        for (i = 0; i <= GRID_N; ++i) {
            float jx = (i > 0 && i < GRID_N) ? (float)((int)((i * 7 + j * 13) % 7) - 3) * 0.125f : 0.0f;
            float jy = (j > 0 && j < GRID_N) ? (float)((int)((i * 11 + j * 5) % 7) - 3) * 0.125f : 0.0f;
            float x = 100.0f + (float)i * 20.0f + jx;
            float y = 100.0f + (float)j * 20.0f + jy;
            grid[j][i].x = x;
            grid[j][i].y = y;
            grid[j][i].z = 0.0f;
            grid[j][i].w = 1.0f;
            grid[j][i].rgba = 0xffffffffU;
            grid[j][i].s = 0.0f; grid[j][i].t = 0.0f; grid[j][i].q = 1.0f;
        }
    }

    /* Render all 2 * GRID_N * GRID_N triangles */
    for (j = 0; j < GRID_N; ++j) {
        for (i = 0; i < GRID_N; ++i) {
            RixTriangle tri1, tri2;
            RixVertex p00 = grid[j][i];
            RixVertex p10 = grid[j][i + 1];
            RixVertex p11 = grid[j + 1][i + 1];
            RixVertex p01 = grid[j + 1][i];

            assert(rix_setup_triangle(&tri1, &p00, &p10, &p11, NULL));
            assert(rix_setup_triangle(&tri2, &p00, &p11, &p01, NULL));

            RixTriangle *tris[2] = { &tri1, &tri2 };
            for (int t = 0; t < 2; ++t) {
                int32_t bx = (int32_t)(tris[t]->words[RIX_WORD_BB_START] & 0xffffU);
                int32_t by = (int32_t)(tris[t]->words[RIX_WORD_BB_START] >> 16);
                int32_t ex = (int32_t)(tris[t]->words[RIX_WORD_BB_END] & 0xffffU);
                int32_t ey = (int32_t)(tris[t]->words[RIX_WORD_BB_END] >> 16);
                int32_t x, y;
                for (y = by; y < ey; ++y) {
                    for (x = bx; x < ex; ++x) {
                        int inside = 1;
                        for (unsigned e = 0; e < 3; ++e) {
                            int32_t wx = (int32_t)tris[t]->words[RIX_WORD_W_X + e];
                            int32_t wy = (int32_t)tris[t]->words[RIX_WORD_W_Y + e];
                            int32_t val = (int32_t)tris[t]->words[RIX_WORD_W_INIT + e] +
                                          wx * (x - bx) + wy * (y - by);
                            int inc = (wx > 0) || (wx == 0 && wy > 0);
                            int edge_in = !((uint32_t)val >> 31) && ((val != 0) || inc);
                            if (!edge_in) { inside = 0; break; }
                        }
                        if (inside && x < 640 && y < 480) {
                            hit_counts[y * 640 + x]++;
                        }
                    }
                }
            }
        }
    }

    /* Verify: every interior pixel in (101..259, 101..259) is hit exactly once! */
    unsigned missing = 0, double_drawn = 0;
    for (int y = 101; y < 259; ++y) {
        for (int x = 101; x < 259; ++x) {
            uint8_t count = hit_counts[y * 640 + x];
            if (count == 0) missing++;
            else if (count > 1) double_drawn++;
        }
    }

    assert(missing == 0);
    assert(double_drawn == 0);
    printf("PASS: test_watertight_tessellation (interior pixels hit exactly once, zero cracks, zero overlaps)\n");
}

int main(void)
{
    g_rix_test_log = test_log_wrapper;
    printf("Starting Wire-v9 RasterIX Mapper Tests...\n");
    test_platform_probe_guard();
    test_state_mapper_fallback_rules();
    test_texture_env_modes();
    test_wire_v9_draw_routing_and_fallback();
    test_watertight_tessellation();
    printf("ALL Wire-v9 RasterIX Mapper Tests PASSED!\n");
    return 0;
}
