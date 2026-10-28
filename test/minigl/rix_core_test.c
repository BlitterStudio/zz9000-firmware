/*
 * RasterIX command/setup/binning host tests
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rix_bin.h"
#include "rix_cmd.h"
#include "rix_setup.h"

#define RIX_SEEDED_STREAM_HASH 0xf8ea48fcU

static uint32_t word_at(const uint8_t *bytes, size_t word)
{
    const uint8_t *p = bytes + word * 4U;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float triangle_float(const RixTriangle *triangle, unsigned int word)
{
    float value;

    memcpy(&value, &triangle->words[word], sizeof(value));
    return value;
}

static void assert_rebased_components(const RixTriangle *unclipped,
                                      const RixTriangle *clipped,
                                      unsigned int value_word,
                                      unsigned int x_word,
                                      unsigned int y_word,
                                      unsigned int count,
                                      int32_t dx, int32_t dy)
{
    unsigned int i;

    for (i = 0U; i < count; ++i) {
        float expected = triangle_float(unclipped, value_word + i) +
                         triangle_float(unclipped, x_word + i) * (float)dx +
                         triangle_float(unclipped, y_word + i) * (float)dy;

        assert(fabsf(triangle_float(clipped, value_word + i) - expected) < 0.00001f);
    }
}

static RixVertex vertex(float x, float y, float z, float w,
                        uint32_t rgba, float s, float t, float q)
{
    RixVertex v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    v.rgba = rgba;
    v.s = s;
    v.t = t;
    v.q = q;
    return v;
}

static void test_register_and_stream_encoding(void)
{
    uint8_t bytes[512];
    RixCmdWriter writer;
    RixFragmentState fragment;
    RixTexEnvState env;
    RixTextureState texture;
    uint32_t pages[2] = { 0x1c000000U, 0x1c000800U };
    float fog[33];
    size_t i;

    memset(&fragment, 0, sizeof(fragment));
    memset(&env, 0, sizeof(env));
    memset(&texture, 0, sizeof(texture));
    for (i = 0; i < 33U; ++i) {
        fog[i] = (float)i / 32.0f;
    }
    fragment.depth_func = RIX_FUNC_LEQUAL;
    fragment.alpha_func = RIX_FUNC_GREATER;
    fragment.alpha_ref = 0x5aU;
    fragment.depth_mask = 1U;
    fragment.color_mask_r = 1U;
    fragment.color_mask_g = 1U;
    fragment.color_mask_b = 1U;
    fragment.color_mask_a = 1U;
    fragment.src_factor = RIX_BLEND_SRC_ALPHA;
    fragment.dst_factor = RIX_BLEND_ONE_MINUS_SRC_ALPHA;
    env.combine_rgb = RIX_TEX_MODULATE;
    env.combine_alpha = RIX_TEX_REPLACE;
    env.src_rgb[0] = RIX_TEXSRC_TEXTURE;
    env.src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
    env.src_alpha[0] = RIX_TEXSRC_TEXTURE;
    env.operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
    env.operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    texture.width_log2 = 8U;
    texture.height_log2 = 8U;
    texture.clamp_s = 1U;
    texture.mag_linear = 1U;
    texture.pixel_format = RIX_TEXTURE_RGB565;

    rix_cmd_init(&writer, bytes, sizeof(bytes));
    assert(rix_cmd_feature_enable(&writer, RIX_FEATURE_DEPTH | RIX_FEATURE_TMU0));
    assert(rix_cmd_fragment_state(&writer, &fragment));
    assert(rix_cmd_tex_env(&writer, 0U, &env));
    assert(rix_cmd_texture_state(&writer, 0U, &texture));
    assert(rix_cmd_texture_pages(&writer, 0U, pages, 2U));
    assert(rix_cmd_fog_lut(&writer, fog));
    assert(word_at(bytes, 0U) == 0x10000000U);
    assert(word_at(bytes, 1U) == (RIX_FEATURE_DEPTH | RIX_FEATURE_TMU0));
    assert(word_at(bytes, 2U) == 0x10000003U);
    assert(word_at(bytes, 3U) == 0x03b7d6acU);
    assert(word_at(bytes, 8U) == 0x50000002U);
    assert(word_at(bytes, 9U) == pages[0]);
    assert(word_at(bytes, 10U) == pages[1]);
    assert(word_at(bytes, 11U) == 0x40000100U);
    assert(rix_cmd_words(&writer) == 76U);
}

static void test_triangle_setup_and_bands(void)
{
    RixVertex v0 = vertex(10.25f, 4.5f, 0.1f, 1.0f, 0xff0000ffU, 0.0f, 0.0f, 1.0f);
    RixVertex v1 = vertex(100.75f, 40.125f, 0.6f, 0.5f, 0x00ff00ffU, 1.0f, 0.0f, 1.0f);
    RixVertex v2 = vertex(20.0f, 88.875f, 0.9f, 0.25f, 0x0000ffffU, 0.0f, 1.0f, 1.0f);
    RixTriangle triangle;
    RixBandList bands;
    RixTriangle *references[32];
    RixBandConfig config;
    uint8_t bytes[8192];
    RixCmdWriter writer;
    size_t band;
    size_t word;
    unsigned commits = 0U;
    assert(rix_setup_triangle(&triangle, &v0, &v1, &v2, NULL));
    assert(triangle.words[1] == 0x0005000aU);
    assert(triangle.words[2] == 0x005a0066U);
    assert(!rix_setup_triangle(&triangle, &v0, &v0, &v2, NULL));

    memset(&config, 0, sizeof(config));
    config.width = 640U;
    config.height = 480U;
    config.color_addr = 0x07820000U;
    config.depth_addr = 0x07b20000U;
    config.color_pitch = 1280U;
    config.depth_pitch = 1280U;
    config.clear_color = 0x11223344U;
    config.clear_depth = 0xffffU;
    config.clear_mask = RIX_CLEAR_COLOR | RIX_CLEAR_DEPTH;
    assert(rix_band_list_init(&bands, 640U, 480U,
                              references, sizeof(references) / sizeof(references[0])));
    /* Complete layout: 16384 / 640 = 25 lines per band, 20 bands, and the
     * bottom-up row ranges start at the topmost screen band. */
    assert(bands.count == 20U);
    assert(bands.band[0U].y_start == 455U && bands.band[0U].y_end == 480U);
    assert(bands.band[18U].y_start == 5U && bands.band[18U].y_end == 30U);
    assert(bands.band[19U].y_start == 0U && bands.band[19U].y_end == 5U);
    assert(rix_band_list_add(&bands, &triangle));
    for (band = 0; band < bands.count; ++band) {
        if (bands.band[band].triangle_count != 0U) {
            assert(bands.band[band].y_start <= triangle.words[2] >> 16);
        }
    }
    rix_cmd_init(&writer, bytes, sizeof(bytes));
    assert(rix_band_emit_frame(&writer, &bands, &config));
    assert(rix_cmd_words(&writer) != 0U);
    for (word = 0U; word < rix_cmd_words(&writer); ++word) {
        uint32_t command = word_at(bytes, word);

        if ((command & 0xf0000000U) == RIX_OP_FRAMEBUFFER &&
            (command & RIX_FRAMEBUFFER_COMMIT) != 0U) {
            assert((command & (RIX_FRAMEBUFFER_COLOR | RIX_FRAMEBUFFER_DEPTH)) ==
                   RIX_FRAMEBUFFER_COLOR);
            ++commits;
        }
    }
    assert(commits == bands.count);
}

static void test_scissor_clips_descriptor_and_interpolation(void)
{
    RixVertex v0 = vertex(10.25f, 4.5f, 0.1f, 1.0f, 0xff0000ffU, 0.0f, 0.0f, 1.0f);
    RixVertex v1 = vertex(100.75f, 40.125f, 0.6f, 0.5f, 0x00ff00ffU, 1.0f, 0.0f, 1.0f);
    RixVertex v2 = vertex(20.0f, 88.875f, 0.9f, 0.25f, 0x0000ffffU, 0.0f, 1.0f, 1.0f);
    RixScissor scissor = { 20U, 10U, 80U, 70U, 1U };
    RixTriangle unclipped;
    RixTriangle clipped;
    int32_t dx;
    int32_t dy;
    unsigned int i;

    assert(rix_setup_triangle(&unclipped, &v0, &v1, &v2, NULL));
    assert(rix_setup_triangle(&clipped, &v0, &v1, &v2, &scissor));
    assert(clipped.words[RIX_WORD_BB_START] == 0x000a0014U);
    assert(clipped.words[RIX_WORD_BB_END] == 0x00460050U);
    dx = (int32_t)(uint16_t)clipped.words[RIX_WORD_BB_START] -
         (int32_t)(uint16_t)unclipped.words[RIX_WORD_BB_START];
    dy = (int32_t)(uint16_t)(clipped.words[RIX_WORD_BB_START] >> 16U) -
         (int32_t)(uint16_t)(unclipped.words[RIX_WORD_BB_START] >> 16U);
    assert(dx == 10 && dy == 5);
    for (i = 0U; i < 3U; ++i) {
        int32_t expected = (int32_t)unclipped.words[RIX_WORD_W_INIT + i] +
                           (int32_t)unclipped.words[RIX_WORD_W_X + i] * dx +
                           (int32_t)unclipped.words[RIX_WORD_W_Y + i] * dy;

        assert(clipped.words[RIX_WORD_W_INIT + i] == (uint32_t)expected);
    }
    assert_rebased_components(&unclipped, &clipped, RIX_WORD_COLOR,
                              RIX_WORD_COLOR_X, RIX_WORD_COLOR_Y, 4U, dx, dy);
    assert_rebased_components(&unclipped, &clipped, RIX_WORD_DEPTH,
                              RIX_WORD_DEPTH_X, RIX_WORD_DEPTH_Y, 2U, dx, dy);
    assert_rebased_components(&unclipped, &clipped, RIX_WORD_TEXTURE,
                              RIX_WORD_TEXTURE_X, RIX_WORD_TEXTURE_Y, 3U, dx, dy);
}

static void test_odd_widths_emit_no_commands(void)
{
    RixBandList bands;
    RixBandConfig config;
    RixBandSetup setup;
    RixTriangle *references[1] = { NULL };
    RixCmdWriter writer;
    uint8_t bytes[128];

    assert(!rix_band_list_init(&bands, 1U, 1U, references, 1U));
    assert(!rix_band_list_init(&bands, 641U, 480U, references, 1U));
    assert(rix_band_list_init(&bands, 2U, 1U, references, 1U));

    memset(&setup, 0, sizeof(setup));
    setup.x_resolution = 1U;
    setup.y_resolution = 1U;
    rix_cmd_init(&writer, bytes, sizeof(bytes));
    assert(!rix_band_emit_header(&writer, &setup));
    assert(rix_cmd_words(&writer) == 0U);

    memset(&config, 0, sizeof(config));
    config.width = 1U;
    config.height = 1U;
    config.color_pitch = 2U;
    config.depth_pitch = 2U;
    config.clear_mask = RIX_CLEAR_COLOR | RIX_CLEAR_DEPTH;
    bands.width = 1U;
    rix_cmd_init(&writer, bytes, sizeof(bytes));
    assert(!rix_band_emit_frame(&writer, &bands, &config));
    assert(rix_cmd_words(&writer) == 0U);
}

static void test_band_protocol_stream(void)
{
    static const uint16_t dimensions[][2] = {
        { 640U, 480U }, { 800U, 600U }, { 1024U, 768U },
        { 640U, 481U }, { 800U, 603U }, { 1024U, 769U }
    };
    RixTriangle *references[1] = { NULL };
    uint8_t bytes[16384];
    size_t dimension;

    for (dimension = 0U; dimension < sizeof(dimensions) / sizeof(dimensions[0]); ++dimension) {
        RixBandList bands;
        RixBandConfig config;
        RixCmdWriter writer;
        RixBandSetup setup;
        size_t word;
        size_t band = 0U;
        uint32_t committed = 0U;

        memset(&config, 0, sizeof(config));
        config.width = dimensions[dimension][0];
        config.height = dimensions[dimension][1];
        config.color_addr = 0x07820000U;
        config.depth_addr = 0x07b20000U;
        config.color_pitch = (uint32_t)config.width * 2U;
        config.depth_pitch = (uint32_t)config.width * 2U;
        assert(rix_band_list_init(&bands, config.width, config.height,
                                  references, 1U));
        /* Bands tile the screen top-down (band 0 holds the topmost rows)
         * while their absolute bottom-up ranges abut towards zero. */
        for (word = 0U; word + 1U < bands.count; ++word)
            assert(bands.band[word].y_start == bands.band[word + 1U].y_end);
        assert(bands.band[bands.count - 1U].y_start == 0U);
        assert(bands.band[0U].y_end == config.height);
        assert((uint32_t)(bands.band[bands.count - 1U].y_end -
                          bands.band[bands.count - 1U].y_start) <= 16384U / config.width);
        rix_cmd_init(&writer, bytes, sizeof(bytes));
        assert(rix_band_emit_frame(&writer, &bands, &config));
        rix_band_setup(&setup, &bands, &config, 0U);
        for (word = 0U; word < rix_cmd_words(&writer); ++word) {
            uint32_t command = word_at(bytes, word);

            if ((command & 0xf0000000U) == RIX_OP_RENDER_CONFIG) {
                if ((command & 0x0fffffffU) == RIX_REG_RESOLUTION) {
                    rix_band_setup(&setup, &bands, &config, (uint16_t)band);
                    assert(word_at(bytes, word + 1U) ==
                           ((uint32_t)setup.x_resolution |
                            ((uint32_t)setup.y_resolution << 16U)));
                } else if ((command & 0x0fffffffU) == RIX_REG_Y_OFFSET) {
                    assert(word_at(bytes, word + 1U) ==
                           ((uint32_t)setup.y_offset << 16U));
                } else if ((command & 0x0fffffffU) == RIX_REG_COLOR_BUFFER_ADDR) {
                    assert(word_at(bytes, word + 1U) == setup.color_addr);
                } else if ((command & 0x0fffffffU) == RIX_REG_DEPTH_BUFFER_ADDR) {
                    assert(word_at(bytes, word + 1U) == setup.depth_addr);
                }
            } else if ((command & 0xf0000000U) == RIX_OP_FRAMEBUFFER &&
                       (command & RIX_FRAMEBUFFER_COMMIT) != 0U) {
                uint32_t pixels = (command >> 8U) & 0xfffffU;

                assert(pixels == setup.pixels);
                assert(pixels <= 16384U);
                committed += pixels;
                ++band;
            }
        }
        assert(band == bands.count);
        assert(committed == (uint32_t)config.width * config.height);
    }
}

static RixTriangle seeded_triangles[2000];
static RixTriangle *seeded_references[40000];
static uint8_t seeded_stream[8U * 1024U * 1024U];

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static float random_coordinate(uint32_t *state, float extent)
{
    return ((float)(next_random(state) >> 8) / 16777215.0f) * extent - extent / 8.0f;
}


static void test_seeded_gears_like_mesh(void)
{
    RixBandList bands;
    RixBandConfig config;
    RixCmdWriter writer;
    uint32_t random = 0x5eed1234U;
    unsigned int accepted = 0U;
    unsigned int i;

    memset(&config, 0, sizeof(config));
    config.width = 640U;
    config.height = 480U;
    config.color_addr = 0x07820000U;
    config.depth_addr = 0x07b20000U;
    config.color_pitch = 1280U;
    config.depth_pitch = 1280U;
    assert(rix_band_list_init(&bands, config.width, config.height,
                              seeded_references,
                              sizeof(seeded_references) / sizeof(seeded_references[0])));
    for (i = 0U; i < 2000U; ++i) {
        RixVertex a = vertex(random_coordinate(&random, 720.0f),
                             random_coordinate(&random, 560.0f),
                             random_coordinate(&random, 1.0f), 0.25f + random_coordinate(&random, 1.0f),
                             next_random(&random), random_coordinate(&random, 8.0f),
                             random_coordinate(&random, 8.0f), 1.0f);
        RixVertex b = vertex(random_coordinate(&random, 720.0f),
                             random_coordinate(&random, 560.0f),
                             random_coordinate(&random, 1.0f), 0.25f + random_coordinate(&random, 1.0f),
                             next_random(&random), random_coordinate(&random, 8.0f),
                             random_coordinate(&random, 8.0f), 1.0f);
        RixVertex c = vertex(random_coordinate(&random, 720.0f),
                             random_coordinate(&random, 560.0f),
                             random_coordinate(&random, 1.0f), 0.25f + random_coordinate(&random, 1.0f),
                             next_random(&random), random_coordinate(&random, 8.0f),
                             random_coordinate(&random, 8.0f), 1.0f);
        if (rix_setup_triangle(&seeded_triangles[accepted], &a, &b, &c, NULL)) {
            assert(rix_band_list_add(&bands, &seeded_triangles[accepted]));
            ++accepted;
        }
    }
    assert(accepted > 1900U);
    rix_cmd_init(&writer, seeded_stream, sizeof(seeded_stream));
    assert(rix_band_emit_frame(&writer, &bands, &config));
    assert(rix_cmd_words(&writer) > 2000U);
}

int main(void)
{
    test_register_and_stream_encoding();
    test_triangle_setup_and_bands();
    test_scissor_clips_descriptor_and_interpolation();
    test_odd_widths_emit_no_commands();
    test_band_protocol_stream();
    test_seeded_gears_like_mesh();
    puts("rix core tests passed");
    return 0;
}
