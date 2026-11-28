/*
 * RasterIX triangle setup for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_setup.h"

#include <math.h>
#include <string.h>
#define RIX_EDGE_SHIFT 5
#define RIX_EDGE_HALF 16
#define RIX_EDGE_ONE 32

typedef struct RixPoint { int32_t x, y; } RixPoint;

static int32_t rix_fixed_coordinate(float value)
{
    /* This reproduces RasterIX Veci::createFromVec's cast semantics. */
    return (int32_t)(value * (float)RIX_EDGE_ONE + 0.5f);
}

static int32_t rix_edge(const RixPoint *a, const RixPoint *b, const RixPoint *c)
{
    uint32_t dx_ca = (uint32_t)c->x - (uint32_t)a->x;
    uint32_t dy_ba = (uint32_t)b->y - (uint32_t)a->y;
    uint32_t dy_ca = (uint32_t)c->y - (uint32_t)a->y;
    uint32_t dx_ba = (uint32_t)b->x - (uint32_t)a->x;

    /* RasterIX consumes the low 32 bits of this signed fixed-point cross
     * product.  Perform it unsigned so large legal coordinates cannot invoke
     * C signed-overflow UB before that target-width truncation. */
    return (int32_t)(dx_ca * dy_ba - dy_ca * dx_ba);
}

static uint32_t rix_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float rix_bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

#define RIX_C(x) ((float)(x) / 255.0f)
#define RIX_C8(x) RIX_C(x), RIX_C(x+1), RIX_C(x+2), RIX_C(x+3), RIX_C(x+4), RIX_C(x+5), RIX_C(x+6), RIX_C(x+7)
#define RIX_C64(x) RIX_C8(x), RIX_C8(x+8), RIX_C8(x+16), RIX_C8(x+24), RIX_C8(x+32), RIX_C8(x+40), RIX_C8(x+48), RIX_C8(x+56)

static const float s_rix_u8_to_float[256] = {
    RIX_C64(0), RIX_C64(64), RIX_C64(128), RIX_C64(192)
};

#undef RIX_C64
#undef RIX_C8
#undef RIX_C

static inline float rix_color_component(uint32_t rgba, unsigned int index)
{
    return s_rix_u8_to_float[(rgba >> (24U - index * 8U)) & 0xffU];
}

/* Barycentric weights of vertices 1 and 2 (and their x/y gradients), the
 * only ones the plane equations below use. */
typedef struct RixNormals {
    float n1, n2, nx1, nx2, ny1, ny2;
} RixNormals;

/* A + (B-A)*w1 + (C-A)*w2 and its x and y gradients, written as float bits.
 * The stores go straight from the float registers. */
static inline __attribute__((always_inline)) void
rix_plane(uint32_t *words, unsigned int word, unsigned int word_x,
          unsigned int word_y, float a, float b, float c, const RixNormals *n)
{
    float d1 = b - a;
    float d2 = c - a;
    float plane = a + d1 * n->n1 + d2 * n->n2;
    float plane_x = d1 * n->nx1 + d2 * n->nx2;
    float plane_y = d1 * n->ny1 + d2 * n->ny2;

    memcpy(&words[word], &plane, sizeof(uint32_t));
    memcpy(&words[word_x], &plane_x, sizeof(uint32_t));
    memcpy(&words[word_y], &plane_y, sizeof(uint32_t));
}

static inline __attribute__((always_inline)) void
rix_setup_texture_planes(RixTriangle *triangle, const RixVertex *v0,
                         const RixVertex *v1, const RixVertex *v2,
                         const RixNormals *normals)
{
    float s0 = v0->s, s1 = v1->s, s2 = v2->s;
    float t0 = v0->t, t1 = v1->t, t2 = v2->t;
    float w_length = sqrtf(v0->w * v0->w + v1->w * v1->w + v2->w * v2->w);
    float w_scale = 1.0f / (w_length > 1e-30f ? w_length : 1e-30f);
    float w0, w1, w2;
    float tex0[3], tex1[3], tex2[3];
    unsigned int i;

    /* Below +/-4 no extremum can trigger wrapping. OR is an inexpensive
     * upper bound on the unsigned magnitudes; false positives, including
     * infinities and NaNs, use the original ordered comparisons. */
    uint32_t extent = (rix_float_bits(s0) | rix_float_bits(s1) | rix_float_bits(s2) |
                       rix_float_bits(t0) | rix_float_bits(t1) | rix_float_bits(t2)) & 0x7fffffffU;
    if (extent > 0x40800000U) {
        float min_s = s0 < s1 ? (s0 < s2 ? s0 : s2) : (s1 < s2 ? s1 : s2);
        float max_s = s0 > s1 ? (s0 > s2 ? s0 : s2) : (s1 > s2 ? s1 : s2);
        float min_t = t0 < t1 ? (t0 < t2 ? t0 : t2) : (t1 < t2 ? t1 : t2);
        float max_t = t0 > t1 ? (t0 > t2 ? t0 : t2) : (t1 > t2 ? t1 : t2);

        if (isfinite(min_s) && min_s < -4.0f) {
            float offset = truncf(min_s);
            s0 -= offset; s1 -= offset; s2 -= offset;
        }
        if (isfinite(min_t) && min_t < -4.0f) {
            float offset = truncf(min_t);
            t0 -= offset; t1 -= offset; t2 -= offset;
        }
        if (isfinite(max_s) && max_s > 4.0f) {
            float offset = truncf(max_s);
            s0 -= offset; s1 -= offset; s2 -= offset;
        }
        if (isfinite(max_t) && max_t > 4.0f) {
            float offset = truncf(max_t);
            t0 -= offset; t1 -= offset; t2 -= offset;
        }
    }
    w0 = v0->w * w_scale;
    w1 = v1->w * w_scale;
    w2 = v2->w * w_scale;
    tex0[0] = s0 * w0; tex0[1] = t0 * w0; tex0[2] = v0->q * w0;
    tex1[0] = s1 * w1; tex1[1] = t1 * w1; tex1[2] = v1->q * w1;
    tex2[0] = s2 * w2; tex2[1] = t2 * w2; tex2[2] = v2->q * w2;
    for (i = 0U; i < 3U; ++i)
        rix_plane(triangle->words, RIX_WORD_TEXTURE + i, RIX_WORD_TEXTURE_X + i,
                  RIX_WORD_TEXTURE_Y + i, tex0[i], tex1[i], tex2[i], normals);
}


int rix_setup_triangle(RixTriangle *triangle, const RixVertex *v0,
                       const RixVertex *v1, const RixVertex *v2,
                       const RixScissor *scissor)
{
    RixPoint p0, p1, p2, origin;
    int32_t area, sign, start_x, start_y, end_x, end_y;
    int32_t wi[3], wx[3], wy[3];
    RixNormals normals;
    float area_inv;
    unsigned int i;

    if (triangle == NULL || v0 == NULL || v1 == NULL || v2 == NULL) {
        return 0;
    }
    p0.x = rix_fixed_coordinate(v0->x); p0.y = rix_fixed_coordinate(v0->y);
    p1.x = rix_fixed_coordinate(v1->x); p1.y = rix_fixed_coordinate(v1->y);
    p2.x = rix_fixed_coordinate(v2->x); p2.y = rix_fixed_coordinate(v2->y);
    area = rix_edge(&p0, &p1, &p2);
    sign = area <= 0 ? -1 : 1;
    area *= sign;
    if (area <= 0) {
        return 0;
    }

    start_x = p0.x < p1.x ? p0.x : p1.x;
    start_x = start_x < p2.x ? start_x : p2.x;
    start_y = p0.y < p1.y ? p0.y : p1.y;
    start_y = start_y < p2.y ? start_y : p2.y;
    end_x = p0.x > p1.x ? p0.x : p1.x;
    end_x = end_x > p2.x ? end_x : p2.x;
    end_y = p0.y > p1.y ? p0.y : p1.y;
    end_y = end_y > p2.y ? end_y : p2.y;

    start_x += RIX_EDGE_HALF; start_y += RIX_EDGE_HALF;
    end_x += RIX_EDGE_ONE + RIX_EDGE_HALF; end_y += RIX_EDGE_ONE + RIX_EDGE_HALF;
    if (scissor != NULL && scissor->enabled) {
        int32_t sx0 = (int32_t)scissor->start_x << RIX_EDGE_SHIFT;
        int32_t sy0 = (int32_t)scissor->start_y << RIX_EDGE_SHIFT;
        int32_t sx1 = (int32_t)scissor->end_x << RIX_EDGE_SHIFT;
        int32_t sy1 = (int32_t)scissor->end_y << RIX_EDGE_SHIFT;
        int32_t clipped_start_x = start_x > sx0 ? start_x : sx0;
        int32_t clipped_start_y = start_y > sy0 ? start_y : sy0;
        int32_t clipped_end_x = end_x < sx1 ? end_x : sx1;
        int32_t clipped_end_y = end_y < sy1 ? end_y : sy1;
        if (clipped_start_x >= clipped_end_x || clipped_start_y >= clipped_end_y) {
            return 0;
        }
        start_x = clipped_start_x;
        start_y = clipped_start_y;
        end_x = clipped_end_x;
        end_y = clipped_end_y;
    }

    /* Every descriptor word except the reserved word 0 is written below. */
    triangle->words[RIX_WORD_RESERVED] = 0U;
    triangle->words[RIX_WORD_BB_START] = ((uint32_t)(uint16_t)(start_y >> RIX_EDGE_SHIFT) << 16) |
                                         (uint16_t)(start_x >> RIX_EDGE_SHIFT);
    triangle->words[RIX_WORD_BB_END] = ((uint32_t)(uint16_t)(end_y >> RIX_EDGE_SHIFT) << 16) |
                                       (uint16_t)(end_x >> RIX_EDGE_SHIFT);
    /* RasterIX reloads these through unsigned descriptor fields before
     * calculating edge origins; preserve that upstream off-screen behavior. */
    origin.x = (int32_t)(uint16_t)(start_x >> RIX_EDGE_SHIFT) << RIX_EDGE_SHIFT;
    origin.y = (int32_t)(uint16_t)(start_y >> RIX_EDGE_SHIFT) << RIX_EDGE_SHIFT;
    wi[0] = rix_edge(&p1, &p2, &origin) * sign;
    wi[1] = rix_edge(&p2, &p0, &origin) * sign;
    wi[2] = rix_edge(&p0, &p1, &origin) * sign;
    wx[0] = (p2.y - p1.y) * RIX_EDGE_ONE * sign;
    wx[1] = (p0.y - p2.y) * RIX_EDGE_ONE * sign;
    wx[2] = (p1.y - p0.y) * RIX_EDGE_ONE * sign;
    wy[0] = (p1.x - p2.x) * RIX_EDGE_ONE * sign;
    wy[1] = (p2.x - p0.x) * RIX_EDGE_ONE * sign;
    wy[2] = (p0.x - p1.x) * RIX_EDGE_ONE * sign;
    area_inv = 1.0f / (float)area;
    normals.n1 = (float)wi[1] * area_inv;
    normals.n2 = (float)wi[2] * area_inv;
    normals.nx1 = (float)wx[1] * area_inv;
    normals.nx2 = (float)wx[2] * area_inv;
    normals.ny1 = (float)wy[1] * area_inv;
    normals.ny2 = (float)wy[2] * area_inv;
    for (i = 0U; i < 3U; ++i) {
        int top_left = (wx[i] > 0) || (wx[i] == 0 && wy[i] > 0);
        if (!top_left) {
            wi[i] -= 1;
        }
        triangle->words[RIX_WORD_W_INIT + i] = (uint32_t)wi[i];
        triangle->words[RIX_WORD_W_X + i] = (uint32_t)wx[i];
        triangle->words[RIX_WORD_W_Y + i] = (uint32_t)wy[i];
    }

    /* Planes from the differences to vertex 0: the barycentric weights sum
     * to one and their gradients to zero, so A*w0 + B*w1 + C*w2 collapses
     * to A + (B-A)*w1 + (C-A)*w2 -- two multiplies per output word instead
     * of three, across colour, depth and texture (the form Xanxi's
     * librixgpu 1.5 `rix_desc_delta` uses). */
    if (v0->rgba == v1->rgba && v0->rgba == v2->rgba) {
        /* Equal vertex colours: both differences are +0, so the plane value
         * is the colour itself (A + (+/-0) + (+/-0) == A for a colour
         * A >= +0) and each gradient is the sum of two signed zeros, -0
         * exactly when both weights' gradients are negative. */
        uint32_t gradient_x = (wx[1] < 0 && wx[2] < 0) ? 0x80000000U : 0U;
        uint32_t gradient_y = (wy[1] < 0 && wy[2] < 0) ? 0x80000000U : 0U;

        for (i = 0U; i < 4U; ++i) {
            float value = rix_color_component(v0->rgba, i);

            memcpy(&triangle->words[RIX_WORD_COLOR + i], &value, sizeof(uint32_t));
            triangle->words[RIX_WORD_COLOR_X + i] = gradient_x;
            triangle->words[RIX_WORD_COLOR_Y + i] = gradient_y;
        }
    } else {
        for (i = 0U; i < 4U; ++i)
            rix_plane(triangle->words, RIX_WORD_COLOR + i, RIX_WORD_COLOR_X + i,
                      RIX_WORD_COLOR_Y + i,
                      rix_color_component(v0->rgba, i), rix_color_component(v1->rgba, i),
                      rix_color_component(v2->rgba, i), &normals);
    }
    rix_plane(triangle->words, RIX_WORD_DEPTH, RIX_WORD_DEPTH_X, RIX_WORD_DEPTH_Y,
              v0->z, v1->z, v2->z, &normals);
    rix_plane(triangle->words, RIX_WORD_DEPTH + 1U, RIX_WORD_DEPTH_X + 1U,
              RIX_WORD_DEPTH_Y + 1U, v0->w, v1->w, v2->w, &normals);
    rix_setup_texture_planes(triangle, v0, v1, v2, &normals);
    return 1;
}

int rix_setup_texture(RixTriangle *triangle, const RixVertex *v0,
                      const RixVertex *v1, const RixVertex *v2)
{
    RixPoint p0, p1, p2, origin;
    int32_t area, sign, wi1, wi2, wx1, wx2, wy1, wy2;
    RixNormals normals;
    float area_inv;
    uint32_t start;

    if (triangle == NULL || v0 == NULL || v1 == NULL || v2 == NULL)
        return 0;
    p0.x = rix_fixed_coordinate(v0->x); p0.y = rix_fixed_coordinate(v0->y);
    p1.x = rix_fixed_coordinate(v1->x); p1.y = rix_fixed_coordinate(v1->y);
    p2.x = rix_fixed_coordinate(v2->x); p2.y = rix_fixed_coordinate(v2->y);
    area = rix_edge(&p0, &p1, &p2);
    sign = area <= 0 ? -1 : 1;
    area *= sign;
    if (area <= 0)
        return 0;
    start = triangle->words[RIX_WORD_BB_START];
    origin.x = (int32_t)(uint16_t)start << RIX_EDGE_SHIFT;
    origin.y = (int32_t)(uint16_t)(start >> 16U) << RIX_EDGE_SHIFT;
    wi1 = rix_edge(&p2, &p0, &origin) * sign;
    wi2 = rix_edge(&p0, &p1, &origin) * sign;
    wx1 = (p0.y - p2.y) * RIX_EDGE_ONE * sign;
    wx2 = (p1.y - p0.y) * RIX_EDGE_ONE * sign;
    wy1 = (p2.x - p0.x) * RIX_EDGE_ONE * sign;
    wy2 = (p0.x - p1.x) * RIX_EDGE_ONE * sign;
    area_inv = 1.0f / (float)area;
    normals.n1 = (float)wi1 * area_inv;
    normals.n2 = (float)wi2 * area_inv;
    normals.nx1 = (float)wx1 * area_inv;
    normals.nx2 = (float)wx2 * area_inv;
    normals.ny1 = (float)wy1 * area_inv;
    normals.ny2 = (float)wy2 * area_inv;
    rix_setup_texture_planes(triangle, v0, v1, v2, &normals);
    return 1;
}

int rix_triangle_in_band(const RixTriangle *triangle, uint16_t line_start, uint16_t line_end)
{
    uint16_t start_y, end_y;
    if (triangle == NULL) {
        return 0;
    }
    start_y = (uint16_t)(triangle->words[RIX_WORD_BB_START] >> 16);
    end_y = (uint16_t)(triangle->words[RIX_WORD_BB_END] >> 16);
    return end_y >= line_start && start_y < line_end;
}

int rix_triangle_increment(RixTriangle *triangle, uint16_t line_start, uint16_t line_end)
{
    uint16_t start_y;
    int32_t diff;
    unsigned int i;
    if (!rix_triangle_in_band(triangle, line_start, line_end)) {
        return 0;
    }
    start_y = (uint16_t)(triangle->words[RIX_WORD_BB_START] >> 16);
    if (start_y >= line_start) {
        return 1;
    }
    /* Rasterizer.v runs in LINE_MODE on ZZ9000: bounding boxes stay in
     * absolute bottom-up coordinates and the hardware starts scanning at
     * max(bbStart_y, RENDER_CONFIG Y offset) while evaluating the descriptor
     * from its initial values.  Advance the edge functions and interpolants
     * to the band start so they match the first scanned row; tbby, the
     * interpolator position, stays relative to the absolute bounding box. */
    diff = (int32_t)line_start - (int32_t)start_y;
    for (i = 0U; i < 3U; ++i) {
        triangle->words[RIX_WORD_W_INIT + i] += (uint32_t)((int32_t)triangle->words[RIX_WORD_W_Y + i] * diff);
    }
    for (i = 0U; i < 2U; ++i) {
        float value = rix_bits_float(triangle->words[RIX_WORD_DEPTH + i]);
        value += rix_bits_float(triangle->words[RIX_WORD_DEPTH_Y + i]) * (float)diff;
        triangle->words[RIX_WORD_DEPTH + i] = rix_float_bits(value);
    }
    for (i = 0U; i < 4U; ++i) {
        float value = rix_bits_float(triangle->words[RIX_WORD_COLOR + i]);
        value += rix_bits_float(triangle->words[RIX_WORD_COLOR_Y + i]) * (float)diff;
        triangle->words[RIX_WORD_COLOR + i] = rix_float_bits(value);
    }
    for (i = 0U; i < 3U; ++i) {
        float value = rix_bits_float(triangle->words[RIX_WORD_TEXTURE + i]);
        value += rix_bits_float(triangle->words[RIX_WORD_TEXTURE_Y + i]) * (float)diff;
        triangle->words[RIX_WORD_TEXTURE + i] = rix_float_bits(value);
    }
    return 1;
}
