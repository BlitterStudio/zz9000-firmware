/*
 * Shared harness for the audio fabric host-test binaries:
 * audio_fabric_test.c (compositor/parity plane) and
 * audio_fabric_lease_test.c (mailbox lease plane).
 *
 * Only stable infrastructure lives here: the assertion primitives, the
 * deterministic PCM generator, the producer-side source model with its
 * ops thunks, the ax.h/scene seam stubs the compositor drives, and the
 * reset/pump-start plumbing every scenario script begins with.
 * Scenario-local state (the golden capture, lease staging buffers,
 * per-scenario helpers) stays in each test TU.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FABRIC_TEST_COMMON_H
#define FABRIC_TEST_COMMON_H

#include <stdint.h>

#include "audio_fabric.h"
#include "audio_scene.h"
#include "memorymap.h"
#include "sdk_mailbox.h"

/* One DMA period (20 ms @ 48 kHz stereo S16) -- the tick quantum. */
#define TICK_BYTES AUDIO_BYTES_PER_PERIOD

/* ---- assertions (suite convention) ---- */
extern int failures;
void check(int ok, const char *name, const char *detail);
const char *fmt(const char *format, ...);

/* ---- deterministic PCM generator (shared by every model) ---- */
int16_t pcm_sample(uint64_t absolute_index);
void pcm_byte(uint8_t *dst, uint64_t byte_index, int big_endian);

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
	/* Failure injection for the mid-loop snapshot test: snapshots
	 * fail from this (1-based) call number on. 0 disables it. */
	int snapshot_fail_from;
	uint32_t snapshot_calls;
	int big_endian_src;
	int gone;
	int is_stream;
	/* accounting */
	uint64_t retired_total;
	uint32_t underrun_calls;
	int tail_pending;
};

void model_init(struct src_model *m, uint32_t rate, uint32_t channels,
                uint32_t format);
void model_publish(struct src_model *m, uint32_t bytes);
int model_snapshot(struct src_model *m, void *source);
int model_stage(struct src_model *m, uint32_t bytes);
void model_retire(struct src_model *m, uint32_t bytes);
void model_underrun(struct src_model *m);
void model_tail_real(struct src_model *m);
void model_tail_drained(struct src_model *m);

/* Prefill a model's whole ring with one constant sample: the exact
 * rail-arithmetic source for the saturating-mix scenario. */
void model_prefill_constant(struct src_model *m, int16_t value);

/* Producer ops thunks: one set per model instance. The thunk functions
 * are TU-local; the ops struct is exported (extern in this header). */
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
const struct audio_fabric_producer_ops fn = {                       \
	.snapshot = fn##_snapshot,                                   \
	.stage = fn##_stage,                                         \
	.retire = fn##_retire,                                       \
	.underrun = fn##_underrun,                                   \
	.gone = fn##_gone,                                           \
	.tail_real = fn##_tail_real,                                 \
	.tail_drained = fn##_tail_drained,                           \
};

extern struct src_model g_model_b;
extern struct src_model g_model_c;
extern const struct audio_fabric_producer_ops g_ops_b;
extern const struct audio_fabric_producer_ops g_ops_c;

/* ---- ax.h / scene seams: the transport and arbiter the compositor
 * drives, stand-ins for the host build (ax.h-setter discipline) ---- */
extern uint32_t g_dma_count;
extern int g_legacy_active;
extern uint32_t g_set_tx_calls, g_init_i2s_calls, g_silence_calls;
extern uint8_t *g_tx_ptr;
extern uint8_t *g_inited_tx;

uint32_t audio_get_dma_transfer_count(void);
void audio_set_tx_buffer(uint8_t *addr);
uint8_t *audio_get_inited_tx_buffer(void);
void audio_init_i2s(void);
void audio_silence(void);
int audio_legacy_output_active(void);

/* R11 seam: audio_fabric_lease.c composes the lease gain through the
 * scene arbiter, which is not linked here -- this provides the
 * link-time definition (the ax.h-setter discipline of the scene
 * suite) with an overridable ceiling bound. The default mirrors the
 * policy at its most permissive so every scenario runs unbounded,
 * exactly as it did before the composition existed. */
extern uint32_t g_lease_gain_bound;
int audio_scene_lease_gain_compose(uint32_t requested,
	struct audio_scene_lease_gain_result *result);

/* ---- DMA / ring helpers ---- */
extern uint8_t g_fabric_tx[AUDIO_TX_BUFFER_SIZE];
extern uint8_t *g_fabric_ring;   /* target of the audio_silence() stub */
extern uint8_t g_lease_ring_a[AUDIO_FABRIC_LEASE_RING_BYTES];
extern uint8_t g_lease_ring_b[AUDIO_FABRIC_LEASE_RING_BYTES];

static inline void dma_tick(unsigned periods)
{
	g_dma_count += periods * TICK_BYTES;
}

/* FNV-1a over the whole TX ring: the characterization fingerprint. */
uint64_t ring_hash(const uint8_t *ring);

/* Committed (non-silent) periods in the TX ring, period-aligned. */
uint32_t count_nonzero_periods(const uint8_t *ring);

/* ---- scenario plumbing ---- */
void fabric_reset_state(void);
void fabric_pump_start(void);
void fabric_pump_resume(void);

#endif /* FABRIC_TEST_COMMON_H */
