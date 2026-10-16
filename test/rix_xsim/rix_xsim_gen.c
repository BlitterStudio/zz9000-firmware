/*
 * Host-side RasterIX scene generator for the XSim ZZ9000 wrapper harness.
 *
 * Build from this directory (or adjust the source path):
 *   gcc -std=c99 -Wall -Wextra -pedantic -I D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src rix_xsim_gen.c D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src/rix_cmd.c D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src/rix_setup.c D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src/rix_bin.c D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src/rix_texture.c -lm -o rix_xsim_gen
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
#define TEXTURE_WIDTH 64U
#define TEXTURE_HEIGHT 64U
#define TEXTURE_BYTES (TEXTURE_WIDTH * TEXTURE_HEIGHT * 2U)
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

static int emit_frame(RixCmdWriter *writer, const RixBandList *bands,
                      const RixBandConfig *config)
{
    uint16_t index;

    if (!rix_cmd_resolution(writer, config->width, config->height) ||
        !rix_cmd_clear_color(writer, config->clear_color) ||
        !rix_cmd_clear_depth(writer, config->clear_depth)) {
        return 0;
    }
    for (index = 0U; index < bands->count; ++index) {
        const RixBand *band = &bands->band[index];
        uint32_t pixels = (uint32_t)config->width *
                          (uint32_t)(band->y_end - band->y_start);
        size_t reference;

        if (!rix_cmd_y_offset(writer, band->y_start) ||
            !rix_cmd_buffer_address(writer, RIX_REG_COLOR_BUFFER_ADDR,
                                    config->color_addr +
                                    (uint32_t)band->y_start * config->color_pitch) ||
            !rix_cmd_buffer_address(writer, RIX_REG_DEPTH_BUFFER_ADDR,
                                    config->depth_addr +
                                    (uint32_t)band->y_start * config->depth_pitch) ||
            !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET |
                                  RIX_FRAMEBUFFER_COLOR |
                                  RIX_FRAMEBUFFER_DEPTH, pixels)) {
            return 0;
        }
        for (reference = 0U; reference < bands->reference_count; ++reference) {
            RixTriangle triangle;
            if (!rix_triangle_in_band(bands->references[reference],
                                      band->y_start, band->y_end)) {
                continue;
            }
            triangle = *bands->references[reference];
            if (!rix_triangle_increment(&triangle, band->y_start, band->y_end) ||
                !rix_cmd_triangle(writer, triangle.words, RIX_TRIANGLE_WORDS)) {
                return 0;
            }
        }
        /* The wrapper test only writes color back.  Do not commit depth. */
        if (!rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_COMMIT |
                                 RIX_FRAMEBUFFER_COLOR, pixels)) {
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

static int add_triangle(RixBandList *bands, RixTriangle *triangles,
                        size_t *triangle_count, const RixVertex *v0,
                        const RixVertex *v1, const RixVertex *v2)
{
    RixScissor scissor;
    RixTriangle *triangle;

    if (*triangle_count >= MAX_TRIANGLES) {
        return 0;
    }
    scissor.start_x = 0U;
    scissor.start_y = 0U;
    scissor.end_x = SCREEN_WIDTH;
    scissor.end_y = SCREEN_HEIGHT;
    scissor.enabled = 1U;
    triangle = &triangles[*triangle_count];
    if (!rix_setup_triangle(triangle, v0, v1, v2, &scissor) ||
        !rix_band_list_add(bands, triangle)) {
        return 0;
    }
    ++*triangle_count;
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
    state.width_log2 = 6U;
    state.height_log2 = 6U;
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
        uint32_t color = tooth % 2U == 0U ? 0x2080ffffU : 0xffa020ffU;
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

static void make_texture(uint8_t texture[TEXTURE_BYTES])
{
    uint32_t rgba[TEXTURE_WIDTH * TEXTURE_HEIGHT];
    unsigned int x;
    unsigned int y;

    for (y = 0U; y < TEXTURE_HEIGHT; ++y) {
        for (x = 0U; x < TEXTURE_WIDTH; ++x) {
            /* Both checker colors are non-red at the S3 triangle centre. */
            rgba[y * TEXTURE_WIDTH + x] = ((x / 8U + y / 8U) & 1U) == 0U
                                              ? 0x00ffffffU : 0xffff00ffU;
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

static int generate_scene(const char *directory, const char *name, unsigned int scene)
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
    int textured = scene == 3U;
    int valid_scene;

    if (!rix_band_list_init(&bands, SCREEN_WIDTH, SCREEN_HEIGHT,
                            RIX_BAND_LAYOUT_COMPLETE, references, MAX_TRIANGLES)) {
        return 0;
    }
    if (bands.count != 20U) {
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
    } else {
        valid_scene = 0;
    }
    if (!valid_scene) {
        return 0;
    }

    rix_cmd_init(&writer, raw, sizeof(raw));
    if (!setup_state(&writer, textured) || !emit_frame(&writer, &bands, &config) ||
        writer.failed || writer.length == 0U || writer.length > 0x00ffffffU ||
        writer.length % 4U != 0U ||
        !make_path(binary_path, sizeof(binary_path), directory, name, ".bin") ||
        !make_path(hex_path, sizeof(hex_path), directory, name, ".hex") ||
        !write_dse_bin(binary_path, raw, writer.length) ||
        !write_dse_hex(hex_path, raw, writer.length)) {
        return 0;
    }
    printf("%s: words=%zu bytes=%zu bands=%u triangles=%zu\n", name,
           writer.length / 4U + 2U, writer.length + 8U, bands.count, triangle_count);
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
        !generate_scene(argv[1], "s1", 1U) ||
        !generate_scene(argv[1], "s2", 2U) ||
        !generate_scene(argv[1], "s3", 3U) ||
        !generate_scene(argv[1], "s4", 4U)) {
        fprintf(stderr, "failed to write RasterIX XSim scene output to %s\n", argv[1]);
        return 1;
    }
    return 0;
}
