/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The AAR transport of the app runtime (spec sections 6, 8.3, 9.6 and 10;
 * aar/transport.h). Per claim there is one control block and one queue pair in
 * host-visible memory, allocated at admission, plus service buffers allocated
 * on first use. All of it lives until the claim is released, which is what
 * lets the client drain messages and the final status after the app ended and
 * keeps a late client write harmless.
 *
 * This is the transport core: pure functions over a record's transport part
 * (zzapp_rec_xport_t) and an injected host-visible allocator. Nothing here
 * touches the scheduler, the mailbox, the clock or the services; the services
 * call it. Everything is testable on the host (test/zzapp/i).
 *
 * Ownership (design 3.4). Core 1, under the API lock, is the only agent that
 * produces on the SQ and consumes from the CQ: every post_*, pump, take_* and
 * abandon below is for core 1. Core 0 never produces on or consumes from a
 * queue. It binds the pair in zzapp_xport_admit() before the record is
 * published LIVE, so before core 1 can use it, and otherwise uses only
 * zzapp_xport_resolve(), zzapp_xport_kick(), the flag helpers and
 * zzapp_xport_release(). Release runs only once core 1 has finished with the
 * record (its retire reached RECLAIMED); in the reset-deadline recovery core 0
 * holds core 1 first. The control block words the
 * platform writes (state, end reason, fault record, accounting, flags,
 * progress, lease period) are written through zzapp_xport_cb_*, under the
 * services slot-state lock, so both cores may write them.
 *
 * Barriers. The queue functions are the reference model of aar/transport.h,
 * called with two hooks from the environment: `acquire` runs after an index
 * is read, `release` before one is written (including the stricter release
 * barrier before the consumer's head store). On the card both are the BSP
 * `dmb` (zzapp_xport_dmb); host tests may use counting hooks or none.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZAPP_XPORT_H
#define ZZAPP_XPORT_H

#include <stdint.h>

#include "aar/app.h"
#include "aar/transport.h"
#include "zzapp_proto.h"
#include "zzapp_rec.h"

/* ---- Sizes and layout --------------------------------------------------- */

/* Queue depth of every claim (AAR_QUEUE_DEPTH_DEFAULT). */
#define ZZAPP_XPORT_DEPTH          AAR_QUEUE_DEPTH_DEFAULT
/* The pair block: the control block, then the SQ ring, then the CQ ring
 * (AAR_PAIR_BYTES, 12 KiB used), in a 16 KiB allocation aligned to 8 KiB so a
 * cache-inhibited 68k page never straddles another block. */
#define ZZAPP_XPORT_PAIR_ALLOC     0x4000u
#define ZZAPP_XPORT_PAIR_ALIGN     0x2000u
/* Every block is a multiple of this and aligned to it. */
#define ZZAPP_XPORT_BLOCK_STEP     0x2000u

/* The service block, allocated on first use of any of its areas. */
#define ZZAPP_XPORT_SERVICE_BYTES  0xC000u /* 48 KiB */

typedef enum {
	ZZAPP_XPORT_AREA_SCRATCH = 0,  /* paths and names */
	ZZAPP_XPORT_AREA_CONSOLE_IN,   /* room of an AAR_OP_CONSOLE_READ */
	ZZAPP_XPORT_AREA_LOG,          /* ZZAPP_XPORT_LOG_SLOTS records */
	ZZAPP_XPORT_AREA_CONSOLE_OUT,  /* firmware-private ring, spans go out */
	ZZAPP_XPORT_AREA_PALETTE,      /* latest colours and the request's copy */
	ZZAPP_XPORT_AREA_COUNT
} zzapp_xport_area_t;

#define ZZAPP_XPORT_SCRATCH_OFF      0x0000u
#define ZZAPP_XPORT_SCRATCH_BYTES    0x0400u
#define ZZAPP_XPORT_CONSOLE_IN_OFF   0x1000u
#define ZZAPP_XPORT_CONSOLE_IN_BYTES 0x1000u
#define ZZAPP_XPORT_LOG_OFF          0x2000u
#define ZZAPP_XPORT_LOG_SLOT_BYTES   AAR_LOG_MAX /* 1 KiB */
#define ZZAPP_XPORT_LOG_BYTES        (ZZAPP_XPORT_LOG_SLOTS * ZZAPP_XPORT_LOG_SLOT_BYTES)
#define ZZAPP_XPORT_CONSOLE_OUT_OFF  0x4000u
#define ZZAPP_XPORT_CONSOLE_OUT_BYTES 0x8000u
/* Palette area: the shadow holds the latest colour of each of the 256 entries
 * (big-endian 0x00RRGGBB words); the stage is the copy a request in flight
 * names, written only while no palette request is outstanding. */
#define ZZAPP_XPORT_PALETTE_OFF      0x0400u
#define ZZAPP_XPORT_PALETTE_BYTES    0x0800u
#define ZZAPP_XPORT_PALETTE_ENTRIES  256u
#define ZZAPP_XPORT_PALETTE_STAGE    0x0400u

/* The data block of file reads and writes grows in steps to this size. */
#define ZZAPP_XPORT_DATA_MAX       0x00040000u /* 256 KiB */

/* Bytes of an audio ring of `frames` frames (S16LE stereo), rounded to the
 * block step. */
#define ZZAPP_XPORT_AUDIO_BYTES(frames) \
	((((AAR_AUDIO_RING_HDR_BYTES + (frames) * 4u) + \
	   (ZZAPP_XPORT_BLOCK_STEP - 1u)) / ZZAPP_XPORT_BLOCK_STEP) * \
	 ZZAPP_XPORT_BLOCK_STEP)

/* Record flags, zzapp_rec_xport_t.flags. */
#define ZZAPP_XPORT_F_PAIR_LIVE         (1u << 0) /* the pair is allocated */
#define ZZAPP_XPORT_F_QUARANTINED       (1u << 1) /* lease lost; shell held */
#define ZZAPP_XPORT_F_RELEASE_REQUESTED (1u << 2) /* core 1's retire frees */
#define ZZAPP_XPORT_F_CORRUPT           (1u << 3) /* a queue index was corrupt */

/* ---- Environment -------------------------------------------------------- */

typedef struct zzapp_xport_env {
	/*
	 * The host-visible allocator. alloc() returns 0 with the block's ARM
	 * pointer and card address, or -1 when nothing fits. `lo`/`hi` are the
	 * placement window (the cacheable part of the heap; both 0: anywhere).
	 * release() returns 0, or -1 for a block it does not know.
	 */
	int (*alloc)(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
		     uint32_t hi, void **ptr, uint32_t *card);
	int (*release)(void *ctx, void *ptr, uint32_t card);
	void *ctx;
	uint32_t lo, hi;
	/* A token is the card address less this (ADDR_ADJ on the card). */
	uint32_t token_adjust;
	/* Barrier hooks, NULL for none. */
	aar_barrier_fn acquire;
	aar_barrier_fn release_barrier;
} zzapp_xport_env_t;

/* Boot, once, and in tests. Copies `env`. Calling it again replaces the
 * environment; records keep their blocks. */
void zzapp_xport_init(const zzapp_xport_env_t *env);
const zzapp_xport_env_t *zzapp_xport_env(void);
/* The barrier the card build passes for both hooks (ARM `dmb ish`; a compiler
 * and full barrier on a host). */
void zzapp_xport_dmb(void);

/* ---- Buffers ------------------------------------------------------------ */

/* A buffer the client can reach: `ptr` is the ARM view, `token` the shared
 * buffer token of the block (what an entry's `buf` names), `off` the offset of
 * this buffer inside the block (the entry's `buf_off`) and `bytes` its size. */
typedef struct {
	void *ptr;
	uint32_t token;
	uint32_t off;
	uint32_t bytes;
} zzapp_xport_buf_t;

/* ---- Admission and release ---------------------------------------------- */

/*
 * Admission step 3, for a record that zzapp_rec_admit() has just made CLAIMED:
 * allocate the pair block, initialise the control block (aar_cb_init, magic
 * last, AAR_CB_STATE = AAR_STATE_CLAIMED, AAR_CB_SHARED_USED = the block),
 * bind both queues, then a release barrier. AAR_REASON_NONE (0), or
 * AAR_REASON_HOSTHEAP when no block fits; a refusal changes nothing. The
 * caller publishes the record LIVE afterwards.
 */
uint32_t zzapp_xport_admit(zzapp_rec_t *rec);
/* 1 when the record owns a pair. */
int zzapp_xport_live(const zzapp_rec_t *rec);
/*
 * Return every block of the record: data, audio, load, service, then the
 * pair last. Each pointer is cleared before its block is returned, so a
 * replayed release frees nothing twice. Idempotent. Only once core 1 has
 * finished with the record (its retire reached RECLAIMED), by core 1 or by
 * core 0; in the reset-deadline recovery core 0 calls it while core 1 is
 * held. 0, or -n when n blocks the allocator did not
 * know (they stay leaked; the record forgets them).
 */
int zzapp_xport_release(zzapp_rec_t *rec);

/*
 * Handle to record for the mailbox operations that name a claim (design
 * 3.14): the record when the handle is the live one of a record that owns a
 * pair. Otherwise NULL, with *status AAR_E_INVAL for handle 0 or generation 0
 * and AAR_E_STALE for the rest. The dispatcher answers either with
 * ZZAPP_ERR_STALE and changes nothing. `status` may be NULL.
 */
zzapp_rec_t *zzapp_xport_resolve(uint32_t handle, int *status);

/* ---- Doorbell (design 3.11) --------------------------------------------- */

/* Core 0, ZZAPP_OP_KICK: resolve the handle and OR `flags` into the record.
 * AAR_OK, AAR_E_INVAL for a bad handle word or an unknown flag, AAR_E_STALE.
 * The caller then wakes core 1. */
int zzapp_xport_kick(uint32_t handle, uint32_t flags);
/* Core 1: the bits rung since the last call, cleared. */
uint32_t zzapp_xport_kick_take(zzapp_rec_t *rec);
/* Core 1: 1 when there is something to pump: a kick bit, or a CQ that holds
 * entries. AAR_E_IO on corrupt indices. Does not consume the kick. */
int zzapp_xport_needs_pump(zzapp_rec_t *rec);

/* Flag word helpers (atomic, either core). */
void zzapp_xport_flag_set(zzapp_rec_t *rec, uint32_t flags);
void zzapp_xport_flag_clear(zzapp_rec_t *rec, uint32_t flags);
int zzapp_xport_flag_test(const zzapp_rec_t *rec, uint32_t flags);

/* ---- Blocks and shared accounting ---------------------------------------- */

/* The limit, in bytes, that the shared memory of a claim may reach (read from
 * AAR_CB_SHARED_MIB at LOAD_BEGIN); 0: not checked. It counts the runtime's
 * blocks and the app's own shared allocations (rec->visible.used). The pair is
 * allocated before any limit exists and is counted but never refused. */
void zzapp_xport_set_shared_limit(zzapp_rec_t *rec, uint32_t bytes);
/* Bytes of the runtime's blocks (the pair, service, data, audio and load). */
uint32_t zzapp_xport_shared_used(const zzapp_rec_t *rec);
/* Publish AAR_CB_SHARED_USED: the runtime's blocks plus rec->visible.used, the
 * app's own aar_shared_alloc blocks. The transport calls it after each of its
 * allocations and frees; zzapp_host.c calls it after each app alloc or free. */
void zzapp_xport_publish_shared(zzapp_rec_t *rec);
/* Publish AAR_CB_PRIVATE_COMMITTED from rec->proc.memory.committed. The host
 * calls it wherever the committed private memory changes: the load commit, a
 * heap grow or shrink, and the release at the end of the app. */
void zzapp_xport_publish_private(zzapp_rec_t *rec);

/*
 * Allocate the block of `kind` (not the pair) of `bytes` rounded up to the
 * block step, zero it and fill *out (off 0). Counted in the shared total.
 * AAR_E_NOMEM when the limit or the heap says no, AAR_E_BUSY when the block
 * already exists, AAR_E_INVAL for the pair, zero bytes or no live pair.
 */
int zzapp_xport_block_alloc(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind,
			    uint32_t bytes, zzapp_xport_buf_t *out);
/* Return the block of `kind` (not the pair). AAR_OK also when absent. */
int zzapp_xport_block_free(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind);
/* 1 and *out filled when the block exists, else 0. */
int zzapp_xport_block_get(const zzapp_rec_t *rec,
			  zzapp_xport_block_kind_t kind,
			  zzapp_xport_buf_t *out);
/* The data block, at least `bytes` (at most ZZAPP_XPORT_DATA_MAX): allocated
 * on first use at the request rounded to the step, grown by free and
 * allocate. A grown block loses its contents. */
int zzapp_xport_data_reserve(zzapp_rec_t *rec, uint32_t bytes,
			     zzapp_xport_buf_t *out);
/* An area of the service block; the block is allocated, zeroed, on the first
 * call for any area. AAR_E_NOMEM as above. */
int zzapp_xport_service_area(zzapp_rec_t *rec, zzapp_xport_area_t area,
			     zzapp_xport_buf_t *out);

/* ---- Control block words ------------------------------------------------ */

/* Big-endian volatile access to a word (or two, hi then lo) of the control
 * block. get returns 0 and put does nothing without a live pair. */
uint32_t zzapp_xport_cb_get(const zzapp_rec_t *rec, uint32_t off);
void zzapp_xport_cb_put(zzapp_rec_t *rec, uint32_t off, uint32_t value);
uint64_t zzapp_xport_cb_get64(const zzapp_rec_t *rec, uint32_t off_hi);
void zzapp_xport_cb_put64(zzapp_rec_t *rec, uint32_t off_hi, uint64_t value);
/* A word the client writes (stop request, lease counter, limits, events
 * dropped): the load, then the acquire barrier. */
uint32_t zzapp_xport_cb_read_client(const zzapp_rec_t *rec, uint32_t off);
/* Fault record fields (AAR_CB_FAULT_*), written first; no barrier. A32: the
 * high words are 0. */
void zzapp_xport_cb_set_fault(zzapp_rec_t *rec, uint32_t fault_class,
			      uint32_t syndrome, uint32_t pstate, uint32_t pc,
			      uint32_t sp, uint32_t lr, uint32_t addr);
/* Publish the lifecycle: the release barrier, then the end reason, then the
 * state, so a client that sees the state sees the fault record. */
void zzapp_xport_cb_set_state(zzapp_rec_t *rec, uint32_t state,
			      uint32_t end_reason);

/* ---- Requests (app side to client) -------------------------------------- */

/*
 * Post a request of the table entry `kind`. The id is assigned here (a
 * per-record counter from 1, never zero, never reused) and returned in *id;
 * `req->id` is ignored. `deadline` is stored for the owner's wait. Payload
 * bytes the request names must be written before this call. AAR_OK;
 * AAR_E_BUSY when the SQ is full (counted in AAR_CB_SQ_FULL; the caller
 * parks until zzapp_xport_sq_space() says 1 and posts again) or the entry is
 * not free; AAR_E_NOSPC when the ids are exhausted; AAR_E_IO on corrupt
 * indices.
 */
int zzapp_xport_post_request(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind,
			     const aar_request *req, uint64_t deadline,
			     uint32_t *id);
/* 1 when the SQ can take an entry now, 0 when full, AAR_E_IO when corrupt.
 * Does not count. */
int zzapp_xport_sq_space(zzapp_rec_t *rec);
/* zzapp_xport_req_state_t of an entry. */
int zzapp_xport_req_state(const zzapp_rec_t *rec, zzapp_xport_req_kind_t kind);
uint32_t zzapp_xport_req_id(const zzapp_rec_t *rec, zzapp_xport_req_kind_t kind);
uint64_t zzapp_xport_req_deadline(const zzapp_rec_t *rec,
				  zzapp_xport_req_kind_t kind);
/* The completion of an entry: 1 with *out filled and the entry FREE, 0 while
 * it is POSTED, AAR_E_INVAL when the entry is FREE. */
int zzapp_xport_take_completion(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind,
				aar_completion *out);
/* The owner gives up (timeout, stop): the entry becomes FREE at once and its
 * id is remembered, so the late completion is dropped and counted as late. */
void zzapp_xport_abandon(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind);

/* A message to the client: AAR_OK, AAR_E_BUSY when only the reserved entry is
 * left (spec: AARSendMessage answers BUSY), AAR_E_IO on corrupt indices. */
int zzapp_xport_post_msg(zzapp_rec_t *rec, const void *msg);
/* One log record of `len` bytes (1 to AAR_LOG_MAX, longer is truncated),
 * copied to a free slot of the log area and posted as AAR_OP_LOG. Lossy:
 * AAR_OK, or AAR_E_BUSY after the drop was counted (AAR_CB_LOG_DROPPED, and
 * AAR_CB_SQ_FULL when the SQ was the reason). AAR_E_NOMEM when the service
 * block cannot be allocated (also counted as a drop), AAR_E_IO on corrupt
 * indices. */
int zzapp_xport_post_log(zzapp_rec_t *rec, const void *data, uint32_t len);

/* ---- The pump and what it fills ----------------------------------------- */

/*
 * Consume the CQ (at most one ring of entries per call) and dispatch each:
 *   a completion whose id and op match a POSTED table entry: stored, DONE;
 *   a completion of an abandoned id: dropped, late_dropped;
 *   any other completion: dropped, unknown_dropped;
 *   AAR_OP_EVENT: into the event ring, or dropped (events_dropped) when it
 *     is full;
 *   AAR_OP_MSG: into the inbox. The client's credits keep the inbox from
 *     overflowing, so the pump never leaves a message in the CQ. A client
 *     that broke its credit and overflows the inbox loses the message
 *     (msgs_overflow), which does not count as consumed.
 * Returns the number of entries consumed, or AAR_E_IO on corrupt indices
 * (ZZAPP_XPORT_F_CORRUPT is set; the caller ends the wait or the app with
 * AAR_E_IO).
 */
int zzapp_xport_pump(zzapp_rec_t *rec);
/* Entries waiting in the CQ, or AAR_E_IO. */
int zzapp_xport_cq_pending(zzapp_rec_t *rec);

/* The oldest pumped event: 1 with *ev filled, 0 when none. */
int zzapp_xport_take_event(zzapp_rec_t *rec, aar_event *ev);
uint32_t zzapp_xport_event_pending(const zzapp_rec_t *rec);
/* The oldest pumped message (AAR_MSG_BYTES): 1 with *msg filled, 0 when none.
 * This is the moment a message counts as consumed: AAR_CB_MSG_CONSUMED
 * advances by one, which hands its credit back to the client. */
int zzapp_xport_take_msg(zzapp_rec_t *rec, void *msg);
uint32_t zzapp_xport_msg_pending(const zzapp_rec_t *rec);

const zzapp_xport_stats_t *zzapp_xport_stats(const zzapp_rec_t *rec);

#endif /* ZZAPP_XPORT_H */
