/*
 * ZZ9000AX audio fabric compositor core (plan U2).
 *
 * The single writer of the formatter TX ring. This module absorbed the
 * AX playback pump's per-period fill loop (fill-frontier scheduling,
 * per-period retirement, cache discipline and the silence policy)
 * from sdk_mailbox.c's TX ISR; the producer side (decode feeds,
 * backpressure kicks, drain accounting) stayed there and registers
 * here through audio_fabric_producer_ops. The fill loop is a
 * behavior-preserving port of the pre-fabric pump (KTD6: bit-identical
 * ring contents for identical single-producer inputs -- proven against
 * the captured golden by test/audio); the additions are the bounded
 * slot table (R4), the saturating int32 mix (KTD4) and per-slot
 * underrun isolation (R8). The mailbox lease plane (begin/submit/
 * release, metering, card-side ring state) lives in
 * audio_fabric_lease.c; the two translation units share the slot-table
 * layout through audio_fabric_internal.h.
 *
 * ISR contract: audio_fabric_isr() runs from the audio formatter
 * period interrupt (isr_audio, every 20 ms), is integer-only, touches
 * no VFP state, no scheduler, no console, and meets the 20 ms deadline
 * by construction (bounded ring walk, bounded per-slot fill).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "audio_fabric.h"
#include "audio_fabric_internal.h"
#include "audio_playback_frontier.h"
#include "ax.h"
#include "memorymap.h"
#include "sdk_mailbox.h"
#include "xil_cache.h"

/* Instrument-build flags (plan U5, docs/audio-fabric.md): compile the
 * firmware with -DAUDIO_FABRIC_BENCH to add ISR cost measurement, and
 * additionally -DAUDIO_FABRIC_BENCH_3SLOT to let the bench session
 * lease the firmware-reserved slot 2 (three-producer measurement).
 * Both are firmware-only; every hook below is a compile-time guard,
 * so production and host-test builds carry none of this code. */
#if defined(AUDIO_FABRIC_BENCH) && defined(AUDIO_FABRIC_HOST_TEST)
#error "AUDIO_FABRIC_BENCH is a firmware-only instrument-build flag"
#endif
#if defined(AUDIO_FABRIC_BENCH_3SLOT) && !defined(AUDIO_FABRIC_BENCH)
#error "AUDIO_FABRIC_BENCH_3SLOT extends AUDIO_FABRIC_BENCH; define both"
#endif

#ifdef AUDIO_FABRIC_BENCH
#include "xil_printf.h"
#include "xtime_l.h"
#endif

#define AUDIO_FABRIC_PERIOD_BYTES  AUDIO_BYTES_PER_PERIOD
#define AUDIO_FABRIC_RING_BYTES    AUDIO_TX_BUFFER_SIZE
#define AUDIO_FABRIC_TARGET_AHEAD \
	(AUDIO_FABRIC_RING_BYTES - 2U * AUDIO_FABRIC_PERIOD_BYTES)
#define AUDIO_FABRIC_RING_PERIODS \
	(AUDIO_FABRIC_RING_BYTES / AUDIO_FABRIC_PERIOD_BYTES)

static struct {
	/* Stored ownership is IDLE or ACTIVE; LEGACY_EXCLUSIVE is
	 * derived from the register-fed playback state on read. */
	uint8_t ownership;
	uint32_t fill_offset;     /* next TX-ring byte, period-aligned */
	struct audio_fabric_slot slot[AUDIO_FABRIC_SLOT_COUNT];
} g_audio_fabric;

/* Shared fill scratch: one period of source PCM, its stereo-expanded
 * copy, and the int32 accumulation the mix clamps from (KTD4). The
 * compositor is single-threaded (core-0 ISR, or the owner's frozen
 * setup window), so one shared set serves every slot; only the
 * converters are per-slot state. */
static int16_t g_fabric_src[AUDIO_FABRIC_PERIOD_BYTES / 2];
static int16_t g_fabric_stereo[AUDIO_FABRIC_PERIOD_BYTES / 2];
static int32_t g_fabric_mix[AUDIO_FABRIC_PERIOD_BYTES / 2];

#ifdef AUDIO_FABRIC_HOST_TEST
static uint8_t *g_fabric_tx;

void audio_fabric_host_set_tx_base(uint8_t *base)
{
	g_fabric_tx = base;
}
#else
#define g_fabric_tx ((uint8_t *)AUDIO_TX_BUFFER_ADDRESS)
#endif

struct audio_fabric_slot *fabric_slot(uint32_t slot)
{
	if (slot >= AUDIO_FABRIC_SLOT_COUNT)
		return NULL;
	return &g_audio_fabric.slot[slot];
}

static int fabric_any_attached(void)
{
	uint32_t i;

	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		if (g_audio_fabric.slot[i].attached)
			return 1;
	}
	return 0;
}

static int fabric_any_live(void)
{
	uint32_t i;

	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		if (g_audio_fabric.slot[i].live)
			return 1;
	}
	return 0;
}

/* Shared-frontier guard for restart callers: nonzero when any slot
 * other than `slot` is live. Re-arming the shared fill frontier under
 * a live mix would re-fill the other producers' staged periods and
 * double-count their staging, so media resume skips the re-arm while
 * this is set (the same guard the lease-submit path applies). */
int audio_fabric_others_live(uint32_t slot)
{
	uint32_t i;

	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		if (i != slot && g_audio_fabric.slot[i].live)
			return 1;
	}
	return 0;
}

static uint32_t fabric_underrun_bump(uint32_t count)
{
	if (count != UINT32_MAX)
		count++;
	return count;
}

/* Drop a slot's registration; the epoch survives so generations stay
 * monotonic across attach cycles. */
static void fabric_slot_drop(struct audio_fabric_slot *s)
{
	uint32_t epoch = s->epoch;

	memset(s, 0, sizeof(*s));
	s->epoch = epoch;
}

#ifdef AUDIO_FABRIC_BENCH
/*
 * Instrument-build cost accounting (plan U5, docs/audio-fabric.md).
 * The clock is the ARM global timer through the same BSP helper the
 * media profiler uses (XTime_GetTime); COUNTS_PER_SECOND is half the
 * CPU core clock, so 2 ticks = 1 CPU cycle at the card's 666.67 MHz.
 * All values are timer ticks, not cycles.
 *
 * Concurrency: the compositor ISR on core 0 is the single writer and
 * the main-loop report is the only reader, same discipline as the
 * lease cursors -- the report reads each 64-bit total with the same
 * high-low-high retry so an audio IRQ landing mid-read cannot tear
 * it. The calls/peak words are plain aligned 32-bit reads.
 *
 * Each measured region includes its own two timer reads (~tens of
 * ns); sub-microsecond regions are dominated by that overhead, which
 * is why the report distinguishes whole-tick from per-stage numbers.
 */
struct audio_fabric_bench_stage {
	volatile uint64_t ticks;   /* cumulative timer ticks */
	volatile uint32_t calls;   /* passes accumulated */
	uint32_t peak;             /* worst single pass, ticks */
};

static struct {
	/* Whole active compositor tick (frontier through tail tracking). */
	struct audio_fabric_bench_stage tick;
	/* Mix + cache: mix zero/add/commit and the per-period TX flush. */
	struct audio_fabric_bench_stage mix;
	/* Whole per-slot fill (pull, byte-swap, meter, expand, gain). */
	struct audio_fabric_bench_stage fill[AUDIO_FABRIC_SLOT_COUNT];
	/* Rate-conversion pass inside a slot's fill (resample only). */
	struct audio_fabric_bench_stage conv[AUDIO_FABRIC_SLOT_COUNT];
} g_fabric_bench;

static uint64_t fabric_bench_now(void)
{
	XTime now;

	XTime_GetTime(&now);
	return (uint64_t)now;
}

static void fabric_bench_add(struct audio_fabric_bench_stage *stage,
	uint64_t start)
{
	uint64_t delta = fabric_bench_now() - start;

	stage->ticks += delta;
	stage->calls++;
	if ((uint32_t)delta > stage->peak)
		stage->peak = (uint32_t)delta;
}

/* One numbers-only line tail; the caller prints the label so per-slot
 * rows stay a single xil_printf each. Monotonic counters: the avg is
 * approximate to one in-flight pass. */
static void fabric_bench_numbers(const struct audio_fabric_bench_stage *stage)
{
	uint64_t ticks = fabric_lease_read_cursor(&stage->ticks);
	uint32_t calls = stage->calls;
	uint32_t avg = calls != 0U ? (uint32_t)(ticks / calls) : 0U;

	xil_printf("avg=%u peak=%u n=%u\r\n", avg, stage->peak, calls);
}

/*
 * Low-rate aggregate report: call from the main loop every pass; it
 * prints one block per second on the firmware console (the ISR never
 * prints). Integer-only.
 */
void audio_fabric_bench_poll(void)
{
	static int armed;
	static XTime last_report;
	XTime now;
	uint32_t i;

	XTime_GetTime(&now);
	if (!armed) {
		armed = 1;
		last_report = now;
		return;
	}
	if ((uint64_t)(now - last_report) < (uint64_t)COUNTS_PER_SECOND)
		return;
	last_report = now;

	xil_printf("FABRIC-BENCH hz=%u ticks (2 ticks = 1 CPU cycle)\r\n",
		(unsigned int)COUNTS_PER_SECOND);
	xil_printf("FABRIC-BENCH isr ");
	fabric_bench_numbers(&g_fabric_bench.tick);
	xil_printf("FABRIC-BENCH mix ");
	fabric_bench_numbers(&g_fabric_bench.mix);
	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		xil_printf("FABRIC-BENCH slot%u fill ", (unsigned int)i);
		fabric_bench_numbers(&g_fabric_bench.fill[i]);
		xil_printf("FABRIC-BENCH slot%u conv ", (unsigned int)i);
		fabric_bench_numbers(&g_fabric_bench.conv[i]);
	}
}
#endif /* AUDIO_FABRIC_BENCH */

/*
 * Per-slot fill: the pre-fabric pump's audio_pump_fill_period, ported
 * verbatim. Writes one period of S16 stereo into the shared stereo
 * scratch and returns the number of source PCM bytes staged (zero when
 * the slot contributes silence: temporary shortage, fault, unusable
 * geometry, or a drained end-of-stream). The caller owns the mix.
 */
static uint32_t fabric_slot_fill(struct audio_fabric_slot *s)
{
	const struct audio_fabric_source *source = &s->source;
	uint8_t *slot = (uint8_t *)g_fabric_stereo;
	uint32_t rate;
	uint32_t channels;
	uint32_t src_frames;
	uint32_t src_bytes;
	uint32_t pull;
	uint32_t offset;
	uint32_t first;
	uint64_t available;
	uint8_t *ring;
	int16_t *pcm;
	uint32_t i;

	if (source->faulted || !source->ring || source->capacity == 0U ||
	    source->produced_bytes < source->staged_bytes)
		goto silence;
	rate = source->sample_rate;
	channels = source->channels;
	if (rate == 0U || channels == 0U || channels > 2U)
		goto silence;
	if (source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16LE &&
	    source->sample_format != SDK_AUDIO_SAMPLE_FORMAT_S16BE)
		goto silence;
	src_frames = rate / 50U;
	if (src_frames == 0U || src_frames > (AUDIO_FABRIC_PERIOD_BYTES / 4U))
		goto silence;
	src_bytes = src_frames * channels * 2U;
	available = source->produced_bytes - source->staged_bytes;
	if (available >= src_bytes) {
		pull = src_bytes;
	} else if (!source->done || available == 0U) {
		goto silence;
	} else {
		pull = (uint32_t)available;
	}
	/* else: true end of stream with a final PCM tail shorter than one
	 * 20 ms period. Drain it zero-padded; refusing partial pulls would
	 * pin used above zero and the stream could never report DONE. */

	/* Pull the source from the PCM ring. The decode side flushed these
	 * bytes before publishing produced_bytes, so a reader-side
	 * invalidate makes them visible on this core. */
	ring = source->ring;
	offset = audio_playback_source_offset(
		source->staged_bytes, source->capacity);
	first = source->capacity - offset;
	if (first > pull)
		first = pull;
	Xil_DCacheInvalidateRange((INTPTR)(ring + offset), first);
	memcpy(g_fabric_src, ring + offset, first);
	if (pull > first) {
		Xil_DCacheInvalidateRange((INTPTR)ring, pull - first);
		memcpy((uint8_t *)g_fabric_src + first, ring, pull - first);
	}
	if (pull < src_bytes)
		memset((uint8_t *)g_fabric_src + pull, 0, src_bytes - pull);
	if (!s->ops->stage(pull))
		goto silence;

	if (source->sample_format == SDK_AUDIO_SAMPLE_FORMAT_S16BE) {
		uint8_t *bytes = (uint8_t *)g_fabric_src;

		for (i = 0U; i < src_bytes; i += 2U) {
			uint8_t high = bytes[i];

			bytes[i] = bytes[i + 1U];
			bytes[i + 1U] = high;
		}
	}

	if (s->lease.ring != NULL)
		fabric_lease_meter(s, (const int16_t *)g_fabric_src, pull);

	pcm = g_fabric_src;
	if (channels == 1U) {
		for (i = 0; i < src_frames; i++) {
			g_fabric_stereo[2U * i] = g_fabric_src[i];
			g_fabric_stereo[2U * i + 1U] = g_fabric_src[i];
		}
		pcm = g_fabric_stereo;
	}
	if (rate == 48000U) {
		memcpy(slot, pcm, AUDIO_FABRIC_PERIOD_BYTES);
	} else {
#ifdef AUDIO_FABRIC_BENCH
		uint64_t bench_conv = fabric_bench_now();
		uint32_t bench_slot = (uint32_t)(s - g_audio_fabric.slot);
#endif
		/* Per-slot converter instance (the pump's pump_convert):
		 * re-arm on rate change; an off-table rate is an unusable
		 * geometry -- a committed but silent period, the staging
		 * cursor still advances. */
		if (rate != s->convert_rate) {
			s->convert_rate = rate;
			zz_audio_convert_init(&s->convert, rate, 48000U);
		}
		if (s->convert.ratio == NULL) {
			memset(slot, 0, AUDIO_FABRIC_PERIOD_BYTES);
			return pull;
		}
		zz_audio_convert_stream(&s->convert, pcm, (int16_t *)slot,
		                        (uint16_t)src_frames,
		                        AUDIO_FABRIC_PERIOD_BYTES / 4);
#ifdef AUDIO_FABRIC_BENCH
		fabric_bench_add(&g_fabric_bench.conv[bench_slot], bench_conv);
#endif
	}
	/* Lease gain (0..255 mixer scale; unity skips the pass so the
	 * pump's bit-identical parity is untouched): one saturating
	 * clamp per frame, then the shared mix clamps again. */
	if (s->gain != AUDIO_FABRIC_GAIN_UNITY) {
		int16_t *out = (int16_t *)slot;
		uint32_t g = s->gain;

		for (i = 0U; i < AUDIO_FABRIC_PERIOD_BYTES / 2U; i++) {
			int32_t v = ((int32_t)out[i] * (int32_t)g) >> 7;

			if (v > 32767)
				v = 32767;
			else if (v < -32768)
				v = -32768;
			out[i] = (int16_t)v;
		}
	}
	return pull;
silence:
	/* The caller commits either the mix (this slot adds nothing) or a
	 * zeroed period, matching the pump's memset-and-return-zero. */
	return 0;
}

static void fabric_mix_zero(void)
{
	memset(g_fabric_mix, 0, sizeof(g_fabric_mix));
}

static void fabric_mix_add(const int16_t *pcm)
{
	uint32_t i;

	for (i = 0U; i < AUDIO_FABRIC_PERIOD_BYTES / 2U; i++)
		g_fabric_mix[i] += pcm[i];
}

/* One saturating clamp per frame after the full int32 sum (KTD4). */
static void fabric_mix_commit(uint8_t *dst)
{
	int16_t *out = (int16_t *)dst;
	uint32_t i;

	for (i = 0U; i < AUDIO_FABRIC_PERIOD_BYTES / 2U; i++) {
		int32_t v = g_fabric_mix[i];

		if (v > 32767)
			v = 32767;
		else if (v < -32768)
			v = -32768;
		out[i] = (int16_t)v;
	}
}

/*
 * TX-fill compositor tick, called from the audio formatter period
 * interrupt every 20 ms. No-op unless the fabric owns the output with
 * at least one live producer. Integer-only; no scheduler/taskq/printf
 * access from here.
 */
void audio_fabric_isr(void)
{
	uint8_t *tx = g_fabric_tx;
	uint32_t pos_period;
	uint32_t ahead;
	uint32_t guard;
	uint32_t i;
#ifdef AUDIO_FABRIC_BENCH
	uint64_t bench_isr;
	uint64_t bench_fill;
	uint64_t bench_mix;
#endif


	if (g_audio_fabric.ownership != AUDIO_FABRIC_ACTIVE ||
	    !fabric_any_live())
		return;

	pos_period =
		audio_get_dma_transfer_count() % AUDIO_FABRIC_RING_BYTES;
	pos_period -= pos_period % AUDIO_FABRIC_PERIOD_BYTES;

	/* Retire every period the DMA advanced through since the
	 * preceding IRQ, per slot: each slot's tags record exactly how
	 * many of ITS source bytes were staged into each period; silence
	 * contributes zero. This is the playback clock -- not the decoder
	 * acknowledgement or the TX-fill frontier. Frozen slots (media
	 * pause) neither retire nor fill: the pause rewound their staging
	 * to retirement, and the resume re-arms from the current DMA
	 * position. */
	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		struct audio_fabric_slot *s = &g_audio_fabric.slot[i];
		uint32_t retired;

		if (!s->live)
			continue;
		retired = audio_playback_retire_to(
			&s->last_dma_offset, pos_period,
			AUDIO_FABRIC_PERIOD_BYTES, AUDIO_FABRIC_RING_BYTES,
			s->period_staged, AUDIO_NUM_PERIODS);
		if (retired != 0U && s->ops->retire)
			s->ops->retire(retired);
	}

	/* Refresh every live slot's source snapshot. A vanished producer
	 * drops its slot here (the pump's abrupt ISR unbind); when that
	 * was the last producer the compositor goes idle without touching
	 * the ring, exactly like the pump did. */
	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		struct audio_fabric_slot *s = &g_audio_fabric.slot[i];
		const struct audio_fabric_producer_ops *ops;

		if (!s->live)
			continue;
		if (s->ops->snapshot(&s->source))
			continue;
		ops = s->ops;
		fabric_slot_drop(s);
		if (ops->gone)
			ops->gone();
	}
	/* A vanished last producer returns the fabric to IDLE here too
	 * (the pump's abrupt ISR unbind: state clear, no ring silence). */
	if (!fabric_any_attached())
		g_audio_fabric.ownership = (uint8_t)AUDIO_FABRIC_IDLE;
	if (!fabric_any_live())
		return;

#ifdef AUDIO_FABRIC_BENCH
	/* Instrument build (U5): the tick accumulator spans one active
	 * compositor pass -- frontier through tail tracking; the idle
	 * early-outs above are not counted. */
	bench_isr = fabric_bench_now();
#endif


	/* If the DMA caught up with (or passed) the fill frontier, it has
	 * actually reached an unfilled silence slot. Count that played
	 * underrun per owing slot (not speculative attempts to fill
	 * future periods), then restart one period ahead. Circular
	 * distance: bias by the ring size BEFORE the modulo -- a plain
	 * u32 (fill - pos) % RING is wrong on wrap because 2^32 is not a
	 * multiple of the 30720-byte ring. */
	if (audio_playback_frontier_needs_rebase(
		    g_audio_fabric.fill_offset, pos_period,
		    AUDIO_FABRIC_TARGET_AHEAD, AUDIO_FABRIC_RING_BYTES)) {
		for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
			struct audio_fabric_slot *s =
				&g_audio_fabric.slot[i];

			if (!s->live)
				continue;
			if (!s->source.done && !s->source.faulted) {
				s->underruns =
					fabric_underrun_bump(s->underruns);
				if (s->ops->underrun)
					s->ops->underrun();
			}
		}
		g_audio_fabric.fill_offset =
			(pos_period + AUDIO_FABRIC_PERIOD_BYTES) %
			AUDIO_FABRIC_RING_BYTES;
	}

	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++)
		g_audio_fabric.slot[i].staged_real = 0U;

	guard = AUDIO_FABRIC_RING_PERIODS;
	while (guard--) {
		uint32_t index;
		uint32_t committed = 0U;   /* slots that staged this period */
		uint32_t next_fill;

		ahead = audio_playback_ring_distance(
			g_audio_fabric.fill_offset, pos_period,
			AUDIO_FABRIC_RING_BYTES);
		/* Stop AT the target, never past it: the frontier must stay
		 * inside [PERIOD, TARGET_AHEAD] so the caught-up reset above
		 * only fires on a genuine DMA overrun. */
		if (ahead >= AUDIO_FABRIC_TARGET_AHEAD)
			break;   /* frontier far enough ahead */
		index = g_audio_fabric.fill_offset / AUDIO_FABRIC_PERIOD_BYTES;
		for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
			struct audio_fabric_slot *s =
				&g_audio_fabric.slot[i];
			uint32_t staged;

			if (!s->live)
				continue;
#ifdef AUDIO_FABRIC_BENCH
			bench_fill = fabric_bench_now();
#endif
			staged = fabric_slot_fill(s);
#ifdef AUDIO_FABRIC_BENCH
			fabric_bench_add(&g_fabric_bench.fill[i], bench_fill);
#endif
			s->period_staged[index] = staged;
			if (staged != 0U) {
#ifdef AUDIO_FABRIC_BENCH
				bench_mix = fabric_bench_now();
#endif
				/* Deferred mix zero: the first committed
				 * slot pays it. */
				if (committed == 0U)
					fabric_mix_zero();
				fabric_mix_add(g_fabric_stereo);
#ifdef AUDIO_FABRIC_BENCH
				fabric_bench_add(&g_fabric_bench.mix, bench_mix);
#endif
				committed++;
				s->staged_real = 1U;
			} else if (!s->source.done &&
			           !s->source.faulted) {
				/* Owed but silent: this slot's underrun
				 * alone moves; the other slots and the
				 * frontier are unaffected (R8). */
				s->underruns =
					fabric_underrun_bump(s->underruns);
			}
		}
#ifdef AUDIO_FABRIC_BENCH
		bench_mix = fabric_bench_now();
#endif
		if (committed == 1U) {
			/* Single producer: the staged period is already
			 * the final mix -- copy it straight, skipping the
			 * int32 zero/add/clamp pass (byte-identical to a
			 * commit of one added source). */
			memcpy(tx + g_audio_fabric.fill_offset,
			       g_fabric_stereo, AUDIO_FABRIC_PERIOD_BYTES);
		} else if (committed != 0U) {
			fabric_mix_commit(tx + g_audio_fabric.fill_offset);
		} else {
			memset(tx + g_audio_fabric.fill_offset, 0,
			       AUDIO_FABRIC_PERIOD_BYTES);
		}
		/* The TX ring is plain cacheable DDR (no TLB override) and
		 * the audio formatter DMA does not snoop: push the period
		 * to DRAM before the frontier advances over it. ~120
		 * lines, microseconds, once per 20 ms. */
	Xil_DCacheFlushRange(
		(INTPTR)(tx + g_audio_fabric.fill_offset),
		AUDIO_FABRIC_PERIOD_BYTES);
#ifdef AUDIO_FABRIC_BENCH
		fabric_bench_add(&g_fabric_bench.mix, bench_mix);
#endif
		next_fill = audio_playback_frontier_after_fill(
			g_audio_fabric.fill_offset, committed,
			AUDIO_FABRIC_PERIOD_BYTES, AUDIO_FABRIC_RING_BYTES);
		if (next_fill == g_audio_fabric.fill_offset)
			break;
		g_audio_fabric.fill_offset = next_fill;
		/* Refresh the published source cursors before filling
		 * another period in this same IRQ. A snapshot failing here
		 * only silences that slot's contribution (the pump's
		 * mid-loop behavior); the next ISR's snapshot decides
		 * whether the source is really gone. */
		for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
			struct audio_fabric_slot *s =
				&g_audio_fabric.slot[i];

			if (!s->live)
				continue;
			if (!s->ops->snapshot(&s->source))
				memset(&s->source, 0, sizeof(s->source));
		}
	}

	/* Play-out tail tracking per slot: this ISR fires once per
	 * formatter period, so each call is one DMA period elapsed. While
	 * real PCM is flowing the tail stays armed; once the slot is
	 * exhausted its periods commit silence, and after a whole ring of
	 * them the DMA has played the last real audio out of the TX
	 * ring. Only then may end-of-stream drop (see the stream result
	 * flags). Media producers leave the tail callbacks unset. */
	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++) {
		struct audio_fabric_slot *s = &g_audio_fabric.slot[i];

		if (!s->live)
			continue;
		if (s->staged_real) {
			s->silence_run = 0U;
			s->tail_pending = 1U;
			if (s->ops->tail_real)
				s->ops->tail_real();
		} else if (s->tail_pending) {
			if (s->silence_run < AUDIO_FABRIC_RING_PERIODS)
				s->silence_run++;
			if (s->silence_run >= AUDIO_FABRIC_RING_PERIODS) {
				s->tail_pending = 0U;
				if (s->ops->tail_drained)
					s->ops->tail_drained();
			}
		}
	}
#ifdef AUDIO_FABRIC_BENCH
	fabric_bench_add(&g_fabric_bench.tick, bench_isr);
#endif
}

int audio_fabric_producer_attach(uint32_t slot,
	const struct audio_fabric_producer_ops *ops)
{
	struct audio_fabric_slot *s = fabric_slot(slot);

	if (s == NULL || ops == NULL || ops->snapshot == NULL)
		return 0;
	if (s->attached) {
		/* Producer restart inside fabric-active: freeze the slot
		 * for the cursor rebuild. NEVER touch the formatter on
		 * producer churn (R2): audio_set_tx_buffer /
		 * audio_init_i2s only run on IDLE -> ACTIVE entry. */
		s->live = 0U;
		s->frozen = 1U;
		return 1;
	}
	/* The derived three-way state (LEGACY_EXCLUSIVE is read-side,
	 * from the register-fed playback flag). */
	if (audio_fabric_ownership() == AUDIO_FABRIC_LEGACY_EXCLUSIVE)
		return 0;
	if (g_audio_fabric.ownership == AUDIO_FABRIC_IDLE) {
		/* Deterministic output target (review decision 2): repoint
		 * the CPU-side buffer at the standard TX ring and, when a
		 * legacy session's AP_TX_BUF_OFFS repointed the formatter
		 * DMA, re-init it -- the only recovery after AHI
		 * retargeting, and safe because the caller is the main
		 * loop with every compositor slot still frozen. */
		audio_set_tx_buffer((uint8_t *)AUDIO_TX_BUFFER_ADDRESS);
		if (audio_get_inited_tx_buffer() !=
		    (uint8_t *)AUDIO_TX_BUFFER_ADDRESS)
			audio_init_i2s();
	}
	fabric_slot_drop(s);
	s->ops = ops;
	s->epoch++;
	s->attached = 1U;
	s->live = 0U;
	s->frozen = 1U;
	s->gain = (uint16_t)AUDIO_FABRIC_GAIN_UNITY;
	zz_audio_convert_reset(&s->convert);
	s->convert_rate = 0U;
	/* Publish ownership LAST: an IRQ before this store still sees
	 * IDLE and stays inert. */
	__asm__ __volatile__("" ::: "memory");
	g_audio_fabric.ownership = (uint8_t)AUDIO_FABRIC_ACTIVE;
	return 1;
}

void audio_fabric_producer_detach(uint32_t slot)
{
	struct audio_fabric_slot *s = fabric_slot(slot);

	if (s == NULL || !s->attached)
		return;
	fabric_slot_drop(s);
	if (!fabric_any_attached()) {
		g_audio_fabric.ownership = (uint8_t)AUDIO_FABRIC_IDLE;
		/* Ring-level silence only now, when the last producer
		 * released (KTD8). */
		audio_silence();
	}
}

void audio_fabric_producer_freeze(uint32_t slot)
{
	struct audio_fabric_slot *s = fabric_slot(slot);

	if (s == NULL || !s->attached)
		return;
	s->live = 0U;
	s->frozen = 1U;
}

void audio_fabric_producer_go_live(uint32_t slot)
{
	struct audio_fabric_slot *s = fabric_slot(slot);

	if (s == NULL || !s->attached)
		return;
	s->frozen = 0U;
	s->live = 1U;
}

void audio_fabric_producer_restart(uint32_t slot)
{
	struct audio_fabric_slot *s = fabric_slot(slot);
	uint32_t pos;

	if (s == NULL || !s->attached)
		return;
	pos = audio_get_dma_transfer_count() % AUDIO_FABRIC_RING_BYTES;
	pos -= pos % AUDIO_FABRIC_PERIOD_BYTES;
	g_audio_fabric.fill_offset =
		(pos + AUDIO_FABRIC_PERIOD_BYTES) % AUDIO_FABRIC_RING_BYTES;
	s->last_dma_offset = pos;
	audio_playback_clear_periods(s->period_staged, AUDIO_NUM_PERIODS);
	s->silence_run = 0U;
	s->staged_real = 0U;
	zz_audio_convert_reset(&s->convert);
	s->convert_rate = 0U;
}

void audio_fabric_ring_silence(uint32_t slot)
{
	struct audio_fabric_slot *s = fabric_slot(slot);

	if (s == NULL || !s->attached)
		return;
	audio_playback_clear_periods(s->period_staged, AUDIO_NUM_PERIODS);
	audio_silence();
}

void audio_fabric_reset(void)
{
	uint32_t epoch[AUDIO_FABRIC_SLOT_COUNT];
	uint32_t i;

	/* R7 fail-closed teardown (warm reset): silence the TX ring while
	 * the fabric owns it, keep the lease epochs moving forward (a
	 * stale pre-reset handle must never validate again -- BEGIN and
	 * RELEASE bump epochs, so a post-reset lease always sits above
	 * every pre-reset one), and zero the card-side rings so no
	 * pre-reset sample can reach a post-reset lease read. BEGIN
	 * re-zeroes its ring too; this is the fail-closed belt.
	 * sdk_mailbox_init() calls this BEFORE sdk_mailbox_generation
	 * advances. */
	for (i = 0U; i < AUDIO_FABRIC_SLOT_COUNT; i++)
		epoch[i] = g_audio_fabric.slot[i].epoch;
	if (g_audio_fabric.ownership == AUDIO_FABRIC_ACTIVE)
		audio_silence();
	/* Publish IDLE before the teardown: an audio IRQ landing mid-reset
	 * checks this single store first and stays inert, instead of
	 * walking a half-zeroed slot table. */
	g_audio_fabric.ownership = (uint8_t)AUDIO_FABRIC_IDLE;
	memset(&g_audio_fabric, 0, sizeof(g_audio_fabric));
	for (i = AUDIO_FABRIC_SLOT_MAILBOX; i < AUDIO_FABRIC_SLOT_COUNT;
	     i++)
		g_audio_fabric.slot[i].epoch = epoch[i] + 1U;
	fabric_lease_reset_rings();
}

enum audio_fabric_ownership audio_fabric_ownership(void)
{
	if (g_audio_fabric.ownership == AUDIO_FABRIC_ACTIVE)
		return AUDIO_FABRIC_ACTIVE;
	if (audio_legacy_output_active())
		return AUDIO_FABRIC_LEGACY_EXCLUSIVE;
	return AUDIO_FABRIC_IDLE;
}

int audio_fabric_output_busy(void)
{
	return g_audio_fabric.ownership == AUDIO_FABRIC_ACTIVE;
}

uint32_t audio_fabric_slot_underruns(uint32_t slot)
{
	struct audio_fabric_slot *s = fabric_slot(slot);

	return s != NULL ? s->underruns : 0U;
}

