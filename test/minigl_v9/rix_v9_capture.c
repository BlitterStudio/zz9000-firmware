/*
 * Wire-v9 RasterIX Scene Capture Tool.
 * Captures RasterIX command streams (commands.hex) and DDR textures (ddr.hex)
 * from wire-v9 streams, and writes CPU reference frames (cpu.ppm).  With
 * --compose it finishes the mixed GLQuake-like frame from an EF replay.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include "mw_ctx.h"
#include "mw_host.h"
#include "mw_types.h"
#include "minigl_raster.h"
#include "minigl_rix_backend.h"
#include "rix_cmd.h"

#define WIDTH 640U
#define HEIGHT 480U
#define COMMAND_BYTES MINIGL_RIX_COMMAND_BYTES
#define TEXTURE_BYTES MINIGL_RIX_TEXTURE_POOL_BYTES
#define CAPTURE_COMMAND_ADDRESS 0x00100000U
#define CAPTURE_WHITE_ADDRESS   0x00300000U
#define CAPTURE_TEXTURE_ADDRESS 0x00380000U
/*
 * The host service renders through ordinary heap pointers, unlike ARM where
 * these are physical card-pool addresses.  The xsim harness maps these stable
 * physical targets to compact backing arrays when it replays a capture.
 */
#define CAPTURE_COLOR_ADDRESS   0x07820000U
#define CAPTURE_DEPTH_ADDRESS   0x07b20000U

struct capture_model {
    uint32_t csr[16];
    uint32_t fifo[6];
    uint32_t packet[RIX_DRV_FIFO_PACKET_WORDS + 2U];
    unsigned packet_words;
    uint32_t color_address;
    uint16_t clear_color;
    uint8_t *command[MINIGL_RIX_COMMAND_BUFFERS];
    uintptr_t command_address[MINIGL_RIX_COMMAND_BUFFERS];
    uint8_t *white;
    uintptr_t white_address;
    uint8_t *texture;
    uintptr_t texture_address;
    uint8_t *submitted;
    size_t submitted_bytes;
    size_t submitted_capacity;
    unsigned submitted_count;
    /* Optional EF replay result written into the target as the hardware's
     * output when a packet draws to it (compose mode only). */
    const uint16_t *replay_color;
    const uint16_t *replay_depth;
    mw_ctx *replay_context;
    unsigned replayed;
};


static uint32_t csr_read(void *context, uint32_t offset)
{
    struct capture_model *model = (struct capture_model *)context;
    return model->csr[offset / 4U];
}

static void csr_write(void *context, uint32_t offset, uint32_t value)
{
    struct capture_model *model = (struct capture_model *)context;
    model->csr[offset / 4U] = value;
}

static uint16_t fence_color(uint32_t rgba)
{
    return (uint16_t)(((rgba >> 27U) << 11U) |
                      ((rgba >> 18U) << 5U) | (rgba >> 11U));
}

static int is_fence_address(const struct capture_model *model, uint32_t address)
{
    unsigned buffer;

    for (buffer = 0U; buffer < MINIGL_RIX_COMMAND_BUFFERS; ++buffer)
        if (address == (uint32_t)(model->command_address[buffer] + COMMAND_BYTES -
                                  MINIGL_RIX_TEXTURE_PAGE_BYTES))
            return 1;
    return 0;
}

static void render_packet(struct capture_model *model)
{
    unsigned index = 2U;
    unsigned parser_words;
    int target_drawn = 0;

    assert(model->packet_words >= 2U);
    parser_words = (model->packet[0] & 0x0fffffffU) / sizeof(uint32_t);
    assert(parser_words + 2U == model->packet_words);
    while (index < model->packet_words) {
        uint32_t command = model->packet[index];
        uint32_t kind = command & 0xf0000000U;

        if (kind == RIX_OP_RENDER_CONFIG) {
            assert(index + 1U < model->packet_words);
            if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_COLOR_BUFFER_ADDR))
                model->color_address = model->packet[index + 1U];
            else if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_CLEAR_COLOR))
                model->clear_color = fence_color(model->packet[index + 1U]);
            index += 2U;
        } else if (kind == RIX_OP_FRAMEBUFFER) {
            if ((command & 0xffU) == (RIX_FRAMEBUFFER_MEMSET |
                                      RIX_FRAMEBUFFER_COLOR) &&
                ((command >> 8U) & 0xfffffU) == 1U) {
                unsigned buffer;
                for (buffer = 0U; buffer < MINIGL_RIX_COMMAND_BUFFERS; ++buffer) {
                    if (model->color_address == model->command_address[buffer] +
                                                COMMAND_BYTES -
                                                MINIGL_RIX_TEXTURE_PAGE_BYTES) {
                        *(uint16_t *)(model->command[buffer] + COMMAND_BYTES -
                                      MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                            model->clear_color;
                        break;
                    }
                }
            }
            ++index;
        } else if (kind == RIX_OP_TRIANGLE_STREAM) {
            unsigned words = (command & 0x3ffU) / sizeof(uint32_t);
            assert((command & 3U) == 0U && index + 1U + words <= model->packet_words);
            if (!is_fence_address(model, model->color_address))
                target_drawn = 1;
            index += 1U + words;
        } else if (kind == RIX_OP_TEXTURE_STREAM) {
            unsigned pages = command & 0x7ffffU;
            assert(index + 1U + pages <= model->packet_words);
            index += 1U + pages;
        } else if (kind == RIX_OP_FOG_LUT_STREAM) {
            unsigned words = (command & 0xffffU) / sizeof(uint32_t);
            assert((command & 3U) == 0U && index + 1U + words <= model->packet_words);
            index += 1U + words;
        } else {
            assert(!"unknown RasterIX parser command");
        }
    }
    if (target_drawn && model->replay_color != NULL) {
        uint16_t *color = (uint16_t *)(uintptr_t)minigl_raster_color_buffer();

        assert(model->replay_context != NULL && model->replayed == 0U);
        memcpy(color, model->replay_color, WIDTH * HEIGHT * sizeof(*color));
        memcpy(model->replay_context->depth_buffer, model->replay_depth,
               WIDTH * HEIGHT * sizeof(*model->replay_depth));
        ++model->replayed;
    }
}

static void append_packet(struct capture_model *model)
{
    unsigned index;
    size_t bytes = (size_t)model->packet_words * sizeof(model->packet[0]);

    assert(model->packet_words >= 2U);
    assert(model->packet[0] == (0x90000000U |
                                ((model->packet_words - 2U) * sizeof(uint32_t))));
    assert(model->packet[1] == 0U);
    assert(model->submitted_bytes + bytes <= model->submitted_capacity);
    for (index = 0U; index < model->packet_words; ++index) {
        uint32_t word = model->packet[index];
        uint8_t *destination = model->submitted + model->submitted_bytes +
                               (size_t)index * sizeof(word);

        destination[0] = (uint8_t)word;
        destination[1] = (uint8_t)(word >> 8U);
        destination[2] = (uint8_t)(word >> 16U);
        destination[3] = (uint8_t)(word >> 24U);
    }
    model->submitted_bytes += bytes;
    ++model->submitted_count;
}

static uint32_t fifo_read(void *context, uint32_t offset)
{
    struct capture_model *model = (struct capture_model *)context;
    return model->fifo[offset / 4U];
}

static void fifo_write(void *context, uint32_t offset, uint32_t value)
{
    struct capture_model *model = (struct capture_model *)context;

    model->fifo[offset / 4U] = value;
    if (offset == RIX_DRV_FIFO_TDFR) {
        model->packet_words = 0U;
    } else if (offset == RIX_DRV_FIFO_TDFD) {
        assert(model->packet_words < RIX_DRV_FIFO_PACKET_WORDS + 2U);
        model->packet[model->packet_words++] = value;
    } else if (offset == RIX_DRV_FIFO_TLF) {
        assert(value == model->packet_words * sizeof(model->packet[0]));
        render_packet(model);
        append_packet(model);
        model->packet_words = 0U;
    }
}

static void cache_noop(void *ctx, uintptr_t addr, size_t bytes) { (void)ctx; (void)addr; (void)bytes; }

static void capture_model_init(struct capture_model *model)
{
    unsigned index;
    memset(model, 0, sizeof(*model));
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model->fifo[RIX_DRV_FIFO_TDFV / 4U] = RIX_DRV_FIFO_PACKET_WORDS + 2U;
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index) {
        model->command[index] = (uint8_t *)calloc(1U, COMMAND_BYTES);
        assert(model->command[index] != NULL);
        model->command_address[index] = CAPTURE_COMMAND_ADDRESS + (uintptr_t)index * COMMAND_BYTES;
    }
    model->white = (uint8_t *)calloc(1U, MINIGL_RIX_WHITE_TEXTURE_BYTES);
    model->texture = (uint8_t *)calloc(1U, TEXTURE_BYTES);
    model->submitted_capacity = 32U * COMMAND_BYTES;
    model->submitted = (uint8_t *)calloc(1U, model->submitted_capacity);
    assert(model->white && model->texture && model->submitted);
    model->white_address = CAPTURE_WHITE_ADDRESS;
    model->texture_address = CAPTURE_TEXTURE_ADDRESS;
}

static void capture_model_destroy(struct capture_model *model)
{
    unsigned index;
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index)
        free(model->command[index]);
    free(model->white);
    free(model->texture);
    free(model->submitted);
}

static void configure_capture_platform(struct capture_model *model)
{
    MiniGLRixPlatform platform;
    memset(&platform, 0, sizeof(platform));
    platform.context = model;
    platform.csr_read = csr_read;
    platform.csr_write = csr_write;
    platform.fifo_read = fifo_read;
    platform.fifo_write = fifo_write;
    platform.cache_clean = cache_noop;
    platform.cache_invalidate = cache_noop;
    platform.command_buffer[0] = model->command[0];
    platform.command_buffer[1] = model->command[1];
    platform.command_address[0] = model->command_address[0];
    platform.command_address[1] = model->command_address[1];
    platform.command_capacity = COMMAND_BYTES;
    platform.white_texture = model->white;
    platform.white_texture_address = model->white_address;
    platform.white_texture_bytes = MINIGL_RIX_WHITE_TEXTURE_BYTES;
    platform.texture_memory = model->texture;
    platform.texture_address = model->texture_address;
    platform.texture_bytes = TEXTURE_BYTES;
    minigl_raster_set_rix_platform(&platform);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static int relocate_address(uint32_t address, const uint32_t *sources,
                            size_t count, uint32_t bytes, uint32_t target,
                            uint32_t *relocated)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        uint32_t offset = address - sources[index];

        if (offset < bytes) {
            *relocated = target + offset;
            return 1;
        }
    }
    return 0;
}

/*
 * Host MiniGL uses process pointers for its targets.  Rewrite only the
 * RasterIX color/depth register values in the captured DSE payload, preserving
 * every backend command and all offsets.  The resulting physical addresses
 * match the sparse xsim DDR aliases.
 */
static void relocate_capture_targets(struct capture_model *model,
                                     const mw_ctx *context,
                                     unsigned *color_count,
                                     unsigned *depth_count)
{
    uint32_t color_sources[3];
    uint32_t depth_source;
    size_t stream = 0U;
    size_t index;

    assert(model != NULL && context != NULL);
    *color_count = 0U;
    *depth_count = 0U;
    for (index = 0U; index < 3U; ++index)
        color_sources[index] = (uint32_t)(uintptr_t)context->color_buffers[index];
    depth_source = (uint32_t)(uintptr_t)context->depth_buffer;

    while (stream < model->submitted_bytes) {
        uint32_t dse_header;
        size_t parser;
        size_t parser_end;

        assert(model->submitted_bytes - stream >= 8U);
        dse_header = read_le32(model->submitted + stream);
        assert((dse_header & 0xf0000000U) == 0x90000000U);
        parser = stream + 8U;
        parser_end = parser + (dse_header & 0x0fffffffU);
        assert(parser_end >= parser && parser_end <= model->submitted_bytes);
        while (parser < parser_end) {
            uint32_t command = read_le32(model->submitted + parser);
            uint32_t kind = command & 0xf0000000U;

            if (kind == RIX_OP_RENDER_CONFIG) {
                uint32_t value;
                uint32_t relocated;

                assert(parser_end - parser >= 8U);
                value = read_le32(model->submitted + parser + 4U);
                if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_COLOR_BUFFER_ADDR)) {
                    /* The fence page lives in the command buffer, which the
                     * harness loads at its captured physical address. */
                    if (!is_fence_address(model, value)) {
                        assert(relocate_address(value, color_sources, 3U,
                                                WIDTH * HEIGHT * sizeof(uint32_t),
                                                CAPTURE_COLOR_ADDRESS, &relocated));
                        write_le32(model->submitted + parser + 4U, relocated);
                        ++*color_count;
                    }
                } else if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_DEPTH_BUFFER_ADDR)) {
                    assert(relocate_address(value, &depth_source, 1U,
                                            WIDTH * HEIGHT * sizeof(uint32_t),
                                            CAPTURE_DEPTH_ADDRESS, &relocated));
                    write_le32(model->submitted + parser + 4U, relocated);
                    ++*depth_count;
                }
                parser += 8U;
            } else if (kind == RIX_OP_FRAMEBUFFER) {
                parser += 4U;
            } else if (kind == RIX_OP_TRIANGLE_STREAM) {
                size_t bytes = command & 0x3ffU;

                assert((bytes & 3U) == 0U && bytes <= parser_end - parser - 4U);
                parser += 4U + bytes;
            } else if (kind == RIX_OP_TEXTURE_STREAM) {
                size_t pages = command & 0x7ffffU;

                assert(pages <= (parser_end - parser - 4U) / 4U);
                parser += 4U + pages * 4U;
            } else if (kind == RIX_OP_FOG_LUT_STREAM) {
                size_t bytes = command & 0xffffU;

                assert((bytes & 3U) == 0U && bytes <= parser_end - parser - 4U);
                parser += 4U + bytes;
            } else {
                assert(!"unknown RasterIX parser command");
            }
        }
        assert(parser == parser_end);
        stream = parser_end;
    }
    assert(stream == model->submitted_bytes);
}

static void write_words(const char *path, const uint8_t *bytes, size_t count)
{
    FILE *file = fopen(path, "w");
    size_t offset;
    assert(file != NULL && count % 4U == 0U);
    for (offset = 0U; offset < count; offset += 4U) {
        uint32_t word = (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1U] << 8U) |
                        ((uint32_t)bytes[offset + 2U] << 16U) | ((uint32_t)bytes[offset + 3U] << 24U);
        assert(fprintf(file, "%08x\n", word) > 0);
    }
    assert(fclose(file) == 0);
}

static void write_ddr_range(FILE *file, uint32_t address, const uint8_t *bytes, size_t count)
{
    size_t offset;
    assert(fprintf(file, "@%08x\n", address) > 0);
    for (offset = 0U; offset < count; ++offset)
        assert(fprintf(file, "%02x\n", bytes[offset]) > 0);
}

static void write_nonzero_ddr_range(FILE *file, uint32_t address, const uint8_t *bytes, size_t count)
{
    size_t offset = 0U;
    while (offset < count) {
        while (offset < count && bytes[offset] == 0U) ++offset;
        if (offset == count) break;
        assert(fprintf(file, "@%08x\n", address + (uint32_t)offset) > 0);
        do {
            assert(fprintf(file, "%02x\n", bytes[offset]) > 0);
            ++offset;
        } while (offset < count && bytes[offset] != 0U);
    }
}

static void write_ddr(const char *path, const struct capture_model *model)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    write_ddr_range(file, (uint32_t)model->white_address, model->white, MINIGL_RIX_WHITE_TEXTURE_BYTES);
    write_nonzero_ddr_range(file, (uint32_t)model->texture_address, model->texture, TEXTURE_BYTES);
    assert(fclose(file) == 0);
}

static void write_ppm(const char *path)
{
    const uint32_t *color = minigl_raster_color_buffer();
    FILE *file = fopen(path, "wb");
    unsigned x, y;
    assert(color != NULL && file != NULL);
    assert(fprintf(file, "P6\n%u %u\n255\n", WIDTH, HEIGHT) > 0);
    for (y = 0U; y < HEIGHT; ++y) {
        for (x = 0U; x < WIDTH; ++x) {
            uint32_t pixel = color[(HEIGHT - 1U - y) * WIDTH + x];
            assert(fputc((int)(pixel >> 24U), file) != EOF);
            assert(fputc((int)(pixel >> 16U), file) != EOF);
            assert(fputc((int)(pixel >> 8U), file) != EOF);
        }
    }
    assert(fclose(file) == 0);
}

/* Gear geometry data structures */
struct gear_spec { float inner, outer, width, tooth; int teeth; };
struct gear_mesh {
    float face[2][18 * 7 * 3 * 3];
    float rim[18 * 4 * 6 * 3];
    float normal[18 * 4 * 3];
};
static const struct gear_spec gear_specs[3] = {
    { 0.45f, 1.25f, 0.42f, 0.22f, 18 },
    { 0.35f, 0.90f, 0.34f, 0.18f, 14 },
    { 0.28f, 0.72f, 0.28f, 0.15f, 12 }
};
static struct gear_mesh gears[3];

static void gear_point(float *out, float radius, float angle, float z)
{
    out[0] = radius * cosf(angle); out[1] = radius * sinf(angle); out[2] = z;
}

static void build_gear(const struct gear_spec *spec, struct gear_mesh *mesh)
{
    const float step = 2.0f * 3.14159265358979323846f / (float)spec->teeth;
    const float half = spec->width * 0.5f;
    const float root = spec->outer - spec->tooth * 0.5f;
    const float tip = spec->outer + spec->tooth * 0.5f;
    int tooth, face, rim = 0, normal = 0;

    memset(mesh, 0, sizeof(*mesh));
    for (face = 0; face < 2; ++face) {
        int vertex = 0;
        float z = face == 0 ? half : -half;
        for (tooth = 0; tooth < spec->teeth; ++tooth) {
            const float a0 = (float)tooth * step;
            const float a1 = a0 + step * .25f;
            const float a2 = a0 + step * .50f;
            const float a3 = a0 + step * .75f;
            const float a4 = a0 + step;
            float *dst = mesh->face[face];
#define FACE_TRI(rA, aA, rB, aB, rC, aC) do { \
    gear_point(dst + vertex * 3, rA, aA, z); \
    gear_point(dst + (vertex + 1) * 3, rB, aB, z); \
    gear_point(dst + (vertex + 2) * 3, rC, aC, z); \
    vertex += 3; \
} while(0)
            if (face == 0) {
                FACE_TRI(spec->inner, a0, root, a0, root, a1);
                FACE_TRI(spec->inner, a0, root, a1, spec->inner, a2);
                FACE_TRI(spec->inner, a2, root, a1, tip, a1);
                FACE_TRI(spec->inner, a2, tip, a1, tip, a2);
                FACE_TRI(spec->inner, a2, tip, a2, root, a3);
                FACE_TRI(spec->inner, a2, root, a3, root, a4);
                FACE_TRI(spec->inner, a2, root, a4, spec->inner, a4);
            } else {
                FACE_TRI(spec->inner, a0, root, a1, root, a0);
                FACE_TRI(spec->inner, a0, spec->inner, a2, root, a1);
                FACE_TRI(spec->inner, a2, tip, a1, root, a1);
                FACE_TRI(spec->inner, a2, tip, a2, tip, a1);
                FACE_TRI(spec->inner, a2, root, a3, tip, a2);
                FACE_TRI(spec->inner, a2, root, a4, root, a3);
                FACE_TRI(spec->inner, a2, spec->inner, a4, root, a4);
            }
#undef FACE_TRI
        }
    }
    for (tooth = 0; tooth < spec->teeth; ++tooth) {
        const float angles[5] = { (float)tooth * step, (float)tooth * step + step * .25f,
                                  (float)tooth * step + step * .50f, (float)tooth * step + step * .75f,
                                  (float)(tooth + 1) * step };
        const float radii[5] = { root, root, tip, tip, root };
        int edge;
        for (edge = 0; edge < 4; ++edge) {
            float *n = mesh->normal + normal * 3;
            float *dst = mesh->rim + rim * 3;
            float angle = (angles[edge] + angles[edge + 1]) * .5f;
            n[0] = cosf(angle); n[1] = sinf(angle);
            gear_point(dst, radii[edge], angles[edge], -half);
            gear_point(dst + 3, radii[edge + 1], angles[edge + 1], -half);
            gear_point(dst + 6, radii[edge + 1], angles[edge + 1], half);
            gear_point(dst + 9, radii[edge], angles[edge], -half);
            gear_point(dst + 12, radii[edge + 1], angles[edge + 1], half);
            gear_point(dst + 15, radii[edge], angles[edge], half);
            ++normal;
            rim += 6;
        }
    }
}

static void build_gears(void)
{
    unsigned index;
    for (index = 0U; index < 3U; ++index) build_gear(&gear_specs[index], &gears[index]);
}

/* Helper to build wire-v9 packet header */
static uint8_t *packet_init(uint8_t *buf, uint32_t cmd_count)
{
    mw_put_be32(buf, MW_MAGIC);
    mw_put_be32(buf + 4, MW_VERSION);
    mw_put_be32(buf + 12, cmd_count);
    return buf + 16;
}

static void packet_finish(uint8_t *buf, uint8_t *q)
{
    uint32_t total = (uint32_t)(q - buf);
    mw_put_be32(buf + 8, total);
}

static void put_state_u32(uint8_t **cursor, uint32_t key, uint32_t value)
{
    uint8_t *q = *cursor;
    mw_put_be32(q, MW_STATE);
    mw_put_be32(q + 4, 12U);
    mw_put_be32(q + 8, key);
    mw_put_be32(q + 12, 1U);
    mw_put_be32(q + 16, value);
    *cursor = q + 20;
}

static void put_state_float2(uint8_t **cursor, uint32_t key, float first, float second)
{
    uint8_t *q = *cursor;
    mw_put_be32(q, MW_STATE);
    mw_put_be32(q + 4, 16U);
    mw_put_be32(q + 8, key);
    mw_put_be32(q + 12, 2U);
    mw_put_bef(q + 16, first);
    mw_put_bef(q + 20, second);
    *cursor = q + 24;
}

static void put_state_u32_float(uint8_t **cursor, uint32_t key,
                                uint32_t first, float second)
{
    uint8_t *q = *cursor;
    mw_put_be32(q, MW_STATE);
    mw_put_be32(q + 4, 16U);
    mw_put_be32(q + 8, key);
    mw_put_be32(q + 12, 2U);
    mw_put_be32(q + 16, first);
    mw_put_bef(q + 20, second);
    *cursor = q + 24;
}

static void put_texture_stage(uint8_t **cursor, uint32_t unit, uint32_t name,
                              uint32_t enabled, uint32_t environment)
{
    uint8_t *q = *cursor;
    mw_put_be32(q, MW_STATE);
    mw_put_be32(q + 4, 24U);
    mw_put_be32(q + 8, MW_TEXTURE_STAGE);
    mw_put_be32(q + 12, 4U);
    mw_put_be32(q + 16, unit);
    mw_put_be32(q + 20, name);
    mw_put_be32(q + 24, enabled);
    mw_put_be32(q + 28, environment);
    *cursor = q + 32;
}

static void put_blend_factors(uint8_t **cursor, uint32_t source, uint32_t destination)
{
    uint8_t *q = *cursor;
    mw_put_be32(q, MW_STATE);
    mw_put_be32(q + 4, 24U);
    mw_put_be32(q + 8, MW_BLEND_FACTORS);
    mw_put_be32(q + 12, 4U);
    mw_put_be32(q + 16, source);
    mw_put_be32(q + 20, destination);
    mw_put_be32(q + 24, source);
    mw_put_be32(q + 28, destination);
    *cursor = q + 32;
}

static void put_texture_image(uint8_t **cursor, uint32_t name,
                              unsigned width, unsigned height,
                              const uint8_t *rgba)
{
    uint8_t *q = *cursor;
    uint32_t source_bytes = width * height * 4U;
    uint32_t payload_bytes = 56U + source_bytes;

    mw_put_be32(q, MW_TEXTURE_IMAGE_RAW);
    mw_put_be32(q + 4, payload_bytes);
    mw_put_be32(q + 8, name);
    mw_put_be32(q + 12, 0U);
    mw_put_be32(q + 16, 4U);
    mw_put_be32(q + 20, width);
    mw_put_be32(q + 24, height);
    mw_put_be32(q + 28, 0x1908U);
    mw_put_be32(q + 32, 0x1401U);
    mw_put_be32(q + 36, 4U);
    mw_put_be32(q + 40, 0U);
    mw_put_be32(q + 44, 0U);
    mw_put_be32(q + 48, 0U);
    mw_put_be32(q + 52, 0U);
    mw_put_be32(q + 56, 0U);
    mw_put_be32(q + 60, source_bytes);
    memcpy(q + 64, rgba, source_bytes);
    *cursor = q + 8U + payload_bytes;
}

static void put_texture_filter(uint8_t **cursor, uint32_t name,
                               uint32_t pname, uint32_t value)
{
    uint8_t *q = *cursor;
    mw_put_be32(q, MW_TEXTURE_PARAM);
    mw_put_be32(q + 4, 12U);
    mw_put_be32(q + 8, name);
    mw_put_be32(q + 12, pname);
    mw_put_be32(q + 16, value);
    *cursor = q + 20;
}

static void put_vertex(uint8_t **cursor, float x, float y, float z,
                       float red, float green, float blue, float alpha,
                       float s0, float t0, float s1, float t1)
{
    uint8_t *q = *cursor;

    mw_put_bef(q, x);
    mw_put_bef(q + 4, y);
    mw_put_bef(q + 8, z);
    mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, red);
    mw_put_bef(q + 20, green);
    mw_put_bef(q + 24, blue);
    mw_put_bef(q + 28, alpha);
    mw_put_bef(q + 32, s0);
    mw_put_bef(q + 36, t0);
    mw_put_bef(q + 40, 0.0f);
    mw_put_bef(q + 44, 1.0f);
    mw_put_bef(q + 48, s1);
    mw_put_bef(q + 52, t1);
    mw_put_bef(q + 56, 0.0f);
    mw_put_bef(q + 60, 1.0f);
    mw_put_bef(q + 64, 0.0f);
    mw_put_bef(q + 68, 0.0f);
    mw_put_bef(q + 72, 1.0f);
    *cursor = q + 76;
}

/* SCENE 1: Blended quad (two triangles sharing diagonal) */
static void submit_blended_quad(mw_ctx *c)
{
    static uint8_t packet[2048];
    uint8_t *q = packet_init(packet, 4);

    /* 1. MW_CLEAR: black clear */
    mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, 3);
    mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 1.0f);
    mw_put_bef(q + 28, 1.0f);
    q += 32;

    /* 2. MW_BLEND: enabled */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_BLEND); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;

    /* 3. MW_BLEND_FACTORS: SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, MW_BLEND_FACTORS); mw_put_be32(q + 12, 4);
    mw_put_be32(q + 16, 0x0302); mw_put_be32(q + 20, 0x0303); /* RGB */
    mw_put_be32(q + 24, 0x0302); mw_put_be32(q + 28, 0x0303); /* Alpha */
    q += 32;

    /* 4. MW_DRAW_OBJECT: 2 triangles sharing diagonal (-0.5, -0.5) to (0.5, 0.5) */
    /* Alpha = 0.5 (semi-transparent red: 1.0, 0.0, 0.0, 0.5) */
    uint32_t obj_payload = 8 + 6 * 76;
    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, obj_payload);
    mw_put_be32(q + 8, 4);  /* GL_TRIANGLES */
    mw_put_be32(q + 12, 6); /* count 6 */
    q += 16;

    /* Triangle 1: (-0.5, -0.5), (0.5, -0.5), (0.5, 0.5) */
    mw_put_bef(q, -0.5f); mw_put_bef(q + 4, -0.5f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 0.0f); mw_put_bef(q + 28, 0.5f);
    q += 76;
    mw_put_bef(q, 0.5f); mw_put_bef(q + 4, -0.5f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 0.0f); mw_put_bef(q + 28, 0.5f);
    q += 76;
    mw_put_bef(q, 0.5f); mw_put_bef(q + 4, 0.5f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 0.0f); mw_put_bef(q + 28, 0.5f);
    q += 76;

    /* Triangle 2: (-0.5, -0.5), (0.5, 0.5), (-0.5, 0.5) */
    mw_put_bef(q, -0.5f); mw_put_bef(q + 4, -0.5f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 0.0f); mw_put_bef(q + 28, 0.5f);
    q += 76;
    mw_put_bef(q, 0.5f); mw_put_bef(q + 4, 0.5f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 0.0f); mw_put_bef(q + 28, 0.5f);
    q += 76;
    mw_put_bef(q, -0.5f); mw_put_bef(q + 4, 0.5f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 0.0f); mw_put_bef(q + 24, 0.0f); mw_put_bef(q + 28, 0.5f);
    q += 76;

    packet_finish(packet, q);
    uint32_t status = mw_host_submit(c, packet, (uint32_t)(q - packet));
    assert(status == MW_STATUS_OK);
    (void)minigl_raster_finish();
}

/* SCENE 2: Gears frame */
static void submit_gears(mw_ctx *c)
{
    static uint8_t packet[128 * 1024];
    uint8_t *q = packet_init(packet, 6);

    /* 1. Clear */
    mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, 3);
    mw_put_bef(q + 12, 0.04f); mw_put_bef(q + 16, 0.04f); mw_put_bef(q + 20, 0.08f); mw_put_bef(q + 24, 1.0f);
    mw_put_bef(q + 28, 1.0f);
    q += 32;

    /* 2. Depth test */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_TEST); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;

    /* 3. Depth func LESS */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_FUNC); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 0x0201);
    q += 20;

    /* 4. Cull face */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_CULL); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;

    /* 5. Projection matrix (Frustum: -1, 1, -0.75, 0.75, 2.5, 20.0) */
    float proj[16] = {0};
    proj[0] = 2.5f; proj[5] = 2.5f / 0.75f;
    proj[10] = -(20.0f + 2.5f) / (20.0f - 2.5f);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * 20.0f * 2.5f) / (20.0f - 2.5f);
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 72);
    mw_put_be32(q + 8, MW_PROJECTION); mw_put_be32(q + 12, 16);
    memcpy(q + 16, proj, 64);
    q += 80;

    /* 6. Draw Gear 1 face */
    const struct gear_spec *spec = &gear_specs[0];
    const struct gear_mesh *mesh = &gears[0];
    int tri_count = spec->teeth * 7;
    uint32_t obj_payload = 8 + (uint32_t)tri_count * 3 * 76;
    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, obj_payload);
    mw_put_be32(q + 8, 4); /* GL_TRIANGLES */
    mw_put_be32(q + 12, (uint32_t)tri_count * 3);
    q += 16;

    for (int t = 0; t < tri_count * 3; ++t) {
        float x = mesh->face[0][t * 3];
        float y = mesh->face[0][t * 3 + 1];
        float z = mesh->face[0][t * 3 + 2] - 5.0f; /* push into view */
        mw_put_bef(q, x); mw_put_bef(q + 4, y); mw_put_bef(q + 8, z); mw_put_bef(q + 12, 1.0f);
        mw_put_bef(q + 16, 0.82f); mw_put_bef(q + 20, 0.18f); mw_put_bef(q + 24, 0.12f); mw_put_bef(q + 28, 1.0f);
        q += 76;
    }

    packet_finish(packet, q);
    uint32_t status = mw_host_submit(c, packet, (uint32_t)(q - packet));
    assert(status == MW_STATUS_OK);
    (void)minigl_raster_finish();
}

/* SCENE 3: Unit-0 diffuse plus unit-1 lightmap in one GL draw. */
static void submit_textured_lightmap(mw_ctx *c)
{
    static uint8_t tex_diffuse[64 * 64 * 4];
    static uint8_t tex_lightmap[64 * 64 * 4];
    unsigned x, y;

    /* Solid, non-white inputs isolate texture upload/fetch and the second
     * pass blend from the two renderers' different texel-boundary rounding. */
    for (y = 0; y < 64; ++y) {
        for (x = 0; x < 64; ++x) {
            uint8_t *p = tex_diffuse + (y * 64 + x) * 4;
            uint8_t *lm = tex_lightmap + (y * 64 + x) * 4;
            p[0] = 198U; p[1] = 132U; p[2] = 66U; p[3] = 255U;
            lm[0] = 160U; lm[1] = 160U; lm[2] = 160U; lm[3] = 255U;
        }
    }

    /* 1. Upload Texture 1 (diffuse) */
    static uint8_t up_packet[64 * 1024];
    uint8_t *q = packet_init(up_packet, 6);

    mw_put_be32(q, MW_TEXTURE_IMAGE_RAW);
    uint32_t up_bytes = 56 + 64 * 64 * 4;
    mw_put_be32(q + 4, up_bytes);
    mw_put_be32(q + 8, 1);       /* name */
    mw_put_be32(q + 12, 0);      /* level */
    mw_put_be32(q + 16, 4);      /* internal 4 (RGBA) */
    mw_put_be32(q + 20, 64);     /* width */
    mw_put_be32(q + 24, 64);     /* height */
    mw_put_be32(q + 28, 0x1908); /* format GL_RGBA */
    mw_put_be32(q + 32, 0x1401); /* type GL_UNSIGNED_BYTE */
    mw_put_be32(q + 36, 4);      /* alignment */
    mw_put_be32(q + 40, 0);      /* row_length */
    mw_put_be32(q + 44, 0);      /* skip_rows */
    mw_put_be32(q + 48, 0);      /* skip_pixels */
    mw_put_be32(q + 52, 0);      /* swap */
    mw_put_be32(q + 56, 0);      /* lsb */
    mw_put_be32(q + 60, 64 * 64 * 4); /* source bytes */
    memcpy(q + 64, tex_diffuse, 64 * 64 * 4);
    q += 8 + up_bytes;

    /* Upload Texture 2 (lightmap) */
    mw_put_be32(q, MW_TEXTURE_IMAGE_RAW);
    mw_put_be32(q + 4, up_bytes);
    mw_put_be32(q + 8, 2);       /* name */
    mw_put_be32(q + 12, 0);
    mw_put_be32(q + 16, 4);
    mw_put_be32(q + 20, 64);
    mw_put_be32(q + 24, 64);
    mw_put_be32(q + 28, 0x1908);
    mw_put_be32(q + 32, 0x1401);
    mw_put_be32(q + 36, 4);
    mw_put_be32(q + 40, 0);
    mw_put_be32(q + 44, 0);
    mw_put_be32(q + 48, 0);
    mw_put_be32(q + 52, 0);
    mw_put_be32(q + 56, 0);
    mw_put_be32(q + 60, 64 * 64 * 4);
    memcpy(q + 64, tex_lightmap, 64 * 64 * 4);
    q += 8 + up_bytes;

    /* Texture params: nearest */
    mw_put_be32(q, MW_TEXTURE_PARAM); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, 1); mw_put_be32(q + 12, 0x2801); mw_put_be32(q + 16, 0x2600);
    q += 20;
    mw_put_be32(q, MW_TEXTURE_PARAM); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, 2); mw_put_be32(q + 12, 0x2801); mw_put_be32(q + 16, 0x2600);
    q += 20;
    mw_put_be32(q, MW_TEXTURE_PARAM); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, 1); mw_put_be32(q + 12, 0x2800); mw_put_be32(q + 16, 0x2600);
    q += 20;
    mw_put_be32(q, MW_TEXTURE_PARAM); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, 2); mw_put_be32(q + 12, 0x2800); mw_put_be32(q + 16, 0x2600);
    q += 20;

    packet_finish(up_packet, q);
    uint32_t status = mw_host_submit(c, up_packet, (uint32_t)(q - up_packet));
    assert(status == MW_STATUS_OK);

    /* One GL draw: unit 0 replaces primary colour; unit 1 modulates it. */
    static uint8_t draw_packet[4096];
    q = packet_init(draw_packet, 7);

    /* Clear */
    mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, 3);
    mw_put_bef(q + 12, 0.02f); mw_put_bef(q + 16, 0.03f); mw_put_bef(q + 20, 0.06f); mw_put_bef(q + 24, 1.0f);
    mw_put_bef(q + 28, 1.0f);
    q += 32;

    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, MW_TEXTURE_STAGE); mw_put_be32(q + 12, 4);
    mw_put_be32(q + 16, 0); mw_put_be32(q + 20, 1); mw_put_be32(q + 24, 1); mw_put_be32(q + 28, 0x1e01);
    q += 32;
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, MW_TEXTURE_STAGE); mw_put_be32(q + 12, 4);
    mw_put_be32(q + 16, 1); mw_put_be32(q + 20, 2); mw_put_be32(q + 24, 1); mw_put_be32(q + 28, 0x2100);
    q += 32;

    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_TEST); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_FUNC); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 0x0203);
    q += 20;
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_MASK); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;

    {
        uint32_t obj_payload = 8 + 6 * 76;
        mw_put_be32(q, MW_DRAW_OBJECT); mw_put_be32(q + 4, obj_payload);
        mw_put_be32(q + 8, 4); mw_put_be32(q + 12, 6);
        q += 16;
#define EMIT_V(px, py, s, t) do { \
    mw_put_bef(q, px); mw_put_bef(q + 4, py); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f); \
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); \
    mw_put_bef(q + 32, s); mw_put_bef(q + 36, t); mw_put_bef(q + 40, 0.0f); mw_put_bef(q + 44, 1.0f); \
    mw_put_bef(q + 48, s); mw_put_bef(q + 52, t); mw_put_bef(q + 56, 0.0f); mw_put_bef(q + 60, 1.0f); \
    mw_put_bef(q + 64, 0.0f); mw_put_bef(q + 68, 0.0f); mw_put_bef(q + 72, 1.0f); \
    q += 76; \
} while(0)
        EMIT_V(-0.8f, -0.8f, 0.001953125f, 0.001953125f);
        EMIT_V( 0.8f, -0.8f, 0.998046875f, 0.001953125f);
        EMIT_V( 0.8f,  0.8f, 0.998046875f, 0.998046875f);
        EMIT_V(-0.8f, -0.8f, 0.001953125f, 0.001953125f);
        EMIT_V( 0.8f,  0.8f, 0.998046875f, 0.998046875f);
        EMIT_V(-0.8f,  0.8f, 0.001953125f, 0.998046875f);
#undef EMIT_V
    }

    packet_finish(draw_packet, q);
    status = mw_host_submit(c, draw_packet, (uint32_t)(q - draw_packet));
    assert(status == MW_STATUS_OK);
    (void)minigl_raster_finish();
}

/* SCENE 4: Single diffuse texture (plain textured quad) */
static void submit_plain_textured(mw_ctx *c)
{
    static uint8_t tex_diffuse[64 * 64 * 4];
    unsigned x, y;

    /* The plain case deliberately uses one non-white texel value throughout:
     * it proves the texture path without conflating fetch correctness with
     * the known CPU/RasterIX nearest-coordinate boundary convention. */
    for (y = 0; y < 64; ++y) {
        for (x = 0; x < 64; ++x) {
            uint8_t *p = tex_diffuse + (y * 64 + x) * 4;
            p[0] = 198U; p[1] = 132U; p[2] = 66U; p[3] = 255U;
        }
    }

    static uint8_t up_packet[32 * 1024];
    uint8_t *q = packet_init(up_packet, 3);

    mw_put_be32(q, MW_TEXTURE_IMAGE_RAW);
    uint32_t up_bytes = 56 + 64 * 64 * 4;
    mw_put_be32(q + 4, up_bytes);
    mw_put_be32(q + 8, 1);       /* name */
    mw_put_be32(q + 12, 0);      /* level */
    mw_put_be32(q + 16, 4);      /* internal 4 (RGBA) */
    mw_put_be32(q + 20, 64);     /* width */
    mw_put_be32(q + 24, 64);     /* height */
    mw_put_be32(q + 28, 0x1908); /* format GL_RGBA */
    mw_put_be32(q + 32, 0x1401); /* type GL_UNSIGNED_BYTE */
    mw_put_be32(q + 36, 4);      /* alignment */
    mw_put_be32(q + 40, 0);      /* row_length */
    mw_put_be32(q + 44, 0);      /* skip_rows */
    mw_put_be32(q + 48, 0);      /* skip_pixels */
    mw_put_be32(q + 52, 0);      /* swap */
    mw_put_be32(q + 56, 0);      /* lsb */
    mw_put_be32(q + 60, 64 * 64 * 4); /* source bytes */
    memcpy(q + 64, tex_diffuse, 64 * 64 * 4);
    q += 8 + up_bytes;

    mw_put_be32(q, MW_TEXTURE_PARAM); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, 1); mw_put_be32(q + 12, 0x2801); mw_put_be32(q + 16, 0x2600);
    q += 20;
    mw_put_be32(q, MW_TEXTURE_PARAM); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, 1); mw_put_be32(q + 12, 0x2800); mw_put_be32(q + 16, 0x2600);
    q += 20;

    packet_finish(up_packet, q);
    uint32_t status = mw_host_submit(c, up_packet, (uint32_t)(q - up_packet));
    assert(status == MW_STATUS_OK);

    static uint8_t draw_packet[4096];
    q = packet_init(draw_packet, 7);

    /* Clear */
    mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, 3);
    mw_put_bef(q + 12, 0.02f); mw_put_bef(q + 16, 0.03f); mw_put_bef(q + 20, 0.06f); mw_put_bef(q + 24, 1.0f);
    mw_put_bef(q + 28, 1.0f);
    q += 32;

    /* Texture stage 0 enabled */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, MW_TEXTURE_STAGE); mw_put_be32(q + 12, 4);
    mw_put_be32(q + 16, 0); mw_put_be32(q + 20, 1); mw_put_be32(q + 24, 1); mw_put_be32(q + 28, 0x1e01);
    q += 32;

    /* Texture stage 1 disabled */
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, MW_TEXTURE_STAGE); mw_put_be32(q + 12, 4);
    mw_put_be32(q + 16, 1); mw_put_be32(q + 20, 0); mw_put_be32(q + 24, 0); mw_put_be32(q + 28, 0x1e01);
    q += 32;

    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_TEST); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_FUNC); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 0x0203);
    q += 20;
    mw_put_be32(q, MW_STATE); mw_put_be32(q + 4, 12);
    mw_put_be32(q + 8, MW_DEPTH_MASK); mw_put_be32(q + 12, 1); mw_put_be32(q + 16, 1);
    q += 20;

    {
        uint32_t obj_payload = 8 + 6 * 76;
        mw_put_be32(q, MW_DRAW_OBJECT); mw_put_be32(q + 4, obj_payload);
        mw_put_be32(q + 8, 4); mw_put_be32(q + 12, 6);
        q += 16;
#define EMIT_V(px, py, s, t) do { \
    mw_put_bef(q, px); mw_put_bef(q + 4, py); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f); \
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); \
    mw_put_bef(q + 32, s); mw_put_bef(q + 36, t); mw_put_bef(q + 40, 0.0f); mw_put_bef(q + 44, 1.0f); \
    mw_put_bef(q + 48, 0.0f); mw_put_bef(q + 52, 0.0f); mw_put_bef(q + 56, 0.0f); mw_put_bef(q + 60, 1.0f); \
    mw_put_bef(q + 64, 0.0f); mw_put_bef(q + 68, 0.0f); mw_put_bef(q + 72, 1.0f); \
    q += 76; \
} while(0)
        /* Keep interpolated texel coordinates off exact texel boundaries:
         * the hardware interpolator rounds boundary-adjacent coordinates
         * down, which a knife-edge 1.0 mapping turns into whole-row
         * off-by-one sampling under NEAREST filtering. */
        EMIT_V(-0.8f, -0.8f, 0.001953125f, 0.001953125f);
        EMIT_V( 0.8f, -0.8f, 0.998046875f, 0.001953125f);
        EMIT_V( 0.8f,  0.8f, 0.998046875f, 0.998046875f);
        EMIT_V(-0.8f, -0.8f, 0.001953125f, 0.001953125f);
        EMIT_V( 0.8f,  0.8f, 0.998046875f, 0.998046875f);
        EMIT_V(-0.8f,  0.8f, 0.001953125f, 0.998046875f);
#undef EMIT_V
    }

    packet_finish(draw_packet, q);
    status = mw_host_submit(c, draw_packet, (uint32_t)(q - draw_packet));
    assert(status == MW_STATUS_OK);
    (void)minigl_raster_finish();
}


#define GLQUAKE_WORLD_TEXTURE 11U
#define GLQUAKE_LIGHTMAP_TEXTURE 12U
#define GLQUAKE_ALIAS_TEXTURE 13U
#define GLQUAKE_HUD_TEXTURE 14U

static void submit_glquake_textures(mw_ctx *c)
{
    static uint8_t packet[8192];
    uint8_t world[8U * 8U * 4U];
    uint8_t lightmap[8U * 8U * 4U];
    uint8_t alias[8U * 8U * 4U];
    uint8_t hud[8U * 8U * 4U];
    uint8_t *q;
    unsigned x;
    unsigned y;

    /* The RasterIX-rendered world uses uniform texels (see
     * submit_plain_textured); the CPU-only alias and HUD keep patterns. */
    for (y = 0U; y < 8U; ++y) {
        for (x = 0U; x < 8U; ++x) {
            size_t offset = (y * 8U + x) * 4U;
            int check = ((x >> 1U) ^ (y >> 1U)) & 1U;
            world[offset] = 66U;
            world[offset + 1U] = 99U;
            world[offset + 2U] = 165U;
            world[offset + 3U] = 255U;
            lightmap[offset] = 192U;
            lightmap[offset + 1U] = 192U;
            lightmap[offset + 2U] = 192U;
            lightmap[offset + 3U] = 255U;
            alias[offset] = check ? 230U : 80U;
            alias[offset + 1U] = check ? 80U : 190U;
            alias[offset + 2U] = 48U;
            alias[offset + 3U] = 255U;
            hud[offset] = 255U;
            hud[offset + 1U] = (uint8_t)(160U + y * 8U);
            hud[offset + 2U] = 32U;
            hud[offset + 3U] = (x == 0U || y == 0U || x == 7U || y == 7U) ? 0U : 255U;
        }
    }

    q = packet_init(packet, 12U);
    put_texture_image(&q, GLQUAKE_WORLD_TEXTURE, 8U, 8U, world);
    put_texture_filter(&q, GLQUAKE_WORLD_TEXTURE, 0x2801U, 0x2600U);
    put_texture_filter(&q, GLQUAKE_WORLD_TEXTURE, 0x2800U, 0x2600U);
    put_texture_image(&q, GLQUAKE_LIGHTMAP_TEXTURE, 8U, 8U, lightmap);
    put_texture_filter(&q, GLQUAKE_LIGHTMAP_TEXTURE, 0x2801U, 0x2600U);
    put_texture_filter(&q, GLQUAKE_LIGHTMAP_TEXTURE, 0x2800U, 0x2600U);
    put_texture_image(&q, GLQUAKE_ALIAS_TEXTURE, 8U, 8U, alias);
    put_texture_filter(&q, GLQUAKE_ALIAS_TEXTURE, 0x2801U, 0x2600U);
    put_texture_filter(&q, GLQUAKE_ALIAS_TEXTURE, 0x2800U, 0x2600U);
    put_texture_image(&q, GLQUAKE_HUD_TEXTURE, 8U, 8U, hud);
    put_texture_filter(&q, GLQUAKE_HUD_TEXTURE, 0x2801U, 0x2600U);
    put_texture_filter(&q, GLQUAKE_HUD_TEXTURE, 0x2800U, 0x2600U);
    packet_finish(packet, q);
    assert(mw_host_submit(c, packet, (uint32_t)(q - packet)) == MW_STATUS_OK);
}

static void submit_glquake_world(mw_ctx *c)
{
    static uint8_t packet[4096];
    uint8_t *q = packet_init(packet, 7U);
    uint32_t payload_bytes = 8U + 6U * 76U;

    mw_put_be32(q, MW_CLEAR);
    mw_put_be32(q + 4, 24U);
    mw_put_be32(q + 8, 3U);
    mw_put_bef(q + 12, 0.0f);
    mw_put_bef(q + 16, 0.0f);
    mw_put_bef(q + 20, 0.0f);
    mw_put_bef(q + 24, 1.0f);
    mw_put_bef(q + 28, 1.0f);
    q += 32;

    put_texture_stage(&q, 0U, GLQUAKE_WORLD_TEXTURE, 1U, 0x1e01U);
    put_texture_stage(&q, 1U, GLQUAKE_LIGHTMAP_TEXTURE, 1U, 0x2100U);
    put_state_u32(&q, MW_DEPTH_TEST, 1U);
    put_state_u32(&q, MW_DEPTH_FUNC, 0x0203U);
    put_state_u32(&q, MW_DEPTH_MASK, 1U);

    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, payload_bytes);
    mw_put_be32(q + 8, 4U);
    mw_put_be32(q + 12, 6U);
    q += 16;
    put_vertex(&q, -0.95f, -0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.0625f, 0.0625f, 0.0625f, 0.0625f);
    put_vertex(&q, 0.95f, -0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.9375f, 0.0625f, 0.9375f, 0.0625f);
    put_vertex(&q, 0.95f, 0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.9375f, 0.9375f, 0.9375f, 0.9375f);
    put_vertex(&q, -0.95f, -0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.0625f, 0.0625f, 0.0625f, 0.0625f);
    put_vertex(&q, 0.95f, 0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.9375f, 0.9375f, 0.9375f, 0.9375f);
    put_vertex(&q, -0.95f, 0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.0625f, 0.9375f, 0.0625f, 0.9375f);

    packet_finish(packet, q);
    assert(mw_host_submit(c, packet, (uint32_t)(q - packet)) == MW_STATUS_OK);
}

static void submit_glquake_alias(mw_ctx *c)
{
    static uint8_t packet[4096];
    uint8_t *q = packet_init(packet, 9U);
    uint32_t payload_bytes = 8U + 12U * 76U;

    put_texture_stage(&q, 1U, 0U, 0U, 0x1e01U);
    put_texture_stage(&q, 0U, GLQUAKE_ALIAS_TEXTURE, 1U, 0x2100U);
    /* Polygon offset has no exact RasterIX equivalent; this draw remains CPU. */
    put_state_u32(&q, MW_DEPTH_TEST, 1U);
    put_state_u32(&q, MW_DEPTH_FUNC, 0x0203U);
    put_state_u32(&q, MW_DEPTH_MASK, 1U);
    put_state_u32(&q, MW_BLEND, 0U);
    put_state_float2(&q, MW_POLYGON_OFFSET, 1.0f, 1.0f);
    put_state_u32(&q, MW_OFFSET_FILL, 1U);

    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, payload_bytes);
    mw_put_be32(q + 8, 4U);
    mw_put_be32(q + 12, 12U);
    q += 16;
#define ALIAS_VERTEX(px, py, s, t, r, g, b) \
    put_vertex(&q, px, py, -0.4f, r, g, b, 1.0f, s, t, 0.0f, 0.0f)
    ALIAS_VERTEX(0.0f, 0.55f, 0.5f, 0.0625f, 1.0f, 0.9f, 0.8f);
    ALIAS_VERTEX(-0.34f, -0.2f, 0.0625f, 0.9375f, 0.7f, 0.8f, 1.0f);
    ALIAS_VERTEX(0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f);
    ALIAS_VERTEX(-0.34f, -0.2f, 0.0625f, 0.9375f, 0.7f, 0.8f, 1.0f);
    ALIAS_VERTEX(0.0f, -0.58f, 0.5f, 0.9375f, 1.0f, 0.8f, 0.7f);
    ALIAS_VERTEX(0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f);
    ALIAS_VERTEX(0.0f, -0.58f, 0.5f, 0.9375f, 1.0f, 0.8f, 0.7f);
    ALIAS_VERTEX(0.34f, -0.2f, 0.9375f, 0.9375f, 0.8f, 1.0f, 0.7f);
    ALIAS_VERTEX(0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f);
    ALIAS_VERTEX(0.34f, -0.2f, 0.9375f, 0.9375f, 0.8f, 1.0f, 0.7f);
    ALIAS_VERTEX(0.0f, 0.55f, 0.5f, 0.0625f, 1.0f, 0.9f, 0.8f);
    ALIAS_VERTEX(0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f);
#undef ALIAS_VERTEX

    packet_finish(packet, q);
    assert(mw_host_submit(c, packet, (uint32_t)(q - packet)) == MW_STATUS_OK);
}

static void submit_glquake_particles(mw_ctx *c)
{
    static uint8_t packet[2048];
    uint8_t *q = packet_init(packet, 7U);
    uint32_t payload_bytes = 8U + 9U * 76U;

    put_state_u32(&q, MW_OFFSET_FILL, 0U);
    put_state_float2(&q, MW_POLYGON_OFFSET, 0.0f, 0.0f);
    put_texture_stage(&q, 0U, 0U, 0U, 0x1e01U);
    put_state_u32(&q, MW_DEPTH_MASK, 0U);
    put_state_u32(&q, MW_BLEND, 1U);
    put_blend_factors(&q, 0x0302U, 0x0303U);

    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, payload_bytes);
    mw_put_be32(q + 8, 4U);
    mw_put_be32(q + 12, 9U);
    q += 16;
#define PARTICLE(px, py, r, g, b) do { \
    put_vertex(&q, (px) - 0.06f, (py) - 0.05f, -0.6f, r, g, b, 0.55f, 0.0f, 0.0f, 0.0f, 0.0f); \
    put_vertex(&q, (px) + 0.06f, (py) - 0.05f, -0.6f, r, g, b, 0.55f, 0.0f, 0.0f, 0.0f, 0.0f); \
    put_vertex(&q, (px), (py) + 0.07f, -0.6f, r, g, b, 0.55f, 0.0f, 0.0f, 0.0f, 0.0f); \
} while (0)
    PARTICLE(-0.55f, 0.20f, 1.0f, 0.2f, 0.1f);
    PARTICLE(0.52f, 0.28f, 0.1f, 0.8f, 1.0f);
    PARTICLE(0.43f, -0.45f, 1.0f, 0.8f, 0.1f);
#undef PARTICLE

    packet_finish(packet, q);
    assert(mw_host_submit(c, packet, (uint32_t)(q - packet)) == MW_STATUS_OK);
}

static void submit_glquake_hud(mw_ctx *c)
{
    static uint8_t packet[2048];
    uint8_t *q = packet_init(packet, 6U);
    uint32_t payload_bytes = 8U + 6U * 76U;

    put_state_u32(&q, MW_BLEND, 0U);
    put_state_u32(&q, MW_DEPTH_TEST, 0U);
    put_texture_stage(&q, 0U, GLQUAKE_HUD_TEXTURE, 1U, 0x1e01U);
    put_state_u32(&q, MW_ALPHA_TEST, 1U);
    put_state_u32_float(&q, MW_ALPHA_FUNC, 0x0204U, 0.5f);

    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, payload_bytes);
    mw_put_be32(q + 8, 4U);
    mw_put_be32(q + 12, 6U);
    q += 16;
    put_vertex(&q, -0.90f, 0.67f, -0.9f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.0625f, 0.0625f, 0.0f, 0.0f);
    put_vertex(&q, -0.25f, 0.67f, -0.9f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.9375f, 0.0625f, 0.0f, 0.0f);
    put_vertex(&q, -0.25f, 0.90f, -0.9f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.9375f, 0.9375f, 0.0f, 0.0f);
    put_vertex(&q, -0.90f, 0.67f, -0.9f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.0625f, 0.0625f, 0.0f, 0.0f);
    put_vertex(&q, -0.25f, 0.90f, -0.9f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.9375f, 0.9375f, 0.0f, 0.0f);
    put_vertex(&q, -0.90f, 0.90f, -0.9f, 1.0f, 1.0f, 1.0f, 1.0f,
               0.0625f, 0.9375f, 0.0f, 0.0f);

    packet_finish(packet, q);
    assert(mw_host_submit(c, packet, (uint32_t)(q - packet)) == MW_STATUS_OK);
}

static void submit_glquake_like(mw_ctx *c)
{
    submit_glquake_textures(c);
    submit_glquake_world(c);
    submit_glquake_alias(c);
    submit_glquake_particles(c);
    submit_glquake_hud(c);
    (void)minigl_raster_finish();
}

static void run_scene(const char *name, mw_ctx *c)
{
    if (strcmp(name, "blended_quad") == 0) submit_blended_quad(c);
    else if (strcmp(name, "gears") == 0) submit_gears(c);
    else if (strcmp(name, "textured_lightmap") == 0) submit_textured_lightmap(c);
    else if (strcmp(name, "plain_textured") == 0) submit_plain_textured(c);
    else if (strcmp(name, "glquake_like") == 0) submit_glquake_like(c);
    else assert(!"unknown scene");
}

/* Read a replayed RasterIX frame as RGB565.  PPM row 0 is the top of the
 * image; the MiniGL target, like write_ppm(), is bottom-up. */
static void load_rgb565_ppm(const char *path, uint16_t *destination)
{
    FILE *file = fopen(path, "rb");
    unsigned width;
    unsigned height;
    unsigned maximum;
    unsigned x;
    unsigned y;

    assert(file != NULL && destination != NULL);
    assert(fscanf(file, "P6 %u %u %u", &width, &height, &maximum) == 3);
    assert(width == WIDTH && height == HEIGHT && maximum == 255U);
    assert(fgetc(file) != EOF);
    for (y = 0U; y < HEIGHT; ++y) {
        for (x = 0U; x < WIDTH; ++x) {
            int red = fgetc(file);
            int green = fgetc(file);
            int blue = fgetc(file);

            assert(red != EOF && green != EOF && blue != EOF);
            destination[(HEIGHT - 1U - y) * WIDTH + x] =
                (uint16_t)((((unsigned)red >> 3U) << 11U) |
                           (((unsigned)green >> 2U) << 5U) |
                           ((unsigned)blue >> 3U));
        }
    }
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
}

/* RasterIX's Z16 target for the world, taken from the CPU reference: the
 * testbench PPM carries colour only.  Layout matches rix_prepare_depth(). */
static void glquake_world_depth(uint16_t *destination)
{
    mw_ctx *c;
    size_t index;

    minigl_raster_set_rix_platform(NULL);
    c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL && c->state.target.depth_bits == 24U);
    submit_glquake_textures(c);
    submit_glquake_world(c);
    (void)minigl_raster_finish();
    for (index = 0U; index < WIDTH * HEIGHT; ++index) {
        const uint8_t *depth = c->depth_buffer + index * 3U;
        destination[index] = (uint16_t)(((uint32_t)depth[0] << 8U) | depth[1]);
    }
    mw_host_destroy(c);
}

/*
 * Finish a GLQuake-like frame on the real firmware path with the EF xsim
 * result standing in for the hardware: the capture model writes the replayed
 * RGB565 world into the target when the world packet is committed.  The
 * alias fallback then performs the production drain/convert hand-off before
 * the remaining CPU draws, so composite.ppm proves order and composition.
 */
static void compose_glquake(const char *directory)
{
    struct capture_model model;
    uint16_t *world = (uint16_t *)malloc(WIDTH * HEIGHT * sizeof(*world));
    uint16_t *depth = (uint16_t *)malloc(WIDTH * HEIGHT * sizeof(*depth));
    mw_ctx *c;
    char path[1024];

    assert(world != NULL && depth != NULL);
    assert(snprintf(path, sizeof(path), "%s/frame.ppm", directory) > 0);
    load_rgb565_ppm(path, world);
    glquake_world_depth(depth);
    capture_model_init(&model);
    model.replay_color = world;
    model.replay_depth = depth;
    configure_capture_platform(&model);
    c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL);
    model.replay_context = c;
    minigl_raster_reset_frame_counters();
    submit_glquake_like(c);
    assert(model.submitted_count == 1U && model.replayed == 1U);
    assert(minigl_raster_draws_hardware() == 1U);
    assert(minigl_raster_draws_software() == 3U);
    assert(minigl_raster_first_fallback_reason() ==
           ZZ9K_MGL_FALLBACK_POLYGON_OFFSET);
    assert(snprintf(path, sizeof(path), "%s/composite.ppm", directory) > 0);
    write_ppm(path);
    printf("composed scene=glquake_like hardware=1 software=3 first_fallback=%s\n",
           minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_POLYGON_OFFSET));
    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    capture_model_destroy(&model);
    free(world);
    free(depth);
}

static void capture_scene(const char *directory, const char *name)
{
    struct capture_model model;
    mw_ctx *c;
    char path[1024];
    unsigned color_relocations;
    unsigned depth_relocations;

    capture_model_init(&model);
    configure_capture_platform(&model);

    c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL);
    minigl_raster_reset_frame_counters();

    run_scene(name, c);

    assert(model.submitted_count >= 1U);
    relocate_capture_targets(&model, c, &color_relocations, &depth_relocations);
    assert(color_relocations != 0U && depth_relocations != 0U);
    assert(snprintf(path, sizeof(path), "%s/commands.hex", directory) > 0);
    write_words(path, model.submitted, model.submitted_bytes);
    assert(snprintf(path, sizeof(path), "%s/ddr.hex", directory) > 0);
    write_ddr(path, &model);
    if (strcmp(name, "glquake_like") == 0) {
        /* One accelerated world draw, then polygon offset forces the alias
         * model and everything after it onto the CPU for the frame. */
        assert(model.submitted_count == 1U);
        assert(minigl_raster_draws_hardware() == 1U);
        assert(minigl_raster_draws_software() == 3U);
        assert(minigl_raster_first_fallback_reason() ==
               ZZ9K_MGL_FALLBACK_POLYGON_OFFSET);
    }

    printf("captured scene=%s submissions=%u command_bytes=%zu targets=%u/%u\n",
           name, model.submitted_count, model.submitted_bytes,
           color_relocations, depth_relocations);

    mw_host_destroy(c);
    minigl_raster_set_rix_platform(NULL);
    capture_model_destroy(&model);
}

static void reference_scene(const char *directory, const char *name)
{
    char path[1024];

    minigl_raster_set_rix_platform(NULL);
    mw_ctx *c = mw_host_create(WIDTH, HEIGHT);
    assert(c != NULL);

    if (strcmp(name, "glquake_like") == 0) {
        /* World-only CPU reference for the EF replay, which ends at the
         * hand-off; cpu.ppm below is the complete frame. */
        submit_glquake_textures(c);
        submit_glquake_world(c);
        (void)minigl_raster_finish();
        assert(snprintf(path, sizeof(path), "%s/cpu_world.ppm", directory) > 0);
        write_ppm(path);
        mw_host_destroy(c);
        c = mw_host_create(WIDTH, HEIGHT);
        assert(c != NULL);
    }
    run_scene(name, c);

    assert(snprintf(path, sizeof(path), "%s/cpu.ppm", directory) > 0);
    write_ppm(path);

    mw_host_destroy(c);
}

int main(int argc, char **argv)
{
    static const char *const scenes[] = {
        "blended_quad", "gears", "textured_lightmap", "plain_textured",
        "glquake_like"
    };
    size_t index;

    if (argc == 3 && strcmp(argv[1], "--compose") == 0) {
        compose_glquake(argv[2]);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n"
                        "       %s --compose <glquake_like-directory>\n",
                argv[0], argv[0]);
        return 2;
    }

    build_gears();

    for (index = 0U; index < sizeof(scenes) / sizeof(scenes[0]); ++index) {
        char scene_dir[1024];
        assert(snprintf(scene_dir, sizeof(scene_dir), "%s/%s", argv[1], scenes[index]) > 0);
        (void)mkdir(scene_dir, 0777);

        capture_scene(scene_dir, scenes[index]);
        reference_scene(scene_dir, scenes[index]);
    }

    printf("real wire-v9 RasterIX capture passed successfully\n");
    return 0;
}
