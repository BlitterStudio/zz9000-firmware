/*
 * ZZ9000AX audio fabric compositor: direct-ring lease plane (plan U3).
 *
 * Main-loop lifecycle on core 0 (SDK_OP_AUDIO_RING_ACQUIRE / _RELEASE
 * handlers in sdk_mailbox.c) plus one scan per compositor ISR
 * (fabric_lease_isr_tick). The SDK producer writes PCM and a seqlock
 * producer line straight into its board-visible grant (memorymap.h
 * direct-ring reservations); the lease plane validates that line,
 * exposes the ring to the compositor through the producer ops
 * trampolines (KTD2), consumes complete periods only, and publishes
 * consumed credits on the firmware-owned line as the DMA retires
 * periods (R7). Copy-submit is never on the data path. The compositor
 * core (audio_fabric.c) owns the slot table; this TU reaches it only
 * through the narrow seams in audio_fabric_internal.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "audio_fabric.h"
#include "audio_fabric_internal.h"
#include "audio_scene.h"
#include "memorymap.h"
#include "sdk_aperture_layout.h"
#include "sdk_mailbox.h"
#include "xil_cache.h"

/* Heartbeat budget (R11/R13): a producer token that goes this stale
 * revokes the lease. One ISR tick is SDK_AUDIO_RING_PERIOD_US (20 ms),
 * so the default tolerates a mailbox poll round-trip plus scheduler
 * latency without ever approaching the TX frontier horizon. */
#define FABRIC_RING_HEARTBEAT_TIMEOUT_MS 1000U
#define FABRIC_RING_TICK_MS \
	(SDK_AUDIO_RING_PERIOD_US / 1000U)

/* Seqlock read attempts per tick. A producer mid-update (odd sequence)
 * or writing faster than firmware reads (torn twice in a row) simply
 * isolates the slot for one pass; four is far beyond anything a
 * well-formed 20-ms producer can produce. */
#define FABRIC_RING_SEQLOCK_ATTEMPTS 4U

/* Non-volatile big-endian word accessors for the shared control block:
 * sdk_mailbox.h's helpers take plain byte pointers, the control lines
 * are firmware-shared memory read through volatile views. */
static inline uint32_t fabric_ring_get_be32(const volatile uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void fabric_ring_put_be32(volatile uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/*
 * Grant map (U2): the board-visible direct-ring reservations for the
 * active bus mode. Zorro III exposes two slot blocks above the audio
 * scratch; Zorro II exposes one compact grant, but only while the
 * generation-2 aperture contract is valid AND acknowledged -- legacy
 * Zorro II bitstreams map nothing and acquire completes UNSUPPORTED.
 * Board-visible offsets are ARM addresses minus ADDR_ADJ.
 */
struct fabric_ring_map {
	uint8_t *ring[AUDIO_FABRIC_SLOT_COUNT];    /* [1..slot_count] */
	uint8_t *control[AUDIO_FABRIC_SLOT_COUNT];
	uint32_t capacity;
	uint32_t slot_count;   /* leaseable slots (Z3: 2, Z2: 1) */
	uint8_t zorro2;
};

/* Host-test seam: the grants are redirected into test-owned buffers
 * (Z3-shaped: two slots) and the heartbeat timeout shrinks so
 * revocation scenarios run in ticks. */
#ifdef AUDIO_FABRIC_HOST_TEST
static struct fabric_ring_map g_ring_host_map;
static uint32_t g_ring_heartbeat_timeout_ms =
	FABRIC_RING_HEARTBEAT_TIMEOUT_MS;

void audio_fabric_host_set_ring_grant(uint32_t slot, uint8_t *ring,
	uint8_t *control, uint32_t capacity)
{
	if (slot == 0U || slot >= AUDIO_FABRIC_SLOT_COUNT)
		return;
	g_ring_host_map.ring[slot] = ring;
	g_ring_host_map.control[slot] = control;
	g_ring_host_map.capacity = capacity;
	g_ring_host_map.slot_count = AUDIO_FABRIC_SLOT_COUNT - 1U;
	g_ring_host_map.zorro2 = 0U;
}

void audio_fabric_host_set_heartbeat_timeout(uint32_t ms)
{
	g_ring_heartbeat_timeout_ms = ms;
}

#define FABRIC_RING_HEARTBEAT_TIMEOUT_NOW \
	g_ring_heartbeat_timeout_ms
#else
#define FABRIC_RING_HEARTBEAT_TIMEOUT_NOW \
	FABRIC_RING_HEARTBEAT_TIMEOUT_MS
#endif

static int fabric_ring_map(struct fabric_ring_map *out)
{
	memset(out, 0, sizeof(*out));
#ifdef AUDIO_FABRIC_HOST_TEST
	if (g_ring_host_map.ring[AUDIO_FABRIC_SLOT_MAILBOX] != NULL) {
		*out = g_ring_host_map;
		return 1;
	}
	return 0;
#else
	if (sdk_aperture_runtime_is_zorro3()) {
		out->control[AUDIO_FABRIC_SLOT_MAILBOX] =
			(uint8_t *)SDK_AUDIO_DIRECT_RING_Z3_CONTROL1_ADDRESS;
		out->ring[AUDIO_FABRIC_SLOT_MAILBOX] =
			(uint8_t *)SDK_AUDIO_DIRECT_RING_Z3_RING1_ADDRESS;
		out->control[AUDIO_FABRIC_SLOT_RESERVED] =
			(uint8_t *)SDK_AUDIO_DIRECT_RING_Z3_CONTROL2_ADDRESS;
		out->ring[AUDIO_FABRIC_SLOT_RESERVED] =
			(uint8_t *)SDK_AUDIO_DIRECT_RING_Z3_RING2_ADDRESS;
		out->capacity = SDK_AUDIO_DIRECT_RING_Z3_CAPACITY_BYTES;
		out->slot_count = SDK_AUDIO_DIRECT_RING_Z3_SLOTS;
		out->zorro2 = 0U;
		return 1;
	}
	/* Zorro II: only the acknowledged generation-2 aperture contract
	 * places the reservation inside a window the host can reach (the
	 * same gate sdk_aperture_host_window_address applies). */
	if ((sdk_aperture_runtime_flags() &
	     (SDK_APERTURE_FLAG_VALID | SDK_APERTURE_FLAG_ACKED)) !=
	    (SDK_APERTURE_FLAG_VALID | SDK_APERTURE_FLAG_ACKED))
		return 0;
	{
		const struct sdk_aperture_layout *layout =
			sdk_aperture_runtime_layout();
		uint32_t control = layout->audio.base -
			SDK_AUDIO_DIRECT_RING_Z2_RESERVE_SIZE;

		out->control[AUDIO_FABRIC_SLOT_MAILBOX] =
			(uint8_t *)(control + ADDR_ADJ);
		out->ring[AUDIO_FABRIC_SLOT_MAILBOX] =
			out->control[AUDIO_FABRIC_SLOT_MAILBOX] +
			SDK_AUDIO_RING_CONTROL_SIZE;
	}
	out->capacity = SDK_AUDIO_DIRECT_RING_Z2_CAPACITY_BYTES;
	out->slot_count = SDK_AUDIO_DIRECT_RING_Z2_SLOTS;
	out->zorro2 = 1U;
	return 1;
#endif
}

/* Admission policy (R4/R5): slot 0 is the pump -- live exactly while
 * an SDK playback bind exists and never leaseable; slots beyond the
 * active grant map do not exist on this bus mode. */
static int fabric_slot_leaseable(uint32_t slot,
	const struct fabric_ring_map *map)
{
	if (slot == AUDIO_FABRIC_SLOT_PUMP ||
	    slot >= AUDIO_FABRIC_SLOT_COUNT)
		return 0;
	return slot <= map->slot_count;
}

/* Post-lease outcome record (per leaseable slot): survives the
 * compositor's slot drop so RELEASE idempotency and STATE_GET stay
 * coherent after an ISR-side revocation wiped the slot. */
static struct fabric_ring_record {
	uint32_t generation;
	uint32_t identity;
	uint32_t heartbeat_ms;
	uint8_t revoked;
	uint8_t status;         /* SDK_AUDIO_RING_STATUS_* at revoke */
} g_ring_record[AUDIO_FABRIC_SLOT_COUNT];

static void fabric_ring_record(uint32_t slot, uint32_t generation,
	uint32_t identity, uint32_t heartbeat_ms, uint8_t revoked,
	uint8_t status)
{
	g_ring_record[slot].generation = generation;
	g_ring_record[slot].identity = identity;
	g_ring_record[slot].heartbeat_ms = heartbeat_ms;
	g_ring_record[slot].revoked = revoked;
	g_ring_record[slot].status = status;
}

/* Zero a card-side grant and push it to DRAM: the compositor reads the
 * ring with reader-side invalidates and the producer reads the control
 * block through the aperture, so the zeroing must survive both. */
static void fabric_ring_zero(uint8_t *ring, uint8_t *control,
	uint32_t capacity)
{
	memset(control, 0, SDK_AUDIO_RING_CONTROL_SIZE);
	memset(ring, 0, capacity);
	Xil_DCacheFlushRange((INTPTR)control, SDK_AUDIO_RING_CONTROL_SIZE);
	Xil_DCacheFlushRange((INTPTR)ring, capacity);
}

void fabric_lease_reset_rings(void)
{
	struct fabric_ring_map map;
	uint32_t slot;

	memset(g_ring_record, 0, sizeof(g_ring_record));
	if (!fabric_ring_map(&map))
		return;
	for (slot = AUDIO_FABRIC_SLOT_MAILBOX; slot <
	     AUDIO_FABRIC_SLOT_COUNT; slot++) {
		if (map.ring[slot] != NULL)
			fabric_ring_zero(map.ring[slot], map.control[slot],
				map.capacity);
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

/*
 * Producer-line read (R4/R6): reader-side invalidate, seqlock-stable
 * snapshot, invalidate + sequence re-check so an update landing mid-
 * read is detected (the cached re-read alone could not see it). A
 * producer mid-update or writing faster than firmware reads simply
 * fails the read; the tick isolates the slot for one pass.
 */
struct fabric_ring_producer_view {
	uint64_t write;
	uint32_t generation;
	uint32_t heartbeat;
	uint32_t flags;
};

static int fabric_ring_read_producer(struct audio_fabric_lease *l,
	struct fabric_ring_producer_view *view)
{
	volatile struct SDKAudioRingProducerLine *p =
		(volatile struct SDKAudioRingProducerLine *)l->control;
	uint32_t attempt;

	for (attempt = 0U; attempt < FABRIC_RING_SEQLOCK_ATTEMPTS;
	     attempt++) {
		uint32_t before;
		uint32_t after;

		Xil_DCacheInvalidateRange((INTPTR)p,
			SDK_AUDIO_RING_CONTROL_LINE_SIZE);
		before = fabric_ring_get_be32(p->sequence);
		if ((before & 1U) != 0U)
			continue;   /* update in flight */
		view->generation = fabric_ring_get_be32(p->generation);
		view->write =
			((uint64_t)fabric_ring_get_be32(p->write_cursor_hi)
			 << 32) |
			(uint64_t)fabric_ring_get_be32(p->write_cursor_lo);
		view->heartbeat = fabric_ring_get_be32(p->heartbeat);
		view->flags = fabric_ring_get_be32(p->flags);
		Xil_DCacheInvalidateRange((INTPTR)p,
			SDK_AUDIO_RING_CONTROL_LINE_SIZE);
		after = fabric_ring_get_be32(p->sequence);
		if (before == after)
			return 1;   /* stable snapshot */
	}
	return 0;
}

/*
 * Firmware-line publication (R7): the lease's only publishers are the
 * acquire path (main loop, strictly before the lease can go live) and
 * the ISR's credit/revocation paths (after), so writers never actually
 * contend -- but the sequence discipline is self-healing anyway
 * (mirroring the corrected SDK helpers): the next commit is rounded
 * down to even, the line opens at next|1 (always odd) and commits at
 * next (always even), so a line caught at any parity still converges
 * and a producer can never see a false-stable snapshot mid-update.
 * Sequence goes odd, the payload lands and is flushed to DRAM, then
 * the sequence goes even and is flushed -- a producer reading through
 * the aperture sees either the old or the new credits, never a torn
 * pair. Redundant publications (no credit or status movement) are
 * skipped to keep the ISR cheap.
 */
static void fabric_ring_publish(struct audio_fabric_lease *l,
	uint32_t status)
{
	volatile struct SDKAudioRingFirmwareLine *fw =
		(volatile struct SDKAudioRingFirmwareLine *)
			(l->control + SDK_AUDIO_RING_CONTROL_LINE_SIZE);
	uint32_t next;
	if (l->published_credits == l->credited &&
	    l->published_status == status)
		return;
	next = (fabric_ring_get_be32(fw->sequence) + 2U) & ~1U;
	fabric_ring_put_be32(fw->sequence, next | 1U);
	Xil_DCacheFlushRange((INTPTR)fw,
		SDK_AUDIO_RING_CONTROL_LINE_SIZE);
	fabric_ring_put_be32(fw->generation, l->generation);
	fabric_ring_put_be32(fw->consumed_cursor_hi,
		(uint32_t)(l->credited >> 32));
	fabric_ring_put_be32(fw->consumed_cursor_lo,
		(uint32_t)l->credited);
	fabric_ring_put_be32(fw->status, status);
	Xil_DCacheFlushRange((INTPTR)fw,
		SDK_AUDIO_RING_CONTROL_LINE_SIZE);
	fabric_ring_put_be32(fw->sequence, next);
	Xil_DCacheFlushRange((INTPTR)fw,
		SDK_AUDIO_RING_CONTROL_LINE_SIZE);
	l->published_credits = l->credited;
	l->published_status = status;
}

/* Heartbeat aging (R11): one tick per compositor ISR; any token change
 * (including a paused producer's -- PAUSED never suspends the
 * heartbeat, R12) refreshes the age. */
static void fabric_ring_heartbeat_age(struct audio_fabric_lease *l)
{
	if (l->heartbeat_ms >
	    UINT32_MAX - FABRIC_RING_TICK_MS)
		l->heartbeat_ms = UINT32_MAX;
	else
		l->heartbeat_ms += FABRIC_RING_TICK_MS;
}

/*
 * Revocation (R13/KTD4): heartbeat expiry or a cursor fault
 * invalidates the generation. Consumption stops, the reason is
 * published under the dying generation, the queued-contribution
 * rebuild is requested while the slot's period tags still live, the
 * epoch bumps so the generation never validates again, and the slot
 * frees for a fresh acquire without a card reset. Peers are untouched
 * (the drop is this slot only; audio_fabric_producer_detach silences
 * the ring just when this was the last producer).
 */
static void fabric_ring_revoke(uint32_t slot, uint32_t status)
{
	struct audio_fabric_slot *s = fabric_slot(slot);
	struct audio_fabric_lease *l = &s->lease;
	uint32_t identity = l->identity;
	uint32_t heartbeat_ms = l->heartbeat_ms;

	l->line_valid = 0U;
	l->tearing = 1U;
	fabric_ring_publish(l, status);
	audio_fabric_request_rebuild(slot);
	s->epoch++;
	/* Record under the POST-bump epoch: STATE_GET reports REVOKED
	 * while the slot's epoch still stands here, and a fresh acquire
	 * (which bumps again) moves the report on to the new lease. */
	fabric_ring_record(slot, s->epoch, identity, heartbeat_ms, 1U,
		(uint8_t)status);
	audio_fabric_producer_detach(slot);
}

/*
 * One direct-ring scan per compositor ISR (before the retire loop):
 * validate every wired lease's producer line, age its heartbeat,
 * revoke dead leases, and move a first-published lease LEASED ->
 * ACTIVE. Malformed lines (unreadable seqlock, foreign generation,
 * unknown flags) isolate the slot to silence for the pass -- never a
 * fault of the lease, never a peer effect. Integer-only, ISR context.
 */
void fabric_lease_isr_tick(void)
{
	uint32_t slot;

	for (slot = AUDIO_FABRIC_SLOT_MAILBOX;
	     slot < AUDIO_FABRIC_SLOT_COUNT; slot++) {
		struct audio_fabric_slot *s = fabric_slot(slot);
		struct audio_fabric_lease *l = &s->lease;
		struct fabric_ring_producer_view view;

		if (l->ring == NULL || l->tearing)
			continue;
		l->line_valid = 0U;
		if (!fabric_ring_read_producer(l, &view)) {
			fabric_ring_heartbeat_age(l);
			continue;
		}
		if (view.generation != l->generation) {
			/* Not this lease's line (R4): a fresh lease before
			 * the first publication, or a ghost line after a
			 * revoke the producer has not observed yet. */
			fabric_ring_heartbeat_age(l);
			continue;
		}
		if ((view.flags &
		     ~SDK_AUDIO_RING_PRODUCER_FLAG_KNOWN) != 0U) {
			/* Rejected line: unknown flag bits. */
			fabric_ring_heartbeat_age(l);
			continue;
		}
		/* Write-distance rule: write - credited within capacity,
		 * backward is a fault. Staged-but-uncredited bytes stay
		 * readable for the queued-period rebuild, so the credit
		 * cursor (not staging) bounds the producer. */
		if (view.write < l->credited ||
		    view.write - l->credited > l->capacity) {
			fabric_ring_revoke(slot,
				SDK_AUDIO_RING_STATUS_FAULT_CURSOR);
			continue;
		}
		if (view.heartbeat != l->heartbeat_token) {
			l->heartbeat_token = view.heartbeat;
			l->heartbeat_ms = 0U;
		} else {
			fabric_ring_heartbeat_age(l);
		}
		if (l->heartbeat_ms >= FABRIC_RING_HEARTBEAT_TIMEOUT_NOW) {
			fabric_ring_revoke(slot,
				SDK_AUDIO_RING_STATUS_REVOKED_HEARTBEAT);
			continue;
		}
		l->paused = (view.flags &
			     SDK_AUDIO_RING_PRODUCER_FLAG_PAUSED) != 0U;
		l->write_cursor = view.write;
		l->line_valid = 1U;
		if (l->state == (uint8_t)AUDIO_FABRIC_SLOT_STATE_LEASED) {
			/* First valid publication: LEASED -> ACTIVE.
			 * Re-arm the fill frontier only when this slot
			 * revives an otherwise idle fabric -- joining a
			 * live mix must never rewind the shared frontier
			 * (the other producers' staged periods would be
			 * re-filled and their staging double-counted). */
			if (!audio_fabric_others_live(slot))
				audio_fabric_producer_restart(slot);
			l->state = (uint8_t)AUDIO_FABRIC_SLOT_STATE_ACTIVE;
			audio_fabric_producer_go_live(slot);
		}
	}
}

/* Lease producer ops: the granted ring through the same
 * snapshot/stage discipline as the pump's stream ring (KTD2). The
 * snapshot serves the tick's cached line view; staging advances the
 * lease's consumed cursor (the ISR is its single writer) and retiring
 * publishes the DMA-confirmed credits. */
static int fabric_ring_source_snapshot(uint32_t slot_index,
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
	if (!l->line_valid) {
		/* Malformed or stale line for this pass: isolate the
		 * slot -- silence without starving it (a fault, not an
		 * underrun). */
		source->faulted = 1U;
		return 1;
	}
	source->ring = l->ring;
	source->capacity = l->capacity;
	source->produced_bytes = l->write_cursor;
	source->staged_bytes = l->consumed;
	source->sample_rate = 48000U;
	source->channels = 2U;
	source->sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	/* PAUSED is intentional silence: cursor progress is suppressed
	 * (the fill loop never pulls) without an underrun. */
	source->faulted = l->paused != 0U;
	return 1;
}

static int fabric_ring_source_stage(uint32_t slot_index, uint32_t bytes)
{
	struct audio_fabric_slot *s = fabric_slot(slot_index);
	struct audio_fabric_lease *l;

	if (s == NULL)
		return 0;
	l = &s->lease;
	if (l->ring == NULL || l->tearing || !l->line_valid)
		return 0;
	if (bytes > l->write_cursor - l->consumed)
		return 0;
	l->consumed += bytes;
	return 1;
}

static void fabric_ring_source_retire(uint32_t slot_index, uint32_t bytes)
{
	struct audio_fabric_slot *s = fabric_slot(slot_index);
	struct audio_fabric_lease *l;

	if (s == NULL || bytes == 0U)
		return;
	l = &s->lease;
	if (l->ring == NULL || l->tearing)
		return;
	l->credited += bytes;
	if (l->credited > l->consumed)
		l->credited = l->consumed;
	/* Credit publication is DMA-confirmed (complete periods, R7) and
	 * always trails staging, so the bytes a queued-period rebuild may
	 * re-read are contractually still intact in the producer ring. */
	fabric_ring_publish(l, SDK_AUDIO_RING_STATUS_OK);
}

#define DEFINE_FABRIC_RING_OPS(fn, IDX)                                  \
static int fn##_snapshot(struct audio_fabric_source *source)             \
{                                                                        \
	return fabric_ring_source_snapshot((IDX), source);              \
}                                                                        \
static int fn##_stage(uint32_t bytes)                                    \
{                                                                        \
	return fabric_ring_source_stage((IDX), bytes);                  \
}                                                                        \
static void fn##_retire(uint32_t bytes)                                  \
{                                                                        \
	fabric_ring_source_retire((IDX), bytes);                        \
}                                                                        \
static const struct audio_fabric_producer_ops fn = {                    \
	.snapshot = fn##_snapshot,                                      \
	.stage = fn##_stage,                                            \
	.retire = fn##_retire,                                          \
};

DEFINE_FABRIC_RING_OPS(fabric_ring_ops_mailbox, AUDIO_FABRIC_SLOT_MAILBOX)
DEFINE_FABRIC_RING_OPS(fabric_ring_ops_reserved, AUDIO_FABRIC_SLOT_RESERVED)

static const struct audio_fabric_producer_ops *fabric_ring_ops(uint32_t slot)
{
	return slot == AUDIO_FABRIC_SLOT_MAILBOX
		? &fabric_ring_ops_mailbox : &fabric_ring_ops_reserved;
}

/*
 * Lease plane lifecycle. Acquire and release run in main-loop context
 * on core 0; the compositor touches the lease only through the
 * producer ops and the tick above.
 */

int audio_fabric_ring_acquire(uint32_t slot, uint32_t identity,
	uint32_t gain, struct audio_fabric_ring_grant *grant)
{
	struct fabric_ring_map map;
	struct audio_fabric_slot *s;
	struct audio_fabric_lease *l;
	struct audio_scene_lease_gain_result composed;
	uint8_t *ring;
	uint8_t *control;

	if (grant != NULL) {
		memset(grant, 0, sizeof(*grant));
	}
	if (!fabric_ring_map(&map))
		return AUDIO_FABRIC_LEASE_ENO_MAP;
	if (!fabric_slot_leaseable(slot, &map))
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	s = fabric_slot(slot);
	if (s == NULL)
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	l = &s->lease;
	/* R11: the lease gain composes against the enforced ceiling under
	 * the active scene (audio_scene.c), exactly like an owner trim: a
	 * request above the composition is bounded and REPORTED through
	 * the grant, never silently clamped. The mailbox layer rejects
	 * gain > 255 before this; the fabric re-arms the same policy. */
	if (audio_scene_lease_gain_compose(gain, &composed) != 0)
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	if (l->ring != NULL || s->attached)
		return AUDIO_FABRIC_LEASE_EBUSY;
	if (audio_fabric_ownership() == AUDIO_FABRIC_LEGACY_EXCLUSIVE)
		return AUDIO_FABRIC_LEASE_ELEGACY;
	ring = map.ring[slot];
	control = map.control[slot];
	/* Fresh grant (R5): the ghost-period bound depends on a lease
	 * never reading a previous lease's residual samples, and the
	 * control block never carrying a previous generation. */
	fabric_ring_zero(ring, control, map.capacity);
	if (!audio_fabric_producer_attach(slot, fabric_ring_ops(slot)))
		return AUDIO_FABRIC_LEASE_EBUSY;
	/* Wire the lease; ring arms the tick LAST (plain release store)
	 * so a concurrent ISR either sees no lease or a complete one. */
	memset(l, 0, sizeof(*l));
	l->control = control;
	l->capacity = map.capacity;
	l->identity = identity;
	l->generation = s->epoch;   /* attach bumped it onto this lease */
	l->state = (uint8_t)AUDIO_FABRIC_SLOT_STATE_LEASED;
	/* Sentinel: the zeroed block carries generation 0, so the first
	 * publication (this lease's generation, zero credits, OK) must
	 * not be skipped as redundant. */
	l->published_status = 0xffffffffU;
	fabric_ring_publish(l, SDK_AUDIO_RING_STATUS_OK);
	__asm__ __volatile__("" ::: "memory");
	l->ring = ring;
	s->gain = (uint16_t)composed.applied;
	fabric_ring_record(slot, l->generation, identity, 0U, 0U,
		SDK_AUDIO_RING_STATUS_OK);
	if (grant != NULL) {
		grant->generation = l->generation;
		grant->ring_offset = (uint32_t)(ring - (uint8_t *)ADDR_ADJ);
		grant->ring_capacity = map.capacity;
		grant->control_offset =
			(uint32_t)(control - (uint8_t *)ADDR_ADJ);
		grant->gain_applied = composed.applied;
		grant->bounded = composed.bounded;
		grant->slot_count = (uint8_t)map.slot_count;
		grant->bus_zorro2 = map.zorro2;
	}
	return AUDIO_FABRIC_LEASE_OK;
}

int audio_fabric_ring_release(uint32_t slot, uint32_t generation)
{
	struct audio_fabric_slot *s = fabric_slot(slot);
	struct fabric_ring_map map;
	struct audio_fabric_lease *l;

	if (s == NULL || slot == AUDIO_FABRIC_SLOT_PUMP)
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	if (!fabric_ring_map(&map) || !fabric_slot_leaseable(slot, &map))
		return AUDIO_FABRIC_LEASE_EBAD_SLOT;
	l = &s->lease;
	if (l->ring == NULL || l->tearing) {
		/* Idempotent surrender: the slot holds no lease, so every
		 * generation is stale (released or revoked already) --
		 * the SDK contract makes release retry-safe. */
		return AUDIO_FABRIC_LEASE_OK;
	}
	if (l->generation != generation) {
		/* A live lease only surrenders to its own grant; anything
		 * else is a stale retry (the current lease is untouched). */
		return AUDIO_FABRIC_LEASE_OK;
	}
	/* Quiesce-then-rebuild (KTD3): tearing silences the slot from
	 * this store on (snapshot/stage/retire check it first), the
	 * queued-contribution rebuild is requested while the slot's
	 * period tags still live, the epoch bump kills the generation,
	 * and the drop frees the slot. Credits already published stand:
	 * they are DMA-confirmed and monotonic; the block re-zeroes at
	 * the next acquire. */
	l->tearing = 1U;
	fabric_ring_record(slot, l->generation, l->identity,
		l->heartbeat_ms, 0U, SDK_AUDIO_RING_STATUS_OK);
	audio_fabric_request_rebuild(slot);
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
	out->heartbeat_ms = SDK_AUDIO_RING_HEARTBEAT_UNKNOWN;
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
		/* Peak/clip/heartbeat stay 0/UNKNOWN: the pump's telemetry
		 * lives with the scene meter, which already scans the
		 * mixed output. */
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
		/* Cross-context reads: the ISR is the single writer of
		 * both cursors and the heartbeat ages per tick. */
		out->written_bytes =
			fabric_lease_read_cursor(&l->write_cursor);
		out->consumed_bytes =
			fabric_lease_read_cursor(&l->credited);
		out->heartbeat_ms = l->heartbeat_ms;
		out->peak = l->peak;
		out->clips = l->clips;
		if (hold_reset)
			s->lease.peak_reset = 1U;
	} else if (g_ring_record[slot].revoked &&
		   g_ring_record[slot].generation == s->epoch) {
		/* The last lease of this slot was revoked and nothing new
		 * attached: report the invalidated generation until a
		 * fresh acquire moves the epoch on. */
		out->state = AUDIO_FABRIC_SLOT_STATE_REVOKED;
		out->identity = g_ring_record[slot].identity;
		out->heartbeat_ms = g_ring_record[slot].heartbeat_ms;
	} else {
		out->state = AUDIO_FABRIC_SLOT_STATE_FREE;
		out->identity = SDK_AUDIO_METER_IDENTITY_UNKNOWN;
	}
	/* Raw epochs stay firmware-internal: the reported generation is
	 * the slot's current lease epoch (the live lease's token, or the
	 * post-revoke epoch whose mismatch tells a stale client its
	 * grant is dead). */
	out->generation = s->epoch;
	out->underruns = s->underruns;
	return 1;
}
