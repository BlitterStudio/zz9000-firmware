/*
 * RasterIX band dispatch protocol test.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Asserts the ZZ9000 RasterIX_IF band protocol that was verified in xsim
 * against Rasterizer.v and InternalFramebufferCommandHandler.v
 * (tb_zz9k_rasterix + compare_frames.py, 2026-10).  Upstream's dispatcher
 * stream is no longer a reference for band dispatch: it pins
 * YOffset=0/RenderResolution=full-frame per band, which overflows the
 * 16,384-pixel band RAM of zz9k_rasterix.  Constructor-level parity with
 * upstream stays covered by rix_differential.cpp, and the band layout
 * matrix (640x480, 800x600, 1024x768, odd heights) by rix_core_test.c.
 */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "rix_bin.h"
}

namespace {
constexpr uint16_t kWidth = 640;
constexpr uint16_t kHeight = 480;
constexpr uint32_t kColorAddress = 0x07820000U;
constexpr uint32_t kDepthAddress = 0x07b20000U;
constexpr uint32_t kPitch = kWidth * 2U;
constexpr uint32_t kBandPixels = 16384U;
constexpr uint32_t kTransferMask =
    RIX_FRAMEBUFFER_COMMIT | RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_READ;

struct Vertex {
    float x, y, s, t;
    uint32_t color;
};

struct Scene {
    const char *name;
    std::vector<std::array<Vertex, 3U>> triangles;
};

struct Band {
    uint32_t resolution {};
    uint32_t yoffset {};
    uint32_t colorAddress {};
    uint32_t depthAddress {};
    std::vector<uint32_t> framebuffer;
    std::vector<std::array<uint32_t, RIX_TRIANGLE_WORDS>> triangles;
};

uint32_t readWord(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

Scene makeFlatScene()
{
    Scene scene {"flat", {}};
    scene.triangles.push_back({Vertex {40.0f, 20.0f, 0.0f, 0.0f, 0x39c7f2ffU},
                               Vertex {600.0f, 80.0f, 1.0f, 0.0f, 0x39c7f2ffU},
                               Vertex {320.0f, 440.0f, 0.5f, 1.0f, 0x39c7f2ffU}});
    return scene;
}

Scene makeTexturedScene()
{
    Scene scene {"textured", {}};
    const Vertex bottomLeft {32.0f, 48.0f, 0.0f, 0.0f, 0xffffffffU};
    const Vertex bottomRight {608.0f, 48.0f, 3.0f, 0.0f, 0xffffffffU};
    const Vertex topRight {608.0f, 432.0f, 3.0f, 2.0f, 0xffffffffU};
    const Vertex topLeft {32.0f, 432.0f, 0.0f, 2.0f, 0xffffffffU};
    scene.triangles.push_back({bottomLeft, bottomRight, topRight});
    scene.triangles.push_back({bottomLeft, topRight, topLeft});
    return scene;
}

Scene makeMixedScene()
{
    Scene scene {"mixed", {}};
    scene.triangles.push_back({Vertex {166.4f, 326.4f, 0.0f, 0.0f, 0xf22e1affU},
                               Vertex {51.2f, 153.6f, 0.0f, 0.0f, 0xf22e1affU},
                               Vertex {281.6f, 153.6f, 0.0f, 0.0f, 0xf22e1affU}});
    scene.triangles.push_back({Vertex {473.6f, 326.4f, 0.0f, 0.0f, 0x2673f5ffU},
                               Vertex {358.4f, 153.6f, 0.0f, 0.0f, 0x2673f5ffU},
                               Vertex {588.8f, 153.6f, 0.0f, 0.0f, 0x2673f5ffU}});
    return scene;
}

void parseRange(const std::vector<uint8_t>& stream, size_t begin, size_t end, Band& band)
{
    size_t index = begin;
    while (index < end) {
        const uint32_t command = readWord(stream.data() + index * sizeof(uint32_t));
        const uint32_t opcode = command & 0xf0000000U;
        if (opcode == RIX_OP_RENDER_CONFIG) {
            assert(index + 1U < end);
            const uint32_t value = readWord(stream.data() + (index + 1U) * sizeof(uint32_t));
            switch (command & 0x0fffffffU) {
            case RIX_REG_Y_OFFSET: band.yoffset = value; break;
            case RIX_REG_RESOLUTION: band.resolution = value; break;
            case RIX_REG_COLOR_BUFFER_ADDR: band.colorAddress = value; break;
            case RIX_REG_DEPTH_BUFFER_ADDR: band.depthAddress = value; break;
            default: break;
            }
            ++index;
        } else if (opcode == RIX_OP_FRAMEBUFFER) {
            band.framebuffer.push_back(command);
        } else if (opcode == RIX_OP_TRIANGLE_STREAM) {
            const size_t words = (command & 0x0fffffffU) / sizeof(uint32_t);
            assert(words == RIX_TRIANGLE_WORDS);
            assert(index + words < end);
            std::array<uint32_t, RIX_TRIANGLE_WORDS> triangle {};
            for (size_t word = 0U; word < words; ++word)
                triangle[word] = readWord(stream.data() + (++index) * sizeof(uint32_t));
            band.triangles.push_back(triangle);
        } else if (opcode == RIX_OP_TEXTURE_STREAM) {
            index += command & 0x7ffffU;
        } else if (opcode == RIX_OP_FOG_LUT_STREAM) {
            index += (command & 0x0fffffffU) / sizeof(uint32_t);
        } else {
            assert(command == 0U);
        }
        ++index;
    }
}

/* Splits the flat command stream into per-band records: a band starts at the
 * RENDER_CONFIG RESOLUTION register that follows the previous band's commit. */
std::vector<Band> parseBands(const std::vector<uint8_t>& stream)
{
    std::vector<Band> bands;
    const size_t words = stream.size() / sizeof(uint32_t);
    size_t start = 0U;
    bool first = true;
    for (size_t index = 0U; index < words; ++index) {
        const uint32_t command = readWord(stream.data() + index * sizeof(uint32_t));
        if ((command & 0xf0000000U) != RIX_OP_RENDER_CONFIG ||
            (command & 0x0fffffffU) != RIX_REG_RESOLUTION)
            continue;
        /* Band 0 starts at the first resolution register: the leading clear
         * registers are frame prologue, not a band. */
        if (first) {
            start = index;
            first = false;
            continue;
        }
        assert(index > start);
        bands.push_back(Band {});
        parseRange(stream, start, index, bands.back());
        start = index;
    }
    assert(!first && start < words);
    bands.push_back(Band {});
    parseRange(stream, start, words, bands.back());
    return bands;
}

RixTriangle expectedIncrement(const RixTriangle& source, uint16_t lineStart, uint16_t lineEnd)
{
    RixTriangle copy = source;
    assert(rix_triangle_increment(&copy, lineStart, lineEnd));
    return copy;
}
} // namespace

int main()
{
    const std::vector<Scene> scenes {makeFlatScene(), makeTexturedScene(), makeMixedScene()};
    const uint32_t rows = kBandPixels / kWidth;
    const uint32_t count = (kHeight + rows - 1U) / rows;


    for (const Scene& scene : scenes) {
        std::vector<RixTriangle> triangles(scene.triangles.size());
        std::vector<RixTriangle *> references(scene.triangles.size());
        RixBandList list {};
        RixBandConfig config {};
        std::vector<uint8_t> stream(64U * 1024U);
        RixCmdWriter writer;
        uint32_t committed = 0U;

        for (size_t index = 0U; index < scene.triangles.size(); ++index) {
            const auto& triangle = scene.triangles[index];
            const RixVertex a {triangle[0].x, triangle[0].y, 0.5f, 1.0f, triangle[0].color,
                               triangle[0].s, triangle[0].t, 1.0f};
            const RixVertex b {triangle[1].x, triangle[1].y, 0.5f, 1.0f, triangle[1].color,
                               triangle[1].s, triangle[1].t, 1.0f};
            const RixVertex c {triangle[2].x, triangle[2].y, 0.5f, 1.0f, triangle[2].color,
                               triangle[2].s, triangle[2].t, 1.0f};
            assert(rix_setup_triangle(&triangles[index], &a, &b, &c, nullptr));
            references[index] = &triangles[index];
        }
        config.width = kWidth;
        config.height = kHeight;
        config.color_addr = kColorAddress;
        config.depth_addr = kDepthAddress;
        config.color_pitch = kPitch;
        config.depth_pitch = kPitch;
        config.clear_color = 0x080a0fffU;
        config.clear_depth = 0xffffU;
        config.clear_mask = RIX_CLEAR_COLOR | RIX_CLEAR_DEPTH;
        assert(rix_band_list_init(&list, kWidth, kHeight, references.data(), references.size()));
        for (RixTriangle *reference : references)
            assert(rix_band_list_add(&list, reference));
        rix_cmd_init(&writer, stream.data(), stream.size());
        assert(rix_band_emit_frame(&writer, &list, &config));
        assert(!writer.failed);
        stream.resize(writer.length);

        const auto bands = parseBands(stream);
        assert(bands.size() == count);
        for (size_t index = 0U; index < bands.size(); ++index) {
            const Band& band = bands[index];
            const uint32_t top = static_cast<uint32_t>(index) * rows;
            const uint32_t lines = kHeight - top < rows ? kHeight - top : rows;
            const uint16_t lineStart = static_cast<uint16_t>(kHeight - top - lines);
            const uint16_t lineEnd = static_cast<uint16_t>(kHeight - top);

            assert(band.resolution == (static_cast<uint32_t>(kWidth) | (lines << 16U)));
            assert(band.yoffset == (static_cast<uint32_t>(lineStart) << 16U));
            assert(band.colorAddress == kColorAddress + top * kPitch);
            assert(band.depthAddress == kDepthAddress + top * kPitch);
            size_t transfers = 0U;
            for (uint32_t command : band.framebuffer) {
                if ((command & kTransferMask) == 0U)
                    continue;
                ++transfers;
                assert((command & RIX_FRAMEBUFFER_COLOR) != 0U);
                assert((command >> 8U & 0xfffffU) == kWidth * lines);
                if ((command & RIX_FRAMEBUFFER_COMMIT) != 0U)
                    committed += command >> 8U & 0xfffffU;
            }
            assert(transfers == 2U); /* memset + commit */
            /* Every descriptor is the band-local rebased expectation. */
            size_t emitted = 0U;
            for (size_t source = 0U; source < triangles.size(); ++source) {
                if (!rix_triangle_in_band(&triangles[source], lineStart, lineEnd))
                    continue;
                const RixTriangle expected = expectedIncrement(triangles[source], lineStart, lineEnd);
                assert(std::equal(band.triangles[emitted].begin(),
                                  band.triangles[emitted].end(), expected.words));
                /* LINE_MODE descriptors keep absolute bottom-up bounding
                 * boxes; the hardware applies the Y offset itself. */
                assert(band.triangles[emitted][RIX_WORD_BB_START] ==
                       triangles[source].words[RIX_WORD_BB_START]);
                assert(band.triangles[emitted][RIX_WORD_BB_END] ==
                       triangles[source].words[RIX_WORD_BB_END]);
                ++emitted;
            }
            assert(emitted == band.triangles.size());
        }
        assert(committed == static_cast<uint32_t>(kWidth) * kHeight);
        std::fprintf(stderr, "%s: bands=%zu descriptors checked\n", scene.name, bands.size());
    }
    std::puts("rix band protocol: per-band resolution, y offset, addresses, sizes, descriptors");
    return 0;
}
