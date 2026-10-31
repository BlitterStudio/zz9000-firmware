/*
 * Host stand-in for the ZZRun rings window. Offsets match section 4.
 * Integers on the window are big-endian. zzapp_event in memory here is native;
 * the push/pop helpers convert.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_MOCK_WINDOW_H
#define ZZAPP_MOCK_WINDOW_H

#include <stdint.h>

#include "zzapp.h"

#ifndef ZZAPP_WINDOW_BYTES
#define ZZAPP_WINDOW_BYTES     0x00060000u

#define ZZAPP_LIFE_OFF         0x00000000u
#define ZZAPP_LIFE_BYTES       0x00000100u

#define ZZAPP_INPUT_OFF        0x00000100u
#define ZZAPP_INPUT_HDR        16u
#define ZZAPP_INPUT_CAP        256u
#define ZZAPP_EVENT_BYTES      16u
#define ZZAPP_INPUT_BYTES      0x00001100u

#define ZZAPP_LOG_OFF          0x00001200u
#define ZZAPP_LOG_BYTES        0x00008000u

#define ZZAPP_FILE_OFF         0x00009200u
#define ZZAPP_FILE_BYTES       64u
#define ZZAPP_PATH_OFF         0x00009240u
#define ZZAPP_PATH_BYTES       256u
#define ZZAPP_ARGV_OFF         0x00009340u
#define ZZAPP_ARGV_BYTES       2048u

#define ZZAPP_DATA_OFF         0x00009B40u
#define ZZAPP_DATA_BYTES       0x00040000u
#define ZZAPP_STAGING_OFF      0x00049B40u
#define ZZAPP_STAGING_BYTES    0x00010000u
#endif

#ifndef ZZAPP_FILE_WIRE_DEFINED
#define ZZAPP_FILE_WIRE_DEFINED
struct zzapp_file_wire {
    uint32_t seq;
    uint32_t op;
    int32_t  fd;
    int32_t  flags;
    int32_t  whence;
    int32_t  err;
    uint32_t path_off;
    uint32_t length;
    uint32_t out_n;
    uint32_t mode;
    uint64_t offset;
    uint64_t out_pos;
    uint32_t mtime;
    uint32_t reserved;
};
#endif

#define ZZAPP_LIFE_MAGIC   0x00u
#define ZZAPP_LIFE_LAYOUT  0x04u
#define ZZAPP_LIFE_STATE   0x08u
#define ZZAPP_LIFE_GEN     0x0Cu
#define ZZAPP_LIFE_HEART   0x10u
#define ZZAPP_LIFE_STOP    0x14u
#define ZZAPP_LIFE_FAULT   0x18u
#define ZZAPP_LIFE_EXIT    0x1Cu
#define ZZAPP_LIFE_PC      0x20u
#define ZZAPP_LIFE_SP      0x24u
#define ZZAPP_LIFE_LR      0x28u
#define ZZAPP_LIFE_FAR     0x2Cu
#define ZZAPP_LIFE_FSR     0x30u
#define ZZAPP_LIFE_CPSR    0x34u
#define ZZAPP_LIFE_FLAGS   0x38u
#define ZZAPP_LIFE_IN_DROP 0x3Cu
#define ZZAPP_LIFE_LOG_DROP 0x40u
#define ZZAPP_LIFE_FLIP_GEN 0x44u
#define ZZAPP_LIFE_FLIP_ACK 0x48u
#define ZZAPP_LIFE_FLIP_SHOW 0x4Cu
#define ZZAPP_LIFE_WIDTH   0x50u
#define ZZAPP_LIFE_HEIGHT  0x54u
#define ZZAPP_LIFE_PITCH   0x58u
#define ZZAPP_LIFE_FORMAT  0x5Cu
#define ZZAPP_LIFE_FRONT   0x60u
#define ZZAPP_LIFE_BACK    0x64u

uint8_t *zzapp_mock_window_new(void);
void zzapp_mock_window_free(uint8_t *window);

uint32_t zzapp_be_u32(const uint8_t *p);
void zzapp_be_set_u32(uint8_t *p, uint32_t v);
uint16_t zzapp_be_u16(const uint8_t *p);
void zzapp_be_set_u16(uint8_t *p, uint16_t v);
uint64_t zzapp_be_u64(const uint8_t *p);
void zzapp_be_set_u64(uint8_t *p, uint64_t v);

uint32_t zzapp_life_get(const uint8_t *window, uint32_t field);
void zzapp_life_set(uint8_t *window, uint32_t field, uint32_t v);

/* 0 written, -1 dropped (log_drops incremented). */
int zzapp_log_write(uint8_t *window, const char *msg, uint32_t len);
/* Payload bytes copied, 0 if empty, -1 if cap is too small. */
int zzapp_log_read(uint8_t *window, char *buf, uint32_t cap);

/* 0 pushed, -1 dropped (input_drops incremented). Event is native. */
int zzapp_input_push(uint8_t *window, const zzapp_event *ev);
/* 1 popped, 0 empty. */
int zzapp_input_pop(uint8_t *window, zzapp_event *out);

/* Seqlock. load returns 0 on a stable even seq, -1 if it stays torn. */
void zzapp_file_store(uint8_t *window, const struct zzapp_file_wire *in);
int zzapp_file_load(const uint8_t *window, struct zzapp_file_wire *out);
void zzapp_file_complete(uint8_t *window, int32_t err, uint32_t out_n,
                         uint64_t out_pos, uint32_t mode, uint32_t mtime);

#endif
