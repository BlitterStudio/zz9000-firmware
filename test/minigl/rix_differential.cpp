/*
 * RasterIX upstream differential command test.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Uses RasterIX commit 9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0 as the
 * independent descriptor/command oracle.  The runner supplies its source root.
 */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "rix_cmd.h"
#include "rix_setup.h"
#include "rix_texture.h"
}

#include "renderer/Rasterizer.hpp"
#include "renderer/commands/FogLutStreamCmd.hpp"
#include "renderer/commands/FramebufferCmd.hpp"
#include "renderer/commands/TextureStreamCmd.hpp"
#include "renderer/commands/TriangleStreamCmd.hpp"
#include "renderer/commands/WriteRegisterCmd.hpp"
#include "ImageConverter.hpp"

/* ImageConverter only reaches this singleton for unsupported client formats;
 * the differential passes GL_RGBA/GL_UNSIGNED_BYTE exclusively. */
rr::RIXGL& rr::RIXGL::getInstance()
{
    std::abort();
}

static RixVertex make_vertex(float x, float y, float z, float w, uint32_t rgba,
                             float s, float t, float q)
{
    return RixVertex { x, y, z, w, rgba, s, t, q };
}

static float component(uint32_t rgba, unsigned index)
{
    return static_cast<float>((rgba >> (24U - index * 8U)) & 0xffU) / 255.0f;
}

static rr::Vec4 upstream_vertex(const RixVertex& input)
{
    return rr::Vec4 { input.x, input.y, input.z, input.w };
}

static rr::Vec4 upstream_color(const RixVertex& input)
{
    return rr::Vec4 { component(input.rgba, 0U), component(input.rgba, 1U),
                      component(input.rgba, 2U), component(input.rgba, 3U) };
}

static std::array<rr::Vec4, rr::RenderConfig::TMU_COUNT>
upstream_texture(const RixVertex& input)
{
    return { rr::Vec4 { input.s, input.t, 0.0f, input.q } };
}

static unsigned int comparison_case;

static void compare_triangle(const RixVertex& a, const RixVertex& b, const RixVertex& c)
{
    RixTriangle ours {};
    rr::Rasterizer rasterizer { true };
    const rr::Vec4 av = upstream_vertex(a), bv = upstream_vertex(b), cv = upstream_vertex(c);
    const rr::Vec4 ac = upstream_color(a), bc = upstream_color(b), cc = upstream_color(c);
    const auto at = upstream_texture(a), bt = upstream_texture(b), ct = upstream_texture(c);
    const rr::TransformedTriangle triangle { av, bv, cv, at, bt, ct, ac, bc, cc };

    ++comparison_case;
    rasterizer.enableTmu(0U, true);
    rr::TriangleStreamCmd upstream { rasterizer, triangle };
    assert(rix_setup_triangle(&ours, &a, &b, &c, nullptr) == (upstream.isVisible() ? 1 : 0));
    if (upstream.isVisible()) {
        const auto payload = upstream.payload();
        static_assert(sizeof(RixTriangle) == sizeof(rr::TriangleStreamCmd::TrDesc));
        assert(payload.size() == 1U);
        /* TriangleDesc::param.reserved is intentionally unspecified by the
         * upstream constructor and ignored by RTL; compare defined words. */
        for (size_t i = 1U; i < RIX_TRIANGLE_WORDS; ++i) {
            if (ours.words[i] != reinterpret_cast<const uint32_t *>(payload.data())[i]) {
                std::fprintf(stderr, "triangle case %u word %zu ours=%08x upstream=%08x\n",
                             comparison_case, i, ours.words[i],
                             reinterpret_cast<const uint32_t *>(payload.data())[i]);
                std::fprintf(stderr, "vertices %.9g,%.9g %.9g,%.9g %.9g,%.9g\n",
                             a.x, a.y, b.x, b.y, c.x, c.y);
                assert(false);
            }
        }
    }
}

static uint32_t next_random(uint32_t& state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

static float random_coordinate(uint32_t& state, float extent)
{
    return (static_cast<float>(next_random(state) >> 8) / 16777215.0f) * extent - extent / 8.0f;
}

static void test_triangles(void)
{
    uint32_t random = 0x5eed1234U;
    compare_triangle(make_vertex(10.25f, 4.5f, 0.1f, 1.0f, 0xff0000ffU, 0.0f, 0.0f, 1.0f),
                     make_vertex(100.75f, 40.125f, 0.6f, 0.5f, 0x00ff00ffU, 1.0f, 0.0f, 1.0f),
                     make_vertex(20.0f, 88.875f, 0.9f, 0.25f, 0x0000ffffU, 0.0f, 1.0f, 1.0f));
    for (unsigned i = 0; i < 2000U; ++i) {
        const RixVertex a = make_vertex(random_coordinate(random, 720.0f), random_coordinate(random, 560.0f),
                                         random_coordinate(random, 1.0f), 0.25f + random_coordinate(random, 1.0f),
                                         next_random(random), random_coordinate(random, 8.0f), random_coordinate(random, 8.0f), 1.0f);
        const RixVertex b = make_vertex(random_coordinate(random, 720.0f), random_coordinate(random, 560.0f),
                                         random_coordinate(random, 1.0f), 0.25f + random_coordinate(random, 1.0f),
                                         next_random(random), random_coordinate(random, 8.0f), random_coordinate(random, 8.0f), 1.0f);
        const RixVertex c = make_vertex(random_coordinate(random, 720.0f), random_coordinate(random, 560.0f),
                                         random_coordinate(random, 1.0f), 0.25f + random_coordinate(random, 1.0f),
                                         next_random(random), random_coordinate(random, 8.0f), random_coordinate(random, 8.0f), 1.0f);
        compare_triangle(a, b, c);
    }
    /* Explicit sub-pixel sliver, band straddle, huge off-screen, and degenerate. */
    compare_triangle(make_vertex(0.01f, 0.01f, 0.0f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f),
                     make_vertex(0.02f, 400.0f, 1.0f, 1.0f, 0xffffffffU, 1.0f, 0.0f, 1.0f),
                     make_vertex(0.03f, 479.9f, 0.5f, 1.0f, 0xffffffffU, 0.0f, 1.0f, 1.0f));
    compare_triangle(make_vertex(-2000.0f, -2000.0f, 0.0f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f),
                     make_vertex(3000.0f, 0.0f, 1.0f, 1.0f, 0xffffffffU, 1.0f, 0.0f, 1.0f),
                     make_vertex(0.0f, 3000.0f, 0.5f, 1.0f, 0xffffffffU, 0.0f, 1.0f, 1.0f));
    compare_triangle(make_vertex(20.0f, 20.0f, 0.0f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f),
                     make_vertex(20.0f, 20.0f, 0.0f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f),
                     make_vertex(20.0f, 20.0f, 0.0f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f));
}

static uint32_t le_word(const uint8_t *bytes, size_t index)
{
    const uint8_t *p = bytes + index * 4U;
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

template <typename Register>
static void compare_register(const RixCmdWriter& ours, const Register& reg)
{
    rr::WriteRegisterCmd upstream { rr::RegisterVariant { reg } };
    assert(ours.length == 8U);
    assert(le_word(ours.bytes, 0U) == upstream.command());
    assert(le_word(ours.bytes, 1U) == upstream.payload()[0]);
}

template <typename Register>
static void compare_masked_register(const RixCmdWriter& ours, const Register& reg, uint32_t mask)
{
    rr::WriteRegisterCmd upstream { rr::RegisterVariant { reg } };
    assert(ours.length == 8U);
    assert(le_word(ours.bytes, 0U) == upstream.command());
    assert(le_word(ours.bytes, 1U) == (upstream.payload()[0] & mask));
}

static void test_commands(void)
{
    uint8_t bytes[512] {};
    RixCmdWriter ours;
    std::array<float, 33> fog {};
    std::array<std::size_t, 3> page_numbers { 0U, 1U, 2U };
    uint32_t pages[3] { 0x1c000000U, 0x1c000800U, 0x1c001000U };
    rr::FogLutStreamCmd upstream_fog;
    rr::TextureStreamCmd upstream_texture { 0U, page_numbers };
    rr::FramebufferCmd upstream_framebuffer { true, true, false, 16000U };
    for (size_t i = 0; i < fog.size(); ++i) fog[i] = static_cast<float>(i) / 32.0f;
    upstream_fog = rr::FogLutStreamCmd { fog };
    upstream_framebuffer.enableMemset();
    upstream_framebuffer.commitFramebuffer();

    rix_cmd_init(&ours, bytes, sizeof(bytes));
    assert(rix_cmd_fog_lut(&ours, fog.data()));
    assert(le_word(bytes, 0U) == upstream_fog.command());
    for (size_t i = 0; i < upstream_fog.payload().size(); ++i)
        assert(le_word(bytes, i + 1U) == static_cast<uint32_t>(upstream_fog.payload()[i]));

    rix_cmd_init(&ours, bytes, sizeof(bytes));
    assert(rix_cmd_texture_pages(&ours, 0U, pages, 3U));
    assert(le_word(bytes, 0U) == upstream_texture.command());
    for (size_t i = 0; i < upstream_texture.payload().size(); ++i)
        assert(le_word(bytes, i + 1U) == upstream_texture.payload()[i]);

    rix_cmd_init(&ours, bytes, sizeof(bytes));
    assert(rix_cmd_framebuffer(&ours, RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COMMIT |
                               RIX_FRAMEBUFFER_COLOR | RIX_FRAMEBUFFER_DEPTH, 16000U));
    /* Upstream's generic constructor page-aligns a transfer to 16,384 pixels.
     * ZZ9000's IF writer deliberately emits its exact 16,000-pixel request so
     * it cannot overrun a 25-line destination band.  The operation bits remain
     * an upstream byte-for-byte match. */
    assert((le_word(bytes, 0U) & 0xf00000ffU) == (upstream_framebuffer.command() & 0xf00000ffU));
    assert((le_word(bytes, 0U) >> 8U & 0xfffffU) == 16000U);
    assert((upstream_framebuffer.command() >> 8U & 0xfffffU) == 16384U);
}

static void test_render_registers(void)
{
    uint8_t bytes[16] {};
    RixCmdWriter ours;
    uint32_t random = 0x9f46a311U;
    for (unsigned i = 0; i < 256U; ++i) {
        const uint32_t value = next_random(random);
        const uint16_t x0 = static_cast<uint16_t>(value);
        const uint16_t y0 = static_cast<uint16_t>(value >> 16);
        const uint16_t x1 = static_cast<uint16_t>(next_random(random));
        const uint16_t y1 = static_cast<uint16_t>(next_random(random) >> 16);
        const uint32_t rgba = next_random(random);
        RixFragmentState fragment {};
        RixTexEnvState texenv {};
        RixTextureState texture {};
        rr::FeatureEnableReg features;
        rr::FragmentPipelineReg upstream_fragment;
        rr::TexEnvReg upstream_texenv;
        rr::TmuTextureReg upstream_texture;
        rr::ColorBufferClearColorReg clear_color;
        rr::DepthBufferClearDepthReg clear_depth;
        rr::FogColorReg fog_color;
        rr::TexEnvColorReg texenv_color;
        rr::RenderResolutionReg resolution;
        rr::YOffsetReg yoffset;
        rr::ScissorStartReg scissor_start;
        rr::ScissorEndReg scissor_end;
        rr::ColorBufferAddrReg color_buffer;
        rr::DepthBufferAddrReg depth_buffer;
        rr::StencilBufferAddrReg stencil_buffer;

        features.setEnableFog((value & RIX_FEATURE_FOG) != 0U);
        features.setEnableBlending((value & RIX_FEATURE_BLEND) != 0U);
        features.setEnableDepthTest((value & RIX_FEATURE_DEPTH) != 0U);
        features.setEnableAlphaTest((value & RIX_FEATURE_ALPHA) != 0U);
        features.setEnableStencilTest((value & RIX_FEATURE_STENCIL) != 0U);
        features.setEnableScissor((value & RIX_FEATURE_SCISSOR) != 0U);
        features.setEnableTmu(0U, (value & RIX_FEATURE_TMU0) != 0U);
        features.setEnableLogicOp((value & RIX_FEATURE_LOGIC) != 0U);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_feature_enable(&ours, value));
        /* Upstream leaves FeatureEnableReg's reserved high bits unspecified. */
        compare_masked_register(ours, features, 0x17fU);

        fragment.depth_func = static_cast<uint8_t>(next_random(random) & 7U);
        fragment.alpha_func = static_cast<uint8_t>(next_random(random) & 7U);
        fragment.alpha_ref = static_cast<uint8_t>(next_random(random));
        fragment.depth_mask = static_cast<uint8_t>(next_random(random) & 1U);
        fragment.color_mask_r = static_cast<uint8_t>(next_random(random) & 1U);
        fragment.color_mask_g = static_cast<uint8_t>(next_random(random) & 1U);
        fragment.color_mask_b = static_cast<uint8_t>(next_random(random) & 1U);
        fragment.color_mask_a = static_cast<uint8_t>(next_random(random) & 1U);
        fragment.src_factor = static_cast<uint8_t>(next_random(random) % 11U);
        fragment.dst_factor = static_cast<uint8_t>(next_random(random) % 11U);
        fragment.logic_op = static_cast<uint8_t>(next_random(random) & 15U);
        upstream_fragment.setDepthFunc(static_cast<rr::TestFunc>(fragment.depth_func));
        upstream_fragment.setAlphaFunc(static_cast<rr::TestFunc>(fragment.alpha_func));
        upstream_fragment.setRefAlphaValue(fragment.alpha_ref);
        upstream_fragment.setDepthMask(fragment.depth_mask != 0U);
        upstream_fragment.setColorMaskR(fragment.color_mask_r != 0U);
        upstream_fragment.setColorMaskG(fragment.color_mask_g != 0U);
        upstream_fragment.setColorMaskB(fragment.color_mask_b != 0U);
        upstream_fragment.setColorMaskA(fragment.color_mask_a != 0U);
        upstream_fragment.setBlendFuncSFactor(static_cast<rr::BlendFunc>(fragment.src_factor));
        upstream_fragment.setBlendFuncDFactor(static_cast<rr::BlendFunc>(fragment.dst_factor));
        upstream_fragment.setLogicOp(static_cast<rr::LogicOp>(fragment.logic_op));
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_fragment_state(&ours, &fragment));
        compare_masked_register(ours, upstream_fragment, 0x7fffffffU);

        texenv.combine_rgb = static_cast<uint8_t>(next_random(random) & 7U);
        texenv.combine_alpha = static_cast<uint8_t>(next_random(random) & 7U);
        for (unsigned channel = 0; channel < 3U; ++channel) {
            texenv.src_rgb[channel] = static_cast<uint8_t>(next_random(random) & 3U);
            texenv.src_alpha[channel] = static_cast<uint8_t>(next_random(random) & 3U);
            texenv.operand_rgb[channel] = static_cast<uint8_t>(next_random(random) & 3U);
            texenv.operand_alpha[channel] = static_cast<uint8_t>(next_random(random) & 1U);
        }
        texenv.shift_rgb = static_cast<uint8_t>(next_random(random) & 3U);
        texenv.shift_alpha = static_cast<uint8_t>(next_random(random) & 3U);
        upstream_texenv.setCombineRgb(static_cast<rr::Combine>(texenv.combine_rgb));
        upstream_texenv.setCombineAlpha(static_cast<rr::Combine>(texenv.combine_alpha));
        upstream_texenv.setSrcRegRgb0(static_cast<rr::SrcReg>(texenv.src_rgb[0]));
        upstream_texenv.setSrcRegRgb1(static_cast<rr::SrcReg>(texenv.src_rgb[1]));
        upstream_texenv.setSrcRegRgb2(static_cast<rr::SrcReg>(texenv.src_rgb[2]));
        upstream_texenv.setSrcRegAlpha0(static_cast<rr::SrcReg>(texenv.src_alpha[0]));
        upstream_texenv.setSrcRegAlpha1(static_cast<rr::SrcReg>(texenv.src_alpha[1]));
        upstream_texenv.setSrcRegAlpha2(static_cast<rr::SrcReg>(texenv.src_alpha[2]));
        upstream_texenv.setOperandRgb0(static_cast<rr::Operand>(texenv.operand_rgb[0]));
        upstream_texenv.setOperandRgb1(static_cast<rr::Operand>(texenv.operand_rgb[1]));
        upstream_texenv.setOperandRgb2(static_cast<rr::Operand>(texenv.operand_rgb[2]));
        upstream_texenv.setOperandAlpha0(static_cast<rr::Operand>(texenv.operand_alpha[0]));
        upstream_texenv.setOperandAlpha1(static_cast<rr::Operand>(texenv.operand_alpha[1]));
        upstream_texenv.setOperandAlpha2(static_cast<rr::Operand>(texenv.operand_alpha[2]));
        upstream_texenv.setShiftRgb(texenv.shift_rgb);
        upstream_texenv.setShiftAlpha(texenv.shift_alpha);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_tex_env(&ours, 0U, &texenv));
        compare_masked_register(ours, upstream_texenv, 0x7fffffffU);

        texture.width_log2 = static_cast<uint8_t>(next_random(random) % 9U);
        texture.height_log2 = static_cast<uint8_t>(next_random(random) % 9U);
        texture.clamp_s = static_cast<uint8_t>(next_random(random) & 1U);
        texture.clamp_t = static_cast<uint8_t>(next_random(random) & 1U);
        texture.mag_linear = static_cast<uint8_t>(next_random(random) & 1U);
        texture.min_linear = static_cast<uint8_t>(next_random(random) & 1U);
        texture.pixel_format = static_cast<uint8_t>(next_random(random) % 3U);
        upstream_texture.setTextureWidth(static_cast<uint16_t>(1U << texture.width_log2));
        upstream_texture.setTextureHeight(static_cast<uint16_t>(1U << texture.height_log2));
        upstream_texture.setWrapModeS(static_cast<rr::TextureWrapMode>(texture.clamp_s));
        upstream_texture.setWrapModeT(static_cast<rr::TextureWrapMode>(texture.clamp_t));
        upstream_texture.setEnableMagFilter(texture.mag_linear != 0U);
        upstream_texture.setEnableMinFilter(texture.min_linear != 0U);
        upstream_texture.setPixelFormat(static_cast<rr::DevicePixelFormat>(texture.pixel_format));
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_texture_state(&ours, 0U, &texture));
        compare_masked_register(ours, upstream_texture, 0xffffU);

        clear_color.setRed(static_cast<uint8_t>(rgba >> 24));
        clear_color.setGreen(static_cast<uint8_t>(rgba >> 16));
        clear_color.setBlue(static_cast<uint8_t>(rgba >> 8));
        clear_color.setAlpha(static_cast<uint8_t>(rgba));
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_clear_color(&ours, rgba));
        compare_register(ours, clear_color);

        clear_depth.setValue(value);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_clear_depth(&ours, static_cast<uint16_t>(value)));
        compare_register(ours, clear_depth);

        fog_color.setRed(static_cast<uint8_t>(rgba >> 24));
        fog_color.setGreen(static_cast<uint8_t>(rgba >> 16));
        fog_color.setBlue(static_cast<uint8_t>(rgba >> 8));
        fog_color.setAlpha(static_cast<uint8_t>(rgba));
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_fog_color(&ours, rgba));
        compare_register(ours, fog_color);

        texenv_color.setTmu(0U);
        texenv_color.setRed(static_cast<uint8_t>(rgba >> 24));
        texenv_color.setGreen(static_cast<uint8_t>(rgba >> 16));
        texenv_color.setBlue(static_cast<uint8_t>(rgba >> 8));
        texenv_color.setAlpha(static_cast<uint8_t>(rgba));
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_tex_env_color(&ours, 0U, rgba));
        compare_register(ours, texenv_color);

        resolution.setX(x0);
        resolution.setY(y0);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_resolution(&ours, x0, y0));
        compare_register(ours, resolution);

        yoffset.setX(0U);
        yoffset.setY(y0);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_y_offset(&ours, y0));
        compare_register(ours, yoffset);

        scissor_start.setX(x0);
        scissor_start.setY(y0);
        scissor_end.setX(x1);
        scissor_end.setY(y1);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_scissor(&ours, x0, y0, x1, y1));
        assert(ours.length == 16U);
        assert(le_word(bytes, 0U) == (RIX_OP_RENDER_CONFIG | scissor_start.getAddr()));
        assert(le_word(bytes, 1U) == scissor_start.serialize());
        assert(le_word(bytes, 2U) == (RIX_OP_RENDER_CONFIG | scissor_end.getAddr()));
        assert(le_word(bytes, 3U) == scissor_end.serialize());

        /* The C API receives the already translated device bus address. */
        color_buffer.deserialize(value);
        depth_buffer.deserialize(next_random(random));
        stencil_buffer.deserialize(next_random(random));
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_buffer_address(&ours, RIX_REG_COLOR_BUFFER_ADDR, color_buffer.serialize()));
        compare_register(ours, color_buffer);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_buffer_address(&ours, RIX_REG_DEPTH_BUFFER_ADDR, depth_buffer.serialize()));
        compare_register(ours, depth_buffer);
        rix_cmd_init(&ours, bytes, sizeof(bytes));
        assert(rix_cmd_buffer_address(&ours, RIX_REG_STENCIL_BUFFER_ADDR, stencil_buffer.serialize()));
        compare_register(ours, stencil_buffer);
    }
}

static rr::InternalPixelFormat upstream_texture_format(enum RixTextureFormat format)
{
    switch (format) {
    case RIX_TEXTURE_RGB565: return rr::InternalPixelFormat::RGB;
    case RIX_TEXTURE_RGBA4444: return rr::InternalPixelFormat::RGBA;
    case RIX_TEXTURE_RGBA5551: return rr::InternalPixelFormat::RGBA1;
    default: assert(false); return rr::InternalPixelFormat::RGB;
    }
}

static void test_texture_uploads(void)
{
    constexpr size_t max_texels = 256U * 256U;
    std::array<uint32_t, max_texels> input {};
    std::array<uint8_t, max_texels * 4U> client {};
    std::array<uint8_t, max_texels * 2U> ours {};
    uint32_t random = 0x4f4b1d2eU;
    for (unsigned format = RIX_TEXTURE_RGBA4444; format <= RIX_TEXTURE_RGB565; ++format) {
        for (size_t side = 256U; side != 0U; side >>= 1U) {
            const size_t count = side * side;
            std::shared_ptr<uint16_t> upstream { new uint16_t[count], std::default_delete<uint16_t[]>() };
            rr::ImageConverter converter;
            for (size_t i = 0U; i < count; ++i) {
                const uint32_t rgba = next_random(random);
                input[i] = rgba;
                client[i * 4U] = static_cast<uint8_t>(rgba >> 24);
                client[i * 4U + 1U] = static_cast<uint8_t>(rgba >> 16);
                client[i * 4U + 2U] = static_cast<uint8_t>(rgba >> 8);
                client[i * 4U + 3U] = static_cast<uint8_t>(rgba);
            }
            converter.convertUnpack(upstream, upstream_texture_format(static_cast<enum RixTextureFormat>(format)),
                                    side, 0, 0, static_cast<GLsizei>(side),
                                    static_cast<GLsizei>(side), GL_RGBA, GL_UNSIGNED_BYTE, client.data());
            assert(rix_texture_convert_rgba8_le(ours.data(), count * 2U, input.data(), count,
                                                static_cast<enum RixTextureFormat>(format)));
            assert(std::memcmp(ours.data(), upstream.get(), count * 2U) == 0);
        }
    }
}

int main()
{
    test_triangles();
    test_commands();
    test_render_registers();
    test_texture_uploads();
    std::puts("rix upstream differential: 2004 triangle cases; 256 each of feature, fragment, texenv, texture, clear color/depth, fog, texenv color, resolution, y offset, scissor, and 3 buffer addresses; RGB565/RGBA4444/RGBA5551 256x256 plus 8 mip levels; fog, texture pages, framebuffer");
    return 0;
}
