/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * ZZ9000 trusted-app launcher protocol. Shared by the firmware and ZZRun.
 * Apps do not include this header. Integer fields on the wire are
 * big-endian. Layout is section 4 of docs/app-runtime.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_PROTO_H
#define ZZAPP_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define ZZAPP_ABI_MAJOR            0u
#define ZZAPP_ABI_MINOR            0u

#define ZZAPP_OP_QUERY             0x0c00u
#define ZZAPP_OP_ALLOC_WINDOW      0x0c01u
#define ZZAPP_OP_LOAD_BEGIN        0x0c02u
#define ZZAPP_OP_LOAD_CHUNK        0x0c03u
#define ZZAPP_OP_LOAD_END          0x0c04u
#define ZZAPP_OP_START             0x0c05u
#define ZZAPP_OP_STOP              0x0c06u
#define ZZAPP_OP_STATUS            0x0c07u
#define ZZAPP_OP_DISPLAY_BIND      0x0c08u
#define ZZAPP_OP_FIRST             ZZAPP_OP_QUERY
#define ZZAPP_OP_LAST              ZZAPP_OP_DISPLAY_BIND
/* 0x0c00..0x0c1f is reserved. v0 uses FIRST..LAST. */
#define ZZAPP_OP_RANGE_LO          0x0c00u
#define ZZAPP_OP_RANGE_HI          0x0c1fu

/* APP_QUERY reply caps. Z2 clears every bit and returns unsupported. */
#define ZZAPP_CAP_Z3               (1u << 0)
#define ZZAPP_CAP_DISPLAY          (1u << 1)
#define ZZAPP_CAP_AUDIO            (1u << 2)
#define ZZAPP_CAP_GL               (1u << 3)

/*
 * Mailbox payloads are big-endian in the 48-byte entry payload.
 * Offsets below are from the start of that payload.
 */
#define ZZAPP_QUERY_ABI_MAJOR      0u
#define ZZAPP_QUERY_ABI_MINOR      4u
#define ZZAPP_QUERY_STATE          8u
#define ZZAPP_QUERY_CAPS           12u
#define ZZAPP_QUERY_REPLY_BYTES    16u

#define ZZAPP_ALLOC_ARM_ADDRESS    0u
#define ZZAPP_ALLOC_SIZE           4u
#define ZZAPP_ALLOC_HOST_OFFSET    8u
#define ZZAPP_ALLOC_REPLY_BYTES    12u

#define ZZAPP_CHUNK_FILE_OFF       0u
#define ZZAPP_CHUNK_LENGTH         4u
#define ZZAPP_CHUNK_STAGING        8u
#define ZZAPP_CHUNK_REQ_BYTES      12u
#define ZZAPP_CHUNK_MIN            1u
#define ZZAPP_CHUNK_MAX            65536u

#define ZZAPP_START_ARGC           0u
#define ZZAPP_START_REQ_BYTES      4u

#define ZZAPP_BIND_WIDTH           0u
#define ZZAPP_BIND_HEIGHT          4u
#define ZZAPP_BIND_PITCH           8u
#define ZZAPP_BIND_FORMAT          12u
#define ZZAPP_BIND_CARD_FRONT      16u
#define ZZAPP_BIND_CARD_BACK       20u
#define ZZAPP_BIND_REQ_BYTES       24u

/* APP_STATUS snapshot. The full block stays in the rings window. */
#define ZZAPP_STATUS_STATE         0u
#define ZZAPP_STATUS_GENERATION    4u
#define ZZAPP_STATUS_FAULT         8u
#define ZZAPP_STATUS_EXIT          12u
#define ZZAPP_STATUS_FLAGS         16u
#define ZZAPP_STATUS_PC            20u
#define ZZAPP_STATUS_SP            24u
#define ZZAPP_STATUS_LR            28u
#define ZZAPP_STATUS_FAR           32u
#define ZZAPP_STATUS_FSR           36u
#define ZZAPP_STATUS_CPSR          40u
#define ZZAPP_STATUS_FLIP_GEN      44u
#define ZZAPP_STATUS_REPLY_BYTES   48u

#define ZZAPP_MAILBOX_PAYLOAD_BYTES 48u

#define ZZAPP_WINDOW_BYTES     0x00060000u   /* 384 KiB allocation */

#define ZZAPP_LIFE_OFF         0x00000000u
#define ZZAPP_LIFE_BYTES       0x00000100u

/* header at +0: head, tail, reserved0, reserved1 (16 bytes).
   events at +16. 256 * 16 = 4096. Region padded to 0x1100. */
#define ZZAPP_INPUT_OFF        0x00000100u
#define ZZAPP_INPUT_HDR        16u
#define ZZAPP_INPUT_CAP        256u
#define ZZAPP_EVENT_BYTES      16u
#define ZZAPP_INPUT_BYTES      0x00001100u   /* ends at 0x1200 */

/* header at +0: head, tail (8 bytes). records follow. */
#define ZZAPP_LOG_OFF          0x00001200u
#define ZZAPP_LOG_BYTES        0x00008000u   /* ends at 0x9200 */
#define ZZAPP_LOG_HDR          8u

#define ZZAPP_FILE_OFF         0x00009200u
#define ZZAPP_FILE_BYTES       64u
#define ZZAPP_PATH_OFF         0x00009240u
#define ZZAPP_PATH_BYTES       256u
#define ZZAPP_ARGV_OFF         0x00009340u
#define ZZAPP_ARGV_BYTES       2048u          /* ends at 0x9B40 */

#define ZZAPP_DATA_OFF         0x00009B40u
#define ZZAPP_DATA_BYTES       0x00040000u   /* 256 KiB, ends at 0x49B40 */
#define ZZAPP_STAGING_OFF      0x00049B40u
#define ZZAPP_STAGING_BYTES    0x00010000u   /* 64 KiB, ends at 0x59B40 */

/* Bytes from 0x59B40 to 0x60000 are reserved zero. */
#define ZZAPP_RESERVED_OFF     0x00059B40u

#define ZZAPP_LIFE_MAGIC       0x5A415050u   /* 'ZAPP' */
#define ZZAPP_LIFE_VERSION     1u

#define ZZAPP_LIFE_MAGIC_OFF       0x00u
#define ZZAPP_LIFE_VERSION_OFF     0x04u
#define ZZAPP_LIFE_STATE_OFF       0x08u
#define ZZAPP_LIFE_GENERATION_OFF  0x0Cu
#define ZZAPP_LIFE_HEARTBEAT_OFF   0x10u
#define ZZAPP_LIFE_STOP_OFF        0x14u
#define ZZAPP_LIFE_FAULT_OFF       0x18u
#define ZZAPP_LIFE_EXIT_OFF        0x1Cu
#define ZZAPP_LIFE_PC_OFF          0x20u
#define ZZAPP_LIFE_SP_OFF          0x24u
#define ZZAPP_LIFE_LR_OFF          0x28u
#define ZZAPP_LIFE_FAR_OFF         0x2Cu
#define ZZAPP_LIFE_FSR_OFF         0x30u
#define ZZAPP_LIFE_CPSR_OFF        0x34u
#define ZZAPP_LIFE_FLAGS_OFF       0x38u
#define ZZAPP_LIFE_INPUT_DROPS_OFF 0x3Cu
#define ZZAPP_LIFE_LOG_DROPS_OFF   0x40u
#define ZZAPP_LIFE_FLIP_GEN_OFF    0x44u
#define ZZAPP_LIFE_FLIP_ACK_OFF    0x48u
#define ZZAPP_LIFE_FLIP_SHOWING_OFF 0x4Cu
#define ZZAPP_LIFE_WIDTH_OFF       0x50u
#define ZZAPP_LIFE_HEIGHT_OFF      0x54u
#define ZZAPP_LIFE_PITCH_OFF       0x58u
#define ZZAPP_LIFE_FORMAT_OFF      0x5Cu
#define ZZAPP_LIFE_CARD_FRONT_OFF  0x60u
#define ZZAPP_LIFE_CARD_BACK_OFF   0x64u
#define ZZAPP_LIFE_PROGRESS_LO_OFF 0x68u
#define ZZAPP_LIFE_PROGRESS_HI_OFF 0x6Cu

/* State: 0 idle, 1 loading, 2 starting, 3 running, 4 paused (unused),
 * 5 stopping, 6 faulted. */
#define ZZAPP_ST_IDLE              0u
#define ZZAPP_ST_LOADING           1u
#define ZZAPP_ST_STARTING          2u
#define ZZAPP_ST_RUNNING           3u
#define ZZAPP_ST_PAUSED            4u
#define ZZAPP_ST_STOPPING          5u
#define ZZAPP_ST_FAULTED           6u

/* Fault: 0 none, 1 data abort, 2 prefetch, 3 undefined, 4 watchdog,
 * 5 forced stop (grace or reset), 6 watchdog that fell through to cold restart. */
#define ZZAPP_FAULT_NONE           0u
#define ZZAPP_FAULT_DATA           1u
#define ZZAPP_FAULT_PREFETCH       2u
#define ZZAPP_FAULT_UNDEF          3u
#define ZZAPP_FAULT_WATCHDOG       4u
#define ZZAPP_FAULT_FORCED         5u
#define ZZAPP_FAULT_WATCHDOG_RESTART 6u

/* Flags: bit0 display wanted, bit1 audio, bit2 GL,
 * bit8 display requested and not yet bound, bit9 vsync wanted. */
#define ZZAPP_F_DISPLAY            (1u << 0)
#define ZZAPP_F_AUDIO              (1u << 1)
#define ZZAPP_F_GL                 (1u << 2)
#define ZZAPP_F_DISPLAY_UNBOUND    (1u << 8)
#define ZZAPP_F_VSYNC              (1u << 9)

#define ZZAPP_EV_KEY_DOWN          1u
#define ZZAPP_EV_KEY_UP            2u
#define ZZAPP_EV_MOUSE             3u
#define ZZAPP_EV_JOY               4u
#define ZZAPP_EV_STOP              5u
#define ZZAPP_EV_RESIZE            6u

#define ZZAPP_FILE_OP_OPEN         1u
#define ZZAPP_FILE_OP_CLOSE        2u
#define ZZAPP_FILE_OP_READ         3u
#define ZZAPP_FILE_OP_WRITE        4u
#define ZZAPP_FILE_OP_SEEK         5u
#define ZZAPP_FILE_OP_STAT         6u
#define ZZAPP_FILE_OP_LIST_OPEN    7u
#define ZZAPP_FILE_OP_LIST_NEXT    8u
#define ZZAPP_FILE_OP_LIST_CLOSE   9u

#define ZZAPP_FILE_READ            1u
#define ZZAPP_FILE_WRITE           2u
#define ZZAPP_FILE_CREATE          4u
#define ZZAPP_SEEK_SET             0
#define ZZAPP_SEEK_CUR             1
#define ZZAPP_SEEK_END             2
#define ZZAPP_MODE_FILE            1u
#define ZZAPP_MODE_DIR             2u

/* FDs are small integers, 16 of them, starting at 3. */
#define ZZAPP_FD_BASE              3
#define ZZAPP_FD_COUNT             16
#define ZZAPP_PATH_MAX             255u

/* Argv area: u32 argc, then 16 offsets from the window base, then the
 * strings, each at most 127 bytes plus NUL. */
#define ZZAPP_ARGV_MAX             16u
#define ZZAPP_ARGV_STR_MAX         128u
#define ZZAPP_ARGV_ARGC_OFF        0u
#define ZZAPP_ARGV_OFFS_OFF        4u
#define ZZAPP_ARGV_STR_OFF         (4u + ZZAPP_ARGV_MAX * 4u)

struct zzapp_event {
    uint16_t type;
    uint16_t code;         /* Amiga rawkey, or button id */
    int16_t  dx;
    int16_t  dy;
    uint16_t buttons;      /* IEQUALIFIER on keys; mouse/joy buttons */
    uint16_t reserved;
    uint32_t time_ms;
};

struct zzapp_file_wire {   /* 64 bytes; do not add a field */
    uint32_t seq;          /* 0 */
    uint32_t op;           /* 4 */
    int32_t  fd;           /* 8 */
    int32_t  flags;        /* 12 */
    int32_t  whence;       /* 16: 0 set, 1 cur, 2 end */
    int32_t  err;          /* 20: 0 or a ZZ_* code */
    uint32_t path_off;     /* 24: offset from window base; 0 if none */
    uint32_t length;       /* 28 */
    uint32_t out_n;        /* 32 */
    uint32_t mode;         /* 36: stat, 1 file, 2 dir */
    uint64_t offset;       /* 40 */
    uint64_t out_pos;      /* 48: seek position; open/stat file size */
    uint32_t mtime;        /* 56: seconds, 0 if unknown */
    uint32_t reserved;     /* 60 */
};

/* Native layout documents offsets. Wire stores are big-endian; use the
 * accessors. Do not read these fields as host integers on a little-endian CPU. */
struct zzapp_life {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t generation;
    uint32_t heartbeat_ms;
    uint32_t stop;
    uint32_t fault_code;
    int32_t  exit_code;
    uint32_t pc;
    uint32_t sp;
    uint32_t lr;
    uint32_t far;
    uint32_t fsr;
    uint32_t cpsr;
    uint32_t flags;
    uint32_t input_drops;
    uint32_t log_drops;
    uint32_t flip_gen;
    uint32_t flip_ack;
    uint32_t flip_showing;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t card_front;
    uint32_t card_back;
    uint32_t progress_lo;
    uint32_t progress_hi;
};

struct zzapp_input_hdr {
    uint32_t head;
    uint32_t tail;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct zzapp_log_hdr {
    uint32_t head;             /* byte offset, monotonic */
    uint32_t tail;
};

_Static_assert(ZZAPP_QUERY_REPLY_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "query");
_Static_assert(ZZAPP_ALLOC_REPLY_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "alloc");
_Static_assert(ZZAPP_CHUNK_REQ_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "chunk");
_Static_assert(ZZAPP_START_REQ_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "start");
_Static_assert(ZZAPP_BIND_REQ_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "bind");
_Static_assert(ZZAPP_STATUS_REPLY_BYTES <= ZZAPP_MAILBOX_PAYLOAD_BYTES, "status");
_Static_assert(ZZAPP_STATUS_REPLY_BYTES == 48, "status fills the payload");

_Static_assert(ZZAPP_LIFE_OFF == 0u, "life");
_Static_assert(ZZAPP_LIFE_OFF + ZZAPP_LIFE_BYTES == ZZAPP_INPUT_OFF, "life end");
_Static_assert(ZZAPP_INPUT_OFF + ZZAPP_INPUT_BYTES == 0x1200u, "input end");
_Static_assert(ZZAPP_INPUT_HDR == 16u, "input hdr");
_Static_assert(ZZAPP_INPUT_HDR + ZZAPP_INPUT_CAP * ZZAPP_EVENT_BYTES == 0x1010u,
               "input events");
_Static_assert(ZZAPP_INPUT_BYTES == 0x1100u, "input region");
_Static_assert(ZZAPP_LOG_OFF == 0x1200u, "log");
_Static_assert(ZZAPP_LOG_OFF + ZZAPP_LOG_BYTES == 0x9200u, "log end");
_Static_assert(ZZAPP_FILE_OFF == 0x9200u, "file");
_Static_assert(ZZAPP_FILE_OFF + ZZAPP_FILE_BYTES == ZZAPP_PATH_OFF, "file end");
_Static_assert(ZZAPP_PATH_OFF + ZZAPP_PATH_BYTES == ZZAPP_ARGV_OFF, "path end");
_Static_assert(ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES == 0x9B40u, "argv end");
_Static_assert(ZZAPP_DATA_OFF == 0x9B40u, "data");
_Static_assert(ZZAPP_DATA_OFF + ZZAPP_DATA_BYTES == 0x49B40u, "data end");
_Static_assert(ZZAPP_STAGING_OFF == 0x49B40u, "staging");
_Static_assert(ZZAPP_STAGING_OFF + ZZAPP_STAGING_BYTES == 0x59B40u, "staging end");
_Static_assert(ZZAPP_STAGING_OFF + ZZAPP_STAGING_BYTES == ZZAPP_RESERVED_OFF,
               "reserved");
_Static_assert(ZZAPP_RESERVED_OFF < ZZAPP_WINDOW_BYTES, "window tail");
_Static_assert(ZZAPP_WINDOW_BYTES == 0x60000u, "window");
_Static_assert(ZZAPP_CHUNK_MAX == ZZAPP_STAGING_BYTES, "chunk fits staging");
_Static_assert(ZZAPP_ARGV_STR_OFF + ZZAPP_ARGV_MAX * ZZAPP_ARGV_STR_MAX
               <= ZZAPP_ARGV_BYTES, "argv strings");

_Static_assert(sizeof(struct zzapp_event) == ZZAPP_EVENT_BYTES, "event");
_Static_assert(offsetof(struct zzapp_event, type) == 0, "event type");
_Static_assert(offsetof(struct zzapp_event, code) == 2, "event code");
_Static_assert(offsetof(struct zzapp_event, dx) == 4, "event dx");
_Static_assert(offsetof(struct zzapp_event, dy) == 6, "event dy");
_Static_assert(offsetof(struct zzapp_event, buttons) == 8, "event buttons");
_Static_assert(offsetof(struct zzapp_event, reserved) == 10, "event reserved");
_Static_assert(offsetof(struct zzapp_event, time_ms) == 12, "event time");

_Static_assert(sizeof(struct zzapp_file_wire) == ZZAPP_FILE_BYTES, "file wire");
_Static_assert(offsetof(struct zzapp_file_wire, seq) == 0, "file seq");
_Static_assert(offsetof(struct zzapp_file_wire, op) == 4, "file op");
_Static_assert(offsetof(struct zzapp_file_wire, fd) == 8, "file fd");
_Static_assert(offsetof(struct zzapp_file_wire, flags) == 12, "file flags");
_Static_assert(offsetof(struct zzapp_file_wire, whence) == 16, "file whence");
_Static_assert(offsetof(struct zzapp_file_wire, err) == 20, "file err");
_Static_assert(offsetof(struct zzapp_file_wire, path_off) == 24, "file path");
_Static_assert(offsetof(struct zzapp_file_wire, length) == 28, "file length");
_Static_assert(offsetof(struct zzapp_file_wire, out_n) == 32, "file out_n");
_Static_assert(offsetof(struct zzapp_file_wire, mode) == 36, "file mode");
_Static_assert(offsetof(struct zzapp_file_wire, offset) == 40, "file offset");
_Static_assert(offsetof(struct zzapp_file_wire, out_pos) == 48, "file out_pos");
_Static_assert(offsetof(struct zzapp_file_wire, mtime) == 56, "file mtime");
_Static_assert(offsetof(struct zzapp_file_wire, reserved) == 60, "file reserved");

_Static_assert(sizeof(struct zzapp_input_hdr) == ZZAPP_INPUT_HDR, "input hdr");
_Static_assert(sizeof(struct zzapp_log_hdr) == ZZAPP_LOG_HDR, "log hdr");
_Static_assert(offsetof(struct zzapp_life, magic) == ZZAPP_LIFE_MAGIC_OFF, "magic");
_Static_assert(offsetof(struct zzapp_life, version) == ZZAPP_LIFE_VERSION_OFF, "ver");
_Static_assert(offsetof(struct zzapp_life, state) == ZZAPP_LIFE_STATE_OFF, "state");
_Static_assert(offsetof(struct zzapp_life, generation) == ZZAPP_LIFE_GENERATION_OFF, "gen");
_Static_assert(offsetof(struct zzapp_life, heartbeat_ms) == ZZAPP_LIFE_HEARTBEAT_OFF, "hb");
_Static_assert(offsetof(struct zzapp_life, stop) == ZZAPP_LIFE_STOP_OFF, "stop");
_Static_assert(offsetof(struct zzapp_life, fault_code) == ZZAPP_LIFE_FAULT_OFF, "fault");
_Static_assert(offsetof(struct zzapp_life, exit_code) == ZZAPP_LIFE_EXIT_OFF, "exit");
_Static_assert(offsetof(struct zzapp_life, pc) == ZZAPP_LIFE_PC_OFF, "pc");
_Static_assert(offsetof(struct zzapp_life, sp) == ZZAPP_LIFE_SP_OFF, "sp");
_Static_assert(offsetof(struct zzapp_life, lr) == ZZAPP_LIFE_LR_OFF, "lr");
_Static_assert(offsetof(struct zzapp_life, far) == ZZAPP_LIFE_FAR_OFF, "far");
_Static_assert(offsetof(struct zzapp_life, fsr) == ZZAPP_LIFE_FSR_OFF, "fsr");
_Static_assert(offsetof(struct zzapp_life, cpsr) == ZZAPP_LIFE_CPSR_OFF, "cpsr");
_Static_assert(offsetof(struct zzapp_life, flags) == ZZAPP_LIFE_FLAGS_OFF, "flags");
_Static_assert(offsetof(struct zzapp_life, input_drops) == ZZAPP_LIFE_INPUT_DROPS_OFF, "idrop");
_Static_assert(offsetof(struct zzapp_life, log_drops) == ZZAPP_LIFE_LOG_DROPS_OFF, "ldrop");
_Static_assert(offsetof(struct zzapp_life, flip_gen) == ZZAPP_LIFE_FLIP_GEN_OFF, "fgen");
_Static_assert(offsetof(struct zzapp_life, flip_ack) == ZZAPP_LIFE_FLIP_ACK_OFF, "fack");
_Static_assert(offsetof(struct zzapp_life, flip_showing) == ZZAPP_LIFE_FLIP_SHOWING_OFF, "fshow");
_Static_assert(offsetof(struct zzapp_life, width) == ZZAPP_LIFE_WIDTH_OFF, "width");
_Static_assert(offsetof(struct zzapp_life, height) == ZZAPP_LIFE_HEIGHT_OFF, "height");
_Static_assert(offsetof(struct zzapp_life, pitch) == ZZAPP_LIFE_PITCH_OFF, "pitch");
_Static_assert(offsetof(struct zzapp_life, format) == ZZAPP_LIFE_FORMAT_OFF, "format");
_Static_assert(offsetof(struct zzapp_life, card_front) == ZZAPP_LIFE_CARD_FRONT_OFF, "front");
_Static_assert(offsetof(struct zzapp_life, card_back) == ZZAPP_LIFE_CARD_BACK_OFF, "back");
_Static_assert(offsetof(struct zzapp_life, progress_lo) == ZZAPP_LIFE_PROGRESS_LO_OFF, "plo");
_Static_assert(offsetof(struct zzapp_life, progress_hi) == ZZAPP_LIFE_PROGRESS_HI_OFF, "phi");
_Static_assert(sizeof(struct zzapp_life) <= ZZAPP_LIFE_BYTES, "life bytes");

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

#endif /* ZZAPP_PROTO_H */
