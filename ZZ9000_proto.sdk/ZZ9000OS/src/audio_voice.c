/*
 * ZZ9000AX voice engine core.
 *
 * Integer-only wavetable voice mixer. Position and loop rules follow
 * libpt4's WIDE path (libpt4/src/voice.c, advance() and the WIDE rate
 * branch of pt4__update_voices): position is an integer sample index plus
 * a 16-bit fraction, step = (hz << 16) / 48000, forward wrap lands at
 * loopstart + overshoot, backward wrap lands at end - under - 1.
 *
 * One deliberate difference: libpt4 addresses an arena where address 0 is
 * never played, so a backward step below address 0 leaves the voice parked
 * there. Sample indexes start at 0 here, so a step below index 0 counts as
 * passing below the loop start: a voice without a loop stops, a voice with
 * a loop wraps.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_voice.h"

#include <string.h>
#if defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#endif
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

/* GCC's -O2 vectorizer turns the scalar kernels below into gathers that go
 * through the stack and NEON to ARM register moves, which are slow on the
 * A9; the NEON code in this file is written out. */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-vectorize")
#endif

void audio_voice_init(struct audio_voice_engine *e, const uint8_t *bank,
                      uint32_t bank_len, uint32_t clock)
{
	memset(e, 0, sizeof(*e));
	e->bank = bank;
	e->bank_len = bank_len;
	e->clock = clock;
	e->interp = AUDIO_VOICE_INTERP_NEAREST;
	e->master = AUDIO_VOICE_MASTER_UNITY;
}

void audio_voice_silence(struct audio_voice_engine *e)
{
	uint32_t i;

	for (i = 0; i < AUDIO_VOICE_MAX_VOICES; i++) {
		e->voice[i].active = 0;
		e->voice[i].ramp_left = 0;
		e->voice[i].stopping = 0;
	}
	for (i = 0; i < AUDIO_VOICE_FADE_SLOTS; i++) {
		e->fade[i].active = 0;
		e->fade[i].ramp_left = 0;
		e->fade[i].stopping = 0;
	}
}

int audio_voice_sample_set(struct audio_voice_engine *e, uint32_t id,
                           uint32_t offset, uint32_t length, uint32_t format)
{
	struct audio_voice_sample *s;

	if (id >= AUDIO_VOICE_MAX_SAMPLES)
		return -1;
	if (format >= AUDIO_VOICE_FMT_COUNT)
		return -1;
	if ((uint64_t)offset + length > e->bank_len)
		return -1;
	s = &e->sample[id];
	s->offset = offset;
	s->length = length;
	s->format = (uint8_t)format;
	s->defined = 1;
	return 0;
}

int audio_voice_position(const struct audio_voice_engine *e, uint32_t voice,
                         uint32_t *index, uint32_t *frac)
{
	if (voice >= AUDIO_VOICE_MAX_VOICES) {
		*index = 0;
		*frac = 0;
		return 0;
	}
	*index = e->voice[voice].pos;
	*frac = e->voice[voice].frac;
	return e->voice[voice].active;
}

static void set_rate(struct audio_voice_engine *e, struct audio_voice *v,
                     uint32_t flags, uint32_t rate)
{
	uint32_t hz;

	if (flags & AUDIO_VOICE_RATE_HZ) {
		hz = rate;
	} else {
		if (rate < AUDIO_VOICE_MIN_PERIOD)
			return;
		hz = e->clock / rate;
	}
	if (hz == 0U)
		return;
	if (hz > AUDIO_VOICE_MAX_HZ)
		hz = AUDIO_VOICE_MAX_HZ;
	v->step = (uint32_t)(((uint64_t)hz << 16) / AUDIO_VOICE_OUT_HZ);
}

/* Byte-to-frame shift of a sample format: a frame is 1, 2, 2 or 4 bytes
 * for S8, S16, S8S and S16S. */
static uint32_t frame_shift(uint8_t format)
{
	if (format == AUDIO_VOICE_FMT_S16S)
		return 2U;
	return (format == AUDIO_VOICE_FMT_S16 ||
	        format == AUDIO_VOICE_FMT_S8S) ? 1U : 0U;
}

static void start_voice(struct audio_voice_engine *e, struct audio_voice *v,
                        const struct audio_voice_cmd *c)
{
	const struct audio_voice_sample *s;
	uint32_t shift, nsamples, off, len, end;

	v->active = 0;
	v->ramp_left = 0;
	v->stopping = 0;
	if (c->flags >= AUDIO_VOICE_MAX_SAMPLES ||
	    !e->sample[c->flags].defined) {
		e->rejected++;
		return;
	}
	s = &e->sample[c->flags];
	shift = frame_shift(s->format);
	nsamples = s->length >> shift;
	off = c->arg0 >> shift;
	len = c->arg1 >> shift;
	if (off >= nsamples || (c->arg1 != 0U && len == 0U)) {
		e->rejected++;
		return;
	}
	end = nsamples;
	if (c->arg1 != 0U && (uint64_t)off + len < nsamples)
		end = off + len;

	v->shift = (uint8_t)shift;
	v->wide = s->format == AUDIO_VOICE_FMT_S16 ||
	          s->format == AUDIO_VOICE_FMT_S16S;
	v->stereo = s->format == AUDIO_VOICE_FMT_S8S ||
	            s->format == AUDIO_VOICE_FMT_S16S;
	v->base = s->offset;
	v->len = nsamples;
	v->pos = off;
	v->frac = 0;
	v->end = end;
	v->backward = 0;
	v->loopstart = 0;
	v->looplen = 0;
	v->active = 1;
}

static void set_loop(struct audio_voice_engine *e, struct audio_voice *v,
                     const struct audio_voice_cmd *c)
{
	uint32_t ls = c->arg0 >> v->shift;
	uint32_t ll = c->arg1 >> v->shift;

	if (ll == 0U) {
		if (c->arg1 == 0U && ls <= v->len) {
			v->loopstart = ls;
			v->looplen = 0;
			return;
		}
	} else if ((uint64_t)ls + ll <= v->len) {
		v->loopstart = ls;
		v->looplen = ll;
		return;
	}
	e->rejected++;
}

/* ---- Ramping ----
 * The records of a session with CONFIG ramp (AUDIO_VOICE_OP_CONFIG) change
 * a gain over e->ramp_frames frames instead of at once; see "Ramping" in
 * audio_voice.h for the rules. */

/* Q8 gain of a volume. */
static int32_t q8(uint32_t volume)
{
	return (int32_t)(volume << 8);
}

/* The gains a voice has at this frame. */
static void gains_now(const struct audio_voice *v, int32_t *l, int32_t *r)
{
	if (v->ramp_left != 0U) {
		*l = v->gain_l;
		*r = v->gain_r;
	} else {
		*l = q8(v->vol_l);
		*r = q8(v->vol_r);
	}
}

/* Start a ramp from the gains (gl, gr) to (tl, tr) over `frames` frames. The
 * per-frame change is the difference divided by the frames, truncated
 * toward zero. */
static void ramp_to(struct audio_voice *v, int32_t gl, int32_t gr,
                    int32_t tl, int32_t tr, uint32_t frames)
{
	if (gl == tl && gr == tr) {
		v->ramp_left = 0;
		return;
	}
	v->gain_l = gl;
	v->gain_r = gr;
	v->inc_l = (tl - gl) / (int32_t)frames;
	v->inc_r = (tr - gr) / (int32_t)frames;
	v->ramp_left = frames;
}
/* Defined with the mixer below. */
static uint64_t div_down(uint64_t a, uint32_t b);
static uint64_t div_up(uint64_t a, uint32_t b);
static void sync_voice(struct audio_voice_engine *e, struct audio_voice *v,
                       uint32_t to);
static void master_change(struct audio_voice_engine *e, uint32_t off,
                          uint32_t master);


/* Frames a voice that never loops plays before its pass ends for good; a
 * looping voice never ends by itself. */
static uint32_t frames_to_end(const struct audio_voice *v)
{
	const uint64_t pos = ((uint64_t)v->pos << 16) | v->frac;
	uint64_t m;

	if (v->looplen != 0U)
		return 0xFFFFFFFFU;
	if (!v->backward) {
		const uint64_t end = (uint64_t)v->end << 16;

		m = pos >= end ? 1U : div_up(end - pos, v->step);
	} else {
		const uint64_t low = (uint64_t)v->loopstart << 16;

		m = pos < low ? 1U : div_down(pos - low, v->step) + 1U;
	}
	return m > 0xFFFFFFFFU ? 0xFFFFFFFFU : (uint32_t)m;
}

/*
 * A START on a sounding voice at frame `off` of the chunk: its sound goes on
 * in a fade slot. The voice has been mixed to `off`. A slot is free when it
 * is inactive or when its end, known from when it was taken, is at or before
 * this frame; a slot that has ended but has not been mixed to its end yet is
 * mixed first, as is one that is cut, before the new tail replaces it.
 */
static void fade_out(struct audio_voice_engine *e, struct audio_voice *v)
{
	const uint32_t off = e->chunk_off;
	const uint32_t now = e->chunk_abs + off;
	struct audio_voice *slot;
	uint32_t i, free_slot = AUDIO_VOICE_FADE_SLOTS, oldest = 0, ends;
	int32_t gl, gr;

	if (v->step == 0U)
		return;
	gains_now(v, &gl, &gr);
	if (v->stopping == 0U && (gl | gr) == 0)
		return;
	for (i = 0; i < AUDIO_VOICE_FADE_SLOTS; i++) {
		if (!e->fade[i].active || (int32_t)(e->fade[i].die - now) <= 0) {
			free_slot = i;
			break;
		}
		if (e->fade_age[i] < e->fade_age[oldest])
			oldest = i;
	}
	if (free_slot == AUDIO_VOICE_FADE_SLOTS)
		free_slot = oldest;
	slot = &e->fade[free_slot];
	sync_voice(e, slot, off);
	*slot = *v;
	if (!v->stopping) {
		ramp_to(slot, gl, gr, 0, 0, e->ramp_frames);
		slot->stopping = 1;
	}
	ends = frames_to_end(slot);
	slot->die = now + (ends < slot->ramp_left ? ends : slot->ramp_left);
	e->fade_age[free_slot] = ++e->fade_seq;
}

/* A STOP on a sounding voice with ramping on. */
static void stop_ramped(struct audio_voice_engine *e, struct audio_voice *v)
{
	int32_t gl, gr;

	if (v->stopping)
		return;
	gains_now(v, &gl, &gr);
	if ((gl | gr) == 0 || v->step == 0U) {
		v->active = 0;
		v->ramp_left = 0;
		return;
	}
	ramp_to(v, gl, gr, 0, 0, e->ramp_frames);
	v->stopping = 1;
}

static void apply_config(struct audio_voice_engine *e,
                         const struct audio_voice_cmd *c)
{
	if (c->flags == 0U || (c->flags & ~AUDIO_VOICE_CONFIG_KNOWN) != 0U) {
		e->rejected++;
		return;
	}
	if ((c->flags & AUDIO_VOICE_CONFIG_RAMP) != 0U) {
		if (c->arg0 > AUDIO_VOICE_RAMP_MAX_FRAMES) {
			e->rejected++;
			return;
		}
		e->ramp_frames = c->arg0;
	}
}

static void apply(struct audio_voice_engine *e,
                  const struct audio_voice_cmd *c)
{
	const uint32_t off = e->chunk_off;
	struct audio_voice *v;
	const uint32_t ramp = e->ramp_frames;

	if (c->op == AUDIO_VOICE_OP_MASTER) {
		master_change(e, off, c->arg0 > AUDIO_VOICE_MASTER_MAX ?
		              AUDIO_VOICE_MASTER_MAX : c->arg0);
		return;
	}
	if (c->op == AUDIO_VOICE_OP_CONFIG) {
		apply_config(e, c);
		return;
	}
	if (c->voice >= AUDIO_VOICE_MAX_VOICES) {
		e->rejected++;
		return;
	}
	v = &e->voice[c->voice];
	/* The record acts on the voice as it is at its frame. */
	sync_voice(e, v, off);
	switch (c->op) {
	case AUDIO_VOICE_OP_START:
		if (ramp != 0U && v->active)
			fade_out(e, v);
		start_voice(e, v, c);
		if (ramp != 0U && v->active)
			ramp_to(v, 0, 0, q8(v->vol_l), q8(v->vol_r), ramp);
		break;
	case AUDIO_VOICE_OP_LOOP:
		set_loop(e, v, c);
		break;
	case AUDIO_VOICE_OP_SET:
		if (c->flags & AUDIO_VOICE_SET_RATE)
			set_rate(e, v, c->flags, c->arg0);
		if (c->flags & AUDIO_VOICE_SET_VOLUME) {
			int32_t gl, gr;

			gains_now(v, &gl, &gr);
			v->vol_l = c->arg1 >> 16;
			v->vol_r = c->arg1 & 0xFFFFU;
			if (v->stopping) {
				/* only the target changes: the voice fades out */
			} else if (ramp != 0U && v->active) {
				ramp_to(v, gl, gr, q8(v->vol_l), q8(v->vol_r), ramp);
			} else {
				v->ramp_left = 0;
			}
		}
		break;
	case AUDIO_VOICE_OP_STOP:
		if (ramp != 0U && v->active) {
			stop_ramped(e, v);
		} else {
			v->active = 0;
			v->ramp_left = 0;
			v->stopping = 0;
		}
		break;
	case AUDIO_VOICE_OP_DIRECTION:
		if (v->active)
			v->backward = (c->flags & AUDIO_VOICE_DIR_BACKWARD) ? 1U : 0U;
		break;
	default:
		e->rejected++;
		break;
	}
}

/*
 * ---- Mixing ----
 *
 * The result must stay bit-identical to the first engine (master dfa9b767,
 * frozen under test/audio/baseline and compared by audio_voice_baseline_test),
 * so the arithmetic is fixed:
 *
 *   sample    8-bit data is promoted by 8 bits, 16-bit data is big-endian.
 *   nearest   side = (sample * volume) >> 16 (arithmetic, per voice).
 *   linear    a + (((b - a) * frac) >> 16), a the frame at the position and
 *             b the one it blends toward, per side.
 *   sum       the voices' sides are added in int32 and, per frame, scaled by
 *             the Q8.8 master in 64 bits (>> 8, arithmetic) and clamped to
 *             S16. The int32 sum cannot overflow: a side is at most 32768 in
 *             magnitude and there are at most 32 voices, so |sum| <= 2^20.
 *
 * Only the order of the work changes. Each voice is mixed one run of frames
 * at a time into an int32 sum per side, and the master and the clamp are
 * applied once at the end. A run stops where the position math of the
 * per-frame advance would do something other than add the step: at the pass
 * end (a loop wrap or the end of the sound) and, for linear play, where the
 * blend partner of the frame is no longer the next frame of the sample. The
 * run length is computed up front from the 16.16 position, so the inner
 * loops carry no per-frame boundary checks. After a run the voice state is
 * exactly what the per-frame advance would have left.
 */

#if defined(__GNUC__)
#define AV_INLINE static inline __attribute__((always_inline))
#else
#define AV_INLINE static inline
#endif

/* The stages below are called once per voice and run: keeping them out of
 * line keeps the inner loops in registers. */
#if defined(__GNUC__)
#define AV_NOINLINE static __attribute__((noinline))
#else
#define AV_NOINLINE static
#endif

/* Unaligned 16 and 32-bit loads are cheap and legal here: ARMv7-A in
 * normal memory and x86. Everything else reads bytes. */
#if (defined(__ARM_FEATURE_UNALIGNED) || defined(__x86_64__) || \
     defined(__i386__)) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define AV_FAST_READS 1
#endif

/* The fraction of a run is kept in an int32: a run is at most
 * AUDIO_VOICE_MIX_FRAMES frames of at most 4.0 source frames each. */
typedef char av_mix_frames_fit[(AUDIO_VOICE_MIX_FRAMES * 4U < 32768U) ? 1 : -1];

/* One big-endian S16 sample. */
AV_INLINE int32_t be16s(const uint8_t *p)
{
#ifdef AV_FAST_READS
	uint16_t h;

	memcpy(&h, p, sizeof(h));
	return (int16_t)__builtin_bswap16(h);
#else
	return (int16_t)(uint16_t)(((uint32_t)p[0] << 8) | p[1]);
#endif
}

/* The sides of the frame at q named by sides (bit 0 left, bit 1 right), in
 * the units of the format (8-bit or 16-bit); a mono frame feeds both sides.
 * The other value is left alone. */
AV_INLINE void fetch(const int wide, const int stereo, const int sides,
                     const uint8_t *q, int32_t *l, int32_t *r)
{
	if (wide) {
		if (!stereo) {
			*l = be16s(q);
			*r = *l;
		} else if (sides == 3) {
#ifdef AV_FAST_READS
			uint32_t w;

			memcpy(&w, q, sizeof(w));
			w = __builtin_bswap32(w);
			*l = (int32_t)w >> 16;
			*r = (int16_t)w;
#else
			*l = be16s(q);
			*r = be16s(q + 2);
#endif
		} else if (sides == 1) {
			*l = be16s(q);
		} else {
			*r = be16s(q + 2);
		}
	} else if (!stereo) {
		*l = (int8_t)q[0];
		*r = *l;
	} else {
		if (sides & 1)
			*l = (int8_t)q[0];
		if (sides & 2)
			*r = (int8_t)q[1];
	}
}

/*
 * acc + ((s * vol) >> 16) with s in S16 range and 0 <= vol < 2^24, the
 * product exact and the shift arithmetic. On ARM with the DSP extension this
 * is one SMLAWB (32x16 multiply, high 32 of 48 bits, accumulate); the plain
 * form is the same value.
 */
AV_INLINE int32_t mac16(int32_t acc, int32_t vol, int32_t s)
{
#if defined(__ARM_FEATURE_DSP)
	return __smlawb(vol, s, acc);
#else
	return acc + ((s * vol) >> 16);
#endif
}

/*
 * Mix n frames of one voice into the sums. p is the frame at the voice
 * position, x the 16-bit fraction as an int32 that moves by dx per frame
 * (the step, negated for backward play) and nbytes the byte distance to the
 * blend partner (one frame, negated for backward play); the caller
 * guarantees that for every frame that partner is the neighbouring frame in
 * the direction of play and that both frames lie inside the sample.
 *
 * The 8-bit nearest case multiplies the raw sample by the volume times 256
 * instead of promoting it first: (s * 256 * v) >> 16 == (s * (v << 8)) >> 16.
 * The linear blend is the same value as a + (((b - a) * frac) >> 16) written
 * as (a * (65536 - frac) + b * frac) >> 16, which stays inside int32 (the
 * weights sum to 65536 and a and b are S16), so no 64-bit product is needed;
 * 8-bit data blends raw and shifts by 8 for the same reason.
 */
AV_INLINE void kernel(const int wide, const int stereo, const int linear,
                      const int sides, const uint8_t *restrict p, int32_t x,
                      const int32_t dx, const int32_t nbytes, uint32_t n,
                      const int32_t vl0, const int32_t vr0,
                      int32_t *restrict al)
{
	const int fs = wide + stereo; /* log2 of the frame size in bytes */
	const int ish = wide ? 16 : 8;
	/* the multiplier of mac16(): raw 8-bit samples carry the 8 bits */
	const int32_t vl = (!linear && !wide) ? vl0 << 8 : vl0;
	const int32_t vr = (!linear && !wide) ? vr0 << 8 : vr0;
	/* One pointer walks both sums: the right side sits a fixed distance on
	 * (an immediate offset), which frees a register in the loop. */
	const int32_t *const end = al + n;

	for (; al != end; al++) {
		const uint8_t *q = p + (x >> 16) * (1 << fs);
		int32_t l = 0, r = 0;

		fetch(wide, stereo, sides, q, &l, &r);
		if (linear) {
			int32_t bl = 0, br = 0;
			const int32_t fr = x & 0xFFFF;
			const int32_t w0 = 65536 - fr;

			fetch(wide, stereo, sides, q + nbytes, &bl, &br);
			l = (l * w0 + bl * fr) >> ish;
			r = stereo ? (r * w0 + br * fr) >> ish : l;
		}
		if (sides & 1)
			al[0] = mac16(al[0], vl, l);
		if (sides & 2)
			al[AUDIO_VOICE_MIX_FRAMES] =
				mac16(al[AUDIO_VOICE_MIX_FRAMES], vr, r);
		x += dx;
	}
}

AV_NOINLINE void run_kernel(const struct audio_voice *v, int linear,
                            int sides, const uint8_t *p, int32_t x,
                            int32_t dx, int32_t nbytes, uint32_t n,
                            int32_t vl, int32_t vr, int32_t *al)
{
#define AV_KS(w, s, l) \
	switch (sides) { \
	case 1: kernel(w, s, l, 1, p, x, dx, nbytes, n, vl, vr, al); break; \
	case 2: kernel(w, s, l, 2, p, x, dx, nbytes, n, vl, vr, al); break; \
	default: kernel(w, s, l, 3, p, x, dx, nbytes, n, vl, vr, al); break; \
	} \
	break
	switch ((v->wide ? 4 : 0) | (v->stereo ? 2 : 0) | (linear ? 1 : 0)) {
	case 0: AV_KS(0, 0, 0);
	case 1: AV_KS(0, 0, 1);
	case 2: AV_KS(0, 1, 0);
	case 3: AV_KS(0, 1, 1);
	case 4: AV_KS(1, 0, 0);
	case 5: AV_KS(1, 0, 1);
	case 6: AV_KS(1, 1, 0);
	default: AV_KS(1, 1, 1);
	}
#undef AV_KS
}

/* Frames of a linear voice that sit on the pass edge: the position does
 * not move there, only the fraction, and the blend partner is the loop
 * start (or the frame itself) instead of the next frame. la/ra and lb/rb
 * are the sides of the frame and of its partner as read from the sample. */
AV_NOINLINE void run_edge(const struct audio_voice *v, int32_t la, int32_t lb,
                          int32_t ra, int32_t rb, int32_t x, int32_t dx,
                          uint32_t n, int32_t vl, int32_t vr, int32_t *al,
                          int32_t *ar)
{
	const int ish = v->wide ? 16 : 8;
	const int stereo = v->stereo;
	uint32_t i;

	for (i = 0; i < n; i++) {
		const int32_t fr = x & 0xFFFF;
		const int32_t w0 = 65536 - fr;
		const int32_t l = (la * w0 + lb * fr) >> ish;
		const int32_t r = stereo ? (ra * w0 + rb * fr) >> ish : l;

		al[i] += (l * vl) >> 16;
		ar[i] += (r * vr) >> 16;
		x += dx;
	}
}

/* The frame a linear read blends toward: the next one in the direction of
 * play, following the loop at the pass end. */
static uint32_t neighbour(const struct audio_voice *v)
{
	if (v->backward) {
		if (v->pos > v->loopstart)
			return v->pos - 1U;
		return v->looplen != 0U ? v->loopstart + v->looplen - 1U : v->pos;
	}
	if (v->pos + 1U < v->end)
		return v->pos + 1U;
	return v->looplen != 0U ? v->loopstart : v->pos;
}

/* Divisions by the step run once per pass end, not per frame; the A9 has
 * no hardware divide, so use the 32-bit routine when the operand fits. */
static uint64_t div_down(uint64_t a, uint32_t b)
{
	if ((a >> 32) == 0U)
		return (uint32_t)a / b;
	return a / b;
}

static uint64_t div_up(uint64_t a, uint32_t b)
{
	return div_down(a + b - 1U, b);
}

/* Move the voice by `frames` steps without the pass-end handling. Returns
 * nonzero when a backward step went below frame 0 (advance() clamps the
 * position and fraction to 0 then and treats it as passing the loop start,
 * whatever the loop start is). */
static int step_state(struct audio_voice *v, uint32_t frames)
{
	const uint64_t total = (uint64_t)frames * v->step;
	uint64_t p = ((uint64_t)v->pos << 16) | v->frac;

	if (!v->backward) {
		p += total;
	} else {
		if (total > p) {
			v->pos = 0;
			v->frac = 0;
			return 1;
		}
		p -= total;
	}
	v->pos = (uint32_t)(p >> 16);
	v->frac = (uint32_t)p & 0xFFFFU;
	return 0;
}

/* The pass-end half of the per-frame advance, after step_state() moved the
 * position onto or past the end. */
static void pass_end(struct audio_voice *v, int below)
{
	if (!v->backward) {
		if (v->pos < v->end)
			return;
		if (v->looplen == 0U) {
			v->active = 0;
		} else {
			uint32_t over = v->pos - v->end;

			if (over >= v->looplen)
				over = 0;
			v->pos = v->loopstart + over;
			v->end = v->loopstart + v->looplen;
		}
	} else {
		if (!below && v->pos >= v->loopstart)
			return;
		if (v->looplen == 0U) {
			v->active = 0;
		} else {
			uint32_t under = v->loopstart - v->pos;

			if (under >= v->looplen)
				under = 0;
			v->end = v->loopstart + v->looplen;
			v->pos = v->end - under - 1U;
		}
	}
}

/*
 * ---- Ramped voices ----
 *
 * A voice whose gain is moving is mixed in two steps so that the sample
 * kernels stay as they are: the samples of the run are read (nearest or
 * linear, as for any voice) into the scratch e->smp, then added to the sums
 * with the gain of each frame, (gain + i * inc) >> 8 per side, i counting
 * from the start of the run. Every operation is the integer one of the
 * plain path, so a ramp of constant gain gives the plain result.
 */

/* Read n frames at p without a volume: the sample of each frame in 16-bit
 * units (8-bit data promoted, linear blends as in kernel()). A mono voice
 * fills only out0. */
AV_INLINE void read_kernel(const int wide, const int stereo, const int linear,
                           const uint8_t *restrict p, int32_t x,
                           const int32_t dx, const int32_t nbytes, uint32_t n,
                           int32_t *restrict out0, int32_t *restrict out1)
{
	const int fs = wide + stereo;
	const int ish = wide ? 16 : 8;
	uint32_t i;

	for (i = 0; i < n; i++) {
		const uint8_t *q = p + (x >> 16) * (1 << fs);
		int32_t l = 0, r = 0;

		fetch(wide, stereo, 3, q, &l, &r);
		if (linear) {
			int32_t bl = 0, br = 0;
			const int32_t fr = x & 0xFFFF;
			const int32_t w0 = 65536 - fr;

			fetch(wide, stereo, 3, q + nbytes, &bl, &br);
			l = (l * w0 + bl * fr) >> ish;
			r = stereo ? (r * w0 + br * fr) >> ish : l;
		} else if (!wide) {
			l *= 256;
			r *= 256;
		}
		out0[i] = l;
		if (stereo)
			out1[i] = r;
		x += dx;
	}
}

#ifndef AUDIO_VOICE_NEON_READ
#define AUDIO_VOICE_NEON_READ 1
#endif
#if defined(__ARM_NEON) && AUDIO_VOICE_NEON_READ
#define AV_NEON_READ 1

/*
 * read_kernel() four frames at a time with NEON, for the ramped voices that
 * are read into scratch. The frames are gathered one lane at a time (vld1
 * lane loads, no ARM to NEON register moves), the big-endian bytes are
 * swapped as a vector and the sides split with vuzp; the blend and the
 * promotion of 8-bit data are the integer operations of read_kernel() in
 * vector form, so the results are the same.
 */
AV_INLINE void gather4(const int wide, const int stereo, const uint8_t *q0,
                       const uint8_t *q1, const uint8_t *q2,
                       const uint8_t *q3, int32x4_t *l, int32x4_t *r)
{
	if (wide && stereo) {
		uint32x4_t g = vdupq_n_u32(0);
		int16x8_t s;
		int16x4x2_t u;

		g = vld1q_lane_u32((const uint32_t *)(const void *)q0, g, 0);
		g = vld1q_lane_u32((const uint32_t *)(const void *)q1, g, 1);
		g = vld1q_lane_u32((const uint32_t *)(const void *)q2, g, 2);
		g = vld1q_lane_u32((const uint32_t *)(const void *)q3, g, 3);
		s = vreinterpretq_s16_u8(vrev16q_u8(vreinterpretq_u8_u32(g)));
		u = vuzp_s16(vget_low_s16(s), vget_high_s16(s));
		*l = vmovl_s16(u.val[0]);
		*r = vmovl_s16(u.val[1]);
	} else if (wide) {
		uint16x4_t g = vdup_n_u16(0);

		g = vld1_lane_u16((const uint16_t *)(const void *)q0, g, 0);
		g = vld1_lane_u16((const uint16_t *)(const void *)q1, g, 1);
		g = vld1_lane_u16((const uint16_t *)(const void *)q2, g, 2);
		g = vld1_lane_u16((const uint16_t *)(const void *)q3, g, 3);
		*l = vmovl_s16(vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_u16(g))));
		*r = *l;
	} else if (stereo) {
		uint16x4_t g = vdup_n_u16(0);
		int8x8x2_t u;

		g = vld1_lane_u16((const uint16_t *)(const void *)q0, g, 0);
		g = vld1_lane_u16((const uint16_t *)(const void *)q1, g, 1);
		g = vld1_lane_u16((const uint16_t *)(const void *)q2, g, 2);
		g = vld1_lane_u16((const uint16_t *)(const void *)q3, g, 3);
		u = vuzp_s8(vreinterpret_s8_u16(g), vreinterpret_s8_u16(g));
		*l = vmovl_s16(vget_low_s16(vmovl_s8(u.val[0])));
		*r = vmovl_s16(vget_low_s16(vmovl_s8(u.val[1])));
	} else {
		uint8x8_t g = vdup_n_u8(0);

		g = vld1_lane_u8(q0, g, 0);
		g = vld1_lane_u8(q1, g, 1);
		g = vld1_lane_u8(q2, g, 2);
		g = vld1_lane_u8(q3, g, 3);
		*l = vmovl_s16(vget_low_s16(vmovl_s8(vreinterpret_s8_u8(g))));
		*r = *l;
	}
}

/* The blend of kernel(): (a * (65536 - f) + b * f) >> 16, or >> 8 for 8-bit
 * data. */
AV_INLINE int32x4_t blend4(const int wide, int32x4_t a, int32x4_t b,
                           int32x4_t w0, int32x4_t fr)
{
	const int32x4_t t = vmlaq_s32(vmulq_s32(a, w0), b, fr);

	return wide ? vshrq_n_s32(t, 16) : vshrq_n_s32(t, 8);
}

/*
 * Returns the frames done, a multiple of 4. With fuse the samples are not
 * stored: each four frames are multiplied by their gain, (gain + i * inc) >>
 * 8 per side as in apply_ramp(), and added to the sums at acc (left) and
 * acc + AUDIO_VOICE_MIX_FRAMES (right).
 */
AV_INLINE uint32_t read_kernel_neon(const int wide, const int stereo,
                                    const int linear, const int fuse,
                                    const uint8_t *restrict p, int32_t x,
                                    const int32_t dx, const int32_t nbytes,
                                    uint32_t n, int32_t *restrict out0,
                                    int32_t *restrict out1,
                                    int32_t *restrict acc, int32_t gl,
                                    int32_t il, int32_t gr, int32_t ir)
{
	const int fs = wide + stereo;
	const int32_t lanes[4] = { 0, 1, 2, 3 };
	const int32x4_t lane = vld1q_s32(lanes);
	const int32x4_t mask = vdupq_n_s32(0xFFFF);
	const int32x4_t one = vdupq_n_s32(65536);
	int32x4_t gvl = vmlaq_n_s32(vdupq_n_s32(gl), lane, il);
	int32x4_t gvr = vmlaq_n_s32(vdupq_n_s32(gr), lane, ir);
	const int32x4_t dl = vdupq_n_s32(4 * il);
	const int32x4_t dr = vdupq_n_s32(4 * ir);
	uint32_t i;

	for (i = 0; i + 4U <= n; i += 4U) {
		const uint8_t *q0 = p + (x >> 16) * (1 << fs);
		const uint8_t *q1 = p + ((x + dx) >> 16) * (1 << fs);
		const uint8_t *q2 = p + ((x + 2 * dx) >> 16) * (1 << fs);
		const uint8_t *q3 = p + ((x + 3 * dx) >> 16) * (1 << fs);
		int32x4_t l, r;

		gather4(wide, stereo, q0, q1, q2, q3, &l, &r);
		if (linear) {
			const int32x4_t fr = vandq_s32(
				vmlaq_n_s32(vdupq_n_s32(x), lane, dx), mask);
			const int32x4_t w0 = vsubq_s32(one, fr);
			int32x4_t bl, br;

			gather4(wide, stereo, q0 + nbytes, q1 + nbytes, q2 + nbytes,
			        q3 + nbytes, &bl, &br);
			l = blend4(wide, l, bl, w0, fr);
			r = stereo ? blend4(wide, r, br, w0, fr) : l;
		} else if (!wide) {
			l = vshlq_n_s32(l, 8);
			r = vshlq_n_s32(r, 8);
		}
		if (fuse) {
			const int32x4_t pl = vshrq_n_s32(
				vmulq_s32(l, vshrq_n_s32(gvl, 8)), 16);
			const int32x4_t pr = vshrq_n_s32(
				vmulq_s32(r, vshrq_n_s32(gvr, 8)), 16);

			vst1q_s32(acc + i, vaddq_s32(vld1q_s32(acc + i), pl));
			vst1q_s32(acc + AUDIO_VOICE_MIX_FRAMES + i,
			          vaddq_s32(vld1q_s32(acc + AUDIO_VOICE_MIX_FRAMES + i),
			                    pr));
			gvl = vaddq_s32(gvl, dl);
			gvr = vaddq_s32(gvr, dr);
		} else {
			vst1q_s32(out0 + i, l);
			if (stereo)
				vst1q_s32(out1 + i, r);
		}
		x += 4 * dx;
	}
	return i;
}

/* read_kernel_neon() with the gain applied, for the whole blocks of four
 * frames of a run; returns the frames done (0 without NEON). */
AV_NOINLINE uint32_t run_read_fused(const struct audio_voice *v, int linear,
                                    const uint8_t *p, int32_t x, int32_t dx,
                                    int32_t nbytes, uint32_t n, int32_t *acc,
                                    int32_t gl, int32_t il, int32_t gr,
                                    int32_t ir)
{
#define AV_F(w, s, l) \
	return read_kernel_neon(w, s, l, 1, p, x, dx, nbytes, n, NULL, NULL, acc, \
	                        gl, il, gr, ir)
	switch ((v->wide ? 4 : 0) | (v->stereo ? 2 : 0) | (linear ? 1 : 0)) {
	case 0: AV_F(0, 0, 0);
	case 1: AV_F(0, 0, 1);
	case 2: AV_F(0, 1, 0);
	case 3: AV_F(0, 1, 1);
	case 4: AV_F(1, 0, 0);
	case 5: AV_F(1, 0, 1);
	case 6: AV_F(1, 1, 0);
	default: AV_F(1, 1, 1);
	}
#undef AV_F
}
#else
AV_NOINLINE uint32_t run_read_fused(const struct audio_voice *v, int linear,
                                    const uint8_t *p, int32_t x, int32_t dx,
                                    int32_t nbytes, uint32_t n, int32_t *acc,
                                    int32_t gl, int32_t il, int32_t gr,
                                    int32_t ir)
{
	(void)v; (void)linear; (void)p; (void)x; (void)dx; (void)nbytes;
	(void)n; (void)acc; (void)gl; (void)il; (void)gr; (void)ir;
	return 0;
}
#endif

AV_NOINLINE void run_read(const struct audio_voice *v, int linear,
                          const uint8_t *p, int32_t x, int32_t dx,
                          int32_t nbytes, uint32_t n, int32_t *out0,
                          int32_t *out1)
{
#ifdef AV_NEON_READ
#define AV_R(w, s, l) \
	do { \
		const uint32_t d = read_kernel_neon(w, s, l, 0, p, x, dx, nbytes, \
		                                    n, out0, out1, NULL, 0, 0, 0, 0); \
		read_kernel(w, s, l, p, x + (int32_t)d * dx, dx, nbytes, n - d, \
		            out0 + d, out1 + d); \
	} while (0); \
	break
#else
#define AV_R(w, s, l) \
	read_kernel(w, s, l, p, x, dx, nbytes, n, out0, out1); break
#endif
	switch ((v->wide ? 4 : 0) | (v->stereo ? 2 : 0) | (linear ? 1 : 0)) {
	case 0: AV_R(0, 0, 0);
	case 1: AV_R(0, 0, 1);
	case 2: AV_R(0, 1, 0);
	case 3: AV_R(0, 1, 1);
	case 4: AV_R(1, 0, 0);
	case 5: AV_R(1, 0, 1);
	case 6: AV_R(1, 1, 0);
	default: AV_R(1, 1, 1);
	}
#undef AV_R
}

/* The edge frames of run_edge() as samples: out0 and out1 get both sides. */
AV_NOINLINE void read_edge(const struct audio_voice *v, int32_t la, int32_t lb,
                           int32_t ra, int32_t rb, int32_t x, int32_t dx,
                           uint32_t n, int32_t *out0, int32_t *out1)
{
	const int ish = v->wide ? 16 : 8;
	const int stereo = v->stereo;
	uint32_t i;

	for (i = 0; i < n; i++) {
		const int32_t fr = x & 0xFFFF;
		const int32_t w0 = 65536 - fr;
		const int32_t l = (la * w0 + lb * fr) >> ish;

		out0[i] = l;
		out1[i] = stereo ? (ra * w0 + rb * fr) >> ish : l;
		x += dx;
	}
}

/* sums += (sample * ((gain + i * inc) >> 8)) >> 16 for each side, as
 * mac16(). The gains stay between two volumes, so the volume is 0..65535. */
AV_NOINLINE void apply_ramp(int32_t *restrict al, const int32_t *restrict sl,
                            const int32_t *restrict sr, uint32_t n,
                            int32_t gl, int32_t il, int32_t gr, int32_t ir)
{
	int32_t *const ar = al + AUDIO_VOICE_MIX_FRAMES;
	uint32_t i = 0;

#ifdef __ARM_NEON
	const int32_t lane[4] = { 0, 1, 2, 3 };
	const int32x4_t idx = vld1q_s32(lane);
	int32x4_t gvl = vmlaq_n_s32(vdupq_n_s32(gl), idx, il);
	int32x4_t gvr = vmlaq_n_s32(vdupq_n_s32(gr), idx, ir);
	const int32x4_t dl = vdupq_n_s32(4 * il);
	const int32x4_t dr = vdupq_n_s32(4 * ir);

	for (; i + 4U <= n; i += 4U) {
		const int32x4_t pl = vshrq_n_s32(
			vmulq_s32(vld1q_s32(sl + i), vshrq_n_s32(gvl, 8)), 16);
		const int32x4_t pr = vshrq_n_s32(
			vmulq_s32(vld1q_s32(sr + i), vshrq_n_s32(gvr, 8)), 16);

		vst1q_s32(al + i, vaddq_s32(vld1q_s32(al + i), pl));
		vst1q_s32(ar + i, vaddq_s32(vld1q_s32(ar + i), pr));
		gvl = vaddq_s32(gvl, dl);
		gvr = vaddq_s32(gvr, dr);
	}
#endif
	for (; i < n; i++) {
		al[i] = mac16(al[i], (gl + (int32_t)i * il) >> 8, sl[i]);
		ar[i] = mac16(ar[i], (gr + (int32_t)i * ir) >> 8, sr[i]);
	}
}

/* Mix a run of a voice whose gain is moving. Returns step_state()'s result
 * for the end of the run. */
AV_NOINLINE int mix_ramp(struct audio_voice_engine *e, struct audio_voice *v,
                         int linear, uint32_t run, uint32_t safe,
                         uint32_t at)
{
	const uint8_t *const bank = e->bank;
	int32_t *const al = e->mix[0] + at;
	const int back = v->backward;
	const int32_t dx = back ? -(int32_t)v->step : (int32_t)v->step;
	const int32_t fb = (int32_t)1 << v->shift;
	int32_t el, er;
	int below;

	/* The gains at the end of the run, to see whether it is silent. */
	if (run == v->ramp_left) {
		el = v->stopping ? 0 : (int32_t)(v->vol_l << 8);
		er = v->stopping ? 0 : (int32_t)(v->vol_r << 8);
	} else {
		el = v->gain_l + v->inc_l * (int32_t)run;
		er = v->gain_r + v->inc_r * (int32_t)run;
	}
	if ((v->gain_l | v->gain_r | el | er) == 0)
		return step_state(v, run);

	if (safe != 0U) {
		const uint8_t *p = bank + v->base + ((size_t)v->pos << v->shift);
		const int32_t x = (int32_t)v->frac;
		const int32_t nb = back ? -fb : fb;
		/* whole blocks of four frames read and added in one go (NEON), the
		 * rest through the scratch */
		const uint32_t d = run_read_fused(v, linear, p, x, dx, nb, safe, al,
		                                  v->gain_l, v->inc_l, v->gain_r,
		                                  v->inc_r);

		if (d < safe) {
			run_read(v, linear, p, x + (int32_t)d * dx, dx, nb, safe - d,
			         e->smp[0], e->smp[1]);
			apply_ramp(al + d, e->smp[0],
			           v->stereo ? e->smp[1] : e->smp[0], safe - d,
			           v->gain_l + v->inc_l * (int32_t)d, v->inc_l,
			           v->gain_r + v->inc_r * (int32_t)d, v->inc_r);
		}
	}
	below = step_state(v, safe);
	if (safe < run) {
		int32_t la, lb, ra, rb;

		fetch(v->wide, v->stereo, 3,
		      bank + v->base + ((size_t)v->pos << v->shift), &la, &ra);
		fetch(v->wide, v->stereo, 3,
		      bank + v->base + ((size_t)neighbour(v) << v->shift), &lb, &rb);
		read_edge(v, la, lb, ra, rb, (int32_t)v->frac, dx, run - safe,
		          e->smp[0], e->smp[1]);
		below = step_state(v, run - safe);
		apply_ramp(al + safe, e->smp[0], v->stereo ? e->smp[1] : e->smp[0],
		           run - safe, v->gain_l + v->inc_l * (int32_t)safe, v->inc_l,
		           v->gain_r + v->inc_r * (int32_t)safe, v->inc_r);
	}
	return below;
}

/* Move a ramp on by `frames`, at most the frames left. At its end the gain
 * is the target, and a stopping voice is done. */
static void ramp_advance(struct audio_voice *v, uint32_t frames)
{
	if (frames >= v->ramp_left) {
		v->ramp_left = 0;
		v->gain_l = v->stopping ? 0 : (int32_t)(v->vol_l << 8);
		v->gain_r = v->stopping ? 0 : (int32_t)(v->vol_r << 8);
		if (v->stopping)
			v->active = 0;
	} else {
		v->ramp_left -= frames;
		v->gain_l += v->inc_l * (int32_t)frames;
		v->gain_r += v->inc_r * (int32_t)frames;
	}
}

/*
 * Mix n frames of an active voice (step != 0) into the sums, al for the left
 * side and AUDIO_VOICE_MIX_FRAMES further on for the right, advancing it. A
 * voice whose volumes are both zero adds exactly nothing to the sums and is
 * only advanced.
 */
AV_NOINLINE void mix_voice(struct audio_voice_engine *e, int linear,
                           struct audio_voice *v, uint32_t start, uint32_t n)
{
	const uint8_t *const bank = e->bank;
	int32_t *const al = e->mix[0] + start;
	const uint32_t step = v->step;
	uint32_t done = 0;

	while (done < n && v->active) {
		const uint32_t left = n - done;
		const uint64_t pos = ((uint64_t)v->pos << 16) | v->frac;
		const int back = v->backward;
		const int ramping = v->ramp_left != 0U;
		const int32_t vl = (int32_t)v->vol_l;
		const int32_t vr = (int32_t)v->vol_r;
		/* A ramping voice is planned as audible: its gain is not its
		 * volume. */
		const int audible = ramping || (v->vol_l | v->vol_r) != 0U;
		/* Which sides get sound: a side at volume 0 adds exactly
		 * nothing. */
		const int sides = (v->vol_l != 0U ? 1 : 0) |
		                  (v->vol_r != 0U ? 2 : 0);
		uint32_t run, safe;
		int hit, below;

		/* run: frames to render before the pass-end handling; hit: that
		 * handling follows the run. safe: leading frames whose blend
		 * partner is the neighbouring frame (linear play only). */
		if (!back) {
			const uint64_t end = (uint64_t)v->end << 16;

			safe = 0;
			if (pos >= end) {
				/* Not reachable: an active voice is inside its pass. */
				run = 1;
				hit = 1;
			} else {
				const uint64_t edge = end - 65536U; /* pos + 1 < end */

				if (pos + (uint64_t)left * step < end) {
					run = left;
					hit = 0;
				} else {
					run = (uint32_t)div_up(end - pos, step);
					hit = 1;
				}
				safe = run;
				if (linear && audible &&
				    pos + (uint64_t)run * step >= edge) {
					safe = 0;
					if (pos < edge) {
						const uint64_t c = div_up(edge - pos, step);

						safe = c < run ? (uint32_t)c : run;
					}
				}
			}
		} else {
			const uint64_t low = (uint64_t)v->loopstart << 16;
			const uint64_t edge = low + 65536U; /* pos > loopstart */

			if (pos < low) {
				run = 1;
				hit = 1;
			} else if ((uint64_t)left * step <= pos - low) {
				run = left;
				hit = 0;
			} else {
				run = (uint32_t)div_down(pos - low, step) + 1U;
				hit = 1;
			}
			safe = run;
			if (linear && audible &&
			    (pos < edge || (uint64_t)run * step > pos - edge)) {
				safe = 0;
				if (pos >= edge) {
					const uint64_t c = div_down(pos - edge, step) + 1U;

					safe = c < run ? (uint32_t)c : run;
				}
			}
		}
		/* A ramp ends the run too: the gain is constant after it. */
		if (ramping && v->ramp_left < run) {
			run = v->ramp_left;
			hit = 0;
			if (safe > run)
				safe = run;
		}

		if (ramping) {
			below = mix_ramp(e, v, linear, run, safe, start + done);
		} else if (audible) {
			const int32_t dx = back ? -(int32_t)step : (int32_t)step;
			const int32_t fb = (int32_t)1 << v->shift;

			if (safe != 0U)
				run_kernel(v, linear, sides,
				           bank + v->base + ((size_t)v->pos << v->shift),
				           (int32_t)v->frac, dx, back ? -fb : fb, safe, vl,
				           vr, al + done);
			below = step_state(v, safe);
			if (safe < run) {
				int32_t la, lb, ra, rb;

				fetch(v->wide, v->stereo, 3, bank + v->base +
				      ((size_t)v->pos << v->shift), &la, &ra);
				fetch(v->wide, v->stereo, 3, bank + v->base +
				      ((size_t)neighbour(v) << v->shift), &lb, &rb);
				run_edge(v, la, lb, ra, rb, (int32_t)v->frac, dx, run - safe,
				         vl, vr, al + done + safe,
				         al + AUDIO_VOICE_MIX_FRAMES + done + safe);
				below = step_state(v, run - safe);
			}
		} else {
			below = step_state(v, run);
		}
		if (hit)
			pass_end(v, below);
		if (ramping)
			ramp_advance(v, run);
		done += run;
	}
}

/*
 * Scale the sums by the master and clamp to S16: x = (sum * master) >> 8 in
 * 64 bits, arithmetic shift, then saturate, per side of every frame. Two
 * cases need no 64-bit product: at unity the shift undoes the scale, and for
 * a master up to AV_MASTER_NARROW the product of a sum (at most 2^21 in
 * magnitude: 32 voices and 32 fade slots) fits int32.
 */
#define AV_MASTER_NARROW 1023U
typedef char av_master_narrow_fits[
	((int64_t)(AUDIO_VOICE_MAX_VOICES + AUDIO_VOICE_FADE_SLOTS) * 32768 *
	 AV_MASTER_NARROW < 2147483648LL) ? 1 : -1];

static int16_t clamp16(int64_t x)
{
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static void finish_c(const int32_t *l, const int32_t *r, int16_t *out,
                     uint32_t n, uint32_t master)
{
	uint32_t i;

	if (master == AUDIO_VOICE_MASTER_UNITY) {
		for (i = 0; i < n; i++) {
			out[2U * i] = clamp16(l[i]);
			out[2U * i + 1U] = clamp16(r[i]);
		}
	} else if (master <= AV_MASTER_NARROW) {
		for (i = 0; i < n; i++) {
			out[2U * i] = clamp16((l[i] * (int32_t)master) >> 8);
			out[2U * i + 1U] = clamp16((r[i] * (int32_t)master) >> 8);
		}
	} else {
		for (i = 0; i < n; i++) {
			out[2U * i] = clamp16(((int64_t)l[i] * (int64_t)master) >> 8);
			out[2U * i + 1U] =
				clamp16(((int64_t)r[i] * (int64_t)master) >> 8);
		}
	}
}

#ifdef __ARM_NEON
/* Four sums at a time: the same arithmetic in vector form. vqmovn is the
 * S16 clamp; the 64-bit case narrows with vshrn, whose result fits int32
 * (at most 2^20 * 65535 / 256). The stores interleave left and right. */
AV_NOINLINE void finish(const int32_t *l, const int32_t *r, int16_t *out,
                        uint32_t n, uint32_t master)
{
	uint32_t i = 0;
	int16x4x2_t o;

	if (master == AUDIO_VOICE_MASTER_UNITY) {
		for (; i + 4U <= n; i += 4U) {
			o.val[0] = vqmovn_s32(vld1q_s32(l + i));
			o.val[1] = vqmovn_s32(vld1q_s32(r + i));
			vst2_s16(out + 2U * i, o);
		}
	} else if (master <= AV_MASTER_NARROW) {
		const int32x4_t m = vdupq_n_s32((int32_t)master);

		for (; i + 4U <= n; i += 4U) {
			o.val[0] = vqmovn_s32(
				vshrq_n_s32(vmulq_s32(vld1q_s32(l + i), m), 8));
			o.val[1] = vqmovn_s32(
				vshrq_n_s32(vmulq_s32(vld1q_s32(r + i), m), 8));
			vst2_s16(out + 2U * i, o);
		}
	} else {
		const int32x2_t m = vdup_n_s32((int32_t)master);

		for (; i + 4U <= n; i += 4U) {
			const int32x4_t a = vld1q_s32(l + i);
			const int32x4_t b = vld1q_s32(r + i);

			o.val[0] = vqmovn_s32(vcombine_s32(
				vshrn_n_s64(vmull_s32(vget_low_s32(a), m), 8),
				vshrn_n_s64(vmull_s32(vget_high_s32(a), m), 8)));
			o.val[1] = vqmovn_s32(vcombine_s32(
				vshrn_n_s64(vmull_s32(vget_low_s32(b), m), 8),
				vshrn_n_s64(vmull_s32(vget_high_s32(b), m), 8)));
			vst2_s16(out + 2U * i, o);
		}
	}
	finish_c(l + i, r + i, out + 2U * i, n - i, master);
}

/* Zero fill with 32 bytes per iteration. */
static void av_zero(void *p, size_t bytes)
{
	uint8_t *d = p;
	const uint8x16_t z = vdupq_n_u8(0);

	for (; bytes >= 32U; bytes -= 32U, d += 32) {
		vst1q_u8(d, z);
		vst1q_u8(d + 16, z);
	}
	if (bytes != 0U)
		memset(d, 0, bytes);
}
#else
#define finish finish_c
#define av_zero(p, bytes) memset((p), 0, (bytes))
#endif

/*
 * ---- Rendering a chunk ----
 *
 * A render call is cut into chunks of up to AUDIO_VOICE_MIX_FRAMES frames,
 * each with its own sums. A voice is not mixed in step with the call: it is
 * mixed up to a frame (sync_voice) only when something needs it there, that
 * is when a record addresses it, when a restart reuses its fade slot, when
 * the master changes and at the end of the chunk. Between those points a
 * voice runs as one long run, so a record costs the mixing of the voice it
 * names and not a pass over all 64 voices and slots.
 *
 * This gives the same output as mixing every voice up to every record,
 * because a voice's contribution to a frame does not depend on any other
 * voice. The two couplings are handled where they arise: a restart needs to
 * know which fade slots are free at its frame (fade_out(), which predicts
 * the end of each slot, so no slot is mixed for the question), and the
 * master is applied to the sums per stretch between master records.
 */

/* Whether a voice may add to the sums: active, with a rate and either a
 * volume or, while it ramps, a gain. */
static int may_sound(const struct audio_voice *v)
{
	if (!v->active || v->step == 0U)
		return 0;
	return (v->vol_l | v->vol_r) != 0U ||
	       (v->ramp_left != 0U && (v->gain_l | v->gain_r) != 0);
}

static void clear_sums(struct audio_voice_engine *e)
{
	av_zero(e->mix[0], e->chunk_n * sizeof(e->mix[0][0]));
	av_zero(e->mix[1], e->chunk_n * sizeof(e->mix[1][0]));
	e->chunk_dirty = 1;
}

/* Mix voice v up to frame `to` of the chunk. A voice without a rate does not
 * move, but its ramp runs on, so a stop or a fade finishes on time. */
static void sync_voice(struct audio_voice_engine *e, struct audio_voice *v,
                       uint32_t to)
{
	if (v->active && to > v->cur) {
		const uint32_t n = to - v->cur;

		if (v->step == 0U) {
			if (v->ramp_left != 0U)
				ramp_advance(v, v->ramp_left < n ? v->ramp_left : n);
		} else {
			if (!e->chunk_dirty && may_sound(v))
				clear_sums(e);
			mix_voice(e, e->interp == AUDIO_VOICE_INTERP_LINEAR, v, v->cur, n);
		}
	}
	v->cur = to;
}

static void sync_all(struct audio_voice_engine *e, uint32_t to)
{
	uint32_t k;

	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++)
		sync_voice(e, &e->voice[k], to);
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++)
		sync_voice(e, &e->fade[k], to);
}

/* Scale the sums of the frames from chunk_fin to `to` by the master into the
 * output; all voices must be mixed to `to`. */
static void finish_range(struct audio_voice_engine *e, uint32_t to)
{
	const uint32_t from = e->chunk_fin;

	if (to > from) {
		int16_t *o = e->chunk_out + 2U * from;

		if (e->chunk_dirty)
			finish(e->mix[0] + from, e->mix[1] + from, o, to - from,
			       e->master);
		else
			av_zero(o, 2U * (to - from) * sizeof(o[0]));
	}
	e->chunk_fin = to;
}

static void master_change(struct audio_voice_engine *e, uint32_t off,
                          uint32_t master)
{
	sync_all(e, off);
	finish_range(e, off);
	e->master = master;
}

static void begin_chunk(struct audio_voice_engine *e, int16_t *out,
                        uint32_t base, uint32_t frames)
{
	const uint32_t left = frames - base;

	e->chunk_n = left < AUDIO_VOICE_MIX_FRAMES ? left : AUDIO_VOICE_MIX_FRAMES;
	e->chunk_out = out + 2U * base;
	e->chunk_abs = e->frame + base;
	e->chunk_fin = 0;
	e->chunk_dirty = 0;
}

static void end_chunk(struct audio_voice_engine *e)
{
	uint32_t k;

	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		sync_voice(e, &e->voice[k], e->chunk_n);
		e->voice[k].cur = 0;
	}
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++) {
		sync_voice(e, &e->fade[k], e->chunk_n);
		e->fade[k].cur = 0;
	}
	finish_range(e, e->chunk_n);
}

size_t audio_voice_render(struct audio_voice_engine *e, int16_t *out,
                          uint32_t frames, const struct audio_voice_cmd *cmds,
                          size_t ncmds)
{
	uint32_t at = 0, base = 0;
	size_t used = 0;

	if (frames == 0U)
		return 0;
	begin_chunk(e, out, 0, frames);
	while (used < ncmds) {
		int32_t rel = (int32_t)(cmds[used].stamp - e->frame);
		int late = rel < 0;

		if (late)
			rel = 0;
		if ((uint32_t)rel >= frames)
			break;
		e->late += (uint32_t)late;
		/* A record out of order applies at the frame reached so far. */
		if ((uint32_t)rel > at)
			at = (uint32_t)rel;
		while (at >= base + e->chunk_n) {
			end_chunk(e);
			base += e->chunk_n;
			begin_chunk(e, out, base, frames);
		}
		e->chunk_off = at - base;
		apply(e, &cmds[used]);
		used++;
	}
	for (;;) {
		end_chunk(e);
		base += e->chunk_n;
		if (base >= frames)
			break;
		begin_chunk(e, out, base, frames);
	}
	e->frame += frames;
	return used;
}
