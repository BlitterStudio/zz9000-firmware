/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The client side of the AAR transport on the ZZ9000. See aar_xport_zz9000.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "aar_xport_zz9000.h"
#include "zz9k/abi.h"

#define XPORT_STEP        0x2000u   /* blocks are 8 KiB aligned and sized */
#define XPORT_LIMIT_MIB   4095u

/* ---- Small helpers ------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/*
 * The audio ring's index words are little-endian and the app (ARM) reads or
 * writes each one with a single 32-bit access while the 68k writes or reads
 * the other. The 68k therefore accesses them as one aligned long too: four
 * byte accesses would show the app a half-written read_frame (0x00ff to
 * 0x0100 passes through 0x0000) and would show this side a half-written
 * write_frame, either of which looks like a ring that is overfull or read
 * past the writer. The offsets are multiples of 4 and the ring is mapped
 * 8 KiB aligned.
 */
static uint32_t swap32(uint32_t v)
{
	return (v << 24) | ((v & 0x0000ff00u) << 8) |
	       ((v >> 8) & 0x0000ff00u) | (v >> 24);
}

static uint32_t le32(const volatile uint8_t *p)
{
	uint32_t v = *(const volatile uint32_t *)(const volatile void *)p;

	return AAR_NATIVE_BIG_ENDIAN ? swap32(v) : v;
}

static void put_le32(volatile uint8_t *p, uint32_t v)
{
	*(volatile uint32_t *)(volatile void *)p =
		AAR_NATIVE_BIG_ENDIAN ? swap32(v) : v;
}

static uint32_t round_step(uint32_t bytes)
{
	return (bytes + (XPORT_STEP - 1u)) & ~(XPORT_STEP - 1u);
}

/* A mailbox status as an AAR error. */
static int status_to_aar(int status)
{
	switch (status) {
	case ZZ9K_STATUS_OK:          return AAR_OK;
	case ZZ9K_STATUS_BUSY:        return AAR_E_BUSY;
	case ZZ9K_STATUS_UNSUPPORTED: return AAR_E_NOSYS;
	case ZZ9K_STATUS_BAD_REQUEST: return AAR_E_INVAL;
	case ZZ9K_STATUS_BAD_HANDLE:  return AAR_E_STALE;
	case ZZ9K_STATUS_NO_MEMORY:   return AAR_E_NOMEM;
	case ZZ9K_STATUS_TIMEOUT:     return AAR_E_TIMEDOUT;
	case ZZ9K_STATUS_NOT_FOUND:   return AAR_E_NOENT;
	default:                      return AAR_E_IO;
	}
}

/* ---- Mailbox ------------------------------------------------------------- */

static int board_call(const aar_xport_board *b, uint16_t op,
		      const uint8_t *payload, uint32_t len, int long_op,
		      aar_xport_tick_fn tick, void *tick_user,
		      aar_xport_reply *reply)
{
	int status;

	memset(reply, 0, sizeof(*reply));
	if (!b || !b->call)
		return AAR_E_NOSYS;
	status = b->call(b->user, op, payload, len, long_op, tick, tick_user,
			 reply);
	return status_to_aar(status);
}

/* A refusal reply carries the AAR_REASON_* word and nothing else. */
static int refusal(aar_xport *x, int rc, const aar_xport_reply *reply)
{
	uint32_t why;

	if (rc == AAR_OK || rc == AAR_E_STALE || reply->len < 4u)
		return rc;
	why = be32(reply->payload);
	if (why == AAR_REASON_NONE || why > AAR_REASON_LIMIT)
		return rc;
	if (x)
		x->reason = why;
	return AAR_E_NORESOURCE;
}

/* The claim is gone or revoked: nothing of it may be touched again. */
static void detach(aar_xport *x)
{
	x->handle = 0u;
	x->cb = NULL;
	x->load = NULL;
	x->audio = NULL;
	x->load_begun = 0u;
	memset(x->maps, 0, sizeof(x->maps));
	x->map_next = 0u;
	x->mouse_pending = 0u;
	x->kick_pending = 0u;
	x->sq_blocked = 0u;
}

static int xport_tick(void *user)
{
	return aar_xport_lease_renew((aar_xport *)user) != AAR_OK;
}

/* One mailbox call on a claim. */
static int mbx(aar_xport *x, uint16_t op, uint8_t *payload, uint32_t len,
	       int long_op, aar_xport_reply *reply)
{
	aar_xport_reply local;
	int rc;

	if (!reply)
		reply = &local;
	rc = board_call(&x->board, op, payload, len, long_op,
			long_op ? xport_tick : NULL, x, reply);
	if (rc == AAR_E_STALE)
		detach(x);
	return rc;
}

static int need_live(const aar_xport *x)
{
	return x && x->handle != 0u && x->cb != NULL ? AAR_OK : AAR_E_STALE;
}

static void handle_request(const aar_xport *x,
			   uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES])
{
	memset(payload, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
	zzapp_build_handle(payload, x->handle);
}

/* ---- Query, list, administrative release -------------------------------- */

int aar_xport_query(const aar_xport_board *board, aar_xport_query_info *info)
{
	aar_xport_reply reply;
	const uint8_t *p = reply.payload;
	int rc;

	if (!info)
		return AAR_E_INVAL;
	memset(info, 0, sizeof(*info));
	rc = board_call(board, ZZAPP_OP_QUERY, NULL, 0u, 0, NULL, NULL, &reply);
	if (rc != AAR_OK)
		return rc;
	if (reply.len < ZZAPP_MAILBOX_PAYLOAD_BYTES)
		return AAR_E_NOSYS;
	info->abi_major = be32(p + ZZAPP_QUERY_ABI_MAJOR);
	info->abi_minor = be32(p + ZZAPP_QUERY_ABI_MINOR);
	info->state = be32(p + ZZAPP_QUERY_STATE);
	info->caps = be32(p + ZZAPP_QUERY_CAPS);
	info->hostheap_largest = be32(p + ZZAPP_QUERY_HOSTHEAP_LARGEST);
	info->queue_depth = be32(p + ZZAPP_QUERY_QUEUE_DEPTH);
	info->commit_free = be32(p + ZZAPP_QUERY_COMMIT_FREE);
	info->commit_total = be32(p + ZZAPP_QUERY_COMMIT_TOTAL);
	info->records_free = be32(p + ZZAPP_QUERY_RECORDS_FREE);
	info->tables_free = be32(p + ZZAPP_QUERY_TABLES_FREE);
	info->tables_total = be32(p + ZZAPP_QUERY_TABLES_TOTAL);
	info->hostheap_free = be32(p + ZZAPP_QUERY_HOSTHEAP_FREE);
	return AAR_OK;
}

int aar_xport_list(const aar_xport_board *board, uint32_t cursor,
		   aar_xport_list_info *info)
{
	uint8_t req[ZZAPP_LIST_CURSOR + 4u];
	aar_xport_reply reply;
	const uint8_t *p = reply.payload;
	uint32_t reasons;
	int rc;

	if (!info)
		return AAR_E_INVAL;
	memset(info, 0, sizeof(*info));
	put32(req, cursor);
	rc = board_call(board, ZZAPP_OP_LIST, req, sizeof(req), 0, NULL, NULL,
			&reply);
	if (rc != AAR_OK)
		return rc;
	if (reply.len < ZZAPP_LIST_REPLY_BYTES)
		return AAR_E_IO;
	info->handle = be32(p + ZZAPP_LIST_HANDLE);
	if (info->handle == 0u)
		return 0;
	info->state = be32(p + ZZAPP_LIST_STATE);
	info->committed = be32(p + ZZAPP_LIST_COMMITTED);
	info->index = be32(p + ZZAPP_LIST_INDEX);
	info->cpu_ms = be32(p + ZZAPP_LIST_CPU_MS);
	info->service_ms = be32(p + ZZAPP_LIST_SERVICE_MS);
	reasons = be32(p + ZZAPP_LIST_REASONS);
	info->block_reason = (uint8_t)(reasons >> 24);
	info->end_reason = (uint8_t)(reasons >> 16);
	info->flags = (uint16_t)reasons;
	memcpy(info->name, p + ZZAPP_LIST_NAME, ZZAPP_LIST_NAME_BYTES);
	info->name[ZZAPP_LIST_NAME_BYTES] = '\0';
	return 1;
}

int aar_xport_release_handle(const aar_xport_board *board, uint32_t handle)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_xport_reply reply;

	memset(req, 0, sizeof(req));
	zzapp_build_handle(req, handle);
	return board_call(board, ZZAPP_OP_RELEASE, req, sizeof(req), 0, NULL,
			  NULL, &reply);
}

/* ---- Mapping --------------------------------------------------------------- */

/* The pointer of [token + off, token + off + len), mapped through the board
 * once per block and extended when a later request needs more of it. The
 * caches never shrink: a cache-inhibited page stays so. */
static volatile uint8_t *map_range(aar_xport *x, uint32_t token, uint32_t off,
				   uint32_t len)
{
	uint32_t need, i, slot;
	volatile uint8_t *p;

	if (token == 0u || !x->board.map)
		return NULL;
	if (len == 0u)
		len = 1u;
	if (off > UINT32_MAX - len - (XPORT_STEP - 1u))
		return NULL;
	need = round_step(off + len);
	for (i = 0u; i < AAR_XPORT_MAPS; i++) {
		if (x->maps[i].ptr && x->maps[i].token == token) {
			if (x->maps[i].bytes >= need)
				return x->maps[i].ptr + off;
			slot = i;
			goto map_it;
		}
	}
	for (slot = 0u; slot < AAR_XPORT_MAPS && x->maps[slot].ptr; slot++)
		;
	if (slot == AAR_XPORT_MAPS) {
		slot = x->map_next;
		x->map_next = (x->map_next + 1u) % AAR_XPORT_MAPS;
	}
map_it:
	p = x->board.map(x->board.user, token, need);
	if (!p)
		return NULL;
	x->maps[slot].token = token;
	x->maps[slot].bytes = need;
	x->maps[slot].ptr = p;
	x->stats.maps++;
	return p + off;
}

volatile uint8_t *aar_xport_map(aar_xport *x, uint32_t token, uint32_t off,
				uint32_t len)
{
	if (need_live(x) != AAR_OK)
		return NULL;
	return map_range(x, token, off, len);
}

/* ---- The claim ---------------------------------------------------------------- */

static void cb_add(aar_xport *x, uint32_t off, uint32_t n)
{
	if (x->cb)
		aar_put_be32(x->cb + off, aar_get_be32(x->cb + off) + n);
}

static int bind_queue(aar_xport *x, uint32_t which)
{
	uint32_t desc = AAR_CB_Q_DESC(which);
	uint32_t token = aar_get_be32(x->cb + desc + AAR_QD_TOKEN);
	uint32_t off = aar_get_be32(x->cb + desc + AAR_QD_OFFSET);
	uint32_t depth = aar_get_be32(x->cb + desc + AAR_QD_DEPTH);
	volatile uint8_t *ring;

	if (!aar_queue_depth_valid(depth))
		return AAR_E_IO;
	ring = map_range(x, token, off, depth * AAR_ENTRY_BYTES);
	if (!ring)
		return AAR_E_IO;
	return aar_cb_queue_bind(which == AAR_QUEUE_SQ ? &x->sq : &x->cq,
				 (volatile void *)x->cb, (volatile void *)ring,
				 which);
}

int aar_xport_open(aar_xport *x, const aar_xport_board *board,
		   uint32_t *reason)
{
	aar_xport_query_info qi;
	aar_xport_reply reply;
	uint32_t token, bytes;
	int rc;

	if (reason)
		*reason = AAR_REASON_NONE;
	if (!x || !board)
		return AAR_E_INVAL;
	memset(x, 0, sizeof(*x));
	x->board = *board;
	rc = aar_xport_query(board, &qi);
	if (rc != AAR_OK)
		return rc;
	if (qi.abi_major != ZZAPP_QUERY_ABI_MAJOR || !(qi.caps & ZZAPP_CAP_Z3))
		return AAR_E_NOSYS;
	rc = board_call(board, ZZAPP_OP_CLAIM, NULL, 0u, 0, NULL, NULL, &reply);
	if (rc != AAR_OK) {
		rc = refusal(x, rc, &reply);
		if (reason)
			*reason = x->reason;
		return rc;
	}
	if (reply.len < ZZAPP_CLAIM_REPLY_BYTES)
		return AAR_E_IO;
	x->handle = be32(reply.payload + ZZAPP_CLAIM_HANDLE);
	token = be32(reply.payload + ZZAPP_CLAIM_CB_TOKEN);
	bytes = be32(reply.payload + ZZAPP_CLAIM_CB_BYTES);
	if (x->handle == 0u)
		return AAR_E_IO;
	rc = AAR_E_IO;
	if (token == 0u || bytes < AAR_CB_BYTES || bytes > 0x100000u)
		goto undo;
	x->cb_token = token;
	x->cb = map_range(x, token, 0u, bytes);
	if (!x->cb)
		goto undo;
	if (aar_cb_check(x->cb) != AAR_OK ||
	    aar_get_be32(x->cb + AAR_CB_HANDLE) != x->handle)
		goto undo;
	if (bind_queue(x, AAR_QUEUE_SQ) != AAR_OK ||
	    bind_queue(x, AAR_QUEUE_CQ) != AAR_OK)
		goto undo;
	aar_put_be32(x->cb + AAR_CB_CLIENT_PRESENT, 1u);
	x->lease = 0u;
	return AAR_OK;

undo:
	{
		uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];

		/* The claim exists on the card; give it back. */
		handle_request(x, req);
		(void)board_call(board, ZZAPP_OP_RELEASE, req, sizeof(req), 0,
				 NULL, NULL, &reply);
	}
	detach(x);
	return rc;
}

uint32_t aar_xport_reason(const aar_xport *x)
{
	return x ? x->reason : 0u;
}

uint32_t aar_xport_handle(const aar_xport *x)
{
	return x ? x->handle : 0u;
}

int aar_xport_live(const aar_xport *x)
{
	return need_live(x) == AAR_OK;
}

/* ---- Limits, load, start, stop, release ---------------------------------- */

int aar_xport_set_limits(aar_xport *x, uint32_t private_mib,
			 uint32_t shared_mib, uint32_t weight)
{
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (x->load_begun)
		return AAR_E_BUSY;
	if (private_mib > XPORT_LIMIT_MIB || shared_mib > XPORT_LIMIT_MIB)
		return AAR_E_INVAL;
	aar_put_be32(x->cb + AAR_CB_PRIVATE_MIB, private_mib);
	aar_put_be32(x->cb + AAR_CB_SHARED_MIB, shared_mib);
	aar_put_be32(x->cb + AAR_CB_WEIGHT, weight);
	return AAR_OK;
}

int aar_xport_load_begin(aar_xport *x, uint32_t client_caps,
			 volatile uint8_t **staging, uint32_t *chunk_max)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_xport_reply reply;
	uint32_t token, size;
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (x->load_begun || x->started)
		return AAR_E_BUSY;
	if (client_caps & ~ZZAPP_CLIENT_KNOWN)
		return AAR_E_INVAL;
	x->reason = AAR_REASON_NONE;
	memset(req, 0, sizeof(req));
	zzapp_build_load_begin_request(req, x->handle, client_caps);
	rc = mbx(x, ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), 1, &reply);
	if (rc != AAR_OK)
		return refusal(x, rc, &reply);
	if (reply.len < ZZAPP_LOAD_BEGIN_REPLY_BYTES)
		return AAR_E_IO;
	token = be32(reply.payload + ZZAPP_LOAD_REPLY_TOKEN);
	size = be32(reply.payload + ZZAPP_LOAD_REPLY_SIZE);
	x->chunk_max = be32(reply.payload + ZZAPP_LOAD_REPLY_CHUNK_MAX);
	x->argv_off = be32(reply.payload + ZZAPP_LOAD_REPLY_ARGV_OFF);
	if (x->chunk_max == 0u || x->chunk_max > ZZAPP_CHUNK_MAX ||
	    x->chunk_max > x->argv_off ||
	    x->argv_off > size || size - x->argv_off < ZZAPP_LOAD_ARGV_BYTES)
		return AAR_E_IO;
	x->load = map_range(x, token, 0u, size);
	if (!x->load)
		return AAR_E_IO;
	x->load_bytes = size;
	x->load_begun = 1u;
	if (staging)
		*staging = x->load;
	if (chunk_max)
		*chunk_max = x->chunk_max;
	return AAR_OK;
}

int aar_xport_load_chunk(aar_xport *x, uint32_t file_off, uint32_t len)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!x->load_begun || x->started)
		return AAR_E_INVAL;
	if (len < ZZAPP_CHUNK_MIN || len > x->chunk_max ||
	    file_off > ZZAPP_IMAGE_MAX_BYTES - len)
		return AAR_E_INVAL;
	memset(req, 0, sizeof(req));
	zzapp_build_load_chunk_request(req, x->handle, file_off, len);
	return mbx(x, ZZAPP_OP_LOAD_CHUNK, req, sizeof(req), 0, NULL);
}

int aar_xport_load_end(aar_xport *x)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_xport_reply reply;
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!x->load_begun || x->started)
		return AAR_E_INVAL;
	handle_request(x, req);
	rc = mbx(x, ZZAPP_OP_LOAD_END, req, sizeof(req), 1, &reply);
	return refusal(x, rc, &reply);
}

int aar_xport_load_memory(aar_xport *x, const void *data, uint32_t len,
			  uint32_t client_caps)
{
	const uint8_t *src = (const uint8_t *)data;
	volatile uint8_t *staging;
	uint32_t chunk_max, off = 0u, n, i;
	int rc;

	if (!data || len == 0u || len > ZZAPP_IMAGE_MAX_BYTES)
		return AAR_E_INVAL;
	rc = aar_xport_load_begin(x, client_caps, &staging, &chunk_max);
	while (rc == AAR_OK && off < len) {
		n = len - off < chunk_max ? len - off : chunk_max;
		for (i = 0u; i < n; i++)
			staging[i] = src[off + i];
		rc = aar_xport_load_chunk(x, off, n);
		off += n;
	}
	return rc == AAR_OK ? aar_xport_load_end(x) : rc;
}

int aar_xport_start(aar_xport *x, const char *const *argv, uint32_t argc)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	volatile uint8_t *area;
	uint32_t at = ZZAPP_ARGV_STR_OFF, i, n, c;
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!x->load_begun || x->started)
		return AAR_E_INVAL;
	if (argc > ZZAPP_ARGV_MAX || (argc != 0u && !argv))
		return AAR_E_INVAL;
	area = x->load + x->argv_off;
	for (i = 0u; i < ZZAPP_LOAD_ARGV_BYTES; i++)
		area[i] = 0u;
	for (i = 0u; i < argc; i++) {
		if (!argv[i])
			return AAR_E_INVAL;
		n = (uint32_t)strlen(argv[i]);
		if (n >= ZZAPP_ARGV_STR_MAX ||
		    at + n + 1u > ZZAPP_LOAD_ARGV_BYTES)
			return AAR_E_INVAL;
		/* Offsets are from the start of the load buffer. */
		aar_put_be32(area + ZZAPP_ARGV_OFFS_OFF + i * 4u,
			     x->argv_off + at);
		for (c = 0u; c <= n; c++)
			area[at + c] = (uint8_t)argv[i][c];
		at += n + 1u;
	}
	aar_put_be32(area + ZZAPP_ARGV_ARGC_OFF, argc);
	memset(req, 0, sizeof(req));
	zzapp_build_start_app_request(req, x->handle, argc);
	rc = mbx(x, ZZAPP_OP_START, req, sizeof(req), 1, NULL);
	if (rc == AAR_OK) {
		/* The card frees the load buffer after it copied argv. */
		x->started = 1u;
		x->load_begun = 0u;
		x->load = NULL;
	}
	return rc;
}

void aar_xport_request_stop(aar_xport *x)
{
	if (need_live(x) == AAR_OK)
		aar_put_be32(x->cb + AAR_CB_STOP_REQUEST, 1u);
}

int aar_xport_stop(aar_xport *x)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	handle_request(x, req);
	return mbx(x, ZZAPP_OP_STOP, req, sizeof(req), 0, NULL);
}

int aar_xport_release(aar_xport *x)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	aar_xport_reply reply;
	int rc;

	if (!x)
		return AAR_E_INVAL;
	if (x->handle == 0u)
		return AAR_OK;
	/* Drop every pointer first: the card frees the memory behind them. */
	handle_request(x, req);
	detach(x);
	rc = board_call(&x->board, ZZAPP_OP_RELEASE, req, sizeof(req), 0, NULL,
			NULL, &reply);
	/* A handle the card no longer knows is released as far as we care. */
	return rc == AAR_E_STALE ? AAR_OK : rc;
}

/* ---- Status ---------------------------------------------------------------------- */

int aar_xport_status_get(aar_xport *x, aar_xport_status *st)
{
	volatile uint8_t *cb;
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!st)
		return AAR_E_INVAL;
	cb = x->cb;
	st->handle = x->handle;
	st->state = aar_get_be32(cb + AAR_CB_STATE);
	st->end_reason = aar_get_be32(cb + AAR_CB_END_REASON);
	st->exit_code = (int32_t)aar_get_be32(cb + AAR_CB_EXIT_CODE);
	st->flags = aar_get_be32(cb + AAR_CB_FLAGS);
	st->progress = aar_get_be64(cb + AAR_CB_PROGRESS_HI);
	st->lease_period_ms = aar_get_be32(cb + AAR_CB_LEASE_PERIOD_MS);
	st->fault_class = aar_get_be32(cb + AAR_CB_FAULT_CLASS);
	st->fault_syndrome = aar_get_be32(cb + AAR_CB_FAULT_SYNDROME);
	st->fault_pstate = aar_get_be32(cb + AAR_CB_FAULT_PSTATE);
	st->fault_pc = aar_get_be64(cb + AAR_CB_FAULT_PC_HI);
	st->fault_sp = aar_get_be64(cb + AAR_CB_FAULT_SP_HI);
	st->fault_lr = aar_get_be64(cb + AAR_CB_FAULT_LR_HI);
	st->fault_addr = aar_get_be64(cb + AAR_CB_FAULT_ADDR_HI);
	st->load_base = aar_get_be64(cb + AAR_CB_LOAD_BASE_HI);
	st->shared_used = aar_get_be64(cb + AAR_CB_SHARED_USED_HI);
	st->private_committed = aar_get_be64(cb + AAR_CB_PRIVATE_COMMITTED_HI);
	st->sq_full = aar_get_be32(cb + AAR_CB_SQ_FULL);
	st->log_dropped = aar_get_be32(cb + AAR_CB_LOG_DROPPED);
	st->events_dropped = aar_get_be32(cb + AAR_CB_EVENTS_DROPPED);
	st->cq_full = aar_get_be32(cb + AAR_CB_CQ_FULL);
	st->msg_consumed = aar_get_be32(cb + AAR_CB_MSG_CONSUMED);
	return AAR_OK;
}

/* ---- Lease ---------------------------------------------------------------------- */

int aar_xport_lease_renew(aar_xport *x)
{
	volatile uint8_t *cb;

	if (!x)
		return AAR_E_INVAL;
	cb = x->cb;
	if (x->handle == 0u || !cb)
		return AAR_E_STALE;
	x->lease++;
	if (x->lease == 0u)
		x->lease = 1u;
	aar_put_be32(cb + AAR_CB_LEASE_COUNTER, x->lease);
	return AAR_OK;
}

/* ---- Doorbell -------------------------------------------------------------------- */

static void kick_send(aar_xport *x)
{
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	int rc;

	if (!x->kick_pending || !x->board.post || need_live(x) != AAR_OK)
		return;
	memset(req, 0, sizeof(req));
	zzapp_build_kick_request(req, x->handle, x->kick_pending);
	rc = x->board.post(x->board.user, ZZAPP_OP_KICK, req, sizeof(req));
	if (rc == AAR_OK) {
		x->kick_pending = 0u;
		x->stats.kicks++;
	}
	/* BUSY: the previous bell is still on its way; keep the bits for the
	 * next pass. The kick is a hint, a lost one costs latency only. */
}

void aar_xport_kick(aar_xport *x, uint32_t flags)
{
	if (!x || need_live(x) != AAR_OK)
		return;
	flags &= ZZAPP_KICK_KNOWN;
	if (!flags)
		return;
	if (x->kick_pending)
		x->stats.kicks_coalesced++;
	x->kick_pending |= flags;
	kick_send(x);
}

/* ---- Events ---------------------------------------------------------------------- */

static void note_cq_full(aar_xport *x)
{
	cb_add(x, AAR_CB_CQ_FULL, 1u);
}

static void note_event_drop(aar_xport *x)
{
	x->stats.events_dropped++;
	cb_add(x, AAR_CB_EVENTS_DROPPED, 1u);
	note_cq_full(x);
}

/* Posts the pending mouse event: AAR_OK, AAR_E_BUSY (CQ full, still pending),
 * AAR_E_IO. */
static int mouse_post(aar_xport *x)
{
	int rc;

	if (!x->mouse_pending)
		return AAR_OK;
	rc = aar_queue_post_event(&x->cq, &x->mouse, x->board.acquire,
				  x->board.release);
	if (rc == AAR_OK) {
		x->mouse_pending = 0u;
		x->stats.events_posted++;
	}
	return rc;
}

static int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

int aar_xport_post_event(aar_xport *x, const aar_event *ev)
{
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!ev)
		return AAR_E_INVAL;
	if (ev->type == AAR_EV_MOUSE) {
		if (x->mouse_pending &&
		    (x->mouse.buttons != ev->buttons ||
		     x->mouse.code != ev->code)) {
			/* A click or a button change is not folded away. */
			rc = mouse_post(x);
			if (rc == AAR_E_BUSY) {
				note_event_drop(x);
				return AAR_E_BUSY;
			}
			if (rc != AAR_OK)
				return rc;
		}
		if (x->mouse_pending) {
			x->mouse.dx = sat16((int32_t)x->mouse.dx + ev->dx);
			x->mouse.dy = sat16((int32_t)x->mouse.dy + ev->dy);
			x->mouse.time_ms = ev->time_ms;
			x->stats.events_merged++;
		} else {
			x->mouse = *ev;
			x->mouse.reserved = 0u;
			x->mouse_pending = 1u;
		}
		rc = mouse_post(x);
		return rc == AAR_E_BUSY ? AAR_OK : rc;
	}
	/* Keep the order of motion, keys and clicks. */
	rc = mouse_post(x);
	if (rc == AAR_E_BUSY) {
		note_event_drop(x);
		return AAR_E_BUSY;
	}
	if (rc != AAR_OK)
		return rc;
	rc = aar_queue_post_event(&x->cq, ev, x->board.acquire,
				  x->board.release);
	if (rc == AAR_OK) {
		x->stats.events_posted++;
		return AAR_OK;
	}
	if (rc == AAR_E_BUSY)
		note_event_drop(x);
	return rc;
}

int aar_xport_flush_events(aar_xport *x)
{
	if (need_live(x) != AAR_OK)
		return 1;
	return mouse_post(x) == AAR_OK;
}

void aar_xport_flush(aar_xport *x)
{
	if (!x || need_live(x) != AAR_OK)
		return;
	(void)mouse_post(x);
	kick_send(x);
}

/* ---- Messages and the SQ -------------------------------------------------------------- */

int aar_xport_send_message(aar_xport *x, const void *msg)
{
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!msg)
		return AAR_E_INVAL;
	rc = aar_cb_post_msg(&x->cq, x->cb, &x->msg_posted, msg,
			     x->board.acquire, x->board.release);
	if (rc == AAR_OK)
		x->stats.msgs_sent++;
	return rc;
}

uint32_t aar_xport_message_count(const aar_xport *x)
{
	return x ? x->inbox_tail - x->inbox_head : 0u;
}

int aar_xport_sq_blocked(const aar_xport *x)
{
	return x ? x->sq_blocked : 0;
}

void aar_xport_set_log_sink(aar_xport *x,
			    void (*sink)(void *user, const void *data,
					 uint32_t len),
			    void *user)
{
	if (x) {
		x->log_sink = sink;
		x->log_user = user;
	}
}

static void sq_pop(aar_xport *x)
{
	uint32_t used;

	/* A full SQ means the app may be parked waiting for room. */
	if (aar_queue_used(&x->sq, &used) == AAR_OK && used == x->sq.depth)
		x->kick_pending |= ZZAPP_KICK_SQ_SPACE;
	aar_queue_pop(&x->sq, x->board.release);
}

static void take_log(aar_xport *x, const aar_request *r)
{
	uint8_t tmp[AAR_LOG_MAX];
	volatile uint8_t *p;
	uint32_t n = r->buf_len, i;

	if (n > AAR_LOG_MAX)
		n = AAR_LOG_MAX;
	if (!x->log_sink || n == 0u) {
		x->stats.logs_unclaimed++;
		return;
	}
	p = map_range(x, r->buf, r->buf_off, n);
	if (!p) {
		x->stats.bad_buf++;
		return;
	}
	for (i = 0u; i < n; i++)
		tmp[i] = p[i];
	x->stats.logs++;
	x->log_sink(x->log_user, tmp, n);
}

/*
 * Moves messages and logs off the front of the SQ. With `req` it stops at the
 * first request and returns it (1); without, it stops there and leaves it.
 * 0 when the SQ is empty, the next entry is a request that stays, or the next
 * entry is a message the full inbox cannot take. That message is not popped:
 * it stays at the SQ head, so the app, which reads the head index, sees the
 * queue fill and gets AAR_E_BUSY from its own post instead of losing messages.
 * Everything behind it, requests included, waits until the owner takes a
 * message (aar_xport_get_message); x->sq_blocked says so. AAR_E_IO on corrupt
 * indices.
 */
static int sq_pump(aar_xport *x, aar_xport_req *req)
{
	x->sq_blocked = 0u;
	for (;;) {
		const volatile uint8_t *slot;
		aar_request r;
		int rc = aar_queue_peek(&x->sq, x->board.acquire, &slot);
		uint16_t op;

		if (rc <= 0)
			return rc;
		op = aar_get_be16(slot + AAR_ENTRY_OP);
		if (op == AAR_OP_MSG) {
			if (x->inbox_tail - x->inbox_head >= AAR_XPORT_INBOX) {
				x->sq_blocked = 1u;
				x->stats.sq_blocked++;
				return 0;
			}
			aar_entry_get_msg(slot,
				x->inbox[x->inbox_tail % AAR_XPORT_INBOX]);
			x->inbox_tail++;
			x->stats.msgs_taken++;
			sq_pop(x);
			continue;
		}
		aar_entry_get_request(slot, &r);
		if (op == AAR_OP_LOG) {
			take_log(x, &r);
			sq_pop(x);
			continue;
		}
		if (op == AAR_OP_NONE || op == AAR_OP_EVENT) {
			/* Not valid on the SQ. */
			x->stats.bad_entries++;
			sq_pop(x);
			continue;
		}
		if (!req)
			return 0;
		memset(req, 0, sizeof(*req));
		req->op = r.op;
		req->flags = r.flags;
		req->id = r.id;
		memcpy(req->arg, r.arg, sizeof(req->arg));
		req->buf = r.buf;
		req->buf_off = r.buf_off;
		req->buf_len = r.buf_len;
		if (r.buf != 0u) {
			req->ptr = map_range(x, r.buf, r.buf_off, r.buf_len);
			if (!req->ptr) {
				aar_completion c;

				/* The card named memory the board does not
				 * have: refuse it, so the app is not left
				 * waiting. With no room on the CQ leave the
				 * entry for the next pass. */
				memset(&c, 0, sizeof(c));
				c.op = r.op;
				c.id = r.id;
				c.result = AAR_E_INVAL;
				if (aar_queue_post_completion(&x->cq, &c,
						x->board.acquire,
						x->board.release) != AAR_OK)
					return 0;
				x->stats.bad_buf++;
				kick_send(x);
				sq_pop(x);
				continue;
			}
		}
		sq_pop(x);
		x->stats.requests++;
		return 1;
	}
}

int aar_xport_service_next(aar_xport *x, aar_xport_req *req)
{
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!req)
		return AAR_E_INVAL;
	aar_xport_flush(x);
	return sq_pump(x, req);
}

int aar_xport_get_message(aar_xport *x, void *msg)
{
	int rc = need_live(x);

	if (rc != AAR_OK && x->inbox_tail == x->inbox_head)
		return rc;
	if (!msg)
		return AAR_E_INVAL;
	if (rc == AAR_OK) {
		rc = sq_pump(x, NULL);
		if (rc < 0 && x->inbox_tail == x->inbox_head)
			return rc;
	}
	if (x->inbox_tail == x->inbox_head)
		return 0;
	memcpy(msg, x->inbox[x->inbox_head % AAR_XPORT_INBOX], AAR_MSG_BYTES);
	x->inbox_head++;
	return 1;
}

int aar_xport_service_reply(aar_xport *x, const aar_xport_req *req)
{
	aar_completion c;
	uint32_t i;
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (!req)
		return AAR_E_INVAL;
	c.op = req->op;
	c.flags = req->reply_flags;
	c.id = req->id;
	c.result = req->result;
	for (i = 0u; i < AAR_ENTRY_OUTS; i++)
		c.out[i] = req->out[i];
	rc = aar_queue_post_completion(&x->cq, &c, x->board.acquire,
				       x->board.release);
	if (rc == AAR_E_BUSY) {
		x->stats.reply_busy++;
		note_cq_full(x);
		return rc;
	}
	if (rc != AAR_OK)
		return rc;
	x->stats.replies++;
	aar_xport_kick(x, ZZAPP_KICK_CQ_POSTED);
	return AAR_OK;
}

/* ---- Audio ------------------------------------------------------------------------- */

int aar_xport_audio_attach(aar_xport *x, uint32_t token, uint32_t bytes,
			   uint32_t frames)
{
	volatile uint8_t *ring;
	uint32_t channels;
	int rc = need_live(x);

	if (rc != AAR_OK)
		return rc;
	if (frames == 0u || (frames & (frames - 1u)) != 0u ||
	    bytes < AAR_AUDIO_RING_HDR_BYTES)
		return AAR_E_INVAL;
	ring = map_range(x, token, 0u, bytes);
	if (!ring)
		return AAR_E_IO;
	channels = le32(ring + AAR_AUDIO_RING_CHANNELS);
	if (le32(ring + AAR_AUDIO_RING_FORMAT) != AAR_AUDIO_FORMAT_S16LE ||
	    channels == 0u || channels > 8u ||
	    (bytes - AAR_AUDIO_RING_HDR_BYTES) / (channels * 2u) < frames)
		return AAR_E_IO;
	x->audio = ring;
	x->audio_frames = frames;
	x->audio_channels = channels;
	return AAR_OK;
}

void aar_xport_audio_detach(aar_xport *x)
{
	if (x) {
		x->audio = NULL;
		x->audio_frames = 0u;
		x->audio_channels = 0u;
	}
}

uint32_t aar_xport_audio_available(aar_xport *x)
{
	uint32_t n;

	if (!x || !x->audio || need_live(x) != AAR_OK)
		return 0u;
	n = le32(x->audio + AAR_AUDIO_RING_WRITE_FRAME) -
	    le32(x->audio + AAR_AUDIO_RING_READ_FRAME);
	return n > x->audio_frames ? 0u : n;
}

int aar_xport_audio_read(aar_xport *x, int16_t *dst, uint32_t frames,
			 uint32_t *got)
{
	volatile uint8_t *ring;
	uint32_t write, read, avail, n, f, c, at;

	if (got)
		*got = 0u;
	if (need_live(x) != AAR_OK || !x->audio)
		return AAR_E_INVAL;
	if (!dst || !got)
		return AAR_E_INVAL;
	ring = x->audio;
	write = le32(ring + AAR_AUDIO_RING_WRITE_FRAME);
	read = le32(ring + AAR_AUDIO_RING_READ_FRAME);
	avail = write - read;
	if (avail > x->audio_frames)
		return AAR_E_IO;
	n = avail < frames ? avail : frames;
	for (f = 0u; f < n; f++) {
		at = AAR_AUDIO_RING_HDR_BYTES +
		     ((read + f) & (x->audio_frames - 1u)) *
		     x->audio_channels * 2u;
		for (c = 0u; c < x->audio_channels; c++)
			dst[f * x->audio_channels + c] = (int16_t)(
				(uint16_t)ring[at + c * 2u] |
				((uint16_t)ring[at + c * 2u + 1u] << 8));
	}
	put_le32(ring + AAR_AUDIO_RING_READ_FRAME, read + n);
	if (n < frames)
		put_le32(ring + AAR_AUDIO_RING_UNDERRUNS,
			 le32(ring + AAR_AUDIO_RING_UNDERRUNS) + 1u);
	*got = n;
	return AAR_OK;
}

const aar_xport_stats *aar_xport_stats_get(const aar_xport *x)
{
	return x ? &x->stats : NULL;
}

/* ====================================================================== */
/* The 68k board                                                            */
/* ====================================================================== */

#if AAR_XPORT_AMIGA

#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <libraries/configvars.h>
#include <libraries/expansion.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/zz9k.h>
#include "zz9k/request.h"

#define XPORT_LONG_OP_MS      120000UL
#define XPORT_MIN_REVISION    36

#define MMU_MAPP_CACHEINHIBIT 0x00000040UL
#define MMU_MAPP_COPYBACK     0x00002000UL
#define MMU_MAPP_IMPRECISE    0x00200000UL
#define MMU_MAPP_NONSERIALIZED 0x20000000UL
#define MMU_CACHE_MASK (MMU_MAPP_CACHEINHIBIT | MMU_MAPP_COPYBACK | \
			MMU_MAPP_IMPRECISE | MMU_MAPP_NONSERIALIZED)

struct mmu_context;

typedef struct aar_xport_amiga {
	struct Library *zz9k;
	struct Library *mmu;
	struct mmu_context *context;
	uint32_t page;
	struct MsgPort *timer_port;
	struct timerequest *timer;
	uint32_t base, size;
	ZZ9KAsyncRequest bell;
	ZZ9KRequest bell_request;
	uint8_t bell_inflight;
} aar_xport_amiga;

static struct mmu_context *mmu_default_context(struct Library *mmu)
{
	register struct mmu_context *d0 __asm("d0");
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -150(a6)" : "=r"(d0) : "r"(a6) :
		       "cc", "memory", "d1", "a0", "a1");
	return d0;
}

static uint32_t mmu_page_size(struct Library *mmu, struct mmu_context *ctx)
{
	register uint32_t d0 __asm("d0");
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -48(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d1", "a1");
	return d0;
}

static int mmu_set_properties(struct Library *mmu, struct mmu_context *ctx,
			      uint32_t flags, uint32_t mask, uint32_t address,
			      uint32_t size)
{
	register uint32_t d0 __asm("d0") = size;
	register uint32_t d1 __asm("d1") = flags;
	register uint32_t d2 __asm("d2") = mask;
	register struct mmu_context *a0 __asm("a0") = ctx;
	register void *a1 __asm("a1") = (void *)(uintptr_t)address;
	register void *a2 __asm("a2") = 0;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -84(a6)" : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(a0),
		       "+r"(a1), "+r"(a2) : "r"(a6) : "cc", "memory");
	return d0 != 0U;
}

static void mmu_lock(struct Library *mmu, struct mmu_context *ctx)
{
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -72(a6)" : "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d0", "d1", "a1", "a2");
}

static void mmu_unlock(struct Library *mmu, struct mmu_context *ctx)
{
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -78(a6)" : "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d0", "d1", "a1", "a2");
}

static int mmu_rebuild(struct Library *mmu, struct mmu_context *ctx)
{
	register uint32_t d0 __asm("d0");
	register struct mmu_context *a0 __asm("a0") = ctx;
	register struct Library *a6 __asm("a6") = mmu;

	__asm volatile("jsr -96(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d1", "a1", "a2");
	return d0 != 0U;
}

/* Cache-inhibits whole pages over [address, address + bytes): write back and
 * invalidate under the old mapping, set the property, rebuild the tree. The
 * pages stay inhibited: nothing restores them, so a later opener cannot make
 * a live claim's block copyback again. */
static int amiga_inhibit(aar_xport_amiga *a, uint32_t address, uint32_t bytes)
{
	uint32_t first = address & ~(a->page - 1u);
	uint32_t last = (address + bytes + a->page - 1u) & ~(a->page - 1u);

	CacheClearE((APTR)(uintptr_t)address, bytes, CACRF_ClearD);
	mmu_lock(a->mmu, a->context);
	if (!mmu_set_properties(a->mmu, a->context, MMU_MAPP_CACHEINHIBIT,
				MMU_CACHE_MASK, first, last - first) ||
	    !mmu_rebuild(a->mmu, a->context)) {
		mmu_unlock(a->mmu, a->context);
		return 0;
	}
	mmu_unlock(a->mmu, a->context);
	return 1;
}

static volatile uint8_t *amiga_map(void *user, uint32_t token, uint32_t bytes)
{
	aar_xport_amiga *a = (aar_xport_amiga *)user;
	uint32_t address;

	if (a->size != 0u && (token >= a->size || bytes > a->size - token))
		return NULL;
	if (bytes == 0u || token > UINT32_MAX - a->base - bytes)
		return NULL;
	address = a->base + token;
	if (!amiga_inhibit(a, address, bytes))
		return NULL;
	return (volatile uint8_t *)(uintptr_t)address;
}

static uint32_t amiga_now_ms(aar_xport_amiga *a)
{
	struct timeval tv;
	register struct Device *a6 __asm("a6") = a->timer->tr_node.io_Device;
	register struct timeval *a0 __asm("a0") = &tv;

	/* timer.device GetSysTime, LVO -66. */
	__asm volatile("jsr -66(a6)" : "+r"(a0) : "r"(a6) :
		       "cc", "memory", "d0", "d1", "a1");
	return tv.tv_secs * 1000UL + tv.tv_micro / 1000UL;
}

static void request_fill(ZZ9KRequest *request, uint16_t opcode,
			 const uint8_t *payload, uint32_t len)
{
	zz9k_request_init(request, opcode);
	request->entry.payload_len = (uint16_t)len;
	if (payload && len)
		memcpy(request->entry.payload.inline_data, payload, len);
}

static void reply_fill(aar_xport_reply *out, const ZZ9KMailboxEntry *in)
{
	out->status = (int32_t)in->status;
	out->len = in->payload_len < ZZAPP_MAILBOX_PAYLOAD_BYTES ?
		   in->payload_len : ZZAPP_MAILBOX_PAYLOAD_BYTES;
	memcpy(out->payload, in->payload.inline_data, out->len);
}

static int amiga_call(void *user, uint16_t opcode, const uint8_t *payload,
		      uint32_t len, int long_op, aar_xport_tick_fn tick,
		      void *tick_user, aar_xport_reply *reply)
{
	aar_xport_amiga *a = (aar_xport_amiga *)user;
	ZZ9KRequest request;
	ZZ9KMailboxEntry r;
	int status;

	request_fill(&request, opcode, payload, len);
	memset(&r, 0, sizeof(r));
	if (!long_op) {
		status = ZZ9KCallAtBase(a->zz9k, &request, &r,
					ZZ9K_DEFAULT_TIMEOUT_TICKS);
	} else {
		/* Deferred reply: the card answers when the work is done. Poll,
		 * renew the lease on the way, retry a BUSY (one deferred
		 * operation per claim) for as long as the lease would allow. */
		uint32_t started = amiga_now_ms(a);

		for (;;) {
			ZZ9KAsyncRequest async;
			uint32_t op_started = amiga_now_ms(a);

			memset(&async, 0, sizeof(async));
			status = ZZ9KCallAsyncAtBase(a->zz9k, &async, &request,
						     NULL, NULL);
			if (status == ZZ9K_STATUS_QUEUED) {
				while (!async.completed) {
					uint32_t done = 0u;

					if (tick && tick(tick_user)) {
						(void)ZZ9KCancelAsyncAtBase(
							a->zz9k, &async);
						return ZZ9K_STATUS_BAD_HANDLE;
					}
					status = ZZ9KPollAtBase(a->zz9k, 1u,
								&done);
					if (status != ZZ9K_STATUS_OK)
						return status;
					if (async.completed)
						break;
					if (amiga_now_ms(a) - op_started >=
					    XPORT_LONG_OP_MS) {
						(void)ZZ9KCancelAsyncAtBase(
							a->zz9k, &async);
						return ZZ9K_STATUS_TIMEOUT;
					}
					Delay(1);
				}
				r = async.reply;
				status = async.status;
			}
			if (status != ZZ9K_STATUS_BUSY &&
			    !(status == ZZ9K_STATUS_OK &&
			      r.status == ZZ9K_STATUS_BUSY))
				break;
			if (amiga_now_ms(a) - started >= XPORT_LONG_OP_MS)
				break;
			if (tick && tick(tick_user))
				return ZZ9K_STATUS_BAD_HANDLE;
			Delay(1);
		}
	}
	reply_fill(reply, &r);
	if (status == ZZ9K_STATUS_OK && r.status != ZZ9K_STATUS_OK)
		status = (int)r.status;
	return status;
}

static int amiga_post(void *user, uint16_t opcode, const uint8_t *payload,
		      uint32_t len)
{
	aar_xport_amiga *a = (aar_xport_amiga *)user;
	uint32_t done = 0u;
	int status;

	if (a->bell_inflight) {
		(void)ZZ9KPollAtBase(a->zz9k, 1u, &done);
		if (!a->bell.completed)
			return AAR_E_BUSY;
		a->bell_inflight = 0u;
	}
	request_fill(&a->bell_request, opcode, payload, len);
	memset(&a->bell, 0, sizeof(a->bell));
	status = ZZ9KCallAsyncAtBase(a->zz9k, &a->bell, &a->bell_request, NULL,
				     NULL);
	if (status != ZZ9K_STATUS_QUEUED)
		return status == ZZ9K_STATUS_BUSY ? AAR_E_BUSY : AAR_E_IO;
	a->bell_inflight = 1u;
	return AAR_OK;
}

void aar_xport_amiga_board_close(aar_xport_board *board)
{
	aar_xport_amiga *a;

	if (!board || !board->user)
		return;
	a = (aar_xport_amiga *)board->user;
	if (a->bell_inflight && a->zz9k)
		(void)ZZ9KCancelAsyncAtBase(a->zz9k, &a->bell);
	if (a->timer) {
		CloseDevice((struct IORequest *)a->timer);
		DeleteIORequest((struct IORequest *)a->timer);
	}
	if (a->timer_port)
		DeleteMsgPort(a->timer_port);
	if (a->mmu)
		CloseLibrary(a->mmu);
	if (a->zz9k)
		CloseLibrary(a->zz9k);
	FreeVec(a);
	memset(board, 0, sizeof(*board));
}

int aar_xport_amiga_board_open(aar_xport_board *board)
{
	aar_xport_amiga *a;
	struct Library *expansion;
	struct ConfigDev *dev;

	if (!board)
		return AAR_E_INVAL;
	memset(board, 0, sizeof(*board));
	a = (aar_xport_amiga *)AllocVec(sizeof(*a), MEMF_PUBLIC | MEMF_CLEAR);
	if (!a)
		return AAR_E_NOMEM;
	board->user = a;
	a->zz9k = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, 0);
	if (!a->zz9k || a->zz9k->lib_Revision < XPORT_MIN_REVISION)
		goto fail;
	expansion = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
	if (!expansion)
		goto fail;
	{
		/* FindConfigDev reads ExpansionBase. */
		struct Library *saved = (struct Library *)ExpansionBase;

		ExpansionBase = (struct ExpansionBase *)expansion;
		dev = FindConfigDev(0, ZZ9K_MNT_MANUFACTURER, ZZ9K_PRODUCT_Z3);
		ExpansionBase = (struct ExpansionBase *)saved;
	}
	if (dev) {
		a->base = (uint32_t)dev->cd_BoardAddr;
		a->size = dev->cd_BoardSize;
	}
	CloseLibrary(expansion);
	if (!dev)
		goto fail;
	a->mmu = OpenLibrary((CONST_STRPTR)"mmu.library", 41);
	if (!a->mmu)
		goto fail;
	a->context = mmu_default_context(a->mmu);
	a->page = a->context ? mmu_page_size(a->mmu, a->context) : 0u;
	if (a->page == 0u || (a->page & (a->page - 1u)) != 0u)
		goto fail;
	a->timer_port = CreateMsgPort();
	if (a->timer_port)
		a->timer = (struct timerequest *)CreateIORequest(
			a->timer_port, sizeof(struct timerequest));
	if (!a->timer || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
				    (struct IORequest *)a->timer, 0) != 0) {
		if (a->timer) {
			DeleteIORequest((struct IORequest *)a->timer);
			a->timer = NULL;
		}
		goto fail;
	}
	board->call = amiga_call;
	board->post = amiga_post;
	board->map = amiga_map;
	return AAR_OK;

fail:
	aar_xport_amiga_board_close(board);
	return AAR_E_NOSYS;
}

uint32_t aar_xport_amiga_board_base(const aar_xport_board *board)
{
	return board && board->user ?
	       ((const aar_xport_amiga *)board->user)->base : 0u;
}

uint32_t aar_xport_amiga_board_size(const aar_xport_board *board)
{
	return board && board->user ?
	       ((const aar_xport_amiga *)board->user)->size : 0u;
}

int aar_xport_load_file(aar_xport *x, const char *path, uint32_t client_caps)
{
	volatile uint8_t *staging;
	uint32_t chunk_max, size, off = 0u, n;
	BPTR file;
	LONG end;
	int rc;

	if (!path)
		return AAR_E_INVAL;
	file = Open((STRPTR)path, MODE_OLDFILE);
	if (!file)
		return AAR_E_NOENT;
	(void)Seek(file, 0L, OFFSET_END);
	end = Seek(file, 0L, OFFSET_BEGINNING);
	if (end <= 0L || (uint32_t)end > ZZAPP_IMAGE_MAX_BYTES) {
		Close(file);
		return AAR_E_INVAL;
	}
	size = (uint32_t)end;
	rc = aar_xport_load_begin(x, client_caps, &staging, &chunk_max);
	while (rc == AAR_OK && off < size) {
		n = size - off < chunk_max ? size - off : chunk_max;
		if (Read(file, (APTR)staging, (LONG)n) != (LONG)n) {
			rc = AAR_E_IO;
			break;
		}
		rc = aar_xport_load_chunk(x, off, n);
		off += n;
	}
	Close(file);
	return rc == AAR_OK ? aar_xport_load_end(x) : rc;
}

#endif /* AAR_XPORT_AMIGA */
