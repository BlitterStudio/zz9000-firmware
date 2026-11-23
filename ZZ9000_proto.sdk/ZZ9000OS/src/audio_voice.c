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
#include "audio_reduce_table.h"

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
	e->dec_quiet = AUDIO_VOICE_DEC_HISTORY;
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
	/* The decimator holds nothing of the sound that was cut. */
	memset(e->dec_hist, 0, sizeof(e->dec_hist));
	e->dec_quiet = AUDIO_VOICE_DEC_HISTORY;
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

uint32_t audio_voice_output_delay(const struct audio_voice_engine *e)
{
	return e->oversample ? AUDIO_VOICE_OVERSAMPLE_DELAY_FRAMES : 0U;
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
	v->rate = (uint32_t)(((uint64_t)hz << 16) / AUDIO_VOICE_OUT_HZ);
	v->step = v->rate >> e->oversample;
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
	v->looped = 0;
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

/* The ramp length in mixed frames (two per output frame when oversampling). */
static uint32_t ramp_len(const struct audio_voice_engine *e)
{
	return e->ramp_frames << e->oversample;
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
static void limiter_change(struct audio_voice_engine *e, uint32_t off,
                           uint32_t enabled);
static void sync_all(struct audio_voice_engine *e, uint32_t to);
static void end_chunk(struct audio_voice_engine *e);
static void mix_voice_cubic(struct audio_voice_engine *e,
                            struct audio_voice *v, uint32_t start,
                            uint32_t n);

/* The interpolation a voice reads with now, AUDIO_VOICE_INTERP_*: its own
 * setting, or else the session default. */
static uint32_t voice_interp(const struct audio_voice_engine *e,
                             const struct audio_voice *v)
{
	const uint32_t i = v->vinterp != 0U ? v->vinterp - 1U : e->interp;

	return (i == AUDIO_VOICE_INTERP_LINEAR || i == AUDIO_VOICE_INTERP_CUBIC) ?
	       i : AUDIO_VOICE_INTERP_NEAREST;
}


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
	/* The tail keeps the interpolation the voice reads with now, whatever
	 * the session default or the voice's setting becomes later. */
	slot->vinterp = (uint8_t)(voice_interp(e, v) + 1U);
	if (!v->stopping) {
		ramp_to(slot, gl, gr, 0, 0, ramp_len(e));
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
	ramp_to(v, gl, gr, 0, 0, ramp_len(e));
	v->stopping = 1;
}

/* Whether a voice or a fade slot makes sound or is about to: active, with a
 * rate, and with a volume or a ramp that is not over. */
static int is_sounding(const struct audio_voice *v)
{
	return v->active && v->step != 0U &&
	       ((v->vol_l | v->vol_r) != 0U || v->ramp_left != 0U);
}

/* Change the voice's unit of time: a mixed frame is half an output frame
 * with oversampling. Only for a voice that is silent. The step follows from
 * the rate as set, and a ramp that is still pending (a voice without a
 * rate) keeps its end gain and its length in time. */
static void retime(struct audio_voice *v, uint32_t oversample)
{
	v->step = v->rate >> oversample;
	if (v->ramp_left == 0U)
		return;
	if (oversample) {
		v->ramp_left <<= 1;
		v->inc_l /= 2;
		v->inc_r /= 2;
	} else {
		v->ramp_left = (v->ramp_left + 1U) >> 1;
		v->inc_l *= 2;
		v->inc_r *= 2;
	}
}

/*
 * CONFIG OVERSAMPLE (see "Oversampling" in audio_voice.h). The record is
 * validated whole. A change needs silence: no voice and no fade slot sounding.
 * It then ends the chunk at this frame, so that the unit of the chunks
 * changes between two of them; returns 1 for that, and the caller starts the
 * next chunk at the record's frame.
 */
static int set_oversample(struct audio_voice_engine *e,
                          const struct audio_voice_cmd *c)
{
	uint32_t k, on;

	if (c->flags != AUDIO_VOICE_CONFIG_OVERSAMPLE || c->arg1 != 0U ||
	    (c->arg0 != AUDIO_VOICE_OVERSAMPLE_OFF &&
	     c->arg0 != AUDIO_VOICE_OVERSAMPLE_2X)) {
		e->rejected++;
		return 0;
	}
	on = c->arg0 == AUDIO_VOICE_OVERSAMPLE_2X ? 1U : 0U;
	if (on == e->oversample)
		return 0;
	sync_all(e, e->chunk_off);
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		if (is_sounding(&e->voice[k])) {
			e->rejected++;
			return 0;
		}
	}
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++) {
		if (is_sounding(&e->fade[k])) {
			e->rejected++;
			return 0;
		}
	}
	/* Everything before this frame is mixed and finished in the old unit. */
	e->chunk_n = e->chunk_off;
	end_chunk(e);
	e->oversample = on;
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++)
		retime(&e->voice[k], on);
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++) {
		/* A silent tail is over; its end is a time in the old unit. */
		e->fade[k].active = 0;
		e->fade[k].ramp_left = 0;
		e->fade[k].stopping = 0;
		retime(&e->fade[k], on);
	}
	memset(e->dec_hist, 0, sizeof(e->dec_hist));
	e->dec_quiet = AUDIO_VOICE_DEC_HISTORY;
	return 1;
}

static int apply_config(struct audio_voice_engine *e,
                        const struct audio_voice_cmd *c)
{
	if (c->flags == 0U || (c->flags & ~AUDIO_VOICE_CONFIG_KNOWN) != 0U) {
		e->rejected++;
		return 0;
	}
	if ((c->flags & AUDIO_VOICE_CONFIG_INTERP) != 0U) {
		/* The default interpolation is a record of its own. Voices that
		 * follow it are mixed up to this frame with the old one first. */
		if (c->flags != AUDIO_VOICE_CONFIG_INTERP ||
		    c->arg0 < AUDIO_VOICE_INTERP_MODE_NEAREST ||
		    c->arg0 > AUDIO_VOICE_INTERP_MODE_CUBIC || c->arg1 != 0U) {
			e->rejected++;
			return 0;
		}
		sync_all(e, e->chunk_off);
		e->interp = c->arg0 - AUDIO_VOICE_INTERP_MODE_NEAREST;
		return 0;
	}
	if ((c->flags & AUDIO_VOICE_CONFIG_OVERSAMPLE) != 0U)
		return set_oversample(e, c);
	if (((c->flags & AUDIO_VOICE_CONFIG_RAMP) != 0U &&
	     c->arg0 > AUDIO_VOICE_RAMP_MAX_FRAMES) ||
	    ((c->flags & AUDIO_VOICE_CONFIG_LIMITER) != 0U &&
	     (c->arg1 > 1U ||
	      ((c->flags & AUDIO_VOICE_CONFIG_RAMP) == 0U && c->arg0 != 0U)))) {
		e->rejected++;
		return 0;
	}
	if ((c->flags & AUDIO_VOICE_CONFIG_RAMP) != 0U)
		e->ramp_frames = c->arg0;
	if ((c->flags & AUDIO_VOICE_CONFIG_LIMITER) != 0U)
		limiter_change(e, e->chunk_off, c->arg1);
	return 0;
}

/* Apply one record at the frame chunk_off of the chunk. Returns 1 when the
 * record ended the chunk there (CONFIG OVERSAMPLE): the caller then starts a
 * new chunk at the record's frame. */
static int apply(struct audio_voice_engine *e,
                 const struct audio_voice_cmd *c)
{
	const uint32_t off = e->chunk_off;
	struct audio_voice *v;
	const uint32_t ramp = ramp_len(e);

	if (c->op == AUDIO_VOICE_OP_MASTER) {
		master_change(e, off, c->arg0 > AUDIO_VOICE_MASTER_MAX ?
		              AUDIO_VOICE_MASTER_MAX : c->arg0);
		return 0;
	}
	if (c->op == AUDIO_VOICE_OP_CONFIG) {
		return apply_config(e, c);
	}
	if (c->voice >= AUDIO_VOICE_MAX_VOICES) {
		e->rejected++;
		return 0;
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
	case AUDIO_VOICE_OP_MODE:
		if (c->flags != AUDIO_VOICE_MODE_INTERP ||
		    c->arg0 > AUDIO_VOICE_INTERP_MODE_CUBIC || c->arg1 != 0U)
			e->rejected++;
		else
			v->vinterp = (uint8_t)c->arg0;
		break;
	default:
		e->rejected++;
		break;
	}
	return 0;
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
 *             S16 unless the optional static limiter is enabled. The int32
 *             sum cannot overflow: each of 32 voices and 32 fade slots is
 *             at most 32768 in magnitude, so |sum| <= 2^21.
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
			v->looped = 1;
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
			v->looped = 1;
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
 * ---- Cubic voices ----
 *
 * A cubic voice reads four frames per output frame, a, b, c and d: the frame
 * before the position in play order, the frame at the position and the next
 * two (see "Interpolation" in audio_voice.h), and mixes
 *
 *   b + (t * (c1 + t * (c2 + t * c3))) / 2
 *   c1 = c - a, c3 = d - a + 3 (b - c), c2 = a + c - 2b - c3
 *
 * which is Catmull-Rom with t the 16-bit fraction, in 16-bit units (8-bit
 * data is promoted by 8 bits first). The three products are rounded to
 * nearest; the vector kernel uses VQRDMULH with t in the high bits, which is
 * the same value, so the scalar and the vector code agree bit for bit. The
 * weights of the four frames add up to at most 1.25 in magnitude, so a side
 * can leave the S16 range by up to a quarter, AV_CUBIC_PEAK in all: the
 * volume is applied in 64 bits (or as VQDMULH on the sample shifted up by
 * 15), and the master and the limiter keep the margin (see below).
 *
 * A run is split in three. The frames whose four taps are the neighbours in
 * memory, in the direction of play, form the interior and go through the
 * vector kernel four at a time. The frames at the pass edges (the first
 * frame of the sample, the loop start after a wrap, the last two frames of
 * the pass, the other way round for backward play) resolve their taps one
 * at a time with cubic_taps(), which follows the loop and the first-pass
 * end the way neighbour() does for the linear blend. Positions only move in
 * one direction inside a run, so the interior is one stretch between two
 * edge stretches.
 */
#define AV_CUBIC_PEAK 40961U

AV_INLINE int32_t cubic_interp(int32_t a, int32_t b, int32_t c, int32_t d,
                               int32_t t)
{
	const int32_t c1 = c - a;
	const int32_t c3 = (d - a) + 3 * (b - c);
	const int32_t c2 = (a + c - 2 * b) - c3;
	int32_t u;

	u = c2 + (int32_t)(((int64_t)c3 * t + 32768) >> 16);
	u = c1 + (int32_t)(((int64_t)u * t + 32768) >> 16);
	return b + (int32_t)(((int64_t)u * t + 65536) >> 17);
}

/* (sample * volume) >> 16 for a cubic sample, which may be beyond S16. */
AV_INLINE int32_t cubic_gain(int32_t s, int32_t vol)
{
	return (int32_t)(((int64_t)s * vol) >> 16);
}

/*
 * The frames the taps a, b, c and d read for a voice about to read the frame
 * at index pos. Forward, the frame after the pass end is the loop start (or
 * the last frame again without a loop) and the frame before the loop start
 * is the loop end once a pass has wrapped; backward, the frame after the
 * loop start is the loop end and the frame before the pass end is the loop
 * start once a pass has wrapped. The first frame of the sample has itself
 * before it.
 */
static void cubic_taps(const struct audio_voice *v, uint32_t pos,
                       uint32_t idx[4])
{
	const uint32_t ls = v->loopstart;
	const uint32_t ll = v->looplen;
	const uint32_t le = ls + ll;
	uint32_t before, next, after;

	if (!v->backward) {
		if (pos + 1U < v->end) {
			next = pos + 1U;
			after = pos + 2U < v->end ? pos + 2U : (ll != 0U ? ls : next);
		} else {
			next = ll != 0U ? ls : pos;
			after = ll > 1U ? ls + 1U : next;
		}
		before = (v->looped && ll != 0U && pos == ls) ? le - 1U :
		         (pos != 0U ? pos - 1U : pos);
	} else {
		if (pos > ls) {
			next = pos - 1U;
			after = next > ls ? next - 1U : (ll != 0U ? le - 1U : next);
		} else {
			next = ll != 0U ? le - 1U : pos;
			after = ll > 1U ? le - 2U : next;
		}
		before = pos + 1U < v->end ? pos + 1U :
		         ((v->looped && ll != 0U) ? ls : pos);
	}
	idx[0] = before;
	idx[1] = pos;
	idx[2] = next;
	idx[3] = after;
}

/* Frames k0 to k1 - 1 of a run that starts at the 16.16 position p0, each
 * through cubic_taps(). al is the left sum of the run's first frame, gl and
 * gr the Q8 gains of the run's first frame and il, ir their change per
 * frame; sides is the bit set of the sides that sound. */
static void cubic_edge(const struct audio_voice *v, const uint8_t *bank,
                       uint64_t p0, uint32_t k0, uint32_t k1, int sides,
                       int32_t *al, int32_t gl, int32_t il, int32_t gr,
                       int32_t ir)
{
	const uint64_t step = v->step;
	uint32_t k;

	for (k = k0; k < k1; k++) {
		const uint64_t p = v->backward ? p0 - k * step : p0 + k * step;
		const int32_t t = (int32_t)(p & 0xFFFFU);
		int32_t l[4], r[4];
		uint32_t idx[4];
		int j;

		cubic_taps(v, (uint32_t)(p >> 16), idx);
		for (j = 0; j < 4; j++) {
			fetch(v->wide, v->stereo, 3,
			      bank + v->base + ((size_t)idx[j] << v->shift), &l[j],
			      &r[j]);
			if (!v->wide) {
				l[j] *= 256;
				r[j] *= 256;
			}
		}
		if (sides & 1)
			al[k] += cubic_gain(cubic_interp(l[0], l[1], l[2], l[3], t),
			                    (gl + (int32_t)k * il) >> 8);
		if (sides & 2)
			al[AUDIO_VOICE_MIX_FRAMES + k] +=
				cubic_gain(cubic_interp(r[0], r[1], r[2], r[3], t),
				           (gr + (int32_t)k * ir) >> 8);
	}
}

/* n interior frames, one at a time: p is the frame at the voice position, x
 * the fraction as an int32 that moves by dx per frame, nb the byte distance
 * to the next frame in play order. */
static void cubic_interior(const int wide, const int stereo,
                           const uint8_t *p, int32_t x, const int32_t dx,
                           const int32_t nb, uint32_t n, const int sides,
                           int32_t *al, int32_t gl, const int32_t il,
                           int32_t gr, const int32_t ir)
{
	const int fs = wide + stereo;
	uint32_t i;

	for (i = 0; i < n; i++) {
		const uint8_t *q = p + (x >> 16) * (1 << fs);
		const int32_t t = x & 0xFFFF;
		int32_t l[4], r[4];
		int j;

		for (j = 0; j < 4; j++) {
			fetch(wide, stereo, 3, q + (j - 1) * nb, &l[j], &r[j]);
			if (!wide) {
				l[j] *= 256;
				r[j] *= 256;
			}
		}
		if (sides & 1)
			al[i] += cubic_gain(cubic_interp(l[0], l[1], l[2], l[3], t),
			                    (gl + (int32_t)i * il) >> 8);
		if (sides & 2)
			al[AUDIO_VOICE_MIX_FRAMES + i] +=
				cubic_gain(cubic_interp(r[0], r[1], r[2], r[3], t),
				           (gr + (int32_t)i * ir) >> 8);
		x += dx;
	}
}

#ifdef AV_NEON_READ
/*
 * cubic_interp() on four frames, one side, from the four taps as S16 (8-bit
 * data promoted by 8 bits first): the differences are widening operations on
 * the S16 taps, VQRDMULH by t << 15 is (u * t + 2^15) >> 16 and by t << 14 is
 * (u * t + 2^16) >> 17, each with a 64-bit product. kk is { 3, 2, 0, 0 }.
 * cubic4w2() is two sides at once with their steps interleaved: the chain of
 * three dependent multiplies is a long wait on the A9.
 */
AV_INLINE int32x4_t cubic4w(const int16x4_t kk, const int16x4_t a,
                            const int16x4_t b, const int16x4_t c,
                            const int16x4_t d, const int32x4_t t15,
                            const int32x4_t t14)
{
	int32x4_t d1 = vsubl_s16(c, a);
	int32x4_t d3 = vsubl_s16(d, a);
	int32x4_t d2 = vaddl_s16(a, c);
	int32x4_t u;

	d3 = vmlal_lane_s16(d3, b, kk, 0);
	d2 = vmlsl_lane_s16(d2, b, kk, 1);
	d3 = vmlsl_lane_s16(d3, c, kk, 0);
	d2 = vsubq_s32(d2, d3);
	u = vqrdmulhq_s32(d3, t15);
	u = vqrdmulhq_s32(vaddq_s32(d2, u), t15);
	u = vqrdmulhq_s32(vaddq_s32(d1, u), t14);
	return vaddw_s16(u, b);
}

AV_INLINE void cubic4w2(const int16x4_t kk, const int16x4_t al,
                        const int16x4_t bl, const int16x4_t cl,
                        const int16x4_t dl, const int16x4_t ar,
                        const int16x4_t br, const int16x4_t cr,
                        const int16x4_t dr, const int32x4_t t15,
                        const int32x4_t t14, int32x4_t *ol, int32x4_t *orr)
{
	int32x4_t d1l = vsubl_s16(cl, al), d1r = vsubl_s16(cr, ar);
	int32x4_t d3l = vsubl_s16(dl, al), d3r = vsubl_s16(dr, ar);
	int32x4_t d2l = vaddl_s16(al, cl), d2r = vaddl_s16(ar, cr);
	int32x4_t ul, ur;

	d3l = vmlal_lane_s16(d3l, bl, kk, 0);
	d3r = vmlal_lane_s16(d3r, br, kk, 0);
	d2l = vmlsl_lane_s16(d2l, bl, kk, 1);
	d2r = vmlsl_lane_s16(d2r, br, kk, 1);
	d3l = vmlsl_lane_s16(d3l, cl, kk, 0);
	d3r = vmlsl_lane_s16(d3r, cr, kk, 0);
	d2l = vsubq_s32(d2l, d3l);
	d2r = vsubq_s32(d2r, d3r);
	ul = vqrdmulhq_s32(d3l, t15);
	ur = vqrdmulhq_s32(d3r, t15);
	ul = vqrdmulhq_s32(vaddq_s32(d2l, ul), t15);
	ur = vqrdmulhq_s32(vaddq_s32(d2r, ur), t15);
	ul = vqrdmulhq_s32(vaddq_s32(d1l, ul), t14);
	ur = vqrdmulhq_s32(vaddq_s32(d1r, ur), t14);
	*ol = vaddw_s16(ul, bl);
	*orr = vaddw_s16(ur, br);
}

/* Four rows of four S16 (one row a frame, one column a tap) transposed:
 * mem[j] holds the j-th lowest-address tap of each of the four frames. */
AV_INLINE void transpose4h(const int16x4_t f0, const int16x4_t f1,
                           const int16x4_t f2, const int16x4_t f3,
                           int16x4_t mem[4])
{
	const int16x4x2_t xa = vtrn_s16(f0, f1);
	const int16x4x2_t xb = vtrn_s16(f2, f3);
	const int32x2x2_t z0 = vtrn_s32(vreinterpret_s32_s16(xa.val[0]),
	                                vreinterpret_s32_s16(xb.val[0]));
	const int32x2x2_t z1 = vtrn_s32(vreinterpret_s32_s16(xa.val[1]),
	                                vreinterpret_s32_s16(xb.val[1]));

	mem[0] = vreinterpret_s16_s32(z0.val[0]);
	mem[1] = vreinterpret_s16_s32(z1.val[0]);
	mem[2] = vreinterpret_s16_s32(z0.val[1]);
	mem[3] = vreinterpret_s16_s32(z1.val[1]);
}

/*
 * The taps a, b, c and d (tl[0] to tl[3], left; tr[], right) of four
 * interior frames, as S16: the frames at x0 to x3 (16.16, relative to p).
 * The four taps of a frame are neighbours in memory (going back one frame
 * forward and two backward), so one load of the four of them gets everything
 * of a frame; the loads of the four frames are transposed instead of
 * gathered a lane at a time. Backward, the lowest address is the last tap.
 */
AV_INLINE void cubic_load4(const int wide, const int stereo, const int back,
                           const uint8_t *p, const int32_t x0,
                           const int32_t x1, const int32_t x2,
                           const int32_t x3, int16x4_t tl[4],
                           int16x4_t tr[4])
{
	const int fs = wide + stereo;
	const int32_t lo = (back ? -2 : -1) * (1 << fs);
	const uint8_t *const q0 = p + (x0 >> 16) * (1 << fs) + lo;
	const uint8_t *const q1 = p + (x1 >> 16) * (1 << fs) + lo;
	const uint8_t *const q2 = p + (x2 >> 16) * (1 << fs) + lo;
	const uint8_t *const q3 = p + (x3 >> 16) * (1 << fs) + lo;
	int16x4_t ml[4], mr[4];

	if (wide && stereo) {
		const int16x8x2_t pa = vtrnq_s16(
			vreinterpretq_s16_u8(vld1q_u8(q0)),
			vreinterpretq_s16_u8(vld1q_u8(q1)));
		const int16x8x2_t pb = vtrnq_s16(
			vreinterpretq_s16_u8(vld1q_u8(q2)),
			vreinterpretq_s16_u8(vld1q_u8(q3)));
		const int32x4x2_t zl = vtrnq_s32(vreinterpretq_s32_s16(pa.val[0]),
		                                 vreinterpretq_s32_s16(pb.val[0]));
		const int32x4x2_t zr = vtrnq_s32(vreinterpretq_s32_s16(pa.val[1]),
		                                 vreinterpretq_s32_s16(pb.val[1]));
		const int16x8_t l02 = vreinterpretq_s16_u8(
			vrev16q_u8(vreinterpretq_u8_s32(zl.val[0])));
		const int16x8_t l13 = vreinterpretq_s16_u8(
			vrev16q_u8(vreinterpretq_u8_s32(zl.val[1])));
		const int16x8_t r02 = vreinterpretq_s16_u8(
			vrev16q_u8(vreinterpretq_u8_s32(zr.val[0])));
		const int16x8_t r13 = vreinterpretq_s16_u8(
			vrev16q_u8(vreinterpretq_u8_s32(zr.val[1])));

		ml[0] = vget_low_s16(l02);
		ml[1] = vget_low_s16(l13);
		ml[2] = vget_high_s16(l02);
		ml[3] = vget_high_s16(l13);
		mr[0] = vget_low_s16(r02);
		mr[1] = vget_low_s16(r13);
		mr[2] = vget_high_s16(r02);
		mr[3] = vget_high_s16(r13);
	} else if (wide) {
		/* mono S16: 8 bytes a frame */
		int16x4_t m[4];

		transpose4h(vreinterpret_s16_u8(vld1_u8(q0)),
		            vreinterpret_s16_u8(vld1_u8(q1)),
		            vreinterpret_s16_u8(vld1_u8(q2)),
		            vreinterpret_s16_u8(vld1_u8(q3)), m);
		ml[0] = vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(m[0])));
		ml[1] = vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(m[1])));
		ml[2] = vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(m[2])));
		ml[3] = vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(m[3])));
		mr[0] = ml[0];
		mr[1] = ml[1];
		mr[2] = ml[2];
		mr[3] = ml[3];
	} else if (stereo) {
		/* S8S: 8 bytes a frame, each tap a (left, right) byte pair */
		int16x4_t m[4];

		transpose4h(vreinterpret_s16_u8(vld1_u8(q0)),
		            vreinterpret_s16_u8(vld1_u8(q1)),
		            vreinterpret_s16_u8(vld1_u8(q2)),
		            vreinterpret_s16_u8(vld1_u8(q3)), m);
		ml[0] = vshl_n_s16(m[0], 8);
		ml[1] = vshl_n_s16(m[1], 8);
		ml[2] = vshl_n_s16(m[2], 8);
		ml[3] = vshl_n_s16(m[3], 8);
		mr[0] = vand_s16(m[0], vdup_n_s16(-256));
		mr[1] = vand_s16(m[1], vdup_n_s16(-256));
		mr[2] = vand_s16(m[2], vdup_n_s16(-256));
		mr[3] = vand_s16(m[3], vdup_n_s16(-256));
	} else {
		/* mono S8: 4 bytes a frame */
		uint32x2_t g01 = vdup_n_u32(0), g23 = vdup_n_u32(0);
		uint8x8x2_t ev, ab, cd;

		g01 = vld1_lane_u32((const uint32_t *)(const void *)q0, g01, 0);
		g01 = vld1_lane_u32((const uint32_t *)(const void *)q1, g01, 1);
		g23 = vld1_lane_u32((const uint32_t *)(const void *)q2, g23, 0);
		g23 = vld1_lane_u32((const uint32_t *)(const void *)q3, g23, 1);
		/* even bytes: taps 0 and 2 of the frames, odd bytes: 1 and 3 */
		ev = vuzp_u8(vreinterpret_u8_u32(g01), vreinterpret_u8_u32(g23));
		ab = vuzp_u8(ev.val[0], ev.val[0]);
		cd = vuzp_u8(ev.val[1], ev.val[1]);
		ml[0] = vget_low_s16(vshll_n_s8(vreinterpret_s8_u8(ab.val[0]), 8));
		ml[2] = vget_low_s16(vshll_n_s8(vreinterpret_s8_u8(ab.val[1]), 8));
		ml[1] = vget_low_s16(vshll_n_s8(vreinterpret_s8_u8(cd.val[0]), 8));
		ml[3] = vget_low_s16(vshll_n_s8(vreinterpret_s8_u8(cd.val[1]), 8));
		mr[0] = ml[0];
		mr[1] = ml[1];
		mr[2] = ml[2];
		mr[3] = ml[3];
	}
	tl[0] = ml[back ? 3 : 0];
	tl[1] = ml[back ? 2 : 1];
	tl[2] = ml[back ? 1 : 2];
	tl[3] = ml[back ? 0 : 3];
	tr[0] = mr[back ? 3 : 0];
	tr[1] = mr[back ? 2 : 1];
	tr[2] = mr[back ? 1 : 2];
	tr[3] = mr[back ? 0 : 3];
}

/*
 * Four interior frames: the taps of each frame (cubic_load4), the curve of
 * the left and right side, and the result added to the sums at al and ar.
 * vvl and vvr are the volumes of the four frames shifted up by 15: VQDMULH
 * doubles the product and keeps the high half, so (sample * (volume << 15))
 * * 2 >> 32 is (sample * volume) >> 16, exact, with no overflow for a
 * sample of at most AV_CUBIC_PEAK (volume << 15 stays below 2^31). xv holds
 * the fractions of the four frames in its low 16 bits: t << 15 and t << 14
 * come from shifting the fraction to the top of the lane and back.
 */
AV_INLINE void cubic_block(const int wide, const int stereo, const int sides,
                           const int back, const uint8_t *restrict p,
                           const int32_t x0, const int32_t x1,
                           const int32_t x2, const int32_t x3,
                           const int32x4_t xv, const int16x4_t kk,
                           const int32x4_t vvl, const int32x4_t vvr,
                           int32_t *restrict al, int32_t *restrict ar)
{
	const uint32x4_t top = vshlq_n_u32(vreinterpretq_u32_s32(xv), 16);
	const int32x4_t t15 = vreinterpretq_s32_u32(vshrq_n_u32(top, 1));
	const int32x4_t t14 = vreinterpretq_s32_u32(vshrq_n_u32(top, 2));
	int16x4_t tl[4], tr[4];

	cubic_load4(wide, stereo, back, p, x0, x1, x2, x3, tl, tr);
	if (!stereo) {
		const int32x4_t o = cubic4w(kk, tl[0], tl[1], tl[2], tl[3], t15, t14);

		vst1q_s32(al, vaddq_s32(vld1q_s32(al), vqdmulhq_s32(o, vvl)));
		vst1q_s32(ar, vaddq_s32(vld1q_s32(ar), vqdmulhq_s32(o, vvr)));
	} else if (sides == 3) {
		int32x4_t ol, o2;

		cubic4w2(kk, tl[0], tl[1], tl[2], tl[3], tr[0], tr[1], tr[2], tr[3],
		         t15, t14, &ol, &o2);
		vst1q_s32(al, vaddq_s32(vld1q_s32(al), vqdmulhq_s32(ol, vvl)));
		vst1q_s32(ar, vaddq_s32(vld1q_s32(ar), vqdmulhq_s32(o2, vvr)));
	} else if (sides & 1) {
		const int32x4_t o = cubic4w(kk, tl[0], tl[1], tl[2], tl[3], t15, t14);

		vst1q_s32(al, vaddq_s32(vld1q_s32(al), vqdmulhq_s32(o, vvl)));
	} else {
		const int32x4_t o = cubic4w(kk, tr[0], tr[1], tr[2], tr[3], t15, t14);

		vst1q_s32(ar, vaddq_s32(vld1q_s32(ar), vqdmulhq_s32(o, vvr)));
	}
}

/*
 * All n interior frames, four at a time, added to the sums at al (left) and
 * al + AUDIO_VOICE_MIX_FRAMES (right) with the gain of each frame, (gain + i
 * * inc) >> 8 as in apply_ramp(), the volume vector rebuilt per block only
 * when the gain moves. sides names the sounding sides of a stereo sample (a
 * mono sample takes 3). A last block of fewer than four frames reads the
 * taps of its last frame again for the lanes it does not use (so it reads
 * nothing outside the interior) and adds through a copy of the sums. Returns
 * n.
 */
AV_INLINE uint32_t cubic_kernel_neon(const int wide, const int stereo,
                                     const int sides, const int back,
                                     const uint8_t *restrict p, int32_t x,
                                     const int32_t dx, uint32_t n,
                                     int32_t *restrict al, int32_t gl,
                                     int32_t il, int32_t gr, int32_t ir)
{
	static const int32_t lanes[4] = { 0, 1, 2, 3 };
	static const int16_t ks[4] = { 3, 2, 0, 0 };
	const int32x4_t lane = vld1q_s32(lanes);
	const int16x4_t kk = vld1_s16(ks);
	int32_t *const ar = al + AUDIO_VOICE_MIX_FRAMES;
	const int ramp = (il | ir) != 0;
	const int32x4_t dx4 = vdupq_n_s32(4 * dx);
	const int32x4_t dgl = vdupq_n_s32(4 * il);
	const int32x4_t dgr = vdupq_n_s32(4 * ir);
	int32x4_t xv = vmlaq_n_s32(vdupq_n_s32(x), lane, dx);
	int32x4_t gvl = vmlaq_n_s32(vdupq_n_s32(gl), lane, il);
	int32x4_t gvr = vmlaq_n_s32(vdupq_n_s32(gr), lane, ir);
	int32x4_t vvl = vdupq_n_s32((gl >> 8) << 15);
	int32x4_t vvr = vdupq_n_s32((gr >> 8) << 15);
	int32_t tmpl[4] = { 0, 0, 0, 0 }, tmpr[4] = { 0, 0, 0, 0 };
	uint32_t i;

	for (i = 0; i < n; i += 4U) {
		const uint32_t rem = n - i;
		int32_t x1 = x + dx, x2 = x + 2 * dx, x3 = x + 3 * dx;
		int32_t *bl = al + i, *br = ar + i;
		uint32_t k;

		if (ramp) {
			vvl = vshlq_n_s32(vshrq_n_s32(gvl, 8), 15);
			vvr = vshlq_n_s32(vshrq_n_s32(gvr, 8), 15);
			gvl = vaddq_s32(gvl, dgl);
			gvr = vaddq_s32(gvr, dgr);
		}
		if (rem < 4U) {
			x1 = rem > 1U ? x1 : x;
			x2 = rem > 2U ? x2 : x1;
			x3 = x2;
			for (k = 0; k < rem; k++) {
				tmpl[k] = bl[k];
				tmpr[k] = br[k];
			}
			bl = tmpl;
			br = tmpr;
		}
		cubic_block(wide, stereo, sides, back, p, x, x1, x2, x3, xv, kk,
		            vvl, vvr, bl, br);
		if (rem < 4U) {
			for (k = 0; k < rem; k++) {
				al[i + k] = tmpl[k];
				ar[i + k] = tmpr[k];
			}
		}
		x += 4 * dx;
		xv = vaddq_s32(xv, dx4);
	}
	return n;
}

AV_NOINLINE uint32_t run_cubic(const struct audio_voice *v, int sides,
                               const uint8_t *p, int32_t x, int32_t dx,
                               int32_t nb, uint32_t n, int32_t *al,
                               int32_t gl, int32_t il, int32_t gr, int32_t ir)
{
#define AV_CK(w, s, sd) \
	return nb < 0 ? cubic_kernel_neon(w, s, sd, 1, p, x, dx, n, al, gl, il, \
	                                  gr, ir) \
	              : cubic_kernel_neon(w, s, sd, 0, p, x, dx, n, al, gl, il, \
	                                  gr, ir)
	if (!v->stereo) {
		if (v->wide)
			AV_CK(1, 0, 3);
		AV_CK(0, 0, 3);
	}
	if (v->wide) {
		switch (sides) {
		case 1: AV_CK(1, 1, 1);
		case 2: AV_CK(1, 1, 2);
		default: AV_CK(1, 1, 3);
		}
	}
	switch (sides) {
	case 1: AV_CK(0, 1, 1);
	case 2: AV_CK(0, 1, 2);
	default: AV_CK(0, 1, 3);
	}
#undef AV_CK
}
#else
static uint32_t run_cubic(const struct audio_voice *v, int sides,
                          const uint8_t *p, int32_t x, int32_t dx, int32_t nb,
                          uint32_t n, int32_t *al, int32_t gl, int32_t il,
                          int32_t gr, int32_t ir)
{
	(void)v; (void)sides; (void)p; (void)x; (void)dx; (void)nb; (void)n;
	(void)al; (void)gl; (void)il; (void)gr; (void)ir;
	return 0;
}
#endif

/* Mix a run of run frames of a cubic voice at the 16.16 position p0 into
 * the sums from frame `at` of the chunk. The voice is not moved. */
static void cubic_run(struct audio_voice_engine *e,
                      const struct audio_voice *v, uint64_t p0, uint32_t run,
                      uint32_t at)
{
	const uint8_t *const bank = e->bank;
	const int back = v->backward;
	const uint32_t step = v->step;
	int32_t *const al = e->mix[0] + at;
	int32_t gl, gr, il = 0, ir = 0;
	uint64_t ka, kb;
	int sides;

	if (v->ramp_left != 0U) {
		int32_t el, er;

		/* The gains at the end of the run, to see whether it is silent. */
		if (run == v->ramp_left) {
			el = v->stopping ? 0 : (int32_t)(v->vol_l << 8);
			er = v->stopping ? 0 : (int32_t)(v->vol_r << 8);
		} else {
			el = v->gain_l + v->inc_l * (int32_t)run;
			er = v->gain_r + v->inc_r * (int32_t)run;
		}
		if ((v->gain_l | v->gain_r | el | er) == 0)
			return;
		gl = v->gain_l;
		gr = v->gain_r;
		il = v->inc_l;
		ir = v->inc_r;
		sides = 3;
	} else {
		sides = (v->vol_l != 0U ? 1 : 0) | (v->vol_r != 0U ? 2 : 0);
		if (sides == 0)
			return;
		gl = (int32_t)(v->vol_l << 8);
		gr = (int32_t)(v->vol_r << 8);
	}
	e->chunk_cubic = 1;

	/* ka: first frame of the interior, kb: first frame after it. */
	if (!back) {
		/* the frame before must be the previous one in memory, and both
		 * following ones must lie inside the pass */
		const uint64_t lo = (uint64_t)((v->looped && v->looplen != 0U) ?
		                               v->loopstart + 1U : 1U) << 16;
		const uint64_t hi = (uint64_t)(v->end >= 3U ? v->end - 2U : 0U) << 16;

		ka = p0 >= lo ? 0U : div_up(lo - p0, step);
		kb = p0 >= hi ? 0U : div_up(hi - p0, step);
	} else {
		const uint64_t top = (uint64_t)(v->end >= 2U ? v->end - 1U : 0U) << 16;
		const uint64_t low = (uint64_t)(v->loopstart + 2U) << 16;

		ka = p0 < top ? 0U : div_down(p0 - top, step) + 1U;
		kb = p0 < low ? 0U : div_down(p0 - low, step) + 1U;
		if (v->end < 2U)
			ka = run;
	}
	if (ka > run)
		ka = run;
	if (kb > run)
		kb = run;
	if (kb < ka)
		kb = ka;
	if (ka != 0U)
		cubic_edge(v, bank, p0, 0, (uint32_t)ka, sides, al, gl, il, gr, ir);
	if (kb > ka) {
		const uint32_t k = (uint32_t)ka;
		const uint32_t cnt = (uint32_t)kb - k;
		const int32_t dx = back ? -(int32_t)step : (int32_t)step;
		const int32_t fb = (int32_t)1 << v->shift;
		const int32_t nb = back ? -fb : fb;
		const uint8_t *p = bank + v->base + ((size_t)v->pos << v->shift);
		const int32_t x = (int32_t)v->frac + (int32_t)k * dx;
		const int32_t g_l = gl + (int32_t)k * il;
		const int32_t g_r = gr + (int32_t)k * ir;
		const uint32_t d = run_cubic(v, sides, p, x, dx, nb, cnt, al + k, g_l,
		                             il, g_r, ir);

		if (d < cnt)
			cubic_interior(v->wide, v->stereo, p, x + (int32_t)d * dx, dx, nb,
			               cnt - d, sides, al + k + d, g_l + (int32_t)d * il,
			               il, g_r + (int32_t)d * ir, ir);
	}
	if (kb < run)
		cubic_edge(v, bank, p0, (uint32_t)kb, run, sides, al, gl, il, gr, ir);
}

/* mix_voice() for a cubic voice: the same run planning (the pass end, the
 * ramp end), the frames of a run mixed by cubic_run(). */
AV_NOINLINE void mix_voice_cubic(struct audio_voice_engine *e,
                                 struct audio_voice *v, uint32_t start,
                                 uint32_t n)
{
	const uint32_t step = v->step;
	uint32_t done = 0;

	while (done < n && v->active) {
		const uint32_t left = n - done;
		const uint64_t pos = ((uint64_t)v->pos << 16) | v->frac;
		const int ramping = v->ramp_left != 0U;
		uint32_t run;
		int hit, below;

		if (!v->backward) {
			const uint64_t end = (uint64_t)v->end << 16;

			if (pos >= end) {
				/* Not reachable: an active voice is inside its pass. */
				run = 1;
				hit = 1;
			} else if (pos + (uint64_t)left * step < end) {
				run = left;
				hit = 0;
			} else {
				run = (uint32_t)div_up(end - pos, step);
				hit = 1;
			}
		} else {
			const uint64_t low = (uint64_t)v->loopstart << 16;

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
		}
		/* A ramp ends the run too: the gain is constant after it. */
		if (ramping && v->ramp_left < run) {
			run = v->ramp_left;
			hit = 0;
		}
		cubic_run(e, v, pos, run, start + done);
		below = step_state(v, run);
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
 * a master up to `narrow` the product of a sum fits int32. Without cubic
 * voices a sum is at most 2^21 in magnitude (32 voices and 32 fade slots of
 * at most 32768) and narrow is AV_MASTER_NARROW; a chunk that a cubic voice
 * has added to can hold sides up to AV_CUBIC_PEAK and takes the smaller
 * AV_MASTER_NARROW_CUBIC.
 */
#define AV_MASTER_NARROW 1023U
#define AV_MASTER_NARROW_CUBIC 511U
typedef char av_master_narrow_fits[
	((int64_t)(AUDIO_VOICE_MAX_VOICES + AUDIO_VOICE_FADE_SLOTS) * 32768 *
	 AV_MASTER_NARROW < 2147483648LL) ? 1 : -1];
typedef char av_master_narrow_cubic_fits[
	((int64_t)(AUDIO_VOICE_MAX_VOICES + AUDIO_VOICE_FADE_SLOTS) *
	 AV_CUBIC_PEAK * AV_MASTER_NARROW_CUBIC < 2147483648LL) ? 1 : -1];
typedef char av_cubic_sum_after_master_fits[
	((((int64_t)(AUDIO_VOICE_MAX_VOICES + AUDIO_VOICE_FADE_SLOTS) *
	   AV_CUBIC_PEAK * AUDIO_VOICE_MASTER_MAX) >> 8) < 2147483648LL) ? 1 : -1];

static int16_t clamp16(int64_t x)
{
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static void finish_c(const int32_t *l, const int32_t *r, int16_t *out,
                     uint32_t n, uint32_t master, uint32_t narrow)
{
	uint32_t i;

	if (master == AUDIO_VOICE_MASTER_UNITY) {
		for (i = 0; i < n; i++) {
			out[2U * i] = clamp16(l[i]);
			out[2U * i + 1U] = clamp16(r[i]);
		}
	} else if (master <= narrow) {
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

/* BEGIN GENERATED VOICE LIMITER TABLE */
/* Generated by util/gen_voice_limiter_table.py; do not edit. */
static const uint16_t voice_limiter_table[4097] = {
	24576, 24608, 24640, 24671, 24702, 24733, 24764, 24794, 24825, 24855, 24884, 24914,
	24943, 24972, 25001, 25030, 25058, 25086, 25115, 25142, 25170, 25197, 25225, 25252,
	25278, 25305, 25332, 25358, 25384, 25410, 25436, 25461, 25486, 25512, 25537, 25562,
	25586, 25611, 25635, 25659, 25683, 25707, 25731, 25754, 25778, 25801, 25824, 25847,
	25870, 25892, 25915, 25937, 25959, 25981, 26003, 26025, 26046, 26068, 26089, 26110,
	26132, 26152, 26173, 26194, 26214, 26235, 26255, 26275, 26295, 26315, 26335, 26355,
	26374, 26394, 26413, 26432, 26451, 26470, 26489, 26508, 26526, 26545, 26563, 26582,
	26600, 26618, 26636, 26654, 26672, 26689, 26707, 26724, 26742, 26759, 26776, 26793,
	26810, 26827, 26844, 26860, 26877, 26894, 26910, 26926, 26942, 26959, 26975, 26991,
	27006, 27022, 27038, 27054, 27069, 27085, 27100, 27115, 27130, 27145, 27161, 27175,
	27190, 27205, 27220, 27234, 27249, 27264, 27278, 27292, 27306, 27321, 27335, 27349,
	27363, 27377, 27390, 27404, 27418, 27432, 27445, 27459, 27472, 27485, 27499, 27512,
	27525, 27538, 27551, 27564, 27577, 27590, 27602, 27615, 27628, 27640, 27653, 27665,
	27678, 27690, 27702, 27714, 27726, 27739, 27751, 27763, 27774, 27786, 27798, 27810,
	27822, 27833, 27845, 27856, 27868, 27879, 27891, 27902, 27913, 27924, 27936, 27947,
	27958, 27969, 27980, 27991, 28001, 28012, 28023, 28034, 28044, 28055, 28066, 28076,
	28086, 28097, 28107, 28118, 28128, 28138, 28148, 28158, 28169, 28179, 28189, 28199,
	28209, 28218, 28228, 28238, 28248, 28258, 28267, 28277, 28286, 28296, 28306, 28315,
	28324, 28334, 28343, 28353, 28362, 28371, 28380, 28389, 28398, 28408, 28417, 28426,
	28435, 28444, 28452, 28461, 28470, 28479, 28488, 28496, 28505, 28514, 28522, 28531,
	28539, 28548, 28556, 28565, 28573, 28582, 28590, 28598, 28606, 28615, 28623, 28631,
	28639, 28647, 28655, 28663, 28671, 28687, 28703, 28719, 28734, 28750, 28765, 28780,
	28796, 28811, 28825, 28840, 28855, 28869, 28884, 28898, 28912, 28926, 28941, 28954,
	28968, 28982, 28996, 29009, 29022, 29036, 29049, 29062, 29075, 29088, 29101, 29114,
	29126, 29139, 29152, 29164, 29176, 29189, 29201, 29213, 29225, 29237, 29249, 29260,
	29272, 29284, 29295, 29307, 29318, 29329, 29341, 29352, 29363, 29374, 29385, 29396,
	29406, 29417, 29428, 29438, 29449, 29459, 29470, 29480, 29490, 29501, 29511, 29521,
	29531, 29541, 29551, 29561, 29570, 29580, 29590, 29599, 29609, 29618, 29628, 29637,
	29646, 29656, 29665, 29674, 29683, 29692, 29701, 29710, 29719, 29728, 29737, 29745,
	29754, 29763, 29771, 29780, 29788, 29797, 29805, 29813, 29822, 29830, 29838, 29846,
	29854, 29863, 29871, 29879, 29886, 29894, 29902, 29910, 29918, 29926, 29933, 29941,
	29948, 29956, 29964, 29971, 29978, 29986, 29993, 30000, 30008, 30015, 30022, 30029,
	30036, 30044, 30051, 30058, 30065, 30072, 30078, 30085, 30092, 30099, 30106, 30113,
	30119, 30126, 30133, 30139, 30146, 30152, 30159, 30165, 30172, 30178, 30184, 30191,
	30197, 30203, 30210, 30216, 30222, 30228, 30234, 30240, 30246, 30253, 30259, 30265,
	30270, 30276, 30282, 30288, 30294, 30300, 30306, 30311, 30317, 30323, 30329, 30334,
	30340, 30345, 30351, 30357, 30362, 30368, 30373, 30379, 30384, 30389, 30395, 30400,
	30405, 30411, 30416, 30421, 30426, 30432, 30437, 30442, 30447, 30452, 30457, 30462,
	30468, 30473, 30478, 30483, 30488, 30492, 30497, 30502, 30507, 30512, 30517, 30522,
	30526, 30531, 30536, 30541, 30545, 30550, 30555, 30560, 30564, 30569, 30573, 30578,
	30582, 30587, 30592, 30596, 30601, 30605, 30609, 30614, 30618, 30623, 30627, 30631,
	30636, 30640, 30644, 30649, 30653, 30657, 30661, 30666, 30670, 30674, 30678, 30682,
	30686, 30691, 30695, 30699, 30703, 30707, 30711, 30715, 30719, 30727, 30735, 30743,
	30750, 30758, 30766, 30773, 30781, 30789, 30796, 30803, 30811, 30818, 30825, 30832,
	30839, 30846, 30854, 30860, 30867, 30874, 30881, 30888, 30894, 30901, 30908, 30914,
	30921, 30927, 30934, 30940, 30946, 30953, 30959, 30965, 30971, 30978, 30984, 30990,
	30996, 31002, 31008, 31013, 31019, 31025, 31031, 31037, 31042, 31048, 31054, 31059,
	31065, 31070, 31076, 31081, 31086, 31092, 31097, 31102, 31108, 31113, 31118, 31123,
	31128, 31134, 31139, 31144, 31149, 31154, 31159, 31164, 31168, 31173, 31178, 31183,
	31188, 31192, 31197, 31202, 31206, 31211, 31216, 31220, 31225, 31229, 31234, 31238,
	31243, 31247, 31252, 31256, 31260, 31265, 31269, 31273, 31277, 31282, 31286, 31290,
	31294, 31298, 31302, 31306, 31310, 31315, 31319, 31323, 31326, 31330, 31334, 31338,
	31342, 31346, 31350, 31354, 31357, 31361, 31365, 31369, 31372, 31376, 31380, 31383,
	31387, 31391, 31394, 31398, 31401, 31405, 31409, 31412, 31416, 31419, 31422, 31426,
	31429, 31433, 31436, 31440, 31443, 31446, 31450, 31453, 31456, 31459, 31463, 31466,
	31469, 31472, 31475, 31479, 31482, 31485, 31488, 31491, 31494, 31497, 31500, 31503,
	31506, 31510, 31513, 31516, 31518, 31521, 31524, 31527, 31530, 31533, 31536, 31539,
	31542, 31545, 31548, 31550, 31553, 31556, 31559, 31562, 31564, 31567, 31570, 31573,
	31575, 31578, 31581, 31583, 31586, 31589, 31591, 31594, 31596, 31599, 31602, 31604,
	31607, 31609, 31612, 31614, 31617, 31620, 31622, 31625, 31627, 31629, 31632, 31634,
	31637, 31639, 31642, 31644, 31646, 31649, 31651, 31654, 31656, 31658, 31661, 31663,
	31665, 31668, 31670, 31672, 31674, 31677, 31679, 31681, 31684, 31686, 31688, 31690,
	31692, 31695, 31697, 31699, 31701, 31703, 31705, 31708, 31710, 31712, 31714, 31716,
	31718, 31720, 31722, 31724, 31726, 31729, 31731, 31733, 31735, 31737, 31739, 31741,
	31743, 31747, 31751, 31755, 31758, 31762, 31766, 31770, 31774, 31778, 31781, 31785,
	31789, 31792, 31796, 31799, 31803, 31806, 31810, 31813, 31817, 31820, 31824, 31827,
	31830, 31834, 31837, 31840, 31844, 31847, 31850, 31853, 31856, 31860, 31863, 31866,
	31869, 31872, 31875, 31878, 31881, 31884, 31887, 31890, 31893, 31896, 31899, 31902,
	31904, 31907, 31910, 31913, 31916, 31918, 31921, 31924, 31926, 31929, 31932, 31934,
	31937, 31940, 31942, 31945, 31947, 31950, 31953, 31955, 31958, 31960, 31963, 31965,
	31967, 31970, 31972, 31975, 31977, 31979, 31982, 31984, 31986, 31989, 31991, 31993,
	31996, 31998, 32000, 32002, 32005, 32007, 32009, 32011, 32013, 32016, 32018, 32020,
	32022, 32024, 32026, 32028, 32030, 32032, 32034, 32036, 32038, 32041, 32043, 32045,
	32046, 32048, 32050, 32052, 32054, 32056, 32058, 32060, 32062, 32064, 32066, 32068,
	32069, 32071, 32073, 32075, 32077, 32079, 32080, 32082, 32084, 32086, 32088, 32089,
	32091, 32093, 32094, 32096, 32098, 32100, 32101, 32103, 32105, 32106, 32108, 32110,
	32111, 32113, 32115, 32116, 32118, 32119, 32121, 32123, 32124, 32126, 32127, 32129,
	32130, 32132, 32133, 32135, 32136, 32138, 32140, 32141, 32142, 32144, 32145, 32147,
	32148, 32150, 32151, 32153, 32154, 32156, 32157, 32158, 32160, 32161, 32163, 32164,
	32165, 32167, 32168, 32170, 32171, 32172, 32174, 32175, 32176, 32178, 32179, 32180,
	32181, 32183, 32184, 32185, 32187, 32188, 32189, 32190, 32192, 32193, 32194, 32196,
	32197, 32198, 32199, 32200, 32202, 32203, 32204, 32205, 32206, 32208, 32209, 32210,
	32211, 32212, 32214, 32215, 32216, 32217, 32218, 32219, 32220, 32222, 32223, 32224,
	32225, 32226, 32227, 32228, 32229, 32231, 32232, 32233, 32234, 32235, 32236, 32237,
	32238, 32239, 32240, 32241, 32242, 32243, 32244, 32245, 32246, 32248, 32249, 32250,
	32251, 32252, 32253, 32254, 32255, 32257, 32259, 32261, 32262, 32264, 32266, 32268,
	32270, 32272, 32274, 32276, 32278, 32279, 32281, 32283, 32285, 32286, 32288, 32290,
	32292, 32293, 32295, 32297, 32298, 32300, 32302, 32303, 32305, 32307, 32308, 32310,
	32311, 32313, 32315, 32316, 32318, 32319, 32321, 32322, 32324, 32325, 32327, 32328,
	32330, 32331, 32333, 32334, 32335, 32337, 32338, 32340, 32341, 32342, 32344, 32345,
	32346, 32348, 32349, 32350, 32352, 32353, 32354, 32356, 32357, 32358, 32360, 32361,
	32362, 32363, 32365, 32366, 32367, 32368, 32369, 32371, 32372, 32373, 32374, 32375,
	32376, 32378, 32379, 32380, 32381, 32382, 32383, 32384, 32386, 32387, 32388, 32389,
	32390, 32391, 32392, 32393, 32394, 32395, 32396, 32397, 32398, 32399, 32400, 32401,
	32402, 32404, 32405, 32406, 32406, 32407, 32408, 32409, 32410, 32411, 32412, 32413,
	32414, 32415, 32416, 32417, 32418, 32419, 32420, 32421, 32422, 32423, 32423, 32424,
	32425, 32426, 32427, 32428, 32429, 32430, 32430, 32431, 32432, 32433, 32434, 32435,
	32436, 32436, 32437, 32438, 32439, 32440, 32441, 32441, 32442, 32443, 32444, 32445,
	32445, 32446, 32447, 32448, 32448, 32449, 32450, 32451, 32451, 32452, 32453, 32454,
	32454, 32455, 32456, 32457, 32457, 32458, 32459, 32460, 32460, 32461, 32462, 32462,
	32463, 32464, 32465, 32465, 32466, 32467, 32467, 32468, 32469, 32469, 32470, 32471,
	32471, 32472, 32473, 32473, 32474, 32475, 32475, 32476, 32477, 32477, 32478, 32478,
	32479, 32480, 32480, 32481, 32482, 32482, 32483, 32483, 32484, 32485, 32485, 32486,
	32486, 32487, 32488, 32488, 32489, 32489, 32490, 32491, 32491, 32492, 32492, 32493,
	32493, 32494, 32495, 32495, 32496, 32496, 32497, 32497, 32498, 32499, 32499, 32500,
	32500, 32501, 32501, 32502, 32502, 32503, 32503, 32504, 32504, 32505, 32505, 32506,
	32506, 32507, 32508, 32508, 32509, 32509, 32510, 32510, 32511, 32512, 32513, 32514,
	32514, 32515, 32516, 32517, 32518, 32519, 32520, 32521, 32522, 32523, 32524, 32525,
	32526, 32526, 32527, 32528, 32529, 32530, 32531, 32532, 32532, 32533, 32534, 32535,
	32536, 32537, 32537, 32538, 32539, 32540, 32541, 32541, 32542, 32543, 32544, 32544,
	32545, 32546, 32547, 32547, 32548, 32549, 32550, 32550, 32551, 32552, 32552, 32553,
	32554, 32554, 32555, 32556, 32556, 32557, 32558, 32558, 32559, 32560, 32560, 32561,
	32562, 32562, 32563, 32564, 32564, 32565, 32566, 32566, 32567, 32567, 32568, 32569,
	32569, 32570, 32570, 32571, 32571, 32572, 32573, 32573, 32574, 32574, 32575, 32575,
	32576, 32577, 32577, 32578, 32578, 32579, 32579, 32580, 32580, 32581, 32581, 32582,
	32582, 32583, 32583, 32584, 32584, 32585, 32586, 32586, 32586, 32587, 32587, 32588,
	32588, 32589, 32589, 32590, 32590, 32591, 32591, 32592, 32592, 32593, 32593, 32594,
	32594, 32595, 32595, 32595, 32596, 32596, 32597, 32597, 32598, 32598, 32598, 32599,
	32599, 32600, 32600, 32601, 32601, 32601, 32602, 32602, 32603, 32603, 32604, 32604,
	32604, 32605, 32605, 32606, 32606, 32606, 32607, 32607, 32607, 32608, 32608, 32609,
	32609, 32609, 32610, 32610, 32610, 32611, 32611, 32612, 32612, 32612, 32613, 32613,
	32613, 32614, 32614, 32614, 32615, 32615, 32616, 32616, 32616, 32617, 32617, 32617,
	32618, 32618, 32618, 32619, 32619, 32619, 32620, 32620, 32620, 32621, 32621, 32621,
	32622, 32622, 32622, 32622, 32623, 32623, 32623, 32624, 32624, 32624, 32625, 32625,
	32625, 32626, 32626, 32626, 32626, 32627, 32627, 32627, 32628, 32628, 32628, 32629,
	32629, 32629, 32629, 32630, 32630, 32630, 32631, 32631, 32631, 32631, 32632, 32632,
	32632, 32633, 32633, 32633, 32633, 32634, 32634, 32634, 32634, 32635, 32635, 32635,
	32635, 32636, 32636, 32636, 32636, 32637, 32637, 32637, 32638, 32638, 32638, 32638,
	32639, 32639, 32640, 32640, 32640, 32641, 32641, 32642, 32642, 32643, 32643, 32644,
	32644, 32645, 32645, 32646, 32646, 32646, 32647, 32647, 32648, 32648, 32649, 32649,
	32649, 32650, 32650, 32651, 32651, 32652, 32652, 32652, 32653, 32653, 32654, 32654,
	32654, 32655, 32655, 32655, 32656, 32656, 32657, 32657, 32657, 32658, 32658, 32658,
	32659, 32659, 32659, 32660, 32660, 32660, 32661, 32661, 32661, 32662, 32662, 32662,
	32663, 32663, 32663, 32664, 32664, 32664, 32665, 32665, 32665, 32666, 32666, 32666,
	32667, 32667, 32667, 32668, 32668, 32668, 32668, 32669, 32669, 32669, 32670, 32670,
	32670, 32670, 32671, 32671, 32671, 32672, 32672, 32672, 32672, 32673, 32673, 32673,
	32673, 32674, 32674, 32674, 32674, 32675, 32675, 32675, 32675, 32676, 32676, 32676,
	32676, 32677, 32677, 32677, 32677, 32678, 32678, 32678, 32678, 32679, 32679, 32679,
	32679, 32680, 32680, 32680, 32680, 32681, 32681, 32681, 32681, 32681, 32682, 32682,
	32682, 32682, 32682, 32683, 32683, 32683, 32683, 32684, 32684, 32684, 32684, 32684,
	32685, 32685, 32685, 32685, 32685, 32686, 32686, 32686, 32686, 32686, 32687, 32687,
	32687, 32687, 32687, 32688, 32688, 32688, 32688, 32688, 32688, 32689, 32689, 32689,
	32689, 32689, 32690, 32690, 32690, 32690, 32690, 32690, 32691, 32691, 32691, 32691,
	32691, 32692, 32692, 32692, 32692, 32692, 32692, 32693, 32693, 32693, 32693, 32693,
	32693, 32694, 32694, 32694, 32694, 32694, 32694, 32694, 32695, 32695, 32695, 32695,
	32695, 32695, 32696, 32696, 32696, 32696, 32696, 32696, 32696, 32697, 32697, 32697,
	32697, 32697, 32697, 32698, 32698, 32698, 32698, 32698, 32698, 32698, 32699, 32699,
	32699, 32699, 32699, 32699, 32699, 32700, 32700, 32700, 32700, 32700, 32700, 32700,
	32700, 32701, 32701, 32701, 32701, 32701, 32701, 32701, 32701, 32702, 32702, 32702,
	32702, 32702, 32702, 32702, 32703, 32703, 32703, 32703, 32703, 32704, 32704, 32704,
	32704, 32705, 32705, 32705, 32705, 32706, 32706, 32706, 32706, 32706, 32707, 32707,
	32707, 32707, 32708, 32708, 32708, 32708, 32708, 32709, 32709, 32709, 32709, 32709,
	32710, 32710, 32710, 32710, 32710, 32711, 32711, 32711, 32711, 32711, 32712, 32712,
	32712, 32712, 32712, 32712, 32713, 32713, 32713, 32713, 32713, 32713, 32714, 32714,
	32714, 32714, 32714, 32714, 32715, 32715, 32715, 32715, 32715, 32715, 32716, 32716,
	32716, 32716, 32716, 32716, 32717, 32717, 32717, 32717, 32717, 32717, 32717, 32718,
	32718, 32718, 32718, 32718, 32718, 32718, 32719, 32719, 32719, 32719, 32719, 32719,
	32719, 32720, 32720, 32720, 32720, 32720, 32720, 32720, 32720, 32721, 32721, 32721,
	32721, 32721, 32721, 32721, 32721, 32722, 32722, 32722, 32722, 32722, 32722, 32722,
	32722, 32723, 32723, 32723, 32723, 32723, 32723, 32723, 32723, 32724, 32724, 32724,
	32724, 32724, 32724, 32724, 32724, 32724, 32724, 32725, 32725, 32725, 32725, 32725,
	32725, 32725, 32725, 32725, 32726, 32726, 32726, 32726, 32726, 32726, 32726, 32726,
	32726, 32726, 32727, 32727, 32727, 32727, 32727, 32727, 32727, 32727, 32727, 32727,
	32727, 32728, 32728, 32728, 32728, 32728, 32728, 32728, 32728, 32728, 32728, 32728,
	32729, 32729, 32729, 32729, 32729, 32729, 32729, 32729, 32729, 32729, 32729, 32730,
	32730, 32730, 32730, 32730, 32730, 32730, 32730, 32730, 32730, 32730, 32730, 32730,
	32731, 32731, 32731, 32731, 32731, 32731, 32731, 32731, 32731, 32731, 32731, 32731,
	32731, 32732, 32732, 32732, 32732, 32732, 32732, 32732, 32732, 32732, 32732, 32732,
	32732, 32732, 32733, 32733, 32733, 32733, 32733, 32733, 32733, 32733, 32733, 32733,
	32733, 32733, 32733, 32733, 32733, 32734, 32734, 32734, 32734, 32734, 32734, 32734,
	32734, 32734, 32734, 32734, 32734, 32734, 32734, 32734, 32735, 32735, 32735, 32735,
	32735, 32735, 32735, 32735, 32735, 32736, 32736, 32736, 32736, 32736, 32736, 32736,
	32736, 32736, 32737, 32737, 32737, 32737, 32737, 32737, 32737, 32737, 32737, 32738,
	32738, 32738, 32738, 32738, 32738, 32738, 32738, 32738, 32738, 32739, 32739, 32739,
	32739, 32739, 32739, 32739, 32739, 32739, 32739, 32739, 32740, 32740, 32740, 32740,
	32740, 32740, 32740, 32740, 32740, 32740, 32740, 32740, 32741, 32741, 32741, 32741,
	32741, 32741, 32741, 32741, 32741, 32741, 32741, 32741, 32742, 32742, 32742, 32742,
	32742, 32742, 32742, 32742, 32742, 32742, 32742, 32742, 32742, 32742, 32743, 32743,
	32743, 32743, 32743, 32743, 32743, 32743, 32743, 32743, 32743, 32743, 32743, 32743,
	32743, 32744, 32744, 32744, 32744, 32744, 32744, 32744, 32744, 32744, 32744, 32744,
	32744, 32744, 32744, 32744, 32744, 32745, 32745, 32745, 32745, 32745, 32745, 32745,
	32745, 32745, 32745, 32745, 32745, 32745, 32745, 32745, 32745, 32745, 32745, 32746,
	32746, 32746, 32746, 32746, 32746, 32746, 32746, 32746, 32746, 32746, 32746, 32746,
	32746, 32746, 32746, 32746, 32746, 32746, 32747, 32747, 32747, 32747, 32747, 32747,
	32747, 32747, 32747, 32747, 32747, 32747, 32747, 32747, 32747, 32747, 32747, 32747,
	32747, 32747, 32747, 32747, 32748, 32748, 32748, 32748, 32748, 32748, 32748, 32748,
	32748, 32748, 32748, 32748, 32748, 32748, 32748, 32748, 32748, 32748, 32748, 32748,
	32748, 32748, 32748, 32748, 32749, 32749, 32749, 32749, 32749, 32749, 32749, 32749,
	32749, 32749, 32749, 32749, 32749, 32749, 32749, 32749, 32749, 32749, 32749, 32749,
	32749, 32749, 32749, 32749, 32749, 32749, 32750, 32750, 32750, 32750, 32750, 32750,
	32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750,
	32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750, 32750,
	32751, 32751, 32751, 32751, 32751, 32751, 32751, 32751, 32751, 32751, 32751, 32751,
	32751, 32751, 32751, 32751, 32751, 32751, 32752, 32752, 32752, 32752, 32752, 32752,
	32752, 32752, 32752, 32752, 32752, 32752, 32752, 32752, 32752, 32752, 32752, 32752,
	32752, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753,
	32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753, 32753,
	32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754,
	32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754, 32754,
	32754, 32754, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755,
	32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755,
	32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32755, 32756, 32756, 32756,
	32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756,
	32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756,
	32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32756, 32757, 32757,
	32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757,
	32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757,
	32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757,
	32757, 32757, 32757, 32757, 32757, 32757, 32757, 32757, 32758, 32758, 32758, 32758,
	32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758,
	32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758,
	32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758,
	32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758, 32758,
	32758, 32758, 32758, 32758, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759,
	32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759,
	32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759, 32759,
	32759, 32759, 32759, 32759, 32759, 32760, 32760, 32760, 32760, 32760, 32760, 32760,
	32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760,
	32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760,
	32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760, 32760,
	32760, 32760, 32760, 32760, 32760, 32760, 32761, 32761, 32761, 32761, 32761, 32761,
	32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761,
	32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761,
	32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761,
	32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761,
	32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761, 32761,
	32761, 32761, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762,
	32762, 32762, 32762, 32762, 32762, 32762, 32762, 32762, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763,
	32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32763, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764, 32764,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765, 32765,
	32765, 32765, 32765, 32765, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
	32766, 32766, 32766, 32766, 32766,
};
/* END GENERATED VOICE LIMITER TABLE */

typedef char av_limiter_table_covers_sum[
	((((uint64_t)(AUDIO_VOICE_MAX_VOICES + AUDIO_VOICE_FADE_SLOTS) *
	    32768U * AUDIO_VOICE_MASTER_MAX) >> 8) - 24576U + 8191U <
	  (1U << 29)) ? 1 : -1];

/* Nodes cover x = 8191 + d from 8192 through 2^29, 256 segments per
 * octave. The largest sum without a cubic voice is 2^21 * 65535 / 256, so
 * the translated input fits this range and int32. Linear interpolation of
 * floor(32767 - 8191^2/x) is monotonic and differs by at most two units
 * from the rational curve. Endpoint values stay below 32767. A sum with
 * cubic voices can be up to 25% larger (see AV_CUBIC_PEAK); it is held at
 * the last node, whose value is within 0.2 of the curve there. */
AV_INLINE int16_t limit_sample(int32_t sample)
{
	const uint32_t magnitude = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
	uint32_t x, octave, shift, index, fraction, value;

	if (magnitude <= 24576U)
		return (int16_t)sample;
	x = magnitude - 24576U + 8191U;
	if (x >= (1U << 29))
		x = (1U << 29) - 1U;
	octave = 31U - (uint32_t)__builtin_clz(x) - 13U;
	shift = octave + 5U;
	index = octave * 256U + (x >> shift) - 256U;
	fraction = x & ((1U << shift) - 1U);
	value = voice_limiter_table[index];
	value += ((voice_limiter_table[index + 1U] - value) * fraction) >> shift;
	return sample < 0 ? (int16_t)-(int32_t)value : (int16_t)value;
}

/* Separate from the plain finish kernels: no per-sample feature branch
 * or change to their NEON narrowing. Master multiplication is wide before
 * the limiter sees the sum, including all 64 possible contributors. */
AV_NOINLINE void finish_limited(const int32_t *l, const int32_t *r,
                                int16_t *out, uint32_t n, uint32_t master,
                                uint32_t narrow)
{
	uint32_t i;

	if (master == AUDIO_VOICE_MASTER_UNITY) {
		for (i = 0; i < n; i++) {
			out[2U * i] = limit_sample(l[i]);
			out[2U * i + 1U] = limit_sample(r[i]);
		}
	} else if (master <= narrow) {
		for (i = 0; i < n; i++) {
			out[2U * i] = limit_sample((l[i] * (int32_t)master) >> 8);
			out[2U * i + 1U] = limit_sample((r[i] * (int32_t)master) >> 8);
		}
	} else {
		for (i = 0; i < n; i++) {
			const int32_t a = (int32_t)(((int64_t)l[i] * master) >> 8);
			const int32_t b = (int32_t)(((int64_t)r[i] * master) >> 8);

			out[2U * i] = limit_sample(a);
			out[2U * i + 1U] = limit_sample(b);
		}
	}
}

#ifdef __ARM_NEON
/* Four sums at a time: the same arithmetic in vector form. vqmovn is the
 * S16 clamp; the 64-bit case narrows with vshrn, whose result fits int32
 * (at most 2^21 * 65535 / 256). The stores interleave left and right. */
AV_NOINLINE void finish(const int32_t *l, const int32_t *r, int16_t *out,
                        uint32_t n, uint32_t master, uint32_t narrow)
{
	uint32_t i = 0;
	int16x4x2_t o;

	if (master == AUDIO_VOICE_MASTER_UNITY) {
		for (; i + 4U <= n; i += 4U) {
			o.val[0] = vqmovn_s32(vld1q_s32(l + i));
			o.val[1] = vqmovn_s32(vld1q_s32(r + i));
			vst2_s16(out + 2U * i, o);
		}
	} else if (master <= narrow) {
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
	finish_c(l + i, r + i, out + 2U * i, n - i, master, narrow);
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
 * ---- Oversampled output ----
 *
 * With CONFIG OVERSAMPLE the chunk's sums hold two mixed frames per output
 * frame (see "Oversampling" in audio_voice.h). Finishing a stretch of them
 * scales the sums by the master in place, in 64 bits and without a clamp,
 * and decimates by two with the half-band FIR of audio_reduce_table.h on
 * the 32-bit values with a 64-bit accumulator: the largest tap sum, 1.77 *
 * 2^30, times a 32-bit sum bounded by 2^30 (see the master bounds above)
 * stays below 2^61. Output k takes the window of 59 mixed frames whose
 * newest is the second of its pair, so the centre tap is on the first frame
 * of the pair 14 frames earlier. Only then does the limiter or the clamp
 * make S16, once.
 *
 * A stretch always begins and ends on a pair: records apply at a pair's
 * first frame and a chunk holds whole pairs. The history is the 57 mixed
 * frames before the stretch (a window reaches 57 frames before its pair).
 * When the sums were never touched the input is zero, and with a zero
 * history so is the output, which is then written without the filter.
 */
typedef char av_dec_history_fits[
	(AUDIO_VOICE_DEC_HISTORY == AUDIO_REDUCE_TAPS - 2U) ? 1 : -1];
typedef char av_dec_delay_fits[
	(AUDIO_VOICE_OVERSAMPLE_DELAY_FRAMES == (AUDIO_REDUCE_TAPS - 1U) / 4U) ?
	1 : -1];
typedef char av_dec_chunk_even[(AUDIO_VOICE_MIX_FRAMES % 2U == 0U) ? 1 : -1];

/* The sum of two taps' inputs stays inside int32: a wide sum after the
 * master is at most the cubic bound of av_cubic_sum_after_master_fits. */
typedef char av_dec_pair_fits[
	((((int64_t)(AUDIO_VOICE_MAX_VOICES + AUDIO_VOICE_FADE_SLOTS) *
	   AV_CUBIC_PEAK * AUDIO_VOICE_MASTER_MAX) >> 8) * 2 < 2147483648LL) ?
	1 : -1];

AV_INLINE int32_t dec_sat(int64_t v)
{
	return (int32_t)(v > 2147483647LL ? 2147483647LL :
	                 v < -2147483647LL ? -2147483647LL : v);
}

/*
 * Output j of one side: the window of 59 sums whose newest is frame 2j + 1
 * of x, the frames before the start of x taken from the history h. Rounded
 * half up like audio_reduce_run().
 */
AV_INLINE int32_t dec_value(const int32_t *x, const int32_t *h, uint32_t j)
{
	const uint32_t hist = AUDIO_VOICE_DEC_HISTORY;
	const uint32_t centre = (AUDIO_REDUCE_TAPS - 1U) / 2U;
	int32_t win[AUDIO_REDUCE_TAPS];
	const int32_t *w;
	int64_t acc;
	uint32_t i;

	if (2U * j >= hist) {
		w = x + (2U * j - hist);
	} else {
		for (i = 0; i < AUDIO_REDUCE_TAPS; i++) {
			/* index into x of window slot i, negative before it */
			const int32_t idx = (int32_t)(2U * j + i) - (int32_t)hist;

			win[i] = idx < 0 ? h[(int32_t)hist + idx] : x[idx];
		}
		w = win;
	}
	acc = (int64_t)AUDIO_REDUCE_CENTRE * w[centre];
	for (i = 0U; i < AUDIO_REDUCE_SIDES; i++)
		acc += (int64_t)audio_reduce_side[i] *
		       (w[centre - (2U * i + 1U)] + w[centre + (2U * i + 1U)]);
	return dec_sat((acc + ((int64_t)1 << (AUDIO_REDUCE_Q - 1))) >>
	               AUDIO_REDUCE_Q);
}

#ifdef __ARM_NEON
/*
 * Outputs j .. j + 3 of one side, all four windows inside x (j >= 29 and
 * 2j + 7 below the end). The even frames of x are the centre taps and the
 * odd frames the side taps, so with the two kinds split by vld2 the four
 * windows are four consecutive lanes: output j takes the centre at even
 * frame j - 14 and the side pair at odd frames j - 14 + i and j - 15 - i.
 */
AV_INLINE void dec_block4(const int32_t *x, uint32_t j, int32_t *res)
{
	const int32_t *const p = x + 2U * (j - 14U);
	const int32x4_t centre = vld2q_s32(p).val[0];
	int64x2_t lo = vmull_n_s32(vget_low_s32(centre), AUDIO_REDUCE_CENTRE);
	int64x2_t hi = vmull_n_s32(vget_high_s32(centre), AUDIO_REDUCE_CENTRE);
	uint32_t i;

	for (i = 0U; i < AUDIO_REDUCE_SIDES; i++) {
		const int32x4_t a = vld2q_s32(p + 2U * i).val[1];
		const int32x4_t b = vld2q_s32(p - 2U * (i + 1U)).val[1];
		const int32x4_t s = vaddq_s32(a, b);

		lo = vmlal_n_s32(lo, vget_low_s32(s), audio_reduce_side[i]);
		hi = vmlal_n_s32(hi, vget_high_s32(s), audio_reduce_side[i]);
	}
	vst1q_s32(res, vcombine_s32(vqrshrn_n_s64(lo, AUDIO_REDUCE_Q),
	                            vqrshrn_n_s64(hi, AUDIO_REDUCE_Q)));
}
#endif

/* Scale n sums in place by the master, as finish() does before its clamp. */
AV_NOINLINE void scale_wide(int32_t *x, uint32_t n, uint32_t master,
                            uint32_t narrow)
{
	uint32_t i;

	if (master == AUDIO_VOICE_MASTER_UNITY)
		return;
	if (master <= narrow) {
		for (i = 0; i < n; i++)
			x[i] = (x[i] * (int32_t)master) >> 8;
	} else {
		for (i = 0; i < n; i++)
			x[i] = (int32_t)(((int64_t)x[i] * (int64_t)master) >> 8);
	}
}

/* The final stage on one filter output. */
AV_INLINE int16_t dec_out(int32_t v, int limited)
{
	return limited ? limit_sample(v) : clamp16(v);
}

/* Decimate the n sums (n even) of both sides that follow their histories
 * into n / 2 interleaved stereo frames at out. */
AV_NOINLINE void dec_run(const int32_t *xl, const int32_t *xr,
                         const int32_t *hl, const int32_t *hr, uint32_t n,
                         int16_t *out, int limited)
{
	const uint32_t outs = n / 2U;
	/* the first output whose window lies inside x */
	const uint32_t inside = AUDIO_VOICE_DEC_HISTORY / 2U + 1U;
	uint32_t j = 0;

	for (; j < outs && j < inside; j++) {
		out[2U * j] = dec_out(dec_value(xl, hl, j), limited);
		out[2U * j + 1U] = dec_out(dec_value(xr, hr, j), limited);
	}
#ifdef __ARM_NEON
	for (; j + 4U <= outs; j += 4U) {
		int32_t l[4], r[4];

		dec_block4(xl, j, l);
		dec_block4(xr, j, r);
		if (!limited) {
			int16x4x2_t o;

			o.val[0] = vqmovn_s32(vld1q_s32(l));
			o.val[1] = vqmovn_s32(vld1q_s32(r));
			vst2_s16(out + 2U * j, o);
		} else {
			uint32_t k;

			for (k = 0; k < 4U; k++) {
				out[2U * (j + k)] = limit_sample(l[k]);
				out[2U * (j + k) + 1U] = limit_sample(r[k]);
			}
		}
	}
#endif
	for (; j < outs; j++) {
		out[2U * j] = dec_out(dec_value(xl, hl, j), limited);
		out[2U * j + 1U] = dec_out(dec_value(xr, hr, j), limited);
	}
}

/* The last AUDIO_VOICE_DEC_HISTORY mixed frames of the history and the n
 * (even) sums x that follow it. */
static void dec_keep(int32_t *h, const int32_t *x, uint32_t n)
{
	const uint32_t hist = AUDIO_VOICE_DEC_HISTORY;

	if (n >= hist) {
		memcpy(h, x + (n - hist), hist * sizeof(h[0]));
	} else {
		memmove(h, h + n, (hist - n) * sizeof(h[0]));
		memcpy(h + (hist - n), x, n * sizeof(h[0]));
	}
}

/* finish_range() of an oversampled chunk: from chunk_fin to `to`, both in
 * mixed frames and even. */
static void finish_range_os(struct audio_voice_engine *e, uint32_t to)
{
	const uint32_t from = e->chunk_fin;
	const uint32_t n = to - from;
	int16_t *const o = e->chunk_out + from;
	const uint32_t hist = AUDIO_VOICE_DEC_HISTORY;

	e->chunk_fin = to;
	if (n == 0U)
		return;
	if (!e->chunk_dirty) {
		if (e->dec_quiet >= hist) {
			av_zero(o, n * sizeof(o[0]));
			return;
		}
		av_zero(e->mix[0] + from, n * sizeof(e->mix[0][0]));
		av_zero(e->mix[1] + from, n * sizeof(e->mix[1][0]));
		e->dec_quiet += n;
		if (e->dec_quiet > hist)
			e->dec_quiet = hist;
	} else {
		const uint32_t narrow = e->chunk_cubic ? AV_MASTER_NARROW_CUBIC :
		                                         AV_MASTER_NARROW;

		scale_wide(e->mix[0] + from, n, e->master, narrow);
		scale_wide(e->mix[1] + from, n, e->master, narrow);
		e->dec_quiet = 0;
	}
	dec_run(e->mix[0] + from, e->mix[1] + from, e->dec_hist[0],
	        e->dec_hist[1], n, o, e->limiter != 0U);
	dec_keep(e->dec_hist[0], e->mix[0] + from, n);
	dec_keep(e->dec_hist[1], e->mix[1] + from, n);
}

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
			const uint32_t mode = voice_interp(e, v);

			if (!e->chunk_dirty && may_sound(v))
				clear_sums(e);
			if (mode == AUDIO_VOICE_INTERP_CUBIC)
				mix_voice_cubic(e, v, v->cur, n);
			else
				mix_voice(e, mode == AUDIO_VOICE_INTERP_LINEAR, v, v->cur,
				          n);
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

/* The output frames of the chunk. */
static uint32_t chunk_frames(const struct audio_voice_engine *e)
{
	return e->chunk_n >> e->oversample;
}

/* Scale the sums of the frames from chunk_fin to `to` by the master into the
 * output; all voices must be mixed to `to`. */
static void finish_range(struct audio_voice_engine *e, uint32_t to)
{
	const uint32_t from = e->chunk_fin;

	if (e->oversample) {
		finish_range_os(e, to);
		return;
	}

	if (to > from) {
		int16_t *o = e->chunk_out + 2U * from;

		if (e->chunk_dirty) {
			const uint32_t narrow = e->chunk_cubic ? AV_MASTER_NARROW_CUBIC :
			                                         AV_MASTER_NARROW;

			if (e->limiter)
				finish_limited(e->mix[0] + from, e->mix[1] + from,
				               o, to - from, e->master, narrow);
			else
				finish(e->mix[0] + from, e->mix[1] + from, o,
				       to - from, e->master, narrow);
		} else {
			av_zero(o, 2U * (to - from) * sizeof(o[0]));
		}
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

static void limiter_change(struct audio_voice_engine *e, uint32_t off,
                           uint32_t enabled)
{
	if (e->limiter != enabled) {
		sync_all(e, off);
		finish_range(e, off);
		e->limiter = enabled;
	}
}

static void begin_chunk(struct audio_voice_engine *e, int16_t *out,
                        uint32_t base, uint32_t frames)
{
	const uint32_t left = (frames - base) << e->oversample;

	e->chunk_n = left < AUDIO_VOICE_MIX_FRAMES ? left : AUDIO_VOICE_MIX_FRAMES;
	e->chunk_out = out + 2U * base;
	e->chunk_abs = (e->frame + base) << e->oversample;
	e->chunk_fin = 0;
	e->chunk_dirty = 0;
	e->chunk_cubic = 0;
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
		while (at >= base + chunk_frames(e)) {
			end_chunk(e);
			base += chunk_frames(e);
			begin_chunk(e, out, base, frames);
		}
		e->chunk_off = (at - base) << e->oversample;
		if (apply(e, &cmds[used])) {
			/* CONFIG OVERSAMPLE ended the chunk at this record. */
			base = at;
			begin_chunk(e, out, base, frames);
		}
		used++;
	}
	for (;;) {
		end_chunk(e);
		base += chunk_frames(e);
		if (base >= frames)
			break;
		begin_chunk(e, out, base, frames);
	}
	e->frame += frames;
	return used;
}
