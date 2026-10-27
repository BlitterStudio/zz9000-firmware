/*
 * Unit tests for Wire Protocol v9 packet parsing and validation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mw_types.h"
#include "mw_parse.h"

static void test_packet_framing(void)
{
    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));

    /* Null or too short */
    assert(mw_validate_packet(NULL, 16) == MW_STATUS_INVALID);
    assert(mw_validate_packet(packet, 12) == MW_STATUS_INVALID);

    /* Valid header framing with 1 command (MW_PRESENT, 0 bytes) */
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, 24); /* 16 byte hdr + 8 byte cmd */
    mw_put_be32(packet + 12, 1); /* 1 command */

    /* Command 0: MW_PRESENT */
    mw_put_be32(packet + 16, MW_PRESENT);
    mw_put_be32(packet + 20, 0); /* 0 payload bytes */

    assert(mw_validate_packet(packet, 24) == MW_STATUS_OK);

    /* Corrupted magic */
    mw_put_be32(packet, 0x12345678);
    assert(mw_validate_packet(packet, 24) == MW_STATUS_INVALID);
    mw_put_be32(packet, MW_MAGIC);

    /* Version mismatch */
    mw_put_be32(packet + 4, 8);
    assert(mw_validate_packet(packet, 24) == MW_STATUS_INVALID);
    mw_put_be32(packet + 4, MW_VERSION);

    /* Length field mismatch */
    mw_put_be32(packet + 8, 28);
    assert(mw_validate_packet(packet, 24) == MW_STATUS_INVALID);
    mw_put_be32(packet + 8, 24);

    /* Count 0 */
    mw_put_be32(packet + 12, 0);
    assert(mw_validate_packet(packet, 24) == MW_STATUS_INVALID);
    mw_put_be32(packet + 12, 1);

    /* Trailing unconsumed bytes */
    assert(mw_validate_packet(packet, 28) == MW_STATUS_INVALID);

    /* Unaligned bytes */
    assert(mw_validate_packet(packet, 25) == MW_STATUS_INVALID);

    printf("PASS: test_packet_framing\n");
}

static void test_opcode_validation(void)
{
    uint8_t packet[512];
    memset(packet, 0, sizeof(packet));

    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 1);

    /* 1. MW_CLEAR: 24 bytes payload (mask, r, g, b, a, depth) */
    mw_put_be32(packet + 8, 16 + 8 + 24);
    mw_put_be32(packet + 16, MW_CLEAR);
    mw_put_be32(packet + 20, 24);
    mw_put_be32(packet + 24, 3); /* mask = color | depth */
    mw_put_bef(packet + 28, 1.0f);
    mw_put_bef(packet + 32, 0.5f);
    mw_put_bef(packet + 36, 0.25f);
    mw_put_bef(packet + 40, 1.0f);
    mw_put_bef(packet + 44, 1.0f);
    assert(mw_validate_packet(packet, 48) == MW_STATUS_OK);

    /* Bad clear mask (e.g. bit 2 set) */
    mw_put_be32(packet + 24, 4);
    assert(mw_validate_packet(packet, 48) == MW_STATUS_INVALID);
    mw_put_be32(packet + 24, 3);

    /* 2. MW_TARGET: 24 bytes payload */
    mw_put_be32(packet + 8, 48);
    mw_put_be32(packet + 16, MW_TARGET);
    mw_put_be32(packet + 20, 24);
    mw_put_be32(packet + 24, 640);
    mw_put_be32(packet + 28, 480);
    mw_put_be32(packet + 32, 2);  /* 2 buffers */
    mw_put_be32(packet + 36, 24); /* 24 depth bits */
    mw_put_be32(packet + 40, 640);
    mw_put_be32(packet + 44, 480);
    assert(mw_validate_packet(packet, 48) == MW_STATUS_OK);

    /* Bad buffer count (4 buffers) */
    mw_put_be32(packet + 32, 4);
    assert(mw_validate_packet(packet, 48) == MW_STATUS_INVALID);

    /* 3. MW_SYNC: 4 bytes payload */
    mw_put_be32(packet + 8, 28);
    mw_put_be32(packet + 16, MW_SYNC);
    mw_put_be32(packet + 20, 4);
    mw_put_be32(packet + 24, 1);
    assert(mw_validate_packet(packet, 28) == MW_STATUS_OK);

    /* 4. MW_TEXTURE_DELETE: 4 bytes payload */
    mw_put_be32(packet + 16, MW_TEXTURE_DELETE);
    mw_put_be32(packet + 20, 4);
    mw_put_be32(packet + 24, 42);
    assert(mw_validate_packet(packet, 28) == MW_STATUS_OK);

    /* 5. Unknown opcode 999 */
    mw_put_be32(packet + 16, 999);
    mw_put_be32(packet + 20, 0);
    mw_put_be32(packet + 8, 24);
    assert(mw_validate_packet(packet, 24) == MW_STATUS_INVALID);

    printf("PASS: test_opcode_validation\n");
}

static void test_state_key_validation(void)
{
    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));

    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 1);
    mw_put_be32(packet + 16, MW_STATE);

    /* Key 1: MW_DEPTH_TEST (1 word, value 0 or 1) */
    mw_put_be32(packet + 20, 8 + 4);
    mw_put_be32(packet + 24, MW_DEPTH_TEST);
    mw_put_be32(packet + 28, 1); /* 1 word */
    mw_put_be32(packet + 32, 1); /* enable = 1 */
    mw_put_be32(packet + 8, 16 + 8 + 8 + 4);
    assert(mw_validate_packet(packet, 36) == MW_STATUS_OK);

    /* Invalid boolean > 1 */
    mw_put_be32(packet + 32, 2);
    assert(mw_validate_packet(packet, 36) == MW_STATUS_INVALID);
    mw_put_be32(packet + 32, 1);

    /* Key 2: MW_DEPTH_FUNC (1 word, compare enum 0x200..0x207) */
    mw_put_be32(packet + 24, MW_DEPTH_FUNC);
    mw_put_be32(packet + 32, 0x203); /* GL_LEQUAL */
    assert(mw_validate_packet(packet, 36) == MW_STATUS_OK);
    mw_put_be32(packet + 32, 0x208); /* out of range */
    assert(mw_validate_packet(packet, 36) == MW_STATUS_INVALID);

    /* Key 38 (RETIRED) must be rejected */
    mw_put_be32(packet + 24, 38);
    assert(mw_validate_packet(packet, 36) == MW_STATUS_INVALID);

    /* Key 43 (RETIRED) must be rejected */
    mw_put_be32(packet + 24, 43);
    assert(mw_validate_packet(packet, 36) == MW_STATUS_INVALID);

    /* Key 50: MW_TEXTURE_ENV_COLOR (5 words: unit < 2, RGBA 4 floats) */
    mw_put_be32(packet + 20, 8 + 20);
    mw_put_be32(packet + 8, 16 + 8 + 8 + 20);
    mw_put_be32(packet + 24, MW_TEXTURE_ENV_COLOR);
    mw_put_be32(packet + 28, 5);
    mw_put_be32(packet + 32, 0); /* unit 0 */
    mw_put_bef(packet + 36, 1.0f);
    mw_put_bef(packet + 40, 0.0f);
    mw_put_bef(packet + 44, 0.5f);
    mw_put_bef(packet + 48, 1.0f);
    assert(mw_validate_packet(packet, 52) == MW_STATUS_OK);

    /* Unit >= 2 rejected */
    mw_put_be32(packet + 32, 2);
    assert(mw_validate_packet(packet, 52) == MW_STATUS_INVALID);

    printf("PASS: test_state_key_validation\n");
}

static void test_draw_validation_and_nan(void)
{
    uint8_t packet[512];
    memset(packet, 0, sizeof(packet));

    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 1);

    /* MW_DRAW_OBJECT with 3 vertices = 8 + 3 * 76 = 236 bytes payload */
    uint32_t payload_bytes = 8 + 3 * 76;
    uint32_t total_bytes = 16 + 8 + payload_bytes;

    mw_put_be32(packet + 8, total_bytes);
    mw_put_be32(packet + 16, MW_DRAW_OBJECT);
    mw_put_be32(packet + 20, payload_bytes);
    mw_put_be32(packet + 24, 4); /* GL_TRIANGLES */
    mw_put_be32(packet + 28, 3); /* count = 3 */

    /* Normal finite coordinates */
    mw_put_bef(packet + 32, 0.0f);
    mw_put_bef(packet + 36, 1.0f);
    mw_put_bef(packet + 40, 0.0f);
    mw_put_bef(packet + 44, 1.0f);
    assert(mw_validate_packet(packet, total_bytes) == MW_STATUS_OK);

    /* Per spec: NaN/Inf vertex words are drawn, NOT rejected! */
    float nan_val = 0.0f / 0.0f;
    float inf_val = 1.0f / 0.0f;
    mw_put_bef(packet + 32, nan_val);
    mw_put_bef(packet + 36, inf_val);
    assert(mw_validate_packet(packet, total_bytes) == MW_STATUS_OK);

    /* Invalid primitive mode > 6 */
    mw_put_be32(packet + 24, 7);
    assert(mw_validate_packet(packet, total_bytes) == MW_STATUS_INVALID);

    /* Invalid triangle vertex count (not multiple of 3) */
    mw_put_be32(packet + 24, 4);
    mw_put_be32(packet + 28, 4);
    assert(mw_validate_packet(packet, total_bytes) == MW_STATUS_INVALID);

    printf("PASS: test_draw_validation_and_nan\n");
}

int main(void)
{
    test_packet_framing();
    test_opcode_validation();
    test_state_key_validation();
    test_draw_validation_and_nan();
    printf("All packet validation tests PASSED!\n");
    return 0;
}
