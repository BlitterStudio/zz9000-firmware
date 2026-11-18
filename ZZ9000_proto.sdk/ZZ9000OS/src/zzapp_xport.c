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

static void run_release(void)
{
	aar_barrier_run(env.release_barrier);
}

static uint32_t token_of(uint32_t card)
{
	return card - env.token_adjust;
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
		      uint32_t bytes, uint32_t align, int exempt,
		      uint32_t lo, uint32_t hi)
{
	zzapp_rec_xport_t *x = &rec->xport;
	void *ptr = NULL;
	uint32_t card = 0U;

	if (!exempt && x->shared_limit != 0U &&
	    (bytes > x->shared_limit ||
	     x->shared_used + rec->visible.used > x->shared_limit - bytes))
		return AAR_E_NOMEM;
	if (!env.alloc ||
	    env.alloc(env.ctx, bytes, align, lo, hi, &ptr, &card) != 0 ||
	    !ptr)
		return AAR_E_NOMEM;
	memset(ptr, 0, bytes);
	x->block[kind].ptr = ptr;
	x->block[kind].card = card;
	x->block[kind].bytes = bytes;
	x->shared_used += bytes;
	return AAR_OK;
}

int zzapp_xport_block_alloc_range(zzapp_rec_t *rec,
				  zzapp_xport_block_kind_t kind,
				  uint32_t bytes, uint32_t lo, uint32_t hi,
				  zzapp_xport_buf_t *out)
{
	uint32_t size;
	int rc;

	if (!is_live(rec) || kind == ZZAPP_XPORT_BLOCK_PAIR ||
	    (unsigned)kind >= ZZAPP_XPORT_BLOCK_COUNT || bytes == 0U ||
	    bytes > 0xffffe000U || ((lo | hi) != 0U && lo >= hi))
		return AAR_E_INVAL;
	if (rec->xport.block[kind].ptr)
		return AAR_E_BUSY;
	size = round_step(bytes);
	rc = block_take(rec, kind, size, ZZAPP_XPORT_BLOCK_STEP, 0, lo, hi);
	if (rc != AAR_OK)
		return rc;
	zzapp_xport_publish_shared(rec);
	block_get(rec, kind, out);
	return AAR_OK;
}

int zzapp_xport_block_alloc(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind,
			    uint32_t bytes, zzapp_xport_buf_t *out)
{
	return zzapp_xport_block_alloc_range(rec, kind, bytes, env.lo, env.hi,
					     out);
}

int zzapp_xport_block_free(zzapp_rec_t *rec, zzapp_xport_block_kind_t kind)
{
	if (!is_live(rec) || kind == ZZAPP_XPORT_BLOCK_PAIR ||
	    (unsigned)kind >= ZZAPP_XPORT_BLOCK_COUNT)
		return AAR_E_INVAL;
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

int zzapp_xport_data_reserve(zzapp_rec_t *rec, uint32_t bytes,
			     zzapp_xport_buf_t *out)
{
	zzapp_xport_buf_t cur;

	if (!is_live(rec) || bytes == 0U || bytes > ZZAPP_XPORT_DATA_MAX)
		return AAR_E_INVAL;
	if (block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &cur) && cur.bytes >= bytes) {
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
		       ZZAPP_XPORT_PAIR_ALIGN, 1, env.lo, env.hi) != AAR_OK)
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
	r->state = ZZAPP_XPORT_REQ_POSTED;
	x->next_id++;
	aar_queue_commit(&x->sq, env.release_barrier);
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
	r->abandoned_id = r->id;
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
		if (c->id != 0U && x->req[k].abandoned_id == c->id) {
			x->stats.late_dropped++;
			return;
		}
	}
	x->stats.unknown_dropped++;
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
			if (x->ev_tail - x->ev_head >= ZZAPP_XPORT_EVENT_ENTRIES) {
				x->stats.events_dropped++;
			} else {
				aar_entry_get_event(slot, &x->events[
					x->ev_tail %
					ZZAPP_XPORT_EVENT_ENTRIES]);
				x->ev_tail++;
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
