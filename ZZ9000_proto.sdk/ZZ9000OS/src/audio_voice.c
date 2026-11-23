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

	for (i = 0; i < AUDIO_VOICE_MAX_VOICES; i++)
		e->voice[i].active = 0;
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

static void apply(struct audio_voice_engine *e,
                  const struct audio_voice_cmd *c)
{
	struct audio_voice *v;

	if (c->op == AUDIO_VOICE_OP_MASTER) {
		e->master = c->arg0 > AUDIO_VOICE_MASTER_MAX ?
		            AUDIO_VOICE_MASTER_MAX : c->arg0;
		return;
	}
	if (c->voice >= AUDIO_VOICE_MAX_VOICES) {
		e->rejected++;
		return;
	}
	v = &e->voice[c->voice];
	switch (c->op) {
	case AUDIO_VOICE_OP_START:
		start_voice(e, v, c);
		break;
	case AUDIO_VOICE_OP_LOOP:
		set_loop(e, v, c);
		break;
	case AUDIO_VOICE_OP_SET:
		if (c->flags & AUDIO_VOICE_SET_RATE)
			set_rate(e, v, c->flags, c->arg0);
		if (c->flags & AUDIO_VOICE_SET_VOLUME) {
			v->vol_l = c->arg1 >> 16;
			v->vol_r = c->arg1 & 0xFFFFU;
		}
		break;
	case AUDIO_VOICE_OP_STOP:
		v->active = 0;
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

/* The two sides of the frame at q, in the units of the format (8-bit or
 * 16-bit); a mono frame feeds both sides. */
AV_INLINE void fetch(const int wide, const int stereo, const uint8_t *q,
                     int32_t *l, int32_t *r)
{
	if (wide) {
		if (stereo) {
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
		} else {
			*l = be16s(q);
			*r = *l;
		}
	} else {
		*l = (int8_t)q[0];
		*r = stereo ? (int8_t)q[1] : *l;
	}
}

/*
 * Mix n frames of one voice into the sums. p is the frame at the voice
 * position, x the 16-bit fraction as an int32 that moves by dx per frame
 * (the step, negated for backward play) and nbytes the byte distance to the
 * blend partner (one frame, negated for backward play); the caller
 * guarantees that for every frame that partner is the neighbouring frame in
 * the direction of play and that both frames lie inside the sample.
 *
 * The 8-bit nearest case multiplies the raw sample and shifts by 8 instead
 * of promoting it first: (s * 256 * v) >> 16 == (s * v) >> 8.
 * The linear blend is the same value as a + (((b - a) * frac) >> 16) written
 * as (a * (65536 - frac) + b * frac) >> 16, which stays inside int32 (the
 * weights sum to 65536 and a and b are S16), so no 64-bit product is needed;
 * 8-bit data blends raw and shifts by 8 for the same reason.
 */
AV_INLINE void kernel(const int wide, const int stereo, const int linear,
                      const uint8_t *restrict p, int32_t x, const int32_t dx,
                      const int32_t nbytes, uint32_t n, const int32_t vl,
                      const int32_t vr, int32_t *restrict al,
                      int32_t *restrict ar)
{
	const int fs = wide + stereo; /* log2 of the frame size in bytes */
	uint32_t i;

	for (i = 0; i < n; i++) {
		const uint8_t *q = p + (x >> 16) * (1 << fs);
		int32_t l, r;

		fetch(wide, stereo, q, &l, &r);
		if (linear) {
			int32_t bl, br;
			const int32_t fr = x & 0xFFFF;
			const int32_t w0 = 65536 - fr;

			fetch(wide, stereo, q + nbytes, &bl, &br);
			l = (l * w0 + bl * fr) >> (wide ? 16 : 8);
			r = stereo ? (r * w0 + br * fr) >> (wide ? 16 : 8) : l;
			al[i] += (l * vl) >> 16;
			ar[i] += (r * vr) >> 16;
		} else {
			al[i] += (l * vl) >> (wide ? 16 : 8);
			ar[i] += (r * vr) >> (wide ? 16 : 8);
		}
		x += dx;
	}
}

AV_NOINLINE void run_kernel(const struct audio_voice *v, int linear,
                       const uint8_t *p, int32_t x, int32_t dx,
                       int32_t nbytes, uint32_t n, int32_t vl, int32_t vr,
                       int32_t *al, int32_t *ar)
{
#define AV_K(w, s, l) \
	kernel(w, s, l, p, x, dx, nbytes, n, vl, vr, al, ar); break
	switch ((v->wide ? 4 : 0) | (v->stereo ? 2 : 0) | (linear ? 1 : 0)) {
	case 0: AV_K(0, 0, 0);
	case 1: AV_K(0, 0, 1);
	case 2: AV_K(0, 1, 0);
	case 3: AV_K(0, 1, 1);
	case 4: AV_K(1, 0, 0);
	case 5: AV_K(1, 0, 1);
	case 6: AV_K(1, 1, 0);
	default: AV_K(1, 1, 1);
	}
#undef AV_K
}

/* Frames of a linear voice that sit on the pass edge: the position does
 * not move there, only the fraction, and the blend partner is the loop
 * start (or the frame itself) instead of the next frame. la/ra and lb/rb
 * are the sides of the frame and of its partner as read from the sample. */
AV_NOINLINE void run_edge(const struct audio_voice *v, int32_t la, int32_t lb,
                     int32_t ra, int32_t rb, int32_t x, int32_t dx, uint32_t n,
                     int32_t vl, int32_t vr, int32_t *al, int32_t *ar)
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
 * Mix n frames of an active voice (step != 0) into al/ar, advancing it. A
 * voice whose volumes are both zero adds exactly nothing to the sums and is
 * only advanced.
 */
AV_NOINLINE void mix_voice(const uint8_t *bank, int linear, struct audio_voice *v,
                      int32_t *al, int32_t *ar, uint32_t n)
{
	const uint32_t step = v->step;
	const int32_t vl = (int32_t)v->vol_l;
	const int32_t vr = (int32_t)v->vol_r;
	const int audible = (v->vol_l | v->vol_r) != 0U;
	uint32_t done = 0;

	while (done < n && v->active) {
		const uint32_t left = n - done;
		const uint64_t pos = ((uint64_t)v->pos << 16) | v->frac;
		const int back = v->backward;
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

		if (audible) {
			const int32_t dx = back ? -(int32_t)step : (int32_t)step;
			const int32_t fb = (int32_t)1 << v->shift;

			if (safe != 0U)
				run_kernel(v, linear,
				           bank + v->base + ((size_t)v->pos << v->shift),
				           (int32_t)v->frac, dx, back ? -fb : fb, safe, vl,
				           vr, al + done, ar + done);
			below = step_state(v, safe);
			if (safe < run) {
				int32_t la, lb, ra, rb;

				fetch(v->wide, v->stereo,
				      bank + v->base + ((size_t)v->pos << v->shift), &la,
				      &ra);
				fetch(v->wide, v->stereo,
				      bank + v->base + ((size_t)neighbour(v) << v->shift),
				      &lb, &rb);
				run_edge(v, la, lb, ra, rb, (int32_t)v->frac, dx, run - safe,
				         vl, vr, al + done + safe, ar + done + safe);
				below = step_state(v, run - safe);
			}
		} else {
			below = step_state(v, run);
		}
		if (hit)
			pass_end(v, below);
		done += run;
	}
}

/*
 * Scale the sums by the master and clamp to S16: x = (sum * master) >> 8 in
 * 64 bits, arithmetic shift, then saturate, per side of every frame.
 */
AV_NOINLINE void finish(const int32_t *l, const int32_t *r, int16_t *out,
                   uint32_t n, uint32_t master)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		int64_t xl = ((int64_t)l[i] * (int64_t)master) >> 8;
		int64_t xr = ((int64_t)r[i] * (int64_t)master) >> 8;

		out[2U * i] = (int16_t)(xl > 32767 ? 32767 : xl < -32768 ? -32768 : xl);
		out[2U * i + 1U] =
			(int16_t)(xr > 32767 ? 32767 : xr < -32768 ? -32768 : xr);
	}
}

static void mix(struct audio_voice_engine *e, int16_t *out, uint32_t frames)
{
	const int linear = e->interp == AUDIO_VOICE_INTERP_LINEAR;

	while (frames != 0U) {
		const uint32_t n = frames < AUDIO_VOICE_MIX_FRAMES ?
		                   frames : AUDIO_VOICE_MIX_FRAMES;
		int dirty = 0;
		uint32_t k;

		for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
			struct audio_voice *v = &e->voice[k];

			if (!v->active || v->step == 0U)
				continue;
			if (!dirty && (v->vol_l | v->vol_r) != 0U) {
				memset(e->mix_l, 0, n * sizeof(e->mix_l[0]));
				memset(e->mix_r, 0, n * sizeof(e->mix_r[0]));
				dirty = 1;
			}
			mix_voice(e->bank, linear, v, e->mix_l, e->mix_r, n);
		}
		if (dirty)
			finish(e->mix_l, e->mix_r, out, n, e->master);
		else
			memset(out, 0, 2U * n * sizeof(out[0]));
		out += 2U * n;
		frames -= n;
	}
}

size_t audio_voice_render(struct audio_voice_engine *e, int16_t *out,
                          uint32_t frames, const struct audio_voice_cmd *cmds,
                          size_t ncmds)
{
	uint32_t at = 0;
	size_t used = 0;

	while (used < ncmds) {
		int32_t rel = (int32_t)(cmds[used].stamp - e->frame);
		int late = rel < 0;

		if (late)
			rel = 0;
		if ((uint32_t)rel >= frames)
			break;
		e->late += (uint32_t)late;
		if ((uint32_t)rel > at) {
			mix(e, out + 2U * at, (uint32_t)rel - at);
			at = (uint32_t)rel;
		}
		apply(e, &cmds[used]);
		used++;
	}
	if (at < frames)
		mix(e, out + 2U * at, frames - at);
	e->frame += frames;
	return used;
}
