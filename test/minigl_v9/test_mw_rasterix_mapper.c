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
#include "rix_cmd.h"
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
    uint32_t fifo[6];
    uint32_t packet[RIX_DRV_FIFO_PACKET_WORDS + 2U];
    unsigned packet_words;
    uint32_t color_address;
    uint16_t clear_color;
    uint8_t cmd_buf[2][64 * 1024];
    uint8_t white_tex[MINIGL_RIX_WHITE_TEXTURE_BYTES];
    uint8_t tex_pool[MINIGL_RIX_TEXTURE_POOL_BYTES];
    unsigned csr_reads;
    unsigned submissions;
    int complete_submissions;
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
}

static uint16_t mock_fence_color(uint32_t rgba)
{
    return (uint16_t)(((rgba >> 27U) << 11U) |
                      ((rgba >> 18U) << 5U) | (rgba >> 11U));
}

/* Pixel coverage of every triangle RasterIX would draw while capture is on,
 * from the descriptor's edge equations (as test_watertight_tessellation).
 * RasterIX rows count up from the bottom (the backend sends height - y), so
 * row r lands in memory row height - 1 - r. */
static int s_capture_coverage;
static uint8_t s_coverage[HEIGHT][WIDTH];

static void mock_cover_triangle(const uint32_t *words)
{
    int32_t bx = (int32_t)(words[RIX_WORD_BB_START] & 0xffffU);
    int32_t by = (int32_t)(words[RIX_WORD_BB_START] >> 16);
    int32_t ex = (int32_t)(words[RIX_WORD_BB_END] & 0xffffU);
    int32_t ey = (int32_t)(words[RIX_WORD_BB_END] >> 16);
    int32_t x, y;
    unsigned e;

    for (y = by; y < ey; ++y) {
        for (x = bx; x < ex; ++x) {
            int inside = 1;

            for (e = 0U; e < 3U && inside; ++e) {
                int32_t wx = (int32_t)words[RIX_WORD_W_X + e];
                int32_t wy = (int32_t)words[RIX_WORD_W_Y + e];
                int32_t val = (int32_t)words[RIX_WORD_W_INIT + e] +
                              wx * (x - bx) + wy * (y - by);
                int inc = (wx > 0) || (wx == 0 && wy > 0);

                inside = !((uint32_t)val >> 31) && ((val != 0) || inc);
            }
            if (inside && x >= 0 && y >= 0 && x < (int32_t)WIDTH && y < (int32_t)HEIGHT)
                ++s_coverage[HEIGHT - 1U - (uint32_t)y][x];
        }
    }
}

static void mock_render_packet(struct mock_platform *p)
{
    unsigned index;

    for (index = 2U; index < p->packet_words; ++index) {
        uint32_t command = p->packet[index];

        if ((command & 0xf0000000U) == RIX_OP_TRIANGLE_STREAM) {
            uint32_t words = (command & 0x3ffU) / 4U;

            assert(index + words < p->packet_words);
            if (s_capture_coverage) {
                assert(words >= RIX_TRIANGLE_WORDS);
                mock_cover_triangle(&p->packet[index + 1U]);
            }
            index += words;
            continue;
        }
        if ((command & 0xf0000000U) == RIX_OP_RENDER_CONFIG) {
            assert(index + 1U < p->packet_words);
            if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_COLOR_BUFFER_ADDR))
                p->color_address = p->packet[index + 1U];
            else if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_CLEAR_COLOR))
                p->clear_color = mock_fence_color(p->packet[index + 1U]);
            ++index;
        } else if ((command & 0xf0000000U) == RIX_OP_FRAMEBUFFER &&
                   (command & 0xffU) == (RIX_FRAMEBUFFER_MEMSET |
                                         RIX_FRAMEBUFFER_COLOR) &&
                   ((command >> 8U) & 0xfffffU) == 1U && p->complete_submissions) {
            if (p->color_address == 0x100000U + sizeof(p->cmd_buf[0]) -
                                    MINIGL_RIX_TEXTURE_PAGE_BYTES)
                *(uint16_t *)(p->cmd_buf[0] + sizeof(p->cmd_buf[0]) -
                              MINIGL_RIX_TEXTURE_PAGE_BYTES) = p->clear_color;
            else if (p->color_address == 0x110000U + sizeof(p->cmd_buf[1]) -
                                         MINIGL_RIX_TEXTURE_PAGE_BYTES)
                *(uint16_t *)(p->cmd_buf[1] + sizeof(p->cmd_buf[1]) -
                              MINIGL_RIX_TEXTURE_PAGE_BYTES) = p->clear_color;
        }
    }
}

static uint32_t mock_fifo_read(void *ctx, uint32_t offset)
{
    return ((struct mock_platform *)ctx)->fifo[offset / 4U];
}

static void mock_fifo_write(void *ctx, uint32_t offset, uint32_t value)
{
    struct mock_platform *p = (struct mock_platform *)ctx;

    if (offset != RIX_DRV_FIFO_TDFD)
        p->fifo[offset / 4U] = value;
    if (offset == RIX_DRV_FIFO_TDFR) {
        p->packet_words = 0U;
    } else if (offset == RIX_DRV_FIFO_TDFD) {
        assert(p->packet_words < RIX_DRV_FIFO_PACKET_WORDS + 2U);
        p->packet[p->packet_words++] = value;
    } else if (offset == RIX_DRV_FIFO_TLF) {
        assert(value == p->packet_words * sizeof(p->packet[0]));
        assert(p->packet_words >= 2U);
        assert(p->packet[0] == (0x90000000U |
                                ((p->packet_words - 2U) * sizeof(uint32_t))));
        assert(p->packet[1] == 0U);
        mock_render_packet(p);
        ++p->submissions;
        p->packet_words = 0U;
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
    p->complete_submissions = 1;
    p->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    p->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    p->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    p->fifo[RIX_DRV_FIFO_TDFV / 4U] = RIX_DRV_FIFO_PACKET_WORDS + 2U;
    out->context = p;
    out->csr_read = mock_csr_read;
    out->csr_write = mock_csr_write;
    out->fifo_read = mock_fifo_read;
    out->fifo_write = mock_fifo_write;
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
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_DRAW_REJECTED), "draw rejected") == 0);

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
    minigl_rix_backend_bind_texture(&backend, 0U, 1U);
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

    /* A unit-0 REPLACE plus unit-1 MODULATE draw maps to RasterIX as two
     * depth-coupled passes; blending and unsupported unit-1 env modes do not. */
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, texels, 64U, 64U, 0));
    minigl_rix_backend_tex_params(&backend, 2U, 0x2600U, 0x2600U, 0x2901U, 0x2901U);
    minigl_rix_backend_bind_texture(&backend, 1U, 2U);
    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0 |
                          ZZ9K_MGL_STATE_TEXTURE_2D_1 |
                          ZZ9K_MGL_STATE_DEPTH_TEST |
                          ZZ9K_MGL_STATE_DEPTH_WRITE;
    state.depth_func = 0x0203U;
    state.color_mask = 7U;
    state.tex_env_mode[0] = 0x1e01U;
    state.tex_env_mode[1] = 0x2100U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);
    state.depth_func = 0x0201U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
    state.depth_func = 0x0203U;
    state.enabled_flags |= ZZ9K_MGL_STATE_BLEND;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_BLEND;
    state.tex_env_mode[1] = 0x2101U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);

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
    minigl_raster_reset_frame_counters();


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

    /* A CPU fallback holds the hardware path until the actual wire frame
     * boundary, not merely until the unsupported state is removed. */
    mw_put_be32(fog_packet + 32, 0);
    assert(mw_host_submit(c, fog_packet, fog_bytes) == MW_STATUS_OK);
    sw_before = minigl_raster_draws_software();
    assert(mw_host_submit(c, packet, packet_bytes) == MW_STATUS_OK);
    assert(minigl_raster_draws_software() > sw_before);
    {
        uint8_t present_packet[24];
        memset(present_packet, 0, sizeof(present_packet));
        mw_put_be32(present_packet, MW_MAGIC);
        mw_put_be32(present_packet + 4, MW_VERSION);
        mw_put_be32(present_packet + 8, sizeof(present_packet));
        mw_put_be32(present_packet + 12, 1);
        mw_put_be32(present_packet + 16, MW_PRESENT);
        assert(mw_host_submit(c, present_packet, sizeof(present_packet)) == MW_STATUS_OK);
    }
    {
        uint32_t hw_before = minigl_raster_draws_hardware();
        assert(mw_host_submit(c, packet, packet_bytes) == MW_STATUS_OK);
        assert(minigl_raster_draws_hardware() > hw_before);
    }

    /* Test forced CPU */
    c->state.force_cpu = 1;
    status = mw_host_submit(c, packet, packet_bytes);
    assert(status == MW_STATUS_OK);
    assert(minigl_raster_first_fallback_reason() == ZZ9K_MGL_FALLBACK_FORCED_CPU);
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);

    printf("PASS: test_wire_v9_draw_routing_and_fallback\n");
}

static void put_blit_request(uint8_t *request)
{
    memset(request, 0, 40U);
    mw_put_be32(request, MW_BLIT);
    mw_put_be32(request + 4, 8U); /* RGBFB_R8G8B8A8 */
    mw_put_be32(request + 8, 8U);
    mw_put_be32(request + 20, 1U);
    mw_put_be32(request + 32, 2U);
    mw_put_be32(request + 36, 2U);
}

static uint32_t submit_single_command(mw_ctx *c, uint32_t opcode,
                                      const uint8_t *payload, uint32_t payload_bytes);

static void assert_blit_pixels(mw_ctx *c, const uint8_t expected[16])
{
    uint8_t request[40];
    uint8_t span[16];
    uint32_t reply_bytes = 0U;

    put_blit_request(request);
    memset(span, 0, sizeof(span));
    assert(mw_host_request(c, request, sizeof(request), span, sizeof(span),
                           &reply_bytes) == MW_STATUS_OK);
    assert(reply_bytes == sizeof(span));
    assert(memcmp(span, expected, sizeof(span)) == 0);
}

static void test_cpu_present_orientation(void)
{
    static const uint8_t expected[16] = {
        0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
        0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    };
    mw_ctx *c;

    minigl_raster_set_rix_platform(NULL);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);
    c->color_buffers[c->state.target.latest][0] = 0xff0000ffU;
    c->color_buffers[c->state.target.latest][1] = 0x00ff00ffU;
    c->color_buffers[c->state.target.latest][2] = 0x0000ffffU;
    c->color_buffers[c->state.target.latest][3] = 0xffffffffU;
    assert_blit_pixels(c, expected);
    mw_host_destroy(c);
    printf("PASS: test_cpu_present_orientation\n");
}

static void test_rasterix_rgb565_present_orientation(void)
{
    static const uint8_t expected[16] = {
        0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
        0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    };
    MiniGLRixPlatform platform;
    mw_ctx *c;
    uint16_t *surface;

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);
    c->state.target.latest = c->state.target.back;
    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    /* The 565 layout presents unflipped like the RGBA view converted in
     * place; the historical span-path flip double-flipped it. */
    assert(!minigl_raster_present_is_bottom_up());
    surface = (uint16_t *)c->color_buffers[c->state.target.latest];
    surface[0] = 0xf800U; /* top row: red, green. */
    surface[1] = 0x07e0U;
    surface[2] = 0x001fU; /* bottom row: blue, white. */
    surface[3] = 0xffffU;
    {
        uint8_t present_cmd[24];
        memset(present_cmd, 0, sizeof(present_cmd));
        mw_put_be32(present_cmd, MW_MAGIC);
        mw_put_be32(present_cmd + 4, MW_VERSION);
        mw_put_be32(present_cmd + 8, sizeof(present_cmd));
        mw_put_be32(present_cmd + 12, 1);
        mw_put_be32(present_cmd + 16, MW_PRESENT);
        assert(mw_host_submit(c, present_cmd, sizeof(present_cmd)) == MW_STATUS_OK);
    }
    assert_blit_pixels(c, expected);
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_rasterix_rgb565_present_orientation\n");
}

static void test_rasterix_rgb565_span_presentation(void)
{
    static const uint8_t expected_rgba[16] = {
        0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
        0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    };
    static uint8_t mock_vram[0x30000];
    MiniGLRixPlatform platform;
    mw_ctx *c;
    uint16_t *surface;
    uint8_t target_cmd[48];
    uint8_t span_packet[80];
    uint32_t card_offset = 0x00020000U;
    uint8_t *span_dest;

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);

    /* 1. Configure single-buffered context via wire command */
    memset(target_cmd, 0, sizeof(target_cmd));
    mw_put_be32(target_cmd, 2U);
    mw_put_be32(target_cmd + 4, 2U);
    mw_put_be32(target_cmd + 8, 1U); /* buffer_count = 1 */
    mw_put_be32(target_cmd + 12, 16U);
    mw_put_be32(target_cmd + 16, 2U);
    mw_put_be32(target_cmd + 20, 2U);
    assert(submit_single_command(c, MW_TARGET, target_cmd, 24U) == MW_STATUS_OK);
    assert(c->state.target.buffer_count == 1U);

    /* 2. Execute MW_PRESENT per frame on single-buffered context */
    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    {
        uint8_t present_cmd[24];
        memset(present_cmd, 0, sizeof(present_cmd));
        mw_put_be32(present_cmd, MW_MAGIC);
        mw_put_be32(present_cmd + 4, MW_VERSION);
        mw_put_be32(present_cmd + 8, sizeof(present_cmd));
        mw_put_be32(present_cmd + 12, 1);
        mw_put_be32(present_cmd + 16, MW_PRESENT);
        assert(mw_host_submit(c, present_cmd, sizeof(present_cmd)) == MW_STATUS_OK);
    }

    /* 3. Populate 565 surface the way hardware leaves it */
    surface = (uint16_t *)c->color_buffers[c->state.target.latest];
    surface[0] = 0xf800U; /* (0,0): red */
    surface[1] = 0x07e0U; /* (1,0): green */
    surface[2] = 0x001fU; /* (0,1): blue */
    surface[3] = 0xffffU; /* (1,1): white */
    /* 4. Execute direct card span presentation (MW_PRESENT_SPAN, RGBFB_R5G6B5PC = 4) */
    memset(mock_vram, 0xa5, sizeof(mock_vram));
    mw_host_set_present_vram(mock_vram, 0x01000000U);
    span_dest = mock_vram + card_offset;

    memset(span_packet, 0, sizeof(span_packet));
    mw_put_be32(span_packet, MW_MAGIC);
    mw_put_be32(span_packet + 4, MW_VERSION);
    mw_put_be32(span_packet + 8, 24U + 48U); /* 1 command: MW_PRESENT_SPAN, 48 payload bytes */
    mw_put_be32(span_packet + 12, 1U);
    mw_put_be32(span_packet + 16, MW_PRESENT_SPAN);
    mw_put_be32(span_packet + 20, 48U);

    /* Payload: MW_BLIT (24 bytes) + rect (16 bytes) + card_offset (4) + span_bytes (4) */
    uint8_t *p = span_packet + 24;
    mw_put_be32(p, MW_BLIT);
    mw_put_be32(p + 4, 4U);  /* format: RGBFB_R5G6B5PC (LE) */
    mw_put_be32(p + 8, 4U);  /* pitch: 2 pixels * 2 bytes = 4 */
    mw_put_be32(p + 12, 0U); /* in_x */
    mw_put_be32(p + 16, 0U); /* in_y */
    mw_put_be32(p + 20, 1U); /* 1 rect */
    mw_put_be32(p + 24, 0U); /* rx0 */
    mw_put_be32(p + 28, 0U); /* ry0 */
    mw_put_be32(p + 32, 2U); /* rx1 */
    mw_put_be32(p + 36, 2U); /* ry1 */
    mw_put_be32(p + 40, card_offset);
    mw_put_be32(p + 44, 8U); /* 4 pixels * 2 bytes = 8 bytes */

    assert(mw_host_submit(c, span_packet, 24U + 48U) == MW_STATUS_OK);

    /* Assert exact RGB565PC (LE) bytes: red=0x00,0xf8; green=0xe0,0x07; blue=0x1f,0x00; white=0xff,0xff */
    assert(span_dest[0] == 0x00 && span_dest[1] == 0xf8);
    assert(span_dest[2] == 0xe0 && span_dest[3] == 0x07);
    assert(span_dest[4] == 0x1f && span_dest[5] == 0x00);
    assert(span_dest[6] == 0xff && span_dest[7] == 0xff);

    /* 5. Also assert blit to RGBA output */
    assert_blit_pixels(c, expected_rgba);

    /* 6. Verify CPU fallback frame in single-buffer mode doesn't corrupt to blue */
    c->color_buffers[0][0] = 0xff0000ffU; /* Red RGBA */
    c->color_buffers[0][1] = 0x00ff00ffU; /* Green RGBA */
    c->color_buffers[0][2] = 0x0000ffffU; /* Blue RGBA */
    c->color_buffers[0][3] = 0xffffffffU; /* White RGBA */
    minigl_raster_finish();
    assert(!minigl_raster_present_is_rgb565());
    {
        uint8_t present_cmd[24];
        memset(present_cmd, 0, sizeof(present_cmd));
        mw_put_be32(present_cmd, MW_MAGIC);
        mw_put_be32(present_cmd + 4, MW_VERSION);
        mw_put_be32(present_cmd + 8, sizeof(present_cmd));
        mw_put_be32(present_cmd + 12, 1);
        mw_put_be32(present_cmd + 16, MW_PRESENT);
        assert(mw_host_submit(c, present_cmd, sizeof(present_cmd)) == MW_STATUS_OK);
    }
    assert(c->color_rgb565[0] == 0U);
    assert_blit_pixels(c, expected_rgba);

    mw_host_set_present_vram(NULL, 0);
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_rasterix_rgb565_span_presentation\n");
}

static void submit_point_size(mw_ctx *c, float size)
{
    uint8_t payload[12];

    mw_put_be32(payload, MW_POINT_SIZE);
    mw_put_be32(payload + 4, 1U);
    mw_put_bef(payload + 8, size);
    assert(submit_single_command(c, MW_STATE, payload, sizeof(payload)) == MW_STATUS_OK);
}

/* Window coordinates (sx, sy) of each vertex; NDC for the identity MVP. */
static void submit_vertices(mw_ctx *c, uint32_t mode, const float (*window)[2],
                            uint32_t count)
{
    static uint8_t packet[16 + 8 + 8 + 8 * 76];
    uint32_t payload = 8U + count * 76U;
    uint8_t *q = packet + 32;
    uint32_t i;

    assert(count <= 8U);
    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 24U + payload);
    mw_put_be32(packet + 12, 1U);
    mw_put_be32(packet + 16, MW_DRAW_OBJECT);
    mw_put_be32(packet + 20, payload);
    mw_put_be32(packet + 24, mode);
    mw_put_be32(packet + 28, count);
    for (i = 0U; i < count; ++i, q += 76) {
        mw_put_bef(q, window[i][0] * 2.0f / (float)WIDTH - 1.0f);
        mw_put_bef(q + 4, window[i][1] * 2.0f / (float)HEIGHT - 1.0f);
        mw_put_bef(q + 8, 0.0f);
        mw_put_bef(q + 12, 1.0f);
        mw_put_bef(q + 16, 1.0f);
        mw_put_bef(q + 20, 1.0f);
        mw_put_bef(q + 24, 1.0f);
        mw_put_bef(q + 28, 1.0f);
    }
    assert(mw_host_submit(c, packet, 24U + payload) == MW_STATUS_OK);
}

/* Draws the same vertices on RasterIX (coverage from the descriptors it is
 * sent) and on the CPU rasterizer, and requires the same pixels. */
static void assert_rasterix_matches_cpu(mw_ctx *c, uint32_t mode,
                                        const float (*window)[2], uint32_t count)
{
    uint32_t hw_before, sw_before;
    const uint32_t *cpu;
    unsigned x, y;

    c->state.force_cpu = 0;
    minigl_raster_reset_frame_counters();
    hw_before = minigl_raster_draws_hardware();
    sw_before = minigl_raster_draws_software();
    memset(s_coverage, 0, sizeof(s_coverage));
    s_capture_coverage = 1;
    submit_vertices(c, mode, window, count);
    assert(minigl_raster_drain());
    s_capture_coverage = 0;
    assert(minigl_raster_draws_hardware() == hw_before + 1U);
    assert(minigl_raster_draws_software() == sw_before);

    c->state.force_cpu = 1;
    assert(minigl_raster_finish());
    memset(c->color_buffers[c->state.target.back], 0, WIDTH * HEIGHT * 4U);
    submit_vertices(c, mode, window, count);
    cpu = c->color_buffers[c->state.target.back];
    for (y = 0U; y < HEIGHT; ++y)
        for (x = 0U; x < WIDTH; ++x)
            assert(s_coverage[y][x] == (cpu[y * WIDTH + x] != 0U ? 1U : 0U));
    /* The forced CPU draw latched this frame to the CPU. */
    minigl_raster_frame_boundary();
    c->state.force_cpu = 0;
}

/* GL_POINTS (particles) stay on RasterIX instead of latching the frame to
 * the CPU rasterizer, and cover exactly the pixels the CPU path covers. */
static void test_points_render_on_rasterix(void)
{
    static const float window[][2] = {
        { 10.5f, 10.5f }, { 30.0f, 20.0f }, { 47.25f, 33.75f }, { 0.2f, 0.2f }
    };
    static const float sizes[] = { 1.0f, 2.0f, 3.0f };
    MiniGLRixPlatform platform;
    mw_ctx *c;
    unsigned s;

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL);
    for (s = 0U; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        submit_point_size(c, sizes[s]);
        assert_rasterix_matches_cpu(c, 0U /* GL_POINTS */, window, 4U);
    }
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_points_render_on_rasterix\n");
}

/* RasterIX samples where OpenGL does (pixel centres): triangles whose edges
 * fall between pixel centres cover the CPU rasterizer's pixels, not ones
 * half a pixel away. */
static void test_triangles_sample_pixel_centres(void)
{
    static const float triangles[][3][2] = {
        { { 3.3f, 4.7f }, { 20.6f, 5.2f }, { 9.4f, 19.8f } },
        { { 30.25f, 2.75f }, { 41.75f, 2.75f }, { 41.75f, 14.25f } },
        { { 50.5f, 30.1f }, { 60.9f, 44.6f }, { 45.2f, 41.3f } }
    };
    MiniGLRixPlatform platform;
    mw_ctx *c;
    unsigned t;

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL);
    for (t = 0U; t < sizeof(triangles) / sizeof(triangles[0]); ++t)
        assert_rasterix_matches_cpu(c, 4U /* GL_TRIANGLES */, triangles[t], 3U);
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_triangles_sample_pixel_centres\n");
}

/* The row fast paths of the 68k present must give the bytes the per-pixel
 * encoder gives, including the 5/6-bit widening, in every screen format
 * they cover. */
static void test_rasterix_rgb565_present_formats(void)
{
    static const struct {
        uint32_t format;
        uint32_t bpp;
        uint8_t bytes[16];
    } cases[] = {
        { 1U, 1U, { 0, 0, 0, 0 } },
        { 2U, 3U, { 0xff, 0x00, 0x00, 0x00, 0xff, 0x00,
                    0x00, 0x00, 0xff, 0x84, 0x82, 0x84 } },
        { 3U, 3U, { 0x00, 0x00, 0xff, 0x00, 0xff, 0x00,
                    0xff, 0x00, 0x00, 0x84, 0x82, 0x84 } },
        { 4U, 2U, { 0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0x10, 0x84 } },
        { 5U, 2U, { 0x00, 0x7c, 0xe0, 0x03, 0x1f, 0x00, 0x10, 0x42 } },
        { 6U, 4U, { 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00,
                    0xff, 0x00, 0x00, 0xff, 0xff, 0x84, 0x82, 0x84 } },
        { 7U, 4U, { 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00,
                    0xff, 0xff, 0x00, 0x00, 0xff, 0x84, 0x82, 0x84 } },
        { 8U, 4U, { 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
                    0x00, 0x00, 0xff, 0xff, 0x84, 0x82, 0x84, 0xff } },
        { 9U, 4U, { 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff,
                    0xff, 0x00, 0x00, 0xff, 0x84, 0x82, 0x84, 0xff } },
        { 10U, 2U, { 0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f, 0x84, 0x10 } },
        { 11U, 2U, { 0x7c, 0x00, 0x03, 0xe0, 0x00, 0x1f, 0x42, 0x10 } },
        { 12U, 2U, { 0x1f, 0x00, 0xe0, 0x07, 0x00, 0xf8, 0x10, 0x84 } },
        { 13U, 2U, { 0x1f, 0x00, 0xe0, 0x03, 0x00, 0x7c, 0x10, 0x42 } }
    };
    static const struct {
        uint32_t format;
        uint32_t bpp;
        uint8_t bytes[16];
    } rgba_cases[] = {
        { 2U, 3U, { 0xff, 0x00, 0x00, 0x00, 0xff, 0x00,
                    0x00, 0x00, 0xff, 0x84, 0x84, 0x84 } },
        { 3U, 3U, { 0x00, 0x00, 0xff, 0x00, 0xff, 0x00,
                    0xff, 0x00, 0x00, 0x84, 0x84, 0x84 } },
        { 4U, 2U, { 0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0x30, 0x84 } },
        { 6U, 4U, { 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00,
                    0xff, 0x00, 0x00, 0xff, 0xff, 0x84, 0x84, 0x84 } },
        { 10U, 2U, { 0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f, 0x84, 0x30 } }
    };
    MiniGLRixPlatform platform;
    mw_ctx *c;
    uint16_t *surface;
    unsigned i;

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);
    c->state.target.latest = c->state.target.back;
    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    surface = (uint16_t *)c->color_buffers[c->state.target.latest];
    surface[0] = 0xf800U;
    surface[1] = 0x07e0U;
    surface[2] = 0x001fU;
    surface[3] = 0x8410U;
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t request[40];
        uint8_t span[16];
        uint32_t reply_bytes = 0U;

        put_blit_request(request);
        mw_put_be32(request + 4, cases[i].format);
        mw_put_be32(request + 8, 2U * cases[i].bpp);
        memset(span, 0, sizeof(span));
        assert(mw_host_request(c, request, sizeof(request), span, 4U * cases[i].bpp,
                               &reply_bytes) == MW_STATUS_OK);
        assert(memcmp(span, cases[i].bytes, 4U * cases[i].bpp) == 0);
    }
    /* The same formats straight from an RGBA-latest buffer (the CPU or
     * converted view): red, green, blue, grey 0x84.  Without a RasterIX
     * platform the format sync marks the buffer RGBA. */
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);
    c->color_buffers[c->state.target.latest][0] = 0xff0000ffU;
    c->color_buffers[c->state.target.latest][1] = 0x00ff00ffU;
    c->color_buffers[c->state.target.latest][2] = 0x0000ffffU;
    c->color_buffers[c->state.target.latest][3] = 0x848484ffU;
    for (i = 0U; i < sizeof(rgba_cases) / sizeof(rgba_cases[0]); ++i) {
        uint8_t request[40];
        uint8_t span[16];
        uint32_t reply_bytes = 0U;

        put_blit_request(request);
        mw_put_be32(request + 4, rgba_cases[i].format);
        mw_put_be32(request + 8, 2U * rgba_cases[i].bpp);
        memset(span, 0, sizeof(span));
        assert(mw_host_request(c, request, sizeof(request), span,
                               4U * rgba_cases[i].bpp, &reply_bytes) == MW_STATUS_OK);
        assert(memcmp(span, rgba_cases[i].bytes, 4U * rgba_cases[i].bpp) == 0);
    }
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_rasterix_rgb565_present_formats\n");
}

/* A second context binding its buffers must not strand the first one's
 * RasterIX frame: the first context's next packet still reads its pixels. */
static void test_second_context_keeps_first_frame(void)
{
    static const uint8_t expected[16] = {
        0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
        0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    };
    static const uint8_t sync_payload[4] = { 0U, 0U, 0U, 0U };
    MiniGLRixPlatform platform;
    mw_ctx *first;
    mw_ctx *second;
    uint16_t *surface;

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    first = mw_host_create(2U, 2U);
    assert(first != NULL);
    first->state.target.latest = first->state.target.back;
    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    surface = (uint16_t *)first->color_buffers[first->state.target.latest];
    surface[0] = 0xf800U;
    surface[1] = 0x07e0U;
    surface[2] = 0x001fU;
    surface[3] = 0xffffU;

    second = mw_host_create(2U, 2U);
    assert(second != NULL);
    assert(submit_single_command(second, MW_SYNC, sync_payload, 4U) == MW_STATUS_OK);
    assert(submit_single_command(first, MW_SYNC, sync_payload, 4U) == MW_STATUS_OK);
    assert_blit_pixels(first, expected);

    mw_host_destroy(second);
    mw_host_destroy(first);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_second_context_keeps_first_frame\n");
}

static void test_backend_disable_cpu_rerender_present(void)
{
    static const uint8_t expected[16] = {
        0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU,
        0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    };
    MiniGLRixPlatform platform;
    mw_ctx *c;
    uint32_t *old_color;
    uint8_t *old_depth;
    uint32_t old_pixels[4];

    setup_mock_platform(&s_mock, &platform);
    s_mock.complete_submissions = 0;
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);
    c->state.target.latest = c->state.target.back;
    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    old_color = c->color_buffers[c->state.target.back];
    old_depth = c->depth_buffer;
    memcpy(old_pixels, old_color, sizeof(old_pixels));
    assert(!minigl_raster_prepare_present()); /* Timeout disables RasterIX. */
    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(c->rix_target_quarantined);
    assert(c->color_buffers[c->state.target.back] != old_color);
    assert(c->depth_buffer != old_depth);
    assert(memcmp(old_color, old_pixels, sizeof(old_pixels)) == 0);
    assert(!minigl_raster_present_is_rgb565());
    assert(!minigl_raster_present_is_bottom_up());
    c->color_buffers[c->state.target.latest][0] = 0xff0000ffU;
    c->color_buffers[c->state.target.latest][1] = 0x00ff00ffU;
    c->color_buffers[c->state.target.latest][2] = 0x0000ffffU;
    c->color_buffers[c->state.target.latest][3] = 0xffffffffU;
    assert_blit_pixels(c, expected);
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_backend_disable_cpu_rerender_present\n");
}

static uint32_t submit_single_command(mw_ctx *c, uint32_t opcode,
                                      const uint8_t *payload, uint32_t payload_bytes)
{
    uint8_t packet[64];
    uint32_t packet_bytes = 24U + payload_bytes;

    assert(packet_bytes <= sizeof(packet));
    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, packet_bytes);
    mw_put_be32(packet + 12, 1U);
    mw_put_be32(packet + 16, opcode);
    mw_put_be32(packet + 20, payload_bytes);
    if (payload_bytes != 0U)
        memcpy(packet + 24, payload, payload_bytes);
    return mw_host_submit(c, packet, packet_bytes);
}

static void test_wire_cpu_handoffs(void)
{
    MiniGLRixPlatform platform;
    mw_ctx *c;
    uint8_t upload[20] = { 0 };
    uint8_t copy[40] = { 0 };
    uint8_t target[24] = { 0 };
    uint8_t draw_buffer[12] = { 0 };

    setup_mock_platform(&s_mock, &platform);
    minigl_raster_set_rix_platform(&platform);
    c = mw_host_create(2U, 2U);
    assert(c != NULL);

    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    mw_put_be32(upload + 8, 1U);
    mw_put_be32(upload + 12, 1U);
    upload[16] = 0x12U;
    upload[17] = 0x34U;
    upload[18] = 0x56U;
    upload[19] = 0x78U;
    assert(submit_single_command(c, MW_UPLOAD, upload, sizeof(upload)) == MW_STATUS_OK);
    assert(!minigl_raster_present_is_rgb565());
    assert(c->color_buffers[c->state.target.back][0] == 0x12345678U);

    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    mw_put_be32(copy, 1U);
    mw_put_be32(copy + 8, 0x1908U);
    mw_put_be32(copy + 28, 1U);
    mw_put_be32(copy + 32, 1U);
    mw_put_be32(copy + 36, 1U);
    assert(submit_single_command(c, MW_TEXTURE_COPY, copy, sizeof(copy)) == MW_STATUS_OK);
    assert(!minigl_raster_present_is_rgb565());

    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    mw_put_be32(target, 4U);
    mw_put_be32(target + 4, 2U);
    mw_put_be32(target + 8, 2U);
    mw_put_be32(target + 12, 24U);
    mw_put_be32(target + 16, 4U);
    mw_put_be32(target + 20, 2U);
    assert(submit_single_command(c, MW_TARGET, target, sizeof(target)) == MW_STATUS_OK);
    assert(c->width == 4U && c->height == 2U);
    assert(!minigl_raster_present_is_rgb565());

    minigl_raster_clear(0x4000U, 0U, 0U);
    assert(minigl_raster_present_is_rgb565());
    mw_put_be32(draw_buffer, MW_DRAW_BUFFER);
    mw_put_be32(draw_buffer + 4, 1U);
    mw_put_be32(draw_buffer + 8, 0x404U);
    assert(submit_single_command(c, MW_STATE, draw_buffer, sizeof(draw_buffer)) == MW_STATUS_OK);
    assert(!minigl_raster_present_is_rgb565());

    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    printf("PASS: test_wire_cpu_handoffs\n");
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
    test_cpu_present_orientation();
    test_rasterix_rgb565_present_orientation();
    test_rasterix_rgb565_span_presentation();
    test_rasterix_rgb565_present_formats();
    test_second_context_keeps_first_frame();
    test_points_render_on_rasterix();
    test_triangles_sample_pixel_centres();
    test_backend_disable_cpu_rerender_present();
    test_wire_cpu_handoffs();
    test_watertight_tessellation();
    printf("ALL Wire-v9 RasterIX Mapper Tests PASSED!\n");
    return 0;
}
