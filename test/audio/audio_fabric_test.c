/*
 * Host tests for the audio fabric compositor (plan U2).
 *
 * The suite is built characterization-first: pump_golden.c is a verbatim
 * capture of the pre-fabric sdk_mailbox.c pump at 9b3654f, and the
 * single-producer scenarios below run that golden and the compositor
 * side by side, requiring bit-identical TX-ring bytes and identical
 * producer-side accounting (staging cursor, retirement totals, played
 * underrun callbacks, stream play-out tail) after every 20 ms tick.
 *
 * Coverage (the seven U2 scenarios):
 *   1. single-producer parity across fill offsets, conversion rates,
 *      mono/byte-swapped sources, underrun rebases and drain tails,
 *      media pause/resume and stop (KTD6),
 *   2. two-source saturating int32 mix -> S16 (KTD4),
 *   3. three-way ownership gate, both directions, clean idle returns,
 *   4. ring silence only when the last producer releases (KTD8),
 *   5. per-slot underrun isolation (R8),
 *   6. formatter ownership: no re-init across producer bind/unbind
 *      inside fabric-active; conditional re-init at fabric re-entry
 *      after a legacy-exclusive session (R2),
 *   7. dispatcher coverage: with the TX ring MMU write-protected, ring
 *      writes happen only inside compositor windows, plus a source
 *      contract scan proving no other firmware module writes the ring
 *      while the fabric is active (R1).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "audio_fabric.h"
#include "memorymap.h"
#include "pump_golden.h"
#include "sdk_mailbox.h"

/* The model snapshot fills either source struct; the golden capture
 * and the compositor must agree on the layout forever. */
_Static_assert(sizeof(struct pump_golden_source) ==
               sizeof(struct audio_fabric_source),
               "golden/fabric source snapshot layout drift");

/* ---- assertions (suite convention) ---- */

static int failures;

static void check(int ok, const char *name, const char *detail)
{
	if (!ok) {
		failures++;
		printf("FAILED: %s (%s)\n", name, detail ? detail : "");
	}
}

static const char *fmt(const char *format, ...)
{
	static char buffer[160];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

/* ---- deterministic PCM generator (shared by every model) ---- */

static int16_t pcm_sample(uint64_t absolute_index)
{
	/* Bounded, nonzero, sign-varying; identical for every model that
	 * stages the same absolute byte range. */
	uint32_t x = (uint32_t)(absolute_index * 2654435761U);
	uint32_t v = (x >> 13) ^ (x << 7);

	return (int16_t)(v % 40001) - 20000;
}

/* One byte of the deterministic stream at absolute byte index: samples
 * are 2 bytes, little- or big-endian per the source format, exactly
 * like the producer PCM rings wrap byte-wise. */
static void pcm_byte(uint8_t *dst, uint64_t byte_index, int big_endian)
{
	int16_t s = pcm_sample(byte_index / 2U);
	uint8_t lo = (uint8_t)s;
	uint8_t hi = (uint8_t)(((uint16_t)s) >> 8);

	if ((byte_index & 1U) == 0U)
		*dst = big_endian ? hi : lo;
	else
		*dst = big_endian ? lo : hi;
}

/* ---- producer-side source model (KTD2 producer ring) ---- */

#define MODEL_RING_BYTES 16384U

struct src_model {
	uint8_t ring[MODEL_RING_BYTES];
	uint32_t capacity;
	uint64_t produced;
	uint64_t staged;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	int done;
	int faulted;
	int snapshot_ok;
	int big_endian_src;
	int gone;
	int is_stream;
	/* accounting */
	uint64_t retired_total;
	uint32_t underrun_calls;
	int tail_pending;
};

static void model_init(struct src_model *m, uint32_t rate, uint32_t channels,
                       uint32_t format)
{
	memset(m, 0, sizeof(*m));
	m->capacity = MODEL_RING_BYTES;
	m->sample_rate = rate;
	m->channels = channels;
	m->sample_format = format;
	m->snapshot_ok = 1;
	m->is_stream = 1;
	m->big_endian_src =
		(format == SDK_AUDIO_SAMPLE_FORMAT_S16BE) ? 1 : 0;
}

/* Publish count PCM bytes: mirrors the decode side flushing bytes and
 * then advancing pcm_ready_total. */
static void model_publish(struct src_model *m, uint32_t bytes)
{
	uint32_t i;
	for (i = 0; i < bytes; i++) {
		uint32_t offset =
			(uint32_t)((m->produced + i) % m->capacity);

		pcm_byte(&m->ring[offset], m->produced + i,
		         m->big_endian_src);
	}
	m->produced += bytes;
}

/* Fill the source snapshot exactly the way the pump's
 * audio_pump_source_snapshot() stream branch does. */
static int model_snapshot(struct src_model *m, void *source)
{
	struct pump_golden_source *s = source;

	if (!m->snapshot_ok)
		return 0;
	memset(s, 0, sizeof(*s));
	s->ring = m->ring;
	s->capacity = m->capacity;
	s->produced_bytes = m->produced;
	s->staged_bytes = m->staged;
	s->sample_rate = m->sample_rate;
	s->channels = m->channels;
	s->sample_format = m->sample_format;
	s->done = (uint8_t)(m->done ? 1 : 0);
	s->faulted = (uint8_t)(m->faulted ? 1 : 0);
	return 1;
}

/* Stream-branch staging discipline: bytes must be available. */
static int model_stage(struct src_model *m, uint32_t bytes)
{
	if (bytes > m->produced - m->staged)
		return 0;
	m->staged += bytes;
	return 1;
}

static void model_retire(struct src_model *m, uint32_t bytes)
{
	m->retired_total += bytes;
}

static void model_underrun(struct src_model *m)
{
	m->underrun_calls++;
}

/* Stream play-out tail: pump_tail_pending semantics. */
static void model_tail_real(struct src_model *m)
{
	if (m->is_stream)
		m->tail_pending = 1;
}

static void model_tail_drained(struct src_model *m)
{
	if (m->is_stream)
		m->tail_pending = 0;
}

/* ---- golden-side wiring ---- */

static uint8_t g_golden_tx[AUDIO_TX_BUFFER_SIZE];
static uint32_t g_dma_count;

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

/* FNV-1a over the whole TX ring: the characterization fingerprint. */
static uint64_t ring_hash(const uint8_t *ring)
{
	uint64_t h = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < AUDIO_TX_BUFFER_SIZE; i++) {
		h ^= ring[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/* ---- scenario 1 driver (golden half): the pre-refactor
 * characterization run. The same script later replays against the
 * compositor for the parity assertion. ---- */

#define TICK_BYTES AUDIO_BYTES_PER_PERIOD

static void dma_tick(unsigned periods)
{
	g_dma_count += periods * TICK_BYTES;
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

/* ---- fabric-side wiring (compositor under test) ---- */

static uint8_t g_fabric_tx[AUDIO_TX_BUFFER_SIZE];
static uint8_t *g_fabric_ring = g_fabric_tx;
static int g_legacy_active;
static uint32_t g_set_tx_calls, g_init_i2s_calls, g_silence_calls;
static uint8_t *g_tx_ptr;
static uint8_t *g_inited_tx;

/* ax.h seam: the transport the compositor drives. audio_silence()
 * wipes whatever ring the fabric currently owns. */
uint32_t audio_get_dma_transfer_count(void)
{
	return g_dma_count;
}

void audio_set_tx_buffer(uint8_t *addr)
{
	g_set_tx_calls++;
	g_tx_ptr = addr;
}

uint8_t *audio_get_inited_tx_buffer(void)
{
	return g_inited_tx;
}

void audio_init_i2s(void)
{
	g_init_i2s_calls++;
	g_inited_tx = g_tx_ptr;
}

void audio_silence(void)
{
	g_silence_calls++;
	memset(g_fabric_ring, 0, AUDIO_TX_BUFFER_SIZE);
}

int audio_legacy_output_active(void)
{
	return g_legacy_active;
}

static struct src_model g_model_b;
static struct src_model g_model_c;

/* Producer ops thunks: one set per model instance. */
#define DEFINE_FABRIC_OPS(fn, MP)                                    \
static int fn##_snapshot(struct audio_fabric_source *s)              \
{                                                                    \
	return model_snapshot(MP, s);                                \
}                                                                    \
static int fn##_stage(uint32_t bytes)                               \
{                                                                    \
	return model_stage(MP, bytes);                               \
}                                                                    \
static void fn##_retire(uint32_t bytes)                             \
{                                                                    \
	model_retire(MP, bytes);                                     \
}                                                                    \
static void fn##_underrun(void)                                     \
{                                                                    \
	model_underrun(MP);                                          \
}                                                                    \
static void fn##_gone(void)                                         \
{                                                                    \
	(MP)->gone++;                                                \
}                                                                    \
static void fn##_tail_real(void)                                    \
{                                                                    \
	model_tail_real(MP);                                         \
}                                                                    \
static void fn##_tail_drained(void)                                 \
{                                                                    \
	model_tail_drained(MP);                                      \
}                                                                    \
static const struct audio_fabric_producer_ops fn = {                \
	.snapshot = fn##_snapshot,                                   \
	.stage = fn##_stage,                                         \
	.retire = fn##_retire,                                       \
	.underrun = fn##_underrun,                                   \
	.gone = fn##_gone,                                           \
	.tail_real = fn##_tail_real,                                 \
	.tail_drained = fn##_tail_drained,                           \
};

DEFINE_FABRIC_OPS(g_ops_b, &g_model_b)
DEFINE_FABRIC_OPS(g_ops_c, &g_model_c)

/* Prefill a model's whole ring with one constant sample: the exact
 * rail-arithmetic source for the saturating-mix scenario. */
static void model_prefill_constant(struct src_model *m, int16_t value)
{
	size_t i;

	for (i = 0; i < MODEL_RING_BYTES; i += 2) {
		if (m->big_endian_src) {
			m->ring[i] = (uint8_t)(((uint16_t)value) >> 8);
			m->ring[i + 1] = (uint8_t)value;
		} else {
			m->ring[i] = (uint8_t)value;
			m->ring[i + 1] = (uint8_t)(((uint16_t)value) >> 8);
		}
	}
	/* Plenty of runway; the pump pulls at staged % capacity. */
	m->produced = 4ULL * MODEL_RING_BYTES;
	m->staged = 0U;
}

static void fabric_reset_state(void)
{
	audio_fabric_reset();
	audio_fabric_host_set_tx_base(g_fabric_tx);
	g_fabric_ring = g_fabric_tx;
	g_legacy_active = 0;
	g_set_tx_calls = 0U;
	g_init_i2s_calls = 0U;
	g_silence_calls = 0U;
	/* Firmware boot state: audio_adau_init() ran audio_init_i2s() with
	 * the default TX ring before any mailbox request. */
	g_tx_ptr = (uint8_t *)AUDIO_TX_BUFFER_ADDRESS;
	g_inited_tx = (uint8_t *)AUDIO_TX_BUFFER_ADDRESS;
	memset(g_fabric_tx, 0xAA, sizeof(g_fabric_tx));
	memset(&g_model_b, 0, sizeof(g_model_b));
	memset(&g_model_c, 0, sizeof(g_model_c));
}

/* Mirror of sdk_mailbox.c's audio_playback_start() sequencing. */
static void fabric_pump_start(void)
{
	(void)audio_fabric_producer_attach(
		AUDIO_FABRIC_SLOT_PUMP, &g_ops_b);
	audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_PUMP);
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

	publish_both(4 * TICK_BYTES);
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
	check(before != ring_hash((const uint8_t *)"x"), "unused", "");
	(void)before;

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

static void scenario_source_contract(void)
{
	const char *prefix = "../../ZZ9000_proto.sdk/ZZ9000OS/src/";
	char path[160];
	char *src;

	snprintf(path, sizeof(path), "%s%s", prefix, "sdk_mailbox.c");
	src = slurp(path);
	check(src != NULL, "contract: sdk_mailbox.c readable", path);
	if (src) {
		check(count_occurrences(src, "AUDIO_TX_BUFFER_ADDRESS") == 0,
		      "contract: pump never names the TX ring", path);
		check(count_occurrences(src, "audio_silence") == 0,
		      "contract: pump never silences the ring", path);
		check(count_occurrences(src, "audio_init_i2s") == 0,
		      "contract: pump never re-inits the formatter", path);
		check(count_occurrences(src, "audio_set_tx_buffer") == 0,
		      "contract: pump never repoints the TX buffer", path);
		check(count_occurrences(
			      src, "audio_playback_retire_to") == 0,
		      "contract: pump no longer retires periods", path);
		check(count_occurrences(
			      src, "Xil_DCacheFlushRange") >= 1,
		      "contract: decode-side flushes remain", path);
		free(src);
	}

	snprintf(path, sizeof(path), "%s%s", prefix, "ax.c");
	src = slurp(path);
	check(src != NULL, "contract: ax.c readable", path);
	if (src) {
		check(count_occurrences(src, "audio_fabric_isr();") == 1,
		      "contract: isr_audio routes the compositor", path);
		check(count_occurrences(
			      src, "sdk_mailbox_audio_playback") == 0,
		      "contract: isr_audio has no pump symbols", path);
		free(src);
	}

	snprintf(path, sizeof(path), "%s%s", prefix, "main.c");
	src = slurp(path);
	check(src != NULL, "contract: main.c readable", path);
	if (src) {
		check(count_occurrences(
			      src, "sdk_mailbox_audio_playback_active") == 0,
		      "contract: legacy gates widened off the pump", path);
		check(count_occurrences(src, "audio_fabric_output_busy") >= 3,
		      "contract: all three legacy gates use the fabric",
		      path);
		check(count_occurrences(src, "sdk_mailbox_audio_playback_pump")
		      == 1, "contract: main-loop refill kick kept", path);
		free(src);
	}

	snprintf(path, sizeof(path), "%s%s", prefix, "audio_fabric.c");
	src = slurp(path);
	check(src != NULL, "contract: audio_fabric.c readable", path);
	if (src) {
		check(count_occurrences(src, "Xil_DCacheFlushRange") >= 1,
		      "contract: compositor owns the TX flush", path);
		check(count_occurrences(src, "audio_fabric_isr") >= 1,
		      "contract: compositor ISR present", path);
		free(src);
	}
}

int main(void)
{
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
	characterization_invariants();

	scenario_mix();
	scenario_gate();
	scenario_silence_policy();
	scenario_underrun_isolation();
	scenario_formatter();
	scenario_ring_authority();
	scenario_source_contract();

	if (failures != 0) {
		printf("audio_fabric_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_fabric_test: all checks passed\n");
	return 0;
}
