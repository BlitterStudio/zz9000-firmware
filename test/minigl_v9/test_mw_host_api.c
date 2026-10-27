/*
 * Integration and behavioral tests for mw_host API and golden image verification
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mw_host.h"
#include "mw_types.h"

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
    /* 32x32 target */
    mw_ctx *c = mw_host_create(32, 32);
    assert(c != NULL);

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
    assert(mw_host_readback_rgba8(c, out, sizeof(out)) == MW_STATUS_OK);

    /* Center pixel (16, 16) is inside the triangle -> White (255, 255, 255) */
    uint32_t center_idx = (16 * 32 + 16) * 4;
    assert(out[center_idx] == 255 && out[center_idx + 1] == 255 && out[center_idx + 2] == 255);

    /* Corner pixel (0, 0) is outside the triangle -> Black (0, 0, 0) */
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0);

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
    assert(strcmp((const char *)reply, "zz9000-minigl-v29-v9") == 0);

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

int main(void)
{
    test_create_destroy();
    test_clear_and_readback();
    test_rejected_packet_changes_no_state();
    test_draw_triangle_golden();
    test_requests();
    printf("All host API tests PASSED!\n");
    return 0;
}
