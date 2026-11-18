/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The ZZ9000 private wire of the AAR transport: the mailbox operations a
 * client (ZZRun, the SDK tools, later aar.library) sends to the card to claim
 * an app, load it, start and stop it, ring the doorbell and release it. This
 * header is the single source of that wire. The queue pair (control block,
 * request and completion rings, entry layouts, op codes) is not here: it is
 * the portable contract of aar/transport.h.
 *
 * Mailbox payloads are the 48-byte entry payload, big-endian. Offsets are
 * from the start of that payload. Every operation except QUERY and CLAIM names
 * its claim by handle at ZZAPP_SEL_HANDLE (aar/transport.h, spec 10.2). A
 * handle that is not the live one gets ZZAPP_ERR_STALE and changes nothing.
 * Unused bytes are zero.
 *
 * The launcher window of the earlier protocol (life block, rings, file wire,
 * slots, ZZAPP_OP_ALLOC_WINDOW, ZZAPP_OP_STATUS, ZZAPP_OP_DISPLAY_BIND) is
 * gone; ZZAPP_OP_CLAIM is the admission and the control block replaces STATUS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_PROTO_H
#define ZZAPP_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* The QUERY reply's ABI words report the AAR API this firmware implements
 * (AAR_API_MAJOR, AAR_API_MINOR in aar/app.h). */
#define ZZAPP_ABI_MAJOR            0u
#define ZZAPP_ABI_MINOR            5u

/* ---- Operations --------------------------------------------------------- */

#define ZZAPP_OP_QUERY             0x0c00u /* no handle */
#define ZZAPP_OP_CLAIM             0x0c01u /* no handle; admits an app */
#define ZZAPP_OP_LOAD_BEGIN        0x0c02u
#define ZZAPP_OP_LOAD_CHUNK        0x0c03u
#define ZZAPP_OP_LOAD_END          0x0c04u
#define ZZAPP_OP_START             0x0c05u
#define ZZAPP_OP_STOP              0x0c06u
/* 0x0c07 (STATUS) and 0x0c08 (DISPLAY_BIND) are retired: the client reads the
 * control block, and binds the display by completing AAR_OP_DISPLAY_OPEN. */
#define ZZAPP_OP_FIRST             ZZAPP_OP_QUERY
/* Next live claim at or after a cursor index; takes no handle. */
#define ZZAPP_OP_LIST              0x0c09u
/* Ends a claim: stops the app if it still runs, then returns the control
 * block, the queues and every service buffer. The normal close. */
#define ZZAPP_OP_RELEASE           0x0c0au
/* Doorbell: the client has posted completions or freed submission space. A
 * hint only; the queue indices are authoritative. */
#define ZZAPP_OP_KICK              0x0c0bu
#define ZZAPP_OP_LAST              ZZAPP_OP_KICK
/* 0x0c00..0x0c1f is reserved. */
#define ZZAPP_OP_RANGE_LO          0x0c00u
#define ZZAPP_OP_RANGE_HI          0x0c1fu

/* QUERY reply caps. Z2 clears every bit and returns unsupported. */
#define ZZAPP_CAP_Z3               (1u << 0)
#define ZZAPP_CAP_DISPLAY          (1u << 1)
#define ZZAPP_CAP_AUDIO            (1u << 2)
#define ZZAPP_CAP_GL               (1u << 3)
#define ZZAPP_CAP_CLUT8            (1u << 4)

/* Status of a request whose handle is not the live one. Matches
 * SDK_STATUS_BAD_HANDLE. */
#define ZZAPP_ERR_STALE            5u

#define ZZAPP_MAILBOX_PAYLOAD_BYTES 48u

/* ---- Handle ------------------------------------------------------------- */

/* The claim, in every operation except QUERY and CLAIM. Bytes 44 to 47 are
 * reserved and zero. */
#define ZZAPP_SEL_HANDLE           40u
#define ZZAPP_SEL_RESERVED         44u

/* ---- QUERY (no handle) -------------------------------------------------- */

/* The reply fills the 48-byte payload. The counts are what a claim, load and
 * release cycle returns to. COMMIT_FREE is the bytes still admissible for app
 * commits and COMMIT_TOTAL that plus what is committed. TABLES_FREE counts L2
 * tables a new admission can still reserve and TABLES_TOTAL the pool less the
 * kernel template. HOSTHEAP_LARGEST is the largest free block of the
 * host-visible heap. QUEUE_DEPTH is the depth of a new claim's queues. */
#define ZZAPP_QUERY_ABI_MAJOR      0u
#define ZZAPP_QUERY_ABI_MINOR      4u
#define ZZAPP_QUERY_STATE          8u
#define ZZAPP_QUERY_CAPS           12u
#define ZZAPP_QUERY_HOSTHEAP_LARGEST 16u
#define ZZAPP_QUERY_QUEUE_DEPTH    20u
#define ZZAPP_QUERY_COMMIT_FREE    24u
#define ZZAPP_QUERY_COMMIT_TOTAL   28u
#define ZZAPP_QUERY_RECORDS_FREE   32u
#define ZZAPP_QUERY_TABLES_FREE    36u
#define ZZAPP_QUERY_TABLES_TOTAL   40u
#define ZZAPP_QUERY_HOSTHEAP_FREE  44u

/* ---- CLAIM (no handle) -------------------------------------------------- */

/* Reply: the claim's handle, the token of its control block (a board offset,
 * the card address less ADDR_ADJ, as aar_shared.haddr) and the contract size
 * of the block (AAR_PAIR_BYTES at the default depth). Depth, ring offsets and
 * ring tokens are read from the control block descriptors. */
#define ZZAPP_CLAIM_HANDLE         0u
#define ZZAPP_CLAIM_CB_TOKEN       4u
#define ZZAPP_CLAIM_CB_BYTES       8u
#define ZZAPP_CLAIM_REPLY_BYTES    12u

/* A refused CLAIM, LOAD_BEGIN or LOAD_END replies with the reason alone
 * (AAR_REASON_*). */
#define ZZAPP_REFUSAL_REASON       0u
#define ZZAPP_REFUSAL_REPLY_BYTES  4u

/* ---- LOAD_BEGIN, LOAD_CHUNK, LOAD_END, START ---------------------------- */

/* LOAD_BEGIN reads the limits the client wrote to the control block
 * (AAR_CB_PRIVATE_MIB, AAR_CB_SHARED_MIB, AAR_CB_WEIGHT). Its request carries
 * only the client's capabilities. It allocates the load buffer, which holds
 * ZZAPP_LOAD_STAGING_BYTES of image staging, then the argv area, and the
 * (deferred) reply names it. */
#define ZZAPP_LOAD_CLIENT_CAPS     0u
#define ZZAPP_LOAD_BEGIN_REQ_BYTES 4u
#define ZZAPP_LOAD_REPLY_TOKEN     0u  /* token of the load buffer */
#define ZZAPP_LOAD_REPLY_SIZE      4u  /* size of the load buffer */
#define ZZAPP_LOAD_REPLY_CHUNK_MAX 8u  /* largest LOAD_CHUNK length */
#define ZZAPP_LOAD_REPLY_ARGV_OFF  12u /* offset of the argv area */
#define ZZAPP_LOAD_BEGIN_REPLY_BYTES 16u

/* Client capabilities, ZZAPP_LOAD_CLIENT_CAPS. They are ZZ9000 details with
 * no control block word. */
#define ZZAPP_CLIENT_LEASE_EXTENDED (1u << 0) /* extended main-loop lease */
#define ZZAPP_CLIENT_AUDIO_AHI      (1u << 1) /* can serve the AHI audio route */
#define ZZAPP_CLIENT_CLUT8          (1u << 2) /* can serve 8-bit CLUT screens */
#define ZZAPP_CLIENT_SCANOUT_ACK    (1u << 3) /* acknowledges pan flips */
#define ZZAPP_CLIENT_LATEST         (1u << 4) /* accepts card-owned flips */
#define ZZAPP_CLIENT_KNOWN          0x1fu

#define ZZAPP_LOAD_STAGING_BYTES   0x00010000u /* 64 KiB, at buffer offset 0 */
#define ZZAPP_LOAD_ARGV_OFF        ZZAPP_LOAD_STAGING_BYTES
#define ZZAPP_LOAD_ARGV_BYTES      2048u
#define ZZAPP_LOAD_BUFFER_BYTES    0x00012000u /* 72 KiB */

/* LOAD_CHUNK: the firmware reads `length` bytes at offset 0 of the load
 * buffer and places them at `file_off` of the image. */
#define ZZAPP_CHUNK_FILE_OFF       0u
#define ZZAPP_CHUNK_LENGTH         4u
#define ZZAPP_CHUNK_REQ_BYTES      8u
#define ZZAPP_CHUNK_MIN            1u
#define ZZAPP_CHUNK_MAX            ZZAPP_LOAD_STAGING_BYTES
#define ZZAPP_IMAGE_MAX_BYTES      0x01000000u

/* LOAD_END replies with the status, and on a refusal the reason alone. */

/* START: the argv table and strings are in the load buffer at the argv
 * offset; the request carries only argc. The load buffer is freed after the
 * firmware has copied argv. Argv area: u32 argc, then ZZAPP_ARGV_MAX offsets
 * from the start of the load buffer, then the strings, each at most
 * ZZAPP_ARGV_STR_MAX bytes including the NUL. */
#define ZZAPP_START_ARGC           0u
#define ZZAPP_START_REQ_BYTES      4u
#define ZZAPP_ARGV_MAX             16u
#define ZZAPP_ARGV_STR_MAX         128u
#define ZZAPP_ARGV_ARGC_OFF        0u
#define ZZAPP_ARGV_OFFS_OFF        4u
#define ZZAPP_ARGV_STR_OFF         (4u + ZZAPP_ARGV_MAX * 4u)

/* Default limit on host-visible (shared) memory, in bytes, when the client
 * writes 0 to AAR_CB_SHARED_MIB. */
#define ZZAPP_HOST_VISIBLE_DEFAULT_BYTES (16u << 20)

/* ---- STOP, RELEASE ------------------------------------------------------ */

/* STOP: the handle alone. An unconditional stop of the named claim. RELEASE:
 * the handle alone; the reply is the status, OK for any valid handle. */

/* ---- LIST --------------------------------------------------------------- */

/* Request: the cursor, a record index, at 0. No handle. Reply: the next live
 * claim at or after the cursor, or handle 0 when there is none. A client lists
 * everything by passing INDEX + 1 until HANDLE is 0. A claim whose handle was
 * revoked (quarantine) is listed under its current handle, so an
 * administrative RELEASE works. */
#define ZZAPP_LIST_CURSOR          0u
#define ZZAPP_LIST_STATE           0u  /* AAR_STATE_* */
#define ZZAPP_LIST_HANDLE          4u  /* 0: no further claim */
#define ZZAPP_LIST_COMMITTED       8u
#define ZZAPP_LIST_INDEX           12u
#define ZZAPP_LIST_CPU_MS          16u
#define ZZAPP_LIST_SERVICE_MS      20u
#define ZZAPP_LIST_REASONS         24u /* block (u8), end reason (u8), flags (u16) */
#define ZZAPP_LIST_NAME            28u
#define ZZAPP_LIST_NAME_BYTES      20u
#define ZZAPP_LIST_REPLY_BYTES     48u
#define ZZAPP_LIST_FLAG_OVERLOAD   1u

/* End reasons (AAR_END_* values). */
#define ZZAPP_END_NONE             0u
#define ZZAPP_END_EXIT             1u
#define ZZAPP_END_FORCED           2u
#define ZZAPP_END_USER_FAULT       3u
#define ZZAPP_END_STALL            4u
#define ZZAPP_END_STUCK_CALL       5u
#define ZZAPP_END_LEASE            6u
#define ZZAPP_END_AMIGA_RESET      7u
#define ZZAPP_END_CORE_RESTART     8u
#define ZZAPP_END_QUARANTINED      9u

/* What the app is blocked on, LIST block reasons. */
#define ZZAPP_BLOCK_NONE           0u
#define ZZAPP_BLOCK_FILE           1u
#define ZZAPP_BLOCK_CONSOLE        2u
#define ZZAPP_BLOCK_EVENT          3u
#define ZZAPP_BLOCK_SLEEP          4u
#define ZZAPP_BLOCK_VSYNC          5u
#define ZZAPP_BLOCK_AUDIO          6u

/* ---- KICK --------------------------------------------------------------- */

/* Flags word at 0. Handler on core 0: look the handle up, OR the flags into
 * the claim's kick word, wake core 1. The reply is the status. */
#define ZZAPP_KICK_FLAGS           0u
#define ZZAPP_KICK_REQ_BYTES       4u
#define ZZAPP_KICK_CQ_POSTED       (1u << 0) /* completions or events posted */
#define ZZAPP_KICK_SQ_SPACE        (1u << 1) /* submission queue entries freed */
#define ZZAPP_KICK_KNOWN           0x3u

/* ---- Entry flags -------------------------------------------------------- */

/* AAR_OP_DISPLAY_FLIP completion flags (the entry's uint16 flags): the client
 * submitted the pan or buffer change and relies on the card's vertical blank
 * to show it. Without it the flip is complete when the completion arrives. */
#define ZZAPP_FLIPF_SUBMITTED      (1u << 0)

/* ---- Layout checks ------------------------------------------------------ */

_Static_assert(ZZAPP_QUERY_HOSTHEAP_FREE + 4u == ZZAPP_MAILBOX_PAYLOAD_BYTES,
               "query fills the payload");
_Static_assert(ZZAPP_CLAIM_REPLY_BYTES <= ZZAPP_SEL_HANDLE, "claim reply");
_Static_assert(ZZAPP_LOAD_BEGIN_REPLY_BYTES <= ZZAPP_SEL_HANDLE, "load reply");
_Static_assert(ZZAPP_CHUNK_REQ_BYTES <= ZZAPP_SEL_HANDLE, "chunk request");
_Static_assert(ZZAPP_START_REQ_BYTES <= ZZAPP_SEL_HANDLE, "start request");
_Static_assert(ZZAPP_KICK_REQ_BYTES <= ZZAPP_SEL_HANDLE, "kick request");
_Static_assert(ZZAPP_LIST_NAME + ZZAPP_LIST_NAME_BYTES == ZZAPP_LIST_REPLY_BYTES,
               "list name ends the reply");
_Static_assert(ZZAPP_LIST_REPLY_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "list reply");
_Static_assert(ZZAPP_SEL_RESERVED + 4u == ZZAPP_MAILBOX_PAYLOAD_BYTES,
               "the handle selector ends the payload");
_Static_assert(ZZAPP_SEL_HANDLE + 4u == ZZAPP_SEL_RESERVED, "handle word");
_Static_assert(ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES <=
               ZZAPP_LOAD_BUFFER_BYTES, "load buffer layout");
_Static_assert((ZZAPP_LOAD_BUFFER_BYTES & 0x1fffu) == 0u,
               "load buffer is a multiple of 8 KiB");
_Static_assert(ZZAPP_ARGV_STR_OFF < ZZAPP_LOAD_ARGV_BYTES, "argv header");
_Static_assert(ZZAPP_ARGV_STR_OFF + ZZAPP_ARGV_STR_MAX <= ZZAPP_LOAD_ARGV_BYTES,
               "one max argv string");
_Static_assert(ZZAPP_OP_LAST <= ZZAPP_OP_RANGE_HI, "operations in range");

/* ---- Big-endian accessors ----------------------------------------------- */

/* Byte stores and loads, so they work on any alignment and either host. */
static inline void zzapp_put_be16(void *p, uint16_t value)
{
    uint8_t *b = (uint8_t *)p;

    b[0] = (uint8_t)(value >> 8);
    b[1] = (uint8_t)value;
}

static inline void zzapp_put_be32(void *p, uint32_t value)
{
    uint8_t *b = (uint8_t *)p;

    b[0] = (uint8_t)(value >> 24);
    b[1] = (uint8_t)(value >> 16);
    b[2] = (uint8_t)(value >> 8);
    b[3] = (uint8_t)value;
}

static inline void zzapp_put_be64(void *p, uint64_t value)
{
    zzapp_put_be32(p, (uint32_t)(value >> 32));
    zzapp_put_be32((uint8_t *)p + 4, (uint32_t)value);
}

static inline uint16_t zzapp_get_be16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;

    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static inline uint32_t zzapp_get_be32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;

    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static inline uint64_t zzapp_get_be64(const void *p)
{
    return ((uint64_t)zzapp_get_be32(p) << 32) |
           zzapp_get_be32((const uint8_t *)p + 4);
}

static inline int32_t zzapp_get_be32s(const void *p)
{
    return (int32_t)zzapp_get_be32(p);
}

static inline int16_t zzapp_get_be16s(const void *p)
{
    return (int16_t)zzapp_get_be16(p);
}

/* ---- Request builders --------------------------------------------------- */

/* Handle word of an operation that names a claim. */
static inline void zzapp_build_handle(uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES],
                                      uint32_t handle)
{
    zzapp_put_be32(payload + ZZAPP_SEL_HANDLE, handle);
    zzapp_put_be32(payload + ZZAPP_SEL_RESERVED, 0u);
}

static inline void zzapp_build_load_begin_request(
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES], uint32_t handle,
    uint32_t client_caps)
{
    zzapp_put_be32(payload + ZZAPP_LOAD_CLIENT_CAPS, client_caps);
    zzapp_build_handle(payload, handle);
}

static inline void zzapp_build_load_chunk_request(
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES], uint32_t handle,
    uint32_t file_off, uint32_t length)
{
    zzapp_put_be32(payload + ZZAPP_CHUNK_FILE_OFF, file_off);
    zzapp_put_be32(payload + ZZAPP_CHUNK_LENGTH, length);
    zzapp_build_handle(payload, handle);
}

static inline void zzapp_build_start_app_request(
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES], uint32_t handle,
    uint32_t argc)
{
    zzapp_put_be32(payload + ZZAPP_START_ARGC, argc);
    zzapp_build_handle(payload, handle);
}

static inline void zzapp_build_kick_request(
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES], uint32_t handle,
    uint32_t flags)
{
    zzapp_put_be32(payload + ZZAPP_KICK_FLAGS, flags);
    zzapp_build_handle(payload, handle);
}

static inline void zzapp_build_list_request(
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES], uint32_t cursor)
{
    zzapp_put_be32(payload + ZZAPP_LIST_CURSOR, cursor);
}

#endif /* ZZAPP_PROTO_H */
