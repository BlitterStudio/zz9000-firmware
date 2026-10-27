/*
 * RasterIX triangle setup for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RIX_SETUP_H
#define RIX_SETUP_H

#include <stdint.h>

#define RIX_TRIANGLE_WORDS 39U

/* 32-bit word indices inside a triangle descriptor. */
enum {
    RIX_WORD_RESERVED = 0,
    RIX_WORD_BB_START = 1,
    RIX_WORD_BB_END = 2,
    RIX_WORD_W_INIT = 3,
    RIX_WORD_W_X = 6,
    RIX_WORD_W_Y = 9,
    RIX_WORD_COLOR = 12,
    RIX_WORD_COLOR_X = 16,
    RIX_WORD_COLOR_Y = 20,
    RIX_WORD_DEPTH = 24,
    RIX_WORD_DEPTH_X = 26,
    RIX_WORD_DEPTH_Y = 28,
    RIX_WORD_TEXTURE = 30,
    RIX_WORD_TEXTURE_X = 33,
    RIX_WORD_TEXTURE_Y = 36
};

/* x/y/z are screen-space post-viewport values; w is reciprocal clip-w.
 * rgba is 0xRRGGBBAA and s/t/q are post-clip texture coordinates. */
typedef struct RixVertex {
    float x, y, z, w;
    uint32_t rgba;
    float s, t, q;
} RixVertex;

typedef struct RixScissor {
    uint16_t start_x, start_y, end_x, end_y;
    uint8_t enabled;
} RixScissor;

typedef struct RixTriangle {
    uint32_t words[RIX_TRIANGLE_WORDS];
} RixTriangle;

int rix_setup_triangle(RixTriangle *triangle, const RixVertex *v0,
                       const RixVertex *v1, const RixVertex *v2,
                       const RixScissor *scissor);
int rix_triangle_increment(RixTriangle *triangle, uint16_t line_start,
                           uint16_t line_end);
int rix_triangle_in_band(const RixTriangle *triangle, uint16_t line_start,
                         uint16_t line_end);

#endif
