/*
 * Per-voice interpolation and the cubic reader of the voice engine (MODE,
 * CONFIG default interpolation, "Interpolation" in audio_voice.h).
 *
 * Three kinds of reference, none of which share the engine's run planning,
 * vector kernel or edge splitting:
 *
 *   - real arithmetic: a sine at several steps against the Catmull-Rom curve
 *     in long double (the stated error of the fixed point form) and against
 *     the sine itself, with linear for comparison;
 *   - unrolled streams: for forward and backward play, first pass, loops of
 *     one, two and many frames and loop restarts, the frames a voice plays
 *     are written out as one list in play order (the unrolled loop) and
 *     every output frame is the cubic of four neighbours in that list;
 *   - a frame-by-frame model of the whole engine (voices, ramps, fade slots,
 *     MODE, CONFIG), checked against the engine on random sequences of
 *     records, together with the engine rendered in odd pieces.
 *
 * Directed cases cover the overshoot of the curve at the largest gain and
 * master with 32 voices and 32 tails, the limiter, all four sample formats,
 * rejected records, same-stamp order and tails that keep the interpolation
 * they were handed over with. An explicit linear voice in a nearest
 * session must equal the session-linear PCM bit for bit.
 *
 * Environment: ZZ9K_VOICE_CUBIC_SEQS (default 6000), ZZ9K_VOICE_CUBIC_SEED.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define HAVE_ALARM 1
#endif

#include "audio_voice.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define INTERP_N AUDIO_VOICE_INTERP_NEAREST
#define INTERP_L AUDIO_VOICE_INTERP_LINEAR
#define INTERP_C AUDIO_VOICE_INTERP_CUBIC

static struct audio_voice_cmd mk(uint32_t stamp, uint8_t voice, uint8_t op,
                                 uint16_t flags, uint32_t a0, uint32_t a1)
{
	struct audio_voice_cmd c;

	c.stamp = stamp;
	c.voice = voice;
	c.op = op;
	c.flags = flags;
	c.arg0 = a0;
	c.arg1 = a1;
	return c;
}

#define SETHZ(stamp, v, hz, vl, vr) \
	mk((stamp), (v), AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE | \
	   AUDIO_VOICE_RATE_HZ | AUDIO_VOICE_SET_VOLUME, (hz), \
	   ((uint32_t)(vl) << 16) | (vr))
#define MODE(stamp, v, sel) \
	mk((stamp), (v), AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, (sel), 0)
#define CFGINT(stamp, sel) \
	mk((stamp), 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP, (sel), 0)
#define CFGRAMP(stamp, frames) \
	mk((stamp), 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, (frames), 0)

/* ---- the model ---- */

struct rvoice {
	int active, backward, stopping, wide, stereo, looped;
	uint32_t vint, shift, base, len, pos, frac, end, ls, ll, step, vl, vr, left;
	int32_t gl, gr, il, ir;
};

struct rmodel {
	const uint8_t *bank;
	uint32_t bank_len, clock, interp, master, ramp, late, rejected, frame;
	uint32_t seq;
	struct {
		uint32_t off, len, fmt;
		int def;
	} smp[AUDIO_VOICE_MAX_SAMPLES];
	struct rvoice v[AUDIO_VOICE_MAX_VOICES];
	struct rvoice f[AUDIO_VOICE_FADE_SLOTS];
	uint32_t age[AUDIO_VOICE_FADE_SLOTS];
};

static void r_init(struct rmodel *m, const uint8_t *bank, uint32_t len,
                   uint32_t clock)
{
	memset(m, 0, sizeof(*m));
	m->bank = bank;
	m->bank_len = len;
	m->clock = clock;
	m->master = 256;
}

static int r_sample_set(struct rmodel *m, uint32_t id, uint32_t off,
                        uint32_t len, uint32_t fmt)
{
	if (id >= 256 || fmt >= 4 || (uint64_t)off + len > m->bank_len)
		return -1;
	m->smp[id].off = off;
	m->smp[id].len = len;
	m->smp[id].fmt = fmt;
	m->smp[id].def = 1;
	return 0;
}

static uint32_t r_shift(uint32_t fmt)
{
	return fmt == 3 ? 2 : (fmt == 1 || fmt == 2) ? 1 : 0;
}

/* One side of frame idx as a 16-bit value. */
static int32_t r_side(const struct rmodel *m, const struct rvoice *v,
                      uint32_t idx, int side)
{
	if (v->wide) {
		const uint8_t *p = m->bank + v->base +
		                   (v->stereo ? 4U * idx + 2U * side : 2U * idx);

		return (int16_t)(uint16_t)((p[0] << 8) | p[1]);
	}
	return (int32_t)(int8_t)m->bank[v->base +
	                                (v->stereo ? 2U * idx + side : idx)] * 256;
}

/* The interpolation a voice reads with: 0 nearest, 1 linear, 2 cubic. */
static uint32_t r_mode(const struct rmodel *m, const struct rvoice *v)
{
	return v->vint ? v->vint - 1 : m->interp;
}

/* The frame a linear read blends toward: next in play order. */
static uint32_t r_neighbour(const struct rvoice *v)
{
	if (v->backward) {
		if (v->pos > v->ls)
			return v->pos - 1;
		return v->ll ? v->ls + v->ll - 1 : v->pos;
	}
	if (v->pos + 1 < v->end)
		return v->pos + 1;
	return v->ll ? v->ls : v->pos;
}

/* The four frames a cubic read of the frame at pos uses, by walking the play
 * order from the position: the frame after the pass end is the loop start
 * (the last frame again without a loop), and once that has wrapped the
 * pass ends at the loop end. */
static void r_taps(const struct rvoice *v, uint32_t pos, uint32_t t[4])
{
	const uint32_t le = v->ls + v->ll;
	uint32_t bound = v->end, i = pos, k;

	t[1] = pos;
	for (k = 2; k < 4; k++) {
		if (!v->backward) {
			if (i + 1 < bound) {
				i = i + 1;
			} else if (v->ll) {
				i = v->ls;
				bound = le;
			}
		} else {
			if (i > v->ls)
				i = i - 1;
			else if (v->ll)
				i = le - 1;
		}
		t[k] = i;
	}
	if (!v->backward) {
		if (v->looped && v->ll && pos == v->ls)
			t[0] = le - 1;
		else
			t[0] = pos ? pos - 1 : pos;
	} else {
		if (pos + 1 < v->end)
			t[0] = pos + 1;
		else
			t[0] = v->looped && v->ll ? v->ls : pos;
	}
}

/* Catmull-Rom with t in 1/65536, every product of the Horner form rounded
 * to nearest, written out from the formula in audio_voice.h. */
static int32_t r_cubic(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t t)
{
	const int64_t c1 = (int64_t)c - a;
	const int64_t c2 = 2 * (int64_t)a - 5 * (int64_t)b + 4 * (int64_t)c - d;
	const int64_t c3 = -(int64_t)a + 3 * (int64_t)b - 3 * (int64_t)c + d;
	int64_t u;

	u = c2 + ((c3 * (int64_t)t + 32768) >> 16);
	u = c1 + ((u * (int64_t)t + 32768) >> 16);
	return (int32_t)(b + ((u * (int64_t)t + 65536) >> 17));
}

static void r_advance(struct rvoice *v)
{
	if (!v->backward) {
		uint64_t p = ((uint64_t)v->pos << 16 | v->frac) + v->step;

		v->pos = (uint32_t)(p >> 16);
		v->frac = (uint32_t)(p & 0xFFFF);
		if (v->pos < v->end)
			return;
		if (!v->ll) {
			v->active = 0;
		} else {
			uint32_t over = v->pos - v->end;

			if (over >= v->ll)
				over = 0;
			v->pos = v->ls + over;
			v->end = v->ls + v->ll;
			v->looped = 1;
		}
	} else {
		uint64_t cur = (uint64_t)v->pos << 16 | v->frac;
		int below = cur < v->step;

		if (below) {
			v->pos = 0;
			v->frac = 0;
		} else {
			cur -= v->step;
			v->pos = (uint32_t)(cur >> 16);
			v->frac = (uint32_t)(cur & 0xFFFF);
		}
		if (!below && v->pos >= v->ls)
			return;
		if (!v->ll) {
			v->active = 0;
		} else {
			uint32_t under = v->ls - v->pos;

			if (under >= v->ll)
				under = 0;
			v->end = v->ls + v->ll;
			v->pos = v->end - under - 1;
			v->looped = 1;
		}
	}
}

static void r_tick(struct rvoice *v)
{
	if (!v->left)
		return;
	v->gl += v->il;
	v->gr += v->ir;
	if (--v->left == 0) {
		v->gl = v->stopping ? 0 : (int32_t)(v->vl << 8);
		v->gr = v->stopping ? 0 : (int32_t)(v->vr << 8);
		if (v->stopping)
			v->active = 0;
	}
}

static void r_gains(const struct rvoice *v, int32_t *l, int32_t *r)
{
	*l = v->left ? v->gl : (int32_t)(v->vl << 8);
	*r = v->left ? v->gr : (int32_t)(v->vr << 8);
}

static void r_ramp_to(struct rvoice *v, int32_t gl, int32_t gr, int32_t tl,
                      int32_t tr, uint32_t n)
{
	if (gl == tl && gr == tr) {
		v->left = 0;
		return;
	}
	v->gl = gl;
	v->gr = gr;
	v->il = (tl - gl) / (int32_t)n;
	v->ir = (tr - gr) / (int32_t)n;
	v->left = n;
}

static void r_fade_out(struct rmodel *m, const struct rvoice *v)
{
	uint32_t i, slot = AUDIO_VOICE_FADE_SLOTS, oldest = 0;
	int32_t gl, gr;

	if (v->step == 0)
		return;
	r_gains(v, &gl, &gr);
	if (!v->stopping && gl == 0 && gr == 0)
		return;
	for (i = 0; i < AUDIO_VOICE_FADE_SLOTS; i++) {
		if (!m->f[i].active) {
			slot = i;
			break;
		}
		if (m->age[i] < m->age[oldest])
			oldest = i;
	}
	if (slot == AUDIO_VOICE_FADE_SLOTS)
		slot = oldest;
	m->f[slot] = *v;
	/* the tail keeps the interpolation the voice has now */
	m->f[slot].vint = r_mode(m, v) + 1;
	if (!v->stopping) {
		r_ramp_to(&m->f[slot], gl, gr, 0, 0, m->ramp);
		m->f[slot].stopping = 1;
	}
	m->age[slot] = ++m->seq;
}

static void r_start(struct rmodel *m, struct rvoice *v,
                    const struct audio_voice_cmd *c)
{
	uint32_t shift, n, off, len, end;

	v->active = 0;
	v->left = 0;
	v->stopping = 0;
	if (c->flags >= 256 || !m->smp[c->flags].def) {
		m->rejected++;
		return;
	}
	shift = r_shift(m->smp[c->flags].fmt);
	n = m->smp[c->flags].len >> shift;
	off = c->arg0 >> shift;
	len = c->arg1 >> shift;
	if (off >= n || (c->arg1 && !len)) {
		m->rejected++;
		return;
	}
	end = n;
	if (c->arg1 && (uint64_t)off + len < n)
		end = off + len;
	v->shift = shift;
	v->wide = m->smp[c->flags].fmt == 1 || m->smp[c->flags].fmt == 3;
	v->stereo = m->smp[c->flags].fmt == 2 || m->smp[c->flags].fmt == 3;
	v->base = m->smp[c->flags].off;
	v->len = n;
	v->pos = off;
	v->frac = 0;
	v->end = end;
	v->backward = 0;
	v->ls = 0;
	v->ll = 0;
	v->looped = 0;
	v->active = 1;
}

static void r_config(struct rmodel *m, const struct audio_voice_cmd *c)
{
	if (c->flags == 0 || (c->flags & ~7U) != 0) {
		m->rejected++;
	} else if (c->flags & AUDIO_VOICE_CONFIG_INTERP) {
		if (c->flags != AUDIO_VOICE_CONFIG_INTERP || c->arg0 < 1 ||
		    c->arg0 > 3 || c->arg1 != 0)
			m->rejected++;
		else
			m->interp = c->arg0 - 1;
	} else if ((c->flags & ~1U) != 0 || c->arg0 > AUDIO_VOICE_RAMP_MAX_FRAMES) {
		/* the limiter is not modelled here and is never sent */
		m->rejected++;
	} else {
		m->ramp = c->arg0;
	}
}

static void r_apply(struct rmodel *m, const struct audio_voice_cmd *c)
{
	struct rvoice *v;
	uint32_t hz;

	if (c->op == AUDIO_VOICE_OP_MASTER) {
		m->master = c->arg0 > 0xFFFF ? 0xFFFF : c->arg0;
		return;
	}
	if (c->op == AUDIO_VOICE_OP_CONFIG) {
		r_config(m, c);
		return;
	}
	if (c->voice >= 32) {
		m->rejected++;
		return;
	}
	v = &m->v[c->voice];
	switch (c->op) {
	case AUDIO_VOICE_OP_START:
		if (m->ramp && v->active)
			r_fade_out(m, v);
		r_start(m, v, c);
		if (m->ramp && v->active)
			r_ramp_to(v, 0, 0, (int32_t)(v->vl << 8),
			          (int32_t)(v->vr << 8), m->ramp);
		break;
	case AUDIO_VOICE_OP_LOOP: {
		uint32_t ls = c->arg0 >> v->shift, ll = c->arg1 >> v->shift;

		if (ll == 0 ? (c->arg1 == 0 && ls <= v->len) :
		              ((uint64_t)ls + ll <= v->len)) {
			v->ls = ls;
			v->ll = ll;
		} else {
			m->rejected++;
		}
		break;
	}
	case AUDIO_VOICE_OP_SET:
		if (c->flags & AUDIO_VOICE_SET_RATE) {
			hz = 0;
			if (c->flags & AUDIO_VOICE_RATE_HZ)
				hz = c->arg0;
			else if (c->arg0 >= AUDIO_VOICE_MIN_PERIOD)
				hz = m->clock / c->arg0;
			if (hz) {
				if (hz > 192000)
					hz = 192000;
				v->step = (uint32_t)(((uint64_t)hz << 16) / 48000);
			}
		}
		if (c->flags & AUDIO_VOICE_SET_VOLUME) {
			int32_t gl, gr;

			r_gains(v, &gl, &gr);
			v->vl = c->arg1 >> 16;
			v->vr = c->arg1 & 0xFFFF;
			if (v->stopping) {
				/* the voice fades out; only the target changes */
			} else if (m->ramp && v->active) {
				r_ramp_to(v, gl, gr, (int32_t)(v->vl << 8),
				          (int32_t)(v->vr << 8), m->ramp);
			} else {
				v->left = 0;
			}
		}
		break;
	case AUDIO_VOICE_OP_STOP:
		if (m->ramp && v->active) {
			int32_t gl, gr;

			if (v->stopping)
				break;
			r_gains(v, &gl, &gr);
			if ((gl == 0 && gr == 0) || v->step == 0) {
				v->active = 0;
				v->left = 0;
			} else {
				r_ramp_to(v, gl, gr, 0, 0, m->ramp);
				v->stopping = 1;
			}
		} else {
			v->active = 0;
			v->left = 0;
			v->stopping = 0;
		}
		break;
	case AUDIO_VOICE_OP_DIRECTION:
		if (v->active)
			v->backward = (c->flags & AUDIO_VOICE_DIR_BACKWARD) != 0;
		break;
	case AUDIO_VOICE_OP_MODE:
		if (c->flags != AUDIO_VOICE_MODE_INTERP || c->arg0 > 3 || c->arg1 != 0)
			m->rejected++;
		else
			v->vint = c->arg0;
		break;
	default:
		m->rejected++;
		break;
	}
}

/* The two sides of the voice's current frame as 16-bit values, before the
 * volume. */
static void r_read(const struct rmodel *m, const struct rvoice *v,
                   int32_t *sl, int32_t *sr)
{
	const uint32_t mode = r_mode(m, v);

	*sl = r_side(m, v, v->pos, 0);
	*sr = v->stereo ? r_side(m, v, v->pos, 1) : *sl;
	if (mode == INTERP_L) {
		uint32_t n = r_neighbour(v);
		int32_t bl = r_side(m, v, n, 0);
		int32_t br = v->stereo ? r_side(m, v, n, 1) : bl;

		*sl += (int32_t)(((int64_t)(bl - *sl) * (int64_t)v->frac) >> 16);
		*sr = v->stereo ?
			*sr + (int32_t)(((int64_t)(br - *sr) * (int64_t)v->frac) >> 16) :
			*sl;
	} else if (mode == INTERP_C) {
		uint32_t t[4];
		int32_t l[4], r[4];
		int j;

		r_taps(v, v->pos, t);
		for (j = 0; j < 4; j++) {
			l[j] = r_side(m, v, t[j], 0);
			r[j] = v->stereo ? r_side(m, v, t[j], 1) : l[j];
		}
		*sl = r_cubic(l[0], l[1], l[2], l[3], v->frac);
		*sr = v->stereo ? r_cubic(r[0], r[1], r[2], r[3], v->frac) : *sl;
	}
}

/* The sums after the master, per frame since the model's first, when a test
 * asks to see them. */
static int64_t *g_rec_l, *g_rec_r;
static uint32_t g_rec_cap;

static void r_frame(struct rmodel *m, int16_t *out)
{
	int64_t sum_l = 0, sum_r = 0;
	uint32_t k;

	for (k = 0; k < 64; k++) {
		struct rvoice *v = k < 32 ? &m->v[k] : &m->f[k - 32];
		int32_t sl, sr, vl, vr;

		if (!v->active)
			continue;
		if (v->step == 0) {
			r_tick(v);
			continue;
		}
		vl = v->left ? (v->gl >> 8) : (int32_t)v->vl;
		vr = v->left ? (v->gr >> 8) : (int32_t)v->vr;
		r_read(m, v, &sl, &sr);
		sum_l += ((int64_t)sl * vl) >> 16;
		sum_r += ((int64_t)sr * vr) >> 16;
		r_advance(v);
		r_tick(v);
	}
	{
		int64_t l = (sum_l * (int64_t)m->master) >> 8;
		int64_t r = (sum_r * (int64_t)m->master) >> 8;

		if (g_rec_l != NULL && m->frame < g_rec_cap) {
			g_rec_l[m->frame] = l;
			g_rec_r[m->frame] = r;
		}
		out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
		out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
}

static size_t r_render(struct rmodel *m, int16_t *out, uint32_t frames,
                       const struct audio_voice_cmd *cmds, size_t n)
{
	size_t used = 0;
	uint32_t i;

	for (i = 0; i < frames; i++) {
		while (used < n) {
			int32_t rel = (int32_t)(cmds[used].stamp - m->frame);

			if (rel > 0)
				break;
			if (rel < 0)
				m->late++;
			r_apply(m, &cmds[used]);
			used++;
		}
		r_frame(m, out + 2U * i);
		m->frame++;
	}
	return used;
}

/* ---- comparison ---- */

static uint32_t fades_in_use(const struct audio_voice_engine *e)
{
	uint32_t i, n = 0;

	for (i = 0; i < AUDIO_VOICE_FADE_SLOTS; i++)
		n += e->fade[i].active;
	return n;
}

static uint32_t model_fades_in_use(const struct rmodel *m)
{
	uint32_t i, n = 0;

	for (i = 0; i < AUDIO_VOICE_FADE_SLOTS; i++)
		n += m->f[i].active != 0;
	return n;
}

static int same_state(const struct audio_voice_engine *e,
                      const struct rmodel *m, uint32_t seq, const char *when)
{
	uint32_t k;

	if (e->frame != m->frame || e->late != m->late ||
	    e->rejected != m->rejected || e->master != m->master ||
	    e->ramp_frames != m->ramp || e->interp != m->interp) {
		printf("FAIL seq %u %s: engine frame %u/%u late %u/%u rejected "
		       "%u/%u master %u/%u ramp %u/%u interp %u/%u\n", seq, when,
		       e->frame, m->frame, e->late, m->late, e->rejected,
		       m->rejected, e->master, m->master, e->ramp_frames, m->ramp,
		       e->interp, m->interp);
		return 0;
	}
	for (k = 0; k < 32; k++) {
		const struct audio_voice *a = &e->voice[k];
		const struct rvoice *b = &m->v[k];

		if (a->vinterp != b->vint || a->active != b->active ||
		    (a->active && (a->pos != b->pos || a->frac != b->frac ||
		                   a->end != b->end || a->backward != b->backward ||
		                   a->step != b->step || a->vol_l != b->vl ||
		                   a->vol_r != b->vr || a->looped != b->looped ||
		                   a->stopping != b->stopping))) {
			printf("FAIL seq %u %s: voice %u active %d/%d pos %u/%u frac "
			       "%u/%u vinterp %u/%u looped %d/%d\n", seq, when, k,
			       a->active, b->active, a->pos, b->pos, a->frac, b->frac,
			       a->vinterp, b->vint, a->looped, b->looped);
			return 0;
		}
	}
	if (fades_in_use(e) != model_fades_in_use(m)) {
		printf("FAIL seq %u %s: fade slots in use %u/%u\n", seq, when,
		       fades_in_use(e), model_fades_in_use(m));
		return 0;
	}
	return 1;
}

/* ---- random corpus ---- */

static uint64_t rng;

static uint32_t rnd(uint32_t n)
{
	uint64_t z = (rng += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return n ? (uint32_t)(z >> 32) % n : 0;
}

static uint8_t g_bank[8192];
static int16_t g_out_e[2 * 4096], g_out_m[2 * 4096], g_out_s[2 * 4096];
static struct audio_voice_cmd g_cmds[1100];

static uint32_t pick_ramp(void)
{
	static const uint32_t r[] = { 1, 2, 3, 7, 16, 64, 100, 256, 511, 512, 0 };

	return r[rnd(sizeof(r) / sizeof(r[0]))];
}

static uint32_t pick_hz(void)
{
	static const uint32_t h[] = { 1, 700, 6000, 22050, 44100, 47999, 48000,
	                              48001, 96000, 192000 };

	switch (rnd(6)) {
	case 0: return 1 + rnd(200000);
	default: return h[rnd(sizeof(h) / sizeof(h[0]))];
	}
}

static uint32_t pick_vol(void)
{
	switch (rnd(6)) {
	case 0: return 0;
	case 1: return 65535;
	case 2: return 1 + rnd(255);
	default: return rnd(65536);
	}
}

/* What the corpus exercised, to catch a generator that went quiet. */
static struct {
	uint32_t renders, cubic_audible, per_voice, tails_cubic, edge_loops,
	         backward, rejected_mode, rejected_cfg, clipped;
} st;

static int cubic_in_use(const struct audio_voice_engine *e)
{
	uint32_t k;

	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		const struct audio_voice *v = &e->voice[k];
		uint32_t mode = v->vinterp ? v->vinterp - 1U : e->interp;

		if (v->active && mode == INTERP_C)
			return 1;
	}
	return 0;
}

static void note_render(const struct audio_voice_engine *e,
                        const int16_t *out, uint32_t frames)
{
	uint32_t k, tails = 0, any = 0, back = 0, loops = 0, per = 0, clipped = 0;

	for (k = 0; k < 2 * frames; k++) {
		any |= out[k] != 0;
		clipped |= out[k] == 32767 || out[k] == -32768;
	}
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++) {
		const struct audio_voice *v = &e->fade[k];

		tails |= v->active && v->vinterp == INTERP_C + 1;
	}
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		const struct audio_voice *v = &e->voice[k];

		back |= v->active && v->backward;
		loops |= v->active && v->looplen != 0 && v->looplen < 4;
		per |= v->vinterp != 0;
	}
	st.renders++;
	st.cubic_audible += cubic_in_use(e) && any;
	st.per_voice += per != 0;
	st.tails_cubic += tails != 0;
	st.edge_loops += loops != 0;
	st.backward += back != 0;
	st.clipped += clipped != 0;
}

/* A bank of random bytes in one of four shapes (full-scale square, noise,
 * walk) so that the curve overshoots. */
static void fill_bank(uint32_t bank_len)
{
	int32_t walk = 0;
	uint32_t kind = rnd(4), i;

	for (i = 0; i < bank_len; i++) {
		uint32_t v = rnd(256);

		walk += (int32_t)(v % 9) - 4;
		g_bank[i] = kind == 0 ? (uint8_t)(v & 1 ? 0x7F : 0x80) :
		            kind == 1 ? (uint8_t)v : (uint8_t)walk;
	}
}

/* The records of one render of a random sequence. with_mode adds MODE and
 * CONFIG interpolation records, valid and not. */
static uint32_t gen_cmds(uint32_t frame, uint32_t frames, uint32_t r,
                         uint32_t nvoices, uint32_t nsamp,
                         const uint32_t *smp_len, int storm, int with_mode,
                         int with_ramp)
{
	uint32_t n = 0, i;
	uint32_t extra = storm ? 150 + rnd(400) : rnd(60);

	if (r == 0) {
		uint32_t v;

		if (with_ramp) {
			g_cmds[n++] = CFGRAMP(frame, pick_ramp());
		}
		for (v = 0; v < nvoices && n + 6 < 300; v++) {
			uint32_t id = rnd(nsamp);
			uint32_t stamp = frame + rnd(40);

			{
				const uint32_t hz = pick_hz();
				const uint32_t vol_l = pick_vol();
				const uint32_t vol_r = pick_vol();

				g_cmds[n++] = SETHZ(stamp, v, hz, vol_l, vol_r);
			}
			if (with_mode && rnd(3) == 0)
				g_cmds[n++] = MODE(stamp, v, rnd(4));
			g_cmds[n++] = mk(stamp, v, AUDIO_VOICE_OP_START, id,
			                 rnd(3) ? 0 : rnd(smp_len[id] + 1), 0);
			if (rnd(2))
				g_cmds[n++] = mk(stamp, v, AUDIO_VOICE_OP_LOOP, 0, 0,
				                 rnd(2) ? smp_len[id] :
				                 rnd(smp_len[id] + 1));
		}
		if (with_mode && rnd(2))
		{
			const uint32_t when = rnd(40);
			const uint32_t sel = 1 + rnd(3);

			g_cmds[n++] = CFGINT(frame + when, sel);
		}
	}
	for (i = 0; i < extra && n < 1000; i++) {
		struct audio_voice_cmd *c = &g_cmds[n++];
		uint32_t pick = rnd(100);
		uint32_t id = rnd(nsamp);

		memset(c, 0, sizeof(*c));
		{
			/* the draws are made in this order, not left to the compiler */
			const uint32_t when = rnd(frames + 1);
			const uint32_t early = rnd(20) == 0 && frame ? 1U : 0U;

			c->stamp = frame + when - early;
		}
		c->voice = (uint8_t)(rnd(30) ? rnd(nvoices) : rnd(40));
		if (pick < (storm ? 50 : 22)) {
			c->op = AUDIO_VOICE_OP_START;
			c->flags = rnd(40) ? id : 200 + rnd(100);
			c->arg0 = rnd(3) ? 0 : rnd(smp_len[id] + 2);
			c->arg1 = rnd(4) ? 0 : rnd(smp_len[id] + 3);
		} else if (pick < 46) {
			c->op = AUDIO_VOICE_OP_SET;
			c->flags = 1 + rnd(7);
			c->arg0 = c->flags & AUDIO_VOICE_RATE_HZ ? pick_hz() :
			          rnd(3) ? 124 + rnd(900) : rnd(10);
			const uint32_t vol_l = pick_vol();
			const uint32_t vol_r = pick_vol();

			c->arg1 = (vol_l << 16) | vol_r;
		} else if (pick < 58) {
			c->op = AUDIO_VOICE_OP_STOP;
		} else if (pick < 68) {
			c->op = AUDIO_VOICE_OP_LOOP;
			c->arg0 = rnd(smp_len[id] + 1);
			c->arg1 = rnd(3) ? rnd(smp_len[id] - c->arg0 + 1) : 0;
			if (rnd(4) == 0 && smp_len[id] > 4) {
				/* short loops: the edges of the taps */
				c->arg0 = rnd(smp_len[id] - 3);
				c->arg1 = 1 + rnd(3);
			}
		} else if (pick < 78) {
			c->op = AUDIO_VOICE_OP_DIRECTION;
			c->flags = (uint16_t)rnd(2);
		} else if (pick < 82) {
			c->op = AUDIO_VOICE_OP_MASTER;
			c->arg0 = rnd(3) == 0 ? 0xFFFF : rnd(2) ? 256 : rnd(600);
		} else if (pick < 86 && with_ramp) {
			c->op = AUDIO_VOICE_OP_CONFIG;
			c->flags = rnd(12) ? 1 : (uint16_t)(rnd(4) << 3);
			c->arg0 = rnd(20) ? pick_ramp() : 513 + rnd(1000);
		} else if (pick < 94 && with_mode) {
			if (rnd(3)) {
				c->op = AUDIO_VOICE_OP_MODE;
				c->flags = rnd(8) ? AUDIO_VOICE_MODE_INTERP :
				           (uint16_t)rnd(8);
				c->arg0 = rnd(6) ? rnd(4) : rnd(10);
				c->arg1 = rnd(8) ? 0 : rnd(3);
			} else {
				c->op = AUDIO_VOICE_OP_CONFIG;
				c->voice = (uint8_t)rnd(40);
				c->flags = rnd(5) ? AUDIO_VOICE_CONFIG_INTERP :
				           (uint16_t)(rnd(2) ? 4 + rnd(4) : 0);
				c->arg0 = rnd(6) ? 1 + rnd(3) : rnd(8);
				c->arg1 = rnd(8) ? 0 : rnd(3);
				/* only forms the model has: no limiter bit */
				if ((c->flags & AUDIO_VOICE_CONFIG_LIMITER) != 0) {
					c->flags &= (uint16_t)~AUDIO_VOICE_CONFIG_LIMITER;
					if (c->flags == 0)
						c->flags = AUDIO_VOICE_CONFIG_INTERP;
				}
			}
		} else {
			c->op = (uint8_t)(10 + rnd(200));
			c->flags = (uint16_t)rnd(65536);
		}
	}
	/* stable insertion sort by stamp */
	for (i = 1; i < n; i++) {
		struct audio_voice_cmd t = g_cmds[i];
		uint32_t j = i;

		while (j && g_cmds[j - 1].stamp > t.stamp) {
			g_cmds[j] = g_cmds[j - 1];
			j--;
		}
		g_cmds[j] = t;
	}
	return n;
}

/* The same render in odd pieces: cuts at the chunk size, one frame, a few
 * frames, and wherever the generator lands. */
static void render_split(struct audio_voice_engine *e, int16_t *out,
                         uint32_t frames, const struct audio_voice_cmd *cmds,
                         size_t n)
{
	uint32_t done = 0;
	size_t used = 0;

	while (done < frames) {
		uint32_t piece;

		switch (rnd(6)) {
		case 0: piece = 1; break;
		case 1: piece = AUDIO_VOICE_MIX_FRAMES; break;
		case 2: piece = AUDIO_VOICE_MIX_FRAMES - 1 + rnd(3); break;
		case 3: piece = 1 + rnd(9); break;
		default: piece = 1 + rnd(400); break;
		}
		if (piece > frames - done)
			piece = frames - done;
		used += audio_voice_render(e, out + 2U * done, piece, cmds + used,
		                           n - used);
		done += piece;
	}
}

static int split_matches(const struct audio_voice_engine *a,
                         const struct audio_voice_engine *b)
{
	uint32_t k;

	if (a->frame != b->frame || a->late != b->late ||
	    a->rejected != b->rejected || a->interp != b->interp)
		return 0;
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++)
		if (a->voice[k].active != b->voice[k].active ||
		    a->voice[k].vinterp != b->voice[k].vinterp ||
		    (a->voice[k].active && (a->voice[k].pos != b->voice[k].pos ||
		                            a->voice[k].frac != b->voice[k].frac ||
		                            a->voice[k].looped != b->voice[k].looped)))
			return 0;
	return 1;
}

static int run_sequence(uint32_t seq, uint64_t seed)
{
	static struct audio_voice_engine e, e2;
	static struct rmodel m;
	uint32_t bank_len = 64 + rnd(sizeof(g_bank) - 64), i, nvoices, nsamp;
	uint32_t interp, storm, frame = 0;
	uint32_t r, renders;
	uint32_t smp_len[8];

	rng = seed + (uint64_t)seq * 0xD1B54A32D192ED03ULL;
	(void)rnd(2);
	interp = rnd(3);
	storm = rnd(5) == 0;
	renders = 2 + rnd(4);
	fill_bank(bank_len);
	audio_voice_init(&e, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_init(&e2, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	r_init(&m, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	e.interp = e2.interp = m.interp = interp;
	nsamp = 1 + rnd(8);
	for (i = 0; i < nsamp; i++) {
		uint32_t fmt = rnd(4), off = rnd(bank_len), len;
		int rc_e, rc_m;

		len = rnd(bank_len - off + 1);
		if (rnd(3) == 0)
			len = bank_len - off;
		rc_e = audio_voice_sample_set(&e, i, off, len, fmt);
		(void)audio_voice_sample_set(&e2, i, off, len, fmt);
		rc_m = r_sample_set(&m, i, off, len, fmt);
		if (rc_e != rc_m) {
			printf("FAIL seq %u sample_set rc\n", seq);
			return 0;
		}
		smp_len[i] = rc_e == 0 ? len : 0;
	}
	nvoices = storm ? 32 : 1 + rnd(32);

	for (r = 0; r < renders; r++) {
		uint32_t frames = 1 + rnd(storm ? 3000 : 2000);
		uint32_t n = gen_cmds(frame, frames, r, nvoices, nsamp, smp_len,
		                      storm, 1, 1);
		size_t ue, um;

		memset(g_out_e, 0x5A, sizeof(g_out_e));
		memset(g_out_m, 0x5A, sizeof(g_out_m));
		memset(g_out_s, 0x5A, sizeof(g_out_s));
		ue = audio_voice_render(&e, g_out_e, frames, g_cmds, n);
		um = r_render(&m, g_out_m, frames, g_cmds, n);
		render_split(&e2, g_out_s, frames, g_cmds, n);
		if (ue != um) {
			printf("FAIL seq %u render %u consumed %zu/%zu\n", seq, r, ue,
			       um);
			return 0;
		}
		if (memcmp(g_out_e, g_out_m, sizeof(g_out_e)) != 0) {
			uint32_t f;

			for (f = 0; f < 2 * frames; f++)
				if (g_out_e[f] != g_out_m[f])
					break;
			printf("FAIL seq %u render %u (%u frames, %u records, default "
			       "%u) differs at frame %u side %u: engine %d model %d\n",
			       seq, r, frames, n, interp, f / 2, f & 1, g_out_e[f],
			       g_out_m[f]);
			return 0;
		}
		if (memcmp(g_out_e, g_out_s, sizeof(g_out_e)) != 0 ||
		    !split_matches(&e, &e2)) {
			uint32_t f;

			for (f = 0; f < 2 * frames; f++)
				if (g_out_e[f] != g_out_s[f])
					break;
			printf("FAIL seq %u render %u: pieces differ from the whole "
			       "render at frame %u (%d vs %d)\n", seq, r, f / 2,
			       g_out_e[f], g_out_s[f]);
			return 0;
		}
		if (!same_state(&e, &m, seq, "after render"))
			return 0;
		note_render(&e, g_out_e, frames);
		frame += frames;
	}
	return 1;
}

/* ---- explicit linear equals session linear ---- */

static int run_equivalence(uint32_t seq, uint64_t seed)
{
	static struct audio_voice_engine a, b, c;
	uint32_t bank_len = 64 + rnd(sizeof(g_bank) - 64), i, nvoices, nsamp;
	uint32_t frame = 0, r, renders, storm;
	uint32_t smp_len[8];

	rng = seed + 0x1234567ULL + (uint64_t)seq * 0xD1B54A32D192ED03ULL;
	storm = rnd(4) == 0;
	renders = 2 + rnd(3);
	fill_bank(bank_len);
	audio_voice_init(&a, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_init(&b, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_init(&c, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	a.interp = INTERP_L;
	b.interp = INTERP_N;
	c.interp = INTERP_N;
	nsamp = 1 + rnd(8);
	for (i = 0; i < nsamp; i++) {
		uint32_t fmt = rnd(4), off = rnd(bank_len);
		uint32_t len = rnd(bank_len - off + 1);

		audio_voice_sample_set(&a, i, off, len, fmt);
		audio_voice_sample_set(&b, i, off, len, fmt);
		audio_voice_sample_set(&c, i, off, len, fmt);
		smp_len[i] = len;
	}
	nvoices = storm ? 32 : 1 + rnd(32);
	for (r = 0; r < renders; r++) {
		uint32_t frames = 1 + rnd(2000), n, k, j;
		static struct audio_voice_cmd pre[40];
		static int16_t oa[2 * 2048], ob[2 * 2048], oc[2 * 2048];

		n = gen_cmds(frame, frames, r, nvoices, nsamp, smp_len, storm, 0, 1);
		/* every record has a stamp of frame or later (or frame - 1 late) */
		audio_voice_render(&a, oa, frames, g_cmds, n);
		k = 0;
		if (r == 0) {
			for (j = 0; j < AUDIO_VOICE_MAX_VOICES; j++)
				pre[k++] = MODE(0, j, AUDIO_VOICE_INTERP_MODE_LINEAR);
		}
		/* prefix records, then the rest: the prefix is stamped 0, never
		 * after any record of the run */
		{
			static struct audio_voice_cmd all[1200];

			memcpy(all, pre, k * sizeof(pre[0]));
			memcpy(all + k, g_cmds, n * sizeof(g_cmds[0]));
			audio_voice_render(&b, ob, frames, all, k + n);
		}
		{
			static struct audio_voice_cmd all[1200];

			if (r == 0) {
				all[0] = CFGINT(0, AUDIO_VOICE_INTERP_MODE_LINEAR);
				memcpy(all + 1, g_cmds, n * sizeof(g_cmds[0]));
				audio_voice_render(&c, oc, frames, all, n + 1);
			} else {
				audio_voice_render(&c, oc, frames, g_cmds, n);
			}
		}
		if (memcmp(oa, ob, 4U * frames) != 0 || a.rejected != b.rejected) {
			printf("FAIL equivalence %u render %u: explicit per-voice linear "
			       "differs from session linear\n", seq, r);
			return 0;
		}
		if (memcmp(oa, oc, 4U * frames) != 0 || a.rejected != c.rejected) {
			printf("FAIL equivalence %u render %u: default linear set by "
			       "CONFIG differs from session linear\n", seq, r);
			return 0;
		}
		frame += frames;
	}
	return 1;
}

/* ---- real arithmetic ---- */

/* Catmull-Rom from its weights in long double. */
static long double real_cubic(long double a, long double b, long double c,
                              long double d, long double t)
{
	const long double t2 = t * t, t3 = t2 * t;

	return 0.5L * (a * (-t3 + 2 * t2 - t) + b * (3 * t3 - 5 * t2 + 2) +
	               c * (-3 * t3 + 4 * t2 + t) + d * (t3 - t2));
}

static void put16(uint8_t *p, int v)
{
	p[0] = (uint8_t)((uint16_t)v >> 8);
	p[1] = (uint8_t)v;
}

static void test_sine(void)
{
	static const uint32_t hzs[] = { 4800, 12000, 24000, 48000, 81600, 187200 };
	static struct audio_voice_engine e;
	static int16_t out[2 * 3000];
	enum { N = 256, FR = 3000 };
	int16_t s[N];
	double worst_ref = 0, worst_sine_c = 0, worst_sine_l = 0;
	uint32_t h, i, k, mode;
	const long double pi = 3.14159265358979323846264338327950288L;

	for (i = 0; i < N; i++) {
		s[i] = (int16_t)lroundl(20000.0L * sinl(2 * pi * i / N));
		put16(g_bank + 2 * i, s[i]);
	}
	for (h = 0; h < sizeof(hzs) / sizeof(hzs[0]); h++) {
		const uint32_t step = (uint32_t)(((uint64_t)hzs[h] << 16) / 48000);

		for (mode = 0; mode < 3; mode++) {
			struct audio_voice_cmd c[5];
			double worst_r = 0, worst_s = 0;

			audio_voice_init(&e, g_bank, 2 * N, AUDIO_VOICE_CLOCK_PAL);
			audio_voice_sample_set(&e, 0, 0, 2 * N, AUDIO_VOICE_FMT_S16);
			/* mode 0: cubic session; 1: cubic voice in a nearest
			 * session; 2: linear session, for the comparison */
			e.interp = mode == 0 ? INTERP_C : mode == 1 ? INTERP_N : INTERP_L;
			c[0] = SETHZ(0, 0, hzs[h], 65535, 65535);
			c[1] = mk(0, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
			c[2] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, 2 * N);
			c[3] = MODE(0, 0, mode == 1 ? AUDIO_VOICE_INTERP_MODE_CUBIC :
			            AUDIO_VOICE_INTERP_MODE_INHERIT);
			audio_voice_render(&e, out, FR, c, 4);
			for (k = 0; k < FR; k++) {
				const uint64_t u = (uint64_t)k * step;
				const uint32_t idx = (uint32_t)(u >> 16) % N;
				const long double t = (long double)(u & 0xFFFF) / 65536.0L;
				const int first = u < ((uint64_t)N << 16);
				const long double a = (first && idx == 0) ? s[0] :
				                      s[(idx + N - 1) % N];
				const long double rc = real_cubic(a, s[idx], s[(idx + 1) % N],
				                                  s[(idx + 2) % N], t);
				const long double ideal = 20000.0L * sinl(2 * pi *
					((long double)(u % ((uint64_t)N << 16)) / 65536.0L) / N);
				const double eref = fabs((double)(out[2 * k] -
					rc * 65535.0L / 65536.0L));
				const double esin = fabs((double)(out[2 * k] -
					ideal * 65535.0L / 65536.0L));

				if (mode != 2 && eref > worst_r)
					worst_r = eref;
				if (esin > worst_s && (!first || idx != 0))
					worst_s = esin;
			}
			if (mode != 2) {
				if (worst_r > worst_ref)
					worst_ref = worst_r;
				if (worst_s > worst_sine_c && hzs[h] <= 48000)
					worst_sine_c = worst_s;
			} else if (worst_s > worst_sine_l && hzs[h] <= 48000) {
				worst_sine_l = worst_s;
			}
			CHECK(mode == 2 || worst_r <= 2.0,
			      "sine %u Hz: cubic differs from the real curve by %.3f",
			      hzs[h], worst_r);
		}
	}
	CHECK(worst_ref <= 2.0, "sine: worst error against the real curve %.3f",
	      worst_ref);
	CHECK(worst_sine_c < worst_sine_l,
	      "sine: cubic (%.2f) is not closer to the sine than linear (%.2f)",
	      worst_sine_c, worst_sine_l);
	printf("  sine: worst difference from the real Catmull-Rom curve %.3f "
	       "LSB; from the sine itself at up to 1 sample per frame: cubic "
	       "%.2f, linear %.2f\n", worst_ref, worst_sine_c, worst_sine_l);
}

/* The fixed point curve of the model (the engine's, as the other tests show)
 * against the real curve over random taps, full-scale ones included. */
static void test_curve_error(void)
{
	double worst = 0, sum = 0;
	uint32_t i, k;

	rng = 0xC0FFEE;
	for (i = 0; i < 4000000U; i++) {
		int32_t tap[4];
		uint32_t t;
		long double real;
		double d;

		for (k = 0; k < 4; k++)
			tap[k] = rnd(3) == 0 ? (rnd(2) ? 32767 : -32768) :
			         (int32_t)rnd(65536) - 32768;
		t = rnd(4) == 0 ? (rnd(2) ? 0xFFFF : 0x8000) : rnd(65536);
		real = real_cubic(tap[0], tap[1], tap[2], tap[3], t / 65536.0L);
		d = fabs((double)(r_cubic(tap[0], tap[1], tap[2], tap[3], t) - real));
		sum += d;
		if (d > worst)
			worst = d;
		CHECK(r_cubic(tap[0], tap[1], tap[2], tap[3], t) >= -40961 &&
		      r_cubic(tap[0], tap[1], tap[2], tap[3], t) <= 40961,
		      "curve: a sample is beyond 40961");
	}
	CHECK(worst <= 1.0, "curve: worst difference from the real curve %.4f",
	      worst);
	printf("  curve: fixed point against real arithmetic over 4000000 random "
	       "taps: worst %.4f LSB, mean %.4f\n", worst, sum / 4000000.0);
}

/* ---- unrolled streams ---- */

struct ucfg {
	const char *name;
	uint32_t len, off, flen, ls, ll;
	int backward;
};

enum { WMAX = 6000 };
static int32_t g_walk[WMAX + 8]; /* g_walk[0] is the frame before the first */

/* The frames a voice plays at one frame per step, in order: the first pass
 * and then the loop unrolled; backward, down from the offset and then the
 * loop unrolled downward (a loop of a backward voice starts at 0). */
static void build_walk(const struct ucfg *c, uint32_t *first_len)
{
	uint32_t n = 1, i;
	const uint32_t end = c->flen ? c->off + c->flen : c->len;
	const uint32_t le = c->ls + c->ll;

	if (!c->backward) {
		g_walk[0] = c->off ? (int32_t)c->off - 1 : (int32_t)c->off;
		for (i = c->off; i < end; i++)
			g_walk[n++] = (int32_t)i;
		*first_len = n - 1;
		if (c->ll) {
			while (n < WMAX)
				for (i = c->ls; i < le && n < WMAX; i++)
					g_walk[n++] = (int32_t)i;
		} else {
			while (n < WMAX)
			{
				g_walk[n] = g_walk[n - 1];
				n++;
			}
		}
	} else {
		g_walk[0] = c->off + 1 < c->len ? (int32_t)c->off + 1 :
		            (int32_t)c->off;
		for (i = c->off + 1; i-- > c->ls;)
			g_walk[n++] = (int32_t)i;
		*first_len = n - 1;
		if (c->ll) {
			while (n < WMAX)
				for (i = le; i-- > c->ls && n < WMAX;)
					g_walk[n++] = (int32_t)i;
		} else {
			while (n < WMAX)
			{
				g_walk[n] = g_walk[n - 1];
				n++;
			}
		}
	}
}

static void test_unrolled_one(const struct ucfg *c, uint32_t hz)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 800];
	static int16_t smp[64];
	enum { FR = 800 };
	struct audio_voice_cmd cmd[6];
	struct rvoice pv;
	uint32_t i, k, n = 0, first_len, u = 1, bad = 0;
	const uint32_t step = (uint32_t)(((uint64_t)hz << 16) / 48000);

	for (i = 0; i < c->len; i++) {
		smp[i] = (int16_t)((int32_t)((i * 7919U + 13U) % 65536U) - 32768);
		if (i % 5 == 0)
			smp[i] = (i & 1) ? 32767 : -32768;
		put16(g_bank + 2 * i, smp[i]);
	}
	build_walk(c, &first_len);
	audio_voice_init(&e, g_bank, 2 * c->len, AUDIO_VOICE_CLOCK_PAL);
	e.interp = INTERP_C;
	audio_voice_sample_set(&e, 0, 0, 2 * c->len, AUDIO_VOICE_FMT_S16);
	cmd[n++] = SETHZ(0, 0, hz, 65535, 65535);
	cmd[n++] = mk(0, 0, AUDIO_VOICE_OP_START, 0, 2 * c->off, 2 * c->flen);
	if (c->ll)
		cmd[n++] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 2 * c->ls, 2 * c->ll);
	if (c->backward)
		cmd[n++] = mk(0, 0, AUDIO_VOICE_OP_DIRECTION,
		              AUDIO_VOICE_DIR_BACKWARD, 0, 0);
	audio_voice_render(&e, out, FR, cmd, n);

	memset(&pv, 0, sizeof(pv));
	pv.active = 1;
	pv.backward = c->backward;
	pv.pos = c->off;
	pv.end = c->flen ? c->off + c->flen : c->len;
	pv.ls = c->ls;
	pv.ll = c->ll;
	pv.step = step;
	for (k = 0; k < FR; k++) {
		int32_t want = 0;
		uint32_t old_pos = pv.pos, old_end = pv.end;
		uint64_t cur = (uint64_t)pv.pos << 16 | pv.frac;

		if (pv.active) {
			const int32_t a = smp[g_walk[u - 1]], b = smp[g_walk[u]];
			const int32_t cc = smp[g_walk[u + 1]], d = smp[g_walk[u + 2]];

			if ((uint32_t)g_walk[u] != pv.pos) {
				CHECK(0, "%s %u Hz frame %u: walk says frame %d, position %u",
				      c->name, hz, k, g_walk[u], pv.pos);
				return;
			}
			want = (int32_t)(((int64_t)r_cubic(a, b, cc, d, pv.frac) *
			                  65535) >> 16);
			want = want > 32767 ? 32767 : want < -32768 ? -32768 : want;
			/* advance the model and the walk position */
			{
				int wrapped;

				if (!c->backward)
					wrapped = (uint32_t)((cur + step) >> 16) >= old_end;
				else
					wrapped = cur < step || (uint32_t)((cur - step) >> 16) <
					          c->ls;
				r_advance(&pv);
				if (!c->backward)
					u += wrapped ? (old_end - old_pos) + (pv.pos - c->ls) :
					     pv.pos - old_pos;
				else
					u += wrapped ? (old_pos - c->ls) + 1 :
					     old_pos - pv.pos;
			}
		}
		if (out[2 * k] != want || out[2 * k + 1] != want) {
			if (bad++ < 3)
				CHECK(0, "%s %u Hz (step %u): frame %u engine %d/%d "
				      "reference %d (position %u.%u)", c->name, hz, step,
				      k, out[2 * k], out[2 * k + 1], want, old_pos,
				      (unsigned)(cur & 0xFFFF));
		}
	}
	(void)first_len;
}

static void test_unrolled(void)
{
	static const struct ucfg cfg[] = {
		{ "forward plain", 24, 0, 0, 0, 0, 0 },
		{ "forward from 5", 24, 5, 0, 0, 0, 0 },
		{ "forward first pass 5..12", 24, 5, 7, 0, 0, 0 },
		{ "forward loop 0..24", 24, 0, 0, 0, 24, 0 },
		{ "forward loop 8..24", 24, 0, 0, 8, 16, 0 },
		{ "forward loop 8..16, first pass 0..16", 24, 0, 16, 8, 8, 0 },
		{ "forward loop of 1", 24, 0, 11, 10, 1, 0 },
		{ "forward loop of 2", 24, 0, 12, 10, 2, 0 },
		{ "forward loop of 3 at the end", 24, 0, 0, 21, 3, 0 },
		{ "forward from 22 into loop 8..24", 24, 22, 0, 8, 16, 0 },
		{ "forward one-frame sample", 1, 0, 0, 0, 0, 0 },
		{ "forward two-frame loop sample", 2, 0, 0, 0, 2, 0 },
		{ "backward plain from 20", 24, 20, 0, 0, 0, 1 },
		{ "backward plain from 23", 24, 23, 0, 0, 0, 1 },
		{ "backward plain from 3", 24, 3, 0, 0, 0, 1 },
		{ "backward loop 0..24 from 20", 24, 20, 0, 0, 24, 1 },
		{ "backward loop 0..10 from 9", 24, 9, 0, 0, 10, 1 },
		{ "backward loop of 1", 24, 0, 0, 0, 1, 1 },
		{ "backward loop of 2", 24, 1, 0, 0, 2, 1 },
		{ "backward loop of 3", 24, 12, 0, 0, 3, 1 },
		{ "backward two-frame loop sample", 2, 1, 0, 0, 2, 1 },
	};
	static const uint32_t hzs[] = { 14400, 24000, 36000, 48000, 60000, 100800,
	                                187200 };
	uint32_t i, h;

	for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
		for (h = 0; h < sizeof(hzs) / sizeof(hzs[0]); h++) {
			const uint32_t whole = hzs[h] / 48000;

			/* the wrap lands inside the loop only when the overshoot is
			 * shorter than the loop; that rule is the position model's */
			if (cfg[i].ll != 0 && whole >= cfg[i].ll)
				continue;
			test_unrolled_one(&cfg[i], hzs[h]);
		}
}

/* ---- the four formats ---- */

static void test_formats(void)
{
	static struct audio_voice_engine e8, e16, s8, s16, ms8, ms16;
	static int16_t o[6][2 * 700];
	enum { N = 20, FR = 700 };
	int8_t x[N], y[N];
	struct audio_voice_cmd c[5];
	uint32_t i, f, k;
	struct audio_voice_engine *eng[6] = { &e8, &e16, &s8, &s16, &ms8, &ms16 };
	static uint8_t banks[6][4 * N];

	for (i = 0; i < N; i++) {
		x[i] = (int8_t)((i * 37 + 11) % 256 - 128);
		y[i] = (int8_t)((i * 91 + 3) % 256 - 128);
		if (i % 4 == 1)
			x[i] = 127;
		if (i % 4 == 2)
			x[i] = 127;
		if (i % 4 == 3)
			x[i] = -128;
		if (i % 4 == 0)
			x[i] = -128;
	}
	for (i = 0; i < N; i++) {
		banks[0][i] = (uint8_t)x[i];                    /* S8 */
		put16(banks[1] + 2 * i, x[i] * 256);            /* S16 */
		banks[2][2 * i] = (uint8_t)x[i];                /* S8S */
		banks[2][2 * i + 1] = (uint8_t)y[i];
		put16(banks[3] + 4 * i, x[i] * 256);            /* S16S */
		put16(banks[3] + 4 * i + 2, y[i] * 256);
		banks[4][2 * i] = (uint8_t)x[i];                /* S8S, left = right */
		banks[4][2 * i + 1] = (uint8_t)x[i];
		put16(banks[5] + 4 * i, x[i] * 256);            /* S16S, left = right */
		put16(banks[5] + 4 * i + 2, x[i] * 256);
	}
	{
		static const uint32_t fmts[6] = { 0, 1, 2, 3, 2, 3 };

		for (f = 0; f < 6; f++) {
			const uint32_t shift = fmts[f] == 3 ? 2 : fmts[f] == 0 ? 0 : 1;

			audio_voice_init(eng[f], banks[f], N << shift,
			                 AUDIO_VOICE_CLOCK_PAL);
			audio_voice_sample_set(eng[f], 0, 0, N << shift, fmts[f]);
			eng[f]->interp = f & 1 ? INTERP_C : INTERP_N;
			c[0] = SETHZ(0, 0, 17000, 40000, 12345);
			c[1] = mk(0, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
			c[2] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, N << shift);
			c[3] = MODE(0, 0, AUDIO_VOICE_INTERP_MODE_CUBIC);
			audio_voice_render(eng[f], o[f], FR, c, 4);
		}
	}
	CHECK(memcmp(o[0], o[1], sizeof(o[0])) == 0,
	      "formats: S8 and S16 (the same samples times 256) differ");
	CHECK(memcmp(o[2], o[3], sizeof(o[0])) == 0,
	      "formats: S8S and S16S differ");
	CHECK(memcmp(o[4], o[5], sizeof(o[0])) == 0,
	      "formats: S8S and S16S with equal sides differ");
	CHECK(memcmp(o[0], o[4], sizeof(o[0])) == 0,
	      "formats: a mono voice differs from a stereo one with equal sides");
	{
		int same = 0, differ = 0;

		for (k = 0; k < FR; k++) {
			same += o[2][2 * k] == o[0][2 * k];
			differ += o[2][2 * k + 1] != o[0][2 * k + 1];
		}
		CHECK(same == FR, "formats: the left side of a stereo sample is not "
		      "the mono sample (%d of %d equal)", same, FR);
		CHECK(differ > FR / 4, "formats: the right side of a stereo sample "
		      "is the mono sample (%d differ)", differ);
	}
	{
		/* the cubic differs from nearest and from linear on this signal */
		static struct audio_voice_engine l;
		static int16_t ol[2 * 700];
		int dn = 0, dl = 0;

		audio_voice_init(&l, banks[0], N, AUDIO_VOICE_CLOCK_PAL);
		audio_voice_sample_set(&l, 0, 0, N, AUDIO_VOICE_FMT_S8);
		l.interp = INTERP_L;
		c[0] = SETHZ(0, 0, 17000, 40000, 12345);
		c[1] = mk(0, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
		c[2] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, N);
		audio_voice_render(&l, ol, FR, c, 3);
		for (k = 0; k < FR; k++)
			dl += ol[2 * k] != o[0][2 * k];
		CHECK(dl > FR / 2, "formats: cubic equals linear (%d differ)", dl);
		(void)dn;
	}
}

/* ---- overshoot, master and limiter ---- */

static long double limiter_curve(long double x)
{
	const long double a = fabsl(x);
	long double v;

	if (a <= 24576.0L)
		return x;
	v = 24576.0L + 8191.0L * (a - 24576.0L) / (8191.0L + (a - 24576.0L));
	return x < 0 ? -v : v;
}

/* 32 voices on a sample whose middle two frames are at one rail and whose
 * outer two are at the other, so that the curve between the middle frames
 * overshoots by a quarter; restarts hand 32 tails over. Compared with the
 * model, in both signs, at the largest gain and master and others. */
static void test_overshoot(void)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	static int16_t oe[2 * 400], om[2 * 400];
	static const uint32_t masters[] = { 256, 0xFFFF, 0x400, 0x3FF, 0x200, 600,
	                                    0xFF, 1 };
	uint32_t mi, sign, rampi, v, n;
	struct audio_voice_cmd c[300];

	for (sign = 0; sign < 2; sign++) {
		/* [-32768, 32767, 32767, -32768] and its negative */
		const int hi = sign ? -32768 : 32767, lo = sign ? 32767 : -32768;
		const int pat[8] = { lo, hi, hi, lo, lo, hi, hi, lo };

		for (v = 0; v < 8; v++)
			put16(g_bank + 2 * v, pat[v]);
		for (rampi = 0; rampi < 3; rampi++) {
			for (mi = 0; mi < sizeof(masters) / sizeof(masters[0]); mi++) {
				const uint32_t ramp = rampi == 0 ? 0 : rampi == 1 ? 4 : 512;
				int bad = 0;
				uint32_t k;

				n = 0;
				audio_voice_init(&e, g_bank, 16, AUDIO_VOICE_CLOCK_PAL);
				r_init(&m, g_bank, 16, AUDIO_VOICE_CLOCK_PAL);
				audio_voice_sample_set(&e, 0, 0, 16, AUDIO_VOICE_FMT_S16);
				r_sample_set(&m, 0, 0, 16, AUDIO_VOICE_FMT_S16);
				e.interp = m.interp = INTERP_C;
				c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, masters[mi], 0);
				c[n++] = CFGRAMP(0, ramp);
				for (v = 0; v < 32; v++) {
					c[n++] = SETHZ(0, v, 24000, 65535, 65535);
					c[n++] = mk(0, v, AUDIO_VOICE_OP_START, 0, 2, 0);
					c[n++] = mk(0, v, AUDIO_VOICE_OP_LOOP, 0, 2, 4);
				}
				/* 32 restarts: 32 tails when ramping */
				for (v = 0; v < 32; v++) {
					c[n++] = mk(40, v, AUDIO_VOICE_OP_START, 0, 2, 0);
					c[n++] = mk(40, v, AUDIO_VOICE_OP_LOOP, 0, 2, 4);
				}
				audio_voice_render(&e, oe, 400, c, n);
				r_render(&m, om, 400, c, n);
				for (k = 0; k < 800; k++)
					bad += oe[k] != om[k];
				CHECK(bad == 0, "overshoot sign %u ramp %u master %#x: %d "
				      "samples differ from the model", sign, ramp,
				      masters[mi], bad);
				/* the sound reaches the rail it should */
				{
					int rail = 0;

					for (k = 0; k < 400; k++)
						rail |= oe[2 * k] == (sign ? -32768 : 32767) &&
						        masters[mi] >= 256;
					CHECK(masters[mi] < 256 || rail || ramp == 512,
					      "overshoot sign %u: no sample at the rail", sign);
				}
			}
		}
	}
}

/* The limiter on with 32 voices and 32 tails of an overshooting sample at
 * the largest and some other masters, in both signs: the output against the
 * reference curve of the sum the model computes in 64 bits (within 2, as for
 * the other modes) and never at a rail. A ramp keeps a restart's tail and
 * the new sound complementary, so this reaches about 32 voices' worth;
 * test_limiter_64() reaches 64 full contributions. */
static void test_limiter_cubic(void)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	static int16_t o[2 * 400], om[2 * 400];
	static int64_t xl[400], xr[400];
	static const uint32_t masters[] = { 256, 0xFFFF, 0x400, 600 };
	uint32_t mi, sign, v, k;
	int64_t peak = 0;

	for (mi = 0; mi < sizeof(masters) / sizeof(masters[0]); mi++) {
		for (sign = 0; sign < 2; sign++) {
			const int hi = sign ? -32768 : 32767, lo = sign ? 32767 : -32768;
			const int pat[4] = { lo, hi, hi, lo };
			struct audio_voice_cmd c[300], cm[300];
			uint32_t n = 0, nm = 0;
			double worst = 0;

			for (v = 0; v < 4; v++)
				put16(g_bank + 2 * v, pat[v]);
			audio_voice_init(&e, g_bank, 8, AUDIO_VOICE_CLOCK_PAL);
			audio_voice_sample_set(&e, 0, 0, 8, AUDIO_VOICE_FMT_S16);
			r_init(&m, g_bank, 8, AUDIO_VOICE_CLOCK_PAL);
			r_sample_set(&m, 0, 0, 8, AUDIO_VOICE_FMT_S16);
			e.interp = m.interp = INTERP_C;
			c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, masters[mi], 0);
			c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
			            AUDIO_VOICE_CONFIG_RAMP | AUDIO_VOICE_CONFIG_LIMITER,
			            4, 1);
			cm[nm++] = c[0];
			cm[nm++] = CFGRAMP(0, 4);
			for (v = 0; v < 32; v++) {
				c[n++] = SETHZ(0, v, 24000, 65535, 65535);
				c[n++] = mk(0, v, AUDIO_VOICE_OP_START, 0, 2, 0);
				c[n++] = mk(0, v, AUDIO_VOICE_OP_LOOP, 0, 2, 4);
			}
			for (v = 0; v < 32; v++) {
				c[n++] = mk(40, v, AUDIO_VOICE_OP_START, 0, 2, 0);
				c[n++] = mk(40, v, AUDIO_VOICE_OP_LOOP, 0, 2, 4);
			}
			memcpy(cm + nm, c + 2, (n - 2) * sizeof(c[0]));
			nm += n - 2;
			audio_voice_render(&e, o, 400, c, n);
			g_rec_l = xl;
			g_rec_r = xr;
			g_rec_cap = 400;
			r_render(&m, om, 400, cm, nm);
			g_rec_l = g_rec_r = NULL;
			for (k = 0; k < 400; k++) {
				const long double wl = limiter_curve((long double)xl[k]);
				const long double cl = wl > 32766.0L ? 32766.0L :
				                       wl < -32766.0L ? -32766.0L : wl;
				const double d = fabs((double)(o[2 * k] - cl));

				if (d > worst)
					worst = d;
				if (llabs((long long)xl[k]) > peak)
					peak = llabs((long long)xl[k]);
				CHECK(o[2 * k] <= 32766 && o[2 * k] >= -32766 &&
				      o[2 * k + 1] <= 32766 && o[2 * k + 1] >= -32766,
				      "limiter sign %u master %#x frame %u: %d at a rail",
				      sign, masters[mi], k, o[2 * k]);
			}
			CHECK(worst <= 2.0, "limiter sign %u master %#x: %.2f from the "
			      "reference curve", sign, masters[mi], worst);
		}
	}
	CHECK(peak > 200000000, "limiter: the sums stay shallow (peak %lld)",
	      (long long)peak);
	printf("  limiter: cubic sums up to %lld after the master (the old "
	       "bound of the other modes is 536854528)\n", (long long)peak);
}

/*
 * 32 voices and 32 tails, all at full gain and in phase on the full
 * overshoot, with the limiter on and the largest master: the case of 64 full
 * cubic contributions. The ramp is 512 frames while the voices rise and then
 * run at full gain; at one stamp every voice restarts (32 tails at full
 * gain, fading over 512 frames), the ramp is set to 0 and the volume is set
 * again, which takes effect at once, so the new sounds are at full gain
 * while the tails still are too. The sum after the master then goes beyond
 * 2^29, the end of the limiter table, up to the 64 * 40961 * 65535 / 256
 * the static assertions in audio_voice.c bound. The output stays within 2 of
 * the reference curve of the model's 64-bit sum and inside the rails.
 */
static void test_limiter_64(void)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	static int16_t o[2 * 800], om[2 * 800];
	static int64_t xl[800], xr[800];
	uint32_t sign, v, k;
	int64_t peak = 0;

	for (sign = 0; sign < 2; sign++) {
		const int hi = sign ? -32768 : 32767, lo = sign ? 32767 : -32768;
		const int pat[4] = { lo, hi, hi, lo };
		struct audio_voice_cmd c[400], cm[400];
		uint32_t n = 0, nm = 0;
		double worst = 0;

		for (v = 0; v < 4; v++)
			put16(g_bank + 2 * v, pat[v]);
		audio_voice_init(&e, g_bank, 8, AUDIO_VOICE_CLOCK_PAL);
		audio_voice_sample_set(&e, 0, 0, 8, AUDIO_VOICE_FMT_S16);
		r_init(&m, g_bank, 8, AUDIO_VOICE_CLOCK_PAL);
		r_sample_set(&m, 0, 0, 8, AUDIO_VOICE_FMT_S16);
		e.interp = m.interp = INTERP_C;
		c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0xFFFF, 0);
		c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
		            AUDIO_VOICE_CONFIG_RAMP | AUDIO_VOICE_CONFIG_LIMITER,
		            512, 1);
		cm[nm++] = c[0];
		cm[nm++] = CFGRAMP(0, 512);
		for (v = 0; v < 32; v++) {
			c[n++] = SETHZ(0, v, 24000, 65535, 65535);
			c[n++] = mk(0, v, AUDIO_VOICE_OP_START, 0, 0, 0);
			c[n++] = mk(0, v, AUDIO_VOICE_OP_LOOP, 0, 0, 8);
		}
		for (v = 0; v < 32; v++) {
			c[n++] = mk(600, v, AUDIO_VOICE_OP_START, 0, 0, 0);
			c[n++] = mk(600, v, AUDIO_VOICE_OP_LOOP, 0, 0, 8);
		}
		c[n++] = mk(600, 0, AUDIO_VOICE_OP_CONFIG,
		            AUDIO_VOICE_CONFIG_RAMP | AUDIO_VOICE_CONFIG_LIMITER,
		            0, 1);
		for (v = 0; v < 32; v++)
			c[n++] = SETHZ(600, v, 24000, 65535, 65535);
		memcpy(cm + nm, c + 2, 96 * sizeof(c[0]));
		nm += 96;
		for (v = 0; v < 32; v++) {
			cm[nm++] = c[98 + 2 * v];
			cm[nm++] = c[99 + 2 * v];
		}
		cm[nm++] = CFGRAMP(600, 0);
		for (v = 0; v < 32; v++)
			cm[nm++] = SETHZ(600, v, 24000, 65535, 65535);
		audio_voice_render(&e, o, 800, c, n);
		g_rec_l = xl;
		g_rec_r = xr;
		g_rec_cap = 800;
		r_render(&m, om, 800, cm, nm);
		g_rec_l = g_rec_r = NULL;
		for (k = 0; k < 800; k++) {
			const long double wl = limiter_curve((long double)xl[k]);
			const long double cl = wl > 32766.0L ? 32766.0L :
			                       wl < -32766.0L ? -32766.0L : wl;
			const double d = fabs((double)(o[2 * k] - cl));

			if (d > worst)
				worst = d;
			if (llabs((long long)xl[k]) > peak)
				peak = llabs((long long)xl[k]);
			CHECK(o[2 * k] <= 32766 && o[2 * k] >= -32766 &&
			      o[2 * k + 1] <= 32766 && o[2 * k + 1] >= -32766,
			      "limiter 64 sign %u frame %u: %d at a rail", sign, k,
			      o[2 * k]);
		}
		CHECK(worst <= 2.0, "limiter 64 sign %u: %.2f from the reference "
		      "curve", sign, worst);
	}
	CHECK(peak > (int64_t)1 << 29,
	      "limiter 64: the sum does not pass the table end (peak %lld)",
	      (long long)peak);
	CHECK(peak <= (int64_t)64 * 40961 * 65535 / 256,
	      "limiter 64: the sum is beyond the static bound (peak %lld)",
	      (long long)peak);
	printf("  limiter: 64 full cubic contributions reach %lld after the "
	       "master (table end %lld, static bound %lld)\n", (long long)peak,
	       (long long)1 << 29, (long long)64 * 40961 * 65535 / 256);
}

/* The sum after the master of a run, per frame, from the model: the
 * reference input of the limiter curve. */
static void model_sums(const uint8_t *bank, uint32_t bank_len, uint32_t master,
                       const struct audio_voice_cmd *c, size_t n,
                       uint32_t frames, int64_t *xl)
{
	static struct rmodel m;
	uint32_t k;

	r_init(&m, bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	r_sample_set(&m, 0, 0, bank_len, AUDIO_VOICE_FMT_S16);
	m.interp = INTERP_C;
	m.master = master;
	/* sums are taken before the clamp: drive the model with unity-gain
	 * voices and read the unclamped value through a wide master */
	{
		size_t used = 0;

		for (k = 0; k < frames; k++) {
			int64_t sum = 0;
			uint32_t j;

			while (used < n && c[used].stamp <= k) {
				if (c[used].op != AUDIO_VOICE_OP_MASTER &&
				    c[used].op != AUDIO_VOICE_OP_CONFIG)
					r_apply(&m, &c[used]);
				used++;
			}
			for (j = 0; j < 32; j++) {
				struct rvoice *v = &m.v[j];
				int32_t sl, sr;

				if (!v->active || v->step == 0)
					continue;
				r_read(&m, v, &sl, &sr);
				sum += ((int64_t)sl * (int64_t)v->vl) >> 16;
				r_advance(v);
			}
			xl[k] = (sum * (int64_t)master) >> 8;
		}
	}
}

static void test_limiter_curve(void)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 600];
	static int64_t xl[600];
	static uint8_t pat[16];
	static const uint32_t masters[] = { 0xFFFF, 0x300, 256, 0x2000 };
	uint32_t mi, v, k;

	for (v = 0; v < 8; v++)
		put16(pat + 2 * v, (v & 2) == 0 ? -32768 : 32767);
	for (mi = 0; mi < sizeof(masters) / sizeof(masters[0]); mi++) {
		struct audio_voice_cmd c[100];
		uint32_t n = 0;
		double worst = 0;
		int64_t peak = 0;

		audio_voice_init(&e, pat, 16, AUDIO_VOICE_CLOCK_PAL);
		audio_voice_sample_set(&e, 0, 0, 16, AUDIO_VOICE_FMT_S16);
		e.interp = INTERP_C;
		c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, masters[mi], 0);
		c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_LIMITER,
		            0, 1);
		for (v = 0; v < 32; v++) {
			c[n++] = SETHZ(0, v, 5000 + 1300 * v, 40000 + 800 * v,
			               65535 - 700 * v);
			c[n++] = mk(0, v, AUDIO_VOICE_OP_START, 0, 0, 0);
			c[n++] = mk(0, v, AUDIO_VOICE_OP_LOOP, 0, 0, 16);
		}
		audio_voice_render(&e, out, 600, c, n);
		model_sums(pat, 16, masters[mi], c + 2, n - 2, 600, xl);
		for (k = 0; k < 600; k++) {
			const long double want = limiter_curve((long double)xl[k]);
			const long double clipped = want > 32766.0L ? 32766.0L :
			                            want < -32766.0L ? -32766.0L : want;
			const double d = fabs((double)(out[2 * k] - clipped));

			if (d > worst)
				worst = d;
			if (llabs((long long)xl[k]) > peak)
				peak = llabs((long long)xl[k]);
			CHECK(out[2 * k] <= 32766 && out[2 * k] >= -32766,
			      "limiter curve master %#x frame %u: %d at a rail",
			      masters[mi], k, out[2 * k]);
		}
		CHECK(worst <= 2.0, "limiter curve master %#x: %.2f from the "
		      "reference curve (peak sum %lld)", masters[mi], worst,
		      (long long)peak);
		if (masters[mi] == 0xFFFF)
			CHECK(peak > 100000000,
			      "limiter curve: the sums stay shallow (%lld)",
			      (long long)peak);
	}
}

/* ---- records ---- */

/* A voice that plays a loud signal and a probe of it: the output only shows
 * which interpolation reads. */
static int render_probe(struct audio_voice_engine *e, uint32_t session,
                        const struct audio_voice_cmd *extra, uint32_t nextra,
                        int16_t *out, uint32_t frames)
{
	struct audio_voice_cmd c[24];
	uint32_t n = 0, i;
	int16_t s[24];

	for (i = 0; i < 24; i++) {
		s[i] = (int16_t)((i * 5000U) % 60000U - 30000);
		put16(g_bank + 2 * i, s[i]);
	}
	audio_voice_init(e, g_bank, 48, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(e, 0, 0, 48, AUDIO_VOICE_FMT_S16);
	e->interp = session;
	c[n++] = SETHZ(0, 3, 21000, 50000, 50000);
	c[n++] = mk(0, 3, AUDIO_VOICE_OP_START, 0, 0, 0);
	c[n++] = mk(0, 3, AUDIO_VOICE_OP_LOOP, 0, 0, 48);
	for (i = 0; i < nextra; i++)
		c[n++] = extra[i];
	audio_voice_render(e, out, frames, c, n);
	return 1;
}

static void test_records(void)
{
	static struct audio_voice_engine e, ref;
	static int16_t o[2 * 300], r[2 * 300];
	struct audio_voice_cmd x[16];
	uint32_t n, j;

	/* a voice follows the session default until MODE gives it its own */
	n = 0;
	x[n++] = MODE(0, 3, AUDIO_VOICE_INTERP_MODE_CUBIC);
	render_probe(&e, INTERP_N, x, n, o, 300);
	render_probe(&ref, INTERP_C, NULL, 0, r, 300);
	CHECK(memcmp(o, r, sizeof(o)) == 0, "records: MODE cubic in a nearest "
	      "session differs from a cubic session");
	n = 0;
	x[n++] = MODE(0, 3, AUDIO_VOICE_INTERP_MODE_NEAREST);
	render_probe(&e, INTERP_C, x, n, o, 300);
	render_probe(&ref, INTERP_N, NULL, 0, r, 300);
	CHECK(memcmp(o, r, sizeof(o)) == 0, "records: MODE nearest in a cubic "
	      "session differs from a nearest session");
	n = 0;
	x[n++] = MODE(0, 3, AUDIO_VOICE_INTERP_MODE_LINEAR);
	x[n++] = MODE(100, 3, AUDIO_VOICE_INTERP_MODE_INHERIT);
	render_probe(&e, INTERP_C, x, n, o, 300);
	{
		static int16_t lin[2 * 300], cub[2 * 300];

		render_probe(&ref, INTERP_L, NULL, 0, lin, 300);
		render_probe(&ref, INTERP_C, NULL, 0, cub, 300);
		/* linear until frame 100, then inherit (cubic) */
		CHECK(memcmp(o, lin, 100 * 4) == 0 &&
		      memcmp(o + 2 * 101, cub + 2 * 101, 199 * 4) == 0,
		      "records: MODE at frame 100 switches linear to inherit "
		      "(cubic) there");
	}

	/* CONFIG default: from its frame on */
	n = 0;
	x[n++] = CFGINT(150, AUDIO_VOICE_INTERP_MODE_CUBIC);
	render_probe(&e, INTERP_N, x, n, o, 300);
	{
		static int16_t nea[2 * 300], cub[2 * 300];

		render_probe(&ref, INTERP_N, NULL, 0, nea, 300);
		render_probe(&ref, INTERP_C, NULL, 0, cub, 300);
		CHECK(memcmp(o, nea, 150 * 4) == 0 &&
		      memcmp(o + 2 * 151, cub + 2 * 151, 149 * 4) == 0,
		      "records: CONFIG default cubic at frame 150 changes the "
		      "voice there");
	}
	/* a voice with its own setting ignores the default */
	n = 0;
	x[n++] = MODE(0, 3, AUDIO_VOICE_INTERP_MODE_NEAREST);
	x[n++] = CFGINT(10, AUDIO_VOICE_INTERP_MODE_CUBIC);
	render_probe(&e, INTERP_L, x, n, o, 300);
	render_probe(&ref, INTERP_N, NULL, 0, r, 300);
	CHECK(memcmp(o, r, sizeof(o)) == 0, "records: the CONFIG default changed a "
	      "voice that has a setting of its own");
	/* the setting stays across START and STOP */
	n = 0;
	x[n++] = MODE(0, 3, AUDIO_VOICE_INTERP_MODE_CUBIC);
	x[n++] = mk(50, 3, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	x[n++] = mk(60, 3, AUDIO_VOICE_OP_START, 0, 0, 0);
	x[n++] = mk(60, 3, AUDIO_VOICE_OP_LOOP, 0, 0, 48);
	render_probe(&e, INTERP_N, x, n, o, 300);
	{
		static int16_t cub[2 * 300];

		render_probe(&ref, INTERP_C, NULL, 0, cub, 300);
		/* from frame 60 the restarted voice (position 0) plays frame
		 * 0 of the loop: shift the reference */
		CHECK(memcmp(o, cub, 50 * 4) == 0 &&
		      memcmp(o + 2 * 60, cub, 100 * 4) == 0,
		      "records: the setting did not survive a STOP and START");
	}

	/* rejected records: counted, nothing changes */
	n = 0;
	x[n++] = mk(0, 3, AUDIO_VOICE_OP_MODE, 0, 1, 0);              /* no flag */
	x[n++] = mk(0, 3, AUDIO_VOICE_OP_MODE, 2, 1, 0);              /* unknown */
	x[n++] = mk(0, 3, AUDIO_VOICE_OP_MODE, 3, 1, 0);              /* unknown */
	x[n++] = mk(0, 3, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, 4, 0);
	x[n++] = mk(0, 3, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, 0xFFFFFFFFU,
	            0);
	x[n++] = mk(0, 3, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, 3, 1);
	x[n++] = mk(0, 32, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, 3, 0);
	x[n++] = mk(0, 200, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, 3, 0);
	x[n++] = mk(0, 3, 10, 1, 1, 0);                               /* unknown */
	x[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP, 0, 0);
	x[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP, 4, 0);
	x[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP, 3, 1);
	x[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
	            AUDIO_VOICE_CONFIG_INTERP | AUDIO_VOICE_CONFIG_RAMP, 3, 0);
	x[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
	            AUDIO_VOICE_CONFIG_INTERP | AUDIO_VOICE_CONFIG_LIMITER, 3, 0);
	x[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 8, 3, 0);
	render_probe(&e, INTERP_N, x, n, o, 300);
	render_probe(&ref, INTERP_N, NULL, 0, r, 300);
	CHECK(e.rejected == n, "records: %u of %u bad records counted as "
	      "rejected", e.rejected, n);
	CHECK(memcmp(o, r, sizeof(o)) == 0 && e.interp == INTERP_N &&
	      e.voice[3].vinterp == 0 && e.ramp_frames == 0 && e.limiter == 0,
	      "records: a rejected record changed something");
	/* a bad record leaves a good setting alone */
	n = 0;
	x[n++] = MODE(0, 3, AUDIO_VOICE_INTERP_MODE_CUBIC);
	x[n++] = mk(5, 3, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, 9, 0);
	x[n++] = CFGINT(6, AUDIO_VOICE_INTERP_MODE_LINEAR);
	x[n++] = mk(7, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP, 0, 0);
	render_probe(&e, INTERP_N, x, n, o, 300);
	CHECK(e.voice[3].vinterp == AUDIO_VOICE_INTERP_MODE_CUBIC &&
	      e.interp == INTERP_L && e.rejected == 2,
	      "records: vinterp %u interp %u rejected %u after a bad MODE and a "
	      "bad CONFIG", e.voice[3].vinterp, e.interp, e.rejected);
	/* every valid MODE selector and CONFIG default is accepted */
	n = 0;
	for (j = 0; j < 4; j++)
		x[n++] = MODE(0, (uint8_t)j, j);
	for (j = 1; j < 4; j++)
		x[n++] = CFGINT(0, j);
	render_probe(&e, INTERP_N, x, n, o, 10);
	CHECK(e.rejected == 0, "records: a valid selector was rejected (%u)",
	      e.rejected);
}

/* Same stamp: the records apply in their supplied order, at the same frame,
 * and a restart hands the old sound over with what the voice reads with at
 * that point of the order. */
static void tails_run(struct audio_voice_engine *e, struct rmodel *m,
                      const struct audio_voice_cmd *extra, uint32_t nextra,
                      int16_t *out, int16_t *mout, uint32_t frames)
{
	static struct audio_voice_cmd c[40];
	static uint8_t bank[256];
	uint32_t n = 0, i;

	for (i = 0; i < 48; i++)
		put16(bank + 2 * i, (int)((i * 7777U) % 60000U) - 30000);
	/* sample 1: 20 silent frames */
	memset(bank + 96, 0, 40);
	audio_voice_init(e, bank, 136, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(e, 0, 0, 96, AUDIO_VOICE_FMT_S16);
	audio_voice_sample_set(e, 1, 96, 40, AUDIO_VOICE_FMT_S16);
	r_init(m, bank, 136, AUDIO_VOICE_CLOCK_PAL);
	r_sample_set(m, 0, 0, 96, AUDIO_VOICE_FMT_S16);
	r_sample_set(m, 1, 96, 40, AUDIO_VOICE_FMT_S16);
	c[n++] = CFGRAMP(0, 120);
	c[n++] = SETHZ(0, 5, 19000, 45000, 30000);
	c[n++] = mk(0, 5, AUDIO_VOICE_OP_START, 0, 0, 0);
	c[n++] = mk(0, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 96);
	for (i = 0; i < nextra; i++)
		c[n++] = extra[i];
	audio_voice_render(e, out, frames, c, n);
	r_render(m, mout, frames, c, n);
}

static void test_tails(void)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	static int16_t a[2 * 700], b[2 * 700], ma[2 * 700];
	struct audio_voice_cmd x[16];
	uint32_t n, j;
	int differ;

	/* The tail of the restart: the engine against the model, and the tail
	 * alone does not change when later records change the voice and the
	 * default. The restart goes to a silent sample, so only the tail sounds. */
	n = 0;
	x[n++] = MODE(0, 5, AUDIO_VOICE_INTERP_MODE_CUBIC);
	x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
	x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
	tails_run(&e, &m, x, n, a, ma, 700);
	CHECK(memcmp(a, ma, sizeof(a)) == 0, "tails: the engine differs from the "
	      "model");
	CHECK(a[2 * 310] != 0, "tails: the tail sounds");
	n = 0;
	x[n++] = MODE(0, 5, AUDIO_VOICE_INTERP_MODE_CUBIC);
	x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
	x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
	x[n++] = MODE(305, 5, AUDIO_VOICE_INTERP_MODE_NEAREST);
	x[n++] = CFGINT(310, AUDIO_VOICE_INTERP_MODE_LINEAR);
	x[n++] = MODE(315, 5, AUDIO_VOICE_INTERP_MODE_INHERIT);
	x[n++] = CFGINT(320, AUDIO_VOICE_INTERP_MODE_NEAREST);
	x[n++] = mk(325, 5, AUDIO_VOICE_OP_DIRECTION, AUDIO_VOICE_DIR_BACKWARD, 0,
	            0);
	x[n++] = mk(330, 5, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	tails_run(&e, &m, x, n, b, ma, 700);
	CHECK(memcmp(b, ma, sizeof(b)) == 0, "tails: the engine differs from the "
	      "model after later records");
	CHECK(memcmp(a, b, sizeof(a)) == 0, "tails: later MODE, CONFIG, "
	      "DIRECTION and STOP records changed the tail");
	/* the tail's mode is the one in force at the restart: nearest instead */
	n = 0;
	x[n++] = MODE(0, 5, AUDIO_VOICE_INTERP_MODE_NEAREST);
	x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
	x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
	tails_run(&e, &m, x, n, b, ma, 700);
	differ = 0;
	for (j = 600; j < 1400; j++)
		differ += a[j] != b[j];
	CHECK(differ > 0, "tails: a nearest tail equals a cubic tail");
	CHECK(memcmp(b, ma, sizeof(b)) == 0, "tails: engine differs from the "
	      "model (nearest tail)");

	/* same stamp: MODE before START freezes the new mode in the tail, MODE
	 * after START leaves the old one in the tail */
	{
		static int16_t before[2 * 700], after[2 * 700];
		int d2 = 0;

		n = 0;
		x[n++] = MODE(0, 5, AUDIO_VOICE_INTERP_MODE_CUBIC);
		x[n++] = MODE(300, 5, AUDIO_VOICE_INTERP_MODE_NEAREST);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
		tails_run(&e, &m, x, n, before, ma, 700);
		CHECK(memcmp(before, ma, sizeof(before)) == 0,
		      "same stamp: MODE then START differs from the model");
		n = 0;
		x[n++] = MODE(0, 5, AUDIO_VOICE_INTERP_MODE_CUBIC);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
		x[n++] = MODE(300, 5, AUDIO_VOICE_INTERP_MODE_NEAREST);
		tails_run(&e, &m, x, n, after, ma, 700);
		CHECK(memcmp(after, ma, sizeof(after)) == 0,
		      "same stamp: START then MODE differs from the model");
		for (j = 600; j < 1400; j++)
			d2 += before[j] != after[j];
		CHECK(d2 > 0, "same stamp: MODE before and after START give the "
		      "same tail");
		CHECK(memcmp(after, a, sizeof(a)) == 0,
		      "same stamp: START then MODE did not keep the cubic tail");
	}
	/* CONFIG default against START at one stamp, the voice following it */
	{
		static int16_t before[2 * 700], after[2 * 700];
		int d2 = 0;

		n = 0;
		x[n++] = CFGINT(0, AUDIO_VOICE_INTERP_MODE_CUBIC);
		x[n++] = CFGINT(300, AUDIO_VOICE_INTERP_MODE_NEAREST);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
		tails_run(&e, &m, x, n, before, ma, 700);
		CHECK(memcmp(before, ma, sizeof(before)) == 0,
		      "same stamp: CONFIG then START differs from the model");
		n = 0;
		x[n++] = CFGINT(0, AUDIO_VOICE_INTERP_MODE_CUBIC);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
		x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
		x[n++] = CFGINT(300, AUDIO_VOICE_INTERP_MODE_NEAREST);
		tails_run(&e, &m, x, n, after, ma, 700);
		CHECK(memcmp(after, ma, sizeof(after)) == 0,
		      "same stamp: START then CONFIG differs from the model");
		for (j = 600; j < 1400; j++)
			d2 += before[j] != after[j];
		CHECK(d2 > 0, "same stamp: CONFIG before and after START give the "
		      "same tail");
	}
}

/* Mode changes while a ramp runs: the engine against the model, with the
 * change at every frame of a short ramp, for the voice and for the default. */
static void test_mid_ramp(void)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	static int16_t a[2 * 400], ma[2 * 400];
	struct audio_voice_cmd x[6];
	uint32_t at, kind, n, bad = 0;

	for (kind = 0; kind < 3; kind++)
		for (at = 295; at < 330; at++) {
			n = 0;
			x[n++] = mk(300, 5, AUDIO_VOICE_OP_START, 1, 0, 0);
			x[n++] = mk(300, 5, AUDIO_VOICE_OP_LOOP, 0, 0, 40);
			x[n++] = SETHZ(300, 5, 31000, 65535, 20000);
			if (kind == 0)
				x[n++] = MODE(at, 5, AUDIO_VOICE_INTERP_MODE_CUBIC);
			else if (kind == 1)
				x[n++] = CFGINT(at, AUDIO_VOICE_INTERP_MODE_CUBIC);
			else
				x[n++] = mk(at, 5, AUDIO_VOICE_OP_STOP, 0, 0, 0);
			tails_run(&e, &m, x, n, a, ma, 400);
			if (memcmp(a, ma, sizeof(a)) != 0)
				bad++;
		}
	CHECK(bad == 0, "mid-ramp: %u of 105 runs differ from the model", bad);
}

/* ---- the engine of this file's header ---- */

int main(void)
{
	const char *env;
	uint32_t seqs = 6000, seq, done = 0, eq;
	uint64_t seed = 1;

	if ((env = getenv("ZZ9K_VOICE_CUBIC_SEQS")) != NULL)
		seqs = (uint32_t)strtoul(env, NULL, 0);
	if ((env = getenv("ZZ9K_VOICE_CUBIC_SEED")) != NULL)
		seed = strtoull(env, NULL, 0);
#ifdef HAVE_ALARM
	alarm(120U + seqs / 50U);
#endif
	test_curve_error();
	test_sine();
	test_unrolled();
	test_formats();
	test_overshoot();
	test_limiter_cubic();
	test_limiter_64();
	test_limiter_curve();
	test_records();
	test_tails();
	test_mid_ramp();
	for (seq = 0; seq < seqs && failures < 5; seq++) {
		if (!run_sequence(seq, seed))
			failures++;
		done++;
	}
	for (eq = 0; eq < seqs / 6 + 50 && failures < 5; eq++)
		if (!run_equivalence(eq, seed))
			failures++;
	if (failures) {
		printf("audio_voice_cubic_test: %d FAILURES\n", failures);
		return 1;
	}
	if (seqs >= 1000U && (st.cubic_audible * 3U < st.renders ||
	    st.per_voice * 3U < st.renders || st.tails_cubic == 0U ||
	    st.edge_loops == 0U || st.backward == 0U || st.clipped == 0U)) {
		printf("audio_voice_cubic_test: corpus too weak (renders %u, cubic "
		       "audible %u, per-voice %u, cubic tails %u, short loops %u, "
		       "backward %u, clipped %u)\n", st.renders, st.cubic_audible,
		       st.per_voice, st.tails_cubic, st.edge_loops, st.backward,
		       st.clipped);
		return 1;
	}
	printf("audio_voice_cubic_test: sine, unrolled streams, formats, "
	       "overshoot, limiter, records, tails and %u random sequences "
	       "against the model and in pieces, and %u equivalence sequences, "
	       "passed (seed %llu); %u renders, %u with a cubic voice sounding, "
	       "%u with per-voice settings, %u with cubic tails, %u with short "
	       "loops, %u backward, %u clipped\n", done, eq,
	       (unsigned long long)seed, st.renders, st.cubic_audible,
	       st.per_voice, st.tails_cubic, st.edge_loops, st.backward, st.clipped);
	return 0;
}
