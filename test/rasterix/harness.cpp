#include "GenericMemoryBusConnector.hpp"
#include "RIXGL.hpp"
#include "Vtop.h"
#include "gl.h"
#include "glu.h"
#include "renderer/devicedatauploader/DeviceDataUploader.hpp"
#include "verilated.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kWidth = HARNESS_WIDTH;
constexpr uint32_t kHeight = HARNESS_HEIGHT;
constexpr uint32_t kBandPixels = 1U << HARNESS_BAND_LG;
constexpr uint32_t kWorldTriangles = 2048;
constexpr uint32_t kParticles = 256;
constexpr uint32_t kTextureEdge = 16;

uint64_t fnv1a(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class MeasuredVerilatorBus final : public rr::GenericMemoryBusConnector<33, 128 * 1024> {
public:
    MeasuredVerilatorBus()
        : m_framebuffer(kWidth * kHeight * 3)
    {
        m_top.m_framebuffer_axis_tready = 1;
        m_top.m_cmd_resp_axis_tready = 0;
        m_top.s_cmd_axis_tvalid = 0;
        m_top.s_cmd_axis_tlast = 0;
        m_top.resetn = 0;
        clk();
        clk();
        m_top.resetn = 1;
        clk();
    }

    ~MeasuredVerilatorBus() override { m_top.final(); }

    void writeData(const uint8_t index, const uint32_t size, const uint32_t offset) override
    {
        m_commandBytes += size;
        const uint32_t* words = reinterpret_cast<const uint32_t*>(m_dlMemTx[index].data() + offset);
        const uint64_t startCycles = m_cycles;
        for (uint32_t word = 0; word < size / sizeof(*words);) {
            if (m_cycles - startCycles > 200000ULL) {
                std::ostringstream diagnostic;
                diagnostic << "RasterIX command stream stalled after " << (m_cycles - startCycles)
                           << " cycles (word=" << word << '/' << size / sizeof(*words)
                           << ", cmd_ready=" << static_cast<unsigned>(m_top.s_cmd_axis_tready)
                           << ", fb_valid=" << static_cast<unsigned>(m_top.m_framebuffer_axis_tvalid)
                           << ", initial_words=";
                for (uint32_t initial = 0; initial < 10 && initial < size / sizeof(*words); ++initial) {
                    diagnostic << (initial == 0 ? "" : ":") << std::hex << words[initial] << std::dec;
                }
                diagnostic << ')';
                throw std::runtime_error(diagnostic.str());
            }
            if (m_top.s_cmd_axis_tready) {
                m_top.s_cmd_axis_tdata = words[word++];
                m_top.s_cmd_axis_tvalid = 1;
            }
            clk();
        }
        m_top.s_cmd_axis_tvalid = 0;
    }

    void readData(const uint8_t index, const uint32_t size) override
    {
        uint32_t* words = reinterpret_cast<uint32_t*>(m_dlMemRx[index].data());
        m_top.m_cmd_resp_axis_tready = 1;
        for (uint32_t word = 0; word < size / sizeof(*words);) {
            if (m_top.m_cmd_resp_axis_tvalid) {
                words[word++] = m_top.m_cmd_resp_axis_tdata;
            }
            clk();
        }
        m_top.m_cmd_resp_axis_tready = 0;
    }

    void blockUntilTransferIsComplete() override { }

    void beginFrame()
    {
        m_frameStartCycles = m_cycles;
        m_commandBytes = 0;
        m_targetCompletedFrames = m_completedFrames + 1;
    }

    void waitForFrame()
    {
        constexpr uint64_t kCycleLimit = 20000000ULL;
        while (m_completedFrames < m_targetCompletedFrames) {
            if (m_cycles - m_frameStartCycles > kCycleLimit) {
                std::ostringstream diagnostic;
                diagnostic << "RasterIX framebuffer stream exceeded " << kCycleLimit
                           << " cycles (cmd_ready=" << static_cast<unsigned>(m_top.s_cmd_axis_tready)
                           << ", fb_valid=" << static_cast<unsigned>(m_top.m_framebuffer_axis_tvalid)
                           << ", fb_last=" << static_cast<unsigned>(m_top.m_framebuffer_axis_tlast)
                           << ", captured_bytes=" << m_streamAddress << ')';
                throw std::runtime_error(diagnostic.str());
            }
            clk();
        }
    }

    uint64_t frameCycles() const { return m_cycles - m_frameStartCycles; }
    uint64_t commandBytes() const { return m_commandBytes; }
    const std::vector<uint8_t>& framebuffer() const { return m_framebuffer; }

private:
    void clk()
    {
        m_top.aclk = 1;
        m_top.eval();
        m_top.aclk = 0;
        m_top.eval();
        ++m_cycles;
        if (!m_top.resetn) {
            return;
        }
        if (m_top.m_framebuffer_axis_tvalid && m_streamAddress < m_framebuffer.size()) {
            const uint16_t pixel = m_top.m_framebuffer_axis_tdata;
            m_framebuffer[m_streamAddress++] = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
            m_framebuffer[m_streamAddress++] = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
            m_framebuffer[m_streamAddress++] = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
        }
        if (m_streamAddress >= m_framebuffer.size() && m_top.m_framebuffer_axis_tlast) {
            m_streamAddress = 0;
            ++m_completedFrames;
        }
    }

    Vtop m_top {new VerilatedContext};
    std::vector<uint8_t> m_framebuffer;
    uint64_t m_cycles {};
    uint64_t m_frameStartCycles {};
    uint64_t m_commandBytes {};
    uint64_t m_completedFrames {};
    uint64_t m_targetCompletedFrames {};
    std::size_t m_streamAddress {};
};

struct Result {
    std::string scene;
    uint64_t cycles {};
    uint64_t commandBytes {};
    double frontendMilliseconds {};
    uint64_t hash {};
};

uint32_t nextRandom(uint32_t& state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

void submitTexturedQuad(float x0, float y0, float x1, float y1, float z, bool secondTmu)
{
    glBegin(GL_TRIANGLES);
    for (const auto& vertex : std::array<std::array<float, 4>, 6> {{
             {{x0, y0, 0.0f, 0.0f}}, {{x1, y0, 1.0f, 0.0f}}, {{x1, y1, 1.0f, 1.0f}},
             {{x0, y0, 0.0f, 0.0f}}, {{x1, y1, 1.0f, 1.0f}}, {{x0, y1, 0.0f, 1.0f}},
         }}) {
        glTexCoord2f(vertex[2], vertex[3]);
        if (secondTmu) {
            glMultiTexCoord2f(GL_TEXTURE1, vertex[2] * 4.0f, vertex[3] * 4.0f);
        }
        glVertex3f(vertex[0], vertex[1], z);
    }
    glEnd();
}

class Scenes {
public:
    explicit Scenes(bool twoTmus)
        : m_twoTmus(twoTmus)
    {
        rr::RIXGL::getInstance().setRenderResolution(kWidth, kHeight);
        createTextures();
    }

    void fill()
    {
        setup2D();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        bindTexture(m_baseTexture, 0);
        submitTexturedQuad(0, 0, kWidth, kHeight, 0.5f, false);
    }

    void quakeWorld()
    {
        setup2D();
        glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        bindTexture(m_baseTexture, 0);
        if (m_twoTmus) {
            bindTexture(m_lightTexture, 1);
            glActiveTexture(GL_TEXTURE1);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glActiveTexture(GL_TEXTURE0);
            drawWorldTriangles(true);
            glActiveTexture(GL_TEXTURE1);
            glDisable(GL_TEXTURE_2D);
            glActiveTexture(GL_TEXTURE0);
        } else {
            drawWorldTriangles(false);
            glDepthFunc(GL_EQUAL);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
            bindTexture(m_lightTexture, 0);
            drawWorldTriangles(false);
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
        }
    }

    void particlesHud()
    {
        setup2D();
        glClearColor(0.02f, 0.03f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        bindTexture(m_alphaTexture, 0);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        uint32_t seed = 0x10203040U;
        for (uint32_t i = 0; i < kParticles; ++i) {
            const float x = static_cast<float>(nextRandom(seed) % kWidth);
            const float y = static_cast<float>(nextRandom(seed) % kHeight);
            const float side = 6.0f + static_cast<float>(nextRandom(seed) % 18);
            glColor4f(1.0f, 0.65f + static_cast<float>(i % 4) * 0.1f, 0.2f, 0.35f);
            submitTexturedQuad(x, y, x + side, y + side, 0.2f, false);
        }
        glColor4f(1, 1, 1, 0.8f);
        submitTexturedQuad(8, 8, 312, 42, 0.1f, false);
        submitTexturedQuad(460, 430, 632, 472, 0.1f, false);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColor4f(1, 1, 1, 1);
    }

    void mipmappedFloor()
    {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glViewport(0, 0, kWidth, kHeight);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(68.0, static_cast<double>(kWidth) / kHeight, 0.25, 100.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        gluLookAt(0.0, 3.2, 5.5, 0.0, 0.0, -8.0, 0.0, 1.0, 0.0);
        glClearColor(0.08f, 0.11f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        bindTexture(m_mipTexture, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        constexpr int kX = 32;
        constexpr int kZ = 32;
        glBegin(GL_TRIANGLES);
        for (int z = 0; z < kZ; ++z) {
            for (int x = 0; x < kX; ++x) {
                const float x0 = -20.0f + 40.0f * x / kX;
                const float x1 = -20.0f + 40.0f * (x + 1) / kX;
                const float z0 = -1.0f - 40.0f * z / kZ;
                const float z1 = -1.0f - 40.0f * (z + 1) / kZ;
                const float s0 = static_cast<float>(x) / 2.0f;
                const float s1 = static_cast<float>(x + 1) / 2.0f;
                const float t0 = static_cast<float>(z) / 2.0f;
                const float t1 = static_cast<float>(z + 1) / 2.0f;
                for (const auto& v : std::array<std::array<float, 5>, 6> {{
                         {{x0, 0, z0, s0, t0}}, {{x1, 0, z0, s1, t0}}, {{x1, 0, z1, s1, t1}},
                         {{x0, 0, z0, s0, t0}}, {{x1, 0, z1, s1, t1}}, {{x0, 0, z1, s0, t1}},
                     }}) {
                    glTexCoord2f(v[3], v[4]);
                    glVertex3f(v[0], v[1], v[2]);
                }
            }
        }
        glEnd();
    }

private:
    void setup2D()
    {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glViewport(0, 0, kWidth, kHeight);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, kWidth, 0, kHeight, 0, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    void bindTexture(GLuint texture, uint32_t unit)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(GL_TEXTURE0);
    }

    void drawWorldTriangles(bool secondTmu)
    {
        uint32_t seed = 0x71625341U;
        glBegin(GL_TRIANGLES);
        for (uint32_t i = 0; i < kWorldTriangles; ++i) {
            const float centerX = static_cast<float>(nextRandom(seed) % kWidth);
            const float centerY = static_cast<float>(nextRandom(seed) % kHeight);
            const float halfW = 8.0f + static_cast<float>(nextRandom(seed) % 13);
            const float halfH = 8.0f + static_cast<float>(nextRandom(seed) % 13);
            const float depth = 0.05f + static_cast<float>(nextRandom(seed) % 800) / 1000.0f;
            const float phase = static_cast<float>(nextRandom(seed) & 255) / 255.0f;
            const std::array<std::array<float, 4>, 3> triangle {{
                {{centerX - halfW, centerY - halfH, phase, 0.0f}},
                {{centerX + halfW, centerY - halfH, 1.0f, phase}},
                {{centerX, centerY + halfH, phase, 1.0f}},
            }};
            for (const auto& vertex : triangle) {
                glTexCoord2f(vertex[2], vertex[3]);
                if (secondTmu) {
                    glMultiTexCoord2f(GL_TEXTURE1, vertex[2] * 4.0f, vertex[3] * 4.0f);
                }
                glVertex3f(vertex[0], vertex[1], depth);
            }
        }
        glEnd();
    }

    void createTextures()
    {
        std::vector<uint8_t> checker(kTextureEdge * kTextureEdge * 4);
        std::vector<uint8_t> light(kTextureEdge * kTextureEdge * 4);
        std::vector<uint8_t> alpha(kTextureEdge * kTextureEdge * 4);
        for (uint32_t y = 0; y < kTextureEdge; ++y) {
            for (uint32_t x = 0; x < kTextureEdge; ++x) {
                const std::size_t offset = (y * kTextureEdge + x) * 4;
                const uint8_t c = ((x / 4) ^ (y / 4)) & 1 ? 210 : 72;
                checker[offset] = c;
                checker[offset + 1] = static_cast<uint8_t>(c * 3 / 4);
                checker[offset + 2] = static_cast<uint8_t>(255 - c / 3);
                checker[offset + 3] = 255;
                const uint8_t l = static_cast<uint8_t>(80 + ((x ^ y) & 63));
                light[offset] = l;
                light[offset + 1] = l;
                light[offset + 2] = l;
                light[offset + 3] = 255;
                const int dx = static_cast<int>(x) - 128;
                const int dy = static_cast<int>(y) - 128;
                const uint8_t a = (dx * dx + dy * dy < 128 * 128) ? 220 : 0;
                alpha[offset] = 255;
                alpha[offset + 1] = 180;
                alpha[offset + 2] = 80;
                alpha[offset + 3] = a;
            }
        }
        glGenTextures(1, &m_baseTexture);
        glGenTextures(1, &m_lightTexture);
        glGenTextures(1, &m_alphaTexture);
        glGenTextures(1, &m_mipTexture);
        uploadTexture(m_baseTexture, checker, false);
        uploadTexture(m_lightTexture, light, false);
        uploadTexture(m_alphaTexture, alpha, false);
        uploadTexture(m_mipTexture, checker, true);
    }

    void uploadTexture(GLuint texture, const std::vector<uint8_t>& pixels, bool mipmapped)
    {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        if (!mipmapped) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTextureEdge, kTextureEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return;
        }
        std::vector<uint8_t> levelPixels = pixels;
        for (uint32_t edge = kTextureEdge, level = 0;; ++level, edge /= 2) {
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, edge, edge, 0, GL_RGBA, GL_UNSIGNED_BYTE, levelPixels.data());
            if (edge == 1) {
                break;
            }
            std::vector<uint8_t> next((edge / 2) * (edge / 2) * 4);
            for (uint32_t y = 0; y < edge / 2; ++y) {
                for (uint32_t x = 0; x < edge / 2; ++x) {
                    for (uint32_t channel = 0; channel < 4; ++channel) {
                        const std::size_t source = ((y * 2) * edge + x * 2) * 4 + channel;
                        next[(y * (edge / 2) + x) * 4 + channel] = static_cast<uint8_t>(
                            (levelPixels[source] + levelPixels[source + 4] + levelPixels[source + edge * 4] + levelPixels[source + edge * 4 + 4]) / 4);
                    }
                }
            }
            levelPixels.swap(next);
        }
    }

    bool m_twoTmus;
    GLuint m_baseTexture {};
    GLuint m_lightTexture {};
    GLuint m_alphaTexture {};
    GLuint m_mipTexture {};
};

void writePpm(const std::filesystem::path& path, const std::vector<uint8_t>& framebuffer)
{
    std::ofstream image(path, std::ios::binary);
    image << "P6\n" << kWidth << " " << kHeight << "\n255\n";
    image.write(reinterpret_cast<const char*>(framebuffer.data()), static_cast<std::streamsize>(framebuffer.size()));
}

void writeResults(const std::filesystem::path& path, const std::vector<Result>& results, uint32_t tmuCount)
{
    std::ofstream json(path);
    json << "{\n  \"upstream_commit\": \"9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0\",\n"
         << "  \"width\": " << kWidth << ",\n  \"height\": " << kHeight << ",\n"
         << "  \"tmu_count\": " << tmuCount << ",\n  \"clock_hz\": 100000000,\n"
         << "  \"internal_band_pixels\": " << kBandPixels << ",\n  \"band_count\": " << (kWidth * kHeight + kBandPixels - 1) / kBandPixels << ",\n"
         << "  \"scenes\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& result = results[i];
        const double fps = 100000000.0 / static_cast<double>(result.cycles);
        const double perBand = static_cast<double>(result.cycles) / 19.0;
        json << "    {\"name\": \"" << result.scene << "\", \"cycles\": " << result.cycles
             << ", \"fps_at_100mhz\": " << std::fixed << std::setprecision(3) << fps
             << ", \"command_bytes\": " << result.commandBytes
             << ", \"frontend_ms_x86\": " << std::setprecision(6) << result.frontendMilliseconds
             << ", \"bands\": 19, \"average_cycles_per_band\": " << std::setprecision(3) << perBand
             << ", \"frame_fnv1a64\": \"" << std::hex << std::setw(16) << std::setfill('0') << result.hash
             << std::dec << std::setfill(' ') << "\"}" << (i + 1 == results.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
}

struct Options {
    uint32_t tmuCount {};
    std::filesystem::path output;
};

Options parseArguments(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--tmu" && i + 1 < argc) {
            options.tmuCount = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--output" && i + 1 < argc) {
            options.output = argv[++i];
        } else {
            throw std::runtime_error("usage: rasterix_harness --tmu <1|2> --output <directory>");
        }
    }
    if ((options.tmuCount != 1 && options.tmuCount != 2) || options.output.empty()) {
        throw std::runtime_error("usage: rasterix_harness --tmu <1|2> --output <directory>");
    }
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseArguments(argc, argv);
        std::filesystem::create_directories(options.output / "frames");
        MeasuredVerilatorBus bus;
        rr::devicedatauploader::DeviceDataUploader device {bus};
        rr::RIXGL::createInstance(device);
        Scenes scenes {options.tmuCount == 2};

        // Prime texture uploads, the display-list rotation, and the pipeline; do not report this cold work.
        bus.beginFrame();
        scenes.fill();
        rr::RIXGL::getInstance().swapDisplayList();
        bus.waitForFrame();

        std::vector<Result> results;
        const std::array<std::pair<std::string, void (Scenes::*)()>, 4> tests {{
            {"fill", &Scenes::fill},
            {"quake_world", &Scenes::quakeWorld},
            {"particles_hud", &Scenes::particlesHud},
            {"mipmapped_floor", &Scenes::mipmappedFloor},
        }};
        for (const auto& [name, draw] : tests) {
            bus.beginFrame();
            const auto start = std::chrono::steady_clock::now();
            (scenes.*draw)();
            const auto stop = std::chrono::steady_clock::now();
            rr::RIXGL::getInstance().swapDisplayList();
            bus.waitForFrame();
            const uint64_t hash = fnv1a(bus.framebuffer());
            writePpm(options.output / "frames" / ("tmu" + std::to_string(options.tmuCount) + "_" + name + ".ppm"), bus.framebuffer());
            results.push_back({name, bus.frameCycles(), bus.commandBytes(),
                std::chrono::duration<double, std::milli>(stop - start).count(), hash});
        }
        writeResults(options.output / ("results-tmu" + std::to_string(options.tmuCount) + ".json"), results, options.tmuCount);
        rr::RIXGL::destroy();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rasterix_harness: " << error.what() << "\n";
        return 1;
    }
}
