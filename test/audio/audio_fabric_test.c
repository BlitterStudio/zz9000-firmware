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
 *   7. dispatcher coverage: with the TX ring MMU write-protected, ring
 *      writes happen only inside compositor windows, plus a source
 *      contract scan proving no other firmware module writes the ring
 *      while the fabric is active (R1),
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
 *      under-bound request passes through untouched.
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
#include "audio_scene.h"
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

/* R11 seam: audio_fabric.c composes the lease gain through the scene
 * arbiter, which is not linked here -- the test provides the
 * link-time definition (the ax.h-setter discipline of the scene
 * suite) with an overridable ceiling bound. The default mirrors the
 * policy at its most permissive so every pre-existing scenario runs
 * unbounded, exactly as it did before the composition existed. */
static uint32_t g_lease_gain_bound = 255U;

int audio_scene_lease_gain_compose(uint32_t requested,
	struct audio_scene_lease_gain_result *result)
{
	uint32_t bound = g_lease_gain_bound;

	if (result != NULL)
		memset(result, 0, sizeof(*result));
	if (requested > 255U)
		return -1;
	if (result != NULL) {
		result->gain_bound = (double)bound;
		result->applied = (uint8_t)(requested < bound ? requested
		                                             : bound);
		result->bounded = (uint32_t)result->applied < requested;
	}
	return 0;
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

static uint8_t g_lease_ring_a[AUDIO_FABRIC_LEASE_RING_BYTES];
static uint8_t g_lease_ring_b[AUDIO_FABRIC_LEASE_RING_BYTES];

static void fabric_reset_state(void)
{
	audio_fabric_host_set_lease_rings(g_lease_ring_a, g_lease_ring_b);
	audio_fabric_reset();
	audio_fabric_host_set_tx_base(g_fabric_tx);
	g_fabric_ring = g_fabric_tx;
	g_legacy_active = 0;
	g_set_tx_calls = 0U;
	g_init_i2s_calls = 0U;
	g_silence_calls = 0U;
	g_lease_gain_bound = 255U;
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

/* ---- U3 lease plane: card-side rings, staging buffer, drivers ---- */

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

static uint32_t count_nonzero_periods(const uint8_t *ring)
{
	uint32_t count = 0U;
	uint32_t i;

	for (i = 0U; i < AUDIO_NUM_PERIODS; i++) {
		const uint8_t *p = ring + i * TICK_BYTES;
		uint32_t j;

		for (j = 0U; j < TICK_BYTES; j++) {
			if (p[j] != 0U) {
				count++;
				break;
			}
		}
	}
	return count;
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
	scenario_lease_lifecycle();
	scenario_lease_zero_contribution();
	scenario_lease_ghost_bound();
	scenario_lease_stale_write();
	scenario_lease_warm_reset();
	scenario_lease_exhaustion();
	scenario_lease_state_read();
	scenario_coexistence_sum();
	scenario_lease_gain_bound();
	scenario_ring_authority();
	scenario_source_contract();

	if (failures != 0) {
		printf("audio_fabric_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_fabric_test: all checks passed\n");
	return 0;
}
