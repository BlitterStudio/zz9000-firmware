/*
 * Queue coherence probe (AAR cutover, slice 2): firmware side.
 *
 * A temporary diagnostic, see sdk_qprobe.h. The wire contract is
 * sdk/tools/zz9k-qprobe.h; this file only adds the per-queue state and the
 * service passes. The pair lives in an SDK shared buffer and is touched
 * through the normal cached mapping of its ARM address, with no cache
 * maintenance: the ACP makes the Amiga's accesses coherent, and that is
 * exactly what the probe checks.
 *
 * Teardown protocol (a CORE1 queue is used by core 1 while core 0 frees the
 * buffer), a Dekker-style handshake on two words of the queue state:
 *
 *   core 1, per pass:  busy = 1; dmb; if (attached) work on the pair;
 *                      dmb; busy = 0
 *   core 0, detach:    attached = 0; dmb; wait for busy == 0
 *
 * Either core 1 sees attached == 0 and never touches the pair, or core 0
 * sees busy == 1 and waits until the pass is over. Once core 0 has read
 * busy == 0 after clearing attached, any later pass reads attached == 0.
 * The wait is bounded. Core 1 may be absent: it counts as gone when the
 * scheduler reports it unavailable or parked (its busy word can then be
 * stale from a fault). Otherwise a timeout means the detach failed and the
 * caller must not free the buffer. A cold restart of core 1 clears every
 * busy word through sdk_qprobe_core1_halted().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>

#include "sdk_qprobe.h"
#include "sdk_mailbox.h"
#include "scheduler.h"
#include "xil_cache.h"
#include "xpseudo_asm.h"
#include "xtime_l.h"

#define QP_DETACH_WAIT_TICKS ((XTime)COUNTS_PER_SECOND / 50U)	/* 20 ms */

_Static_assert(sizeof(struct SDKQProbeState) <= SDK_QPROBE_STATE_MAX_BYTES,
               "queue probe state exceeds its window");
_Static_assert(SDK_QPROBE_STATE_OFFSET + SDK_QPROBE_STATE_MAX_BYTES <=
                   SDK_IMAGE_SESSIONS_OFFSET,
               "queue probe state overlaps the image sessions");
_Static_assert(sizeof(taskq_shared_t) <= SDK_QPROBE_STATE_OFFSET,
               "queue probe state overlaps the task queue");
_Static_assert(sizeof(struct SDKQProbeQueue) == 2U * ZZ9K_QP_LINE_BYTES,
               "queue state should fill two lines");

typedef volatile struct SDKQProbeQueue QpQueue;

static uint32_t qp_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void qp_put(uint8_t *reply, uint32_t word, uint32_t value)
{
	uint8_t *p = reply + 4U * word;

	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static uint32_t qp_stamp(void)
{
	XTime now;

	XTime_GetTime(&now);
	return (uint32_t)now;
}

static uint32_t qp_origin(const QpQueue *q)
{
	return q->owner == ZZ9K_QP_OWNER_CORE1 ? ZZ9K_QP_ORIGIN_CORE1
	                                       : ZZ9K_QP_ORIGIN_CORE0;
}

static void qp_error(QpQueue *q, uint32_t code, uint32_t seq)
{
	q->up_errors++;
	if (q->first_error == ZZ9K_QP_OK) {
		q->first_error = code;
		q->first_error_seq = seq;
	}
}

/* An index beyond the ring is counted once until it is sane again. */
static int qp_index_ok(QpQueue *q, uint32_t bit, int32_t available,
                       uint32_t seq)
{
	if (available >= 0) {
		q->flags &= ~bit;
		return 1;
	}
	if ((q->flags & bit) == 0U) {
		q->flags |= bit;
		qp_error(q, ZZ9K_QP_ERR_INDEX, seq);
	}
	return 0;
}

/* Up ring, firmware is the consumer: head, dmb, entries, dmb, tail. */
static uint32_t qp_consume(QpQueue *q, uint32_t index, volatile void *pair)
{
	uint32_t head = zz9k_qp_get(pair, ZZ9K_QP_UP_HEAD);
	uint32_t tail = q->up_consumed;
	uint32_t taken = 0U;
	int32_t available;

	dmb();
	available = zz9k_qp_available(head, tail);
	if (!qp_index_ok(q, SDK_QPROBE_F_UP_INDEX, available, tail + 1U)) {
		/* Follow the producer's count so the seqs line up again. */
		q->up_consumed = head;
		zz9k_qp_set(pair, ZZ9K_QP_UP_TAIL, head);
		return 0U;
	}
	while (taken < (uint32_t)available) {
		ZZ9KQpEntry entry;
		uint32_t seq = tail + 1U;
		uint32_t result = zz9k_qp_entry_check(
		    zz9k_qp_slot(pair, ZZ9K_QP_UP_RING_OFFSET, tail), seq, index,
		    1U << ZZ9K_QP_ORIGIN_AMIGA, &entry);

		if (result == ZZ9K_QP_OK) {
			if (entry.kind == ZZ9K_QP_KIND_PING) {
				/* The PONG waits for the down ring; never drop it.
				 * Leave the PING in the ring until there is room. */
				if (q->pong_count >= SDK_QPROBE_PONG_DEPTH)
					break;
				q->pong_id[(q->pong_head + q->pong_count) %
				           SDK_QPROBE_PONG_DEPTH] = entry.stamp;
				q->pong_count++;
			} else if (entry.kind == ZZ9K_QP_KIND_COMPLETE) {
				if (entry.arg <= q->last_complete ||
				    entry.arg > q->down_produced)
					result = ZZ9K_QP_ERR_ORDER;
				else
					q->last_complete = entry.arg;
			} else {
				result = ZZ9K_QP_ERR_KIND;
			}
		}
		if (result != ZZ9K_QP_OK)
			qp_error(q, result, seq);
		q->up_checked++;
		tail++;
		taken++;
	}
	if (taken != 0U) {
		dmb();
		q->up_consumed = tail;
		zz9k_qp_set(pair, ZZ9K_QP_UP_TAIL, tail);
	}
	return taken;
}

/* Down ring, firmware is the producer: entry words, dmb, head. PONGs go out
 * first; in STREAM mode the rest of the ring is filled with STREAM entries. */
static void qp_produce(QpQueue *q, uint32_t index, volatile void *pair)
{
	uint32_t head = q->down_produced;
	uint32_t tail = zz9k_qp_get(pair, ZZ9K_QP_DOWN_TAIL);
	uint32_t stream = zz9k_qp_get(pair, ZZ9K_QP_HDR_MODE) ==
	                  ZZ9K_QP_MODE_STREAM;
	uint32_t origin = qp_origin(q);
	int32_t used = zz9k_qp_available(head, tail);

	dmb();
	if (!qp_index_ok(q, SDK_QPROBE_F_DOWN_INDEX, used, head + 1U))
		return;
	for (;;) {
		uint32_t kind;
		uint32_t arg = 0U;

		if (q->pong_count != 0U) {
			kind = ZZ9K_QP_KIND_PONG;
			arg = q->pong_id[q->pong_head];
		} else if (stream) {
			kind = ZZ9K_QP_KIND_STREAM;
		} else {
			break;
		}
		if ((uint32_t)used >= ZZ9K_QP_ENTRIES) {
			/* Counted once per time the ring runs full. */
			if ((q->flags & SDK_QPROBE_F_FULL) == 0U) {
				q->flags |= SDK_QPROBE_F_FULL;
				q->ring_full++;
			}
			break;
		}
		zz9k_qp_entry_write(zz9k_qp_slot(pair, ZZ9K_QP_DOWN_RING_OFFSET,
		                                 head),
		                    head + 1U, kind, index, origin, qp_stamp(), arg);
		if (kind == ZZ9K_QP_KIND_PONG) {
			q->pong_head = (q->pong_head + 1U) % SDK_QPROBE_PONG_DEPTH;
			q->pong_count--;
			q->pongs++;
		}
		dmb();
		head++;
		used++;
		q->down_produced = head;
		zz9k_qp_set(pair, ZZ9K_QP_DOWN_HEAD, head);
		q->flags &= ~SDK_QPROBE_F_FULL;
	}
}

/* One service pass; returns the up entries consumed. */
static uint32_t qp_pass(QpQueue *q, uint32_t index)
{
	volatile void *pair = SDK_QPROBE_PTR(q->address);
	uint32_t taken;

	q->passes++;
	taken = qp_consume(q, index, pair);
	qp_produce(q, index, pair);
	return taken;
}

void sdk_qprobe_service_core0(void)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	uint32_t mask = state->summary.core0_mask;

	while (mask != 0U) {
		uint32_t index = (uint32_t)__builtin_ctz(mask);

		mask &= mask - 1U;
		qp_pass(&state->queue[index], index);
	}
}

void sdk_qprobe_service_core1(void)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	uint32_t mask = state->summary.core1_mask;

	while (mask != 0U) {
		uint32_t index = (uint32_t)__builtin_ctz(mask);
		QpQueue *q = &state->queue[index];

		mask &= mask - 1U;
		q->busy = 1U;
		dmb();
		if (q->attached != 0U)
			qp_pass(q, index);
		dmb();
		q->busy = 0U;
	}
}

static uint32_t qp_mask_bit(uint32_t index)
{
	return 1U << index;
}

static volatile uint32_t *qp_mask_word(struct SDKQProbeState *state,
                                       uint32_t owner)
{
	if (owner == ZZ9K_QP_OWNER_CORE0)
		return &state->summary.core0_mask;
	if (owner == ZZ9K_QP_OWNER_CORE1)
		return &state->summary.core1_mask;
	return 0;
}

/* Core 1 is out of the pair, or provably not running. */
static int qp_core1_left(QpQueue *q)
{
	XTime start;
	XTime now;

	XTime_GetTime(&start);
	for (;;) {
		dmb();
		if (q->busy == 0U)
			return 1;
		if (!scheduler_core1_available() ||
		    scheduler_shared()->core1_parked != 0U) {
			q->busy = 0U;
			return 1;
		}
		XTime_GetTime(&now);
		if (now - start > QP_DETACH_WAIT_TICKS)
			return 0;
	}
}

/* Stops the owner touching the pair. The slot stays taken. */
static int qp_stop(struct SDKQProbeState *state, uint32_t index)
{
	QpQueue *q = &state->queue[index];
	volatile uint32_t *mask = qp_mask_word(state, q->owner);

	if (mask)
		*mask &= ~qp_mask_bit(index);
	q->attached = 0U;
	dmb();
	if (q->owner == ZZ9K_QP_OWNER_CORE1)
		return qp_core1_left(q);
	return 1;
}

static void qp_release(struct SDKQProbeState *state, uint32_t index)
{
	QpQueue *q = &state->queue[index];
	volatile uint32_t *word = (volatile uint32_t *)q;
	uint32_t i;

	for (i = 0U; i < sizeof(*q) / sizeof(uint32_t); i++)
		word[i] = 0U;
	dmb();
}

static int qp_detach(struct SDKQProbeState *state, uint32_t index)
{
	if (!state->queue[index].in_use)
		return 1;
	if (!qp_stop(state, index))
		return 0;
	qp_release(state, index);
	return 1;
}

int sdk_qprobe_detach_handle(uint32_t handle)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	uint32_t index;
	int clean = 1;

	for (index = 0U; index < ZZ9K_QP_MAX_QUEUES; index++) {
		QpQueue *q = &state->queue[index];

		if (q->in_use && q->handle == handle && !qp_detach(state, index))
			clean = 0;
	}
	return clean;
}

/* The state window is uninitialised DDR until the first sdk_qprobe_reset(),
 * which runs at cold boot before core 1 exists. */
static int qp_window_ready;

int sdk_qprobe_reset(void)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	uint32_t index;
	int clean = 1;

	if (!qp_window_ready) {
		volatile uint32_t *word = (volatile uint32_t *)SDK_QPROBE_STATE_BASE;

		for (index = 0U; index < sizeof(*state) / sizeof(uint32_t); index++)
			word[index] = 0U;
		/* The lines may be filled under other attributes than the
		 * coherent mapping set up later: push them to DRAM. */
		Xil_DCacheFlushRange((INTPTR)(uintptr_t)SDK_QPROBE_STATE_BASE,
		                     sizeof(*state));
		dmb();
		qp_window_ready = 1;
		return 1;
	}
	for (index = 0U; index < ZZ9K_QP_MAX_QUEUES; index++) {
		if (!qp_detach(state, index))
			clean = 0;
	}
	return clean;
}

void sdk_qprobe_core1_halted(void)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	uint32_t index;

	for (index = 0U; index < ZZ9K_QP_MAX_QUEUES; index++)
		state->queue[index].busy = 0U;
	dmb();
}

static void qp_clear_pair(volatile void *pair)
{
	volatile uint32_t *word = (volatile uint32_t *)pair;
	uint32_t i;

	for (i = 0U; i < ZZ9K_QP_PAIR_BYTES / sizeof(uint32_t); i++)
		word[i] = 0U;
}

static uint16_t qp_attach(uint16_t client_id, const uint8_t *payload,
                          uint16_t payload_len, uint8_t *reply,
                          uint16_t *reply_len, sdk_qprobe_lookup_fn lookup)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	struct SDKQProbeBuffer buffer;
	volatile uint32_t *mask;
	uint32_t handle = qp_be32(payload + 4);
	uint32_t index = qp_be32(payload + 8);
	uint32_t owner = qp_be32(payload + 12);
	uint32_t i;
	QpQueue *q;
	volatile void *pair;

	if (payload_len < 16U || index >= ZZ9K_QP_MAX_QUEUES ||
	    owner >= ZZ9K_QP_OWNER_COUNT)
		return SDK_STATUS_BAD_REQUEST;
	q = &state->queue[index];
	if (q->in_use)
		return SDK_STATUS_BUSY;
	if (!lookup || !lookup(handle, &buffer) || buffer.owner_id != client_id)
		return SDK_STATUS_BAD_HANDLE;
	if ((buffer.flags & (SDK_ALLOC_CARD_ONLY | SDK_ALLOC_MEDIA)) != 0U ||
	    buffer.length < ZZ9K_QP_PAIR_BYTES ||
	    (buffer.address & (ZZ9K_QP_PAIR_ALIGN - 1U)) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	/* One pair per buffer: a second queue on it would overlap the first. */
	for (i = 0U; i < ZZ9K_QP_MAX_QUEUES; i++) {
		if (state->queue[i].in_use && state->queue[i].handle == handle)
			return SDK_STATUS_BUSY;
	}
	if (owner == ZZ9K_QP_OWNER_CORE1 && !scheduler_core1_available())
		return SDK_STATUS_UNSUPPORTED;

	pair = SDK_QPROBE_PTR(buffer.address);
	qp_clear_pair(pair);
	zz9k_qp_set(pair, ZZ9K_QP_HDR_MAGIC, ZZ9K_QP_MAGIC);
	zz9k_qp_set(pair, ZZ9K_QP_HDR_VERSION, ZZ9K_QP_VERSION);
	zz9k_qp_set(pair, ZZ9K_QP_HDR_QUEUE, index);
	zz9k_qp_set(pair, ZZ9K_QP_HDR_OWNER, owner);
	zz9k_qp_set(pair, ZZ9K_QP_HDR_MODE, ZZ9K_QP_MODE_IDLE);

	qp_release(state, index);
	q->in_use = 1U;
	q->owner = owner;
	q->client_id = client_id;
	q->handle = handle;
	q->address = buffer.address;
	dmb();
	q->attached = 1U;
	mask = qp_mask_word(state, owner);
	if (mask) {
		dmb();
		*mask |= qp_mask_bit(index);
	}

	qp_put(reply, ZZ9K_QP_ATTACH_ARM_ADDR, buffer.address);
	qp_put(reply, ZZ9K_QP_ATTACH_PAIR_BYTES, ZZ9K_QP_PAIR_BYTES);
	qp_put(reply, ZZ9K_QP_ATTACH_VERSION, ZZ9K_QP_VERSION);
	*reply_len = ZZ9K_QP_ATTACH_WORDS * 4U;
	return SDK_STATUS_OK;
}

static void qp_status_words(const QpQueue *q, uint8_t *reply,
                            uint16_t *reply_len)
{
	qp_put(reply, ZZ9K_QP_STATUS_OWNER, q->owner);
	qp_put(reply, ZZ9K_QP_STATUS_DOWN_PRODUCED, q->down_produced);
	qp_put(reply, ZZ9K_QP_STATUS_UP_CHECKED, q->up_checked);
	qp_put(reply, ZZ9K_QP_STATUS_UP_ERRORS, q->up_errors);
	qp_put(reply, ZZ9K_QP_STATUS_FIRST_ERROR, q->first_error);
	qp_put(reply, ZZ9K_QP_STATUS_FIRST_ERROR_SEQ, q->first_error_seq);
	qp_put(reply, ZZ9K_QP_STATUS_RING_FULL, q->ring_full);
	qp_put(reply, ZZ9K_QP_STATUS_PASSES, q->passes);
	qp_put(reply, ZZ9K_QP_STATUS_TIMER_HZ, (uint32_t)COUNTS_PER_SECOND);
	qp_put(reply, ZZ9K_QP_STATUS_COMPLETED, q->last_complete);
	qp_put(reply, ZZ9K_QP_STATUS_PONGS, q->pongs);
	*reply_len = ZZ9K_QP_STATUS_WORDS * 4U;
}

/* Core 0 serves every DOORBELL queue once and times the whole sweep. */
static void qp_kick(uint8_t *reply, uint16_t *reply_len)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	XTime start;
	XTime end;
	uint32_t queues = 0U;
	uint32_t consumed = 0U;
	uint32_t index;

	XTime_GetTime(&start);
	for (index = 0U; index < ZZ9K_QP_MAX_QUEUES; index++) {
		QpQueue *q = &state->queue[index];

		if (q->in_use && q->attached != 0U &&
		    q->owner == ZZ9K_QP_OWNER_DOORBELL) {
			consumed += qp_pass(q, index);
			queues++;
		}
	}
	XTime_GetTime(&end);
	qp_put(reply, ZZ9K_QP_KICK_QUEUES, queues);
	qp_put(reply, ZZ9K_QP_KICK_CONSUMED, consumed);
	qp_put(reply, ZZ9K_QP_KICK_TICKS, (uint32_t)(end - start));
	qp_put(reply, ZZ9K_QP_KICK_TIMER_HZ, (uint32_t)COUNTS_PER_SECOND);
	*reply_len = ZZ9K_QP_KICK_WORDS * 4U;
}

uint16_t sdk_qprobe_command(uint16_t client_id, const uint8_t *payload,
                            uint16_t payload_len, uint8_t *reply,
                            uint16_t *reply_len,
                            sdk_qprobe_lookup_fn lookup)
{
	struct SDKQProbeState *state = SDK_QPROBE_STATE;
	uint32_t index;

	*reply_len = 0U;
	if (payload_len < 4U)
		return SDK_STATUS_BAD_REQUEST;
	switch (qp_be32(payload)) {
	case ZZ9K_QP_CMD_ATTACH:
		return qp_attach(client_id, payload, payload_len, reply, reply_len,
		                 lookup);
	case ZZ9K_QP_CMD_KICK:
		qp_kick(reply, reply_len);
		return SDK_STATUS_OK;
	case ZZ9K_QP_CMD_STATUS:
	case ZZ9K_QP_CMD_DETACH:
		if (payload_len < 8U)
			return SDK_STATUS_BAD_REQUEST;
		index = qp_be32(payload + 4);
		if (index >= ZZ9K_QP_MAX_QUEUES)
			return SDK_STATUS_BAD_REQUEST;
		if (!state->queue[index].in_use)
			return SDK_STATUS_NOT_FOUND;
		if (qp_be32(payload) == ZZ9K_QP_CMD_STATUS) {
			qp_status_words(&state->queue[index], reply, reply_len);
			return SDK_STATUS_OK;
		}
		if (state->queue[index].client_id != client_id)
			return SDK_STATUS_BAD_HANDLE;
		if (!qp_stop(state, index))
			return SDK_STATUS_BUSY;
		/* The owner has stopped: the counters are final. */
		qp_status_words(&state->queue[index], reply, reply_len);
		qp_release(state, index);
		return SDK_STATUS_OK;
	default:
		return SDK_STATUS_BAD_REQUEST;
	}
}
