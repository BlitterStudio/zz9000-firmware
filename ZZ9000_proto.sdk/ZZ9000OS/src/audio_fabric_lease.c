/*
 * ZZ9000AX audio fabric compositor: mailbox lease plane (plan U3).
 *
 * Main-loop context on core 0: the SDK AUDIO_FABRIC_* request handlers
 * drive these entry points (begin/submit/release) and the per-slot
 * state read. The producer ops trampolines hand the slot's card-side
 * ring to the compositor through the same snapshot/stage discipline
 * as the pump's stream ring (KTD2). The compositor core
 * (audio_fabric.c) owns the slot table; this TU reaches it only
 * through the narrow seams in audio_fabric_internal.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "audio_fabric.h"
#include "audio_fabric_internal.h"
#include "audio_scene.h"
#include "memorymap.h"
#include "sdk_mailbox.h"
#include "xil_cache.h"

/* Card-side lease rings (memorymap.h reservation). The host-test seam
 * redirects them; firmware builds keep the static claim. */
#ifdef AUDIO_FABRIC_HOST_TEST
static uint8_t *g_fabric_lease_ring[AUDIO_FABRIC_SLOT_COUNT];

void audio_fabric_host_set_lease_rings(uint8_t *slot1, uint8_t *slot2)
{
	g_fabric_lease_ring[AUDIO_FABRIC_SLOT_MAILBOX] = slot1;
	g_fabric_lease_ring[AUDIO_FABRIC_SLOT_RESERVED] = slot2;
}
#else
static uint8_t *const g_fabric_lease_ring[AUDIO_FABRIC_SLOT_COUNT] = {
	NULL,
	(uint8_t *)AUDIO_FABRIC_LEASE_RING1_ADDRESS,
	(uint8_t *)AUDIO_FABRIC_LEASE_RING2_ADDRESS,
};
#endif

/* Admission policy (R4/R5): slot 0 is the pump -- live exactly while
 * an SDK playback bind exists and never leaseable -- and slot 2 stays
 * firmware-reserved until the AHI/synthesis follow-on claims it; both
 * are client errors, not exhaustion. AUDIO_FABRIC_BENCH_3SLOT lifts
 * only the slot 2 reservation so the bench session can measure three
 * producers (U5); the compositor itself already walks all three
 * slots. */
static int fabric_slot_leaseable(uint32_t slot)
{
	if (slot == AUDIO_FABRIC_SLOT_PUMP ||
	    slot >= AUDIO_FABRIC_SLOT_COUNT)
		return 0;
#ifndef AUDIO_FABRIC_BENCH_3SLOT
	if (slot == AUDIO_FABRIC_SLOT_RESERVED)
		return 0;
#endif
	return 1;
}

/* Zero a card-side ring and push it to DRAM: the compositor reads it
 * with reader-side invalidates, so the zeroing must survive them. */
static void fabric_lease_ring_zero(uint8_t *ring, uint32_t capacity)
{
	memset(ring, 0, capacity);
	Xil_DCacheFlushRange((INTPTR)ring, capacity);
}

void fabric_lease_reset_rings(void)
{
	uint32_t i;

	for (i = AUDIO_FABRIC_SLOT_MAILBOX; i < AUDIO_FABRIC_SLOT_COUNT;
	     i++) {
		if (g_fabric_lease_ring[i] != NULL)
			fabric_lease_ring_zero(g_fabric_lease_ring[i],
				AUDIO_FABRIC_LEASE_RING_BYTES);
	}
}

/* Per-slot source metering (KTD7): peak in the scene meter's 16.16
 * vocabulary and at-rail region counting with the same continuity
 * latch, measured on the source frames exactly as read from the ring
 * (post byte-swap, pre gain and pre mix -- the lease's own signal,
 * never the mixed output). */
void fabric_lease_meter(struct audio_fabric_slot *s,
	const int16_t *pcm, uint32_t bytes)
{
	struct audio_fabric_lease *l = &s->lease;
	uint32_t samples = bytes / 2U;
	uint32_t i;

	if (l->peak_reset) {
		l->peak_reset = 0U;
		l->peak = 0U;
	}
	for (i = 0U; i < samples; i++) {
		int32_t v = pcm[i];
		int32_t neg = v >> 31;
		uint32_t magnitude = (uint32_t)((v + neg) ^ neg);

		if ((magnitude << 1) > l->peak)
			l->peak = magnitude << 1;
		if (v == INT16_MAX || v == INT16_MIN) {
			if (!l->clip_open) {
				l->clip_open = 1U;
				if (l->clips != UINT32_MAX)
					l->clips++;
			}
		} else {
			l->clip_open = 0U;
		}
	}
}

/* Lease producer ops: the slot's card-side ring through the same
 * snapshot/stage discipline as the pump's stream ring (KTD2). A
 * tearing slot reports an empty source so the compositor
 * zero-contributes after at most one residual period (KTD3). */
static int fabric_lease_fill_source(uint32_t slot_index,
	struct audio_fabric_source *source)
{
	struct audio_fabric_slot *s = fabric_slot(slot_index);
	const struct audio_fabric_lease *l;

	memset(source, 0, sizeof(*source));
	if (s == NULL)
		return 1;
	l = &s->lease;
	if (l->ring == NULL || l->tearing)
		return 1;
	source->ring = l->ring;
	source->capacity = l->capacity;
	source->produced_bytes = fabric_lease_read_cursor(&l->produced);
	source->staged_bytes = l->staged; /* ISR-only writer; tearing
	 * is impossible against this reader (see fabric_lease_read_cursor
	 * for the cross-context reads that still need the retry). */
	source->sample_rate = 48000U;
	source->channels = 2U;
	source->sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	return 1;
}

static int fabric_lease_advance(uint32_t slot_index, uint32_t bytes)
{
	struct audio_fabric_slot *s = fabric_slot(slot_index);
	struct audio_fabric_lease *l;
	/* produced is main-loop-written (SUBMIT): tearing-safe read.
	 * staged is this ISR's own cursor: plain. */
	uint64_t produced;
	uint64_t staged;

	if (s == NULL)
		return 0;
	l = &s->lease;
	produced = fabric_lease_read_cursor(&l->produced);
	staged = l->staged;
	if (l->ring == NULL || l->tearing)
		return 0;
	if (bytes > produced - staged)
		return 0;
	l->staged = staged + bytes;
	return 1;
}

#define DEFINE_FABRIC_LEASE_OPS(fn, IDX)                                 \
static int fn##_snapshot(struct audio_fabric_source *source)             \
{                                                                        \
	return fabric_lease_fill_source((IDX), source);                  \
}                                                                        \
static int fn##_stage(uint32_t bytes)                                    \
{                                                                        \
	return fabric_lease_advance((IDX), bytes);                       \
}                                                                        \
static const struct audio_fabric_producer_ops fn = {                    \
	.snapshot = fn##_snapshot,                                        \
	.stage = fn##_stage,                                              \
};

DEFINE_FABRIC_LEASE_OPS(fabric_lease_ops_mailbox, AUDIO_FABRIC_SLOT_MAILBOX)
DEFINE_FABRIC_LEASE_OPS(fabric_lease_ops_reserved, AUDIO_FABRIC_SLOT_RESERVED)

static const struct audio_fabric_producer_ops *fabric_lease_ops(uint32_t slot)
{
	return slot == AUDIO_FABRIC_SLOT_MAILBOX
		? &fabric_lease_ops_mailbox : &fabric_lease_ops_reserved;
}

/*
 * Lease plane (plan U3). Everything below runs in main-loop context
 * on core 0; the compositor touches the lease only through the
 * producer ops above.
 */

int audio_fabric_lease_begin(uint32_t slot, uint32_t identity,
	uint32_t gain, uint32_t *lease,
	struct audio_fabric_lease_grant *grant)
{
	struct audio_fabric_slot *s = fabric_slot(slot);
	struct audio_fabric_lease *l;
	struct audio_scene_lease_gain_result composed;
	uint8_t *ring;

	if (lease == NULL)
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	*lease = 0U;
	if (grant != NULL) {
		grant->gain = 0U;
		grant->bounded = 0U;
	}
	/* Admission policy: fabric_slot_leaseable above. */
	if (!fabric_slot_leaseable(slot))
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	/* R11: the lease gain is not a free-floating mixer scale -- it
	 * composes against the enforced ceiling under the active scene
	 * (audio_scene.c), exactly like an owner trim: a request above
	 * the composition is bounded and REPORTED through the grant,
	 * never silently clamped. The mailbox layer rejects gain > 255
	 * before this; the fabric re-arms the same admission policy. */
	if (audio_scene_lease_gain_compose(gain, &composed) != 0)
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	if (s->lease.ring != NULL || s->attached)
		return AUDIO_FABRIC_LEASE_EBUSY;
	if (audio_fabric_ownership() == AUDIO_FABRIC_LEGACY_EXCLUSIVE)
		return AUDIO_FABRIC_LEASE_ELEGACY;
	ring = g_fabric_lease_ring[slot];
	if (ring == NULL)
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	/* Fresh card-side ring: the ghost-period bound (R5) depends on a
	 * lease never reading a previous lease's residual samples. */
	fabric_lease_ring_zero(ring, AUDIO_FABRIC_LEASE_RING_BYTES);
	if (!audio_fabric_producer_attach(slot, fabric_lease_ops(slot)))
		return AUDIO_FABRIC_LEASE_EBUSY;
	l = &s->lease;
	l->ring = ring;
	l->capacity = AUDIO_FABRIC_LEASE_RING_BYTES;
	l->identity = identity;
	l->generation = s->epoch;   /* attach bumped it onto this lease */
	l->state = (uint8_t)AUDIO_FABRIC_SLOT_STATE_LEASED;
	s->gain = (uint16_t)composed.applied;
	if (grant != NULL) {
		grant->gain = composed.applied;
		grant->bounded = composed.bounded;
	}
	*lease = AUDIO_FABRIC_LEASE_HANDLE(slot, l->generation);
	return AUDIO_FABRIC_LEASE_OK;
}

int audio_fabric_lease_submit(uint32_t handle, const uint8_t *src,
	uint32_t length, uint32_t *bytes_consumed)
{
	uint32_t slot = AUDIO_FABRIC_LEASE_HANDLE_SLOT(handle);
	struct audio_fabric_slot *s = fabric_slot(slot);
	struct audio_fabric_lease *l;
	uint64_t produced;
	uint64_t staged;
	uint32_t space;
	uint32_t accept;
	uint32_t offset;
	uint32_t first;

	if (bytes_consumed == NULL)
		return AUDIO_FABRIC_LEASE_EHANDLE;
	*bytes_consumed = 0U;
	if (s == NULL)
		return AUDIO_FABRIC_LEASE_EHANDLE;
	l = &s->lease;
	/* Stale handle (released lease, warm reset, garbage): epoch
	 * mismatch or a slot no lease holds -- nothing is written. */
	if (l->ring == NULL || l->tearing ||
	    l->generation != AUDIO_FABRIC_LEASE_HANDLE_EPOCH(handle))
		return AUDIO_FABRIC_LEASE_EHANDLE;
	if (length == 0U)
		return AUDIO_FABRIC_LEASE_OK;
	if (src == NULL)
		return AUDIO_FABRIC_LEASE_EHANDLE;
	staged = fabric_lease_read_cursor(&l->staged);
	produced = l->produced;   /* this thread is the sole writer */
	/* A torn staged read can land ahead of produced: clamp it back so
	 * the tear degrades to zero-accept instead of an oversized accept. */
	if (staged > produced)
		staged = produced;
	space = l->capacity - (uint32_t)(produced - staged);
	accept = length < space ? length : space;
	if (accept == 0U)
		return AUDIO_FABRIC_LEASE_OK;   /* BUSY at the mailbox layer */
	/* Reader-side invalidate first: the client wrote these bytes
	 * through the aperture, non-coherent with this cache. */
	Xil_DCacheInvalidateRange((INTPTR)src, accept);
	offset = (uint32_t)(produced % l->capacity);
	first = l->capacity - offset;
	if (first > accept)
		first = accept;
	memcpy(l->ring + offset, src, first);
	Xil_DCacheFlushRange((INTPTR)(l->ring + offset), first);
	if (accept > first) {
		memcpy(l->ring, src + first, accept - first);
		Xil_DCacheFlushRange((INTPTR)l->ring, accept - first);
	}
	/* Publish the write cursor only after the flushed copy: the
	 * compositor's snapshot reads it with the same monotonic
	 * single-writer discipline as the stream ring. */
	l->produced = produced + accept;
	*bytes_consumed = accept;
	if (l->state == (uint8_t)AUDIO_FABRIC_SLOT_STATE_LEASED) {
		/* First accepted bytes: LEASED -> ACTIVE. Re-arm the fill
		 * frontier only when this slot revives an otherwise idle
		 * fabric -- joining a live mix must never rewind the
		 * shared frontier (the other producers' staged periods
		 * would be re-filled and their staging double-counted). */
		if (!audio_fabric_others_live(slot))
			audio_fabric_producer_restart(slot);
		l->state = (uint8_t)AUDIO_FABRIC_SLOT_STATE_ACTIVE;
		audio_fabric_producer_go_live(slot);
	}
	return AUDIO_FABRIC_LEASE_OK;
}

int audio_fabric_lease_release(uint32_t handle)
{
	uint32_t slot = AUDIO_FABRIC_LEASE_HANDLE_SLOT(handle);
	struct audio_fabric_slot *s = fabric_slot(slot);
	uint32_t epoch = AUDIO_FABRIC_LEASE_HANDLE_EPOCH(handle);
	struct audio_fabric_lease *l;

	if (s == NULL || !fabric_slot_leaseable(slot))
		return AUDIO_FABRIC_LEASE_EHANDLE;
	l = &s->lease;
	if (l->ring == NULL) {
		/* Idempotent surrender: BEGIN and RELEASE each bump the
		 * epoch, so the handle of the slot's immediately previous
		 * lease is exactly one behind (SDK contract: release of
		 * an already-released lease completes OK). */
		return s->epoch == epoch + 1U ? AUDIO_FABRIC_LEASE_OK
		                              : AUDIO_FABRIC_LEASE_EHANDLE;
	}
	if (l->generation != epoch)
		return AUDIO_FABRIC_LEASE_EHANDLE;
	/* Quiesce-then-bump (KTD3): mark the slot tearing so the
	 * compositor zero-contributes after at most one residual period
	 * (the ghost bound), bump the epoch so stale handles die now
	 * rather than at the next BEGIN, then zero the ring cursors and
	 * free the slot (the drop also clears the lease state). */
	l->tearing = 1U;
	s->epoch++;
	audio_fabric_producer_detach(slot);
	return AUDIO_FABRIC_LEASE_OK;
}

int audio_fabric_slot_state(uint32_t slot, uint32_t pump_identity,
	int hold_reset, struct audio_fabric_slot_state *out)
{
	struct audio_fabric_slot *s = fabric_slot(slot);
	const struct audio_fabric_lease *l;

	if (s == NULL || out == NULL)
		return 0;
	memset(out, 0, sizeof(*out));
	out->lease = 0xffffffffU;
	if (slot == AUDIO_FABRIC_SLOT_PUMP) {
		/* The pump slot is never leased: FREE / LEASED (bound but
		 * paused) / ACTIVE follows the playback bind state. */
		out->state = !s->attached ? AUDIO_FABRIC_SLOT_STATE_FREE
			: (s->live ? AUDIO_FABRIC_SLOT_STATE_ACTIVE
			           : AUDIO_FABRIC_SLOT_STATE_LEASED);
		out->identity = s->attached
			? pump_identity : SDK_AUDIO_METER_IDENTITY_UNKNOWN;
		out->written_bytes = s->source.produced_bytes;
		out->consumed_bytes = s->source.staged_bytes;
		out->underruns = s->underruns;
		/* Peak/clip stay 0: the pump's telemetry lives with the
		 * scene meter, which already scans the mixed output. */
		out->generation = s->epoch;
		return 1;
	}
	l = &s->lease;
	if (l->ring != NULL) {
		out->state = l->state ==
			(uint8_t)AUDIO_FABRIC_SLOT_STATE_ACTIVE
			? AUDIO_FABRIC_SLOT_STATE_ACTIVE
			: AUDIO_FABRIC_SLOT_STATE_LEASED;
		out->identity = l->identity;
		out->lease = AUDIO_FABRIC_LEASE_HANDLE(slot, l->generation);
		out->written_bytes = l->produced;
		out->consumed_bytes = fabric_lease_read_cursor(&l->staged);
		out->peak = l->peak;
		out->clips = l->clips;
		if (hold_reset)
			s->lease.peak_reset = 1U;
	} else {
		out->state = AUDIO_FABRIC_SLOT_STATE_FREE;
		out->identity = SDK_AUDIO_METER_IDENTITY_UNKNOWN;
	}
	/* Raw epochs stay firmware-internal: the reported generation is
	 * the word embedded in the (current or most recent) lease
	 * handle. */
	out->generation = s->epoch;
	out->underruns = s->underruns;
	return 1;
}
