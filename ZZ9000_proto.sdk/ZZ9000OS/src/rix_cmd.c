/*
 * RasterIX command encoder for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_cmd.h"

#include <string.h>

static int rix_cmd_has(const RixCmdWriter *writer, size_t bytes)
{
    return writer != NULL && writer->bytes != NULL && !writer->failed &&
           bytes <= writer->capacity - writer->length;
}


void rix_cmd_init(RixCmdWriter *writer, void *bytes, size_t capacity)
{
    writer->bytes = (uint8_t *)bytes;
    writer->capacity = capacity;
    writer->length = 0U;
    writer->failed = 0;
}

size_t rix_cmd_words(const RixCmdWriter *writer)
{
    return writer == NULL ? 0U : writer->length / 4U;
}

int rix_cmd_word(RixCmdWriter *writer, uint32_t word)
{
    uint8_t *p;
    if (!rix_cmd_has(writer, 4U)) {
        if (writer != NULL) {
            writer->failed = 1;
        }
        return 0;
    }
    p = writer->bytes + writer->length;
    p[0] = (uint8_t)word;
    p[1] = (uint8_t)(word >> 8);
    p[2] = (uint8_t)(word >> 16);
    p[3] = (uint8_t)(word >> 24);
    writer->length += 4U;
    return 1;
}

int rix_cmd_register(RixCmdWriter *writer, uint8_t reg, uint32_t value)
{
    if (reg >= 19U || !rix_cmd_has(writer, 8U)) {
        if (writer != NULL) {
            writer->failed = 1;
        }
        return 0;
    }
    return rix_cmd_word(writer, RIX_OP_RENDER_CONFIG | reg) && rix_cmd_word(writer, value);
}

int rix_cmd_feature_enable(RixCmdWriter *writer, uint32_t flags)
{
    return rix_cmd_register(writer, RIX_REG_FEATURE_ENABLE, flags & 0x17fU);
}

int rix_cmd_clear_color(RixCmdWriter *writer, uint32_t rgba)
{
    return rix_cmd_register(writer, RIX_REG_CLEAR_COLOR, rgba);
}

int rix_cmd_clear_depth(RixCmdWriter *writer, uint16_t depth)
{
    return rix_cmd_register(writer, RIX_REG_CLEAR_DEPTH, depth);
}

int rix_cmd_fragment_state(RixCmdWriter *writer, const RixFragmentState *state)
{
    uint32_t value;
    if (state == NULL) {
        return 0;
    }
    value = ((uint32_t)(state->depth_func & 7U)) |
            ((uint32_t)(state->alpha_func & 7U) << 3) |
            ((uint32_t)state->alpha_ref << 6) |
            ((uint32_t)(state->depth_mask & 1U) << 14) |
            ((uint32_t)(state->color_mask_a & 1U) << 15) |
            ((uint32_t)(state->color_mask_b & 1U) << 16) |
            ((uint32_t)(state->color_mask_g & 1U) << 17) |
            ((uint32_t)(state->color_mask_r & 1U) << 18) |
            ((uint32_t)(state->src_factor & 15U) << 19) |
            ((uint32_t)(state->dst_factor & 15U) << 23) |
            ((uint32_t)(state->logic_op & 15U) << 27);
    return rix_cmd_register(writer, RIX_REG_FRAGMENT_PIPELINE, value);
}

int rix_cmd_fog_color(RixCmdWriter *writer, uint32_t rgba)
{
    return rix_cmd_register(writer, RIX_REG_FOG_COLOR, rgba);
}

int rix_cmd_scissor(RixCmdWriter *writer, uint16_t start_x, uint16_t start_y,
                    uint16_t end_x, uint16_t end_y)
{
    return rix_cmd_register(writer, RIX_REG_SCISSOR_START,
                            ((uint32_t)(start_x & 0x7ffU)) | ((uint32_t)(start_y & 0x7ffU) << 16)) &&
           rix_cmd_register(writer, RIX_REG_SCISSOR_END,
                            ((uint32_t)(end_x & 0x7ffU)) | ((uint32_t)(end_y & 0x7ffU) << 16));
}

int rix_cmd_y_offset(RixCmdWriter *writer, uint16_t y)
{
    return rix_cmd_register(writer, RIX_REG_Y_OFFSET, (uint32_t)(y & 0x7ffU) << 16);
}

int rix_cmd_resolution(RixCmdWriter *writer, uint16_t width, uint16_t height)
{
    return rix_cmd_register(writer, RIX_REG_RESOLUTION,
                            ((uint32_t)(width & 0x7ffU)) | ((uint32_t)(height & 0x7ffU) << 16));
}

int rix_cmd_tex_env(RixCmdWriter *writer, uint8_t tmu, const RixTexEnvState *state)
{
    uint32_t value;
    if (tmu != 0U || state == NULL) {
        return 0;
    }
    value = ((uint32_t)(state->combine_rgb & 7U)) |
            ((uint32_t)(state->combine_alpha & 7U) << 3) |
            ((uint32_t)(state->src_rgb[0] & 3U) << 6) |
            ((uint32_t)(state->src_rgb[1] & 3U) << 8) |
            ((uint32_t)(state->src_rgb[2] & 3U) << 10) |
            ((uint32_t)(state->src_alpha[0] & 3U) << 12) |
            ((uint32_t)(state->src_alpha[1] & 3U) << 14) |
            ((uint32_t)(state->src_alpha[2] & 3U) << 16) |
            ((uint32_t)(state->operand_rgb[0] & 3U) << 18) |
            ((uint32_t)(state->operand_rgb[1] & 3U) << 20) |
            ((uint32_t)(state->operand_rgb[2] & 3U) << 22) |
            ((uint32_t)(state->operand_alpha[0] & 1U) << 24) |
            ((uint32_t)(state->operand_alpha[1] & 1U) << 25) |
            ((uint32_t)(state->operand_alpha[2] & 1U) << 26) |
            ((uint32_t)(state->shift_rgb & 3U) << 27) |
            ((uint32_t)(state->shift_alpha & 3U) << 29);
    return rix_cmd_register(writer, RIX_REG_TMU0_TEX_ENV, value);
}

int rix_cmd_tex_env_color(RixCmdWriter *writer, uint8_t tmu, uint32_t rgba)
{
    return tmu == 0U && rix_cmd_register(writer, RIX_REG_TMU0_TEX_ENV_COLOR, rgba);
}

int rix_cmd_texture_state(RixCmdWriter *writer, uint8_t tmu, const RixTextureState *state)
{
    uint32_t value;
    if (tmu != 0U || state == NULL) {
        return 0;
    }
    value = ((uint32_t)(state->width_log2 & 15U)) |
            ((uint32_t)(state->height_log2 & 15U) << 4) |
            ((uint32_t)(state->clamp_s & 1U) << 8) |
            ((uint32_t)(state->clamp_t & 1U) << 9) |
            ((uint32_t)(state->mag_linear & 1U) << 10) |
            ((uint32_t)(state->min_linear & 1U) << 11) |
            ((uint32_t)(state->pixel_format & 15U) << 12);
    return rix_cmd_register(writer, RIX_REG_TMU0_TEXTURE, value);
}

int rix_cmd_buffer_address(RixCmdWriter *writer, uint8_t reg, uint32_t address)
{
    if (reg != RIX_REG_COLOR_BUFFER_ADDR && reg != RIX_REG_DEPTH_BUFFER_ADDR &&
        reg != RIX_REG_STENCIL_BUFFER_ADDR) {
        return 0;
    }
    return rix_cmd_register(writer, reg, address);
}

int rix_cmd_framebuffer(RixCmdWriter *writer, uint32_t flags, uint32_t pixels)
{
    if (pixels > 0x000fffffU) {
        return 0;
    }
    return rix_cmd_word(writer, RIX_OP_FRAMEBUFFER | (flags & 0xffU) |
                        (pixels << 8));
}

int rix_cmd_triangle(RixCmdWriter *writer, const uint32_t *words, size_t count)
{
    size_t i;
    if (words == NULL || count == 0U || count * 4U > 0x3ffU || !rix_cmd_has(writer, (count + 1U) * 4U)) {
        return 0;
    }
    if (!rix_cmd_word(writer, RIX_OP_TRIANGLE_STREAM | (uint32_t)(count * 4U))) {
        return 0;
    }
    for (i = 0U; i < count; ++i) {
        if (!rix_cmd_word(writer, words[i])) {
            return 0;
        }
    }
    return 1;
}

int rix_cmd_texture_pages(RixCmdWriter *writer, uint8_t tmu, const uint32_t *pages, size_t count)
{
    size_t i;
    if (tmu != 0U || pages == NULL || count == 0U || count > 0x7ffffU || !rix_cmd_has(writer, (count + 1U) * 4U)) {
        return 0;
    }
    if (!rix_cmd_word(writer, RIX_OP_TEXTURE_STREAM | (uint32_t)count | ((uint32_t)tmu << 19))) {
        return 0;
    }
    for (i = 0U; i < count; ++i) {
        if (!rix_cmd_word(writer, pages[i])) {
            return 0;
        }
    }
    return 1;
}

int rix_cmd_fog_lut(RixCmdWriter *writer, const float fog[33])
{
    size_t i;
    if (fog == NULL || !rix_cmd_has(writer, (64U + 1U) * 4U) ||
        !rix_cmd_word(writer, RIX_OP_FOG_LUT_STREAM | 256U)) {
        return 0;
    }
    for (i = 0U; i < 32U; ++i) {
        float f = fog[i] < 0.0f ? 0.0f : (fog[i] > 1.0f ? 1.0f : fog[i]);
        float next = fog[i + 1U] < 0.0f ? 0.0f : (fog[i + 1U] > 1.0f ? 1.0f : fog[i + 1U]);
        int32_t m = (int32_t)((next - f) * 4194304.0f);
        int32_t b = (int32_t)(f * 1073741824.0f);
        if (!rix_cmd_word(writer, (uint32_t)m) || !rix_cmd_word(writer, (uint32_t)b)) {
            return 0;
        }
    }
    return 1;
}
