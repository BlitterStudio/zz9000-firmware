/*
 * Integration and behavioral tests for mw_host API and golden image verification
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mw_parse.h"
#include "mw_ctx.h"
#include "mw_host.h"
#include "mw_types.h"
#include "sdk_palette.h"


static void test_create_destroy(void)
{
    mw_ctx *c = mw_host_create(64, 48);
    assert(c != NULL);
    mw_host_destroy(c);

    /* Invalid dimensions */
    assert(mw_host_create(0, 48) == NULL);
    assert(mw_host_create(64, 0) == NULL);
    assert(mw_host_create(5000, 48) == NULL);

    printf("PASS: test_create_destroy\n");
}

static void test_clear_and_readback(void)
{
    mw_ctx *c = mw_host_create(16, 16);
    assert(c != NULL);

    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));

    /* Assemble packet with 2 commands: MW_CLEAR and MW_PRESENT */
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 2); /* 2 commands */

    /* Command 0: MW_CLEAR (red color 1.0, 0.0, 0.0, 1.0) */
    mw_put_be32(packet + 16, MW_CLEAR);
    mw_put_be32(packet + 20, 24);
    mw_put_be32(packet + 24, 3); /* color and depth mask */
    mw_put_bef(packet + 28, 1.0f); /* R */
    mw_put_bef(packet + 32, 0.0f); /* G */
    mw_put_bef(packet + 36, 0.0f); /* B */
    mw_put_bef(packet + 40, 1.0f); /* A */
    mw_put_bef(packet + 44, 1.0f); /* Depth */

    /* Command 1: MW_PRESENT */
    mw_put_be32(packet + 48, MW_PRESENT);
    mw_put_be32(packet + 52, 0);

    uint32_t packet_bytes = 16 + (8 + 24) + (8 + 0);
    mw_put_be32(packet + 8, packet_bytes);

    uint32_t status = mw_host_submit(c, packet, packet_bytes);
    assert(status == MW_STATUS_OK);

    /* Read back presented frame */
    uint8_t out[16 * 16 * 4];
    memset(out, 0, sizeof(out));

    status = mw_host_readback_rgba8(c, out, sizeof(out));
    assert(status == MW_STATUS_OK);

    /* Every pixel should be Red: (255, 0, 0, 255) */
    for (int i = 0; i < 16 * 16; ++i) {
        assert(out[i * 4] == 255);     /* R */
        assert(out[i * 4 + 1] == 0);   /* G */
        assert(out[i * 4 + 2] == 0);   /* B */
        assert(out[i * 4 + 3] == 255); /* A */
    }

    mw_host_destroy(c);
    printf("PASS: test_clear_and_readback\n");
}

static void test_latest_readback_rows(void)
{
    mw_ctx *c = mw_host_create(2U, 3U);
    uint8_t out[8];

    assert(c != NULL);
    c->state.target.latest = 0U;
    c->color_buffers[0][0] = 0x01020304U;
    c->color_buffers[0][1] = 0x05060708U;
    c->color_buffers[0][2] = 0x11121314U;
    c->color_buffers[0][3] = 0x15161718U;
    c->color_buffers[0][4] = 0x21222324U;
    c->color_buffers[0][5] = 0x25262728U;
    assert(mw_host_readback_latest_rows(c, out, sizeof(out), 1U, 1U) ==
           MW_STATUS_OK);
    assert(out[0] == 0x11U && out[1] == 0x12U && out[2] == 0x13U &&
           out[3] == 0x14U && out[4] == 0x15U && out[5] == 0x16U &&
           out[6] == 0x17U && out[7] == 0x18U);
    assert(mw_host_readback_latest_rows(c, out, sizeof(out), 3U, 1U) ==
           MW_STATUS_INVALID);
    assert(mw_host_readback_latest_rows(c, out, sizeof(out) - 1U, 1U, 1U) ==
           MW_STATUS_CAPACITY);
    mw_host_destroy(c);
}

static void test_rejected_packet_changes_no_state(void)
{
    mw_ctx *c = mw_host_create(8, 8);
    assert(c != NULL);

    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));

    /* 1. Clear to Green (0, 255, 0, 255) and present */
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 48 + 8);
    mw_put_be32(packet + 12, 2);

    mw_put_be32(packet + 16, MW_CLEAR);
    mw_put_be32(packet + 20, 24);
    mw_put_be32(packet + 24, 3);
    mw_put_bef(packet + 28, 0.0f);
    mw_put_bef(packet + 32, 1.0f); /* Green */
    mw_put_bef(packet + 36, 0.0f);
    mw_put_bef(packet + 40, 1.0f);
    mw_put_bef(packet + 44, 1.0f);

    mw_put_be32(packet + 48, MW_PRESENT);
    mw_put_be32(packet + 52, 0);

    assert(mw_host_submit(c, packet, 56) == MW_STATUS_OK);

    /* 2. Submit a malformed packet (e.g. valid clear to Blue, followed by invalid opcode) */
    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 48 + 8);
    mw_put_be32(packet + 12, 2);

    mw_put_be32(packet + 16, MW_CLEAR);
    mw_put_be32(packet + 20, 24);
    mw_put_be32(packet + 24, 3);
    mw_put_bef(packet + 28, 0.0f);
    mw_put_bef(packet + 32, 0.0f);
    mw_put_bef(packet + 36, 1.0f); /* Blue */
    mw_put_bef(packet + 40, 1.0f);
    mw_put_bef(packet + 44, 1.0f);

    /* Corrupted second command: unknown opcode 9999 */
    mw_put_be32(packet + 48, 9999);
    mw_put_be32(packet + 52, 0);

    uint32_t status = mw_host_submit(c, packet, 56);
    assert(status == MW_STATUS_INVALID);

    /* Verify that previous presented state was NOT modified */
    uint8_t out[8 * 8 * 4];
    assert(mw_host_readback_rgba8(c, out, sizeof(out)) == MW_STATUS_OK);
    assert(out[0] == 0 && out[1] == 255 && out[2] == 0 && out[3] == 255); /* Still Green! */

    mw_host_destroy(c);
    printf("PASS: test_rejected_packet_changes_no_state\n");
}

static void test_draw_triangle_golden(void)
{
    /* Submit this existing scenario through both transport trust levels. */
    mw_ctx *c = mw_host_create(32, 32);
    mw_ctx *trusted = mw_host_create(32, 32);
    assert(c != NULL && trusted != NULL);

    uint8_t packet[1024];
    memset(packet, 0, sizeof(packet));

    /* Commands:
     * 1. MW_CLEAR (black 0,0,0,1)
     * 2. MW_DRAW_OBJECT (a white triangle covering center)
     * 3. MW_PRESENT
     */
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 3); /* 3 commands */

    uint8_t *q = packet + 16;

    /* 1. MW_CLEAR */
    mw_put_be32(q, MW_CLEAR);
    mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, 3);
    mw_put_bef(q + 12, 0.0f);
    mw_put_bef(q + 16, 0.0f);
    mw_put_bef(q + 20, 0.0f);
    mw_put_bef(q + 24, 1.0f);
    mw_put_bef(q + 28, 1.0f);
    q += 32;

    /* 2. MW_DRAW_OBJECT (3 vertices in NDC, white color) */
    uint32_t obj_payload = 8 + 3 * 76;
    mw_put_be32(q, MW_DRAW_OBJECT);
    mw_put_be32(q + 4, obj_payload);
    mw_put_be32(q + 8, 4);  /* GL_TRIANGLES */
    mw_put_be32(q + 12, 3); /* count = 3 */
    q += 16;

    /* Vert 0: top center (0, 0.8, 0, 1), white (1,1,1,1) */
    mw_put_bef(q, 0.0f); mw_put_bef(q + 4, 0.8f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f);
    q += 76;

    /* Vert 1: bottom left (-0.8, -0.8, 0, 1), white */
    mw_put_bef(q, -0.8f); mw_put_bef(q + 4, -0.8f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f);
    q += 76;

    /* Vert 2: bottom right (0.8, -0.8, 0, 1), white */
    mw_put_bef(q, 0.8f); mw_put_bef(q + 4, -0.8f); mw_put_bef(q + 8, 0.0f); mw_put_bef(q + 12, 1.0f);
    mw_put_bef(q + 16, 1.0f); mw_put_bef(q + 20, 1.0f); mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f);
    q += 76;

    /* 3. MW_PRESENT */
    mw_put_be32(q, MW_PRESENT);
    mw_put_be32(q + 4, 0);
    q += 8;

    uint32_t total_bytes = q - packet;
    mw_put_be32(packet + 8, total_bytes);

    assert(mw_host_submit(c, packet, total_bytes) == MW_STATUS_OK);

    uint8_t out[32 * 32 * 4];
    uint8_t trusted_out[sizeof(out)];
    assert(mw_host_readback_rgba8(c, out, sizeof(out)) == MW_STATUS_OK);
    assert(mw_host_submit_trusted(trusted, packet, total_bytes) == MW_STATUS_OK);
    assert(mw_host_readback_rgba8(trusted, trusted_out, sizeof(trusted_out)) ==
           MW_STATUS_OK);
    assert(memcmp(out, trusted_out, sizeof(out)) == 0);

    /* Center pixel (16, 16) is inside the triangle -> White (255, 255, 255) */
    uint32_t center_idx = (16 * 32 + 16) * 4;
    assert(out[center_idx] == 255 && out[center_idx + 1] == 255 && out[center_idx + 2] == 255);

    /* Corner pixel (0, 0) is outside the triangle -> Black (0, 0, 0) */
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0);

    mw_host_destroy(trusted);
    mw_host_destroy(c);
    printf("PASS: test_draw_triangle_golden\n");
}

static void test_requests(void)
{
    mw_ctx *c = mw_host_create(16, 16);
    assert(c != NULL);

    uint8_t req[32];
    uint8_t reply[128];
    uint32_t reply_bytes = 0;

    /* 1. MW_PLUGIN_VERSION */
    mw_put_be32(req, MW_PLUGIN_VERSION);
    assert(mw_host_request(c, req, 4, reply, sizeof(reply), &reply_bytes) == MW_STATUS_OK);
    assert(reply_bytes > 0);
    assert(strcmp((const char *)reply, "zz9000-minigl-v29-v9+unknown") == 0);

    /* 2. MW_BLIT and MW_CAPTURE with a truncated request are invalid:
     * a valid span request needs at least 24 bytes (id + 5 words + 1 rect). */
    mw_put_be32(req, MW_BLIT);
    assert(mw_host_request(c, req, 4, reply, sizeof(reply), &reply_bytes) == MW_STATUS_INVALID);
    mw_put_be32(req, MW_CAPTURE);
    assert(mw_host_request(c, req, 4, reply, sizeof(reply), &reply_bytes) == MW_STATUS_INVALID);

    /* 3. MW_READ_PIXELS: read 2x2 pixels */
    memset(req, 0, sizeof(req));
    mw_put_be32(req, MW_READ_PIXELS);
    mw_put_be32(req + 4, 0x1908); /* GL_RGBA */
    mw_put_be32(req + 8, 0);      /* x = 0 */
    mw_put_be32(req + 12, 0);     /* y = 0 */
    mw_put_be32(req + 16, 2);     /* w = 2 */
    mw_put_be32(req + 20, 2);     /* h = 2 */

    reply_bytes = 0;
    assert(mw_host_request(c, req, 24, reply, sizeof(reply), &reply_bytes) == MW_STATUS_OK);
    assert(reply_bytes == 16); /* 2x2x4 = 16 bytes */

    mw_host_destroy(c);
    printf("PASS: test_requests\n");
}

static uint32_t texture_packet(uint8_t *packet, uint32_t opcode, uint32_t payload_bytes)
{
    memset(packet, 0, 24U + payload_bytes);
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 24U + payload_bytes);
    mw_put_be32(packet + 12, 1U);
    mw_put_be32(packet + 16, opcode);
    mw_put_be32(packet + 20, payload_bytes);
    return 24U + payload_bytes;
}

static uint32_t define_texture(uint8_t *packet, uint32_t name)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_IMAGE_RAW, 56U);

    mw_put_be32(packet + 24, name);
    mw_put_be32(packet + 28, 0U);
    mw_put_be32(packet + 32, 4U);
    mw_put_be32(packet + 36, 256U);
    mw_put_be32(packet + 40, 256U);
    mw_put_be32(packet + 44, 0x1908U);
    mw_put_be32(packet + 48, 0x1401U);
    mw_put_be32(packet + 52, 4U);
    mw_put_be32(packet + 76, 0U);
    return bytes;
}

static uint32_t texture_generate_mips(uint8_t *packet, uint32_t name)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_PARAM, 12U);

    mw_put_be32(packet + 24, name);
    mw_put_be32(packet + 28, 0x8191U);
    mw_put_be32(packet + 32, 1U);
    return bytes;
}

static uint32_t texture_param(uint8_t *packet, uint32_t name,
                              uint32_t pname, uint32_t value)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_PARAM, 12U);

    mw_put_be32(packet + 24, name);
    mw_put_be32(packet + 28, pname);
    mw_put_be32(packet + 32, value);
    return bytes;
}

static uint32_t lightmap_subimage(uint8_t *packet, uint32_t name, uint8_t value)
{
    uint32_t bytes = texture_packet(packet, MW_TEXTURE_SUB_RAW, 60U + 16U * 16U * 4U);
    uint8_t *pixels = packet + 84;

    mw_put_be32(packet + 24, name);
    mw_put_be32(packet + 28, 0U);
    mw_put_be32(packet + 32, 0U);
    mw_put_be32(packet + 36, 0U);
    mw_put_be32(packet + 40, 16U);
    mw_put_be32(packet + 44, 16U);
    mw_put_be32(packet + 48, 0x1908U);
    mw_put_be32(packet + 52, 0x1401U);
    mw_put_be32(packet + 56, 4U);
    mw_put_be32(packet + 80, 16U * 16U * 4U);
    memset(pixels, value, 16U * 16U * 4U);
    return bytes;
}

static void test_wire_texture_bounds_and_indices(void)
{
    mw_ctx *c = mw_host_create(4U, 4U);
    uint8_t packet[160] = { 0 };
    uint8_t *p;
    uint8_t *src;
    uint32_t bytes;
    mw_texture *t;

    assert(c != NULL);
    assert(mw_host_submit(c, packet, define_texture(packet, 90U)) == MW_STATUS_OK);

    bytes = texture_packet(packet, MW_TEXTURE_COPY, 40U);
    p = packet + 24U;
    mw_put_be32(p, 90U);
    mw_put_be32(p + 8U, 4U);
    mw_put_be32(p + 12U, UINT32_MAX);
    mw_put_be32(p + 28U, 2U);
    mw_put_be32(p + 32U, 1U);
    assert(mw_validate_packet(packet, bytes) == MW_STATUS_OK);
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    assert(c->error == 0x0502U);
    c->error = 0U;

    bytes = texture_packet(packet, MW_TEXTURE_SUB_RAW, 68U);
    p = packet + 24U;
    mw_put_be32(p, 90U);
    mw_put_be32(p + 8U, UINT32_MAX);
    mw_put_be32(p + 16U, 2U);
    mw_put_be32(p + 20U, 1U);
    mw_put_be32(p + 24U, 0x1908U);
    mw_put_be32(p + 28U, 0x1401U);
    mw_put_be32(p + 32U, 4U);
    mw_put_be32(p + 56U, 8U);
    assert(mw_validate_packet(packet, bytes) == MW_STATUS_OK);
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    assert(c->error == 0x0502U);
    c->error = 0U;

    c->palette[1][0] = 0xffU; c->palette[1][3] = 0xffU;
    c->palette[2][1] = 0xffU; c->palette[2][3] = 0xffU;
    c->palette_width = 3U;
    bytes = texture_packet(packet, MW_TEXTURE_IMAGE_RAW, 60U);
    p = packet + 24U;
    mw_put_be32(p, 91U);
    mw_put_be32(p + 8U, 4U);
    mw_put_be32(p + 12U, 2U);
    mw_put_be32(p + 16U, 1U);
    mw_put_be32(p + 20U, 0x1900U);
    mw_put_be32(p + 24U, 0x1403U);
    mw_put_be32(p + 28U, 2U);
    mw_put_be32(p + 52U, 4U);
    p[56] = 0U; p[57] = 1U; p[58] = 0U; p[59] = 2U;
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    t = mw_texmgr_find(&c->texmgr, 91U);
    assert(t != NULL && t->pixels[0][0] == 0xff0000ffU &&
           t->pixels[0][1] == 0x00ff00ffU);

    bytes = texture_packet(packet, MW_TEXTURE_IMAGE_RAW, 64U);
    p = packet + 24U;
    mw_put_be32(p, 92U);
    mw_put_be32(p + 8U, 4U);
    mw_put_be32(p + 12U, 2U);
    mw_put_be32(p + 16U, 2U);
    mw_put_be32(p + 20U, 0x1900U);
    mw_put_be32(p + 24U, 0x1403U);
    mw_put_be32(p + 28U, 2U);
    mw_put_be32(p + 52U, 8U);
    p[56] = 0U; p[57] = 1U; p[58] = 0U; p[59] = 2U;
    p[60] = 0U; p[61] = 2U; p[62] = 0U; p[63] = 1U;
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);

    bytes = texture_packet(packet, MW_TEXTURE_SUB_RAW, 84U);
    p = packet + 24U;
    mw_put_be32(p, 92U);
    mw_put_be32(p + 16U, 2U);
    mw_put_be32(p + 20U, 2U);
    mw_put_be32(p + 24U, 0x1900U);
    mw_put_be32(p + 28U, 0x1403U);
    mw_put_be32(p + 32U, 2U);
    mw_put_be32(p + 36U, 4U);
    mw_put_be32(p + 40U, 1U);
    mw_put_be32(p + 44U, 1U);
    mw_put_be32(p + 48U, 1U);
    mw_put_be32(p + 56U, 22U);
    src = p + 60U;
    src[10] = 1U; src[11] = 0U; src[12] = 2U; src[13] = 0U;
    src[18] = 2U; src[19] = 0U; src[20] = 1U; src[21] = 0U;
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    t = mw_texmgr_find(&c->texmgr, 92U);
    assert(t != NULL && t->pixels[0][0] == 0xff0000ffU &&
           t->pixels[0][1] == 0x00ff00ffU &&
           t->pixels[0][2] == 0x00ff00ffU &&
           t->pixels[0][3] == 0xff0000ffU);

    mw_host_destroy(c);
    printf("PASS: test_wire_texture_bounds_and_indices\n");
}
static void test_copy_texture_discards_index_storage(void)
{
    mw_ctx *c = mw_host_create(4U, 4U);
    uint8_t packet[128] = { 0 };
    uint8_t *p;
    uint32_t bytes;
    mw_texture *t;

    assert(c != NULL);
    bytes = texture_packet(packet, MW_TEXTURE_IMAGE_RAW, 60U);
    p = packet + 24U;
    mw_put_be32(p, 93U);
    mw_put_be32(p + 8U, 4U);
    mw_put_be32(p + 12U, 1U);
    mw_put_be32(p + 16U, 1U);
    mw_put_be32(p + 20U, 0x1900U);
    mw_put_be32(p + 24U, 0x1401U);
    mw_put_be32(p + 28U, 1U);
    mw_put_be32(p + 52U, 1U);
    p[56] = 0U;
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);

    bytes = texture_packet(packet, MW_TEXTURE_COPY, 40U);
    p = packet + 24U;
    mw_put_be32(p, 93U);
    mw_put_be32(p + 8U, 4U);
    mw_put_be32(p + 28U, 4U);
    mw_put_be32(p + 32U, 4U);
    mw_put_be32(p + 36U, 1U);
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    t = mw_texmgr_find(&c->texmgr, 93U);

    bytes = texture_packet(packet, MW_COLOR_TABLE, 32U);
    p = packet + 24U;
    mw_put_be32(p, 0x80d0U);
    mw_put_be32(p + 12U, 1U);
    mw_put_be32(p + 16U, 0x1907U);
    mw_put_be32(p + 20U, 0x1401U);
    mw_put_be32(p + 24U, 3U);
    p[28] = 0xffU;
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    assert(t != NULL && !t->indexed && t->indices[0] == NULL &&
           t->pixels[0][15] == 0x000000ffU);
    mw_host_destroy(c);
    printf("PASS: test_copy_texture_discards_index_storage\n");
}


static void test_draw_buffer_none_preserves_color(void)
{
    mw_ctx *c = mw_host_create(8U, 8U);
    uint8_t packet[320] = { 0 };
    uint8_t *p;
    uint8_t *vertex;
    uint32_t bytes;
    uint32_t i;

    assert(c != NULL);
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4U, MW_VERSION);
    mw_put_be32(packet + 12U, 2U);
    p = packet + 16U;
    mw_put_be32(p, MW_STATE);
    mw_put_be32(p + 4U, 12U);
    mw_put_be32(p + 8U, MW_DRAW_BUFFER);
    mw_put_be32(p + 12U, 1U);
    mw_put_be32(p + 16U, 0U);
    p += 20U;
    mw_put_be32(p, MW_CLEAR);
    mw_put_be32(p + 4U, 24U);
    mw_put_be32(p + 8U, 3U);
    mw_put_bef(p + 12U, 1.0f);
    mw_put_bef(p + 24U, 0.0f);
    mw_put_bef(p + 28U, 1.0f);
    mw_put_bef(p + 32U, 1.0f);
    p += 32U;
    bytes = (uint32_t)(p - packet);
    mw_put_be32(packet + 8U, bytes);
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    for (i = 0U; i < 64U; ++i)
        assert(c->color_buffers[c->state.target.back][i] == 0x000000ffU);
    assert(c->depth_buffer[(4U * 8U + 4U) * 3U] == 0xffU);

    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4U, MW_VERSION);
    mw_put_be32(packet + 12U, 2U);
    p = packet + 16U;
    mw_put_be32(p, MW_STATE);
    mw_put_be32(p + 4U, 12U);
    mw_put_be32(p + 8U, MW_DEPTH_TEST);
    mw_put_be32(p + 12U, 1U);
    mw_put_be32(p + 16U, 1U);
    p += 20U;
    mw_put_be32(p, MW_DRAW_OBJECT);
    mw_put_be32(p + 4U, 8U + 3U * MW_OBJECT_BYTES);
    mw_put_be32(p + 8U, 4U);
    mw_put_be32(p + 12U, 3U);
    vertex = p + 16U;
    mw_put_bef(vertex, -1.0f); mw_put_bef(vertex + 4U, -1.0f);
    mw_put_bef(vertex + 8U, 0.0f); mw_put_bef(vertex + 12U, 1.0f);
    mw_put_bef(vertex + 16U, 1.0f); mw_put_bef(vertex + 20U, 1.0f);
    mw_put_bef(vertex + 24U, 1.0f); mw_put_bef(vertex + 28U, 1.0f);
    vertex += MW_OBJECT_BYTES;
    mw_put_bef(vertex, 1.0f); mw_put_bef(vertex + 4U, -1.0f);
    mw_put_bef(vertex + 8U, 0.0f); mw_put_bef(vertex + 12U, 1.0f);
    mw_put_bef(vertex + 16U, 1.0f); mw_put_bef(vertex + 20U, 1.0f);
    mw_put_bef(vertex + 24U, 1.0f); mw_put_bef(vertex + 28U, 1.0f);
    vertex += MW_OBJECT_BYTES;
    mw_put_bef(vertex, 0.0f); mw_put_bef(vertex + 4U, 1.0f);
    mw_put_bef(vertex + 8U, 0.0f); mw_put_bef(vertex + 12U, 1.0f);
    mw_put_bef(vertex + 16U, 1.0f); mw_put_bef(vertex + 20U, 1.0f);
    mw_put_bef(vertex + 24U, 1.0f); mw_put_bef(vertex + 28U, 1.0f);
    bytes = 44U + 8U + 3U * MW_OBJECT_BYTES;
    mw_put_be32(packet + 8U, bytes);
    assert(mw_host_submit(c, packet, bytes) == MW_STATUS_OK);
    for (i = 0U; i < 64U; ++i)
        assert(c->color_buffers[c->state.target.back][i] == 0x000000ffU);
    assert(c->depth_buffer[(4U * 8U + 4U) * 3U] != 0xffU);

    mw_host_destroy(c);
    printf("PASS: test_draw_buffer_none_preserves_color\n");
}

static void test_glquake_texture_soak(void)
{
    enum { texture_count = 64, frame_count = 2048 };
    uint8_t packet[1128];
    uint32_t failed_opcode = 0U;
    mw_ctx *c = mw_host_create(640U, 480U);
    uint32_t texture;

    assert(c != NULL);
    /* A GL command failure records an error but must not reject its packet. */
    uint32_t bytes = lightmap_subimage(packet, 999U, 0U);
    mw_put_be32(packet + bytes, MW_TEXTURE_PARAM);
    mw_put_be32(packet + bytes + 4U, 12U);
    mw_put_be32(packet + bytes + 8U, 998U);
    mw_put_be32(packet + bytes + 12U, 0x2801U);
    mw_put_be32(packet + bytes + 16U, 0x2703U);
    bytes += 20U;
    mw_put_be32(packet + 8U, bytes);
    mw_put_be32(packet + 12U, 2U);
    assert(mw_host_submit_report(c, packet, bytes, &failed_opcode) == MW_STATUS_OK);
    assert(failed_opcode == 0U);
    assert(mw_texmgr_find(&c->texmgr, 998U) != NULL);
    assert(c->error == 0x0502U);
    c->error = 0U;

    for (texture = 1U; texture <= texture_count; ++texture) {
        /* GLQuake sets texture state before it defines the first level. */
        assert(mw_host_submit(c, packet,
                              texture_param(packet, texture, 0x2801U,
                                            0x2703U)) == MW_STATUS_OK);
        assert(mw_host_submit(c, packet,
                              texture_generate_mips(packet, texture)) ==
               MW_STATUS_OK);
        assert(mw_host_submit(c, packet,
                              define_texture(packet, texture)) ==
               MW_STATUS_OK);
    }
    /* gl_texturemode changes every live texture after upload. */
    for (texture = 1U; texture <= texture_count; ++texture)
        assert(mw_host_submit(c, packet,
                              texture_param(packet, texture, 0x2801U,
                                            0x2701U)) == MW_STATUS_OK);
    for (texture = 0U; texture < frame_count; ++texture)
        assert(mw_host_submit(c, packet,
                              lightmap_subimage(packet, texture_count,
                                                (uint8_t)texture)) == MW_STATUS_OK);
    assert(c->error == 0U);
    assert(mw_texmgr_find(&c->texmgr, texture_count)->pixels[0] != NULL);
    assert(mw_texmgr_find(&c->texmgr, texture_count)->pixels[1] != NULL);
    mw_host_destroy(c);
    printf("PASS: test_glquake_texture_soak\n");
}

static void test_palette_snapshot(void)
{
    uint32_t palette[SDK_PALETTE_ENTRIES];
    uint32_t generation;

    sdk_palette_reset();
    sdk_palette_set(3U, 0x00112233U);
    sdk_palette_set(255U, 0x00aabbccU);
    assert(sdk_palette_snapshot(palette, &generation));
    assert(!(generation & 1U));
    assert(palette[3] == 0x00112233U);
    assert(palette[255] == 0x00aabbccU);
}
static void test_draw_packed_indexed_oracle(void)
{
    mw_ctx *c = mw_host_create(32, 32);
    assert(c != NULL);

    uint8_t packet[2048];
    uint8_t out_packed[32 * 32 * 4];
    uint8_t out_indexed[32 * 32 * 4];

    float vpos[4][3] = {
        {-0.7f, -0.7f, 0.0f},
        { 0.7f, -0.7f, 0.0f},
        { 0.7f,  0.7f, 0.0f},
        {-0.7f,  0.7f, 0.0f}
    };
    uint32_t vcol[4] = { 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff };
    float vtex[4][2] = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f}
    };
    uint16_t indices_tri[6] = { 0, 1, 2, 0, 2, 3 };
    uint16_t indices_strip[4] = { 0, 1, 3, 2 };

    /* --- Scenario 1: GL_TRIANGLES with RGBA8 Colour format --- */
    uint32_t fmt_col = (1U << MW_PACK_POSITION) | (1U << MW_PACK_COLOR) |
                       (0U << MW_PACK_TEX0) | (0U << MW_PACK_TEX1) | (0U << MW_PACK_NORMAL);
    {
        /* 1a. Expanded DRAW_PACKED */
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 3);
        uint8_t *q = packet + 16;
        mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24); mw_put_be32(q + 8, 3);
        mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f);
        mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); q += 32;
        uint32_t consts = 11, words = 4;
        uint32_t pld = 12 + 4 * (consts + 6 * words);
        mw_put_be32(q, MW_DRAW_PACKED); mw_put_be32(q + 4, pld);
        mw_put_be32(q + 8, 4); mw_put_be32(q + 12, 6); mw_put_be32(q + 16, fmt_col);
        uint8_t *cur = q + 20;
        for (int k = 0; k < 8; ++k) { mw_put_bef(cur, 0.0f); cur += 4; }
        mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 1.0f); cur += 4;
        for (int k = 0; k < 6; ++k) {
            int vi = indices_tri[k];
            mw_put_bef(cur, vpos[vi][0]); cur += 4;
            mw_put_bef(cur, vpos[vi][1]); cur += 4;
            mw_put_bef(cur, vpos[vi][2]); cur += 4;
            mw_put_be32(cur, vcol[vi]); cur += 4;
        }
        q += 8 + pld;
        mw_put_be32(q, MW_PRESENT); mw_put_be32(q + 4, 0); q += 8;
        uint32_t tot = q - packet; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_OK);
        assert(mw_host_readback_rgba8(c, out_packed, sizeof(out_packed)) == MW_STATUS_OK);
    }
    {
        /* 1b. DRAW_PACKED_INDEXED */
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 3);
        uint8_t *q = packet + 16;
        mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24); mw_put_be32(q + 8, 3);
        mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f);
        mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); q += 32;
        uint32_t consts = 11, words = 4, idx_bytes = 12;
        uint32_t pld = 16 + 4 * (consts + 4 * words) + idx_bytes;
        mw_put_be32(q, MW_DRAW_PACKED_INDEXED); mw_put_be32(q + 4, pld);
        mw_put_be32(q + 8, 4); mw_put_be32(q + 12, 4); mw_put_be32(q + 16, 6); mw_put_be32(q + 20, fmt_col);
        uint8_t *cur = q + 24;
        for (int k = 0; k < 8; ++k) { mw_put_bef(cur, 0.0f); cur += 4; }
        mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 1.0f); cur += 4;
        for (int k = 0; k < 4; ++k) {
            mw_put_bef(cur, vpos[k][0]); cur += 4;
            mw_put_bef(cur, vpos[k][1]); cur += 4;
            mw_put_bef(cur, vpos[k][2]); cur += 4;
            mw_put_be32(cur, vcol[k]); cur += 4;
        }
        for (int k = 0; k < 6; ++k) {
            mw_put_be16(cur, indices_tri[k]); cur += 2;
        }
        q += 8 + pld;
        mw_put_be32(q, MW_PRESENT); mw_put_be32(q + 4, 0); q += 8;
        uint32_t tot = q - packet; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_OK);
        assert(mw_host_readback_rgba8(c, out_indexed, sizeof(out_indexed)) == MW_STATUS_OK);
    }
    assert(memcmp(out_packed, out_indexed, sizeof(out_packed)) == 0);

    /* --- Scenario 2: GL_TRIANGLE_STRIP --- */
    {
        /* 2a. Expanded DRAW_PACKED */
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 3);
        uint8_t *q = packet + 16;
        mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24); mw_put_be32(q + 8, 3);
        mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f);
        mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); q += 32;
        uint32_t consts = 11, words = 4;
        uint32_t pld = 12 + 4 * (consts + 4 * words);
        mw_put_be32(q, MW_DRAW_PACKED); mw_put_be32(q + 4, pld);
        mw_put_be32(q + 8, 5); /* GL_TRIANGLE_STRIP */
        mw_put_be32(q + 12, 4); mw_put_be32(q + 16, fmt_col);
        uint8_t *cur = q + 20;
        for (int k = 0; k < 8; ++k) { mw_put_bef(cur, 0.0f); cur += 4; }
        mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 1.0f); cur += 4;
        for (int k = 0; k < 4; ++k) {
            int vi = indices_strip[k];
            mw_put_bef(cur, vpos[vi][0]); cur += 4;
            mw_put_bef(cur, vpos[vi][1]); cur += 4;
            mw_put_bef(cur, vpos[vi][2]); cur += 4;
            mw_put_be32(cur, vcol[vi]); cur += 4;
        }
        q += 8 + pld;
        mw_put_be32(q, MW_PRESENT); mw_put_be32(q + 4, 0); q += 8;
        uint32_t tot = q - packet; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_OK);
        assert(mw_host_readback_rgba8(c, out_packed, sizeof(out_packed)) == MW_STATUS_OK);
    }
    {
        /* 2b. DRAW_PACKED_INDEXED */
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 3);
        uint8_t *q = packet + 16;
        mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24); mw_put_be32(q + 8, 3);
        mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f);
        mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); q += 32;
        uint32_t consts = 11, words = 4, idx_bytes = 8;
        uint32_t pld = 16 + 4 * (consts + 4 * words) + idx_bytes;
        mw_put_be32(q, MW_DRAW_PACKED_INDEXED); mw_put_be32(q + 4, pld);
        mw_put_be32(q + 8, 5); /* GL_TRIANGLE_STRIP */
        mw_put_be32(q + 12, 4); mw_put_be32(q + 16, 4); mw_put_be32(q + 20, fmt_col);
        uint8_t *cur = q + 24;
        for (int k = 0; k < 8; ++k) { mw_put_bef(cur, 0.0f); cur += 4; }
        mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 1.0f); cur += 4;
        for (int k = 0; k < 4; ++k) {
            mw_put_bef(cur, vpos[k][0]); cur += 4;
            mw_put_bef(cur, vpos[k][1]); cur += 4;
            mw_put_bef(cur, vpos[k][2]); cur += 4;
            mw_put_be32(cur, vcol[k]); cur += 4;
        }
        for (int k = 0; k < 4; ++k) {
            mw_put_be16(cur, indices_strip[k]); cur += 2;
        }
        q += 8 + pld;
        mw_put_be32(q, MW_PRESENT); mw_put_be32(q + 4, 0); q += 8;
        uint32_t tot = q - packet; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_OK);
        assert(mw_host_readback_rgba8(c, out_indexed, sizeof(out_indexed)) == MW_STATUS_OK);
    }
    assert(memcmp(out_packed, out_indexed, sizeof(out_packed)) == 0);

    /* --- Scenario 3: Tex0 (ST) + Tex1 (multitexture ST0 + ST1) format --- */
    uint32_t fmt_mtex = (1U << MW_PACK_POSITION) | (1U << MW_PACK_COLOR) |
                        (1U << MW_PACK_TEX0) | (1U << MW_PACK_TEX1) | (0U << MW_PACK_NORMAL);
    {
        /* 3a. Expanded DRAW_PACKED */
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 3);
        uint8_t *q = packet + 16;
        mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24); mw_put_be32(q + 8, 3);
        mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f);
        mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); q += 32;
        uint32_t consts = 3; /* Normal only */
        uint32_t words = 3 + 1 + 2 + 2; /* Pos(3) + Col(1) + Tex0(2) + Tex1(2) = 8 */
        uint32_t pld = 12 + 4 * (consts + 6 * words);
        mw_put_be32(q, MW_DRAW_PACKED); mw_put_be32(q + 4, pld);
        mw_put_be32(q + 8, 4); mw_put_be32(q + 12, 6); mw_put_be32(q + 16, fmt_mtex);
        uint8_t *cur = q + 20;
        mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 1.0f); cur += 4;
        for (int k = 0; k < 6; ++k) {
            int vi = indices_tri[k];
            mw_put_bef(cur, vpos[vi][0]); cur += 4;
            mw_put_bef(cur, vpos[vi][1]); cur += 4;
            mw_put_bef(cur, vpos[vi][2]); cur += 4;
            mw_put_be32(cur, vcol[vi]); cur += 4;
            mw_put_bef(cur, vtex[vi][0]); cur += 4;
            mw_put_bef(cur, vtex[vi][1]); cur += 4;
            mw_put_bef(cur, vtex[vi][1]); cur += 4; /* S1 */
            mw_put_bef(cur, vtex[vi][0]); cur += 4; /* T1 */
        }
        q += 8 + pld;
        mw_put_be32(q, MW_PRESENT); mw_put_be32(q + 4, 0); q += 8;
        uint32_t tot = q - packet; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_OK);
        assert(mw_host_readback_rgba8(c, out_packed, sizeof(out_packed)) == MW_STATUS_OK);
    }
    {
        /* 3b. DRAW_PACKED_INDEXED */
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 3);
        uint8_t *q = packet + 16;
        mw_put_be32(q, MW_CLEAR); mw_put_be32(q + 4, 24); mw_put_be32(q + 8, 3);
        mw_put_bef(q + 12, 0.0f); mw_put_bef(q + 16, 0.0f); mw_put_bef(q + 20, 0.0f);
        mw_put_bef(q + 24, 1.0f); mw_put_bef(q + 28, 1.0f); q += 32;
        uint32_t consts = 3, words = 8, idx_bytes = 12;
        uint32_t pld = 16 + 4 * (consts + 4 * words) + idx_bytes;
        mw_put_be32(q, MW_DRAW_PACKED_INDEXED); mw_put_be32(q + 4, pld);
        mw_put_be32(q + 8, 4); mw_put_be32(q + 12, 4); mw_put_be32(q + 16, 6); mw_put_be32(q + 20, fmt_mtex);
        uint8_t *cur = q + 24;
        mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 0.0f); cur += 4; mw_put_bef(cur, 1.0f); cur += 4;
        for (int k = 0; k < 4; ++k) {
            mw_put_bef(cur, vpos[k][0]); cur += 4;
            mw_put_bef(cur, vpos[k][1]); cur += 4;
            mw_put_bef(cur, vpos[k][2]); cur += 4;
            mw_put_be32(cur, vcol[k]); cur += 4;
            mw_put_bef(cur, vtex[k][0]); cur += 4;
            mw_put_bef(cur, vtex[k][1]); cur += 4;
            mw_put_bef(cur, vtex[k][1]); cur += 4;
            mw_put_bef(cur, vtex[k][0]); cur += 4;
        }
        for (int k = 0; k < 6; ++k) {
            mw_put_be16(cur, indices_tri[k]); cur += 2;
        }
        q += 8 + pld;
        mw_put_be32(q, MW_PRESENT); mw_put_be32(q + 4, 0); q += 8;
        uint32_t tot = q - packet; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_OK);
        assert(mw_host_readback_rgba8(c, out_indexed, sizeof(out_indexed)) == MW_STATUS_OK);
    }
    assert(memcmp(out_packed, out_indexed, sizeof(out_packed)) == 0);

    /* --- Scenario 4: Rejections leave framebuffer untouched --- */
    /* Save baseline readback */
    uint8_t baseline[32 * 32 * 4];
    memcpy(baseline, out_indexed, sizeof(baseline));

    /* 4a. Out-of-range index */
    {
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 1);
        uint32_t consts = 11, words = 4, idx_bytes = 12;
        uint32_t pld = 16 + 4 * (consts + 4 * words) + idx_bytes;
        mw_put_be32(packet + 16, MW_DRAW_PACKED_INDEXED); mw_put_be32(packet + 20, pld);
        mw_put_be32(packet + 24, 4); mw_put_be32(packet + 28, 4); mw_put_be32(packet + 32, 6); mw_put_be32(packet + 36, fmt_col);
        uint8_t *cur = packet + 40;
        for (int k = 0; k < 11; ++k) { mw_put_bef(cur, 0.0f); cur += 4; }
        for (uint32_t k = 0; k < 4 * words; ++k) { mw_put_bef(cur, 0.0f); cur += 4; }
        for (int k = 0; k < 5; ++k) { mw_put_be16(cur, 0); cur += 2; }
        mw_put_be16(cur, 4); /* Out-of-range index 4 >= v_count 4 */
        uint32_t tot = 24 + pld; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_INVALID);
        assert(mw_host_readback_rgba8(c, out_indexed, sizeof(out_indexed)) == MW_STATUS_OK);
        assert(memcmp(baseline, out_indexed, sizeof(baseline)) == 0);
    }

    /* 4b. Truncated payload */
    {
        memset(packet, 0, sizeof(packet));
        mw_put_be32(packet, MW_MAGIC); mw_put_be32(packet + 4, MW_VERSION); mw_put_be32(packet + 12, 1);
        uint32_t consts = 11, words = 4, idx_bytes = 12;
        uint32_t pld = 16 + 4 * (consts + 4 * words) + idx_bytes;
        mw_put_be32(packet + 16, MW_DRAW_PACKED_INDEXED); mw_put_be32(packet + 20, pld - 4);
        uint32_t tot = 24 + pld - 4; mw_put_be32(packet + 8, tot);
        assert(mw_host_submit(c, packet, tot) == MW_STATUS_INVALID);
        assert(mw_host_readback_rgba8(c, out_indexed, sizeof(out_indexed)) == MW_STATUS_OK);
        assert(memcmp(baseline, out_indexed, sizeof(baseline)) == 0);
    }

    mw_host_destroy(c);
    printf("PASS: test_draw_packed_indexed_oracle\n");
}


static void test_direct_rgb565_read_pixels(void)
{
    mw_ctx *c = mw_host_create(2U, 2U);
    uint16_t direct[4] = { 0xf800U, 0x07e0U, 0x001fU, 0xffffU };
    uint8_t request[24] = { 0 };
    uint8_t reply[16] = { 0 };
    uint32_t reply_bytes = 0U;
    uint32_t index;

    assert(c != NULL);
    index = c->state.target.back;
    c->color_direct[index] = 1U;
    c->color_rgb565[index] = 1U;
    c->direct_color_buffers[index] = direct;
    mw_put_be32(request, MW_READ_PIXELS);
    mw_put_be32(request + 4, 0x1908U);
    mw_put_be32(request + 16, 2U);
    mw_put_be32(request + 20, 2U);
    assert(mw_host_request(c, request, sizeof(request), reply, sizeof(reply),
                           &reply_bytes) == MW_STATUS_OK);
    assert(reply_bytes == sizeof(reply));
    assert(memcmp(reply, (const uint8_t[]){
        0x00U, 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0x00U, 0x00U, 0xffU, 0x00U, 0xffU, 0x00U, 0xffU
    }, sizeof(reply)) == 0);
    mw_host_destroy(c);
    printf("PASS: test_direct_rgb565_read_pixels\n");
}

static void clear_depth_half(mw_ctx *c)
{
    uint8_t packet[64];
    uint8_t *q = packet + 16;
    uint32_t total;

    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 1);
    mw_put_be32(q, MW_CLEAR);
    mw_put_be32(q + 4, 24);
    mw_put_be32(q + 8, 2); /* depth only */
    mw_put_bef(q + 28, 0.5f);
    q += 32;
    total = (uint32_t)(q - packet);
    mw_put_be32(packet + 8, total);
    assert(mw_host_submit(c, packet, total) == MW_STATUS_OK);
}

static void test_read_pixels_activates_its_context(void)
{
    mw_ctx *first = mw_host_create(2U, 2U);
    mw_ctx *second;
    uint8_t request[24] = { 0 };
    uint8_t reply[16] = { 0 };
    uint32_t reply_bytes = 0U;
    uint32_t index;

    assert(first != NULL);
    first->state.target.depth_bits = 24U;
    clear_depth_half(first);
    second = mw_host_create(2U, 2U);
    assert(second != NULL);
    second->state.target.depth_bits = 16U;
    clear_depth_half(second);

    /* The rasterizer now holds the second context's buffers and depth
     * format; a read of the first must not decode those. */
    mw_put_be32(request, MW_READ_PIXELS);
    mw_put_be32(request + 4, 0x1902U);
    mw_put_be32(request + 16, 2U);
    mw_put_be32(request + 20, 2U);
    assert(mw_host_request(first, request, sizeof(request), reply, sizeof(reply),
                           &reply_bytes) == MW_STATUS_OK);
    assert(reply_bytes == sizeof(reply));
    for (index = 0U; index < 4U; ++index) {
        float depth = mw_bef(reply + index * 4U);
        assert(depth > 0.499f && depth < 0.501f);
    }
    mw_host_destroy(second);
    mw_host_destroy(first);
    printf("PASS: test_read_pixels_activates_its_context\n");
}

int main(void)
{
    test_create_destroy();
    test_clear_and_readback();
    test_latest_readback_rows();
    test_rejected_packet_changes_no_state();
    test_draw_triangle_golden();
    test_requests();
    test_wire_texture_bounds_and_indices();
    test_copy_texture_discards_index_storage();
    test_draw_buffer_none_preserves_color();
    test_glquake_texture_soak();
    test_draw_packed_indexed_oracle();
    test_direct_rgb565_read_pixels();
    test_read_pixels_activates_its_context();
    test_palette_snapshot();
    printf("All host API tests PASSED!\n");
    return 0;
}
