/*
 * Unit tests for Wire Protocol v9 packet parsing and validation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
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
static void test_draw_packed_indexed_validation(void)
{
    uint8_t packet[512];
    memset(packet, 0, sizeof(packet));

    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 12, 1);

    uint32_t format = (1U << MW_PACK_POSITION) | (1U << MW_PACK_COLOR) | (1U << MW_PACK_TEX0);
    uint32_t v_count = 3;
    uint32_t idx_count = 3;
    uint32_t words = 6;
    uint32_t constants = 7;
    uint32_t idx_bytes = ((idx_count * 2 + 3) & ~3U);
    uint32_t payload = 16 + 4 * (constants + v_count * words) + idx_bytes;
    uint32_t total = 16 + 8 + payload;

    mw_put_be32(packet + 8, total);
    mw_put_be32(packet + 16, MW_DRAW_PACKED_INDEXED);
    mw_put_be32(packet + 20, payload);
    mw_put_be32(packet + 24, 4); /* GL_TRIANGLES */
    mw_put_be32(packet + 28, v_count);
    mw_put_be32(packet + 32, idx_count);
    mw_put_be32(packet + 36, format);

    uint8_t *idx_ptr = packet + 24 + 16 + 4 * (constants + v_count * words);
    mw_put_be16(idx_ptr, 0);
    mw_put_be16(idx_ptr + 2, 1);
    mw_put_be16(idx_ptr + 4, 2);
    mw_put_be16(idx_ptr + 6, 0);

    assert(mw_validate_packet(packet, total) == MW_STATUS_OK);

    /* Out-of-range index >= v_count */
    mw_put_be16(idx_ptr + 4, 3);
    assert(mw_validate_packet(packet, total) == MW_STATUS_INVALID);
    mw_put_be16(idx_ptr + 4, 2);

    /* Truncated payload */
    mw_put_be32(packet + 20, payload - 4);
    mw_put_be32(packet + 8, total - 4);
    assert(mw_validate_packet(packet, total - 4) == MW_STATUS_INVALID);
    mw_put_be32(packet + 20, payload);
    mw_put_be32(packet + 8, total);

    /* Invalid mode > 6 */
    mw_put_be32(packet + 24, 7);
    assert(mw_validate_packet(packet, total) == MW_STATUS_INVALID);
    mw_put_be32(packet + 24, 4);

    /* Non-multiple-of-3 index count for GL_TRIANGLES */
    mw_put_be32(packet + 32, 4);
    assert(mw_validate_packet(packet, total) == MW_STATUS_INVALID);
    mw_put_be32(packet + 32, idx_count);

    /* Zero v_count */
    mw_put_be32(packet + 28, 0);
    assert(mw_validate_packet(packet, total) == MW_STATUS_INVALID);
    mw_put_be32(packet + 28, v_count);

    printf("PASS: test_draw_packed_indexed_validation\n");
}

static void test_raw_array_index_validation(void)
{
    uint8_t packet[88];
    uint8_t *payload = packet + 24;

    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, sizeof(packet));
    mw_put_be32(packet + 12, 1);
    mw_put_be32(packet + 16, MW_DRAW_ARRAY_RAW);
    mw_put_be32(packet + 20, 64);
    mw_put_be32(payload, 0);          /* GL_POINTS */
    mw_put_be32(payload + 4, 1);      /* one source vertex */
    mw_put_be32(payload + 16, 1);     /* one stream */
    mw_put_be32(payload + 20, 0x1405);/* GL_UNSIGNED_INT indices */
    mw_put_be32(payload + 24, 4);
    mw_put_be32(payload + 32, 0);     /* position */
    mw_put_be32(payload + 36, 2);
    mw_put_be32(payload + 40, 0x1406);/* GL_FLOAT */
    mw_put_be32(payload + 44, 8);
    mw_put_be32(payload + 48, 8);
    mw_put_bef(payload + 52, 0.0f);
    mw_put_bef(payload + 56, 0.0f);
    mw_put_be32(payload + 60, 2);

    assert(mw_validate_packet(packet, sizeof(packet)) == MW_STATUS_INVALID);
    mw_put_be32(payload + 60, 0);
    assert(mw_validate_packet(packet, sizeof(packet)) == MW_STATUS_OK);
    printf("PASS: test_raw_array_index_validation\n");
}
static void test_raw_array_components_validation(void)
{
    uint8_t packet[84];
    uint8_t *payload = packet + 24;

    memset(packet, 0, sizeof(packet));
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, sizeof(packet));
    mw_put_be32(packet + 12, 1);
    mw_put_be32(packet + 16, MW_DRAW_ARRAY_RAW);
    mw_put_be32(packet + 20, 60);
    mw_put_be32(payload, 0);          /* GL_POINTS */
    mw_put_be32(payload + 4, 1);      /* one source vertex */
    mw_put_be32(payload + 16, 1);     /* one stream */
    mw_put_be32(payload + 20, 0);     /* index_type none */
    mw_put_be32(payload + 24, 0);     /* index_bytes 0 */
    mw_put_be32(payload + 32, 0);     /* position slot */
    mw_put_be32(payload + 36, 1026);  /* components = 1026 (> 4) */
    mw_put_be32(payload + 40, 0x1406);/* GL_FLOAT */
    mw_put_be32(payload + 44, 1026 * 4);
    mw_put_be32(payload + 48, 8);

    assert(mw_validate_packet(packet, sizeof(packet)) == MW_STATUS_INVALID);

    mw_put_be32(payload + 36, 32);    /* components = 32 */
    assert(mw_validate_packet(packet, sizeof(packet)) == MW_STATUS_INVALID);

    mw_put_be32(payload + 36, 2);     /* valid components = 2 */
    mw_put_be32(payload + 44, 8);
    mw_put_be32(payload + 48, 8);
    assert(mw_validate_packet(packet, sizeof(packet)) == MW_STATUS_OK);
    printf("PASS: test_raw_array_components_validation\n");
}

static void test_raw_texture_empty_payload(void)
{
    uint8_t *packet = calloc(1U, 24U);

    assert(packet != NULL);
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4U, MW_VERSION);
    mw_put_be32(packet + 8U, 24U);
    mw_put_be32(packet + 12U, 1U);
    mw_put_be32(packet + 16U, MW_TEXTURE_IMAGE_RAW);
    assert(mw_validate_packet(packet, 24U) == MW_STATUS_INVALID);
    mw_put_be32(packet + 16U, MW_TEXTURE_SUB_RAW);
    assert(mw_validate_packet(packet, 24U) == MW_STATUS_INVALID);
    free(packet);
    printf("PASS: test_raw_texture_empty_payload\n");
}


int main(void)
{
    test_packet_framing();
    test_opcode_validation();
    test_state_key_validation();
    test_draw_validation_and_nan();
    test_draw_packed_indexed_validation();
    test_raw_array_index_validation();
    test_raw_array_components_validation();
    test_raw_texture_empty_payload();
    printf("All packet validation tests PASSED!\n");
    return 0;
}
