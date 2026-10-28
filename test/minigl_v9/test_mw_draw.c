/*
 * Unit tests for Wire Protocol v9 draw decoding and primitive expansion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mw_types.h"
#include "mw_draw.h"

static void test_draw_packed_decode(void)
{
    uint8_t payload[256];
    float out[3 * MW_OBJECT_WORDS];
    uint32_t mode, count;
    uint32_t format;

    memset(payload, 0, sizeof(payload));

    /* Format: position 3D (Z present, W=1), RGBA8 color, 2D Tex0 (ST), no tex1, constant normal */
    /* position: 1 (3 components minus 2), color: 1 (RGBA8), tex0: 1 (2 floats), tex1: 0, normal: 0 */
    format = (1U << MW_PACK_POSITION) |
             (1U << MW_PACK_COLOR)    |
             (1U << MW_PACK_TEX0)     |
             (0U << MW_PACK_TEX1)     |
             (0U << MW_PACK_NORMAL);

    mw_put_be32(payload, 4);      /* GL_TRIANGLES */
    mw_put_be32(payload + 4, 3);  /* count = 3 */
    mw_put_be32(payload + 8, format);

    /* Constants: tex1 (4 floats), normal (3 floats) */
    uint8_t *q = payload + 12;
    mw_put_bef(q, 0.0f); q += 4; /* tex1 S */
    mw_put_bef(q, 0.0f); q += 4; /* tex1 T */
    mw_put_bef(q, 0.0f); q += 4; /* tex1 P */
    mw_put_bef(q, 1.0f); q += 4; /* tex1 Q */
    mw_put_bef(q, 0.0f); q += 4; /* normal X */
    mw_put_bef(q, 0.0f); q += 4; /* normal Y */
    mw_put_bef(q, 1.0f); q += 4; /* normal Z */

    /* Vertex 0: X=1.0, Y=2.0, Z=3.0, RGBA8=0xff0000ff (red), S=0.2, T=0.4 */
    mw_put_bef(q, 1.0f); q += 4;
    mw_put_bef(q, 2.0f); q += 4;
    mw_put_bef(q, 3.0f); q += 4;
    mw_put_be32(q, 0xff0000ff); q += 4;
    mw_put_bef(q, 0.2f); q += 4;
    mw_put_bef(q, 0.4f); q += 4;

    /* Vertex 1: X=4.0, Y=5.0, Z=6.0, RGBA8=0x00ff00ff (green), S=0.5, T=0.7 */
    mw_put_bef(q, 4.0f); q += 4;
    mw_put_bef(q, 5.0f); q += 4;
    mw_put_bef(q, 6.0f); q += 4;
    mw_put_be32(q, 0x00ff00ff); q += 4;
    mw_put_bef(q, 0.5f); q += 4;
    mw_put_bef(q, 0.7f); q += 4;

    /* Vertex 2: X=7.0, Y=8.0, Z=9.0, RGBA8=0x0000ffff (blue), S=0.8, T=1.0 */
    mw_put_bef(q, 7.0f); q += 4;
    mw_put_bef(q, 8.0f); q += 4;
    mw_put_bef(q, 9.0f); q += 4;
    mw_put_be32(q, 0x0000ffff); q += 4;
    mw_put_bef(q, 0.8f); q += 4;
    mw_put_bef(q, 1.0f); q += 4;

    size_t bytes = q - payload;
    assert(mw_decode_draw_packed(payload, bytes, out, &mode, &count));
    assert(mode == 4);
    assert(count == 3);

    /* Verify Vertex 0 */
    assert(fabsf(out[0] - 1.0f) < 1e-5f); /* X */
    assert(fabsf(out[1] - 2.0f) < 1e-5f); /* Y */
    assert(fabsf(out[2] - 3.0f) < 1e-5f); /* Z */
    assert(fabsf(out[3] - 1.0f) < 1e-5f); /* W = 1 */
    assert(fabsf(out[4] - 1.0f) < 1e-5f); /* R */
    assert(fabsf(out[5] - 0.0f) < 1e-5f); /* G */
    assert(fabsf(out[6] - 0.0f) < 1e-5f); /* B */
    assert(fabsf(out[7] - 1.0f) < 1e-5f); /* A */
    assert(fabsf(out[8] - 0.2f) < 1e-5f); /* S0 */
    assert(fabsf(out[9] - 0.4f) < 1e-5f); /* T0 */
    assert(fabsf(out[16] - 0.0f) < 1e-5f); /* Nx */
    assert(fabsf(out[17] - 0.0f) < 1e-5f); /* Ny */
    assert(fabsf(out[18] - 1.0f) < 1e-5f); /* Nz */

    printf("PASS: test_draw_packed_decode\n");
}

static void test_draw_array_raw_decode(void)
{
    uint8_t payload[256];
    float out[2 * MW_OBJECT_WORDS];
    uint32_t mode, count;

    memset(payload, 0, sizeof(payload));

    mw_put_be32(payload, 1);     /* GL_LINES */
    mw_put_be32(payload + 4, 2); /* count = 2 */
    mw_put_be32(payload + 8, 0); /* group */
    mw_put_be32(payload + 12, 0);/* flags */
    mw_put_be32(payload + 16, 2);/* 2 streams: pos and color */
    mw_put_be32(payload + 20, 0);/* index_type = 0 (no indices) */
    mw_put_be32(payload + 24, 0);/* index_bytes = 0 */
    mw_put_be32(payload + 28, 0);/* reserved */

    /* Stream 0: Slot 0 (pos, 3 floats = 12 bytes/vertex, total 24 bytes) */
    uint8_t *q = payload + 32;
    mw_put_be32(q, 0);      /* slot 0 */
    mw_put_be32(q + 4, 3);  /* 3 components */
    mw_put_be32(q + 8, 0x1406); /* GL_FLOAT */
    mw_put_be32(q + 12, 12);/* element bytes = 12 */
    mw_put_be32(q + 16, 24);/* total data bytes = 24 */
    q += 20;
    /* Vert 0 pos */
    mw_put_bef(q, 10.0f); mw_put_bef(q + 4, 20.0f); mw_put_bef(q + 8, 30.0f); q += 12;
    /* Vert 1 pos */
    mw_put_bef(q, 40.0f); mw_put_bef(q + 4, 50.0f); mw_put_bef(q + 8, 60.0f); q += 12;

    /* Stream 1: Slot 1 (color, 4 ubyte = 4 bytes/vertex, total 8 bytes) */
    mw_put_be32(q, 1);      /* slot 1 */
    mw_put_be32(q + 4, 4);  /* 4 components */
    mw_put_be32(q + 8, 0x1401); /* GL_UNSIGNED_BYTE */
    mw_put_be32(q + 12, 4); /* element bytes = 4 */
    mw_put_be32(q + 16, 8); /* total data bytes = 8 */
    q += 20;
    /* Vert 0 color: RGBA = 255, 0, 0, 255 */
    q[0] = 255; q[1] = 0; q[2] = 0; q[3] = 255; q += 4;
    /* Vert 1 color: RGBA = 0, 255, 0, 255 */
    q[0] = 0; q[1] = 255; q[2] = 0; q[3] = 255; q += 4;

    size_t bytes = q - payload;
    assert(mw_decode_draw_array_raw(payload, bytes, out, &mode, &count));
    assert(mode == 1);
    assert(count == 2);

    /* Verify Vert 0 */
    assert(fabsf(out[0] - 10.0f) < 1e-5f);
    assert(fabsf(out[1] - 20.0f) < 1e-5f);
    assert(fabsf(out[2] - 30.0f) < 1e-5f);
    assert(fabsf(out[4] - 1.0f) < 1e-5f); /* R = 1.0 */
    assert(fabsf(out[5] - 0.0f) < 1e-5f); /* G = 0.0 */

    /* Verify Vert 1 */
    float *v1 = out + MW_OBJECT_WORDS;
    assert(fabsf(v1[0] - 40.0f) < 1e-5f);
    assert(fabsf(v1[1] - 50.0f) < 1e-5f);
    assert(fabsf(v1[2] - 60.0f) < 1e-5f);
    assert(fabsf(v1[4] - 0.0f) < 1e-5f); /* R = 0.0 */
    assert(fabsf(v1[5] - 1.0f) < 1e-5f); /* G = 1.0 */

    printf("PASS: test_draw_array_raw_decode\n");
}

static void test_lowered_area_and_outline(void)
{
    float identity[16];
    mw_matrix_identity(identity);

    /* Triangle in NDC: (0,0), (1,0), (0,1) -> CCW */
    float a[19] = {0.0f, 0.0f, 0.0f, 1.0f};
    float b[19] = {1.0f, 0.0f, 0.0f, 1.0f};
    float c[19] = {0.0f, 1.0f, 0.0f, 1.0f};

    float area = mw_clipped_area(identity, 640.0f, 480.0f, a, b, c);
    assert(area > 0.0f); /* CCW area is positive */

    /* CW triangle */
    float area_cw = mw_clipped_area(identity, 640.0f, 480.0f, a, c, b);
    assert(area_cw < 0.0f); /* CW area is negative */

    /* Outline expansion of a triangle with all edges enabled */
    float tri[3 * MW_OBJECT_WORDS];
    float lines[6 * MW_OBJECT_WORDS];
    memcpy(tri, a, sizeof(a));
    memcpy(tri + 19, b, sizeof(b));
    memcpy(tri + 38, c, sizeof(c));

    uint8_t mask[4] = {0xff, 0xff, 0xff, 0xff};
    uint8_t selected_edges[4];
    size_t line_verts = mw_outline_lowered(4 /* GL_TRIANGLES */, tri, 3, mask, lines);
    assert(line_verts == 6); /* 3 edges * 2 verts = 6 */

    /* Edge masks are wire-order big-endian words.  On a little-endian host
     * 00 00 00 05 selects edges 0 and 2, not bits 24 and 26. */
    mw_put_be32(selected_edges, (1U << 0) | (1U << 2));
    line_verts = mw_outline_lowered(4 /* GL_TRIANGLES */, tri, 3, selected_edges, lines);
    assert(line_verts == 4);
    assert(lines[0] == a[0] && lines[1] == a[1]);
    assert(lines[MW_OBJECT_WORDS] == b[0] && lines[MW_OBJECT_WORDS + 1U] == b[1]);
    assert(lines[2U * MW_OBJECT_WORDS] == c[0] && lines[2U * MW_OBJECT_WORDS + 1U] == c[1]);
    assert(lines[3U * MW_OBJECT_WORDS] == a[0] && lines[3U * MW_OBJECT_WORDS + 1U] == a[1]);

    printf("PASS: test_lowered_area_and_outline\n");
}

int main(void)
{
    test_draw_packed_decode();
    test_draw_array_raw_decode();
    test_lowered_area_and_outline();
    printf("All draw tests PASSED!\n");
    return 0;
}
