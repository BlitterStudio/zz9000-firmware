/*
 * Host tests for the audio fabric compositor core (plan U2).
 *
 * The suite is built characterization-first: pump_golden.c is a verbatim
 * capture of the pre-fabric sdk_mailbox.c pump at 9b3654f, and the
 * single-producer scenarios below run that golden and the compositor
 * side by side, requiring bit-identical TX-ring bytes and identical
 * producer-side accounting (staging cursor, retirement totals, played
 * underrun callbacks, stream play-out tail) after every 20 ms tick.
 *
 * The mailbox lease plane (plan U3) is covered by the companion binary
 * audio_fabric_lease_test.c; both share their harness in
 * fabric_test_common.{h,c}.
 *
 * Coverage:
 *   1. single-producer parity across fill offsets, conversion rates,
 *      mono/byte-swapped sources, underrun rebases and drain tails,
 *      media pause/resume and stop (KTD6), including off-table decode
 *      rates that commit silent periods,
 *   2. two-source saturating int32 mix -> S16 (KTD4),
 *   3. three-way ownership gate, both directions, clean idle returns,
 *   4. ring silence only when the last producer releases (KTD8),
 *   5. per-slot underrun isolation (R8), plus the exclusion for
 *      faulted/done producers (U5),
 *   6. formatter ownership: no re-init across producer bind/unbind
 *      inside fabric-active; conditional re-init at fabric re-entry,
 *   7. dispatcher coverage: with the TX ring MMU write-protected, ring
 *      writes happen only inside compositor windows, plus a source
 *      contract scan proving no other firmware module writes the ring
 *      while the fabric is active (R1). The scan reads the firmware
 *      sources from --srcdir (the Makefile recipe passes it explicitly;
 *      standalone runs derive it from argv[0] and SKIP when the tree is
 *      absent),
 *   8. mid-loop snapshot failure vs top-of-ISR unbind (U5).
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "audio_fabric.h"
#include "fabric_test_common.h"
#include "memorymap.h"
#include "pump_golden.h"
#include "sdk_mailbox.h"

/* The model snapshot fills either source struct; the golden capture
 * and the compositor must agree on the layout forever. */
_Static_assert(sizeof(struct pump_golden_source) ==
               sizeof(struct audio_fabric_source),
               "golden/fabric source snapshot layout drift");


/* ---- golden-side wiring ---- */

static uint8_t g_golden_tx[AUDIO_TX_BUFFER_SIZE];

static uint32_t test_dma_count(void)
{
	return g_dma_count;
}

static struct pump_golden g_golden;
static struct src_model g_model;

static int golden_snapshot(struct pump_golden_source *source)
{
	return model_snapshot(&g_model, source);
}

static int golden_stage(uint32_t bytes)
{
	return model_stage(&g_model, bytes);
}

static void golden_retire(uint32_t bytes)
{
	model_retire(&g_model, bytes);
}

static void golden_underrun(void)
{
	model_underrun(&g_model);
}

static void golden_tail_real(void)
{
	model_tail_real(&g_model);
}

static void golden_tail_drained(void)
{
	model_tail_drained(&g_model);
}

static int golden_tail_pending(void)
{
	return g_model.tail_pending;
}

static const struct pump_golden_ops g_golden_ops = {
	.snapshot = golden_snapshot,
	.stage = golden_stage,
	.retire = golden_retire,
	.underrun = golden_underrun,
	.tail_real = golden_tail_real,
	.tail_drained = golden_tail_drained,
	.tail_pending = golden_tail_pending,
};

static void golden_reset(struct src_model *m)
{
	memset(&g_golden, 0, sizeof(g_golden));
	memset(g_golden_tx, 0xAA, sizeof(g_golden_tx));
	memset(m, 0, sizeof(*m));
	g_dma_count = 0;
	g_golden.tx = g_golden_tx;
	g_golden.dma_count = test_dma_count;
	g_golden.ops = &g_golden_ops;
}

/* Post-run invariants of the captured pump: these are not the parity

 * contract (the golden run above is), just sanity that the capture
 * actually behaves like a pump before its output is frozen in. */
static void characterization_invariants(void)
{
	int tick;

	golden_reset(&g_model);
	model_init(&g_model, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_dma_count = 0;
	pump_golden_start(&g_golden, PUMP_GOLDEN_SOURCE_STREAM, 1U);
	model_publish(&g_model, 4 * TICK_BYTES);
	dma_tick(1);
	pump_golden_isr(&g_golden);

	/* 48 kHz stereo passthrough: the first filled period carries the
	 * first staged source samples verbatim (little-endian), so slot
	 * int16 word j equals generated sample j. */
	check(g_golden.fill_offset >= TICK_BYTES,
	      "golden fills ahead after first tick",
	      fmt("fill_offset=%u", g_golden.fill_offset));
	{
		int j;
		int ok = 1;

		for (j = 0; j < 16; j++) {
			int16_t got;

			/* start() at dma=0 arms the frontier at one
			 * period; the first ISR's rebase moves it to
			 * two, so the first filled slot sits there. */
			memcpy(&got, &g_golden_tx[2 * TICK_BYTES + 2 * j],
			       sizeof(got));
			if (got != pcm_sample((uint64_t)j)) {
				ok = 0;
				break;
			}
		}
		check(ok, "golden passthrough bytes",
		      fmt("word %d mismatch", j));
	}

	/* Underrun: starve until the DMA passes the stalled frontier. */
	for (tick = 0; tick < 10; tick++) {
		dma_tick(1);
		pump_golden_isr(&g_golden);
	}
	check(g_model.underrun_calls > 0, "golden played-underrun counted",
	      fmt("calls=%u", g_model.underrun_calls));

	/* Drain tail: after done, a full ring of silent periods drops the
	 * play-out tail. */
	g_model.done = 1;
	for (tick = 0; tick < 8; tick++) {
		dma_tick(1);
		pump_golden_isr(&g_golden);
	}
	check(g_model.tail_pending == 0, "golden tail drained",
	      fmt("tail=%d silence_run=%u", g_model.tail_pending,
	          g_golden.silence_run));

	pump_golden_stop(&g_golden);
	{
		static const uint8_t zero[AUDIO_TX_BUFFER_SIZE];

		check(ring_hash(g_golden_tx) == ring_hash(zero),
		      "golden stop silences ring",
		      fmt("hash=%016llx",
		          (unsigned long long)ring_hash(g_golden_tx)));
	}
}


/* ---- scenario 1: single-producer parity against the golden ---- */

static void parity_compare(const char *label, int tick)
{
	char stage[64];

	snprintf(stage, sizeof(stage), "%s tick %d", label, tick);
	if (memcmp(g_golden_tx, g_fabric_tx, AUDIO_TX_BUFFER_SIZE) != 0) {
		uint32_t i;

		for (i = 0U; i < AUDIO_TX_BUFFER_SIZE; i++) {
			if (g_golden_tx[i] != g_fabric_tx[i])
				break;
		}
		check(0, "parity ring bytes",
		      fmt("%s: first diff at byte %u (golden %02x fabric %02x)",
		          stage, i, g_golden_tx[i], g_fabric_tx[i]));
		return;
	}
	check(g_model.staged == g_model_b.staged, "parity staged cursor",
	      fmt("%s: golden %llu fabric %llu", stage,
	          (unsigned long long)g_model.staged,
	          (unsigned long long)g_model_b.staged));
	check(g_model.retired_total == g_model_b.retired_total,
	      "parity retired total", stage);
	check(g_model.underrun_calls == g_model_b.underrun_calls,
	      "parity underrun callbacks", stage);
	check(g_model.tail_pending == g_model_b.tail_pending,
	      "parity play-out tail", stage);
	check((g_golden.session != 0U) == (audio_fabric_output_busy() != 0),
	      "parity bound state", stage);
}

static void publish_both(uint32_t bytes)
{
	model_publish(&g_model, bytes);
	model_publish(&g_model_b, bytes);
}

/* The characterization script, replayed against golden and compositor
 * in lockstep: fill offsets across the ring wrap, starvation rebase,
 * recovery, drain tail, media pause/resume, stop (and, for one
 * config, the mid-flight source kill = ISR unbind). */
static void parity_run(const char *label, uint32_t rate, uint32_t channels,
                       uint32_t format, int stream_mode, int kill_source)
{
	char name[64];
	int tick;

	snprintf(name, sizeof(name), "%s/%uHz/ch%u/fmt%u", label, rate,
	         channels, format);
	golden_reset(&g_model);
	model_init(&g_model, rate, channels, format);
	g_model.is_stream = stream_mode;
	fabric_reset_state();
	model_init(&g_model_b, rate, channels, format);
	g_model_b.is_stream = stream_mode;

	g_dma_count = 3 * TICK_BYTES;
	pump_golden_start(&g_golden,
	                  stream_mode ? PUMP_GOLDEN_SOURCE_STREAM
	                              : PUMP_GOLDEN_SOURCE_MEDIA,
	                  0x5a5a0001U);
	fabric_pump_start();
	publish_both(6 * TICK_BYTES);

	for (tick = 0; tick < 8; tick++) {
		dma_tick(1);
		publish_both(TICK_BYTES);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, tick);
	}

	for (tick = 8; tick < 12; tick++) {
		dma_tick(1);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, tick);
	}
	for (tick = 12; tick < 20; tick++) {
		dma_tick(1);
		if (tick % 3 == 0)
			publish_both(TICK_BYTES);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, tick);
	}

	publish_both(TICK_BYTES / 3);
	g_model.done = 1;
	g_model_b.done = 1;
	for (tick = 20; tick < 32; tick++) {
		dma_tick(1);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, tick);
	}

	g_model.done = 0;
	g_model_b.done = 0;
	pump_golden_pause(&g_golden);
	audio_fabric_producer_freeze(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_ring_silence(AUDIO_FABRIC_SLOT_PUMP);
	for (tick = 32; tick < 36; tick++) {
		dma_tick(1);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, tick);
	}
	pump_golden_start(&g_golden,
	                  stream_mode ? PUMP_GOLDEN_SOURCE_STREAM
	                              : PUMP_GOLDEN_SOURCE_MEDIA,
	                  0x5a5a0001U);
	fabric_pump_start();
	publish_both(3 * TICK_BYTES);
	for (tick = 36; tick < 44; tick++) {
		dma_tick(1);
		if (tick % 2 == 0)
			publish_both(TICK_BYTES / 2);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, tick);
	}

	if (kill_source) {
		/* Session closed under us: the snapshot fails, both sides
		 * unbind in the ISR and leave the ring untouched. */
		g_model.snapshot_ok = 0;
		g_model_b.snapshot_ok = 0;
		dma_tick(1);
		pump_golden_isr(&g_golden);
		audio_fabric_isr();
		parity_compare(name, 44);
		check(g_model_b.gone == 1, "ISR unbind gone callback",
		      fmt("%s: gone=%d", name, g_model_b.gone));
		check(g_model.staged == g_model_b.staged,
		      "ISR unbind staged cursor", name);
		return;
	}

	pump_golden_stop(&g_golden);
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_PUMP);
	parity_compare(name, -1);
}

/* ---- scenario 2: two-source saturating int32 mix ---- */

static void mix_case(const char *label, int16_t a, int16_t b)
{
	int32_t expected_raw = (int32_t)a + (int32_t)b;
	int16_t expected;
	const int16_t *period;
	int j;

	if (expected_raw > 32767)
		expected = 32767;
	else if (expected_raw < -32768)
		expected = -32768;
	else
		expected = (int16_t)expected_raw;

	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, a);
	model_prefill_constant(&g_model_c, b);
	g_dma_count = 0;
	fabric_pump_start();
	(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c);
	audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_MAILBOX);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_MAILBOX);
	dma_tick(1);
	audio_fabric_isr();
	dma_tick(1);
	audio_fabric_isr();

	/* Every committed period (post-rebase slots from 2*TICK on) must
	 * be the clamped sum, word for word. */
	period = (const int16_t *)&g_fabric_tx[2 * TICK_BYTES];
	for (j = 0; j < (int)(TICK_BYTES / 2); j++) {
		if (period[j] != expected) {
			check(0, "saturating mix",
			      fmt("%s: word %d = %d, expected %d",
			          label, j, period[j], expected));
			return;
		}
	}
	check(1, "saturating mix", label);
}

static void scenario_mix(void)
{
	mix_case("clip+", 20000, 20000);   /* 40000 -> 32767 */
	mix_case("clip-", -20000, -20000); /* -40000 -> -32768 */
	mix_case("exact-rail", 16384, 16383); /* 32767: no clip */
	mix_case("cancel", 12345, -12345);    /* 0 */
}

/* ---- scenario 3: three-way ownership gate ---- */

static void scenario_gate(void)
{
	fabric_reset_state();

	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "gate: idle at rest", "ownership not IDLE");
	check(audio_fabric_output_busy() == 0, "gate: not busy at rest", "");

	g_legacy_active = 1;
	check(audio_fabric_ownership() == AUDIO_FABRIC_LEGACY_EXCLUSIVE,
	      "gate: legacy exclusive",
	      fmt("ownership=%d", audio_fabric_ownership()));
	check(audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_PUMP,
	                                   &g_ops_b) == 0,
	      "gate: attach rejected under legacy", "");

	g_legacy_active = 0;
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "gate: clean return to idle", "");

	fabric_pump_start();
	check(audio_fabric_ownership() == AUDIO_FABRIC_ACTIVE,
	      "gate: fabric active from first bind", "");
	check(audio_fabric_output_busy() != 0,
	      "gate: legacy entry rejected while active", "");
	/* A legacy client raising the PLAY bit mid-fabric must not change
	 * the ownership state (the register handler drops the bit). */
	g_legacy_active = 1;
	check(audio_fabric_ownership() == AUDIO_FABRIC_ACTIVE,
	      "gate: fabric survives legacy attempt", "");
	g_legacy_active = 0;

	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_PUMP);
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "gate: clean return to idle after detach", "");
	check(g_silence_calls == 1, "gate: last release silenced ring",
	      fmt("silence=%u", g_silence_calls));
}

/* ---- scenario 4: silence policy (KTD8) ---- */

static void scenario_silence_policy(void)
{
	uint64_t before;
	int tick;

	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, 777);
	g_model_c.done = 1;   /* zero-contribution live slot */
	g_dma_count = 0;
	fabric_pump_start();
	(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_MAILBOX);
	for (tick = 0; tick < 3; tick++) {
		dma_tick(1);
		audio_fabric_isr();
	}
	before = ring_hash(g_fabric_tx);

	/* Producer 1 stops while producer 2 is still live: the ring must
	 * NOT be silenced. */
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_PUMP);
	check(audio_fabric_output_busy() != 0,
	      "silence: fabric active after producer 1 release", "");
	check(ring_hash(g_fabric_tx) == before,
	      "silence: ring untouched on non-last release", "");
	dma_tick(1);
	audio_fabric_isr();

	/* Last producer releases: ring-level silence. */
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_MAILBOX);
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "silence: idle after last release", "");
	{
		static const uint8_t zero[AUDIO_TX_BUFFER_SIZE];

		check(ring_hash(g_fabric_tx) == ring_hash(zero),
		      "silence: ring zeroed on last release",
		      fmt("hash=%016llx",
		          (unsigned long long)ring_hash(g_fabric_tx)));
	}
}

/* ---- scenario 5: per-slot underrun isolation ---- */

static void scenario_underrun_isolation(void)
{
	static uint8_t reference[AUDIO_TX_BUFFER_SIZE];
	uint32_t solo_underruns;
	int tick;

	/* Reference: producer 1 alone on the same script. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_dma_count = 0;
	fabric_pump_start();
	model_publish(&g_model_b, 8 * TICK_BYTES);
	for (tick = 0; tick < 6; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		if (tick % 2 == 0)
			model_publish(&g_model_b, TICK_BYTES);
	}
	memcpy(reference, g_fabric_tx, sizeof(reference));
	solo_underruns = audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_PUMP);

	/* Same script with a stalled second producer: identical ring (the
	 * stalled slot contributes silence), only its counter moves. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	/* g_model_c: live, not done, forever empty -- stalled. */
	g_dma_count = 0;
	fabric_pump_start();
	(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_MAILBOX);
	model_publish(&g_model_b, 8 * TICK_BYTES);
	for (tick = 0; tick < 6; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		if (tick % 2 == 0)
			model_publish(&g_model_b, TICK_BYTES);
	}

	check(memcmp(reference, g_fabric_tx, AUDIO_TX_BUFFER_SIZE) == 0,
	      "underrun isolation: stalled slot contributes silence",
	      "ring differs from solo reference");
	check(audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_MAILBOX) > 0U,
	      "underrun isolation: stalled slot counted",
	      fmt("count=%u",
	          audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_MAILBOX)));
	check(audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_PUMP) ==
	      solo_underruns,
	      "underrun isolation: feeding slot unaffected",
	      fmt("solo=%u mixed=%u", solo_underruns,
	          audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_PUMP)));
	/* The start rebase costs each not-done live slot one played
	 * callback (the captured pump's artifact); the stall itself adds
	 * none beyond it. */
	check(g_model_c.underrun_calls == 1U,
	      "underrun isolation: only the start-rebase callback",
	      fmt("calls=%u", g_model_c.underrun_calls));
}

/* ---- scenario 6: formatter ownership across transitions ---- */

static void scenario_formatter(void)
{
	fabric_reset_state();

	/* Fresh bind from boot (formatter already at the TX ring): the
	 * claim re-points the CPU buffer but never re-inits. */
	fabric_pump_start();
	check(g_set_tx_calls == 1, "formatter: claim re-points TX buffer",
	      fmt("set_tx=%u", g_set_tx_calls));
	check(g_init_i2s_calls == 0, "formatter: no re-init at boot bind",
	      fmt("init=%u", g_init_i2s_calls));

	/* Producer churn inside fabric-active: freeze/restart/go_live and
	 * a second slot attach must not touch the formatter at all. */
	audio_fabric_producer_freeze(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_PUMP);
	(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c);
	check(g_set_tx_calls == 1 && g_init_i2s_calls == 0,
	      "formatter: idle through bind/unbind churn",
	      fmt("set_tx=%u init=%u", g_set_tx_calls, g_init_i2s_calls));
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_MAILBOX);
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_PUMP);
	check(g_set_tx_calls == 1 && g_init_i2s_calls == 0,
	      "formatter: detach silent on transport",
	      fmt("set_tx=%u init=%u", g_set_tx_calls, g_init_i2s_calls));

	/* Legacy-exclusive interlude: AHI repoints the formatter DMA and
	 * re-inits it onto its own buffer. */
	g_legacy_active = 1;
	audio_set_tx_buffer((uint8_t *)0x00400000);
	audio_init_i2s();
	check(audio_fabric_ownership() == AUDIO_FABRIC_LEGACY_EXCLUSIVE,
	      "formatter: legacy owns between sessions", "");
	g_legacy_active = 0;

	/* Fabric re-entry after legacy: the conditional recovery DOES
	 * re-init the formatter onto the standard ring. */
	g_set_tx_calls = 0U;
	g_init_i2s_calls = 0U;
	fabric_pump_start();
	check(g_set_tx_calls == 1, "formatter: re-entry re-points TX",
	      fmt("set_tx=%u", g_set_tx_calls));
	check(g_init_i2s_calls == 1,
	      "formatter: re-entry re-inits after legacy repoint",
	      fmt("init=%u", g_init_i2s_calls));
	check(audio_get_inited_tx_buffer() ==
	      (uint8_t *)AUDIO_TX_BUFFER_ADDRESS,
	      "formatter: DMA captured the standard ring", "");
}
/* Faulted and done producers contribute silence WITHOUT moving the
 * per-slot underrun counter (the R8 exclusion), in both the per-period
 * path and the start-rebase path. */
static void scenario_excluded_underrun(void)
{
	static uint8_t reference[AUDIO_TX_BUFFER_SIZE];
	uint32_t solo_underruns;
	int tick;

	/* Reference: pump alone on the script. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_dma_count = 0;
	fabric_pump_start();
	model_publish(&g_model_b, 8 * TICK_BYTES);
	for (tick = 0; tick < 6; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		if (tick % 2 == 0)
			model_publish(&g_model_b, TICK_BYTES);
	}
	memcpy(reference, g_fabric_tx, sizeof(reference));
	solo_underruns = audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_PUMP);

	/* Same script plus a FAULTED second producer (data present but
	 * faulted): identical ring, and its underrun counter never moves --
	 * not on the per-period path, not on the rebase. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_model_c.faulted = 1;
	model_publish(&g_model_c, 8 * TICK_BYTES);   /* data present */
	g_dma_count = 0;
	fabric_pump_start();
	(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_MAILBOX);
	model_publish(&g_model_b, 8 * TICK_BYTES);
	for (tick = 0; tick < 6; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		if (tick % 2 == 0)
			model_publish(&g_model_b, TICK_BYTES);
	}
	check(memcmp(reference, g_fabric_tx, AUDIO_TX_BUFFER_SIZE) == 0,
	      "excluded: faulted slot contributes silence", "");
	check(audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_MAILBOX) == 0U,
	      "excluded: faulted slot underrun counter frozen",
	      fmt("count=%u",
	          audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_MAILBOX)));
	check(g_model_c.underrun_calls == 0U,
	      "excluded: no rebase callback for a faulted slot",
	      fmt("calls=%u", g_model_c.underrun_calls));
	check(audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_PUMP) ==
	      solo_underruns,
	      "excluded: feeding slot unaffected", "");

	/* A DONE (drained) producer is excluded the same way. */
	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	g_model_c.done = 1;   /* drained end-of-stream from the start */
	g_dma_count = 0;
	fabric_pump_start();
	(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
	                                   &g_ops_c);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_MAILBOX);
	model_publish(&g_model_b, 8 * TICK_BYTES);
	for (tick = 0; tick < AUDIO_NUM_PERIODS + 2; tick++) {
		dma_tick(1);
		audio_fabric_isr();
		if (tick % 2 == 0)
			model_publish(&g_model_b, TICK_BYTES);
	}
	check(audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_MAILBOX) == 0U,
	      "excluded: drained done slot underrun counter frozen",
	      fmt("count=%u",
	          audio_fabric_slot_underruns(AUDIO_FABRIC_SLOT_MAILBOX)));
	check(g_model_c.underrun_calls == 0U,
	      "excluded: no rebase callback for a done slot", "");
}

/* A snapshot that fails mid-fill-loop silences only the remaining
 * periods of that IRQ (the pump's mid-loop behavior); the next ISR's
 * top-of-loop snapshot drops the slot outright -- an abrupt unbind
 * that leaves the ring untouched. */
static void scenario_snapshot_midloop(void)
{
	uint64_t hash_after_first;
	int tick;

	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_b, 777);
	g_dma_count = 0;
	fabric_pump_start();
	/* Warm up: lap the frontier around the whole ring so every
	 * period holds the constant. */
	for (tick = 0; tick < AUDIO_NUM_PERIODS; tick++) {
		dma_tick(1);
		audio_fabric_isr();
	}
	check(count_nonzero_periods(g_fabric_tx) == AUDIO_NUM_PERIODS,
	      "midloop: warm-up filled the whole ring",
	      fmt("nonzero=%u", count_nonzero_periods(g_fabric_tx)));

	/* Detach and re-attach for a known-zero ring (KTD8 silence on
	 * the last release) without losing the model's accounting. */
	audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_PUMP);
	fabric_pump_start();
	check(count_nonzero_periods(g_fabric_tx) == 0U,
	      "midloop: ring zeroed for the failure window", "");

	/* The next top-of-ISR snapshot succeeds; the first mid-loop
	 * refresh fails. */
	g_model_b.snapshot_fail_from =
		(int)(g_model_b.snapshot_calls + 2U);
	dma_tick(1);
	audio_fabric_isr();
	check(g_model_b.gone == 0, "midloop: slot survives its own IRQ", "");
	check(audio_fabric_output_busy() != 0,
	      "midloop: fabric still busy after mid-loop failure", "");
	check(count_nonzero_periods(g_fabric_tx) == 1U,
	      "midloop: only the pre-failure period committed",
	      fmt("nonzero=%u", count_nonzero_periods(g_fabric_tx)));
	hash_after_first = ring_hash(g_fabric_tx);

	/* The next ISR's top snapshot fails: abrupt unbind -- slot gone,
	 * fabric idle, ring untouched (no silence). */
	dma_tick(1);
	audio_fabric_isr();
	check(g_model_b.gone == 1, "midloop: next ISR drops the slot", "");
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "midloop: idle after last producer vanished", "");
	check(ring_hash(g_fabric_tx) == hash_after_first,
	      "midloop: abrupt unbind leaves the ring untouched", "");
	check(g_silence_calls == 1U,
	      "midloop: no extra ring wipe on unbind",
	      fmt("wipes=%u", g_silence_calls));
}


/* ---- scenario 7: dispatcher coverage / ring authority ---- */

static sigjmp_buf g_segv_jmp;
static volatile sig_atomic_t g_segv_hit;
static uint8_t *g_watch_ring;
static size_t g_watch_len;

static void segv_handler(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)si;
	(void)ctx;
	g_segv_hit = (sig_atomic_t)1;
	siglongjmp(g_segv_jmp, 1);
}

static void ring_protect(int writable)
{
	mprotect(g_watch_ring, g_watch_len,
	         writable ? (PROT_READ | PROT_WRITE) : PROT_READ);
}

#define RING_WINDOW(stmt) do { ring_protect(1); { stmt; } ring_protect(0); } while (0)

static void scenario_ring_authority(void)
{
	struct sigaction sa;
	struct sigaction old;
	long page = sysconf(_SC_PAGESIZE);
	size_t pages = (AUDIO_TX_BUFFER_SIZE + (size_t)page - 1U) /
	               (size_t)page;
	volatile uint64_t h0 = 0, h1 = 0;
	volatile int tick;

	if (posix_memalign((void **)&g_watch_ring, (size_t)page,
	                   pages * (size_t)page) != 0) {
		check(0, "authority: ring allocation", "posix_memalign");
		return;
	}
	g_watch_len = pages * (size_t)page;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old);

	fabric_reset_state();
	model_init(&g_model_b, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_init(&g_model_c, 48000U, 2U, SDK_AUDIO_SAMPLE_FORMAT_S16LE);
	model_prefill_constant(&g_model_c, 555);
	audio_fabric_host_set_tx_base(g_watch_ring);
	g_fabric_ring = g_watch_ring;   /* audio_silence stub target */
	model_publish(&g_model_b, 8 * TICK_BYTES);
	g_segv_hit = 0;
	if (sigsetjmp(g_segv_jmp, 1) == 0) {
		ring_protect(0);
		g_dma_count = 0;

		/* No window: none of these may write the ring. */
		fabric_pump_start();
		(void)audio_fabric_producer_attach(AUDIO_FABRIC_SLOT_MAILBOX,
		                                   &g_ops_c);
		audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_MAILBOX);
		audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_MAILBOX);
		audio_fabric_producer_freeze(AUDIO_FABRIC_SLOT_PUMP);
		audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_PUMP);
		audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_PUMP);
		h0 = ring_hash(g_watch_ring);

		for (tick = 0; tick < 4; tick++) {
			dma_tick(1);
			RING_WINDOW(audio_fabric_isr());
		}
		h1 = ring_hash(g_watch_ring);
		RING_WINDOW(audio_fabric_ring_silence(AUDIO_FABRIC_SLOT_PUMP));
		/* Non-last detach: no ring write, no window. */
		audio_fabric_producer_detach(AUDIO_FABRIC_SLOT_PUMP);
		RING_WINDOW(audio_fabric_isr());
		/* Last release silences inside detach: windowed. */
		RING_WINDOW(audio_fabric_producer_detach(
			AUDIO_FABRIC_SLOT_MAILBOX));
		ring_protect(1);
	}
	sigaction(SIGSEGV, &old, NULL);

	check(g_segv_hit == 0,
	      "authority: no writes outside compositor windows",
	      g_segv_hit ? "SIGSEGV write outside window" : "");
	check(h1 != h0, "authority: compositor filled the ring",
	      "ring unchanged across ISR windows");
	check(audio_fabric_ownership() == AUDIO_FABRIC_IDLE,
	      "authority: idle after last detach", "");
	free(g_watch_ring);
	g_watch_ring = NULL;
}

/* Source contract scan: prove by source that nothing outside the
 * compositor writes the TX ring while the fabric owns it. */
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long size;
	char *buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)size + 1U);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1U, (size_t)size, f) != (size_t)size) {
		fclose(f);
		free(buf);
		return NULL;
	}
	buf[size] = '\0';
	fclose(f);
	return buf;
}

static int count_occurrences(const char *hay, const char *needle)
{
	const char *p = hay;
	int n = 0;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/* Source-contract scan root: --srcdir on the command line (the Makefile
 * recipe passes it explicitly) or derived from argv[0] for standalone
 * runs, where a missing tree is a SKIP rather than a failure. */
static char g_srcdir[512];
static int g_srcdir_explicit;

static void scenario_source_contract(void)
{
	const char *names[4] = { "sdk_mailbox.c", "ax.c", "main.c",
	                         "audio_fabric.c" };
	char paths[4][560];
	char *srcs[4] = { NULL, NULL, NULL, NULL };
	int i;

	for (i = 0; i < 4; i++) {
		snprintf(paths[i], sizeof(paths[i]), "%s/%s", g_srcdir,
		         names[i]);
		srcs[i] = slurp(paths[i]);
	}
	if (!g_srcdir_explicit &&
	    (srcs[0] == NULL || srcs[1] == NULL || srcs[2] == NULL ||
	     srcs[3] == NULL)) {
		printf("SKIP: source contract (sources not readable at %s)\n",
		       g_srcdir);
		for (i = 0; i < 4; i++)
			free(srcs[i]);
		return;
	}

	check(srcs[0] != NULL, "contract: sdk_mailbox.c readable", paths[0]);
	if (srcs[0]) {
		check(count_occurrences(srcs[0], "AUDIO_TX_BUFFER_ADDRESS") == 0,
		      "contract: pump never names the TX ring", paths[0]);
		check(count_occurrences(srcs[0], "audio_silence") == 0,
		      "contract: pump never silences the ring", paths[0]);
		check(count_occurrences(srcs[0], "audio_init_i2s") == 0,
		      "contract: pump never re-inits the formatter", paths[0]);
		check(count_occurrences(srcs[0], "audio_set_tx_buffer") == 0,
		      "contract: pump never repoints the TX buffer", paths[0]);
		check(count_occurrences(
			      srcs[0], "audio_playback_retire_to") == 0,
		      "contract: pump no longer retires periods", paths[0]);
		check(count_occurrences(
			      srcs[0], "Xil_DCacheFlushRange") >= 1,
		      "contract: decode-side flushes remain", paths[0]);
	}

	check(srcs[1] != NULL, "contract: ax.c readable", paths[1]);
	if (srcs[1]) {
		check(count_occurrences(srcs[1], "audio_fabric_isr();") == 1,
		      "contract: isr_audio routes the compositor", paths[1]);
		check(count_occurrences(
			      srcs[1], "sdk_mailbox_audio_playback") == 0,
		      "contract: isr_audio has no pump symbols", paths[1]);
	}

	check(srcs[2] != NULL, "contract: main.c readable", paths[2]);
	if (srcs[2]) {
		check(count_occurrences(
			      srcs[2], "sdk_mailbox_audio_playback_active") == 0,
		      "contract: legacy gates widened off the pump", paths[2]);
		check(count_occurrences(srcs[2], "audio_fabric_output_busy") >= 3,
		      "contract: all three legacy gates use the fabric",
		      paths[2]);
		check(count_occurrences(
			      srcs[2], "sdk_mailbox_audio_playback_pump")
		      == 1, "contract: main-loop refill kick kept", paths[2]);
	}

	check(srcs[3] != NULL, "contract: audio_fabric.c readable", paths[3]);
	if (srcs[3]) {
		check(count_occurrences(srcs[3], "Xil_DCacheFlushRange") >= 1,
		      "contract: compositor owns the TX flush", paths[3]);
		check(count_occurrences(srcs[3], "audio_fabric_isr") >= 1,
		      "contract: compositor ISR present", paths[3]);
	}

	for (i = 0; i < 4; i++)
		free(srcs[i]);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--srcdir=", 9) == 0) {
			snprintf(g_srcdir, sizeof(g_srcdir), "%s",
			         argv[i] + 9);
			g_srcdir_explicit = 1;
		} else if (strcmp(argv[i], "--srcdir") == 0 && i + 1 < argc) {
			i++;
			snprintf(g_srcdir, sizeof(g_srcdir), "%s", argv[i]);
			g_srcdir_explicit = 1;
		}
	}
	if (!g_srcdir_explicit) {
		const char *slash = strrchr(argv[0], '/');
		size_t dir_len = (size_t)((slash != NULL ? slash :
		                             argv[0] + strlen(argv[0])) -
		                             argv[0]);

		snprintf(g_srcdir, sizeof(g_srcdir),
		         "%.*s/../../ZZ9000_proto.sdk/ZZ9000OS/src",
		         (int)dir_len, argv[0]);
	}

	printf("audio_fabric_test: parity + compositor scenarios\n");

	/* scenario 1: parity vs the captured golden (KTD6) */
	parity_run("stream48", 48000U, 2U,
	           SDK_AUDIO_SAMPLE_FORMAT_S16LE, 1, 0);
	parity_run("media44", 44100U, 2U,
	           SDK_AUDIO_SAMPLE_FORMAT_S16LE, 0, 0);
	parity_run("mono48", 48000U, 1U,
	           SDK_AUDIO_SAMPLE_FORMAT_S16LE, 1, 0);
	parity_run("media48be", 48000U, 2U,
	           SDK_AUDIO_SAMPLE_FORMAT_S16BE, 0, 0);
	parity_run("stream48kill", 48000U, 2U,
	           SDK_AUDIO_SAMPLE_FORMAT_S16LE, 1, 1);
	/* Off-table decode rate (9.6 kHz): both sides commit silent
	 * periods with the staging cursor still advancing. */
	parity_run("offtable96", 9600U, 2U,
	           SDK_AUDIO_SAMPLE_FORMAT_S16LE, 1, 0);
	characterization_invariants();

	scenario_mix();
	scenario_gate();
	scenario_silence_policy();
	scenario_underrun_isolation();
	scenario_formatter();
	scenario_excluded_underrun();
	scenario_snapshot_midloop();
	scenario_ring_authority();
	scenario_source_contract();

	if (failures != 0) {
		printf("audio_fabric_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_fabric_test: all checks passed\n");
	return 0;
}
