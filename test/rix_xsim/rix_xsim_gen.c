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
#define MAX_TRIANGLES 64U
#define PI 3.14159265358979323846
#define TEXTURE_PAGE_BYTES 2048U
#define SMALL_TEXTURE_PAGES 2U
#define STRESS_TEXTURE_LOG2 8U
#define STRESS_TEXTURE_SIZE (1U << STRESS_TEXTURE_LOG2)
#define STRESS_TEXTURE_PAGES \
    (STRESS_TEXTURE_SIZE * STRESS_TEXTURE_SIZE * 2U / TEXTURE_PAGE_BYTES)
#define TEXENV_TEXTURE_LOG2 6U
#define TEXENV_TEXTURE_SIZE (1U << TEXENV_TEXTURE_LOG2)
#define TEXENV_TEXTURE_PAGES \
    (TEXENV_TEXTURE_SIZE * TEXENV_TEXTURE_SIZE * 2U / TEXTURE_PAGE_BYTES)
#define TEXENV_TEXTURE_ADDRESS \
    (STRESS_TEXTURE_ADDRESS + STRESS_TEXTURE_PAGES * TEXTURE_PAGE_BYTES)
#define TEXTURE_PAGES \
    (SMALL_TEXTURE_PAGES + STRESS_TEXTURE_PAGES + TEXENV_TEXTURE_PAGES)
#define TEXTURE_BYTES (TEXTURE_PAGE_BYTES * TEXTURE_PAGES)
#define LIGHTMAP_ADDRESS (TEXTURE_ADDRESS + TEXTURE_PAGE_BYTES)
#define LIGHTMAP_RGB565 0x8410U
#define STRESS_TEXTURE_ADDRESS \
    (TEXTURE_ADDRESS + SMALL_TEXTURE_PAGES * TEXTURE_PAGE_BYTES)
#define STRESS_BLOCK 64U
#define STRESS_BLOCK_MARGIN 4U
#define STRESS_BASE_TRIANGLES 40U
#define STRESS_LIGHT_TRIANGLES 6U

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

typedef struct EfFrameConfig {
    uint16_t width;
    uint16_t height;
    uint32_t color_addr;
    uint32_t depth_addr;
    uint32_t clear_color;
    uint16_t clear_depth;
} EfFrameConfig;

/* EF owns full color and depth targets in external DDR. */
static int emit_external_frame(RixCmdWriter *writer, const RixTriangle *triangles,
                               size_t triangle_count, const EfFrameConfig *config)
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

static int add_triangle(RixTriangle *triangles, size_t *triangle_count,
                        const RixVertex *v0, const RixVertex *v1,
                        const RixVertex *v2)
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
    if (!rix_setup_triangle(triangle, &f0, &f1, &f2, &scissor)) {
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

static int triangle_covers(const RixTriangle *triangle, int32_t x, int32_t y)
{
    int32_t bx = (int32_t)(triangle->words[RIX_WORD_BB_START] & 0xffffU);
    int32_t by = (int32_t)(triangle->words[RIX_WORD_BB_START] >> 16);
    int32_t ex = (int32_t)(triangle->words[RIX_WORD_BB_END] & 0xffffU);
    int32_t ey = (int32_t)(triangle->words[RIX_WORD_BB_END] >> 16);
    unsigned int edge;

    if (x < bx || x >= ex || y < by || y >= ey) {
        return 0;
    }
    for (edge = 0U; edge < 3U; ++edge) {
        int32_t wx = (int32_t)triangle->words[RIX_WORD_W_X + edge];
        int32_t wy = (int32_t)triangle->words[RIX_WORD_W_Y + edge];
        int32_t value = (int32_t)triangle->words[RIX_WORD_W_INIT + edge] +
                        wx * (x - bx) + wy * (y - by);
        int inclusive = wx > 0 || (wx == 0 && wy > 0);

        if (value < 0 || (value == 0 && !inclusive)) {
            return 0;
        }
    }
    return 1;
}

static void paint_reference(uint16_t *frame, const RixTriangle *triangle,
                            uint16_t color565)
{
    int32_t x;
    int32_t y;

    for (y = 0; y < (int32_t)SCREEN_HEIGHT; ++y) {
        for (x = 0; x < (int32_t)SCREEN_WIDTH; ++x) {
            if (triangle_covers(triangle, x, y)) {
                frame[y * (int32_t)SCREEN_WIDTH + x] = color565;
            }
        }
    }
}

static int write_frame(const char *directory, const char *name,
                       const uint16_t *frame)
{
    char path[1024];
    FILE *file;
    unsigned int index;

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
    return fclose(file) == 0;
}

static int write_reference(const char *directory, const char *name,
                           uint32_t clear_rgba, int textured)
{
    static uint16_t frame[SCREEN_WIDTH * SCREEN_HEIGHT];
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
    return write_frame(directory, name, frame);
}

static int setup_state(RixCmdWriter *writer, int textured)
{
    RixFragmentState fragment;
    RixTextureState state;
    RixTexEnvState environment;
    uint32_t pages[1];

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
    pages[0] = TEXTURE_ADDRESS;
    if (!rix_cmd_texture_state(writer, 0U, &state) ||
        !rix_cmd_tex_env(writer, 0U, &environment) ||
        !rix_cmd_texture_pages(writer, 0U, pages, 1U)) {
        return 0;
    }
    return 1;
}

static int build_s2(RixTriangle *triangles, size_t *count)
{
    RixVertex v0 = vertex(220.0f, 140.0f, 0.5f, 0x00ff00ffU, 0.0f, 0.0f);
    RixVertex v1 = vertex(420.0f, 160.0f, 0.5f, 0x00ff00ffU, 1.0f, 0.0f);
    RixVertex v2 = vertex(320.0f, 360.0f, 0.5f, 0x00ff00ffU, 0.5f, 1.0f);
    return add_triangle(triangles, count, &v0, &v1, &v2);
}

static int build_s3(RixTriangle *triangles, size_t *count)
{
    RixVertex v0 = vertex(220.0f, 140.0f, 0.5f, 0xffffffffU, 0.0f, 0.0f);
    RixVertex v1 = vertex(420.0f, 160.0f, 0.5f, 0xffffffffU, 1.0f, 0.0f);
    RixVertex v2 = vertex(320.0f, 360.0f, 0.5f, 0xffffffffU, 0.5f, 1.0f);
    return add_triangle(triangles, count, &v0, &v1, &v2);
}

/* Two-pass lightmap geometry: same footprint as ef-s3.  paint_rgba is the
 * 8-bit channel expansion of LIGHTMAP_RGB565, so the analytic reference of
 * the blended second pass (white base x uniform lightmap) re-quantizes to
 * exactly the lightmap texel. */
static int build_lm(RixTriangle *triangles, size_t *count)
{
    RixVertex v0 = vertex(220.0f, 140.0f, 0.5f, 0x848284ffU, 0.0f, 0.0f);
    RixVertex v1 = vertex(420.0f, 160.0f, 0.5f, 0x848284ffU, 1.0f, 0.0f);
    RixVertex v2 = vertex(320.0f, 360.0f, 0.5f, 0x848284ffU, 0.5f, 1.0f);
    return add_triangle(triangles, count, &v0, &v1, &v2);
}

static int build_s4(RixTriangle *triangles, size_t *count)
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

        if (!add_triangle(triangles, count, &i0, &o0, &o1) ||
            !add_triangle(triangles, count, &i0, &o1, &i1)) {
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
static int build_probe(RixTriangle *triangles, size_t *count)
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

    return add_triangle(triangles, count, &t0, &t1, &t2) &&
           add_triangle(triangles, count, &b0, &b1, &b2) &&
           add_triangle(triangles, count, &l0, &l1, &l2);
}

/*
 * ef-stress: shared-read-channel stress.  Pass one draws STRESS_BASE_TRIANGLES
 * large overlapping triangles in shuffled depth order with LESS depth test
 * and depth writes.  Each samples its own uniform 64x64 block of a 256x256
 * RGB565 texture (nearest, clamped, UVs at least STRESS_BLOCK_MARGIN texels
 * inside the block), so the 8 KiB texture cache keeps missing while every
 * fragment reads and writes depth.  Pass two binds the uniform lightmap and
 * blends ZERO,SRC_COLOR over STRESS_LIGHT_TRIANGLES disjoint triangles with
 * the depth test still on (LEQUAL, no write), adding a colour read and write
 * per fragment.  Block colours only use zero or full channels, so each
 * blended channel is exactly zero or the lightmap channel (the ef-lm
 * identity) and the analytic reference stays exact.
 */
static const uint32_t stress_palette[7] = {
    0xffffffffU, 0xffff00ffU, 0x00ffffffU, 0x00ff00ffU,
    0xff00ffffU, 0x0000ffffU, 0x000000ffU
};
static float stress_depth[MAX_TRIANGLES];
static size_t stress_base_count;

static uint32_t stress_random(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state >> 8;
}

static float stress_range(uint32_t *state, float low, float high)
{
    return low + (high - low) * (float)(stress_random(state) & 0xffffU) / 65535.0f;
}

static float stress_clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

static void fill_stress_texture(uint8_t *texture)
{
    static uint32_t rgba[STRESS_TEXTURE_SIZE * STRESS_TEXTURE_SIZE];
    unsigned int x;
    unsigned int y;

    for (y = 0U; y < STRESS_TEXTURE_SIZE; ++y) {
        for (x = 0U; x < STRESS_TEXTURE_SIZE; ++x) {
            unsigned int block = (y / STRESS_BLOCK) * (STRESS_TEXTURE_SIZE / STRESS_BLOCK) +
                                 x / STRESS_BLOCK;
            rgba[y * STRESS_TEXTURE_SIZE + x] = stress_palette[block % 7U];
        }
    }
    (void)rix_texture_convert_rgba8_le(texture, STRESS_TEXTURE_PAGES * TEXTURE_PAGE_BYTES,
                                       rgba, STRESS_TEXTURE_SIZE * STRESS_TEXTURE_SIZE,
                                       RIX_TEXTURE_RGB565);
}
/* Per-texel gradient page for ef-texenv: every texel differs from its
 * neighbours so adjacent fragments present different TexEnv operands. */
static void fill_texenv_texture(uint8_t *texture)
{
    static uint32_t rgba[TEXENV_TEXTURE_SIZE * TEXENV_TEXTURE_SIZE];
    unsigned int x;
    unsigned int y;

    for (y = 0U; y < TEXENV_TEXTURE_SIZE; ++y) {
        for (x = 0U; x < TEXENV_TEXTURE_SIZE; ++x) {
            uint32_t r = (x * 5U) & 0xffU;
            uint32_t g = (y * 4U) & 0xffU;
            uint32_t b = ((x + y) * 3U) & 0xffU;

            rgba[y * TEXENV_TEXTURE_SIZE + x] = (r << 24) | (g << 16) | (b << 8) | 0xffU;
        }
    }
    (void)rix_texture_convert_rgba8_le(texture, TEXENV_TEXTURE_PAGES * TEXTURE_PAGE_BYTES,
                                       rgba, TEXENV_TEXTURE_SIZE * TEXENV_TEXTURE_SIZE,
                                       RIX_TEXTURE_RGB565);
}
static int build_stress(RixTriangle *triangles, size_t *count)
{
    uint32_t state = 0x5eed1234U;
    unsigned int order[STRESS_BASE_TRIANGLES];
    unsigned int blocks_per_row = STRESS_TEXTURE_SIZE / STRESS_BLOCK;
    unsigned int i;

    for (i = 0U; i < STRESS_BASE_TRIANGLES; ++i) {
        order[i] = i;
    }
    for (i = STRESS_BASE_TRIANGLES - 1U; i > 0U; --i) {
        unsigned int j = stress_random(&state) % (i + 1U);
        unsigned int swap = order[i];
        order[i] = order[j];
        order[j] = swap;
    }
    for (i = 0U; i < STRESS_BASE_TRIANGLES; ++i) {
        unsigned int block = i % (blocks_per_row * blocks_per_row);
        float size = (float)STRESS_TEXTURE_SIZE;
        float s0 = (float)((block % blocks_per_row) * STRESS_BLOCK + STRESS_BLOCK_MARGIN) / size;
        float s1 = (float)((block % blocks_per_row + 1U) * STRESS_BLOCK - STRESS_BLOCK_MARGIN) / size;
        float t0 = (float)((block / blocks_per_row) * STRESS_BLOCK + STRESS_BLOCK_MARGIN) / size;
        float t1 = (float)((block / blocks_per_row + 1U) * STRESS_BLOCK - STRESS_BLOCK_MARGIN) / size;
        float z = 0.1f + 0.02f * (float)order[i];
        RixVertex v[3];
        float area;
        unsigned int attempts = 0U;

        do {
            float cx = stress_range(&state, 140.0f, 500.0f);
            float cy = stress_range(&state, 110.0f, 370.0f);
            unsigned int k;

            for (k = 0U; k < 3U; ++k) {
                float angle = stress_range(&state, 0.0f, (float)(2.0 * PI));
                float radius = stress_range(&state, 80.0f, 170.0f);

                v[k] = vertex(stress_clamp(cx + radius * cosf(angle), 20.0f, 620.0f),
                              stress_clamp(cy + radius * sinf(angle), 20.0f, 460.0f),
                              z, stress_palette[block % 7U],
                              stress_range(&state, s0, s1), stress_range(&state, t0, t1));
            }
            area = (v[1].x - v[0].x) * (v[2].y - v[0].y) -
                   (v[2].x - v[0].x) * (v[1].y - v[0].y);
        } while (fabsf(area) < 12000.0f && ++attempts < 1000U);
        if (!add_triangle(triangles, count, &v[0], &v[1], &v[2])) {
            return 0;
        }
        stress_depth[*count - 1U] = z;
    }
    stress_base_count = *count;
    /* Disjoint 3x2 grid cells keep every lightmapped pixel blended once. */
    for (i = 0U; i < STRESS_LIGHT_TRIANGLES; ++i) {
        float x0 = 20.0f + (float)(i % 3U) * 200.0f;
        float y0 = 20.0f + (float)(i / 3U) * 220.0f;
        RixVertex a = vertex(x0 + 6.0f, y0 + 6.0f, 0.0f, 0xffffffffU, 0.0f, 0.0f);
        RixVertex b = vertex(x0 + 194.0f, y0 + 6.0f, 0.0f, 0xffffffffU, 1.0f, 0.0f);
        RixVertex c = vertex((i % 2U) ? x0 + 194.0f : x0 + 6.0f, y0 + 214.0f, 0.0f,
                             0xffffffffU, 0.5f, 1.0f);

        if (!add_triangle(triangles, count, &a, &b, &c)) {
            return 0;
        }
        stress_depth[*count - 1U] = 0.0f;
    }
    return 1;
}

/* ZERO,SRC_COLOR with the uniform lightmap: zero channels stay zero, full
 * channels become the lightmap channel; anything else is outside the exact
 * identity the reference relies on. */
static int stress_lightmap(uint16_t *pixel)
{
    uint16_t value = *pixel;
    uint16_t result = 0U;
    unsigned int r = value >> 11;
    unsigned int g = (value >> 5) & 0x3fU;
    unsigned int b = value & 0x1fU;

    if ((r != 0U && r != 0x1fU) || (g != 0U && g != 0x3fU) || (b != 0U && b != 0x1fU)) {
        return 0;
    }
    if (r != 0U) result |= LIGHTMAP_RGB565 & 0xf800U;
    if (g != 0U) result |= LIGHTMAP_RGB565 & 0x07e0U;
    if (b != 0U) result |= LIGHTMAP_RGB565 & 0x001fU;
    *pixel = result;
    return 1;
}

static int write_stress_reference(const char *directory, const char *name)
{
    static uint16_t frame[SCREEN_WIDTH * SCREEN_HEIGHT];
    static float depth[SCREEN_WIDTH * SCREEN_HEIGHT];
    uint16_t clear = rgba_to_rgb565(CLEAR_RGBA);
    unsigned int index;
    int32_t x;
    int32_t y;

    for (index = 0U; index < SCREEN_WIDTH * SCREEN_HEIGHT; ++index) {
        frame[index] = clear;
        depth[index] = 1.0f;
    }
    for (index = 0U; index < scene_triangle_count; ++index) {
        const RixTriangle *triangle = &scene_triangles[index].prepared;
        int lightmap = index >= stress_base_count;

        for (y = 0; y < (int32_t)SCREEN_HEIGHT; ++y) {
            for (x = 0; x < (int32_t)SCREEN_WIDTH; ++x) {
                size_t pixel = (size_t)y * SCREEN_WIDTH + (size_t)x;

                if (!triangle_covers(triangle, x, y)) {
                    continue;
                }
                if (lightmap) {
                    if (!stress_lightmap(&frame[pixel])) {
                        return 0;
                    }
                } else if (stress_depth[index] < depth[pixel]) {
                    frame[pixel] = rgba_to_rgb565(scene_triangles[index].paint_rgba);
                    depth[pixel] = stress_depth[index];
                }
            }
        }
    }
    /* The harness requires the screen centre to be rendered (not clear). */
    if (frame[240U * SCREEN_WIDTH + 320U] == clear ||
        frame[239U * SCREEN_WIDTH + 320U] == clear) {
        return 0;
    }
    return write_frame(directory, name, frame);
}

static int emit_stress(RixCmdWriter *writer, const RixTriangle *triangles,
                       const EfFrameConfig *config)
{
    RixFragmentState fragment;
    RixTextureState state;
    RixTexEnvState environment;
    uint32_t pages[STRESS_TEXTURE_PAGES];
    size_t index;

    memset(&fragment, 0, sizeof(fragment));
    fragment.depth_func = RIX_FUNC_LESS;
    fragment.depth_mask = 1U;
    fragment.color_mask_r = 1U;
    fragment.color_mask_g = 1U;
    fragment.color_mask_b = 1U;
    fragment.color_mask_a = 1U;
    fragment.src_factor = RIX_BLEND_ONE;
    fragment.dst_factor = RIX_BLEND_ZERO;
    memset(&state, 0, sizeof(state));
    state.width_log2 = STRESS_TEXTURE_LOG2;
    state.height_log2 = STRESS_TEXTURE_LOG2;
    state.clamp_s = 1U;
    state.clamp_t = 1U;
    state.pixel_format = RIX_TEXTURE_RGB565;
    memset(&environment, 0, sizeof(environment));
    environment.combine_rgb = RIX_TEX_REPLACE;
    environment.combine_alpha = RIX_TEX_REPLACE;
    environment.src_rgb[0] = RIX_TEXSRC_TEXTURE;
    environment.src_alpha[0] = RIX_TEXSRC_TEXTURE;
    environment.operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
    environment.operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    for (index = 0U; index < STRESS_TEXTURE_PAGES; ++index) {
        pages[index] = STRESS_TEXTURE_ADDRESS + (uint32_t)index * TEXTURE_PAGE_BYTES;
    }
    if (!rix_cmd_feature_enable(writer, RIX_FEATURE_TMU0 | RIX_FEATURE_DEPTH) ||
        !rix_cmd_fragment_state(writer, &fragment) ||
        !rix_cmd_texture_state(writer, 0U, &state) ||
        !rix_cmd_tex_env(writer, 0U, &environment) ||
        !rix_cmd_texture_pages(writer, 0U, pages, STRESS_TEXTURE_PAGES) ||
        !emit_external_frame(writer, triangles, stress_base_count, config)) {
        return 0;
    }
    fragment.depth_func = RIX_FUNC_LEQUAL;
    fragment.depth_mask = 0U;
    fragment.src_factor = RIX_BLEND_ZERO;
    fragment.dst_factor = RIX_BLEND_SRC_COLOR;
    state.width_log2 = 3U;
    state.height_log2 = 3U;
    pages[0] = LIGHTMAP_ADDRESS;
    if (!rix_cmd_feature_enable(writer, RIX_FEATURE_TMU0 | RIX_FEATURE_BLEND |
                                        RIX_FEATURE_DEPTH) ||
        !rix_cmd_fragment_state(writer, &fragment) ||
        !rix_cmd_texture_state(writer, 0U, &state) ||
        !rix_cmd_texture_pages(writer, 0U, pages, 1U)) {
        return 0;
    }
    for (index = stress_base_count; index < scene_triangle_count; ++index) {
        if (!rix_cmd_triangle(writer, triangles[index].words, RIX_TRIANGLE_WORDS)) {
            return 0;
        }
    }
    return 1;
}

static void fill_texture_page(uint8_t *page, uint32_t rgba_value)
{
    uint32_t rgba[TEXTURE_WIDTH * TEXTURE_HEIGHT];
    unsigned int x;
    unsigned int y;

    for (y = 0U; y < TEXTURE_HEIGHT; ++y) {
        for (x = 0U; x < TEXTURE_WIDTH; ++x) {
            rgba[y * TEXTURE_WIDTH + x] = rgba_value;
        }
    }
    (void)rix_texture_convert_rgba8_le(page, TEXTURE_PAGE_BYTES, rgba,
                                       TEXTURE_WIDTH * TEXTURE_HEIGHT,
                                       RIX_TEXTURE_RGB565);
}

static void make_texture(uint8_t texture[TEXTURE_BYTES])
{
    uint8_t r = (uint8_t)((LIGHTMAP_RGB565 >> 11) & 0x1fU);
    uint8_t g = (uint8_t)((LIGHTMAP_RGB565 >> 5) & 0x3fU);
    uint8_t b = (uint8_t)(LIGHTMAP_RGB565 & 0x1fU);
    uint32_t lightmap_rgba;

    memset(texture, 0, TEXTURE_BYTES);
    /* Page 0 keeps TMU0 live for flat scenes with a deterministic white texel;
     * page 1 is the uniform lightmap sampled by the ef-lm second pass. */
    fill_texture_page(&texture[0], 0xffffffffU);
    /* Expand the quantized lightmap back to the 8-bit channels the RGBA
     * conversion expects, so the RTL texel equals LIGHTMAP_RGB565 exactly. */
    lightmap_rgba = ((uint32_t)((r << 3) | (r >> 2)) << 24) |
                    ((uint32_t)((g << 2) | (g >> 4)) << 16) |
                    ((uint32_t)((b << 3) | (b >> 2)) << 8) | 0xffU;
    fill_texture_page(&texture[TEXTURE_PAGE_BYTES], lightmap_rgba);
    /* Pages 2.. hold the 256x256 ef-stress block texture. */
    fill_stress_texture(&texture[SMALL_TEXTURE_PAGES * TEXTURE_PAGE_BYTES]);
    /* Pages 18.. hold the 64x64 ef-texenv gradient texture. */
    fill_texenv_texture(&texture[(SMALL_TEXTURE_PAGES + STRESS_TEXTURE_PAGES) * TEXTURE_PAGE_BYTES]);
}

/*
 * ef-texenv: TexEnv operand-timing coverage.  A large triangle samples the
 * 64x64 gradient texture (nearest, clamped) so every fragment row presents a
 * different texel to MODULATE (texture x per-vertex interpolated primary
 * colour).  A second overlapping triangle uses INTERPOLATE with the constant
 * env colour as the blend factor, exercising all four operand registers with
 * per-pixel varying inputs.  The reference frame is the committed post-fix
 * RTL baseline (refs/ef-texenv_ref.ppm.gz); the analytic renderer cannot
 * reproduce perspective texture-coordinate interpolation bit-exactly.
 */
static int build_texenv(RixTriangle *triangles, size_t *count)
{
    RixVertex m0 = vertex(120.0f, 100.0f, 0.5f, 0xffffffffU, 0.0f, 0.0f);
    RixVertex m1 = vertex(520.0f, 140.0f, 0.5f, 0x40ff8000U, 1.0f, 0.0f);
    RixVertex m2 = vertex(320.0f, 420.0f, 0.5f, 0xff00c040U, 0.0f, 1.0f);
    RixVertex i0 = vertex(140.0f, 380.0f, 0.5f, 0xffffffffU, 1.0f, 1.0f);
    RixVertex i1 = vertex(500.0f, 400.0f, 0.5f, 0xffffffffU, 1.0f, 0.0f);
    RixVertex i2 = vertex(320.0f, 120.0f, 0.5f, 0xffffffffU, 0.0f, 1.0f);

    return add_triangle(triangles, count, &m0, &m1, &m2) &&
           add_triangle(triangles, count, &i0, &i1, &i2);
}

static int emit_texenv(RixCmdWriter *writer, const RixTriangle *triangles,
                       const EfFrameConfig *config)
{
    RixFragmentState fragment;
    RixTextureState state;
    RixTexEnvState environment;
    uint32_t pages[TEXENV_TEXTURE_PAGES];

    memset(&fragment, 0, sizeof(fragment));
    fragment.depth_func = RIX_FUNC_ALWAYS;
    fragment.depth_mask = 0U;
    fragment.color_mask_r = 1U;
    fragment.color_mask_g = 1U;
    fragment.color_mask_b = 1U;
    fragment.color_mask_a = 1U;
    fragment.src_factor = RIX_BLEND_ONE;
    fragment.dst_factor = RIX_BLEND_ZERO;
    memset(&state, 0, sizeof(state));
    state.width_log2 = TEXENV_TEXTURE_LOG2;
    state.height_log2 = TEXENV_TEXTURE_LOG2;
    state.clamp_s = 1U;
    state.clamp_t = 1U;
    state.pixel_format = RIX_TEXTURE_RGB565;
    memset(&environment, 0, sizeof(environment));
    environment.combine_rgb = RIX_TEX_MODULATE;
    environment.combine_alpha = RIX_TEX_MODULATE;
    environment.src_rgb[0] = RIX_TEXSRC_TEXTURE;
    environment.src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
    environment.src_alpha[0] = RIX_TEXSRC_TEXTURE;
    environment.src_alpha[1] = RIX_TEXSRC_PRIMARY_COLOR;
    environment.operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
    environment.operand_rgb[1] = RIX_TEXOP_SRC_COLOR;
    environment.operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    environment.operand_alpha[1] = RIX_TEXAOP_SRC_ALPHA;
    for (unsigned int index = 0U; index < TEXENV_TEXTURE_PAGES; ++index) {
        pages[index] = TEXENV_TEXTURE_ADDRESS + (uint32_t)index * TEXTURE_PAGE_BYTES;
    }
    if (!rix_cmd_feature_enable(writer, RIX_FEATURE_TMU0) ||
        !rix_cmd_fragment_state(writer, &fragment) ||
        !rix_cmd_tex_env_color(writer, 0U, 0x3080ff40U) ||
        !rix_cmd_texture_state(writer, 0U, &state) ||
        !rix_cmd_tex_env(writer, 0U, &environment) ||
        !rix_cmd_texture_pages(writer, 0U, pages, TEXENV_TEXTURE_PAGES) ||
        !emit_external_frame(writer, triangles, 1U, config)) {
        return 0;
    }
    memset(&environment, 0, sizeof(environment));
    environment.combine_rgb = RIX_TEX_INTERPOLATE;
    environment.combine_alpha = RIX_TEX_INTERPOLATE;
    environment.src_rgb[0] = RIX_TEXSRC_TEXTURE;
    environment.src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
    environment.src_rgb[2] = RIX_TEXSRC_CONSTANT;
    environment.src_alpha[0] = RIX_TEXSRC_TEXTURE;
    environment.src_alpha[1] = RIX_TEXSRC_PRIMARY_COLOR;
    environment.src_alpha[2] = RIX_TEXSRC_CONSTANT;
    environment.operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
    environment.operand_rgb[1] = RIX_TEXOP_SRC_COLOR;
    environment.operand_rgb[2] = RIX_TEXOP_SRC_COLOR;
    environment.operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    environment.operand_alpha[1] = RIX_TEXAOP_SRC_ALPHA;
    environment.operand_alpha[2] = RIX_TEXAOP_SRC_ALPHA;
    return rix_cmd_tex_env(writer, 0U, &environment) &&
           rix_cmd_triangle(writer, triangles[1].words, RIX_TRIANGLE_WORDS);
}

/*
 * Two-pass lightmap frame: pass one draws the textured base (uniform white
 * page 0, REPLACE, blending off), pass two redraws the same triangle with the
 * uniform lightmap page bound and ZERO,SRC_COLOR blending.  With a white
 * base the blender computes floor((src*255+255)/256) per channel, which
 * re-quantizes to exactly the lightmap texel's RGB565 value.
 */
static int emit_lightmap_pass(RixCmdWriter *writer, const RixTriangle *triangles,
                              size_t triangle_count)
{
    RixFragmentState fragment;
    uint32_t pages[1];
    size_t index;

    memset(&fragment, 0, sizeof(fragment));
    fragment.depth_func = RIX_FUNC_ALWAYS;
    fragment.depth_mask = 0U;
    fragment.color_mask_r = 1U;
    fragment.color_mask_g = 1U;
    fragment.color_mask_b = 1U;
    fragment.color_mask_a = 1U;
    fragment.src_factor = RIX_BLEND_ZERO;
    fragment.dst_factor = RIX_BLEND_SRC_COLOR;
    pages[0] = LIGHTMAP_ADDRESS;
    if (!rix_cmd_feature_enable(writer, RIX_FEATURE_TMU0 | RIX_FEATURE_BLEND) ||
        !rix_cmd_fragment_state(writer, &fragment) ||
        !rix_cmd_texture_pages(writer, 0U, pages, 1U)) {
        return 0;
    }
    for (index = 0U; index < triangle_count; ++index) {
        if (!rix_cmd_triangle(writer, triangles[index].words,
                              RIX_TRIANGLE_WORDS)) {
            return 0;
        }
    }
    return 1;
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
                          unsigned int scene)
{
    static uint8_t raw[COMMAND_CAPACITY];
    RixTriangle triangles[MAX_TRIANGLES];
    EfFrameConfig config;
    RixCmdWriter writer;
    size_t triangle_count = 0U;
    char binary_path[1024];
    char hex_path[1024];
    int textured = scene == 3U || scene == 4U || scene == 6U;
    int valid_scene;

    memset(triangles, 0, sizeof(triangles));
    scene_triangle_count = 0U;
    config.width = SCREEN_WIDTH;
    config.height = SCREEN_HEIGHT;
    config.color_addr = COLOR_ADDRESS;
    config.depth_addr = DEPTH_ADDRESS;
    config.clear_color = CLEAR_RGBA;
    config.clear_depth = 0xffffU;

    if (scene == 1U) {
        valid_scene = 1;
    } else if (scene == 2U) {
        valid_scene = build_s2(triangles, &triangle_count);
    } else if (scene == 3U) {
        valid_scene = build_s3(triangles, &triangle_count);
    } else if (scene == 4U) {
        valid_scene = build_s4(triangles, &triangle_count);
    } else if (scene == 5U) {
        valid_scene = build_probe(triangles, &triangle_count);
    } else if (scene == 6U) {
        valid_scene = build_lm(triangles, &triangle_count);
    } else if (scene == 7U) {
        valid_scene = build_stress(triangles, &triangle_count);
    } else if (scene == 8U) {
        valid_scene = build_texenv(triangles, &triangle_count);
    } else {
        valid_scene = 0;
    }
    if (!valid_scene) {
        return 0;
    }
    scene_triangle_count = triangle_count;

    rix_cmd_init(&writer, raw, sizeof(raw));
    if (scene == 7U) {
        valid_scene = emit_stress(&writer, triangles, &config) &&
                      write_stress_reference(directory, name);
    } else if (scene == 8U) {
        /* No analytic reference: the runner restores the committed post-fix
         * RTL baseline from refs/ef-texenv_ref.ppm.gz. */
        valid_scene = emit_texenv(&writer, triangles, &config);
    } else {
        valid_scene = setup_state(&writer, textured) &&
                      emit_external_frame(&writer, triangles, triangle_count, &config) &&
                      (scene != 6U || emit_lightmap_pass(&writer, triangles, triangle_count)) &&
                      write_reference(directory, name, CLEAR_RGBA, textured && scene != 6U);
    }
    if (!valid_scene ||
        writer.failed || writer.length == 0U || writer.length > 0x00ffffffU ||
        writer.length % 4U != 0U ||
        !make_path(binary_path, sizeof(binary_path), directory, name, ".bin") ||
        !make_path(hex_path, sizeof(hex_path), directory, name, ".hex") ||
        !write_dse_bin(binary_path, raw, writer.length) ||
        !write_dse_hex(hex_path, raw, writer.length)) {
        return 0;
    }
    printf("%s: words=%zu bytes=%zu triangles=%zu\n", name,
           writer.length / 4U + 2U, writer.length + 8U, triangle_count);
    return 1;
}

int main(int argc, char **argv)
{
    static uint8_t texture[TEXTURE_BYTES];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    make_texture(texture);
    printf("layout: commands=0x%08x color=0x%08x depth=0x%08x texture=0x%08x\n",
           COMMAND_ADDRESS, COLOR_ADDRESS, DEPTH_ADDRESS, TEXTURE_ADDRESS);
    if (!write_texture_files(argv[1], texture) ||
        !generate_scene(argv[1], "ef-s1", 1U) ||
        !generate_scene(argv[1], "ef-s2", 2U) ||
        !generate_scene(argv[1], "ef-s3", 3U) ||
        !generate_scene(argv[1], "ef-s4", 4U) ||
        !generate_scene(argv[1], "ef-probe", 5U) ||
        !generate_scene(argv[1], "ef-lm", 6U) ||
        !generate_scene(argv[1], "ef-stress", 7U) ||
        !generate_scene(argv[1], "ef-texenv", 8U)) {
        fprintf(stderr, "failed to write RasterIX XSim scene output to %s\n", argv[1]);
        return 1;
    }
    return 0;
}
