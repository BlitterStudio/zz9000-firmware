/*
 * Unit tests for Wire Protocol v9 texture manager and image decoding
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mw_types.h"
#include "mw_texture.h"

static void test_texture_image_raw_rgba8(void)
{
    mw_texture_manager tm;
    uint8_t payload[128];
    mw_texture *t;

    mw_texmgr_init(&tm);
    memset(payload, 0, sizeof(payload));

    /* 2x2 RGBA8 texture upload */
    mw_put_be32(payload, 1);      /* name = 1 */
    mw_put_be32(payload + 4, 0);  /* level = 0 */
    mw_put_be32(payload + 8, 4);  /* internal = 4 (RGBA) */
    mw_put_be32(payload + 12, 2); /* width = 2 */
    mw_put_be32(payload + 16, 2); /* height = 2 */
    mw_put_be32(payload + 20, 0x1908); /* GL_RGBA */
    mw_put_be32(payload + 24, 0x1401); /* GL_UNSIGNED_BYTE */
    mw_put_be32(payload + 28, 4); /* alignment = 4 */
    mw_put_be32(payload + 32, 0); /* row_length = 0 */
    mw_put_be32(payload + 36, 0); /* skip_rows = 0 */
    mw_put_be32(payload + 40, 0); /* skip_pixels = 0 */
    mw_put_be32(payload + 44, 0); /* swap = 0 */
    mw_put_be32(payload + 48, 0); /* lsb = 0 */
    mw_put_be32(payload + 52, 16);/* source = 16 bytes (2x2x4) */

    /* 4 pixels: red, green, blue, white */
    uint8_t *pix = payload + 56;
    pix[0] = 255; pix[1] = 0; pix[2] = 0; pix[3] = 255;     /* (0,0) Red */
    pix[4] = 0; pix[5] = 255; pix[6] = 0; pix[7] = 255;     /* (1,0) Green */
    pix[8] = 0; pix[9] = 0; pix[10] = 255; pix[11] = 255;   /* (0,1) Blue */
    pix[12] = 255; pix[13] = 255; pix[14] = 255; pix[15] = 255; /* (1,1) White */

    assert(mw_texmgr_image_raw(&tm, payload, 56 + 16, NULL, 0));

    t = mw_texmgr_find(&tm, 1);
    assert(t != NULL);
    assert(t->widths[0] == 2 && t->heights[0] == 2);
    assert(t->pixels[0] != NULL);

    /* Verify decoded 32-bit RGBA words */
    assert(t->pixels[0][0] == 0xff0000ff); /* Red */
    assert(t->pixels[0][1] == 0x00ff00ff); /* Green */
    assert(t->pixels[0][2] == 0x0000ffff); /* Blue */
    assert(t->pixels[0][3] == 0xffffffff); /* White */

    mw_texmgr_destroy(&tm);
    printf("PASS: test_texture_image_raw_rgba8\n");
}

static void test_texture_image_raw_rgb565(void)
{
    mw_texture_manager tm;
    uint8_t payload[128];
    mw_texture *t;

    mw_texmgr_init(&tm);
    memset(payload, 0, sizeof(payload));

    /* 1x1 RGB565 texture upload: pure red (0xf800) */
    mw_put_be32(payload, 2);      /* name = 2 */
    mw_put_be32(payload + 4, 0);  /* level = 0 */
    mw_put_be32(payload + 8, 3);  /* internal = 3 (RGB) */
    mw_put_be32(payload + 12, 1); /* width = 1 */
    mw_put_be32(payload + 16, 1); /* height = 1 */
    mw_put_be32(payload + 20, 0x1907); /* GL_RGB */
    mw_put_be32(payload + 24, 0x8363); /* GL_UNSIGNED_SHORT_5_6_5 */
    mw_put_be32(payload + 28, 2); /* alignment = 2 */
    mw_put_be32(payload + 52, 2); /* source = 2 bytes */

    /* 0xf800 is full red (5 bits red = 31, green = 0, blue = 0) */
    payload[56] = 0xf8;
    payload[57] = 0x00;

    assert(mw_texmgr_image_raw(&tm, payload, 56 + 4, NULL, 0));

    t = mw_texmgr_find(&tm, 2);
    assert(t != NULL);
    assert(t->widths[0] == 1 && t->heights[0] == 1);
    assert(t->pixels[0] != NULL);

    /* Decoded RGB565 red should be (255, 0, 0, 255) */
    assert(t->pixels[0][0] == 0xff0000ff);

    mw_texmgr_destroy(&tm);
    printf("PASS: test_texture_image_raw_rgb565\n");
}

static void test_texture_generate_mipmap(void)
{
    mw_texture_manager tm;
    uint8_t payload[128];
    mw_texture *t;

    mw_texmgr_init(&tm);
    memset(payload, 0, sizeof(payload));

    /* Enable GENERATE_MIPMAP on texture 3 before upload */
    mw_texmgr_param(&tm, 3, 0x8191 /* GL_GENERATE_MIPMAP */, 1, 0.0f);

    /* 2x2 RGBA8 texture upload: Red (255,0,0,255) */
    mw_put_be32(payload, 3);
    mw_put_be32(payload + 4, 0);  /* level 0 */
    mw_put_be32(payload + 8, 4);  /* RGBA */
    mw_put_be32(payload + 12, 2); /* 2x2 */
    mw_put_be32(payload + 16, 2);
    mw_put_be32(payload + 20, 0x1908);
    mw_put_be32(payload + 24, 0x1401);
    mw_put_be32(payload + 28, 4);
    mw_put_be32(payload + 52, 16);

    uint8_t *pix = payload + 56;
    int i;
    for (i = 0; i < 4; ++i) {
        pix[i * 4] = 200; pix[i * 4 + 1] = 100; pix[i * 4 + 2] = 50; pix[i * 4 + 3] = 255;
    }

    assert(mw_texmgr_image_raw(&tm, payload, 56 + 16, NULL, 0));

    t = mw_texmgr_find(&tm, 3);
    assert(t != NULL);
    assert(t->widths[0] == 2 && t->heights[0] == 2);

    /* Level 1 must have been automatically generated as 1x1 */
    assert(t->widths[1] == 1 && t->heights[1] == 1);
    assert(t->pixels[1] != NULL);
    /* Level 1 pixel should equal average of level 0 pixels */
    assert(t->pixels[1][0] == t->pixels[0][0]);

    mw_texmgr_destroy(&tm);
    printf("PASS: test_texture_generate_mipmap\n");
}

static void test_texture_sub_raw(void)
{
    mw_texture_manager tm;
    uint8_t payload[128];
    mw_texture *t;

    mw_texmgr_init(&tm);
    memset(payload, 0, sizeof(payload));

    /* 1. Storage-only allocation (source = 0) of 4x4 texture */
    mw_put_be32(payload, 4);
    mw_put_be32(payload + 4, 0);
    mw_put_be32(payload + 8, 4);
    mw_put_be32(payload + 12, 4);
    mw_put_be32(payload + 16, 4);
    mw_put_be32(payload + 20, 0x1908);
    mw_put_be32(payload + 24, 0x1401);
    mw_put_be32(payload + 28, 4);
    mw_put_be32(payload + 52, 0); /* source_bytes = 0 */

    assert(mw_texmgr_image_raw(&tm, payload, 56, NULL, 0));
    t = mw_texmgr_find(&tm, 4);
    assert(t != NULL && t->widths[0] == 4 && t->heights[0] == 4);

    /* 2. Banded upload: sub-image 2x2 at (1, 1) */
    memset(payload, 0, sizeof(payload));
    mw_put_be32(payload, 4);      /* name = 4 */
    mw_put_be32(payload + 4, 0);  /* level = 0 */
    mw_put_be32(payload + 8, 1);  /* xoffset = 1 */
    mw_put_be32(payload + 12, 1); /* yoffset = 1 */
    mw_put_be32(payload + 16, 2); /* width = 2 */
    mw_put_be32(payload + 20, 2); /* height = 2 */
    mw_put_be32(payload + 24, 0x1908);
    mw_put_be32(payload + 28, 0x1401);
    mw_put_be32(payload + 32, 4);
    mw_put_be32(payload + 56, 16);/* source = 16 */

    uint8_t *pix = payload + 60;
    for (int k = 0; k < 16; ++k) pix[k] = 255; /* White (255, 255, 255, 255) */

    assert(mw_texmgr_sub_raw(&tm, payload, 60 + 16, NULL, 0));

    /* Check that pixel (0,0) is still 0 (from storage-only alloc) */
    assert(t->pixels[0][0] == 0);
    /* Check that pixel (1,1) is now White */
    assert(t->pixels[0][1 * 4 + 1] == 0xffffffff);

    mw_texmgr_destroy(&tm);
    printf("PASS: test_texture_sub_raw\n");
}
static void test_texture_indexed_palette(void)
{
    mw_texture_manager tm;
    uint8_t payload[128];
    uint8_t palette[256][4];
    mw_texture *t;

    mw_texmgr_init(&tm);
    memset(payload, 0, sizeof(payload));
    memset(palette, 0, sizeof(palette));

    /* Palette: index 0 = Red, index 1 = Green, index 2 = Blue */
    palette[0][0] = 255; palette[0][1] = 0;   palette[0][2] = 0;   palette[0][3] = 255;
    palette[1][0] = 0;   palette[1][1] = 255; palette[1][2] = 0;   palette[1][3] = 255;
    palette[2][0] = 0;   palette[2][1] = 0;   palette[2][2] = 255; palette[2][3] = 255;

    /* 2x2 indexed texture: indices 0, 1, 2, 0 */
    mw_put_be32(payload, 5);      /* name = 5 */
    mw_put_be32(payload + 4, 0);  /* level = 0 */
    mw_put_be32(payload + 8, 4);  /* internal = RGBA */
    mw_put_be32(payload + 12, 2); /* width = 2 */
    mw_put_be32(payload + 16, 2); /* height = 2 */
    mw_put_be32(payload + 20, 0x1900); /* GL_COLOR_INDEX */
    mw_put_be32(payload + 24, 0x1401); /* GL_UNSIGNED_BYTE */
    mw_put_be32(payload + 28, 1); /* alignment = 1 */
    mw_put_be32(payload + 52, 4); /* source = 4 bytes */

    uint8_t *idx = payload + 56;
    idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 0;

    assert(mw_texmgr_image_raw(&tm, payload, 56 + 4, palette, 3));

    t = mw_texmgr_find(&tm, 5);
    assert(t != NULL && t->indexed);
    assert(t->pixels[0][0] == 0xff0000ff); /* Red */
    assert(t->pixels[0][1] == 0x00ff00ff); /* Green */
    assert(t->pixels[0][2] == 0x0000ffff); /* Blue */
    assert(t->pixels[0][3] == 0xff0000ff); /* Red */

    mw_texmgr_destroy(&tm);
    printf("PASS: test_texture_indexed_palette\n");
}


int main(void)
{
    test_texture_image_raw_rgba8();
    test_texture_image_raw_rgb565();
    test_texture_generate_mipmap();
    test_texture_sub_raw();
    test_texture_indexed_palette();
    printf("All texture tests PASSED!\n");
    return 0;
}
