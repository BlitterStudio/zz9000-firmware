/*
 * Host-side RasterIX scene generator for the XSim ZZ9000 wrapper harness.
 *
 * Emits the verified per-band RasterIX_IF protocol (see rix_bin.h in the
 * MiniGL worktree) plus an analytic reference frame per scene, rendered with
 * the exact Rasterizer.v edge rules so compare_frames.py can gate full-frame
 * output instead of point samples.
 *
 * Build and run inside gcc:13 (see run_scenes.sh):
 *   gcc -std=c99 -Wall -Wextra -Werror \
 *       -I /mg/ZZ9000_proto.sdk/ZZ9000OS/src \
 *       /if/test/rix_xsim/rix_xsim_gen.c \
 *       /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_cmd.c \
 *       /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_setup.c \
 *       /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_bin.c \
 *       /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_texture.c \
 *       -lm -o rix_xsim_gen && rix_xsim_gen <output-dir>
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rix_bin.h"
#include "rix_cmd.h"
#include "rix_setup.h"
#include "rix_texture.h"

#define SCREEN_WIDTH 640U
#define SCREEN_HEIGHT 480U
#define COMMAND_ADDRESS 0x00100000U
#define COLOR_ADDRESS 0x00200000U
#define DEPTH_ADDRESS 0x002A0000U
#define TEXTURE_ADDRESS 0x00340000U
#define COLOR_PITCH (SCREEN_WIDTH * 2U)
#define DEPTH_PITCH (SCREEN_WIDTH * 2U)
#define CLEAR_RGBA 0xff0000ffU
#define COMMAND_CAPACITY (256U * 1024U)
#define TEXTURE_WIDTH 8U
#define TEXTURE_HEIGHT 8U
#define TEXTURE_BYTES 2048U
#define TEXTURE_WORDS (TEXTURE_BYTES / 4U)
#define MAX_TRIANGLES 64U
#define PI 3.14159265358979323846

static int put_le32(FILE *file, uint32_t value)
{
    return fputc((int)(value & 0xffU), file) != EOF &&
           fputc((int)((value >> 8) & 0xffU), file) != EOF &&
           fputc((int)((value >> 16) & 0xffU), file) != EOF &&
           fputc((int)((value >> 24) & 0xffU), file) != EOF;
}

static uint32_t get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int make_path(char *path, size_t size, const char *directory,
                     const char *name, const char *suffix)
{
    int length = snprintf(path, size, "%s/%s%s", directory, name, suffix);
    return length >= 0 && (size_t)length < size;
}

static int write_dse_bin(const char *path, const uint8_t *raw, size_t raw_bytes)
{
    FILE *file = fopen(path, "wb");
    uint32_t header = 0x90000000U | (uint32_t)raw_bytes;
    int ok;

    if (file == NULL) {
        return 0;
    }
    if (!put_le32(file, header) ||
        !put_le32(file, 0U)) { /* DSE address beat zero: direct parser stream. */
        (void)fclose(file);
        return 0;
    }
    ok = fwrite(raw, 1U, raw_bytes, file) == raw_bytes;
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static int write_dse_hex(const char *path, const uint8_t *raw, size_t raw_bytes)
{
    FILE *file = fopen(path, "w");
    size_t offset;
    uint32_t header = 0x90000000U | (uint32_t)raw_bytes;
    int ok = 1;

    if (file == NULL) {
        return 0;
    }
    if (fprintf(file, "%08x\n%08x\n", header, 0U) < 0) {
        ok = 0;
    }
    for (offset = 0U; ok && offset < raw_bytes; offset += 4U) {
        if (fprintf(file, "%08x\n", get_le32(raw + offset)) < 0) {
            ok = 0;
        }
    }
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

/*
 * EF owns full color and depth targets in external DDR.  StreamFramebuffer
 * derives the transfer extent from resolution, not the framebuffer command's
 * size field; the field remains the visible pixel count for parser ABI
 * compatibility.  It needs no band offset, COMMIT, or READ command.
 */
static int emit_external_frame(RixCmdWriter *writer, const RixTriangle *triangles,
                               size_t triangle_count, const RixBandConfig *config)
{
    size_t index;
    uint32_t pixels = (uint32_t)config->width * (uint32_t)config->height;

    if (!rix_cmd_resolution(writer, config->width, config->height) ||
        !rix_cmd_clear_color(writer, config->clear_color) ||
        !rix_cmd_clear_depth(writer, config->clear_depth) ||
        !rix_cmd_buffer_address(writer, RIX_REG_COLOR_BUFFER_ADDR,
                                config->color_addr) ||
        !rix_cmd_buffer_address(writer, RIX_REG_DEPTH_BUFFER_ADDR,
                                config->depth_addr) ||
        !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET |
                              RIX_FRAMEBUFFER_COLOR |
                              RIX_FRAMEBUFFER_DEPTH, pixels)) {
        return 0;
    }
    for (index = 0U; index < triangle_count; ++index) {
        if (!rix_cmd_triangle(writer, triangles[index].words,
                              RIX_TRIANGLE_WORDS)) {
            return 0;
        }
    }
    if (!rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_COMMIT |
                              RIX_FRAMEBUFFER_COLOR |
                              RIX_FRAMEBUFFER_DEPTH, pixels)) {
        return 0;
    }
    return 1;
}
static RixVertex vertex(float x, float y, float z, uint32_t rgba, float s, float t)
{
    RixVertex value;
    value.x = x;
    value.y = y;
    value.z = z;
    value.w = 1.0f;
    value.rgba = rgba;
    value.s = s;
    value.t = t;
    value.q = 1.0f;
    return value;
}

/* Scene geometry in top-down screen coordinates; stored flipped to the
 * RasterIX bottom-up axis exactly like minigl_rix_backend_add_triangle(). */
typedef struct SceneTriangle {
    RixTriangle prepared;
    uint32_t paint_rgba;
} SceneTriangle;

static SceneTriangle scene_triangles[MAX_TRIANGLES];
static size_t scene_triangle_count;

static int add_triangle(RixBandList *bands, RixTriangle *triangles,
                        size_t *triangle_count, const RixVertex *v0,
                        const RixVertex *v1, const RixVertex *v2)
{
    RixScissor scissor;
    RixVertex f0, f1, f2;
    RixTriangle *triangle;

    if (*triangle_count >= MAX_TRIANGLES) {
        return 0;
    }
    scissor.start_x = 0U;
    scissor.start_y = 0U;
    scissor.end_x = SCREEN_WIDTH;
    scissor.end_y = SCREEN_HEIGHT;
    scissor.enabled = 1U;
    f0 = *v0; f0.y = (float)SCREEN_HEIGHT - f0.y;
    f1 = *v1; f1.y = (float)SCREEN_HEIGHT - f1.y;
    f2 = *v2; f2.y = (float)SCREEN_HEIGHT - f2.y;
    triangle = &triangles[*triangle_count];
    if (!rix_setup_triangle(triangle, &f0, &f1, &f2, &scissor) ||
        !rix_band_list_add(bands, triangle)) {
        return 0;
    }
    scene_triangles[*triangle_count].prepared = *triangle;
    scene_triangles[*triangle_count].paint_rgba = v0->rgba;
    ++*triangle_count;
    return 1;
}

/*
 * Analytic reference renderer.  Reproduces Rasterizer.v exactly: the edge
 * functions of rix_setup_triangle evaluated per bounding box pixel with the
 * RTL's inclusivity rule (edge0Inclusive <= incX > 0 || (incX == 0 && incY
 * > 0)) and inside test (!E[31] && (E != 0 || inclusive)).  The band layout
 * clamps the same bounding boxes, so full-box coverage equals the union of
 * the per-band rasterizations.  Frames are stored bottom-up (PPM row =
 * absolute RasterIX y), matching tb_zz9k_rasterix.sv's dump_ppm view of the
 * top-down target memory.
 */
static uint16_t rgba_to_rgb565(uint32_t rgba)
{
    uint32_t r = (rgba >> 24) & 0xffU;
    uint32_t g = (rgba >> 16) & 0xffU;
    uint32_t b = (rgba >> 8) & 0xffU;

    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static void paint_reference(uint16_t *frame, const RixTriangle *triangle,
                            uint16_t color565)
{
    int32_t bx = (int32_t)(triangle->words[RIX_WORD_BB_START] & 0xffffU);
    int32_t by = (int32_t)(triangle->words[RIX_WORD_BB_START] >> 16);
    int32_t ex = (int32_t)(triangle->words[RIX_WORD_BB_END] & 0xffffU);
    int32_t ey = (int32_t)(triangle->words[RIX_WORD_BB_END] >> 16);
    int32_t x;
    int32_t y;

    for (y = by; y < ey; ++y) {
        for (x = bx; x < ex; ++x) {
            unsigned int edge;
            int inside = 1;

            for (edge = 0U; edge < 3U; ++edge) {
                int32_t wx = (int32_t)triangle->words[RIX_WORD_W_X + edge];
                int32_t wy = (int32_t)triangle->words[RIX_WORD_W_Y + edge];
                int32_t value = (int32_t)triangle->words[RIX_WORD_W_INIT + edge] +
                                wx * (x - bx) + wy * (y - by);
                int inclusive = wx > 0 || (wx == 0 && wy > 0);

                if (value < 0 || (value == 0 && !inclusive)) {
                    inside = 0;
                    break;
                }
            }
            if (inside) {
                frame[y * (int32_t)SCREEN_WIDTH + x] = color565;
            }
        }
    }
}

static int write_reference(const char *directory, const char *name,
                           uint32_t clear_rgba, int textured)
{
    static uint16_t frame[SCREEN_WIDTH * SCREEN_HEIGHT];
    char path[1024];
    FILE *file;
    unsigned int index;

    for (index = 0U; index < SCREEN_WIDTH * SCREEN_HEIGHT; ++index) {
        frame[index] = rgba_to_rgb565(clear_rgba);
    }
    for (index = 0U; index < scene_triangle_count; ++index) {
        /* Textured scenes use the uniform white 8x8 texture with
         * RIX_TEX_REPLACE, so every covered pixel is white. */
        paint_reference(frame, &scene_triangles[index].prepared,
                        textured ? 0xffffU
                                 : rgba_to_rgb565(scene_triangles[index].paint_rgba));
    }
    if (!make_path(path, sizeof(path), directory, name, "_ref.ppm")) {
        return 0;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT) < 0) {
        (void)fclose(file);
        return 0;
    }
    for (index = 0U; index < SCREEN_WIDTH * SCREEN_HEIGHT; ++index) {
        uint16_t pixel = frame[index];
        uint8_t rgb[3] = {
            (uint8_t)((pixel >> 11) << 3),
            (uint8_t)((pixel >> 5 & 0x3fU) << 2),
            (uint8_t)((pixel & 0x1fU) << 3)
        };

        if (fwrite(rgb, 1U, 3U, file) != 3U) {
            (void)fclose(file);
            return 0;
        }
    }
    if (fclose(file) != 0) {
        return 0;
    }
    return 1;
}

static int setup_state(RixCmdWriter *writer, int textured)
{
    RixFragmentState fragment;
    RixTextureState state;
    RixTexEnvState environment;
    uint32_t pages[TEXTURE_BYTES / 2048U];
    size_t index;

    memset(&fragment, 0, sizeof(fragment));
    fragment.depth_func = RIX_FUNC_ALWAYS;
    fragment.depth_mask = 0U;
    fragment.color_mask_r = 1U;
    fragment.color_mask_g = 1U;
    fragment.color_mask_b = 1U;
    fragment.color_mask_a = 1U;
    fragment.src_factor = RIX_BLEND_ONE;
    fragment.dst_factor = RIX_BLEND_ZERO;
    /*
     * RasterIX's pixel pipeline always submits a TMU request, even when its
     * final colour mux would bypass the unit.  The external TextureReader
     * accepts those requests only while TMU0 and a texture source are enabled.
     * Keep the reader live for flat shading, but use primary colour as the
     * REPLACE source; source 1 remains TEXTURE solely to enable the reader.
     */
    if (!rix_cmd_feature_enable(writer, RIX_FEATURE_TMU0) ||
        !rix_cmd_fragment_state(writer, &fragment)) {
        return 0;
    }
    memset(&state, 0, sizeof(state));
    state.width_log2 = 3U;
    state.height_log2 = 3U;
    state.clamp_s = 1U;
    state.clamp_t = 1U;
    state.pixel_format = RIX_TEXTURE_RGB565;
    memset(&environment, 0, sizeof(environment));
    environment.combine_rgb = RIX_TEX_REPLACE;
    environment.combine_alpha = RIX_TEX_REPLACE;
    environment.src_rgb[0] = textured ? RIX_TEXSRC_TEXTURE : RIX_TEXSRC_PRIMARY_COLOR;
    environment.src_alpha[0] = textured ? RIX_TEXSRC_TEXTURE : RIX_TEXSRC_PRIMARY_COLOR;
    environment.operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
    environment.operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    for (index = 0U; index < TEXTURE_BYTES / 2048U; ++index) {
        pages[index] = TEXTURE_ADDRESS + (uint32_t)index * 2048U;
    }
    if (!rix_cmd_texture_state(writer, 0U, &state) ||
        !rix_cmd_tex_env(writer, 0U, &environment) ||
        !rix_cmd_texture_pages(writer, 0U, pages, TEXTURE_BYTES / 2048U)) {
        return 0;
    }
    return 1;
}

static int build_s2(RixBandList *bands, RixTriangle *triangles, size_t *count)
{
    RixVertex v0 = vertex(220.0f, 140.0f, 0.5f, 0x00ff00ffU, 0.0f, 0.0f);
    RixVertex v1 = vertex(420.0f, 160.0f, 0.5f, 0x00ff00ffU, 1.0f, 0.0f);
    RixVertex v2 = vertex(320.0f, 360.0f, 0.5f, 0x00ff00ffU, 0.5f, 1.0f);
    return add_triangle(bands, triangles, count, &v0, &v1, &v2);
}

static int build_s3(RixBandList *bands, RixTriangle *triangles, size_t *count)
{
    RixVertex v0 = vertex(220.0f, 140.0f, 0.5f, 0xffffffffU, 0.0f, 0.0f);
    RixVertex v1 = vertex(420.0f, 160.0f, 0.5f, 0xffffffffU, 1.0f, 0.0f);
    RixVertex v2 = vertex(320.0f, 360.0f, 0.5f, 0xffffffffU, 0.5f, 1.0f);
    return add_triangle(bands, triangles, count, &v0, &v1, &v2);
}

static int build_s4(RixBandList *bands, RixTriangle *triangles, size_t *count)
{
    unsigned int tooth;

    for (tooth = 0U; tooth < 24U; ++tooth) {
        double a0 = (double)tooth * 2.0 * PI / 24.0;
        double a1 = (double)(tooth + 1U) * 2.0 * PI / 24.0;
        double outer = tooth % 2U == 0U ? 156.0 : 130.0;
        double inner = 72.0;
        uint32_t color = 0x00ff00ffU;
        RixVertex i0 = vertex((float)(320.0 + inner * cos(a0)),
                              (float)(240.0 + inner * sin(a0)), 0.5f,
                              color, 0.0f, 0.0f);
        RixVertex o0 = vertex((float)(320.0 + outer * cos(a0)),
                              (float)(240.0 + outer * sin(a0)), 0.5f,
                              color, 0.0f, 0.0f);
        RixVertex o1 = vertex((float)(320.0 + outer * cos(a1)),
                              (float)(240.0 + outer * sin(a1)), 0.5f,
                              color, 0.0f, 0.0f);
        RixVertex i1 = vertex((float)(320.0 + inner * cos(a1)),
                              (float)(240.0 + inner * sin(a1)), 0.5f,
                              color, 0.0f, 0.0f);

        if (!add_triangle(bands, triangles, count, &i0, &o0, &o1) ||
            !add_triangle(bands, triangles, count, &i0, &o1, &i1)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Asymmetric band-protocol probe: a small right triangle at the top-left
 * (bands 0-2), one at the bottom-right (bands 16-18), and a one-pixel-wide
 * vertical line through the screen centre crossing every band boundary.  A
 * vertical flip swaps the corner triangles; a wrong Y offset, band-local
 * rebase, or DDR row order shifts or mirrors individual 25-line bands.
 */
static int build_probe(RixBandList *bands, RixTriangle *triangles, size_t *count)
{
    RixVertex t0 = vertex(8.0f, 6.0f, 0.5f, 0x00ff00ffU, 0.0f, 0.0f);
    RixVertex t1 = vertex(60.0f, 6.0f, 0.5f, 0x00ff00ffU, 1.0f, 0.0f);
    RixVertex t2 = vertex(8.0f, 54.0f, 0.5f, 0x00ff00ffU, 0.0f, 1.0f);
    RixVertex b0 = vertex(580.0f, 420.0f, 0.5f, 0x0000ffffU, 0.0f, 0.0f);
    RixVertex b1 = vertex(632.0f, 420.0f, 0.5f, 0x0000ffffU, 1.0f, 0.0f);
    RixVertex b2 = vertex(632.0f, 468.0f, 0.5f, 0x0000ffffU, 1.0f, 1.0f);
    RixVertex l0 = vertex(320.0f, 5.0f, 0.5f, 0xffff00ffU, 0.0f, 0.0f);
    RixVertex l1 = vertex(320.0f, 475.0f, 0.5f, 0xffff00ffU, 0.0f, 0.0f);
    RixVertex l2 = vertex(321.0f, 475.0f, 0.5f, 0xffff00ffU, 0.0f, 0.0f);

    return add_triangle(bands, triangles, count, &t0, &t1, &t2) &&
           add_triangle(bands, triangles, count, &b0, &b1, &b2) &&
           add_triangle(bands, triangles, count, &l0, &l1, &l2);
}

static void make_texture(uint8_t texture[TEXTURE_BYTES])
{
    uint32_t rgba[TEXTURE_WIDTH * TEXTURE_HEIGHT];
    unsigned int x;
    unsigned int y;

    memset(texture, 0, TEXTURE_BYTES);
    for (y = 0U; y < TEXTURE_HEIGHT; ++y) {
        for (x = 0U; x < TEXTURE_WIDTH; ++x) {
            /* Flat scenes keep TMU0 live with this deterministic white texel. */
            rgba[y * TEXTURE_WIDTH + x] = 0xffffffffU;
        }
    }
    (void)rix_texture_convert_rgba8_le(texture, TEXTURE_BYTES, rgba,
                                       TEXTURE_WIDTH * TEXTURE_HEIGHT,
                                       RIX_TEXTURE_RGB565);
}

static int write_texture_files(const char *directory, const uint8_t texture[TEXTURE_BYTES])
{
    char binary_path[1024];
    char hex_path[1024];
    FILE *file;
    size_t index;

    if (!make_path(binary_path, sizeof(binary_path), directory, "textures", ".bin") ||
        !make_path(hex_path, sizeof(hex_path), directory, "textures", ".hex")) {
        return 0;
    }
    file = fopen(binary_path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (fwrite(texture, 1U, TEXTURE_BYTES, file) != TEXTURE_BYTES) {
        (void)fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        return 0;
    }
    file = fopen(hex_path, "w");
    if (file == NULL) {
        return 0;
    }
    for (index = 0U; index < TEXTURE_BYTES; ++index) {
        if (fprintf(file, "%02x\n", texture[index]) < 0) {
            (void)fclose(file);
            return 0;
        }
    }
    if (fclose(file) != 0) {
        return 0;
    }
    printf("textures: %u bytes at 0x%08x; textures.hex is one little-endian byte per line\n",
           TEXTURE_BYTES, TEXTURE_ADDRESS);
    return 1;
}

static int generate_scene(const char *directory, const char *name,
                          unsigned int scene, int external_framebuffer)
{
    static uint8_t raw[COMMAND_CAPACITY];
    RixTriangle triangles[MAX_TRIANGLES];
    RixTriangle *references[MAX_TRIANGLES];
    RixBandList bands;
    RixBandConfig config;
    RixCmdWriter writer;
    size_t triangle_count = 0U;
    char binary_path[1024];
    char hex_path[1024];
    int textured = scene == 3U || scene == 4U;
    int valid_scene;

    memset(triangles, 0, sizeof(triangles));
    memset(references, 0, sizeof(references));
    scene_triangle_count = 0U;
    if (!rix_band_list_init(&bands, SCREEN_WIDTH, SCREEN_HEIGHT,
                            references, MAX_TRIANGLES)) {
        return 0;
    }
    memset(&config, 0, sizeof(config));
    config.width = SCREEN_WIDTH;
    config.height = SCREEN_HEIGHT;
    config.color_addr = COLOR_ADDRESS;
    config.depth_addr = DEPTH_ADDRESS;
    config.color_pitch = COLOR_PITCH;
    config.depth_pitch = DEPTH_PITCH;
    config.clear_color = CLEAR_RGBA;
    config.clear_depth = 0xffffU;
    config.clear_mask = RIX_CLEAR_COLOR | RIX_CLEAR_DEPTH;

    if (scene == 1U) {
        valid_scene = 1;
    } else if (scene == 2U) {
        valid_scene = build_s2(&bands, triangles, &triangle_count);
    } else if (scene == 3U) {
        valid_scene = build_s3(&bands, triangles, &triangle_count);
    } else if (scene == 4U) {
        valid_scene = build_s4(&bands, triangles, &triangle_count);
    } else if (scene == 5U) {
        valid_scene = build_probe(&bands, triangles, &triangle_count);
    } else {
        valid_scene = 0;
    }
    if (!valid_scene) {
        return 0;
    }
    scene_triangle_count = triangle_count;

    rix_cmd_init(&writer, raw, sizeof(raw));
    if (!setup_state(&writer, textured) ||
        !(external_framebuffer ?
              emit_external_frame(&writer, triangles, triangle_count, &config) :
              rix_band_emit_frame(&writer, &bands, &config)) ||
        writer.failed || writer.length == 0U || writer.length > 0x00ffffffU ||
        writer.length % 4U != 0U ||
        !write_reference(directory, name, CLEAR_RGBA, textured) ||
        !make_path(binary_path, sizeof(binary_path), directory, name, ".bin") ||
        !make_path(hex_path, sizeof(hex_path), directory, name, ".hex") ||
        !write_dse_bin(binary_path, raw, writer.length) ||
        !write_dse_hex(hex_path, raw, writer.length)) {
        return 0;
    }
    printf("%s: words=%zu bytes=%zu bands=%u triangles=%zu\n", name,
           writer.length / 4U + 2U, writer.length + 8U,
           external_framebuffer ? 0U : bands.count, triangle_count);
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t texture[TEXTURE_BYTES];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    make_texture(texture);
    printf("layout: commands=0x%08x color=0x%08x depth=0x%08x texture=0x%08x\n",
           COMMAND_ADDRESS, COLOR_ADDRESS, DEPTH_ADDRESS, TEXTURE_ADDRESS);
    if (!write_texture_files(argv[1], texture) ||
        !generate_scene(argv[1], "s1", 1U, 0) ||
        !generate_scene(argv[1], "s2", 2U, 0) ||
        !generate_scene(argv[1], "s3", 3U, 0) ||
        !generate_scene(argv[1], "s4", 4U, 0) ||
        !generate_scene(argv[1], "probe", 5U, 0) ||
        !generate_scene(argv[1], "ef-s1", 1U, 1) ||
        !generate_scene(argv[1], "ef-s2", 2U, 1) ||
        !generate_scene(argv[1], "ef-s3", 3U, 1) ||
        !generate_scene(argv[1], "ef-s4", 4U, 1) ||
        !generate_scene(argv[1], "ef-probe", 5U, 1)) {
        fprintf(stderr, "failed to write RasterIX XSim scene output to %s\n", argv[1]);
        return 1;
    }
    return 0;
}
