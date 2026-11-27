/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The AAR transport of the app runtime. See zzapp_xport.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "zzapp_xport.h"

static zzapp_xport_env_t env;

/*
 * Profile rings. A ring is a debugging aid and few claims have one, so its
 * bookkeeping is a small static table, not a field of every record (which
 * would grow all of them). A slot belongs to one record from open to close;
 * core 1 owns the sampling fields.
 */
typedef struct {
	zzapp_rec_t *rec;     /* the owner, NULL when the slot is free */
	void *ptr;            /* ARM view of the ring */
	uint32_t card;
	uint32_t bytes;
	uint32_t mask;        /* samples in the ring, less one */
	uint32_t interval;    /* scheduler ticks per sample */
	uint32_t tick;        /* ticks since the last sample */
	uint32_t head;        /* samples written, free-running */
	uint32_t dropped;     /* ticks that recorded no sample */
} zzapp_profile_slot_t;

static zzapp_profile_slot_t profile_slots[ZZAPP_PROFILE_SLOTS];

_Static_assert(ZZAPP_PROFILE_RING_BYTES >= AAR_PROFILE_RING_MIN_BYTES &&
	       (ZZAPP_PROFILE_RING_BYTES & (ZZAPP_PROFILE_RING_BYTES - 1U)) == 0U,
	       "the ring is a power of two of at least the contract's minimum");
_Static_assert(ZZAPP_PROFILE_RING_BYTES % ZZAPP_XPORT_BLOCK_STEP == 0U,
	       "the ring is whole block steps");

void zzapp_xport_init(const zzapp_xport_env_t *e)
{
	if (e)
		env = *e;
	else
		memset(&env, 0, sizeof(env));
}

const zzapp_xport_env_t *zzapp_xport_env(void)
{
	return &env;
}

void zzapp_xport_dmb(void)
{
#if defined(__arm__) || defined(__aarch64__)
	__asm__ __volatile__("dmb ish" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static void run_release(void)
{
	aar_barrier_run(env.release_barrier);
}

static uint32_t token_of(uint32_t card)
{
	return card - env.token_adjust;
}

/* ---- Event counts -------------------------------------------------------- */

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
	       "the event words are swapped from a little-endian core");
_Static_assert(1u + ZZAPP_EVENT_GROUPS ==
	       ZZAPP_EVENT_BYTES / 4u, "event block layout");
_Static_assert(ZZAPP_EVENT_BLOCK_BYTES % ZZAPP_XPORT_BLOCK_STEP == 0u,
	       "the event block is whole block steps");

static volatile uint8_t *events_ptr;
static uint32_t events_card;

int zzapp_xport_events_init(void)
{
	void *ptr = NULL;
	uint32_t card = 0U;
	unsigned i;

	/* Whatever block there was belonged to a heap that is rebuilt. */
	/* The rings of a rebuilt heap are gone with it, and so are their
	 * records. */
	memset(profile_slots, 0, sizeof(profile_slots));
	events_ptr = NULL;
	events_card = 0U;
	if (!env.alloc ||
	    env.alloc(env.ctx, ZZAPP_EVENT_BLOCK_BYTES, ZZAPP_XPORT_BLOCK_STEP,
		      env.lo, env.hi, &ptr, &card) != 0 || !ptr)
		return -1;
	for (i = 0U; i < ZZAPP_EVENT_BYTES; i += 4U)
		aar_put_be32((volatile uint8_t *)ptr + i, 0U);
	run_release();
	events_card = card;
	events_ptr = (volatile uint8_t *)ptr;
	return 0;
}

uint32_t zzapp_xport_events_token(void)
{
	return events_ptr ? token_of(events_card) : 0U;
}

volatile uint8_t *zzapp_xport_events_block(void)
{
	return events_ptr;
}

/* Add 1 to a big-endian word. The core's exclusive load and store make it
 * atomic against the other core. */
static void be32_inc(volatile uint8_t *word)
{
	volatile uint32_t *w = (volatile uint32_t *)word;
	uint32_t old = __atomic_load_n(w, __ATOMIC_RELAXED);

	while (!__atomic_compare_exchange_n(
		       w, &old, __builtin_bswap32(__builtin_bswap32(old) + 1U),
		       1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
		;
}

void zzapp_xport_event_post(const zzapp_rec_t *rec)
{
	uint32_t group;

	if (!events_ptr || !rec)
		return;
	/* The store that made the news visible comes first. */
	zzapp_xport_dmb();
	group = (rec->hdr.handle & 0xffffU) / ZZAPP_EVENT_GROUP_RECORDS;
	if (group < ZZAPP_EVENT_GROUPS)
		be32_inc(events_ptr + 4U * (1U + group));
	be32_inc(events_ptr);
}

/* ---- Small helpers ------------------------------------------------------- */

static uint32_t round_step(uint32_t bytes)
{
	return (bytes + (ZZAPP_XPORT_BLOCK_STEP - 1U)) &
	       ~(ZZAPP_XPORT_BLOCK_STEP - 1U);
}

static int is_live(const zzapp_rec_t *rec)
{
	return rec && (__atomic_load_n(&rec->xport.flags, __ATOMIC_ACQUIRE) &
		       ZZAPP_XPORT_F_PAIR_LIVE) != 0U;
}

static volatile uint8_t *cb_base(const zzapp_rec_t *rec)
{
	return is_live(rec) ? (volatile uint8_t *)rec->xport.block[
		ZZAPP_XPORT_BLOCK_PAIR].ptr : NULL;
}

/* A queue view found the indices corrupt. */
static int io_error(zzapp_rec_t *rec)
{
	rec->xport.stats.io_errors++;
	zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_CORRUPT);
	return AAR_E_IO;
}

static void cb_put32(zzapp_rec_t *rec, uint32_t off, uint32_t value)
{
	volatile uint8_t *cb = cb_base(rec);

	if (cb)
		aar_put_be32(cb + off, value);
}

/* One aligned 32-bit store of a big-endian word, released: the words the
 * client reads as published state (the stack size, the profile rate and head)
 * are never written in pieces, and everything stored before them is visible
 * first. The byte order is the core's (little-endian, asserted above). */
static void be32_publish(volatile uint8_t *word, uint32_t value)
{
	__atomic_store_n((volatile uint32_t *)word, __builtin_bswap32(value),
			 __ATOMIC_RELEASE);
}

static void cb_publish32(zzapp_rec_t *rec, uint32_t off, uint32_t value)
{
	volatile uint8_t *cb = cb_base(rec);

	if (cb)
		be32_publish(cb + off, value);
}

/* AAR_CB_SHARED_USED is the runtime's blocks plus the app's own shared
 * allocations (rec->visible.used, kept by zzapp_host.c). */
void zzapp_xport_publish_shared(zzapp_rec_t *rec)
{
	uint64_t used;

	if (!rec)
		return;
	used = (uint64_t)rec->xport.shared_used + rec->visible.used;
	zzapp_xport_cb_put64(rec, AAR_CB_SHARED_USED_HI, used);
}

/* AAR_CB_PRIVATE_COMMITTED is the private memory the record holds
 * (rec->proc.memory.committed). It is published wherever that changes: the
 * load commit, a heap grow or shrink, and the release at the end. */
void zzapp_xport_publish_private(zzapp_rec_t *rec)
{
	if (!rec)
		return;
	zzapp_xport_cb_put64(rec, AAR_CB_PRIVATE_COMMITTED_HI,
			     rec->proc.memory.committed);
}

/* ---- Flags, doorbell ----------------------------------------------------- */

void zzapp_xport_flag_set(zzapp_rec_t *rec, uint32_t flags)
{
	if (rec)
		__atomic_fetch_or(&rec->xport.flags, flags, __ATOMIC_ACQ_REL);
}

void zzapp_xport_flag_clear(zzapp_rec_t *rec, uint32_t flags)
{
	if (rec)
		__atomic_fetch_and(&rec->xport.flags, ~flags, __ATOMIC_ACQ_REL);
}

int zzapp_xport_flag_test(const zzapp_rec_t *rec, uint32_t flags)
{
	return rec && (__atomic_load_n(&rec->xport.flags, __ATOMIC_ACQUIRE) &
		       flags) == flags;
}

int zzapp_xport_live(const zzapp_rec_t *rec)
{
	return is_live(rec);
}

zzapp_rec_t *zzapp_xport_resolve(uint32_t handle, int *status)
{
	zzapp_rec_t *rec = NULL;
	int rc = AAR_OK;

	if (handle == 0U || aar_handle_generation(handle) == 0U) {
		rc = AAR_E_INVAL;
	} else {
		rec = zzapp_rec_by_handle(handle);
		if (!rec || !is_live(rec)) {
			rec = NULL;
			rc = AAR_E_STALE;
		}
	}
	if (status)
		*status = rc;
	return rec;
}

int zzapp_xport_kick(uint32_t handle, uint32_t flags)
{
	int status;
	zzapp_rec_t *rec = zzapp_xport_resolve(handle, &status);

	if (!rec)
		return status;
	if ((flags & ~ZZAPP_KICK_KNOWN) != 0U)
		return AAR_E_INVAL;
	__atomic_fetch_or(&rec->xport.kick, flags, __ATOMIC_ACQ_REL);
	return AAR_OK;
}

uint32_t zzapp_xport_kick_take(zzapp_rec_t *rec)
{
	return rec ? __atomic_exchange_n(&rec->xport.kick, 0U, __ATOMIC_ACQ_REL)
		   : 0U;
}

int zzapp_xport_needs_pump(zzapp_rec_t *rec)
{
	int pending;

	if (!is_live(rec))
		return 0;
	if (__atomic_load_n(&rec->xport.kick, __ATOMIC_ACQUIRE) != 0U)
		return 1;
	pending = zzapp_xport_cq_pending(rec);
	return pending < 0 ? pending : pending > 0;
}

/* ---- Blocks -------------------------------------------------------------- */

void zzapp_xport_set_shared_limit(zzapp_rec_t *rec, uint32_t bytes)
{
	if (rec)
		rec->xport.shared_limit = bytes;
}

uint32_t zzapp_xport_shared_used(const zzapp_rec_t *rec)
{
	return rec ? rec->xport.shared_used : 0U;
}

/* Return one block. The pointer is forgotten first, so a replay never frees
 * a block twice or a block a new owner was handed. 0, or -1 when the
 * allocator did not know it. */
static int block_return(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind)
{
	zzapp_rec_xport_t *x = &rec->xport;
	zzapp_xport_block_t b = x->block[kind];

	if (!b.ptr)
		return 0;
	memset(&x->block[kind], 0, sizeof(x->block[kind]));
	x->shared_used = x->shared_used >= b.bytes ? x->shared_used - b.bytes
						    : 0U;
	if (kind != ZZAPP_XPORT_BLOCK_PAIR)
		zzapp_xport_publish_shared(rec);
	return env.release && env.release(env.ctx, b.ptr, b.card) == 0 ? 0 : -1;
}

static int block_get(const zzapp_rec_t *rec, zzapp_xport_block_kind_t kind,
		     zzapp_xport_buf_t *out)
{
	const zzapp_xport_block_t *b = &rec->xport.block[kind];

	if (!b->ptr)
		return 0;
	if (out) {
		out->ptr = b->ptr;
		out->token = token_of(b->card);
		out->off = 0U;
		out->bytes = b->bytes;
	}
	return 1;
}

static int block_take(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind,
		      uint32_t bytes, uint32_t align, int exempt)
{
	zzapp_rec_xport_t *x = &rec->xport;
	void *ptr = NULL;
	uint32_t card = 0U;

	if (!exempt && x->shared_limit != 0U &&
	    (bytes > x->shared_limit ||
	     x->shared_used + rec->visible.used > x->shared_limit - bytes))
		return AAR_E_NOMEM;
	if (!env.alloc ||
	    env.alloc(env.ctx, bytes, align, env.lo, env.hi, &ptr, &card) != 0 ||
	    !ptr)
		return AAR_E_NOMEM;
	memset(ptr, 0, bytes);
	x->block[kind].ptr = ptr;
	x->block[kind].card = card;
	x->block[kind].bytes = bytes;
	x->shared_used += bytes;
	return AAR_OK;
}

int zzapp_xport_block_alloc(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind,
			    uint32_t bytes, zzapp_xport_buf_t *out)
{
	uint32_t size;
	int rc;

	if (!is_live(rec) || kind == ZZAPP_XPORT_BLOCK_PAIR ||
	    (unsigned)kind >= ZZAPP_XPORT_BLOCK_COUNT || bytes == 0U ||
	    bytes > 0xffffe000U)
		return AAR_E_INVAL;
	if (rec->xport.block[kind].ptr)
		return AAR_E_BUSY;
	size = round_step(bytes);
	rc = block_take(rec, kind, size, ZZAPP_XPORT_BLOCK_STEP, 0);
	if (rc != AAR_OK)
		return rc;
	zzapp_xport_publish_shared(rec);
	block_get(rec, kind, out);
	return AAR_OK;
}

int zzapp_xport_block_free(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind)
{
	if (!is_live(rec) || kind == ZZAPP_XPORT_BLOCK_PAIR ||
	    (unsigned)kind >= ZZAPP_XPORT_BLOCK_COUNT)
		return AAR_E_INVAL;
	if ((kind == ZZAPP_XPORT_BLOCK_DATA &&
	     zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_DATA)) ||
	    (kind == ZZAPP_XPORT_BLOCK_AUDIO &&
	     zzapp_xport_buffer_held(rec, ZZAPP_XPORT_NAMES_AUDIO)))
		return AAR_OK;
	return block_return(rec, kind) == 0 ? AAR_OK : AAR_E_IO;
}

int zzapp_xport_block_get(const zzapp_rec_t *rec,
			  zzapp_xport_block_kind_t kind,
			  zzapp_xport_buf_t *out)
{
	if (!rec || (unsigned)kind >= ZZAPP_XPORT_BLOCK_COUNT)
		return 0;
	return block_get(rec, kind, out);
}

/* Core 1, the owner of the queues, asks whether a buffer is still held: it
 * takes what the client has completed so far first, so a late completion is
 * seen even when no other call would pump. Core 0 never pumps and uses
 * zzapp_xport_buffer_held() alone. */
static int buffer_held_pumped(zzapp_rec_t *rec, uint32_t names)
{
	if (!zzapp_xport_buffer_held(rec, names))
		return 0;
	(void)zzapp_xport_pump(rec);
	return zzapp_xport_buffer_held(rec, names);
}

int zzapp_xport_data_reserve(zzapp_rec_t *rec, uint32_t bytes,
			     zzapp_xport_buf_t *out)
{
	zzapp_xport_buf_t cur;
	int have;

	if (!is_live(rec) || bytes == 0U || bytes > ZZAPP_XPORT_DATA_MAX)
		return AAR_E_INVAL;
	have = block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &cur);
	/* An abandoned request may still be reading or writing the block: it
	 * is neither replaced nor reused, or the old FILE_WRITE writes the new
	 * bytes and a late FILE_READ overwrites them. */
	if (have && buffer_held_pumped(rec, ZZAPP_XPORT_NAMES_DATA))
		return AAR_E_BUSY;
	if (have && cur.bytes >= bytes) {
		if (out)
			*out = cur;
		return AAR_OK;
	}
	if (block_return(rec, ZZAPP_XPORT_BLOCK_DATA) != 0)
		return AAR_E_IO;
	return zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_DATA, bytes, out);
}

int zzapp_xport_service_area(zzapp_rec_t *rec, zzapp_xport_area_t area,
			     zzapp_xport_buf_t *out)
{
	static const uint32_t off[ZZAPP_XPORT_AREA_COUNT] = {
		ZZAPP_XPORT_SCRATCH_OFF, ZZAPP_XPORT_CONSOLE_IN_OFF,
		ZZAPP_XPORT_LOG_OFF, ZZAPP_XPORT_CONSOLE_OUT_OFF,
		ZZAPP_XPORT_PALETTE_OFF };
	static const uint32_t len[ZZAPP_XPORT_AREA_COUNT] = {
		ZZAPP_XPORT_SCRATCH_BYTES, ZZAPP_XPORT_CONSOLE_IN_BYTES,
		ZZAPP_XPORT_LOG_BYTES, ZZAPP_XPORT_CONSOLE_OUT_BYTES,
		ZZAPP_XPORT_PALETTE_BYTES };
	zzapp_xport_buf_t blk;
	int rc;

	if (!is_live(rec) || (unsigned)area >= ZZAPP_XPORT_AREA_COUNT)
		return AAR_E_INVAL;
	/* The paths and the console-read room are payloads of one request each:
	 * an abandoned one may still be read from, or written to, its area. */
	if ((area == ZZAPP_XPORT_AREA_SCRATCH ||
	     area == ZZAPP_XPORT_AREA_CONSOLE_IN) &&
	    buffer_held_pumped(rec, ZZAPP_XPORT_NAMES_AREA(area)))
		return AAR_E_BUSY;
	if (!block_get(rec, ZZAPP_XPORT_BLOCK_SERVICE, &blk)) {
		rc = zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_SERVICE,
					     ZZAPP_XPORT_SERVICE_BYTES, &blk);
		if (rc != AAR_OK)
			return rc;
	}
	if (out) {
		out->ptr = (uint8_t *)blk.ptr + off[area];
		out->token = blk.token;
		out->off = off[area];
		out->bytes = len[area];
	}
	return AAR_OK;
}

int zzapp_xport_buffer_held(const zzapp_rec_t *rec, uint32_t names)
{
	unsigned k;

	if (!rec)
		return 0;
	/* Hold even a completed read until the app copies it in its own
	 * address space. Core 0 only checks data ownership after handoff. */
	if ((names & ZZAPP_XPORT_NAMES_DATA) &&
	    rec->xport.req[ZZAPP_XPORT_REQ_FILE_ASYNC].state != ZZAPP_XPORT_REQ_FREE)
		return 1;
	for (k = 0U; k < ZZAPP_XPORT_REQUESTS; k++)
		if ((__atomic_load_n(&rec->xport.req[k].abandoned_names,
				     __ATOMIC_ACQUIRE) & names) != 0U)
			return 1;
	return 0;
}

/* ---- Admission and release ------------------------------------------------ */

uint32_t zzapp_xport_admit(zzapp_rec_t *rec)
{
	zzapp_rec_xport_t *x;
	volatile uint8_t *cb;
	uint8_t *ring;
	uint32_t token;

	if (!rec)
		return AAR_REASON_HOSTHEAP;
	x = &rec->xport;
	if (is_live(rec))
		return AAR_REASON_NONE;
	if (block_take(rec, ZZAPP_XPORT_BLOCK_PAIR, ZZAPP_XPORT_PAIR_ALLOC,
		       ZZAPP_XPORT_PAIR_ALIGN, 1) != AAR_OK)
		return AAR_REASON_HOSTHEAP;
	token = token_of(x->block[ZZAPP_XPORT_BLOCK_PAIR].card);
	ring = (uint8_t *)x->block[ZZAPP_XPORT_BLOCK_PAIR].ptr;
	cb = ring;
	if (aar_cb_init(cb, rec->hdr.handle, AAR_PROFILE_A32, ZZAPP_XPORT_DEPTH,
			token, AAR_PAIR_SQ_OFFSET, token,
			aar_pair_cq_offset(ZZAPP_XPORT_DEPTH)) != AAR_OK ||
	    aar_cb_queue_bind(&x->sq, cb, ring + AAR_PAIR_SQ_OFFSET,
			      AAR_QUEUE_SQ) != AAR_OK ||
	    aar_cb_queue_bind(&x->cq, cb,
			      ring + aar_pair_cq_offset(ZZAPP_XPORT_DEPTH),
			      AAR_QUEUE_CQ) != AAR_OK) {
		(void)block_return(rec, ZZAPP_XPORT_BLOCK_PAIR);
		memset(&x->sq, 0, sizeof(x->sq));
		memset(&x->cq, 0, sizeof(x->cq));
		return AAR_REASON_HOSTHEAP;
	}
	x->next_id = 1U;
	aar_put_be32(cb + AAR_CB_STATE, AAR_STATE_CLAIMED);
	aar_put_be32(cb + AAR_CB_SHARED_USED_LO, x->shared_used);
	/* The block is complete before the record, and with it the token, is
	 * published. */
	run_release();
	__atomic_fetch_or(&x->flags, ZZAPP_XPORT_F_PAIR_LIVE, __ATOMIC_RELEASE);
	return AAR_REASON_NONE;
}

int zzapp_xport_release(zzapp_rec_t *rec)
{
	static const zzapp_xport_block_kind_t order[] = {
		ZZAPP_XPORT_BLOCK_DATA, ZZAPP_XPORT_BLOCK_AUDIO,
		ZZAPP_XPORT_BLOCK_LOAD, ZZAPP_XPORT_BLOCK_SERVICE,
		ZZAPP_XPORT_BLOCK_PAIR };
	int lost = 0;
	unsigned i;

	if (!rec)
		return AAR_E_INVAL;
	/* The ring first: its grant is withdrawn while the pair still reads. */
	if (zzapp_xport_profile_close(rec) != 0)
		lost++;
	for (i = 0U; i < sizeof(order) / sizeof(order[0]); i++) {
		if (order[i] == ZZAPP_XPORT_BLOCK_PAIR) {
			/* Nothing touches the control block or the rings from
			 * here on. */
			zzapp_xport_flag_clear(rec, ZZAPP_XPORT_F_PAIR_LIVE);
			memset(&rec->xport.sq, 0, sizeof(rec->xport.sq));
			memset(&rec->xport.cq, 0, sizeof(rec->xport.cq));
		}
		if (block_return(rec, order[i]) != 0)
			lost++;
	}
	rec->xport.shared_used = 0U;
	return -lost;
}

/* ---- Control block words --------------------------------------------------- */

uint32_t zzapp_xport_cb_get(const zzapp_rec_t *rec, uint32_t off)
{
	volatile uint8_t *cb = cb_base(rec);

	return cb ? aar_get_be32(cb + off) : 0U;
}

void zzapp_xport_cb_put(zzapp_rec_t *rec, uint32_t off, uint32_t value)
{
	cb_put32(rec, off, value);
	if (off == AAR_CB_END_REASON && value != 0U && is_live(rec))
		zzapp_xport_event_post(rec);
}

uint64_t zzapp_xport_cb_get64(const zzapp_rec_t *rec, uint32_t off_hi)
{
	volatile uint8_t *cb = cb_base(rec);

	return cb ? aar_get_be64(cb + off_hi) : 0U;
}

void zzapp_xport_cb_put64(zzapp_rec_t *rec, uint32_t off_hi, uint64_t value)
{
	volatile uint8_t *cb = cb_base(rec);

	if (cb)
		aar_put_be64(cb + off_hi, value);
}

uint32_t zzapp_xport_cb_read_client(const zzapp_rec_t *rec, uint32_t off)
{
	uint32_t value = zzapp_xport_cb_get(rec, off);

	aar_barrier_run(env.acquire);
	return value;
}

void zzapp_xport_cb_set_fault(zzapp_rec_t *rec, uint32_t fault_class,
			      uint32_t syndrome, uint32_t pstate, uint32_t pc,
			      uint32_t sp, uint32_t lr, uint32_t addr)
{
	cb_put32(rec, AAR_CB_FAULT_CLASS, fault_class);
	cb_put32(rec, AAR_CB_FAULT_SYNDROME, syndrome);
	cb_put32(rec, AAR_CB_FAULT_PSTATE, pstate);
	cb_put32(rec, AAR_CB_FAULT_PC_HI, 0U);
	cb_put32(rec, AAR_CB_FAULT_PC_LO, pc);
	cb_put32(rec, AAR_CB_FAULT_SP_HI, 0U);
	cb_put32(rec, AAR_CB_FAULT_SP_LO, sp);
	cb_put32(rec, AAR_CB_FAULT_LR_HI, 0U);
	cb_put32(rec, AAR_CB_FAULT_LR_LO, lr);
	cb_put32(rec, AAR_CB_FAULT_ADDR_HI, 0U);
	cb_put32(rec, AAR_CB_FAULT_ADDR_LO, addr);
}

void zzapp_xport_cb_set_state(zzapp_rec_t *rec, uint32_t state,
			      uint32_t end_reason)
{
	if (!is_live(rec))
		return;
	run_release();
	cb_put32(rec, AAR_CB_END_REASON, end_reason);
	run_release();
	cb_put32(rec, AAR_CB_STATE, state);
	/* The state is the last store: a client that takes the news reads both. */
	if (end_reason != 0U)
		zzapp_xport_event_post(rec);
}

/* ---- Fault stack snapshot ---------------------------------------------------- */

void zzapp_xport_cb_set_stack(zzapp_rec_t *rec, uint32_t sp, const void *data,
			      uint32_t bytes)
{
	volatile uint8_t *cb = cb_base(rec);
	const uint8_t *src = (const uint8_t *)data;
	uint32_t i;

	if (!cb)
		return;
	/* The size goes first, so a reader that raced an earlier snapshot sees
	 * none rather than a size with the wrong bytes. */
	be32_publish(cb + AAR_CB_STACK_BYTES, 0U);
	if (!src || bytes == 0U) {
		aar_put_be32(cb + AAR_CB_STACK_SP_HI, 0U);
		aar_put_be32(cb + AAR_CB_STACK_SP_LO, 0U);
		return;
	}
	if (bytes > AAR_CB_STACK_MAX)
		bytes = AAR_CB_STACK_MAX;
	run_release();
	for (i = 0U; i < bytes; i++)
		cb[AAR_CB_STACK_DATA + i] = src[i];
	aar_put_be32(cb + AAR_CB_STACK_SP_HI, 0U);
	aar_put_be32(cb + AAR_CB_STACK_SP_LO, sp);
	/* The size is the last store: the bytes it names are in place. */
	run_release();
	be32_publish(cb + AAR_CB_STACK_BYTES, bytes);
}

/* ---- Profiler -------------------------------------------------------------------- */

static zzapp_profile_slot_t *profile_find(const zzapp_rec_t *rec)
{
	unsigned i;

	if (!rec)
		return NULL;
	for (i = 0U; i < ZZAPP_PROFILE_SLOTS; i++)
		if (__atomic_load_n(&profile_slots[i].rec, __ATOMIC_ACQUIRE) == rec)
			return &profile_slots[i];
	return NULL;
}

int zzapp_xport_profile_open(zzapp_rec_t *rec, uint32_t granted_hz,
			     uint32_t interval)
{
	zzapp_profile_slot_t *s = NULL;
	zzapp_rec_xport_t *x;
	void *ptr = NULL;
	uint32_t card = 0U;
	unsigned i;

	if (!is_live(rec) || granted_hz == 0U || interval == 0U)
		return AAR_E_INVAL;
	zzapp_xport_profile_close(rec);
	x = &rec->xport;
	if (x->shared_limit != 0U &&
	    (ZZAPP_PROFILE_RING_BYTES > x->shared_limit ||
	     x->shared_used + rec->visible.used >
		     x->shared_limit - ZZAPP_PROFILE_RING_BYTES))
		return AAR_E_NOMEM;
	for (i = 0U; i < ZZAPP_PROFILE_SLOTS && !s; i++) {
		zzapp_rec_t *none = NULL;

		if (__atomic_compare_exchange_n(&profile_slots[i].rec, &none, rec,
						0, __ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			s = &profile_slots[i];
	}
	if (!s)
		return AAR_E_NORESOURCE;
	if (!env.alloc ||
	    env.alloc(env.ctx, ZZAPP_PROFILE_RING_BYTES, ZZAPP_XPORT_BLOCK_STEP,
		      env.lo, env.hi, &ptr, &card) != 0 || !ptr) {
		__atomic_store_n(&s->rec, NULL, __ATOMIC_RELEASE);
		return AAR_E_NOMEM;
	}
	memset(ptr, 0, ZZAPP_PROFILE_RING_BYTES);
	/* The slot is ours until its rec word is released again. The block is
	 * recorded as soon as it exists, and `bytes` is the accounting marker:
	 * it is set only once shared_used counts the ring, so a close that finds
	 * a slot an interrupted open left behind frees what was allocated and
	 * returns only what was counted. */
	s->ptr = ptr;
	s->card = card;
	s->bytes = 0U;
	s->mask = ZZAPP_PROFILE_RING_BYTES / AAR_PROFILE_SAMPLE_BYTES - 1U;
	s->interval = interval;
	s->tick = 0U;
	s->head = 0U;
	s->dropped = 0U;
	x->shared_used += ZZAPP_PROFILE_RING_BYTES;
	s->bytes = ZZAPP_PROFILE_RING_BYTES;
	zzapp_xport_publish_shared(rec);
	cb_publish32(rec, AAR_CB_PROFILE_TOKEN, token_of(card));
	cb_publish32(rec, AAR_CB_PROFILE_BYTES, s->bytes);
	cb_publish32(rec, AAR_CB_PROFILE_DROPPED, 0U);
	cb_publish32(rec, AAR_CB_PROFILE_HEAD, 0U);
	/* The rate is the last word: a reader that sees it non-zero may use the
	 * token and the size. */
	run_release();
	cb_publish32(rec, AAR_CB_PROFILE_GRANTED_HZ, granted_hz);
	zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_PROFILE);
	return AAR_OK;
}

int zzapp_xport_profile_close(zzapp_rec_t *rec)
{
	zzapp_profile_slot_t *s = profile_find(rec);
	zzapp_rec_xport_t *x;
	void *ptr;
	uint32_t card;
	uint32_t bytes;
	int rc = 0;

	if (!s)
		return 0;
	x = &rec->xport;
	/* No more samples, then no grant (the rate first: a reader that finds it
	 * zero must not use the token), then the ring goes back. */
	zzapp_xport_flag_clear(rec, ZZAPP_XPORT_F_PROFILE);
	cb_publish32(rec, AAR_CB_PROFILE_GRANTED_HZ, 0U);
	cb_publish32(rec, AAR_CB_PROFILE_TOKEN, 0U);
	cb_publish32(rec, AAR_CB_PROFILE_BYTES, 0U);
	/* Detach what the slot owns once, before anything is returned: a halt
	 * after this point leaks the ring (as block_return does), it never frees
	 * it twice. The ownership word is left alone until the end. */
	ptr = s->ptr;
	card = s->card;
	bytes = s->bytes;
	s->ptr = NULL;
	s->card = 0U;
	s->bytes = 0U;
	s->mask = 0U;
	s->interval = 0U;
	s->tick = 0U;
	s->head = 0U;
	s->dropped = 0U;
	if (bytes != 0U) {
		x->shared_used = x->shared_used >= bytes ?
				 x->shared_used - bytes : 0U;
		zzapp_xport_publish_shared(rec);
	}
	if (ptr)
		rc = env.release && env.release(env.ctx, ptr, card) == 0 ? 0 : -1;
	__atomic_store_n(&s->rec, NULL, __ATOMIC_RELEASE);
	return rc;
}

void zzapp_xport_profile_sample(zzapp_rec_t *rec, int have_pc, uint32_t pc)
{
	zzapp_profile_slot_t *s;

	if (!zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_PROFILE))
		return;
	s = profile_find(rec);
	if (!s || ++s->tick < s->interval)
		return;
	s->tick = 0U;
	if (!have_pc) {
		s->dropped++;
		cb_publish32(rec, AAR_CB_PROFILE_DROPPED, s->dropped);
		return;
	}
	aar_put_be64((volatile uint8_t *)s->ptr +
		     (size_t)(s->head & s->mask) * AAR_PROFILE_SAMPLE_BYTES, pc);
	/* The sample is in place before the count that names it. */
	run_release();
	s->head++;
	cb_publish32(rec, AAR_CB_PROFILE_HEAD, s->head);
}

/* ---- Requests -------------------------------------------------------------- */

static void note_sq_full(zzapp_rec_t *rec)
{
	rec->xport.stats.sq_full++;
	cb_put32(rec, AAR_CB_SQ_FULL, rec->xport.stats.sq_full);
}

static void note_log_dropped(zzapp_rec_t *rec)
{
	rec->xport.stats.log_dropped++;
	cb_put32(rec, AAR_CB_LOG_DROPPED, rec->xport.stats.log_dropped);
}

int zzapp_xport_post_request(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind,
			     const aar_request *req, uint64_t deadline,
			     uint32_t *id)
{
	zzapp_rec_xport_t *x;
	zzapp_xport_req_t *r;
	volatile uint8_t *slot;
	aar_request out;
	int rc;

	if (!is_live(rec) || !req || (unsigned)kind >= ZZAPP_XPORT_REQUESTS)
		return AAR_E_INVAL;
	x = &rec->xport;
	r = &x->req[kind];
	/* The previous request of this entry was abandoned and the client has
	 * not completed it: what it names must not be reused yet. */
	if (__atomic_load_n(&r->abandoned_names, __ATOMIC_ACQUIRE) != 0U) {
		(void)zzapp_xport_pump(rec);
		if (__atomic_load_n(&r->abandoned_names, __ATOMIC_ACQUIRE) != 0U)
			return AAR_E_BUSY;
	}
	if (r->state != ZZAPP_XPORT_REQ_FREE)
		return AAR_E_BUSY;
	if (x->next_id == 0U)
		return AAR_E_NOSPC;
	rc = aar_queue_prepare(&x->sq, 0U, env.acquire, &slot);
	if (rc == AAR_E_IO)
		return io_error(rec);
	if (rc != AAR_OK) {
		note_sq_full(rec);
		return rc;
	}
	out = *req;
	out.id = x->next_id;
	aar_entry_put_request(slot, &out);
	r->id = out.id;
	r->op = req->op;
	r->result = 0;
	r->flags = 0U;
	memset(r->out, 0, sizeof(r->out));
	r->deadline = deadline;
	r->names = 0U;
	if (req->buf != 0U) {
		const zzapp_xport_block_t *d = &x->block[ZZAPP_XPORT_BLOCK_DATA];
		const zzapp_xport_block_t *a = &x->block[ZZAPP_XPORT_BLOCK_AUDIO];
		const zzapp_xport_block_t *s = &x->block[ZZAPP_XPORT_BLOCK_SERVICE];

		if (d->ptr && token_of(d->card) == req->buf)
			r->names |= ZZAPP_XPORT_NAMES_DATA;
		if (a->ptr && token_of(a->card) == req->buf)
			r->names |= ZZAPP_XPORT_NAMES_AUDIO;
		if (s->ptr && token_of(s->card) == req->buf) {
			/* The scratch area starts at offset 0. */
			if (req->buf_off < ZZAPP_XPORT_SCRATCH_OFF +
					   ZZAPP_XPORT_SCRATCH_BYTES)
				r->names |= ZZAPP_XPORT_NAMES_AREA(
					ZZAPP_XPORT_AREA_SCRATCH);
			if (req->buf_off >= ZZAPP_XPORT_CONSOLE_IN_OFF &&
			    req->buf_off < ZZAPP_XPORT_CONSOLE_IN_OFF +
					   ZZAPP_XPORT_CONSOLE_IN_BYTES)
				r->names |= ZZAPP_XPORT_NAMES_AREA(
					ZZAPP_XPORT_AREA_CONSOLE_IN);
		}
	}
	/* AUDIO_CLOSE names no buffer but tells the client to stop using the
	 * ring, so the ring is held until the client has answered it. */
	if (req->op == AAR_OP_AUDIO_CLOSE && x->block[ZZAPP_XPORT_BLOCK_AUDIO].ptr)
		r->names |= ZZAPP_XPORT_NAMES_AUDIO;
	r->state = ZZAPP_XPORT_REQ_POSTED;
	x->next_id++;
	aar_queue_commit(&x->sq, env.release_barrier);
	zzapp_xport_event_post(rec);
	if (id)
		*id = out.id;
	return AAR_OK;
}

int zzapp_xport_sq_space(zzapp_rec_t *rec)
{
	volatile uint8_t *slot;
	int rc;

	if (!is_live(rec))
		return AAR_E_INVAL;
	rc = aar_queue_prepare(&rec->xport.sq, 0U, env.acquire, &slot);
	if (rc == AAR_E_IO)
		return io_error(rec);
	return rc == AAR_OK;
}

int zzapp_xport_req_state(const zzapp_rec_t *rec, zzapp_xport_req_kind_t kind)
{
	if (!rec || (unsigned)kind >= ZZAPP_XPORT_REQUESTS)
		return AAR_E_INVAL;
	return rec->xport.req[kind].state;
}

uint32_t zzapp_xport_req_id(const zzapp_rec_t *rec, zzapp_xport_req_kind_t kind)
{
	if (!rec || (unsigned)kind >= ZZAPP_XPORT_REQUESTS)
		return 0U;
	return rec->xport.req[kind].id;
}

uint64_t zzapp_xport_req_deadline(const zzapp_rec_t *rec,
				  zzapp_xport_req_kind_t kind)
{
	if (!rec || (unsigned)kind >= ZZAPP_XPORT_REQUESTS)
		return 0U;
	return rec->xport.req[kind].deadline;
}

int zzapp_xport_take_completion(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind,
				aar_completion *out)
{
	zzapp_xport_req_t *r;

	if (!rec || (unsigned)kind >= ZZAPP_XPORT_REQUESTS)
		return AAR_E_INVAL;
	r = &rec->xport.req[kind];
	if (r->state == ZZAPP_XPORT_REQ_FREE)
		return AAR_E_INVAL;
	if (r->state != ZZAPP_XPORT_REQ_DONE)
		return 0;
	if (out) {
		out->op = r->op;
		out->flags = r->flags;
		out->id = r->id;
		out->result = r->result;
		memcpy(out->out, r->out, sizeof(out->out));
	}
	r->state = ZZAPP_XPORT_REQ_FREE;
	return 1;
}

void zzapp_xport_abandon(zzapp_rec_t *rec, zzapp_xport_req_kind_t kind)
{
	zzapp_xport_req_t *r;

	if (!rec || (unsigned)kind >= ZZAPP_XPORT_REQUESTS)
		return;
	r = &rec->xport.req[kind];
	if (r->state == ZZAPP_XPORT_REQ_FREE)
		return;
	r->abandoned_op = r->op;
	r->abandoned_id = r->id;
	__atomic_store_n(&r->abandoned_names,
			 r->state == ZZAPP_XPORT_REQ_POSTED ? r->names : 0U,
			 __ATOMIC_RELEASE);
	r->state = ZZAPP_XPORT_REQ_FREE;
}

/* ---- Messages and logs to the client --------------------------------------- */

int zzapp_xport_post_msg(zzapp_rec_t *rec, const void *msg)
{
	int rc;

	if (!is_live(rec) || !msg)
		return AAR_E_INVAL;
	rc = aar_queue_post_msg(&rec->xport.sq, msg, env.acquire,
				env.release_barrier);
	if (rc == AAR_OK)
		zzapp_xport_event_post(rec);
	return rc == AAR_E_IO ? io_error(rec) : rc;
}

/* A log slot is reusable once the SQ head has passed its entry. */
static int log_slot_free(const zzapp_rec_t *rec, uint32_t head, uint32_t slot)
{
	const zzapp_rec_xport_t *x = &rec->xport;

	return (x->log_busy & (1U << slot)) == 0U ||
	       (int32_t)(head - x->log_index[slot]) > 0;
}

int zzapp_xport_post_log(zzapp_rec_t *rec, const void *data, uint32_t len)
{
	zzapp_rec_xport_t *x;
	zzapp_xport_buf_t area;
	volatile uint8_t *slot;
	aar_request r;
	uint32_t head;
	uint32_t index;
	uint32_t i;
	int rc;

	if (!is_live(rec) || !data || len == 0U)
		return AAR_E_INVAL;
	x = &rec->xport;
	if (len > AAR_LOG_MAX)
		len = AAR_LOG_MAX;
	head = aar_get_be32(x->sq.head);
	for (i = 0U; i < ZZAPP_XPORT_LOG_SLOTS; i++)
		if (log_slot_free(rec, head, i))
			break;
	if (i == ZZAPP_XPORT_LOG_SLOTS) {
		note_log_dropped(rec);
		return AAR_E_BUSY;
	}
	rc = zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_LOG, &area);
	if (rc != AAR_OK) {
		note_log_dropped(rec);
		return rc;
	}
	rc = aar_queue_prepare(&x->sq, 0U, env.acquire, &slot);
	if (rc == AAR_E_IO)
		return io_error(rec);
	if (rc != AAR_OK) {
		note_sq_full(rec);
		note_log_dropped(rec);
		return rc;
	}
	memcpy((uint8_t *)area.ptr + (size_t)i * ZZAPP_XPORT_LOG_SLOT_BYTES,
	       data, len);
	memset(&r, 0, sizeof(r));
	r.op = AAR_OP_LOG;
	r.buf = area.token;
	r.buf_off = area.off + i * ZZAPP_XPORT_LOG_SLOT_BYTES;
	r.buf_len = len;
	aar_entry_put_request(slot, &r);
	index = aar_get_be32(x->sq.tail);
	x->log_index[i] = index;
	x->log_busy |= 1U << i;
	aar_queue_commit(&x->sq, env.release_barrier);
	zzapp_xport_event_post(rec);
	return AAR_OK;
}

/* ---- The pump ---------------------------------------------------------------- */

static void store_completion(zzapp_rec_t *rec, const aar_completion *c)
{
	zzapp_rec_xport_t *x = &rec->xport;
	unsigned k;

	for (k = 0U; k < ZZAPP_XPORT_REQUESTS; k++) {
		zzapp_xport_req_t *r = &x->req[k];

		if (r->state == ZZAPP_XPORT_REQ_POSTED && c->id != 0U &&
		    r->id == c->id && r->op == c->op) {
			r->result = c->result;
			r->flags = c->flags;
			memcpy(r->out, c->out, sizeof(r->out));
			r->state = ZZAPP_XPORT_REQ_DONE;
			return;
		}
	}
	for (k = 0U; k < ZZAPP_XPORT_REQUESTS; k++) {
		if (c->id != 0U && x->req[k].abandoned_id == c->id &&
		    x->req[k].abandoned_op == c->op) {
			__atomic_store_n(&x->req[k].abandoned_names, 0U,
					 __ATOMIC_RELEASE);
			x->stats.late_dropped++;
			return;
		}
	}
	x->stats.unknown_dropped++;
}

/* Hold one motion event while the ring is full. While buttons and code
 * match, keep the latest absolute position or sum relative motion. The
 * pending event enters the ring before newer events. Return zero when the
 * event cannot be held so the caller counts the drop. */
static int event_merge_mouse(zzapp_rec_xport_t *x, const aar_event *ev)
{
	int32_t dx, dy;

	if (ev->type != AAR_EV_MOUSE)
		return 0;
	if (!x->ev_mouse_valid) {
		x->ev_mouse = *ev;
		x->ev_mouse.reserved = 0U;
		x->ev_mouse_valid = 1U;
		return 1;
	}
	if (x->ev_mouse.buttons != ev->buttons || x->ev_mouse.code != ev->code)
		return 0;
	if (ev->buttons & AAR_MOUSE_ABSOLUTE) {
		x->ev_mouse.dx = ev->dx;
		x->ev_mouse.dy = ev->dy;
		x->ev_mouse.time_ms = ev->time_ms;
		return 1;
	}
	dx = (int32_t)x->ev_mouse.dx + ev->dx;
	dy = (int32_t)x->ev_mouse.dy + ev->dy;
	x->ev_mouse.dx = (int16_t)(dx > 32767 ? 32767 : dx < -32768 ? -32768 : dx);
	x->ev_mouse.dy = (int16_t)(dy > 32767 ? 32767 : dy < -32768 ? -32768 : dy);
	x->ev_mouse.time_ms = ev->time_ms;
	return 1;
}

static void event_flush_mouse(zzapp_rec_xport_t *x)
{
	if (x->ev_mouse_valid &&
	    x->ev_tail - x->ev_head < ZZAPP_XPORT_EVENT_ENTRIES) {
		x->events[x->ev_tail % ZZAPP_XPORT_EVENT_ENTRIES] = x->ev_mouse;
		x->ev_tail++;
		x->ev_mouse_valid = 0U;
	}
}

int zzapp_xport_pump(zzapp_rec_t *rec)
{
	zzapp_rec_xport_t *x;
	const volatile uint8_t *slot;
	int n = 0;

	if (!is_live(rec))
		return AAR_E_INVAL;
	x = &rec->xport;
	while ((uint32_t)n < x->cq.depth) {
		uint16_t op;
		int rc = aar_queue_peek(&x->cq, env.acquire, &slot);

		if (rc < 0)
			return io_error(rec);
		if (rc == 0)
			break;
		op = aar_get_be16(slot + AAR_ENTRY_OP);
		if (op == AAR_OP_MSG) {
			if (x->msg_count >= ZZAPP_XPORT_INBOX_ENTRIES) {
				x->stats.msgs_overflow++;
			} else {
				uint32_t at = (x->msg_head + x->msg_count) %
					      ZZAPP_XPORT_INBOX_ENTRIES;

				aar_entry_get_msg(slot, x->inbox[at]);
				x->msg_count++;
			}
		} else if (op == AAR_OP_EVENT) {
			aar_event ev;

			aar_entry_get_event(slot, &ev);
			event_flush_mouse(x);
			if (x->ev_tail - x->ev_head < ZZAPP_XPORT_EVENT_ENTRIES) {
				x->events[x->ev_tail %
					  ZZAPP_XPORT_EVENT_ENTRIES] = ev;
				x->ev_tail++;
			} else if (!event_merge_mouse(x, &ev)) {
				x->stats.events_dropped++;
			}
		} else {
			aar_completion c;

			aar_entry_get_completion(slot, &c);
			store_completion(rec, &c);
		}
		aar_queue_pop(&x->cq, env.release_barrier);
		x->stats.pumped++;
		n++;
	}
	return n;
}

int zzapp_xport_cq_pending(zzapp_rec_t *rec)
{
	uint32_t used;

	if (!is_live(rec))
		return AAR_E_INVAL;
	if (aar_queue_used(&rec->xport.cq, &used) != AAR_OK)
		return io_error(rec);
	return (int)used;
}

int zzapp_xport_take_event(zzapp_rec_t *rec, aar_event *ev)
{
	zzapp_rec_xport_t *x;

	if (!rec)
		return 0;
	x = &rec->xport;
	if (x->ev_tail == x->ev_head)
		return 0;
	if (ev)
		*ev = x->events[x->ev_head % ZZAPP_XPORT_EVENT_ENTRIES];
	x->ev_head++;
	event_flush_mouse(x);
	return 1;
}

uint32_t zzapp_xport_event_pending(const zzapp_rec_t *rec)
{
	return rec ? rec->xport.ev_tail - rec->xport.ev_head : 0U;
}

int zzapp_xport_take_msg(zzapp_rec_t *rec, void *msg)
{
	zzapp_rec_xport_t *x;

	if (!rec)
		return 0;
	x = &rec->xport;
	if (x->msg_count == 0U)
		return 0;
	if (msg)
		memcpy(msg, x->inbox[x->msg_head], AAR_MSG_BYTES);
	x->msg_head = (x->msg_head + 1U) % ZZAPP_XPORT_INBOX_ENTRIES;
	x->msg_count--;
	/* The credit goes back to the client only now, when the app has the
	 * message, not when the pump moved it out of the CQ. */
	x->msg_consumed++;
	cb_put32(rec, AAR_CB_MSG_CONSUMED, x->msg_consumed);
	return 1;
}

uint32_t zzapp_xport_msg_pending(const zzapp_rec_t *rec)
{
	return rec ? rec->xport.msg_count : 0U;
}

const zzapp_xport_stats_t *zzapp_xport_stats(const zzapp_rec_t *rec)
{
	return rec ? &rec->xport.stats : NULL;
}
