/*
 * RasterIX command encoder for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RIX_CMD_H
#define RIX_CMD_H

#include <stddef.h>
#include <stdint.h>

#define RIX_OP_RENDER_CONFIG    0x10000000U
#define RIX_OP_FRAMEBUFFER      0x20000000U
#define RIX_OP_TRIANGLE_STREAM  0x30000000U
#define RIX_OP_FOG_LUT_STREAM   0x40000000U
#define RIX_OP_TEXTURE_STREAM   0x50000000U

#define RIX_REG_FEATURE_ENABLE      0U
#define RIX_REG_CLEAR_COLOR         1U
#define RIX_REG_CLEAR_DEPTH         2U
#define RIX_REG_FRAGMENT_PIPELINE   3U
#define RIX_REG_STENCIL             4U
#define RIX_REG_FOG_COLOR           5U
#define RIX_REG_SCISSOR_START       6U
#define RIX_REG_SCISSOR_END         7U
#define RIX_REG_Y_OFFSET             8U
#define RIX_REG_RESOLUTION           9U
#define RIX_REG_TMU0_TEX_ENV        10U
#define RIX_REG_TMU0_TEX_ENV_COLOR  11U
#define RIX_REG_TMU0_TEXTURE        12U
#define RIX_REG_COLOR_BUFFER_ADDR   16U
#define RIX_REG_DEPTH_BUFFER_ADDR   17U
#define RIX_REG_STENCIL_BUFFER_ADDR 18U

#define RIX_FEATURE_FOG      (1U << 0)
#define RIX_FEATURE_BLEND    (1U << 1)
#define RIX_FEATURE_DEPTH    (1U << 2)
#define RIX_FEATURE_ALPHA    (1U << 3)
#define RIX_FEATURE_STENCIL  (1U << 4)
#define RIX_FEATURE_SCISSOR  (1U << 5)
#define RIX_FEATURE_TMU0     (1U << 6)
#define RIX_FEATURE_LOGIC    (1U << 8)

#define RIX_FRAMEBUFFER_COMMIT  (1U << 0)
#define RIX_FRAMEBUFFER_MEMSET  (1U << 1)
#define RIX_FRAMEBUFFER_SWAP    (1U << 2)
#define RIX_FRAMEBUFFER_READ    (1U << 3)
#define RIX_FRAMEBUFFER_COLOR   (1U << 4)
#define RIX_FRAMEBUFFER_DEPTH   (1U << 5)
#define RIX_FRAMEBUFFER_STENCIL (1U << 6)
#define RIX_FRAMEBUFFER_VSYNC   (1U << 7)

enum RixCompareFunc {
    RIX_FUNC_ALWAYS, RIX_FUNC_NEVER, RIX_FUNC_LESS, RIX_FUNC_EQUAL,
    RIX_FUNC_LEQUAL, RIX_FUNC_GREATER, RIX_FUNC_NOTEQUAL, RIX_FUNC_GEQUAL
};
enum RixBlendFactor {
    RIX_BLEND_ZERO, RIX_BLEND_ONE, RIX_BLEND_DST_COLOR, RIX_BLEND_SRC_COLOR,
    RIX_BLEND_ONE_MINUS_DST_COLOR, RIX_BLEND_ONE_MINUS_SRC_COLOR,
    RIX_BLEND_SRC_ALPHA, RIX_BLEND_ONE_MINUS_SRC_ALPHA, RIX_BLEND_DST_ALPHA,
    RIX_BLEND_ONE_MINUS_DST_ALPHA, RIX_BLEND_SRC_ALPHA_SATURATE
};
enum RixTextureFormat { RIX_TEXTURE_RGBA4444, RIX_TEXTURE_RGBA5551, RIX_TEXTURE_RGB565 };
enum RixTexCombine { RIX_TEX_REPLACE, RIX_TEX_MODULATE, RIX_TEX_ADD, RIX_TEX_ADD_SIGNED,
                     RIX_TEX_INTERPOLATE, RIX_TEX_SUBTRACT, RIX_TEX_DOT3_RGB, RIX_TEX_DOT3_RGBA };
enum RixTexSource { RIX_TEXSRC_TEXTURE, RIX_TEXSRC_CONSTANT, RIX_TEXSRC_PRIMARY_COLOR, RIX_TEXSRC_PREVIOUS };
enum RixTexOperand { RIX_TEXOP_SRC_ALPHA, RIX_TEXOP_ONE_MINUS_SRC_ALPHA,
                     RIX_TEXOP_SRC_COLOR, RIX_TEXOP_ONE_MINUS_SRC_COLOR };
enum RixTexAlphaOperand { RIX_TEXAOP_SRC_ALPHA, RIX_TEXAOP_ONE_MINUS_SRC_ALPHA };

typedef struct RixCmdWriter {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
    int failed;
} RixCmdWriter;

typedef struct RixFragmentState {
    uint8_t depth_func, alpha_func, alpha_ref, depth_mask;
    uint8_t color_mask_r, color_mask_g, color_mask_b, color_mask_a;
    uint8_t src_factor, dst_factor, logic_op;
} RixFragmentState;

typedef struct RixTexEnvState {
    uint8_t combine_rgb, combine_alpha;
    uint8_t src_rgb[3], src_alpha[3];
    uint8_t operand_rgb[3], operand_alpha[3];
    uint8_t shift_rgb, shift_alpha;
} RixTexEnvState;

typedef struct RixTextureState {
    uint8_t width_log2, height_log2, clamp_s, clamp_t;
    uint8_t mag_linear, min_linear, pixel_format;
} RixTextureState;

void rix_cmd_init(RixCmdWriter *writer, void *bytes, size_t capacity);
size_t rix_cmd_words(const RixCmdWriter *writer);
int rix_cmd_word(RixCmdWriter *writer, uint32_t word);
int rix_cmd_register(RixCmdWriter *writer, uint8_t reg, uint32_t value);
int rix_cmd_feature_enable(RixCmdWriter *writer, uint32_t flags);
int rix_cmd_clear_color(RixCmdWriter *writer, uint32_t rgba);
int rix_cmd_clear_depth(RixCmdWriter *writer, uint16_t depth);
int rix_cmd_fragment_state(RixCmdWriter *writer, const RixFragmentState *state);
int rix_cmd_fog_color(RixCmdWriter *writer, uint32_t rgba);
int rix_cmd_scissor(RixCmdWriter *writer, uint16_t start_x, uint16_t start_y,
                    uint16_t end_x, uint16_t end_y);
int rix_cmd_y_offset(RixCmdWriter *writer, uint16_t y);
int rix_cmd_resolution(RixCmdWriter *writer, uint16_t width, uint16_t height);
int rix_cmd_tex_env(RixCmdWriter *writer, uint8_t tmu, const RixTexEnvState *state);
int rix_cmd_tex_env_color(RixCmdWriter *writer, uint8_t tmu, uint32_t rgba);
int rix_cmd_texture_state(RixCmdWriter *writer, uint8_t tmu, const RixTextureState *state);
int rix_cmd_buffer_address(RixCmdWriter *writer, uint8_t reg, uint32_t address);
int rix_cmd_framebuffer(RixCmdWriter *writer, uint32_t flags, uint32_t pixels);
int rix_cmd_triangle(RixCmdWriter *writer, const uint32_t *words, size_t count);
int rix_cmd_texture_pages(RixCmdWriter *writer, uint8_t tmu, const uint32_t *pages, size_t count);
int rix_cmd_fog_lut(RixCmdWriter *writer, const float fog[33]);

#endif
