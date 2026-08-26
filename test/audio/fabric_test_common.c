/*
 * Shared harness for the audio fabric host-test binaries: see
 * fabric_test_common.h for what lives here and why.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fabric_test_common.h"
#include "pump_golden.h"
#include "xil_cache.h"

#ifdef XIL_CACHE_MOCK_RECORD
/* The lease-plane test binary compiles with -DXIL_CACHE_MOCK_RECORD;
 * this TU owns the mock state every other TU in that binary records
 * into (see mock/xil_cache.h). */
struct xil_cache_mock_state g_xil_cache_mock;
#endif

/* ---- assertions (suite convention) ---- */

int failures;

void check(int ok, const char *name, const char *detail)
{
	if (!ok) {
		failures++;
		printf("FAILED: %s (%s)\n", name, detail ? detail : "");
	}
}

const char *fmt(const char *format, ...)
{
	static char buffer[160];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

/* ---- deterministic PCM generator (shared by every model) ---- */

int16_t pcm_sample(uint64_t absolute_index)
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
void pcm_byte(uint8_t *dst, uint64_t byte_index, int big_endian)
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

void model_init(struct src_model *m, uint32_t rate, uint32_t channels,
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
void model_publish(struct src_model *m, uint32_t bytes)
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
int model_snapshot(struct src_model *m, void *source)
{
	struct pump_golden_source *s = source;

	if (!m->snapshot_ok)
		return 0;
	m->snapshot_calls++;
	if (m->snapshot_fail_from != 0 &&
	    m->snapshot_calls >= (uint32_t)m->snapshot_fail_from)
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
int model_stage(struct src_model *m, uint32_t bytes)
{
	if (bytes > m->produced - m->staged)
		return 0;
	m->staged += bytes;
	return 1;
}

void model_retire(struct src_model *m, uint32_t bytes)
{
	m->retired_total += bytes;
}

void model_underrun(struct src_model *m)
{
	m->underrun_calls++;
}

/* Stream play-out tail: pump_tail_pending semantics. */
void model_tail_real(struct src_model *m)
{
	if (m->is_stream)
		m->tail_pending = 1;
}

void model_tail_drained(struct src_model *m)
{
	if (m->is_stream)
		m->tail_pending = 0;
}

/* Prefill a model's whole ring with one constant sample: the exact
 * rail-arithmetic source for the saturating-mix scenario. */
void model_prefill_constant(struct src_model *m, int16_t value)
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

struct src_model g_model_b;
struct src_model g_model_c;

DEFINE_FABRIC_OPS(g_ops_b, &g_model_b)
DEFINE_FABRIC_OPS(g_ops_c, &g_model_c)

/* ---- ax.h / scene seams (main-loop stand-ins for the host build) ---- */

uint32_t g_dma_count;
int g_legacy_active;
uint32_t g_set_tx_calls, g_init_i2s_calls, g_silence_calls;
uint8_t *g_tx_ptr;
uint8_t *g_inited_tx;

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

/* R11 seam (see fabric_test_common.h). */
uint32_t g_lease_gain_bound = 255U;

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

/* ---- DMA / ring helpers ---- */

uint8_t g_fabric_tx[AUDIO_TX_BUFFER_SIZE];
uint8_t *g_fabric_ring = g_fabric_tx;
uint8_t g_lease_ring_a[AUDIO_FABRIC_LEASE_RING_BYTES];
uint8_t g_lease_ring_b[AUDIO_FABRIC_LEASE_RING_BYTES];

/* FNV-1a over the whole TX ring: the characterization fingerprint. */
uint64_t ring_hash(const uint8_t *ring)
{
	uint64_t h = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < AUDIO_TX_BUFFER_SIZE; i++) {
		h ^= ring[i];
		h *= 1099511628211ULL;
	}
	return h;
}

uint32_t count_nonzero_periods(const uint8_t *ring)
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

/* ---- scenario plumbing ---- */

void fabric_reset_state(void)
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
void fabric_pump_start(void)
{
	(void)audio_fabric_producer_attach(
		AUDIO_FABRIC_SLOT_PUMP, &g_ops_b);
	audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_PUMP);
}

/* Mirror of sdk_mailbox.c's audio_playback_start() sequencing after the
 * shared-frontier guard: the re-arm is skipped while another slot is
 * live (joining a live mix must never rewind the frontier). */
void fabric_pump_resume(void)
{
	(void)audio_fabric_producer_attach(
		AUDIO_FABRIC_SLOT_PUMP, &g_ops_b);
	if (!audio_fabric_others_live(AUDIO_FABRIC_SLOT_PUMP))
		audio_fabric_producer_restart(AUDIO_FABRIC_SLOT_PUMP);
	audio_fabric_producer_go_live(AUDIO_FABRIC_SLOT_PUMP);
}
