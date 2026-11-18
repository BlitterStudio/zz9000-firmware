/*
 * Queue coherence probe (AAR cutover, slice 2): firmware side.
 *
 * A temporary diagnostic, removed when the AAR transport (slice 6) replaces
 * it. Both ARM cores produce and consume 64-byte entries in a control block
 * and queue pair inside an SDK shared buffer; the Amiga reads and writes the
 * same memory through its own mapping and checks what it sees. The wire
 * contract is sdk/tools/zz9k-qprobe.h; the mailbox opcode is
 * SDK_OP_DIAG_QPROBE.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDK_QPROBE_H
#define SDK_QPROBE_H

#include <stdint.h>

#include "memorymap.h"
#include "../../../sdk/tools/zz9k-qprobe.h"

#ifdef SDK_QPROBE_HOST_TEST
/* The host test supplies SDK_QPROBE_STATE_BASE (storage for the state
 * window) and SDK_QPROBE_PTR (fake ARM address to host pointer). */
#include "sdk_qprobe_host.h"
#else
#define SDK_QPROBE_STATE_BASE ((void *)(uintptr_t)SDK_QPROBE_STATE_ADDRESS)
#define SDK_QPROBE_PTR(address) ((volatile void *)(uintptr_t)(address))
#endif

#define SDK_QPROBE_PONG_DEPTH 8U

/* SDKQProbeQueue.flags: each condition is counted once until it clears. */
#define SDK_QPROBE_F_DOWN_INDEX 1U	/* down head - tail beyond the ring */
#define SDK_QPROBE_F_UP_INDEX 2U	/* up head - tail beyond the ring */
#define SDK_QPROBE_F_FULL 4U		/* down ring found full */

/* The state window lives in the SCU-coherent task-queue region. The summary
 * is its first line: one bit per queue, set while the queue is attached. */
struct SDKQProbeSummary {
	volatile uint32_t core0_mask;	/* CORE0-owned queues, polled by core 0 */
	volatile uint32_t core1_mask;	/* CORE1-owned queues, polled by core 1 */
	uint32_t reserved[14];
};

struct SDKQProbeQueue {
	/* Core 0 clears `attached` to stop the owner touching the pair; core 1
	 * sets `busy` around every pass over a CORE1 queue (see sdk_qprobe.c). */
	volatile uint32_t attached;
	volatile uint32_t busy;
	uint32_t in_use;		/* core 0 only: the slot is taken */
	uint32_t owner;
	uint32_t client_id;
	uint32_t handle;
	uint32_t address;
	uint32_t flags;			/* SDK_QPROBE_F_*: conditions already counted */
	uint32_t down_produced;		/* == down head */
	uint32_t up_consumed;		/* == up tail */
	uint32_t last_complete;
	uint32_t pongs;
	uint32_t up_checked;
	uint32_t up_errors;
	uint32_t first_error;
	uint32_t first_error_seq;
	uint32_t ring_full;
	uint32_t passes;
	uint32_t pong_head;
	uint32_t pong_count;
	uint32_t pong_id[SDK_QPROBE_PONG_DEPTH];
} __attribute__((aligned(ZZ9K_QP_LINE_BYTES)));

struct SDKQProbeState {
	struct SDKQProbeSummary summary;
	struct SDKQProbeQueue queue[ZZ9K_QP_MAX_QUEUES];
};

/* A shared buffer as the mailbox found it for a handle. */
struct SDKQProbeBuffer {
	uint32_t handle;
	uint32_t address;
	uint32_t length;
	uint32_t flags;
	uint16_t owner_id;
};

/* Fills `out` for a live shared buffer; returns 0 for an unknown handle. */
typedef int (*sdk_qprobe_lookup_fn)(uint32_t handle,
                                    struct SDKQProbeBuffer *out);

#define SDK_QPROBE_STATE ((struct SDKQProbeState *)SDK_QPROBE_STATE_BASE)

/* Mailbox command handler. `payload` is the request payload (big-endian
 * words [cmd, a, b, c]); the reply words are written to `reply` (at most 48
 * bytes). Returns an SDK_STATUS_* code. */
uint16_t sdk_qprobe_command(uint16_t client_id, const uint8_t *payload,
                            uint16_t payload_len, uint8_t *reply,
                            uint16_t *reply_len,
                            sdk_qprobe_lookup_fn lookup);

/* One pass over every attached CORE0 queue (main loop) or CORE1 queue
 * (core-1 worker). Call only when the matching mask is nonzero. */
void sdk_qprobe_service_core0(void);
void sdk_qprobe_service_core1(void);

/* The empty case costs one load and a branch. */
static inline void sdk_qprobe_poll_core0(void)
{
	if (SDK_QPROBE_STATE->summary.core0_mask != 0U)
		sdk_qprobe_service_core0();
}

/* True while a CORE1 queue is attached: the worker must not sleep. */
static inline int sdk_qprobe_core1_active(void)
{
	return SDK_QPROBE_STATE->summary.core1_mask != 0U;
}

/* Teardown, core 0 only. Detach every queue on `handle`, and return 1 once
 * no core can touch the pair any more. Return 0 if core 1 did not leave in
 * time: the caller must not free the memory then. */
int sdk_qprobe_detach_handle(uint32_t handle);

/* Amiga reset: detach every queue (same return value). The first call, at
 * cold boot before core 1 starts, initialises the uninitialised window. */
int sdk_qprobe_reset(void);

/* Core 1 is halted (cold restart or fault recovery): its busy words are
 * stale and are cleared. Part of zzapp_recover_external_locks. */
void sdk_qprobe_core1_halted(void);

#endif
