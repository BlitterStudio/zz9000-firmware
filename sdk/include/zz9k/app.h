/*
 * ZZ9000 ARM app-runtime client wire helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_APP_H
#define ZZ9K_APP_H

#include <stdint.h>
#include "zz9k/host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZZ9K_APP_ABI_MAJOR 0U
#define ZZ9K_APP_ABI_MINOR 3U

#define ZZ9K_APP_OP_QUERY        0x0c00U
#define ZZ9K_APP_OP_ALLOC_WINDOW 0x0c01U
#define ZZ9K_APP_OP_LOAD_BEGIN   0x0c02U
#define ZZ9K_APP_OP_LOAD_CHUNK   0x0c03U
#define ZZ9K_APP_OP_LOAD_END     0x0c04U
#define ZZ9K_APP_OP_START        0x0c05U
#define ZZ9K_APP_OP_STOP         0x0c06U
#define ZZ9K_APP_STOP_GENERATION 0U
#define ZZ9K_APP_STOP_REQ_BYTES  4U
#define ZZ9K_APP_OP_STATUS       0x0c07U

#define ZZ9K_APP_QUERY_REPLY_BYTES 16U
#define ZZ9K_APP_ALLOC_REPLY_BYTES 12U
#define ZZ9K_APP_QUERY_SLOTS 16U
#define ZZ9K_APP_QUERY_FREE_SLOTS 20U
#define ZZ9K_APP_QUERY_COMMIT_FREE 24U
#define ZZ9K_APP_QUERY_COMMIT_TOTAL 28U
#define ZZ9K_APP_QUERY_EXT_REPLY_BYTES 32U
#define ZZ9K_APP_ALLOC_SLOT 12U
#define ZZ9K_APP_ALLOC_GENERATION 16U
#define ZZ9K_APP_ALLOC_EXT_REPLY_BYTES 20U
#define ZZ9K_APP_MAILBOX_PAYLOAD_BYTES 48U
#define ZZ9K_APP_SEL_SLOT 40U
#define ZZ9K_APP_SEL_GENERATION 44U
#define ZZ9K_APP_STATUS_REPLY_BYTES 48U
#define ZZ9K_APP_CHUNK_MAX 65536U
#define ZZ9K_APP_IMAGE_MAX_BYTES 0x01000000U
#define ZZ9K_APP_WINDOW_BYTES 0x00094000U
#define ZZ9K_APP_STAGING_OFFSET 0x00049b40U
#define ZZ9K_APP_STAGING_BYTES 0x00010000U
#define ZZ9K_APP_ARGV_OFFSET 0x00009340U
#define ZZ9K_APP_ARGV_BYTES 2048U
#define ZZ9K_APP_ARGV_MAX 16U
#define ZZ9K_APP_ARGV_STRING_MAX 128U
#define ZZ9K_APP_ARGV_OFFSETS_OFFSET 4U
#define ZZ9K_APP_ARGV_STRINGS_OFFSET (4U + ZZ9K_APP_ARGV_MAX * 4U)
#define ZZ9K_APP_LIFE_OFFSET 0U
#define ZZ9K_APP_LIFE_BYTES 0x100U
#define ZZ9K_APP_LIFE_MAGIC 0x5a415050U
#define ZZ9K_APP_LIFE_VERSION 2U
#define ZZ9K_APP_LIFE_STATE_OFFSET 8U
#define ZZ9K_APP_LIFE_GENERATION_OFFSET 12U
#define ZZ9K_APP_LIFE_HEARTBEAT_OFFSET 16U
#define ZZ9K_APP_LIFE_STOP_OFFSET 20U
#define ZZ9K_APP_LIFE_PROGRESS_LO_OFFSET 0x68U
#define ZZ9K_APP_LIFE_PROGRESS_HI_OFFSET 0x6CU

#define ZZ9K_APP_MESSAGE_H2A_OFFSET 0x00092000U
#define ZZ9K_APP_MESSAGE_A2H_OFFSET 0x00093000U
#define ZZ9K_APP_MESSAGE_RING_BYTES 0x00001000U
#define ZZ9K_APP_MESSAGE_RING_HEADER_BYTES 16U
#define ZZ9K_APP_MESSAGE_BYTES 32U
#define ZZ9K_APP_MESSAGE_CAPACITY 64U

#define ZZ9K_APP_CAP_Z3 (1U << 0)
#define ZZ9K_APP_CAP_SLOTS (1U << 5)
#define ZZ9K_APP_CAP_LOAD_PROGRESS (1U << 7)
#define ZZ9K_APP_SLOT_COUNT 4U

#define ZZ9K_APP_STATE_IDLE 0U
#define ZZ9K_APP_STATE_LOADING 1U
#define ZZ9K_APP_STATE_STARTING 2U
#define ZZ9K_APP_STATE_RUNNING 3U
#define ZZ9K_APP_STATE_PAUSED 4U
#define ZZ9K_APP_STATE_STOPPING 5U
#define ZZ9K_APP_STATE_FAULTED 6U

#define ZZ9K_APP_FAULT_NONE 0U

typedef struct ZZ9KAppStatusInfo {
  uint32_t state;
  uint32_t generation;
  uint32_t fault_code;
  int32_t exit_code;
  uint32_t flags;
  uint32_t pc;
  uint32_t sp;
  uint32_t lr;
  uint32_t far;
  uint32_t fsr;
  uint32_t cpsr;
  uint32_t flip_generation;
} ZZ9KAppStatusInfo;

typedef struct ZZ9KAppMessage {
  uint8_t bytes[ZZ9K_APP_MESSAGE_BYTES];
} ZZ9KAppMessage;

typedef struct ZZ9KAppSlotRegistry {
  void *handles[ZZ9K_APP_SLOT_COUNT];
} ZZ9KAppSlotRegistry;

/* The resident library owns a generation only after LOAD_BEGIN succeeds.
 * Every live-window access fences itself against a replacement launcher. */
int zz9k_app_generation_matches(uint32_t owned_generation,
                                 uint32_t observed_generation);
int zz9k_app_slot_claim(ZZ9KAppSlotRegistry *registry, uint32_t slot,
                        void *handle);
void *zz9k_app_slot_get(const ZZ9KAppSlotRegistry *registry, uint32_t slot);
int zz9k_app_slot_release(ZZ9KAppSlotRegistry *registry, uint32_t slot,
                          const void *handle);
/* Replaces a stale owner after firmware assigns the slot to a new generation.
 * The former owner is returned so its handle can remain closeable. */
int zz9k_app_slot_replace(ZZ9KAppSlotRegistry *registry, uint32_t slot,
                          void *handle, void **previous);
int zz9k_app_request_init(ZZ9KRequest *request, uint16_t opcode);
int zz9k_app_request_stop(ZZ9KRequest *request, uint32_t generation);
/* Changes a legacy request into the 48-byte slot-addressed form. */
int zz9k_app_request_select(ZZ9KRequest *request, uint32_t slot,
                            uint32_t generation);
int zz9k_app_request_chunk(ZZ9KRequest *request, uint32_t file_offset,
                           uint32_t length);
int zz9k_app_request_start(ZZ9KRequest *request, uint32_t argc);
int zz9k_app_parse_status(const ZZ9KMailboxEntry *reply,
                          ZZ9KAppStatusInfo *status);
int zz9k_app_ring_indices(const volatile uint8_t *ring, uint32_t *head,
                          uint32_t *tail);
int zz9k_app_ring_push(volatile uint8_t *ring, const uint8_t message[32]);
int zz9k_app_ring_pop(volatile uint8_t *ring, uint8_t message[32]);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_APP_H */
