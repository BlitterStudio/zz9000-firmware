/*
 * Process records, handles, and the boot-time record arena.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Vocabulary (slice 5b):
 *   window  a wire-layer launcher window, ZZAPP_SLOT_COUNT of them.
 *   pid     process-record index, 0 .. capacity - 1. The same index always
 *           names the same record, stack, L1 table and table pages.
 *   handle  (generation << 16) | pid, built with aar_handle_make(). Never 0.
 *           Dies when the record is freed. Lookups by handle fail (NULL) once
 *           the generation moved on.
 *
 * One block of the card pool (class APP_ARENA, permanent, never in the
 * revocable tail) holds, in parallel areas indexed by pid, the L1 tables,
 * core-1 stacks, records, and the user API table pages, plus the shared L2
 * table pool. Records are type stable: they are never freed to an allocator,
 * so a lock-free reader on the other core sees a generation mismatch, not
 * freed memory.
 *
 * Locking. The handle table has no lock of its own. Every mutation
 * (admit, unadmit, free, set_state, reconcile) runs under the services
 * slot-state lock, or on core 0 with core 1 halted. Lock order: slot-state
 * lock, then the address-space lock; never the reverse.
 */
#ifndef ZZAPP_REC_H
#define ZZAPP_REC_H

#include <stdint.h>

#include "aar/app.h"
#include "aar/transport.h"
#include "card_pool.h"
#include "zzapp_as.h"
#include "zzapp_kernel.h"
#include "zzapp_runtime.h"

/* ---- Arena layout constants -------------------------------------------- */

/* Handles carry a 16-bit index; the table is bounded at 1024 records. */
#define ZZAPP_REC_MAX_APPS      1024U
#define ZZAPP_REC_MIN_APPS      8U
/* Sizing basis: permanent memory (less the system reserve) per record. It
 * equals the commit floor of one app (one image frame, one stack frame), so
 * the arena matches what commit admission can hold. */
#define ZZAPP_REC_APP_BASIS_BYTES (2U << 20)
/* Heap MiB are doubled into shared L2 tables (alias sections), rounded up. */
#define ZZAPP_REC_SHARED_TABLE_STEP 32U
#define ZZAPP_REC_ARENA_ALIGN   (1U << 20)

#define ZZAPP_REC_L1_BYTES      ZZAPP_AS_L1_BYTES   /* 16 KiB, 16 KiB aligned */
#define ZZAPP_REC_STACK_BYTES   ZZAPP_KERNEL_STACK_BYTES /* 32 KiB */
#define ZZAPP_REC_RECORD_BYTES  0x4000U             /* 16 KiB, 64 B aligned */
#define ZZAPP_REC_TABLES_BYTES  0x2000U             /* API page + GL page */
#define ZZAPP_REC_API_PAGE_OFFSET 0U
#define ZZAPP_REC_GL_PAGE_OFFSET  0x1000U

#define ZZAPP_PID_NONE          0xffffffffU
/* SHIM (window protocol, delete with zzapp_services.c's window code). */
#define ZZAPP_REC_NO_WINDOW     0xffffU

/* Record state. Core 0 and core 1 publish transitions with release stores. */
typedef enum {
	ZZAPP_REC_FREE = 0,
	/* Admitted, not yet published to the launcher; core 0 may still undo. */
	ZZAPP_REC_CLAIMED,
	ZZAPP_REC_LIVE,
	/* Retire started. Never goes back. */
	ZZAPP_REC_RETIRING,
	/* Resources returned, shell and handle held (quarantine). */
	ZZAPP_REC_RECLAIMED
} zzapp_rec_state_t;

/* Retire progress marker in the record, so a restarted core 1 resumes after
 * the last completed stage. Numbers follow the design's retire steps. */
typedef enum {
	ZZAPP_RETIRE_NONE = 0,
	/* 1: state RETIRING, thread off the run list. */
	ZZAPP_RETIRE_DETACHED,
	/* 2-3: address space switched away, pointers detached, host-visible
	 * blocks, display, audio, GL, sessions and staging returned. */
	ZZAPP_RETIRE_RESOURCES,
	/* 4: alias extents, L2 list, L2 reserve, L1, ASID returned
	 * (zzapp_rec_retire_tables). */
	ZZAPP_RETIRE_TABLES,
	/* 5: commit ledger released. */
	ZZAPP_RETIRE_COMMIT,
	/* 6: wire layer detached from the window. Step 7 is zzapp_rec_free(). */
	ZZAPP_RETIRE_WIRE
} zzapp_retire_stage_t;

/* ---- Record contents ---------------------------------------------------- */

/* 64 bytes at the head of the non-thread part of the record. The handle
 * table is the only authority on state and generation. */
typedef struct {
	uint32_t handle;         /* 0 while FREE */
	uint32_t pid;
	/* Global 24-bit nonzero serial, one per admission, replaces the old slot
	 * plus generation packing in card-pool owner ids. */
	uint32_t owner_serial;
	uint32_t launch_generation;
	uint32_t kstack_base;    /* lowest byte of this record's core-1 stack */
	uint32_t kstack_top;     /* one past the highest byte */
	uint32_t tables;         /* 8 KiB: user API page, user GL page */
	uint32_t l1;             /* this index's L1 table */
	uint16_t window;         /* SHIM: window protocol only; ZZAPP_REC_NO_WINDOW */
	uint8_t  retire_stage;   /* zzapp_retire_stage_t */
	uint8_t  reserved8;
	uint32_t flags;
	/* Owner serial before this admission, so an undone claim restores it. */
	uint32_t serial_prev;
	uint32_t reserved[5];
} zzapp_rec_hdr_t;

/* Core-0-visible block (was struct zzapp_coherent in zzapp_host.c). */
typedef struct {
	volatile uint32_t running;
	volatile uint32_t in_api;
	volatile uint32_t generation;
	volatile uint32_t fault;
	volatile uint64_t progress;
	volatile uint64_t tick_seen;
	volatile uint64_t core_heartbeat;
	volatile uint64_t heartbeat_seen;
	volatile uint32_t heartbeat_value;
	volatile uint32_t owner;
	volatile uint32_t reset_request;
	volatile uint32_t reset_ack;
} zzapp_rec_coherent_t;

/* Per-record words the kernel keeps (was zzapp_kernel_resources_t and the
 * slot-indexed arrays of zzapp_kernel_t). 64 bytes. */
typedef struct {
	zzapp_kernel_resources_t resources;
	uint32_t audio_low_water;
	uint32_t audio_starved;
	uint32_t audio_underruns;
	uint32_t tlb_instruction_refills;
	uint32_t tlb_data_refills;
	uint32_t api_wait_sequence;
	uint8_t api_waiting;
	uint8_t api_resume_waiting;
	/* Replace the 1U << slot report masks. */
	uint8_t perf_report_pending;
	uint8_t perf_report_force;
	uint32_t reserved[2];
} zzapp_rec_kernel_t;

/* Launcher-facing claim state that follows the app (was struct
 * zzapp_service_slot, less the window-only fields). 64 bytes. */
typedef struct {
	uint32_t wire_generation;
	uint32_t state;
	uint32_t memory_limit;
	uint32_t host_visible_limit;
	uint32_t load_progress;
	uint32_t load_step;
	uint32_t refusal_reason;
	uint32_t terminal_reason;
	uint32_t reserved[8];
} zzapp_rec_claim_t;

/* Host state of one app (was struct zzapp_host_slot in zzapp_host.c). */
typedef struct {
	uint8_t *mem;
	uint32_t card;
	uint32_t owner;
	uint32_t state;
	uint32_t entry;
	int image_loaded;
	uint32_t image_end;
	uint32_t stack_bytes;
	uint64_t stop_mark;
	uint64_t fault_mark;
	uint32_t exit_code;
	uint32_t heap_off;
	uint32_t heap_end;
	uint32_t heap_bump;
	uint32_t stack_canary;
	int faulted;
	/* Core 1 only: keep the hot SVC watchdog state out of the uncached
	 * lifecycle block. The timer IRQ samples it every 5 ms. */
	volatile uint32_t api_calls;
	volatile uint32_t in_api;
	volatile uint32_t service_due;
#if ZZAPP_PERF_COUNTERS
	uint8_t perf_svc_sampled;
#endif
	uint32_t seen_api_calls;
	uint64_t api_entered;
	uint64_t last_tick;
	volatile uint32_t watchdog_reason;
	uint32_t terminal_reason;
	volatile uint32_t stop_requested;
	/* 1 once event_poll has told the app about the current stop request. */
	volatile uint32_t stop_event_delivered;
	volatile uint32_t stop_pending;
	uint32_t pending_fault;
	uint32_t pending_pc;
	uint32_t pending_sp;
	uint32_t pending_lr;
	uint32_t pending_far;
	uint32_t pending_fsr;
	uint32_t pending_cpsr;
	uint32_t stop_thread_state;
	uint32_t stop_wake_reason;
	uint32_t stop_in_api;
	uint32_t stop_parked;
	uint32_t stop_wait_registered;
	uint32_t stop_svc_number;
	uint32_t stop_diagnostics_valid;
	volatile uint32_t low_memory_pending;
	volatile uint32_t low_memory_crossings;
	/* ZZAPP_EXIT_* : a normal exit's teardown is owed from the exit call
	 * until it ends. Core 1 writes, core 0 reads with acquire. It survives
	 * the loader's unbind, which clears the rest of this block. */
	volatile uint32_t exit_state;
	/* Core 1 only: the app is parked in an API wait or yield, so the stall
	 * watchdog must not count it. */
	volatile uint32_t api_parked;
} zzapp_rec_host_t;

#define ZZAPP_REC_VISIBLE_SLOTS 64U
/* One host-visible block (was struct zzapp_host_visible_block). */
typedef struct {
	void *ptr;
	uint32_t card;
	uint32_t alias;
	uint32_t bytes;
} zzapp_rec_visible_block_t;

typedef struct {
	zzapp_rec_visible_block_t block[ZZAPP_REC_VISIBLE_SLOTS];
	volatile uint32_t commit_active;
	uint32_t used;
} zzapp_rec_visible_t;

/*
 * Parts whose definitions stay private to their owners' files in 5b are
 * reserved as aligned byte blocks. The owner overlays its struct and proves
 * the fit with a static assert against the budget:
 *   services_runtime  struct zzapp_service_runtime (zzapp_services.c)
 *   loader            struct loader_slot (zzapp_loader.c)
 *   svc               SVC counters, entry stamp, class, last number, veneer
 *                     and table aliases, gl_client_state_t, GL telemetry
 *                     (zzapp_svc.c)
 *   decode            image sessions inline, audio session pointer
 *                     (zzapp_api.c)
 */
#define ZZAPP_REC_SERVICES_BYTES 1024U
#define ZZAPP_REC_LOADER_BYTES   384U
#define ZZAPP_REC_SVC_BYTES      1152U
#define ZZAPP_REC_DECODE_BYTES   64U

typedef struct {
	_Alignas(8) uint8_t bytes[ZZAPP_REC_SERVICES_BYTES];
} zzapp_rec_services_part_t;
typedef struct {
	_Alignas(8) uint8_t bytes[ZZAPP_REC_LOADER_BYTES];
} zzapp_rec_loader_part_t;
typedef struct {
	_Alignas(8) uint8_t bytes[ZZAPP_REC_SVC_BYTES];
} zzapp_rec_svc_part_t;
typedef struct {
	_Alignas(8) uint8_t bytes[ZZAPP_REC_DECODE_BYTES];
} zzapp_rec_decode_part_t;

/*
 * The AAR transport part (zzapp_xport.[ch] own its use). It holds, for one
 * claim: the blocks of host-visible memory it owns (the control block and
 * queue pair, the service block, the data block, the audio ring, the load
 * buffer), the two queue views, the request table, the message inbox, the
 * event ring, the doorbell word and the counters. Nothing in it is shared
 * with the client: the client sees only the pair and the buffers named in
 * requests. Pointers are ARM views and card addresses of the same blocks;
 * NULL means the block is not allocated.
 */
#define ZZAPP_XPORT_INBOX_ENTRIES AAR_MSG_CAPACITY      /* 63 */
#define ZZAPP_XPORT_EVENT_ENTRIES 64U
#define ZZAPP_XPORT_REQUESTS      3U
#define ZZAPP_XPORT_LOG_SLOTS     8U

typedef enum {
	ZZAPP_XPORT_BLOCK_PAIR = 0,
	ZZAPP_XPORT_BLOCK_SERVICE,
	ZZAPP_XPORT_BLOCK_DATA,
	ZZAPP_XPORT_BLOCK_AUDIO,
	ZZAPP_XPORT_BLOCK_LOAD,
	ZZAPP_XPORT_BLOCK_COUNT
} zzapp_xport_block_kind_t;

/* The request table entries: what may be in flight at once. */
typedef enum {
	ZZAPP_XPORT_REQ_BLOCKING = 0,  /* file, list, console read, display open... */
	ZZAPP_XPORT_REQ_FLIP,          /* an asynchronous flip */
	ZZAPP_XPORT_REQ_CONSOLE_OUT,   /* the console output stream */
	ZZAPP_XPORT_REQ_COUNT = ZZAPP_XPORT_REQUESTS
} zzapp_xport_req_kind_t;

typedef enum {
	ZZAPP_XPORT_REQ_FREE = 0,
	ZZAPP_XPORT_REQ_POSTED,        /* on the SQ, no completion yet */
	ZZAPP_XPORT_REQ_DONE           /* completion stored, not yet taken */
} zzapp_xport_req_state_t;

typedef struct {
	void *ptr;
	uint32_t card;
	uint32_t bytes;                /* size of the allocation */
} zzapp_xport_block_t;

typedef struct {
	uint32_t id;
	uint16_t op;
	uint8_t  state;                /* zzapp_xport_req_state_t */
	uint8_t  reserved8;
	int32_t  result;
	uint16_t flags;
	uint16_t reserved16;
	uint32_t out[AAR_ENTRY_OUTS];
	/* The id of the last request of this entry that its owner gave up on:
	 * a completion that arrives for it is dropped and counted as late. */
	uint32_t abandoned_id;
	uint32_t reserved32;
	uint64_t deadline;             /* owner's clock, opaque to the transport */
} zzapp_xport_req_t;

/* Counters, all free-running and written by core 1 only. */
typedef struct {
	uint32_t events_dropped;       /* event ring full */
	uint32_t msgs_overflow;        /* inbox full: a client broke its credit */
	uint32_t late_dropped;         /* completion of an abandoned request */
	uint32_t unknown_dropped;      /* completion that matches nothing */
	uint32_t sq_full;              /* a post found the SQ full */
	uint32_t log_dropped;          /* log record dropped, slot or SQ full */
	uint32_t io_errors;            /* corrupt indices seen */
	uint32_t pumped;               /* CQ entries consumed */
} zzapp_xport_stats_t;

typedef struct {
	zzapp_xport_block_t block[ZZAPP_XPORT_BLOCK_COUNT];
	aar_queue sq;                  /* view of the SQ in the pair */
	aar_queue cq;
	zzapp_xport_req_t req[ZZAPP_XPORT_REQUESTS];
	zzapp_xport_stats_t stats;
	/* ZZAPP_XPORT_F_*. Core 0 and core 1 use the atomic accessors. */
	volatile uint32_t flags;
	/* ZZAPP_KICK_* bits OR-ed in by the doorbell handler on core 0 and taken
	 * by core 1. */
	volatile uint32_t kick;
	uint32_t next_id;              /* next request id, 0 once exhausted */
	uint32_t shared_used;          /* bytes of the blocks above */
	uint32_t shared_limit;         /* bytes the blocks may reach; 0: unchecked */
	uint32_t msg_head, msg_count;  /* inbox: read index below 63, entries held */
	uint32_t msg_consumed;         /* messages taken out of the inbox */
	uint32_t ev_head, ev_tail;     /* event ring, free-running */
	/* The SQ index each log slot was posted at, and which slots hold a
	 * record the client may not have read yet. */
	uint32_t log_index[ZZAPP_XPORT_LOG_SLOTS];
	uint32_t log_busy;
	uint32_t reserved[3];
	uint8_t  inbox[ZZAPP_XPORT_INBOX_ENTRIES][AAR_MSG_BYTES];
	aar_event events[ZZAPP_XPORT_EVENT_ENTRIES];
} zzapp_rec_xport_t;

/*
 * The record. The kernel thread is first, so a kernel that walks threads by
 * base and stride (stride = ZZAPP_REC_RECORD_BYTES) sees the record area as
 * its thread array. The alias extents live in proc.as.
 */
typedef struct zzapp_rec {
	zzapp_thread_t thread;
	zzapp_rec_hdr_t hdr;
	zzapp_rec_coherent_t coh;
	zzapp_rec_kernel_t kernel;
	zzapp_rec_claim_t claim;
	zzapp_proc_t proc;
	zzapp_rec_host_t host;
	zzapp_rec_visible_t visible;
	zzapp_rec_services_part_t services;
	zzapp_rec_loader_part_t loader;
	zzapp_rec_svc_part_t svc;
	zzapp_rec_decode_part_t decode;
	zzapp_rec_xport_t xport;
} zzapp_rec_t;

_Static_assert(sizeof(zzapp_rec_hdr_t) == 64, "record header is one line");
_Static_assert(sizeof(zzapp_rec_coherent_t) == 64, "coherent block");
_Static_assert(sizeof(zzapp_rec_kernel_t) == 64, "kernel words");
_Static_assert(sizeof(zzapp_rec_claim_t) == 64, "claim words");
_Static_assert(sizeof(zzapp_rec_t) <= ZZAPP_REC_RECORD_BYTES,
	       "record exceeds its 16 KiB slot");
_Static_assert(ZZAPP_REC_RECORD_BYTES % 64U == 0U, "record stride alignment");
_Static_assert(ZZAPP_REC_STACK_BYTES == (32U << 10), "stack stride");

/* ---- Sizing ------------------------------------------------------------- */

typedef struct {
	uint32_t apps;         /* record capacity */
	uint32_t l2_template;  /* tables the kernel template may take */
	uint32_t l2_shared;    /* tables outside any reserve */
	uint32_t l2_total;     /* template + apps * 16 + shared */
	uint32_t as_bytes;     /* zzapp_as_pool_bytes(): template L1, L1s, L2s */
	uint32_t tables_off;   /* user table pages, 4 KiB aligned */
	uint32_t stack_off;    /* core-1 stacks */
	uint32_t record_off;   /* records, 64 B aligned */
	uint32_t bytes;        /* whole arena, rounded up to 1 MiB */
} zzapp_rec_layout_t;

/* Record capacity for the memory the card pool can hold permanently:
 * clamp((permanent - reserve) / BASIS, MIN_APPS, MAX_APPS). */
uint32_t zzapp_rec_apps_for(uint32_t permanent_bytes,
			    uint32_t system_reserve_bytes);
/* Layout for a record count and the host-visible heap size: shared tables are
 * 2 per heap MiB rounded up to a multiple of 32. apps is clamped to
 * MIN_APPS..MAX_APPS. */
void zzapp_rec_layout_for(uint32_t apps, uint32_t host_heap_bytes,
			  zzapp_rec_layout_t *out);

/* ---- Boot --------------------------------------------------------------- */

/*
 * Boot, once, after card_pool_setup_card() and before zzapp_as_init(): size
 * by memory, take one 1 MiB aligned block of class APP_ARENA from the pool,
 * and initialise the handle table and the address-space table pool. When the
 * block does not fit, retries with half the records down to MIN_APPS.
 * 0, or -1 when even MIN_APPS does not fit (no app can run). The arena is
 * never freed and survives Amiga reset.
 */
int zzapp_rec_arena_carve(card_pool_t *pool, uint32_t permanent_bytes,
			  uint32_t system_reserve_bytes,
			  uint32_t host_heap_bytes);
/* Initialise over caller-provided memory (`base` 16 KiB aligned, at least
 * layout->bytes, below 4 GiB because L1 and L2 addresses go into 32-bit
 * descriptors). For tests and for the carve itself. 0 or -1. */
int zzapp_rec_arena_init(void *base, const zzapp_rec_layout_t *layout);
/* Card address and layout of the live arena; layout NULL before init. */
uint32_t zzapp_rec_arena_base(void);
const zzapp_rec_layout_t *zzapp_rec_layout(void);
uint32_t zzapp_rec_capacity(void);

/* ---- Handles and records ----------------------------------------------- */

/* Hot path: pointer arithmetic on a known index. NULL when pid >= capacity. */
zzapp_rec_t *zzapp_rec_by_pid(uint32_t pid);
/* NULL for handle 0, generation 0, index out of range, FREE, or a generation
 * that is not the live one. */
zzapp_rec_t *zzapp_rec_by_handle(uint32_t handle);
/* The handle a record was admitted under; 0 for a FREE one. */
uint32_t zzapp_rec_handle(uint32_t pid);
/* 0 (FREE) for a pid outside the table. */
zzapp_rec_state_t zzapp_rec_state(uint32_t pid);
/* CLAIMED -> LIVE, any admitted state -> RETIRING -> RECLAIMED. FREE is only
 * reached through zzapp_rec_free()/zzapp_rec_unadmit(). 0, or -1 for a FREE
 * record, a request for FREE, or one that moves back from RETIRING. */
int zzapp_rec_set_state(zzapp_rec_t *rec, zzapp_rec_state_t state);

/* Kernel stack, user table pages and L1 of an index, valid for any pid below
 * capacity whatever its state. */
uint32_t zzapp_rec_stack_base(uint32_t pid);
uint32_t zzapp_rec_stack_top(uint32_t pid);
void *zzapp_rec_tables(uint32_t pid);

/* ---- Admission ---------------------------------------------------------- */

/*
 * Admission steps 3 and 4 of a claim: pop the oldest free index (FIFO),
 * clear the record (the transport part included: no block, empty queues),
 * and reserve ZZAPP_AS_L2_RESERVE tables. The pair is allocated afterwards by
 * zzapp_xport_admit().
 * 0 on success with *handle (and *rec when not NULL) set; AAR_REASON_MEMORY
 * when no record is free; AAR_REASON_TABLES when the reserve does not fit.
 * A refusal changes nothing the table can observe. The record is CLAIMED.
 */
uint32_t zzapp_rec_admit(uint32_t *handle, zzapp_rec_t **rec);
/* SHIM: the window form, for callers that still bind a window. It calls
 * zzapp_rec_admit() and records `window` in hdr.window. The macro below
 * picks it by argument count, so existing three-argument calls compile. */
uint32_t zzapp_rec_admit_window(uint32_t window, uint32_t *handle,
				zzapp_rec_t **rec);
#define ZZAPP_REC_ADMIT_PICK(a, b, c, name, ...) name
#define zzapp_rec_admit(...) \
	ZZAPP_REC_ADMIT_PICK(__VA_ARGS__, zzapp_rec_admit_window, \
			     zzapp_rec_admit, unused)(__VA_ARGS__)
/*
 * Undo a claim that failed before publication: the reserve returns, the index
 * goes back to the head of the free queue, and its generation, the queue and
 * every counter are bit-identical to before zzapp_rec_admit(). 0, or -1 for a
 * stale handle or a record that is not CLAIMED.
 */
int zzapp_rec_unadmit(uint32_t handle);

/* ---- Retire ------------------------------------------------------------- */

/* Record a completed retire stage. */
void zzapp_rec_set_stage(zzapp_rec_t *rec, zzapp_retire_stage_t stage);
/*
 * Retire stage 4, idempotent: destroy the address space (alias extents, L2
 * list, L1, ASID), return the L2 reserve, and mark the stage. Core 1, or
 * core 0 with core 1 halted.
 */
void zzapp_rec_retire_tables(zzapp_rec_t *rec);
/*
 * Retire step 7, idempotent: bump the generation, publish FREE, clear the
 * handle and append the index to the FIFO queue. Bumping first means no stale
 * handle can match even if the core halts before the state flips. The record
 * keeps its handle until FREE is published, so a replay with that handle
 * completes the free from whichever store the halt stopped after, including
 * the bumped generation without the state change. 0, or -1 for a handle of
 * another life.
 */
int zzapp_rec_free(uint32_t handle);
/*
 * Quarantine (design 3.10): advance the generation of an admitted record to
 * the next one, so no handle the client holds names it, and make that the
 * record's handle. Returns the new handle, or 0 for a NULL or FREE record. The
 * record keeps its state and every block; zzapp_rec_free() with the new handle
 * ends it (and bumps the generation again). Calling it twice yields two
 * different handles; only the last one is live. Same locking as free.
 */
uint32_t zzapp_rec_revoke_handle(zzapp_rec_t *rec);
/*
 * Core 1 is halted (recovery, or core 0 retiring everything). Rebuild the
 * FIFO queue and every count from the state bytes, and the table pool from
 * its ownership marks. A FREE index missing from the queue is appended in
 * ascending order; the order of the rest is kept. Idempotent. Returns the
 * number of records that are not FREE, which the caller walks with
 * zzapp_rec_state() and retires (replaying zzapp_rec_free() for a record
 * whose free was interrupted).
 */
uint32_t zzapp_rec_reconcile_halted(void);

typedef struct {
	uint32_t capacity;
	uint32_t free;
	uint32_t claimed;
	uint32_t live;
	uint32_t retiring;
	uint32_t reclaimed;
	uint32_t refused_memory;
	uint32_t refused_tables;
	uint32_t owner_serial;     /* last serial handed out */
} zzapp_rec_stats_t;
void zzapp_rec_stats(zzapp_rec_stats_t *out);

/* Every table field a refused or undone claim must leave unchanged. */
typedef struct {
	uint16_t gen[ZZAPP_REC_MAX_APPS];
	uint8_t state[ZZAPP_REC_MAX_APPS];
	uint16_t next[ZZAPP_REC_MAX_APPS];
	uint32_t head;
	uint32_t tail;
	uint32_t free_count;
	uint32_t owner_serial;
} zzapp_rec_table_snapshot_t;
void zzapp_rec_table_snapshot(zzapp_rec_table_snapshot_t *out);

#ifdef ZZAPP_HOST_TEST
/* Stop a staged operation after `step` of its ordered writes, as a core-1
 * halt would. 0 runs to the end. zzapp_rec_free(): 1 generation bumped,
 * 2 state FREE, 3 queued (the end). zzapp_rec_retire_tables(): 1 space
 * destroyed, 2 reserve released, then the stage is marked (the end). */
extern unsigned zzapp_rec_test_halt_step;
/* Overwrite one link of the free queue, to tear it for reconcile tests. */
void zzapp_rec_test_poke_next(uint32_t index, uint32_t next);
#endif

#endif
