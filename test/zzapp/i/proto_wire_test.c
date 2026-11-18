/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The ZZ9000 private wire (zzapp_proto.h) with the window protocol shim
 * switched off: it must stand alone, name no window, and lay the payloads out
 * as the transport design says. Byte offsets are checked against the builders
 * so a drifted constant fails here and not on the card.
 */
#include <stdio.h>
#include <string.h>

#include "zzapp_proto.h"

/* None of the window protocol may leak into the wire header. */
#ifdef ZZAPP_WINDOW_BYTES
#error "the window layout is back in zzapp_proto.h"
#endif
#ifdef ZZAPP_OP_ALLOC_WINDOW
#error "ZZAPP_OP_ALLOC_WINDOW is back in zzapp_proto.h"
#endif
#ifdef ZZAPP_SLOT_COUNT
#error "slots are back in zzapp_proto.h"
#endif
#ifdef ZZAPP_SEL_SLOT
#error "the slot selector is back in zzapp_proto.h"
#endif
#ifdef ZZAPP_OP_STATUS
#error "ZZAPP_OP_STATUS is back in zzapp_proto.h"
#endif
#ifdef ZZAPP_OP_DISPLAY_BIND
#error "ZZAPP_OP_DISPLAY_BIND is back in zzapp_proto.h"
#endif
#ifdef ZZAPP_LIFE_MAGIC
#error "the life block is back in zzapp_proto.h"
#endif

static int checks, failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

int main(void)
{
    uint8_t p[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    /* Operations: the old opcodes keep their numbers, KICK is new. */
    CHECK(ZZAPP_OP_QUERY == 0x0c00u && ZZAPP_OP_CLAIM == 0x0c01u);
    CHECK(ZZAPP_OP_LOAD_BEGIN == 0x0c02u && ZZAPP_OP_LOAD_CHUNK == 0x0c03u);
    CHECK(ZZAPP_OP_LOAD_END == 0x0c04u && ZZAPP_OP_START == 0x0c05u);
    CHECK(ZZAPP_OP_STOP == 0x0c06u && ZZAPP_OP_LIST == 0x0c09u);
    CHECK(ZZAPP_OP_RELEASE == 0x0c0au && ZZAPP_OP_KICK == 0x0c0bu);
    CHECK(ZZAPP_OP_FIRST == ZZAPP_OP_QUERY && ZZAPP_OP_LAST == ZZAPP_OP_KICK);
    CHECK(ZZAPP_ERR_STALE == 5u);

    /* Every handle operation names its claim at 40, 44 reserved. */
    CHECK(ZZAPP_SEL_HANDLE == 40u && ZZAPP_SEL_RESERVED == 44u);
    memset(p, 0xff, sizeof(p));
    zzapp_build_handle(p, 0x00030007u);
    CHECK(zzapp_get_be32(p + 40) == 0x00030007u && zzapp_get_be32(p + 44) == 0u);
    CHECK(p[40] == 0x00 && p[41] == 0x03 && p[42] == 0x00 && p[43] == 0x07);

    memset(p, 0, sizeof(p));
    zzapp_build_load_begin_request(p, 0x10001u,
        ZZAPP_CLIENT_LEASE_EXTENDED | ZZAPP_CLIENT_LATEST);
    CHECK(zzapp_get_be32(p + ZZAPP_LOAD_CLIENT_CAPS) == 0x11u);
    CHECK(zzapp_get_be32(p + ZZAPP_SEL_HANDLE) == 0x10001u);

    memset(p, 0, sizeof(p));
    zzapp_build_load_chunk_request(p, 0x10001u, 0x1234u, 0x8000u);
    CHECK(zzapp_get_be32(p + ZZAPP_CHUNK_FILE_OFF) == 0x1234u);
    CHECK(zzapp_get_be32(p + ZZAPP_CHUNK_LENGTH) == 0x8000u);
    CHECK(zzapp_get_be32(p + 8) == 0u);            /* no staging word any more */
    CHECK(ZZAPP_CHUNK_REQ_BYTES == 8u && ZZAPP_CHUNK_MAX == 65536u);

    memset(p, 0, sizeof(p));
    zzapp_build_start_app_request(p, 0x20002u, 3u);
    CHECK(zzapp_get_be32(p + ZZAPP_START_ARGC) == 3u);
    CHECK(zzapp_get_be32(p + ZZAPP_SEL_HANDLE) == 0x20002u);

    memset(p, 0, sizeof(p));
    zzapp_build_kick_request(p, 0x20002u, ZZAPP_KICK_CQ_POSTED | ZZAPP_KICK_SQ_SPACE);
    CHECK(zzapp_get_be32(p + ZZAPP_KICK_FLAGS) == 3u);
    CHECK(zzapp_get_be32(p + ZZAPP_SEL_HANDLE) == 0x20002u);

    memset(p, 0, sizeof(p));
    zzapp_build_list_request(p, 5u);
    CHECK(zzapp_get_be32(p + ZZAPP_LIST_CURSOR) == 5u);

    /* Replies: CLAIM gives handle, token and size; LOAD_BEGIN the buffer. */
    CHECK(ZZAPP_CLAIM_HANDLE == 0u && ZZAPP_CLAIM_CB_TOKEN == 4u &&
          ZZAPP_CLAIM_CB_BYTES == 8u && ZZAPP_CLAIM_REPLY_BYTES == 12u);
    CHECK(ZZAPP_REFUSAL_REASON == 0u && ZZAPP_REFUSAL_REPLY_BYTES == 4u);
    CHECK(ZZAPP_LOAD_REPLY_TOKEN == 0u && ZZAPP_LOAD_REPLY_SIZE == 4u &&
          ZZAPP_LOAD_REPLY_CHUNK_MAX == 8u && ZZAPP_LOAD_REPLY_ARGV_OFF == 12u);
    CHECK(ZZAPP_LOAD_ARGV_OFF == 0x10000u && ZZAPP_LOAD_BUFFER_BYTES == 0x12000u);
    CHECK(ZZAPP_ARGV_STR_OFF == 68u && ZZAPP_ARGV_MAX == 16u);

    /* QUERY fills the payload; the two new words sit where the slot counts were. */
    CHECK(ZZAPP_QUERY_HOSTHEAP_LARGEST == 16u && ZZAPP_QUERY_QUEUE_DEPTH == 20u);
    CHECK(ZZAPP_QUERY_COMMIT_FREE == 24u && ZZAPP_QUERY_HOSTHEAP_FREE == 44u);

    /* LIST keeps the offsets of the per-slot snapshot, with handle and index
     * in the words that held the generation and the limit. */
    CHECK(ZZAPP_LIST_STATE == 0u && ZZAPP_LIST_HANDLE == 4u &&
          ZZAPP_LIST_COMMITTED == 8u && ZZAPP_LIST_INDEX == 12u &&
          ZZAPP_LIST_NAME == 28u && ZZAPP_LIST_REPLY_BYTES == 48u);

    CHECK(ZZAPP_FLIPF_SUBMITTED == 1u);
    CHECK(ZZAPP_END_QUARANTINED == 9u && ZZAPP_BLOCK_AUDIO == 6u);

    /* Big-endian accessors: odd addresses, 64 bits. */
    memset(p, 0, sizeof(p));
    zzapp_put_be64(p + 1, 0x0102030405060708ull);
    CHECK(p[1] == 1 && p[8] == 8 && zzapp_get_be64(p + 1) == 0x0102030405060708ull);
    zzapp_put_be16(p + 3, 0xabcd);
    CHECK(zzapp_get_be16(p + 3) == 0xabcd && zzapp_get_be16s(p + 3) == (int16_t)0xabcd);
    zzapp_put_be32(p + 5, 0xfffffffeu);
    CHECK(zzapp_get_be32s(p + 5) == -2);

    printf("zzapp_proto_wire: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
