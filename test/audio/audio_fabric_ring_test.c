/*
 * Host tests for the audio fabric compositor's direct-ring lease plane
 * (plan U3).
 *
 * Companion to audio_fabric_test.c (compositor/parity plane); the two
 * binaries share their harness in fabric_test_common.{h,c}. The
 * scenarios here drive the SDK-facing lease API -- RING_ACQUIRE /
 * RING_RELEASE and the per-slot state read -- against the real
 * compositor core, with the card-side grants redirected through the
 * host-test seam and a producer that publishes the seqlock producer
 * line exactly the way the SDK client does.
 *
 * Coverage:
 *   A. admission and grant shape: leaseable slots, pump/out-of-range
 *      rejection, double-acquire BUSY, legacy rejection, grant fields
 *      (generation, capacity, slot count, gain bounding reported);
 *   A2. rate-bearing admission (AHI migration): off-table rate
 *      refusal, the two-converting-producer budget across pump and
 *      leases, bypass leases always admissible, budget re-opening;
 *   B2. rate-lease conversion parity: a 44.1-kHz lease's TX periods
 *      are the qualified kernel's image of the source, credits
 *      advance in source-rate periods;
 *   B. lifecycle: LEASED -> first valid publication -> ACTIVE, TX
 *      periods carrying the producer's PCM, DMA-retirement credits
 *      published on the firmware line (seqlock-stable read), state
 *      cursors (write from the producer line, read = credited);
 *   C. malformed-line isolation (R4/R8): stuck-odd seqlock, foreign
 *      generation, unknown flags -- each faults without underruns and
 *      without touching a live peer;
 *   C2. transient seqlock-miss grace (two-client drop fix): one tick
 *      landing mid-publication holds the previous valid line view and
 *      stages PCM instead of silencing; a fresh publication
 *      revalidates;
 *   D. cursor fault (R13): backward write and over-capacity write
 *      revoke with FAULT_CURSOR, publish the reason under the dying
 *      generation, free the slot for re-acquire, leave the peer
 *      playing;
 *   E. heartbeat (R11/R12): a stale token revokes with
 *      REVOKED_HEARTBEAT; a refreshing token (even PAUSED) keeps the
 *      lease, PAUSED contributing silence without underruns;
 *   F. release (KTD3): the queued-contribution rebuild -- one pass
 *      after a mid-mix release, every TX period strictly ahead of the
 *      DMA position is rebuilt pump-only while the one period the DMA
 *      was entering may still carry the lease's audio; the timelines
 *      converge to identical pump-only rings;
 *   G. warm reset (R7): grants and control blocks zeroed, slots FREE
 *      with UNKNOWN heartbeat, fresh generations above pre-reset ones;
 *   H. cache fidelity: producer-line reads invalidate the exact
 *      control line and credit publications flush the firmware line
 *      (recording mock).
 *
 * Producer discipline mirrored from the SDK client: PCM is written
 * into the granted ring AFTER acquire (acquire zeroes the grant -- the
 * R5 ghost bound), the write cursor only ever covers whole published
 * periods, and the heartbeat token changes on every publication (a
 * constant token is a heartbeat revocation).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fabric_test_common.h"
#include "audio_convert.h"
#include "sdk_mailbox.h"
#include "xil_cache.h"

#define LEASE_PCM 7777     /* constant lease source sample */
#define PUMP_PCM 5000      /* constant peer pump sample */
#define MIXED_PCM 12777    /* saturating sum of the two */

/* ---- producer-side model (mirrors the SDK client's publisher) ---- */

static struct {
	uint32_t sequence;    /* producer-line seqlock, even at rest */
	uint32_t token;
	uint32_t generation;
} g_producer[AUDIO_FABRIC_SLOT_COUNT];

static volatile struct SDKAudioRingProducerLine *producer_line(
	uint32_t slot)
{
	return (volatile struct SDKAudioRingProducerLine *)
		(slot == AUDIO_FABRIC_SLOT_MAILBOX ? g_ring_control_a
		                                   : g_ring_control_b);
}

static volatile struct SDKAudioRingFirmwareLine *firmware_line(
	uint32_t slot)
{
	return (volatile struct SDKAudioRingFirmwareLine *)
		((slot == AUDIO_FABRIC_SLOT_MAILBOX ? g_ring_control_a
		                                    : g_ring_control_b) +
		 SDK_AUDIO_RING_CONTROL_LINE_SIZE);
}

/* Big-endian accessors for the shared (volatile) control lines;
 * sdk_mailbox.h's helpers take plain byte pointers. */
static uint32_t be32(const volatile uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32(volatile uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Publish the producer line the way the SDK client does: sequence
 * odd, big-endian fields, sequence even, token refreshed. write only
 * ever covers whole periods of already-written PCM. */
static void producer_publish(uint32_t slot, uint64_t write,
	uint32_t flags)
{
	volatile struct SDKAudioRingProducerLine *p = producer_line(slot);
	uint32_t next = (g_producer[slot].sequence + 2U) & ~1U;

	g_producer[slot].token++;
	put32(p->sequence, next | 1U);
	put32(p->generation, g_producer[slot].generation);
	put32(p->write_cursor_hi, (uint32_t)(write >> 32));
	put32(p->write_cursor_lo, (uint32_t)write);
	put32(p->heartbeat, g_producer[slot].token);
	put32(p->flags, flags);
	put32(p->sequence, next);
	g_producer[slot].sequence = next;
}

/* Leave the line mid-update (odd sequence): a malformed producer. */
static void producer_publish_stuck_odd(uint32_t slot)
{
	volatile struct SDKAudioRingProducerLine *p = producer_line(slot);

	g_producer[slot].sequence += 2U;
	put32(p->sequence, g_producer[slot].sequence | 1U);
}

/* Fill the whole granted ring with a constant S16LE stereo sample and
 * publish the full capacity: a producer that never starves for the
 * duration of a scenario (starvation semantics belong to the pump's
 * per-slot counter, tested at the compositor plane). */
static void producer_fill(uint32_t slot, int16_t value)
{
	uint8_t *ring = slot == AUDIO_FABRIC_SLOT_MAILBOX
		? g_ring_pcm_a : g_ring_pcm_b;
	uint32_t i;

	for (i = 0U; i + 1U < RING_TEST_CAPACITY; i += 2U) {
		ring[i] = (uint8_t)(uint16_t)value;
		ring[i + 1U] = (uint8_t)((uint16_t)value >> 8);
	}
	producer_publish(slot, RING_TEST_CAPACITY, 0U);
}

/* Seqlock-stable firmware-line read (the SDK credit reader). */
static void firmware_snapshot(uint32_t slot,
	struct SDKAudioRingFirmwareLine *out)
{
	volatile struct SDKAudioRingFirmwareLine *fw = firmware_line(slot);
	uint32_t before;

	do {
		before = be32(fw->sequence);
		if ((before & 1U) == 0U)
			memcpy(out, (const void *)fw, sizeof(*out));
	} while ((before & 1U) != 0U || before != be32(fw->sequence));
}

static struct audio_fabric_slot_state lease_state(uint32_t slot)
{
	struct audio_fabric_slot_state st;

	(void)audio_fabric_slot_state(
		slot, SDK_AUDIO_METER_IDENTITY_SDK_STREAM, 0, &st);
	return st;
}


/* One compositor pass: the DMA advances one period, the ISR runs. */
static void fabric_pass(void)
{
	dma_tick(1);
	audio_fabric_isr();
}

/* Every sample of one TX period equals value. */
static int period_is_constant(uint32_t index, int16_t value)
{
	const int16_t *p = (const int16_t *)(g_fabric_tx +
		index * TICK_BYTES);
	uint32_t i;

	for (i = 0U; i < TICK_BYTES / 2U; i++) {
		if (p[i] != value)
			return 0;
	}
	return 1;
}

/* Peer pump stand-by: a constant 48-kHz stereo producer on slot 0,
 * plus one arming pass so the shared fill frontier carries its full
 * horizon before any lease joins (a producer going live exactly at a
 * caught-up frontier takes the compositor's symmetric rebase bump --
 * pre-existing pump-parity semantics, not lease behavior). Returns
 * the pump's underrun count after arming as the peer baseline. */
static uint32_t pump_standby_start(void)
{
	struct audio_fabric_slot_state st;

	model_init(&g_model_b, 48000U, 2U,
		SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, PUMP_PCM);
	fabric_pump_start();
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_PUMP);
	return st.underruns;
}

static int acquire_lease_rate(uint32_t slot, uint32_t gain,
	uint32_t rate, struct audio_fabric_ring_grant *grant)
{
	/* The admission scenario deliberately passes out-of-range slots
	 * (PR #88 review): never index the producer model with them. */
	if (slot >= AUDIO_FABRIC_SLOT_COUNT)
		return audio_fabric_ring_acquire(slot,
			SDK_AUDIO_METER_IDENTITY_MEDIA, gain, rate, grant);
	memset(&g_producer[slot], 0, sizeof(g_producer[slot]));
	return audio_fabric_ring_acquire(slot,
		SDK_AUDIO_METER_IDENTITY_MEDIA, gain, rate, grant);
}

static int acquire_lease(uint32_t slot, uint32_t gain,
	struct audio_fabric_ring_grant *grant)
{
	return acquire_lease_rate(slot, gain, 0U, grant);
}

/* ---- A. admission and grant shape ---- */

static void scenario_admission(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_ring_grant bounded;
	uint32_t generation;

	fabric_reset_state();
	check(acquire_lease(AUDIO_FABRIC_SLOT_PUMP, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "admission: pump slot never leaseable", "");
	check(acquire_lease(AUDIO_FABRIC_SLOT_COUNT, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "admission: out-of-range slot rejected", "");
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK &&
	      grant.generation != 0U &&
	      grant.ring_capacity == RING_TEST_CAPACITY &&
	      grant.control_offset != grant.ring_offset &&
	      grant.slot_count == 2U &&
	      grant.bus_zorro2 == 0U &&
	      grant.gain_applied == 128U && grant.bounded == 0U,
	      "admission: slot-1 grant shape",
	      fmt("gen=%u cap=%u slots=%u gain=%u",
	          grant.generation, grant.ring_capacity,
	          grant.slot_count, grant.gain_applied));
	generation = grant.generation;
	g_lease_gain_bound = 200U;
	check(acquire_lease(AUDIO_FABRIC_SLOT_RESERVED, 255U, &bounded) ==
	      AUDIO_FABRIC_LEASE_OK &&
	      bounded.bounded != 0U && bounded.gain_applied == 200U,
	      "admission: over-ceiling gain bounded and REPORTED",
	      fmt("applied=%u", bounded.gain_applied));
	g_lease_gain_bound = 255U;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_EBUSY,
	      "admission: double acquire BUSY", "");
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		generation) == AUDIO_FABRIC_LEASE_OK &&
	      audio_fabric_ring_release(AUDIO_FABRIC_SLOT_RESERVED,
		bounded.generation) == AUDIO_FABRIC_LEASE_OK,
	      "admission: both slots release", "");
	g_legacy_active = 1;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_ELEGACY,
	      "admission: legacy-exclusive output rejected", "");
	g_legacy_active = 0;
}

/* ---- A2. rate-bearing admission and conversion budget ---- */

static void scenario_rate_admission(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_ring_grant conv;

	fabric_reset_state();
	/* Off-vocabulary rates are refused like a bad slot (the mailbox
	 * layer validates the same set; the fabric re-arms it so no
	 * internal caller can arm a geometry the fill would silence). */
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_MAILBOX, 128U, 44101U,
	                         &grant) == AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "rate admission: off-table rate refused", "");
	/* A converting pump occupies one of the two qualified
	 * conversion-bearing slots; a converting lease fills the budget,
	 * a second converting lease is refused like an occupied slot,
	 * and a bypass lease stays admissible regardless. */
	model_init(&g_model_b, 44100U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, PUMP_PCM);
	fabric_pump_start();
	fabric_pass();	/* cached pump source carries the rate now */
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_MAILBOX, 128U, 44100U,
	                         &conv) == AUDIO_FABRIC_LEASE_OK,
	      "rate admission: converting lease beside converting pump",
	      "");
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_RESERVED, 128U, 8000U,
	                         &grant) == AUDIO_FABRIC_LEASE_EBUSY,
	      "rate admission: third converting producer refused", "");
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_RESERVED, 128U, 48000U,
	                         &grant) == AUDIO_FABRIC_LEASE_OK,
	      "rate admission: bypass lease still admissible", "");
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_RESERVED,
		grant.generation) == AUDIO_FABRIC_LEASE_OK, "", "");
	/* Releasing a converting lease re-opens the budget slot. */
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		conv.generation) == AUDIO_FABRIC_LEASE_OK, "", "");
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_RESERVED, 128U, 12000U,
	                         &grant) == AUDIO_FABRIC_LEASE_OK,
	      "rate admission: budget re-opens after release", "");
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_RESERVED,
		grant.generation);
	/* The media-bind path asks the same central policy before attaching
	 * the pump. Model the reported join order directly: two converting
	 * direct-ring leases exhaust the budget, while bypass stays free. */
	fabric_reset_state();
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_MAILBOX, 128U, 44100U,
	                         &conv) == AUDIO_FABRIC_LEASE_OK,
	      "rate admission: first direct converter admitted", "");
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_RESERVED, 128U, 8000U,
	                         &grant) == AUDIO_FABRIC_LEASE_OK,
	      "rate admission: second direct converter admitted", "");
	check(!audio_fabric_conversion_admissible(32000U),
	      "rate admission: media converter refused after two leases", "");
	check(audio_fabric_conversion_admissible(48000U),
	      "rate admission: media bypass remains admissible", "");
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_RESERVED,
		grant.generation);
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		conv.generation);
}

/* ---- B2. rate-lease conversion parity (AHI migration) ---- */

static void scenario_rate_conversion(void)
{
	struct audio_fabric_ring_grant grant;
	struct SDKAudioRingFirmwareLine fw;
	struct zz_audio_convert ref;
	static int16_t src[882U * 2U];
	static int16_t out[TICK_BYTES / 2U];
	static int16_t expected[TICK_BYTES / 2U];
	uint8_t *ring = g_ring_pcm_a;
	uint64_t consumed;
	uint32_t frame;
	uint32_t pass;
	int matched = 0;

	fabric_reset_state();
	check(acquire_lease_rate(AUDIO_FABRIC_SLOT_MAILBOX, 128U, 44100U,
	                         &grant) == AUDIO_FABRIC_LEASE_OK,
	      "rate conv: acquire 44.1-kHz lease", "");
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation =
		grant.generation;
	/* A repeating 882-frame stereo pattern: period-synchronized, so
	 * once the converter history settles, every output period is the
	 * same fixed image of it and a position-independent TX search is
	 * exact. */
	for (frame = 0U; frame < 882U; frame++) {
		src[2U * frame] =
			(int16_t)(1200 + (int32_t)(frame * 7U % 3000U));
		src[2U * frame + 1U] =
			(int16_t)(-900 - (int32_t)(frame * 11U % 2500U));
	}
	for (frame = 0U; frame < RING_TEST_CAPACITY / 4U; frame++) {
		int16_t l = src[2U * (frame % 882U)];
		int16_t r = src[2U * (frame % 882U) + 1U];

		ring[frame * 4U] = (uint8_t)(uint16_t)l;
		ring[frame * 4U + 1U] = (uint8_t)((uint16_t)l >> 8);
		ring[frame * 4U + 2U] = (uint8_t)(uint16_t)r;
		ring[frame * 4U + 3U] = (uint8_t)((uint16_t)r >> 8);
	}
	/* Reference: the same qualified kernel fed the same repeating
	 * source; the third output period is the settled image. */
	zz_audio_convert_init(&ref, 44100U, 48000U);
	zz_audio_convert_stream(&ref, src, out, 882U, TICK_BYTES / 4U);
	zz_audio_convert_stream(&ref, src, out, 882U, TICK_BYTES / 4U);
	zz_audio_convert_stream(&ref, src, out, 882U, TICK_BYTES / 4U);
	memcpy(expected, out, sizeof(expected));
	/* Publish before every pass: the heartbeat token must change on
	 * every publication (R11) or the harness's 3-tick timeout revokes
	 * the lease mid-scenario. */
	for (pass = 0U; pass < 6U; pass++) {
		producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
			RING_TEST_CAPACITY, 0U);
		fabric_pass();
	}
	for (pass = 0U; pass < AUDIO_TX_BUFFER_SIZE / TICK_BYTES;
	     pass++) {
		if (memcmp(g_fabric_tx + pass * TICK_BYTES, expected,
		           TICK_BYTES) == 0)
			matched = 1;
	}
	check(matched,
	      "rate conv: TX carries the kernel image of the source", "");
	/* Credits are source-byte paced: whole 3528-byte periods. */
	firmware_snapshot(AUDIO_FABRIC_SLOT_MAILBOX, &fw);
	consumed = ((uint64_t)be32(fw.consumed_cursor_hi) << 32) |
	           be32(fw.consumed_cursor_lo);
	check(consumed >= 2U * 3528U && (consumed % 3528U) == 0U,
	      "rate conv: credits advance in source periods",
	      fmt("consumed=%llu", (unsigned long long)consumed));
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		grant.generation) == AUDIO_FABRIC_LEASE_OK, "", "");
	(void)zz_audio_convert_clips(&ref);
}

/* ---- B. lifecycle, audible periods, retirement credits ---- */

static void scenario_lifecycle(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	struct SDKAudioRingFirmwareLine fw;
	uint32_t generation;
	uint32_t tick;

	fabric_reset_state();
	g_dma_count = 0;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "lifecycle: acquire", "");
	generation = grant.generation;
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_LEASED &&
	      st.identity == SDK_AUDIO_METER_IDENTITY_MEDIA &&
	      st.generation == generation &&
	      st.written_bytes == 0U && st.consumed_bytes == 0U,
	      "lifecycle: LEASED before first publication",
	      fmt("state=%u gen=%u", st.state, st.generation));
	/* No publication yet: silent passes, no underruns, still LEASED
	 * (below the 3-tick heartbeat timeout the harness arms). */
	fabric_pass();
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_LEASED &&
	      st.underruns == 0U,
	      "lifecycle: silent but LEASED, no starvation counted",
	      fmt("state=%u underruns=%u", st.state, st.underruns));
	/* Fill the ring and publish: the next pass goes ACTIVE and the
	 * fill loop pulls real PCM. */
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.written_bytes == RING_TEST_CAPACITY,
	      "lifecycle: first publication -> ACTIVE",
	      fmt("state=%u written=%llu", st.state,
	          (unsigned long long)st.written_bytes));
	check(!period_is_constant(4U, 0) || !period_is_constant(5U, 0),
	      "lifecycle: queued periods carry producer PCM", "");
	/* Four more passes retire three whole periods (the restart at the
	 * ACTIVE pass re-arms the frontier one period ahead, so the
	 * first credit lands on the second pass after it); credits must
	 * follow on the firmware line under the lease generation. */
	for (tick = 0U; tick < 4U; tick++) {
		producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
			RING_TEST_CAPACITY, 0U);
		fabric_pass();
	}
	firmware_snapshot(AUDIO_FABRIC_SLOT_MAILBOX, &fw);
	check(be32(fw.generation) == generation &&
	      be32(fw.status) == SDK_AUDIO_RING_STATUS_OK &&
	      ((uint64_t)be32(fw.consumed_cursor_hi) << 32 |
	       be32(fw.consumed_cursor_lo)) == 3U * TICK_BYTES,
	      "lifecycle: retirement credits published on fw line",
	      fmt("gen=%u consumed=%u", be32(fw.generation),
	          (unsigned)be32(fw.consumed_cursor_lo)));
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.consumed_bytes == 3U * TICK_BYTES &&
	      st.underruns == 0U,
	      "lifecycle: state read cursor == credited",
	      fmt("consumed=%llu underruns=%u",
	          (unsigned long long)st.consumed_bytes, st.underruns));
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		generation) == AUDIO_FABRIC_LEASE_OK,
	      "lifecycle: release", "");
}

/* ---- C. malformed-line isolation (peer unaffected) ---- */

static void scenario_malformed(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	uint32_t generation;
	uint32_t period;
	uint32_t pump_baseline;

	fabric_reset_state();
	g_dma_count = 0;
	pump_baseline = pump_standby_start();
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "isolation: acquire", "");
	generation = grant.generation;
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	fabric_pass();
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "isolation: lease active in the mix",
	      fmt("state=%u underruns=%u", st.state, st.underruns));

	/* Stuck-odd seqlock: a transient publication miss. Since the
	 * grace fix the tick holds the previous valid line view and the
	 * slot keeps staging; only a persistently dead line ages out via
	 * the heartbeat. Asserted precisely in scenario_grace. */
	producer_publish_stuck_odd(AUDIO_FABRIC_SLOT_MAILBOX);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "isolation: stuck-odd line silences without starvation",
	      fmt("state=%u underruns=%u", st.state, st.underruns));
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY, 0U);
	fabric_pass();

	/* Foreign generation: ignored (silence), lease intact. */
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation + 1U;
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY, 0U);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "isolation: foreign-generation line ignored", "");
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY, 0U);
	fabric_pass();

	/* Unknown flag bits: line rejected (silence), lease intact. */
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY, 0x80000000U);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "isolation: unknown flags rejected", "");
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY, 0U);
	/* Recovery: a well-formed line mixes again, and freshly queued
	 * periods are the saturating sum of both producers. */
	fabric_pass();
	{
		int mixed = 0;

		for (period = 0U; period < AUDIO_NUM_PERIODS; period++) {
			if (period_is_constant(period, MIXED_PCM))
				mixed = 1;
		}
		check(mixed, "isolation: recovered lease mixes again", "");
	}
	st = lease_state(AUDIO_FABRIC_SLOT_PUMP);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == pump_baseline,
	      "isolation: peer pump unaffected throughout",
	      fmt("underruns=%u baseline=%u", st.underruns,
	          pump_baseline));
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		generation);
}


/* ---- C2. transient seqlock-miss grace (two-client drop fix) ---- */

static void scenario_grace(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	uint32_t generation;

	fabric_reset_state();
	g_dma_count = 0;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "grace: acquire", "");
	generation = grant.generation;
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	fabric_pass();
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "grace: lease active",
	      fmt("state=%u underruns=%u", st.state, st.underruns));
	/* One tick lands mid-publication (odd sequence): the grace path
	 * must hold the previous valid line view and stage real PCM --
	 * the exact condition behind the audible two-client dips. */
	producer_publish_stuck_odd(AUDIO_FABRIC_SLOT_MAILBOX);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "grace: transient miss keeps lease ACTIVE, no starvation",
	      fmt("state=%u underruns=%u", st.state, st.underruns));
	check(period_is_constant(0U, LEASE_PCM) ||
	      period_is_constant(1U, LEASE_PCM),
	      "grace: transient miss stages PCM, not silence", "");
	/* A fresh valid publication revalidates the line and playback
	 * continues uninterrupted. */
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY, 0U);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "grace: valid publication revalidates",
	      fmt("state=%u underruns=%u", st.state, st.underruns));
	check(period_is_constant(0U, LEASE_PCM) ||
	      period_is_constant(1U, LEASE_PCM),
	      "grace: playback continues after recovery", "");
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		generation) == AUDIO_FABRIC_LEASE_OK,
	      "grace: release", "");
}

/* ---- D. cursor-fault revocation ---- */

static void scenario_cursor_fault(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	struct SDKAudioRingFirmwareLine fw;
	uint32_t generation;
	uint32_t tick;

	fabric_reset_state();
	g_dma_count = 0;
	(void)pump_standby_start();
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "fault: acquire", "");
	generation = grant.generation;
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	/* Keep the heartbeat fresh while the lease earns credits. A lease
	 * joining a live mix never restarts the frontier, so the DMA has
	 * to walk up to the join-era fills before the first retirement
	 * credit lands -- eight passes covers the whole TX horizon. */
	for (tick = 0U; tick < 8U; tick++) {
		producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
			RING_TEST_CAPACITY, 0U);
		fabric_pass();
	}
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.consumed_bytes >= TICK_BYTES,
	      "fault: lease active with credits",
	      fmt("consumed=%llu",
	          (unsigned long long)st.consumed_bytes));
	/* Backward write: write below the credited cursor. */
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX, 0U, 0U);
	fabric_pass();
	firmware_snapshot(AUDIO_FABRIC_SLOT_MAILBOX, &fw);
	check(be32(fw.generation) == generation &&
	      be32(fw.status) == SDK_AUDIO_RING_STATUS_FAULT_CURSOR,
	      "fault: backward write publishes FAULT_CURSOR under the "
	      "dying generation",
	      fmt("gen=%u status=%u", be32(fw.generation),
	          be32(fw.status)));
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_REVOKED &&
	      st.generation != generation,
	      "fault: slot REVOKED, generation invalidated",
	      fmt("state=%u gen=%u", st.state, st.generation));
	st = lease_state(AUDIO_FABRIC_SLOT_PUMP);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE,
	      "fault: peer pump unaffected", "");
	/* Re-acquire on the freed slot; over-capacity write faults too. */
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK &&
	      grant.generation > generation,
	      "fault: revoked slot re-acquirable at a higher generation",
	      "");
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation =
		grant.generation;
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		(uint64_t)RING_TEST_CAPACITY + 2U * TICK_BYTES, 0U);
	fabric_pass();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_REVOKED,
	      "fault: write past capacity revokes", "");
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		grant.generation);
}

/* ---- E. heartbeat expiry, refresh, and PAUSED ---- */

static void scenario_heartbeat(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	struct SDKAudioRingFirmwareLine fw;
	uint32_t generation;
	uint32_t tick;

	fabric_reset_state();
	g_dma_count = 0;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "heartbeat: acquire", "");
	generation = grant.generation;
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	fabric_pass();
	/* Refreshing token: stays ACTIVE past the 3-tick timeout. */
	for (tick = 0U; tick < 6U; tick++) {
		producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
			RING_TEST_CAPACITY, 0U);
		fabric_pass();
	}
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE,
	      "heartbeat: refreshing token keeps the lease", "");
	/* Stale token: revoked after the timeout, reason published. */
	for (tick = 0U; tick < 4U; tick++)
		fabric_pass();
	firmware_snapshot(AUDIO_FABRIC_SLOT_MAILBOX, &fw);
	check(be32(fw.status) ==
		      SDK_AUDIO_RING_STATUS_REVOKED_HEARTBEAT,
	      "heartbeat: expiry publishes REVOKED_HEARTBEAT", "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_REVOKED &&
	      st.heartbeat_ms >= 3U * (SDK_AUDIO_RING_PERIOD_US / 1000U),
	      "heartbeat: REVOKED with the measured age",
	      fmt("state=%u age=%u", st.state, st.heartbeat_ms));
	/* PAUSED: a refreshing token with the pause flag keeps the lease
	 * alive while contributing silence without underruns. */
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "heartbeat: re-acquire", "");
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation =
		grant.generation;
	producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
		RING_TEST_CAPACITY,
		SDK_AUDIO_RING_PRODUCER_FLAG_PAUSED);
	fabric_pass();
	for (tick = 0U; tick < 4U; tick++) {
		producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
			RING_TEST_CAPACITY,
			SDK_AUDIO_RING_PRODUCER_FLAG_PAUSED);
		fabric_pass();
	}
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.underruns == 0U,
	      "heartbeat: PAUSED stays live, silent, unfaulted",
	      fmt("state=%u underruns=%u", st.state, st.underruns));
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		grant.generation);
}

/* ---- E2. dead producer: unreadable line still revokes (PR #88) ---- */

static void scenario_dead_line(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	struct SDKAudioRingFirmwareLine fw;
	uint32_t generation;
	uint32_t tick;

	fabric_reset_state();
	g_dma_count = 0;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "dead: acquire", "");
	generation = grant.generation;
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation = generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	fabric_pass();
	/* The producer dies mid-publication: the seqlock stays odd
	 * forever. The grace path must still age the heartbeat and revoke
	 * at the timeout -- a permanently unreadable line may not pin the
	 * slot until reset. */
	producer_publish_stuck_odd(AUDIO_FABRIC_SLOT_MAILBOX);
	for (tick = 0U; tick < 4U; tick++)
		fabric_pass();
	firmware_snapshot(AUDIO_FABRIC_SLOT_MAILBOX, &fw);
	check(be32(fw.status) ==
		      SDK_AUDIO_RING_STATUS_REVOKED_HEARTBEAT,
	      "dead: stuck-odd line still publishes REVOKED_HEARTBEAT", "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_REVOKED,
	      "dead: slot REVOKED on the unreadable line",
	      fmt("state=%u", st.state));
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		generation);
}

/* ---- F. release and the queued-period rebuild ---- */

static void run_rebuild_pass(uint8_t *pre_release, uint8_t *post_rebuild,
	uint8_t *final, int with_lease)
{
	struct audio_fabric_ring_grant grant;
	uint32_t generation = 0U;
	uint32_t tick;

	fabric_reset_state();
	g_dma_count = 0;
	(void)pump_standby_start();
	if (with_lease) {
		check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U,
			&grant) == AUDIO_FABRIC_LEASE_OK,
		      "rebuild: acquire", "");
		generation = grant.generation;
		g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation =
			generation;
		producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	}
	/* Passes 1-4: the lease mixes; pass 4 ends with a queued window
	 * of mixed periods. Pass 5 (first ISR after the release)
	 * rebuilds everything strictly ahead of the DMA position. */
	for (tick = 1U; tick <= 8U; tick++) {
		if (with_lease && tick <= 4U)
			producer_publish(AUDIO_FABRIC_SLOT_MAILBOX,
				RING_TEST_CAPACITY, 0U);
		fabric_pass();
		if (tick == 4U) {
			if (pre_release != NULL)
				memcpy(pre_release, g_fabric_tx,
					AUDIO_TX_BUFFER_SIZE);
			if (with_lease) {
				check(audio_fabric_ring_release(
					AUDIO_FABRIC_SLOT_MAILBOX,
					generation) ==
					AUDIO_FABRIC_LEASE_OK,
					"rebuild: mid-mix release", "");
			}
		}
		if (tick == 5U && post_rebuild != NULL)
			memcpy(post_rebuild, g_fabric_tx,
				AUDIO_TX_BUFFER_SIZE);
	}
	memcpy(final, g_fabric_tx, AUDIO_TX_BUFFER_SIZE);
}

static int snapshot_period_is_constant(const uint8_t *snap,
	uint32_t index, int16_t value)
{
	const int16_t *p = (const int16_t *)(snap + index * TICK_BYTES);
	uint32_t i;

	for (i = 0U; i < TICK_BYTES / 2U; i++) {
		if (p[i] != value)
			return 0;
	}
	return 1;
}

static void scenario_rebuild(void)
{
	static uint8_t pump_final[AUDIO_TX_BUFFER_SIZE];
	static uint8_t mixed_pre[AUDIO_TX_BUFFER_SIZE];
	static uint8_t mixed_post[AUDIO_TX_BUFFER_SIZE];
	static uint8_t mixed_final[AUDIO_TX_BUFFER_SIZE];
	struct audio_fabric_slot_state st;
	uint32_t period;
	int mixed_seen = 0;

	run_rebuild_pass(NULL, NULL, pump_final, 0);
	run_rebuild_pass(mixed_pre, mixed_post, mixed_final, 1);
	/* Pre-release: the mixed timeline really carries queued mixed
	 * periods ahead of the DMA position. */
	for (period = 0U; period < AUDIO_NUM_PERIODS; period++) {
		if (snapshot_period_is_constant(mixed_pre, period,
				MIXED_PCM))
			mixed_seen = 1;
	}
	check(mixed_seen,
	      "rebuild: mixed timeline carried lease audio", "");
	/* One pass after the release: pass 5 has the DMA entering period 5;
	 * the rebuild window is everything strictly ahead of it
	 * (6, 7, 0, 1, ...). Queued periods must be rebuilt pump-only,
	 * while period 5, the period being entered, may still carry the
	 * lease's audio. Periods behind the DMA position keep their
	 * played-out content until refilled; they are not the queue. */
	check(snapshot_period_is_constant(mixed_pre, 7U, MIXED_PCM),
	      "rebuild: period 7 was queued mixed before the release", "");
	check(snapshot_period_is_constant(mixed_post, 7U, PUMP_PCM) &&
	      snapshot_period_is_constant(mixed_post, 0U, PUMP_PCM) &&
	      snapshot_period_is_constant(mixed_post, 1U, PUMP_PCM),
	      "rebuild: queued periods rebuilt pump-only", "");
	check(snapshot_period_is_constant(mixed_post, 5U, MIXED_PCM) ||
	      snapshot_period_is_constant(mixed_post, 5U, PUMP_PCM),
	      "rebuild: entered period is the only allowed residual", "");
	/* The timelines converge: the final mixed ring is byte-identical
	 * to the pump-only one (no lease audio anywhere at the end). */
	check(memcmp(mixed_final, pump_final, AUDIO_TX_BUFFER_SIZE) == 0,
	      "rebuild: final ring converges to pump-only", "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_FREE,
	      "rebuild: released slot is FREE", "");
	/* Release is retry-safe: stale and garbage generations on a free
	 * slot complete OK without touching anything. */
	check(audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		0U) == AUDIO_FABRIC_LEASE_OK &&
	      audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		0xfffffffeU) == AUDIO_FABRIC_LEASE_OK,
	      "rebuild: release retry-safe on a free slot", "");
}

/* ---- G. warm reset ---- */

static void scenario_warm_reset(void)
{
	struct audio_fabric_ring_grant grant;
	struct audio_fabric_slot_state st;
	uint32_t i;
	int pcm_zeroed = 1;
	int control_zeroed = 1;

	fabric_reset_state();
	g_dma_count = 0;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "reset: acquire", "");
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation =
		grant.generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	fabric_pass();
	fabric_reset_state();
	for (i = 0U; i < RING_TEST_CAPACITY; i++) {
		if (g_ring_pcm_a[i] != 0U)
			pcm_zeroed = 0;
	}
	for (i = 0U; i < SDK_AUDIO_RING_CONTROL_SIZE; i++) {
		if (g_ring_control_a[i] != 0U)
			control_zeroed = 0;
	}
	check(pcm_zeroed && control_zeroed,
	      "reset: grant and control block zeroed", "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_FREE &&
	      st.heartbeat_ms == SDK_AUDIO_RING_HEARTBEAT_UNKNOWN,
	      "reset: slot FREE, heartbeat UNKNOWN",
	      fmt("state=%u age=%u", st.state, st.heartbeat_ms));
	/* Fresh generations always sit above pre-reset ones, so a stale
	 * pre-reset grant can never validate again. */
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK && grant.generation > 1U,
	      "reset: fresh generation above the pre-reset one",
	      fmt("gen=%u", grant.generation));
	(void)audio_fabric_ring_release(AUDIO_FABRIC_SLOT_MAILBOX,
		grant.generation);
}

/* ---- H. cache fidelity (recording mock) ---- */

static void scenario_cache_fidelity(void)
{
	struct audio_fabric_ring_grant grant;
	uintptr_t producer_adr = (uintptr_t)g_ring_control_a;
	uintptr_t firmware_adr = (uintptr_t)(g_ring_control_a +
		SDK_AUDIO_RING_CONTROL_LINE_SIZE);
	int producer_invalidated = 0;
	int firmware_flushed = 0;
	unsigned i;
	int kind;
	uintptr_t adr;
	unsigned long len;

	fabric_reset_state();
	g_dma_count = 0;
	check(acquire_lease(AUDIO_FABRIC_SLOT_MAILBOX, 128U, &grant) ==
	      AUDIO_FABRIC_LEASE_OK, "cache: acquire", "");
	g_producer[AUDIO_FABRIC_SLOT_MAILBOX].generation =
		grant.generation;
	producer_fill(AUDIO_FABRIC_SLOT_MAILBOX, LEASE_PCM);
	xil_cache_mock_reset();
	xil_cache_mock_set_enabled(1);
	fabric_pass();
	fabric_pass();
	fabric_pass();
	xil_cache_mock_set_enabled(0);
	for (i = 0U; xil_cache_mock_op(i, &kind, &adr, &len); i++) {
		if (kind == XIL_CACHE_MOP_INVALIDATE &&
		    adr >= producer_adr &&
		    adr < producer_adr +
			    SDK_AUDIO_RING_CONTROL_LINE_SIZE)
			producer_invalidated = 1;
		if (kind == XIL_CACHE_MOP_FLUSH &&
		    adr >= firmware_adr &&
		    adr < firmware_adr +
			    SDK_AUDIO_RING_CONTROL_LINE_SIZE)
			firmware_flushed = 1;
	}
	check(producer_invalidated,
	      "cache: producer-line read invalidates the line", "");
	check(firmware_flushed,
	      "cache: credit publication flushes the fw line", "");
}

int main(void)
{
	scenario_admission();
	scenario_rate_admission();
	scenario_lifecycle();
	scenario_rate_conversion();
	scenario_malformed();
	scenario_grace();
	scenario_cursor_fault();
	scenario_heartbeat();
	scenario_dead_line();
	scenario_rebuild();
	scenario_warm_reset();
	scenario_cache_fidelity();
	if (failures != 0)
		printf("audio_fabric_ring_test: %d FAILED\n", failures);
	else
		printf("audio_fabric_ring_test: all scenarios passed\n");
	return failures != 0;
}
