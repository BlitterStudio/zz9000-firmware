/*
 * Host tests for the audio fabric compositor's mailbox lease plane
 * (plan U3).
 *
 * Companion to audio_fabric_test.c (compositor/parity plane); the two
 * binaries share their harness in fabric_test_common.{h,c}. The
 * scenarios here drive the SDK-facing lease API -- BEGIN / SUBMIT /
 * RELEASE and the per-slot state read -- against the real compositor
 * core, with the card-side rings redirected through the host-test
 * seam.
 *
 * Coverage:
 *   8. lease lifecycle: BEGIN -> LEASED -> first SUBMIT -> ACTIVE ->
 *      audible period -> RELEASE -> FREE with last-release silence,
 *   9. release zero-contribution: a mixed lease's ring contribution
 *      disappears after release, returning bit-exact to pump-only,
 *   10. ghost bound (R5): <= 1 residual period after a mid-stream
 *      release, and a fresh lease never reads retired ring bytes,
 *   11. stale writes: released/garbage handles rejected with no ring
 *      or cursor effect; generations advance per lease (R5),
 *   12. warm reset (R7): slots FREE, rings zeroed, TX silenced,
 *      pre-reset handles stale, fresh generations above old ones,
 *      first post-reset period free of pre-reset samples,
 *   13. exhaustion/admission: BUSY on a leased slot, BAD_SLOT on the
 *      pump/firmware-reserved/out-of-range slots, legacy rejection,
 *      partial accept and zero-accept backpressure,
 *   14. state read vs driven state: cursors, per-slot underruns,
 *      16.16 peak with the scene-meter hold-reset convention and
 *      at-rail clip-region counting,
 *   15. coexistence (R9/AE1 host side): pump slot and lease slot both
 *      active -> every committed period is the saturating arithmetic
 *      sum, the lease side at its applied gain,
 *   16. ceiling-bounded lease gain (R11): an over-ceiling gain
 *      request is bounded and REPORTED (applied-vs-requested, the I3
 *      trim-bound pattern), the mix runs at the applied gain, an
 *      under-bound request passes through untouched,
 *   17. U5 hardening: lease backpressure and straddling ring-end
 *      copies, warm reset at IDLE, begin-BUSY on an attached slot,
 *      media resume without a shared-frontier rewind,
 *   18. cache fidelity: every lease source-ring read is preceded by
 *      the exact reader-side invalidate, and every committed TX period
 *      (real PCM or silence) is pushed with the exact flush -- asserted
 *      against the recording xil_cache mock of this binary.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fabric_test_common.h"
#include "xil_cache.h"

/* Client staging buffer: the mailbox handler resolves src_handle +
 * offset to an address and hands it to the fabric API exactly like
 * the stream feed path; the tests drive the resolved address. */
static uint8_t g_lease_stage[AUDIO_FABRIC_LEASE_RING_BYTES];

static void stage_fill_constant(uint8_t *buf, uint32_t bytes, int16_t value)
{
	uint32_t i;

	for (i = 0U; i + 1U < bytes; i += 2U) {
		buf[i] = (uint8_t)(uint16_t)value;
		buf[i + 1U] = (uint8_t)((uint16_t)value >> 8);
	}
}

/* 8 samples at +FULL_SCALE then 8 at 1000: 120 at-rail regions per
 * 3840-byte period, and the period boundary always lands mid-group
 * so regions never merge across periods. */
static void stage_fill_clip_pattern(uint8_t *buf, uint32_t bytes)
{
	uint32_t i;

	for (i = 0U; i + 1U < bytes; i += 2U) {
		int16_t v = ((i / 2U) & 8U) ? 1000 : 32767;

		buf[i] = (uint8_t)(uint16_t)v;
		buf[i + 1U] = (uint8_t)((uint16_t)v >> 8);
	}
}

static struct audio_fabric_slot_state lease_state(uint32_t slot)
{
	struct audio_fabric_slot_state st;

	(void)audio_fabric_slot_state(
		slot, SDK_AUDIO_METER_IDENTITY_SDK_STREAM, 0, &st);
	return st;
}

static void lease_feed(uint32_t handle, uint32_t bytes,
	uint32_t expect_consumed, const char *name)
{
	uint32_t consumed = 0xdeadbeefU;
	int rc = audio_fabric_lease_submit(
		handle, g_lease_stage, bytes, &consumed);

	check(rc == AUDIO_FABRIC_LEASE_OK && consumed == expect_consumed,
	      name, fmt("rc=%d consumed=%u want=%u", rc, consumed,
	                expect_consumed));
}

static int period_is_constant(const uint8_t *ring, uint32_t index,
	int16_t value)
{
	const int16_t *p = (const int16_t *)(ring + index * TICK_BYTES);
	uint32_t i;

	for (i = 0U; i < TICK_BYTES / 2U; i++) {
		if (p[i] != value)
			return 0;
	}
	return 1;
}

/* ---- U3 scenario 8: full lease lifecycle, one slot ---- */

static void scenario_lease_lifecycle(void)
{
	struct audio_fabric_slot_state st;
	uint32_t lease = 0U;
	uint32_t i;
	int audible = 0;

	fabric_reset_state();
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK &&
	      lease != 0U &&
	      AUDIO_FABRIC_LEASE_HANDLE_SLOT(lease) ==
	      AUDIO_FABRIC_SLOT_MAILBOX,
	      "lease: begin grants a slot-1 handle",
	      fmt("handle=%08x", lease));
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_LEASED &&
	      st.identity == SDK_AUDIO_METER_IDENTITY_MEDIA &&
	      st.lease == lease &&
	      st.generation == AUDIO_FABRIC_LEASE_HANDLE_EPOCH(lease) &&
	      st.written_bytes == 0U && st.consumed_bytes == 0U &&
	      st.underruns == 0U,
	      "lease: LEASED snapshot before first audio",
	      fmt("state=%u gen=%u w=%llu", st.state, st.generation,
	          (unsigned long long)st.written_bytes));

	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 15000);
	lease_feed(lease, TICK_BYTES, TICK_BYTES,
	           "lease: first submit fully accepted");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE,
	      "lease: ACTIVE after first accepted bytes",
	      fmt("state=%u", st.state));
	check(lease_state(AUDIO_FABRIC_SLOT_PUMP).state ==
	      AUDIO_FABRIC_SLOT_STATE_FREE,
	      "lease: pump slot stays FREE (never leaseable)", "");

	dma_tick(1);
	audio_fabric_isr();
	lease_feed(lease, 3U * TICK_BYTES, 3U * TICK_BYTES,
	           "lease: steady-state submit accepted");
	dma_tick(1);
	audio_fabric_isr();
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		if (period_is_constant(g_fabric_tx, i, 15000))
			audible = 1;
	}
	check(audible, "lease: committed period carries lease PCM",
	      "no period equals the submitted constant");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.written_bytes == 4ULL * TICK_BYTES &&
	      st.consumed_bytes > 0U,
	      "lease: published/consumed cursors move",
	      fmt("w=%llu r=%llu",
	          (unsigned long long)st.written_bytes,
	          (unsigned long long)st.consumed_bytes));

	check(audio_fabric_lease_release(lease) == AUDIO_FABRIC_LEASE_OK,
	      "lease: release completes", "");
	/* Idempotent for the immediately-previous lease (SDK contract). */
	check(audio_fabric_lease_release(lease) == AUDIO_FABRIC_LEASE_OK,
	      "lease: release idempotent", "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_FREE &&
	      st.lease == 0xffffffffU &&
	      st.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
	      "lease: FREE after release",
	      fmt("state=%u lease=%08x", st.state, st.lease));
	{
		static const uint8_t zero[AUDIO_TX_BUFFER_SIZE];

		check(memcmp(g_fabric_tx, zero, sizeof(zero)) == 0,
		      "lease: last release silenced the ring (KTD8)", "");
	}
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "lease: idle after last release", "");
}

/* ---- U3 scenario 9: release zero-contribution in a live mix ---- */

static void scenario_lease_zero_contribution(void)
{
	/* Per-tick pump-solo snapshots: the frontier cadence is identical
	 * across the two runs (the pump never runs dry), so ring position
	 * p is filled at the same ISR index in both and the snapshots are
	 * directly comparable. */
	static uint8_t solo_snaps[20][AUDIO_TX_BUFFER_SIZE];
	static uint8_t during[AUDIO_TX_BUFFER_SIZE];
	uint32_t lease = 0U;
	int tick;

	/* Pump-solo reference on the never-dry script. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_dma_count = 0;
	fabric_pump_start();
	model_publish(&g_model_b, 6U * TICK_BYTES);
	for (tick = 0; tick < 20; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		model_publish(&g_model_b, TICK_BYTES);
		memcpy(solo_snaps[tick], g_fabric_tx, AUDIO_TX_BUFFER_SIZE);
	}

	/* Same script with a mixed lease, released at tick 6: with 14
	 * post-release ISRs every ring position's last fill is
	 * post-release, so the end state must return to the pump-only
	 * ring exactly. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_dma_count = 0;
	fabric_pump_start();
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "zero-contribution: begin", "");
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 4000);
	model_publish(&g_model_b, 6U * TICK_BYTES);
	for (tick = 0; tick < 20; tick++) {
		if (tick < 6)
			lease_feed(lease, TICK_BYTES, TICK_BYTES,
			           "zero-contribution: lease fed");
		if (tick == 5)
			memcpy(during, g_fabric_tx, sizeof(during));
		if (tick == 6)
			check(audio_fabric_lease_release(lease) ==
			      AUDIO_FABRIC_LEASE_OK,
			      "zero-contribution: release mid-mix", "");
		dma_tick(1);
		audio_fabric_isr();
		model_publish(&g_model_b, TICK_BYTES);
	}
	check(memcmp(during, solo_snaps[5], AUDIO_TX_BUFFER_SIZE) != 0,
	      "zero-contribution: lease audible in the mix",
	      "mixed ring equals the pump-only reference");
	check(memcmp(g_fabric_tx, solo_snaps[19],
	             AUDIO_TX_BUFFER_SIZE) == 0,
	      "zero-contribution: ring returns to pump-only after release",
	      "residual lease audio in the ring");
	check(lease_state(AUDIO_FABRIC_SLOT_MAILBOX).state ==
	      AUDIO_FABRIC_SLOT_STATE_FREE,
	      "zero-contribution: slot FREE", "");
}

/* ---- U3 scenario 10: ghost bound (R5) ---- */

static void scenario_lease_ghost_bound(void)
{
	uint32_t lease = 0U;
	int tick;

	fabric_reset_state();
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK, "ghost: begin", "");
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), -20000);
	lease_feed(lease, 4U * TICK_BYTES, 4U * TICK_BYTES, "ghost: fed");
	dma_tick(1);
	audio_fabric_isr();
	dma_tick(1);
	audio_fabric_isr();
	check(count_nonzero_periods(g_fabric_tx) > 0U,
	      "ghost: lease audible before release", "");
	/* Mid-stream release with un-consumed PCM still in the source
	 * ring: after release, at most ONE period of retired audio may
	 * differ from a silence reference; the last-producer silence
	 * policy holds it to zero. */
	check(audio_fabric_lease_release(lease) == AUDIO_FABRIC_LEASE_OK,
	      "ghost: release mid-stream", "");
	for (tick = 0; tick < 4; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		check(count_nonzero_periods(g_fabric_tx) <= 1U,
		      "ghost: at most one residual period after release",
		      fmt("tick %d: %u nonzero periods", tick,
		          count_nonzero_periods(g_fabric_tx)));
	}
	/* A fresh lease on the same slot must not read the retired
	 * lease's residual ring bytes (BEGIN re-zeroes the ring). */
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK, "ghost: re-begin", "");
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 7777);
	lease_feed(lease, TICK_BYTES, TICK_BYTES, "ghost: re-fed");
	dma_tick(1);
	audio_fabric_isr();
	dma_tick(1);
	audio_fabric_isr();
	{
		uint32_t i;
		int clean = 1;

		for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
			if (!period_is_constant(g_fabric_tx, i, 0) &&
			    !period_is_constant(g_fabric_tx, i, 7777))
				clean = 0;
		}
		check(clean,
		      "ghost: fresh lease reads only fresh samples",
		      "period holds neither silence nor the new constant");
	}
}

/* ---- U3 scenario 11: stale writes rejected, no ring effect ---- */

static void scenario_lease_stale_write(void)
{
	struct audio_fabric_slot_state st;
	uint64_t hash_before;
	uint32_t lease = 0U;
	uint32_t lease2 = 0U;
	uint32_t consumed = 0xdeadbeefU;

	fabric_reset_state();
	g_dma_count = 0;
	(void)audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL);
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 11111);
	lease_feed(lease, 2U * TICK_BYTES, 2U * TICK_BYTES,
	           "stale: lease fed");
	hash_before = ring_hash(g_lease_ring_a);
	check(audio_fabric_lease_release(lease) == AUDIO_FABRIC_LEASE_OK,
	      "stale: release", "");

	consumed = 0xdeadbeefU;
	check(audio_fabric_lease_submit(
		      lease, g_lease_stage, TICK_BYTES,
		      &consumed) == AUDIO_FABRIC_LEASE_EHANDLE &&
	      consumed == 0U,
	      "stale: submit after release rejected",
	      fmt("consumed=%u", consumed));
	check(audio_fabric_lease_submit(
		      0xffffffffU, g_lease_stage, TICK_BYTES,
		      &consumed) == AUDIO_FABRIC_LEASE_EHANDLE,
	      "stale: invalid-handle constant rejected", "");
	check(ring_hash(g_lease_ring_a) == hash_before,
	      "stale: rejected submit left the source ring untouched", "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_FREE &&
	      st.written_bytes == 0U,
	      "stale: cursors were zeroed at release",
	      fmt("w=%llu", (unsigned long long)st.written_bytes));

	/* A stale handle must not target the NEXT lease on the slot. */
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease2, NULL) == AUDIO_FABRIC_LEASE_OK &&
	      AUDIO_FABRIC_LEASE_HANDLE_EPOCH(lease2) >
	      AUDIO_FABRIC_LEASE_HANDLE_EPOCH(lease),
	      "stale: generations advance per lease", "");
	check(audio_fabric_lease_submit(
		      lease, g_lease_stage, TICK_BYTES,
		      &consumed) == AUDIO_FABRIC_LEASE_EHANDLE,
	      "stale: previous-generation handle rejected on live slot",
	      "");
	check(audio_fabric_lease_release(lease) ==
	      AUDIO_FABRIC_LEASE_EHANDLE,
	      "stale: previous-generation release rejected", "");
	(void)audio_fabric_lease_release(lease2);
}

/* ---- U3 scenario 12: warm reset (R7) ---- */

static void scenario_lease_warm_reset(void)
{
	struct audio_fabric_slot_state st;
	uint32_t lease = 0U;
	uint32_t fresh = 0U;
	uint32_t old_generation;
	uint32_t probe = 0U;
	uint32_t i;
	int tick;

	fabric_reset_state();
	g_dma_count = 0;
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	fabric_pump_start();
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK, "reset: begin", "");
	old_generation = AUDIO_FABRIC_LEASE_HANDLE_EPOCH(lease);
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 15000);
	lease_feed(lease, 3U * TICK_BYTES, 3U * TICK_BYTES, "reset: fed");
	model_publish(&g_model_b, 6U * TICK_BYTES);
	dma_tick(1);
	audio_fabric_isr();
	dma_tick(1);
	audio_fabric_isr();
	check(count_nonzero_periods(g_fabric_tx) > 0U,
	      "reset: mix audible pre-reset", "");

	/* The sdk_mailbox_init seam (called BEFORE the mailbox generation
	 * advances there): fail-closed teardown. */
	audio_fabric_reset();
	check(lease_state(AUDIO_FABRIC_SLOT_PUMP).state ==
	      AUDIO_FABRIC_SLOT_STATE_FREE &&
	      lease_state(AUDIO_FABRIC_SLOT_MAILBOX).state ==
	      AUDIO_FABRIC_SLOT_STATE_FREE &&
	      lease_state(AUDIO_FABRIC_SLOT_RESERVED).state ==
	      AUDIO_FABRIC_SLOT_STATE_FREE,
	      "reset: every slot FREE", "");
	{
		static const uint8_t zero[AUDIO_TX_BUFFER_SIZE];
		static const uint8_t zero_ring[AUDIO_FABRIC_LEASE_RING_BYTES];

		check(memcmp(g_fabric_tx, zero, sizeof(zero)) == 0,
		      "reset: owned TX ring silenced", "");
		check(memcmp(g_lease_ring_a, zero_ring,
		             sizeof(zero_ring)) == 0,
		      "reset: card-side lease ring zeroed", "");
	}
	check(audio_fabric_lease_submit(
		      lease, g_lease_stage, TICK_BYTES,
		      &probe) == AUDIO_FABRIC_LEASE_EHANDLE,
	      "reset: pre-reset handle stale", "");
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&fresh, NULL) == AUDIO_FABRIC_LEASE_OK &&
	      AUDIO_FABRIC_LEASE_HANDLE_EPOCH(fresh) > old_generation,
	      "reset: fresh lease generation above pre-reset ones",
	      fmt("fresh=%u old=%u",
	          AUDIO_FABRIC_LEASE_HANDLE_EPOCH(fresh), old_generation));
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), -22222);
	lease_feed(fresh, 3U * TICK_BYTES, 3U * TICK_BYTES,
	           "reset: fresh lease fed");
	for (tick = 0; tick < 3; tick++) {
		dma_tick(1);
		audio_fabric_isr();
	}
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		if (!period_is_constant(g_fabric_tx, i, 0) &&
		    !period_is_constant(g_fabric_tx, i, -22222)) {
			check(0, "reset: post-reset periods free of "
			      "pre-reset samples",
			      fmt("period %u holds neither silence nor "
			          "the fresh constant", i));
			return;
		}
	}
	check(1, "reset: post-reset periods free of pre-reset samples",
	      "");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_ACTIVE &&
	      st.lease == fresh,
	      "reset: fresh lease ACTIVE", "");
	(void)audio_fabric_lease_release(fresh);
}

/* ---- U3 scenario 13: exhaustion, admission policy, backpressure ---- */

static void scenario_lease_exhaustion(void)
{
	uint32_t lease = 0U;
	uint32_t again = 0U;
	uint32_t consumed = 0U;

	fabric_reset_state();
	g_dma_count = 0;
	g_legacy_active = 1;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&again, NULL) == AUDIO_FABRIC_LEASE_ELEGACY,
	      "exhaustion: legacy-exclusive output rejected", "");
	g_legacy_active = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "exhaustion: first begin", "");
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&again, NULL) == AUDIO_FABRIC_LEASE_EBUSY,
	      "exhaustion: leased slot completes BUSY", "");
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_RESERVED,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&again, NULL) == AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "exhaustion: firmware-reserved slot rejected", "");
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_PUMP,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&again, NULL) == AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "exhaustion: pump slot rejected", "");
	check(audio_fabric_lease_begin(5U,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&again, NULL) == AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "exhaustion: out-of-range slot rejected", "");

	/* Partial accept: the ring bounds the lease. */
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 3333);
	lease_feed(lease, AUDIO_FABRIC_LEASE_RING_BYTES - 2U * TICK_BYTES,
	           AUDIO_FABRIC_LEASE_RING_BYTES - 2U * TICK_BYTES,
	           "exhaustion: ring-fill submit accepted");
	lease_feed(lease, 2U * TICK_BYTES, 2U * TICK_BYTES,
	           "exhaustion: remaining space accepted");
	consumed = 0xdeadbeefU;
	check(audio_fabric_lease_submit(
		      lease, g_lease_stage, TICK_BYTES,
		      &consumed) == AUDIO_FABRIC_LEASE_OK && consumed == 0U,
	      "exhaustion: full ring accepts zero (mailbox BUSY)",
	      fmt("consumed=%u", consumed));
	dma_tick(1);
	audio_fabric_isr();
	lease_feed(lease, TICK_BYTES, TICK_BYTES,
	           "exhaustion: consumption reopens space");

	check(audio_fabric_lease_release(lease) == AUDIO_FABRIC_LEASE_OK,
	      "exhaustion: release", "");
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&again, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "exhaustion: begin succeeds after release", "");
	(void)audio_fabric_lease_release(again);
}

/* ---- U3 scenario 14: state read vs driven state ---- */

static void scenario_lease_state_read(void)
{
	struct audio_fabric_slot_state st;
	uint32_t lease = 0U;
	uint32_t consumed;
	uint32_t underruns_fed;
	int tick;

	fabric_reset_state();
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK, "state: begin", "");

	/* Peak: 20000 amplitude reads back as 16.16 (20000 << 1). */
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 20000);
	lease_feed(lease, 2U * TICK_BYTES, 2U * TICK_BYTES, "state: fed");
	dma_tick(1);
	audio_fabric_isr();
	lease_feed(lease, 2U * TICK_BYTES, 2U * TICK_BYTES, "state: fed");
	dma_tick(1);
	audio_fabric_isr();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.peak == 40000U && st.clips == 0U,
	      "state: peak in 16.16, no clips",
	      fmt("peak=%u clips=%u", st.peak, st.clips));
	underruns_fed = st.underruns;
	check(underruns_fed >= 1U, "state: start-rebase underrun counted",
	      fmt("u=%u", underruns_fed));

	/* Starving an ACTIVE lease moves only its underrun counter. */
	for (tick = 0; tick < 3; tick++) {
		dma_tick(1);
		audio_fabric_isr();
	}
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.underruns > underruns_fed,
	      "state: starvation grows underrun_count",
	      fmt("fed=%u starved=%u", underruns_fed, st.underruns));

	/* Clip regions: 120 at-rail regions per clip-pattern period
	 * consumed (earlier non-rail periods contribute none). */
	{
		uint64_t consumed_pre = lease_state(
			AUDIO_FABRIC_SLOT_MAILBOX).consumed_bytes;

		stage_fill_clip_pattern(g_lease_stage,
		                        sizeof(g_lease_stage));
		lease_feed(lease, 2U * TICK_BYTES, 2U * TICK_BYTES,
		           "state: clip pattern fed");
		dma_tick(1);
		audio_fabric_isr();
		dma_tick(1);
		audio_fabric_isr();
		st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
		consumed = (uint32_t)(st.consumed_bytes - consumed_pre);
		check(consumed >= TICK_BYTES &&
		      st.clips == 120U * (consumed / TICK_BYTES),
		      "state: clip counts at-rail regions",
		      fmt("clip-consumed=%u clips=%u", consumed,
		          st.clips));
	}
	check(st.peak == 65534U, "state: rail sample updates peak",
	      fmt("peak=%u", st.peak));

	/* Drain, then hold-reset: the next source read opens a fresh
	 * peak window (the scene-meter read-and-clear convention). */
	for (tick = 0; tick < 24; tick++) {
		st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
		if (st.consumed_bytes == st.written_bytes)
			break;
		dma_tick(1);
		audio_fabric_isr();
	}
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.consumed_bytes == st.written_bytes,
	      "state: drained before hold-reset", "");
	check(audio_fabric_slot_state(AUDIO_FABRIC_SLOT_MAILBOX,
		      SDK_AUDIO_METER_IDENTITY_SDK_STREAM, 1, &st) == 1,
	      "state: hold-reset read completes", "");
	check(st.peak == 65534U,
	      "state: hold-reset read peeks the old window", "");
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 10000);
	lease_feed(lease, TICK_BYTES, TICK_BYTES, "state: soft signal fed");
	dma_tick(1);
	audio_fabric_isr();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.peak == 20000U,
	      "state: fresh peak window after hold-reset",
	      fmt("peak=%u", st.peak));
	/* Without the flag the read only peeks. */
	lease_feed(lease, TICK_BYTES, TICK_BYTES, "state: softer signal");
	dma_tick(1);
	audio_fabric_isr();
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.peak == 20000U,
	      "state: plain read holds the window",
	      fmt("peak=%u", st.peak));

	/* Reserved slot and the pump slot's bind state. */
	st = lease_state(AUDIO_FABRIC_SLOT_RESERVED);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_FREE &&
	      st.lease == 0xffffffffU && st.peak == 0U && st.clips == 0U,
	      "state: reserved slot reports FREE with no lease", "");
	st = lease_state(AUDIO_FABRIC_SLOT_PUMP);
	check(st.state == AUDIO_FABRIC_SLOT_STATE_FREE &&
	      st.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
	      "state: pump slot FREE while unbound", "");
	(void)audio_fabric_lease_release(lease);
}

/* ---- U5: lease submission backpressure and cursor accounting ---- */

static void scenario_lease_backpressure(void)
{
	struct audio_fabric_slot_state st;
	uint32_t lease = 0U;
	int tick;

	fabric_reset_state();
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "backpressure: begin", "");
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 2500);
	/* Fill the ring to one period short of capacity. */
	lease_feed(lease, AUDIO_FABRIC_LEASE_RING_BYTES - TICK_BYTES,
	           AUDIO_FABRIC_LEASE_RING_BYTES - TICK_BYTES,
	           "backpressure: near-full feed");
	/* Only one period fits: partial accept at the tail. */
	lease_feed(lease, 2U * TICK_BYTES, TICK_BYTES,
	           "backpressure: partial accept");
	/* Full ring: zero accept (mailbox BUSY). */
	lease_feed(lease, TICK_BYTES, 0U, "backpressure: full ring busy");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.written_bytes ==
	      (uint64_t)AUDIO_FABRIC_LEASE_RING_BYTES,
	      "backpressure: published cursor counts the partial",
	      fmt("w=%llu", (unsigned long long)st.written_bytes));
	/* The compositor consumes periods; space reopens. */
	for (tick = 0; tick < 2; tick++) {
		dma_tick(1);
		audio_fabric_isr();
	}
	lease_feed(lease, TICK_BYTES, TICK_BYTES,
	           "backpressure: resubmit fully accepted");
	st = lease_state(AUDIO_FABRIC_SLOT_MAILBOX);
	check(st.written_bytes ==
	      (uint64_t)AUDIO_FABRIC_LEASE_RING_BYTES + TICK_BYTES &&
	      st.consumed_bytes > 0U,
	      "backpressure: cursors after consumption",
	      fmt("w=%llu r=%llu",
	          (unsigned long long)st.written_bytes,
	          (unsigned long long)st.consumed_bytes));
	(void)audio_fabric_lease_release(lease);
}

/* A straddling submit must split its copy across the ring end
 * byte-for-byte; a position-dependent pattern makes any mis-copy
 * visible. */
static void scenario_lease_wrap_copy(void)
{
	uint32_t lease = 0U;
	uint32_t tail = TICK_BYTES / 2U;
	uint32_t i;
	int ok;

	fabric_reset_state();
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK, "wrap: begin", "");
	for (i = 0U; i < sizeof(g_lease_stage); i++)
		g_lease_stage[i] = (uint8_t)(i & 0xffU);
	lease_feed(lease, AUDIO_FABRIC_LEASE_RING_BYTES - TICK_BYTES,
	           AUDIO_FABRIC_LEASE_RING_BYTES - TICK_BYTES, "wrap: near-end feed");
	/* The compositor consumes five periods, reopening ring space while
	 * the write cursor stays near the end. */
	dma_tick(1);
	audio_fabric_isr();
	lease_feed(lease, tail, tail,
	           "wrap: cursor half a period from the end");
	lease_feed(lease, TICK_BYTES, TICK_BYTES, "wrap: straddling submit");
	ok = 1;
	for (i = 0U; i < tail; i++) {
		if (g_lease_ring_a[AUDIO_FABRIC_LEASE_RING_BYTES - tail + i] !=
		    g_lease_stage[i] ||
		    g_lease_ring_a[i] != g_lease_stage[tail + i]) {
			ok = 0;
			break;
		}
	}
	check(ok, "wrap: straddling submit split across the ring end",
	      fmt("first bad byte %u", i));
	(void)audio_fabric_lease_release(lease);
}

/* A warm reset with nothing attached must not touch the ring and must
 * keep generations moving. */
static void scenario_reset_idle(void)
{
	uint32_t lease = 0U;
	uint32_t fresh = 0U;
	uint32_t old_generation;
	uint32_t wipes_before;

	fabric_reset_state();
	g_dma_count = 0;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK, "reset-idle: begin", "");
	old_generation = AUDIO_FABRIC_LEASE_HANDLE_EPOCH(lease);
	check(audio_fabric_lease_release(lease) == AUDIO_FABRIC_LEASE_OK,
	      "reset-idle: release", "");
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "reset-idle: idle before the warm reset", "");
	wipes_before = g_silence_calls;
	audio_fabric_reset();
	check(g_silence_calls == wipes_before,
	      "reset-idle: no ring wipe when not ACTIVE",
	      fmt("wipes=%u", g_silence_calls));
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "reset-idle: still idle after reset", "");
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&fresh, NULL) == AUDIO_FABRIC_LEASE_OK &&
	      AUDIO_FABRIC_LEASE_HANDLE_EPOCH(fresh) > old_generation,
	      "reset-idle: fresh lease generation above the previous",
	      fmt("fresh=%u old=%u",
	          AUDIO_FABRIC_LEASE_HANDLE_EPOCH(fresh), old_generation));
	(void)audio_fabric_lease_release(fresh);
}

/* A non-lease producer attached to a slot blocks the lease plane on it. */
static void scenario_lease_begin_busy(void)
{
	uint32_t lease = 0U;

	fabric_reset_state();
	g_dma_count = 0;
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	check(audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c) == 1,
	      "begin-busy: producer attach", "");
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_EBUSY,
	      "begin-busy: lease begin BUSY on an attached slot", "");
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_MAILBOX);
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK && lease != 0U,
	      "begin-busy: begin succeeds after detach", "");
	(void)audio_fabric_lease_release(lease);
}

/* Media resume while a lease is live must not rewind the shared fill
 * frontier (the guarded audio_playback_start): the already-committed
 * periods of the mix stand, and the lease cursor advances by exactly
 * one period on the resume tick. */
static void scenario_resume_guard(void)
{
	static uint8_t pre_resume[AUDIO_TX_BUFFER_SIZE];
	uint32_t lease = 0U;
	uint64_t consumed_before, consumed_after;
	uint32_t diff_periods = 0U;
	uint32_t lease_periods = 0U;
	uint32_t i;
	int tick;

	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_dma_count = 0;
	fabric_pump_start();
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "resume-guard: begin", "");
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 4000);
	model_publish(&g_model_b, 6U * TICK_BYTES);

	/* Steady mixed state: the frontier sits six periods ahead. */
	for (tick = 0; tick < 8; tick++) {
		lease_feed(lease, TICK_BYTES, TICK_BYTES,
		           "resume-guard: lease fed");
		dma_tick(1);
		audio_fabric_isr();
		model_publish(&g_model_b, TICK_BYTES);
	}

	/* Media pause: freeze the pump and rewind its staging to
	 * retirement (the session's pause semantics), then wipe. */
	g_model_b.staged = g_model_b.retired_total;
	audio_fabric_producer_freeze(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_ring_silence(AUDIO_FABRIC_SLOT_PUMP);

	/* Lease-only playback while the pump is paused: each tick
	 * commits exactly one frontier period of lease PCM. */
	for (tick = 0; tick < 4; tick++) {
		lease_feed(lease, TICK_BYTES, TICK_BYTES,
		           "resume-guard: lease fed during pause");
		dma_tick(1);
		audio_fabric_isr();
	}
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		if (period_is_constant(g_fabric_tx, i, 4000))
			lease_periods++;
	}
	check(lease_periods == 4U,
	      "resume-guard: pause left four lease-only periods",
	      fmt("periods=%u", lease_periods));
	memcpy(pre_resume, g_fabric_tx, sizeof(pre_resume));
	consumed_before =
		lease_state(AUDIO_FABRIC_SLOT_MAILBOX).consumed_bytes;

	/* Media resume: the guarded audio_playback_start sequencing. The
	 * lease keeps being fed as in steady state, with enough runway for
	 * an (incorrect) burst re-fill to show up in the cursor delta. */
	fabric_pump_resume();
	lease_feed(lease, 5U * TICK_BYTES, 5U * TICK_BYTES,
	           "resume-guard: lease fed for resume");
	dma_tick(1);
	audio_fabric_isr();
	model_publish(&g_model_b, TICK_BYTES);
	consumed_after =
		lease_state(AUDIO_FABRIC_SLOT_MAILBOX).consumed_bytes;

	/* Only the frontier period advanced naturally: exactly one ring
	 * period differs from pre-resume. An unguarded re-arm would
	 * burst-refill ~five periods and jump the lease cursor with them. */
	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		if (memcmp(pre_resume + i * TICK_BYTES,
		             g_fabric_tx + i * TICK_BYTES,
		             TICK_BYTES) != 0)
			diff_periods++;
	}
	check(diff_periods == 1U,
	      "resume-guard: committed periods not overwritten",
	      fmt("diff periods=%u", diff_periods));
	check(consumed_after - consumed_before == TICK_BYTES,
	      "resume-guard: lease cursor advanced one period only",
	      fmt("delta=%llu",
	          (unsigned long long)(consumed_after - consumed_before)));
	(void)audio_fabric_lease_release(lease);
}

/* ---- U4 scenario 15: steady-state two-source arithmetic sum ---- */

/* Pump slot (a bound stream producer) plus a real lease, both fed
 * known constants through their actual submission paths: every
 * committed steady-state period must be the saturating sum, with the
 * lease side scaled by its applied gain (U3 scenario 9 proved the
 * release dynamics; this pins the both-active arithmetic). */
static void coexistence_case(const char *label, int16_t pump,
	int16_t lease_pcm, uint32_t gain)
{
	int32_t lease_scaled = ((int32_t)lease_pcm * (int32_t)gain) >> 7;
	int32_t expected_raw = (int32_t)pump + lease_scaled;
	int16_t expected;
	const int16_t *period;
	uint32_t lease = 0U;
	uint32_t j;

	if (expected_raw > 32767)
		expected = 32767;
	else if (expected_raw < -32768)
		expected = -32768;
	else
		expected = (int16_t)expected_raw;

	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, pump);
	g_dma_count = 0;
	fabric_pump_start();
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, gain,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "coexistence: begin", label);
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), lease_pcm);
	lease_feed(lease, AUDIO_FABRIC_LEASE_RING_BYTES,
	           AUDIO_FABRIC_LEASE_RING_BYTES,
	           "coexistence: ring pre-filled");
	dma_tick(1);
	audio_fabric_isr();
	dma_tick(1);
	audio_fabric_isr();

	/* The frontier sits past period 1 after the start rebase; every
	 * period the burst fill committed carried both sources. */
	period = (const int16_t *)&g_fabric_tx[2 * TICK_BYTES];
	for (j = 0U; j < (TICK_BYTES / 2U); j++) {
		if (period[j] != expected) {
			check(0, "coexistence: saturating sum",
			      fmt("%s: word %u = %d, expected %d",
			          label, j, period[j], expected));
			(void)audio_fabric_lease_release(lease);
			return;
		}
	}
	check(1, "coexistence: saturating sum", label);
	check(lease_state(AUDIO_FABRIC_SLOT_MAILBOX).state ==
	      AUDIO_FABRIC_SLOT_STATE_ACTIVE,
	      "coexistence: lease ACTIVE mid-mix", label);
	check(lease_state(AUDIO_FABRIC_SLOT_PUMP).state ==
	      AUDIO_FABRIC_SLOT_STATE_ACTIVE,
	      "coexistence: pump ACTIVE mid-mix", label);
	(void)audio_fabric_lease_release(lease);
}

static void scenario_coexistence_sum(void)
{
	coexistence_case("unity-sum", 12000, 9000,
	                 AUDIO_FABRIC_GAIN_UNITY);      /* 21000 */
	coexistence_case("clip+", 20000, 20000,
	                 AUDIO_FABRIC_GAIN_UNITY);      /* 40000 -> 32767 */
	coexistence_case("attenuated", 12000, 9000, 64U); /* +4500 = 16500 */
}

/* ---- U4 scenario 16: ceiling-bounded lease gain (R11) ---- */

static void scenario_lease_gain_bound(void)
{
	struct audio_fabric_lease_grant grant;
	int16_t expected;
	const int16_t *period;
	uint32_t lease = 0U;
	uint32_t j;

	fabric_reset_state();
	g_dma_count = 0;

	/* Out-of-range requests are admission errors (the mailbox layer
	 * rejects them before the fabric; the fabric re-arms the same
	 * policy). */
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, 256U,
		&lease, NULL) == AUDIO_FABRIC_LEASE_EBAD_SLOT,
	      "gain: out-of-range gain rejected", "");

	/* A request beyond the ceiling-enforced composition is bounded,
	 * with the applied-vs-requested distinction REPORTED through the
	 * grant (the I3 trim-bound semantics: never a silent clamp). */
	g_lease_gain_bound = 100U;
	grant.gain = 0U;
	grant.bounded = 1U;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, 200U,
		&lease, &grant) == AUDIO_FABRIC_LEASE_OK &&
	      grant.gain == 100U && grant.bounded != 0U,
	      "gain: over-ceiling request bounded and reported",
	      fmt("applied=%u bounded=%u", (unsigned)grant.gain,
	          (unsigned)grant.bounded));

	/* The slot mixes at the APPLIED gain, not the request: pump
	 * 12000 plus 9000 scaled by 100/128 = 7031 -> 19031. */
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, 12000);
	fabric_pump_start();
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 9000);
	lease_feed(lease, AUDIO_FABRIC_LEASE_RING_BYTES,
	           AUDIO_FABRIC_LEASE_RING_BYTES, "gain: fed");
	dma_tick(1);
	audio_fabric_isr();
	dma_tick(1);
	audio_fabric_isr();
	expected = (int16_t)(12000 + ((9000 * 100) >> 7));
	period = (const int16_t *)&g_fabric_tx[2 * TICK_BYTES];
	for (j = 0U; j < (TICK_BYTES / 2U); j++) {
		if (period[j] != expected) {
			check(0, "gain: mix uses the applied gain",
			      fmt("word %u = %d, expected %d",
			          j, period[j], expected));
			(void)audio_fabric_lease_release(lease);
			return;
		}
	}
	check(1, "gain: mix uses the applied gain", "");
	(void)audio_fabric_lease_release(lease);

	/* A request under the bound passes through untouched. */
	g_lease_gain_bound = 100U;
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, 90U,
		&lease, &grant) == AUDIO_FABRIC_LEASE_OK &&
	      grant.gain == 90U && grant.bounded == 0U,
	      "gain: under-bound request applied unmodified",
	      fmt("applied=%u bounded=%u", (unsigned)grant.gain,
	          (unsigned)grant.bounded));
	(void)audio_fabric_lease_release(lease);
}

/* ---- U3 scenario 18: cache fidelity of the lease data path ---- */

/* This binary compiles mock/xil_cache.h in recording mode; every
 * invalidate/flush is logged with its exact range. The scenario pins
 * the two disciplines that keep a non-coherent client and the
 * formatter DMA honest:
 *   - the compositor's read of a lease source period is preceded by an
 *     invalidate of exactly that period's ring range, and
 *   - every committed TX period -- real PCM or silence -- is flushed at
 *     exactly its ring offset before the frontier advances over it.
 * Removing or mis-ranging any one of those calls breaks this test. */

#define CACHE_TEST_PERIODS 3U

static void cache_check_op(unsigned index, int kind, uintptr_t adr,
                           unsigned long len, const char *name)
{
	int got_kind;
	uintptr_t got_adr;
	unsigned long got_len;

	if (!xil_cache_mock_op(index, &got_kind, &got_adr, &got_len)) {
		check(0, name, fmt("op %u missing (count=%u)", index,
		                  g_xil_cache_mock.count));
		return;
	}
	check(got_kind == kind && got_adr == adr && got_len == len,
	      name, fmt("op %u: kind=%d want %d, adr=%llx want %llx, "
	                    "len=%lu want %lu",
	                    index, got_kind, kind,
	                    (unsigned long long)got_adr,
	                    (unsigned long long)adr, got_len, len));
}

static void scenario_cache_fidelity(void)
{
	uint32_t lease = 0U;
	uintptr_t ring = (uintptr_t)g_lease_ring_a;
	uintptr_t stage = (uintptr_t)g_lease_stage;
	uintptr_t tx = (uintptr_t)g_fabric_tx;

	fabric_reset_state();
	g_dma_count = 0;
	xil_cache_mock_reset();
	xil_cache_mock_set_enabled(1);

	/* Window 1: BEGIN zeroes the card-side ring and pushes it to
	 * DRAM so the compositor's reader-side invalidates survive. */
	check(audio_fabric_lease_begin(AUDIO_FABRIC_SLOT_MAILBOX,
		SDK_AUDIO_METER_IDENTITY_MEDIA, AUDIO_FABRIC_GAIN_UNITY,
		&lease, NULL) == AUDIO_FABRIC_LEASE_OK,
	      "cache: begin", "");
	cache_check_op(0, XIL_CACHE_MOP_FLUSH, ring,
		AUDIO_FABRIC_LEASE_RING_BYTES,
		"cache: BEGIN flushes the zeroed lease ring");

	/* Window 2: SUBMIT invalidates the client staging range before
	 * reading it and flushes exactly the copied ring chunk. */
	stage_fill_constant(g_lease_stage, sizeof(g_lease_stage), 5000);
	lease_feed(lease, CACHE_TEST_PERIODS * TICK_BYTES,
	           CACHE_TEST_PERIODS * TICK_BYTES, "cache: fed");
	cache_check_op(1, XIL_CACHE_MOP_INVALIDATE, stage,
		CACHE_TEST_PERIODS * TICK_BYTES,
		"cache: SUBMIT invalidates the staging read");
	cache_check_op(2, XIL_CACHE_MOP_FLUSH, ring,
		CACHE_TEST_PERIODS * TICK_BYTES,
		"cache: SUBMIT flushes the copied ring chunk");

	/* Window 3: one compositor tick. The first accepted bytes re-armed
	 * the frontier at 2*TICK; the fill loop then consumes exactly the
	 * three staged periods (one invalidate per source-ring read) and
	 * commits four TX periods -- the three real ones plus the first
	 * silence -- each flushed at its own offset. */
	xil_cache_mock_reset();
	xil_cache_mock_set_enabled(1);
	dma_tick(1);
	audio_fabric_isr();
	cache_check_op(0, XIL_CACHE_MOP_INVALIDATE, ring + 0U * TICK_BYTES,
		TICK_BYTES, "cache: source read 0 invalidated");
	cache_check_op(1, XIL_CACHE_MOP_FLUSH, tx + 2U * TICK_BYTES,
		TICK_BYTES, "cache: TX period 2 flushed");
	cache_check_op(2, XIL_CACHE_MOP_INVALIDATE, ring + 1U * TICK_BYTES,
		TICK_BYTES, "cache: source read 1 invalidated");
	cache_check_op(3, XIL_CACHE_MOP_FLUSH, tx + 3U * TICK_BYTES,
		TICK_BYTES, "cache: TX period 3 flushed");
	cache_check_op(4, XIL_CACHE_MOP_INVALIDATE, ring + 2U * TICK_BYTES,
		TICK_BYTES, "cache: source read 2 invalidated");
	cache_check_op(5, XIL_CACHE_MOP_FLUSH, tx + 4U * TICK_BYTES,
		TICK_BYTES, "cache: TX period 4 flushed");
	cache_check_op(6, XIL_CACHE_MOP_FLUSH, tx + 5U * TICK_BYTES,
		TICK_BYTES, "cache: silence period 5 flushed");
	check(g_xil_cache_mock.count == 7U && g_xil_cache_mock.overflow == 0U,
	      "cache: no unexpected cache ops in the tick",
	      fmt("count=%u overflow=%u", g_xil_cache_mock.count,
	          g_xil_cache_mock.overflow));

	xil_cache_mock_set_enabled(0);
	xil_cache_mock_reset();
	(void)audio_fabric_lease_release(lease);
}

int main(void)
{
	printf("audio_fabric_lease_test: lease plane scenarios\n");

	scenario_lease_lifecycle();
	scenario_lease_zero_contribution();
	scenario_lease_ghost_bound();
	scenario_lease_stale_write();
	scenario_lease_warm_reset();
	scenario_lease_exhaustion();
	scenario_lease_state_read();
	scenario_coexistence_sum();
	scenario_lease_gain_bound();
	scenario_lease_backpressure();
	scenario_lease_wrap_copy();
	scenario_reset_idle();
	scenario_lease_begin_busy();
	scenario_resume_guard();
	scenario_cache_fidelity();

	if (failures != 0) {
		printf("audio_fabric_lease_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_fabric_lease_test: all checks passed\n");
	return 0;
}
