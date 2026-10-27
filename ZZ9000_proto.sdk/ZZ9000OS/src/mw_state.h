/*
 * ZZ9000 MiniGL Wire Protocol v9 Context State
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_STATE_H
#define MW_STATE_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "mw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mw_target_state {
    uint32_t width;
    uint32_t height;
    uint32_t buffer_count;
    uint32_t depth_bits;
    uint32_t present_width;
    uint32_t present_height;
    uint32_t front;
    uint32_t back;
    uint32_t latest;
} mw_target_state;

typedef struct mw_stage_state {
    uint32_t name;
    uint32_t environment;
    float    env_color[4];
    uint8_t  enabled;
} mw_stage_state;

typedef struct mw_light_param {
    float   ambient[4];
    float   diffuse[4];
    float   specular[4];
    float   position[4];
    float   spot_direction[3];
    float   spot_exponent;
    float   spot_cutoff;
    float   constant;
    float   linear;
    float   quadratic;
    uint8_t enabled;
} mw_light_param;

typedef struct mw_material_param {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float emission[4];
    float shininess;
} mw_material_param;

typedef struct mw_texgen_state {
    uint8_t  s_enabled;
    uint8_t  t_enabled;
    uint32_t s_mode;
    uint32_t t_mode;
    float    obj_plane_s[4];
    float    obj_plane_t[4];
    float    eye_plane_s[4];
    float    eye_plane_t[4];
} mw_texgen_state;

typedef struct mw_state {
    mw_target_state target;

    /* Viewport and Scissor */
    int32_t  viewport_x, viewport_y;
    uint32_t viewport_w, viewport_h;
    int32_t  scissor_x, scissor_y;
    uint32_t scissor_w, scissor_h;
    uint8_t  scissor_enabled;

    /* Clear values */
    float    clear_color[4];
    float    clear_depth;

    /* Depth state */
    uint8_t  depth_test;
    uint8_t  depth_mask;
    uint32_t depth_func;
    float    depth_range_near;
    float    depth_range_far;

    /* Blend state */
    uint8_t  blend_enabled;
    uint32_t blend_src_rgb;
    uint32_t blend_dst_rgb;
    uint32_t blend_src_alpha;
    uint32_t blend_dst_alpha;
    uint32_t blend_equation;

    /* Alpha state */
    uint8_t  alpha_test;
    uint32_t alpha_func;
    float    alpha_ref;

    /* Cull & Face */
    uint8_t  cull_enabled;
    uint32_t cull_face;
    uint32_t front_face;

    /* Color mask */
    uint8_t  color_mask[4];

    /* Polygon offset */
    uint8_t  offset_fill;
    uint8_t  offset_line;
    uint8_t  offset_point;
    float    polygon_offset_factor;
    float    polygon_offset_units;

    /* Raster parameters */
    float    line_width;
    float    point_size;
    uint32_t shade_model;
    uint8_t  dither;
    uint8_t  point_smooth;
    float    z_offset;
    uint8_t  perspective;

    /* Buffers */
    uint32_t draw_buffer;
    uint32_t read_buffer;

    /* Fog */
    uint8_t  fog_enabled;
    uint32_t fog_mode;
    float    fog_density;
    float    fog_start;
    float    fog_end;
    float    fog_color[4];

    /* Lighting */
    uint8_t           lighting_enabled;
    mw_light_param    lights[8];
    float             light_model_ambient[4];
    uint8_t           light_model_local_viewer;
    uint8_t           light_model_two_side;
    mw_material_param materials[2];
    uint8_t           color_material_enabled;
    uint32_t          color_material_face;
    uint32_t          color_material_mode;
    uint8_t           normalize_enabled;

    /* Texture stages */
    mw_stage_state stages[2];

    /* Texgen */
    mw_texgen_state texgen[2];

    /* Normal matrix (3x3 column-major) */
    float normal_matrix[9];

    /* Matrices (column-major) */
    float modelview[16];
    float projection[16];
    float mvp[16];
    float texture_matrix[2][16];
    uint8_t force_cpu;
} mw_state;

static inline void mw_matrix_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mw_matrix_multiply(float out[16], const float a[16], const float b[16])
{
    int c, r, k;
    for (c = 0; c < 4; ++c) {
        for (r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (k = 0; k < 4; ++k)
                sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
    }
}
static inline void mw_state_update_normal_matrix(mw_state *s)
{
    float a[4][8];
    unsigned row, col, k, pivot;
    for (row = 0; row < 4; ++row) {
        for (col = 0; col < 8; ++col) {
            a[row][col] = (col < 4) ? s->modelview[col * 4 + row] : ((col - 4 == row) ? 1.0f : 0.0f);
        }
    }
    for (col = 0; col < 4; ++col) {
        pivot = col;
        float greatest = (float)fabs(a[col][col]);
        for (row = col + 1; row < 4; ++row) {
            float val = (float)fabs(a[row][col]);
            if (val > greatest) {
                greatest = val;
                pivot = row;
            }
        }
        if (greatest < 1e-30f) {
            memset(s->normal_matrix, 0, sizeof(s->normal_matrix));
            s->normal_matrix[0] = s->normal_matrix[4] = s->normal_matrix[8] = 1.0f;
            return;
        }
        if (pivot != col) {
            for (k = 0; k < 8; ++k) {
                float tmp = a[col][k];
                a[col][k] = a[pivot][k];
                a[pivot][k] = tmp;
            }
        }
        float divisor = a[col][col];
        for (k = 0; k < 8; ++k)
            a[col][k] /= divisor;
        for (row = 0; row < 4; ++row) {
            if (row != col) {
                float factor = a[row][col];
                for (k = 0; k < 8; ++k)
                    a[row][k] -= factor * a[col][k];
            }
        }
    }
    for (col = 0; col < 3; ++col) {
        for (row = 0; row < 3; ++row) {
            s->normal_matrix[col * 3 + row] = a[col][row + 4];
        }
    }
}


static inline void mw_state_init(mw_state *s, uint32_t width, uint32_t height)
{
    int i;
    memset(s, 0, sizeof(*s));

    s->target.width = width;
    s->target.height = height;
    s->target.buffer_count = 2;
    s->target.depth_bits = 24;
    s->target.present_width = width;
    s->target.present_height = height;
    s->target.front = 0;
    s->target.back = 1;
    s->target.latest = 0;
    s->force_cpu = 0;

    s->viewport_x = 0;
    s->viewport_y = 0;
    s->viewport_w = width;
    s->viewport_h = height;

    s->scissor_x = 0;
    s->scissor_y = 0;
    s->scissor_w = width;
    s->scissor_h = height;
    s->scissor_enabled = 0;

    s->clear_color[0] = 0.0f;
    s->clear_color[1] = 0.0f;
    s->clear_color[2] = 0.0f;
    s->clear_color[3] = 0.0f;
    s->clear_depth = 1.0f;

    s->depth_test = 0;
    s->depth_mask = 1;
    s->depth_func = 0x201; /* GL_LESS */
    s->depth_range_near = 0.0f;
    s->depth_range_far = 1.0f;

    s->blend_enabled = 0;
    s->blend_src_rgb = 1; /* GL_ONE */
    s->blend_dst_rgb = 0; /* GL_ZERO */
    s->blend_src_alpha = 1;
    s->blend_dst_alpha = 0;
    s->blend_equation = 0x8006; /* GL_FUNC_ADD */

    s->alpha_test = 0;
    s->alpha_func = 0x207; /* GL_ALWAYS */
    s->alpha_ref = 0.0f;

    s->cull_enabled = 0;
    s->cull_face = 0x405; /* GL_BACK */
    s->front_face = 0x901; /* GL_CCW */

    s->color_mask[0] = 1;
    s->color_mask[1] = 1;
    s->color_mask[2] = 1;
    s->color_mask[3] = 1;

    s->line_width = 1.0f;
    s->point_size = 1.0f;
    s->shade_model = 0x1d01; /* GL_SMOOTH */
    s->dither = 1;
    s->perspective = 1;

    s->draw_buffer = 0x405; /* GL_BACK */
    s->read_buffer = 0x404; /* GL_FRONT */

    s->fog_mode = 0x800; /* GL_EXP */
    s->fog_density = 1.0f;
    s->fog_end = 1.0f;

    for (i = 0; i < 2; ++i) {
        s->stages[i].environment = 0x2100; /* GL_MODULATE */
        mw_matrix_identity(s->texture_matrix[i]);
    }
    for (i = 0; i < 2; ++i) {
        s->texgen[i].s_mode = 0x2400; /* GL_EYE_LINEAR */
        s->texgen[i].t_mode = 0x2400;
    }
    s->normal_matrix[0] = s->normal_matrix[4] = s->normal_matrix[8] = 1.0f;

    mw_matrix_identity(s->modelview);
    mw_matrix_identity(s->projection);
    mw_matrix_identity(s->mvp);
}

static inline void mw_state_update_mvp(mw_state *s)
{
    mw_matrix_multiply(s->mvp, s->projection, s->modelview);
}

#ifdef __cplusplus
}
#endif

#endif /* MW_STATE_H */
