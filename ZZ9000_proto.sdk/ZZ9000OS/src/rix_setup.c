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

static float rix_color_component(uint32_t rgba, unsigned int index)
{
    return (float)((rgba >> (24U - index * 8U)) & 0xffU) / 255.0f;
}

static void rix_store_vec(RixTriangle *triangle, unsigned int start, const float *value, unsigned int count)
{
    unsigned int i;
    for (i = 0U; i < count; ++i) {
        triangle->words[start + i] = rix_float_bits(value[i]);
    }
}

static void rix_interpolate3(float *out, const float *a, const float *b, const float *c,
                             float w0, float w1, float w2, unsigned int count)
{
    unsigned int i;
    for (i = 0U; i < count; ++i) {
        out[i] = a[i] * w0 + b[i] * w1 + c[i] * w2;
    }
}

int rix_setup_triangle(RixTriangle *triangle, const RixVertex *v0,
                       const RixVertex *v1, const RixVertex *v2,
                       const RixScissor *scissor)
{
    RixPoint p0, p1, p2, origin;
    int32_t area, sign, start_x, start_y, end_x, end_y;
    int32_t wi[3], wx[3], wy[3];
    float normalized[3], normalized_x[3], normalized_y[3], area_inv;
    float c0[4], c1[4], c2[4], depth0[2], depth1[2], depth2[2];
    float tex0[3], tex1[3], tex2[3], result[4];
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

    memset(triangle, 0, sizeof(*triangle));
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
    for (i = 0U; i < 3U; ++i) {
        int top_left = (wx[i] > 0) || (wx[i] == 0 && wy[i] > 0);
        normalized[i] = (float)wi[i] * area_inv;
        normalized_x[i] = (float)wx[i] * area_inv;
        normalized_y[i] = (float)wy[i] * area_inv;
        if (!top_left) {
            wi[i] -= 1;
        }
        triangle->words[RIX_WORD_W_INIT + i] = (uint32_t)wi[i];
        triangle->words[RIX_WORD_W_X + i] = (uint32_t)wx[i];
        triangle->words[RIX_WORD_W_Y + i] = (uint32_t)wy[i];
    }
    for (i = 0U; i < 4U; ++i) {
        c0[i] = rix_color_component(v0->rgba, i);
        c1[i] = rix_color_component(v1->rgba, i);
        c2[i] = rix_color_component(v2->rgba, i);
    }
    rix_interpolate3(result, c0, c1, c2, normalized[0], normalized[1], normalized[2], 4U);
    rix_store_vec(triangle, RIX_WORD_COLOR, result, 4U);
    rix_interpolate3(result, c0, c1, c2, normalized_x[0], normalized_x[1], normalized_x[2], 4U);
    rix_store_vec(triangle, RIX_WORD_COLOR_X, result, 4U);
    rix_interpolate3(result, c0, c1, c2, normalized_y[0], normalized_y[1], normalized_y[2], 4U);
    rix_store_vec(triangle, RIX_WORD_COLOR_Y, result, 4U);

    depth0[0] = v0->z; depth0[1] = v0->w;
    depth1[0] = v1->z; depth1[1] = v1->w;
    depth2[0] = v2->z; depth2[1] = v2->w;
    rix_interpolate3(result, depth0, depth1, depth2, normalized[0], normalized[1], normalized[2], 2U);
    rix_store_vec(triangle, RIX_WORD_DEPTH, result, 2U);
    rix_interpolate3(result, depth0, depth1, depth2, normalized_x[0], normalized_x[1], normalized_x[2], 2U);
    rix_store_vec(triangle, RIX_WORD_DEPTH_X, result, 2U);
    rix_interpolate3(result, depth0, depth1, depth2, normalized_y[0], normalized_y[1], normalized_y[2], 2U);
    rix_store_vec(triangle, RIX_WORD_DEPTH_Y, result, 2U);

    {
        float s0 = v0->s, s1 = v1->s, s2 = v2->s;
        float t0 = v0->t, t1 = v1->t, t2 = v2->t;
        float min_s = s0 < s1 ? (s0 < s2 ? s0 : s2) : (s1 < s2 ? s1 : s2);
        float max_s = s0 > s1 ? (s0 > s2 ? s0 : s2) : (s1 > s2 ? s1 : s2);
        float min_t = t0 < t1 ? (t0 < t2 ? t0 : t2) : (t1 < t2 ? t1 : t2);
        float max_t = t0 > t1 ? (t0 > t2 ? t0 : t2) : (t1 > t2 ? t1 : t2);
        float w_length = sqrtf(v0->w * v0->w + v1->w * v1->w + v2->w * v2->w);
        float w_scale = 1.0f / (w_length > 1e-30f ? w_length : 1e-30f);
        if (min_s < -4.0f) {
            float offset = (float)(int32_t)min_s;
            s0 -= offset; s1 -= offset; s2 -= offset;
        }
        if (min_t < -4.0f) {
            float offset = (float)(int32_t)min_t;
            t0 -= offset; t1 -= offset; t2 -= offset;
        }
        if (max_s > 4.0f) {
            float offset = (float)(int32_t)max_s;
            s0 -= offset; s1 -= offset; s2 -= offset;
        }
        if (max_t > 4.0f) {
            float offset = (float)(int32_t)max_t;
            t0 -= offset; t1 -= offset; t2 -= offset;
        }
        {
            float w0 = v0->w * w_scale;
            float w1 = v1->w * w_scale;
            float w2 = v2->w * w_scale;
            tex0[0] = s0 * w0; tex0[1] = t0 * w0; tex0[2] = v0->q * w0;
            tex1[0] = s1 * w1; tex1[1] = t1 * w1; tex1[2] = v1->q * w1;
            tex2[0] = s2 * w2; tex2[1] = t2 * w2; tex2[2] = v2->q * w2;
        }
    }
    rix_interpolate3(result, tex0, tex1, tex2, normalized[0], normalized[1], normalized[2], 3U);
    rix_store_vec(triangle, RIX_WORD_TEXTURE, result, 3U);
    rix_interpolate3(result, tex0, tex1, tex2, normalized_x[0], normalized_x[1], normalized_x[2], 3U);
    rix_store_vec(triangle, RIX_WORD_TEXTURE_X, result, 3U);
    rix_interpolate3(result, tex0, tex1, tex2, normalized_y[0], normalized_y[1], normalized_y[2], 3U);
    rix_store_vec(triangle, RIX_WORD_TEXTURE_Y, result, 3U);
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
