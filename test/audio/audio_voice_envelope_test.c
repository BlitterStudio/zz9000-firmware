/*
 * Envelopes of the voice engine (ENVELOPE, STOP_RELEASE, "Envelopes" in
 * audio_voice.h).
 *
 * The reference is a model written from the rules in that comment, frame by
 * frame, with none of the engine's run planning, vector code or incremental
 * stepping: the level of a block is a pure function of the stage and the
 * block number (a division for the attack, a long double 2^x table and the
 * documented interpolation for the exponentials), the volume of each frame
 * is ((gain >> 8) * multiplier) >> 15 and the position, ramp and fade-slot
 * rules are the ones of the ramp test's model. It checks
 *
 *   - random sequences with ENVELOPE, key-offs, ramps, loops and restarts
 *     against the engine: byte-identical PCM when the engine renders a
 *     period in one call and when it renders it in random pieces, equal
 *     voice state, envelope state, end count and fade slots in use after
 *     every period (nearest, linear, and cubic on constant samples, where
 *     the cubic curve is the constant);
 *   - directed cases with the expected values worked out by hand or from
 *     the real-valued curves: stage boundaries, a key-off during each stage,
 *     a volume change in sustain, zero-length stages, a retrigger, frozen
 *     tails, long releases, end counting, record validation;
 *   - random legacy sequences against the frozen baseline engine, with
 *     envelope-clearing records and STOP flags mixed in: the same PCM.
 *
 * Environment: ZZ9K_VOICE_ENV_SEQS (default 6000), ZZ9K_VOICE_ENV_SEED.
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
#include "audio_voice_baseline.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- the model ---- */

enum { ST_NONE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };

#define ONE (1ULL << 30)
#define DEPTH (16ULL << 24)

static uint32_t g_tab[257];

static void init_table(void)
{
	uint32_t j;

	for (j = 0; j < 257; j++)
		g_tab[j] = (uint32_t)llroundl(
			ldexpl(exp2l(-(long double)j / 256.0L), 31));
}

/* 2^-a in Q31, a in Q24 octaves: whole octaves shift, the table and its
 * linear interpolation by the 16-bit fraction of the index. */
static uint64_t m_exp2neg(uint64_t a)
{
	uint32_t oct = (uint32_t)(a >> 24), j = (uint32_t)(a >> 16) & 0xFF;
	uint64_t t = a & 0xFFFF;
	uint64_t y = g_tab[j] - (((uint64_t)(g_tab[j] - g_tab[j + 1]) * t) >> 16);

	return oct >= 32 ? 0 : y >> oct;
}

struct rvoice {
	int active, backward, stopping, wide, stereo, tail;
	uint32_t shift, base, len, pos, frac, end, ls, ll, step, vl, vr, left;
	int32_t gl, gr, il, ir;
	/* envelope: the setting for the next START (times in blocks, the
	 * sustain Q15) and the one in use */
	int env_on;
	uint32_t sa, sd, ss, sr;
	uint32_t ea, ed, es, er;
	int stage;
	uint32_t k, blk;
	uint64_t lbase;
};

struct rmodel {
	const uint8_t *bank;
	uint32_t bank_len, clock, interp, master, ramp, late, rejected, frame;
	uint32_t seq, ends;
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

/* The level of the block the voice is in, Q30. */
static uint64_t m_level(const struct rvoice *v)
{
	const uint64_t s = (uint64_t)v->es << 15;

	switch (v->stage) {
	case ST_ATTACK:
		return (uint64_t)v->k * ONE / v->ea;
	case ST_DECAY:
		return s + (((ONE - s) *
		             m_exp2neg((uint64_t)v->k * DEPTH / v->ed)) >> 31);
	case ST_SUSTAIN:
		return s;
	case ST_RELEASE:
		return (v->lbase * m_exp2neg((uint64_t)v->k * DEPTH / v->er)) >> 31;
	}
	return ONE;
}

static uint32_t m_mul(const struct rvoice *v)
{
	return v->stage == ST_NONE ? 32768U : (uint32_t)(m_level(v) >> 15);
}

static void m_enter(struct rvoice *v, int stage)
{
	v->stage = stage;
	v->k = 0;
	v->blk = 0;
	if (stage == ST_ATTACK && v->ea == 0)
		m_enter(v, ST_DECAY);
	else if (stage == ST_DECAY && v->ed == 0)
		v->stage = ST_SUSTAIN;
}

static void m_end(struct rmodel *m, struct rvoice *v)
{
	v->active = 0;
	v->left = 0;
	v->stopping = 0;
	v->stage = ST_NONE;
	if (!v->tail)
		m->ends++;
}

static void m_keyoff(struct rmodel *m, struct rvoice *v)
{
	if (v->stage == ST_RELEASE)
		return;
	v->lbase = m_level(v);
	if (v->er == 0) {
		m_end(m, v);
		return;
	}
	v->stage = ST_RELEASE;
	v->k = 0;
	v->blk = 0;
}

/* One frame of the envelope clock; sustain has no clock. */
static void m_env_tick(struct rmodel *m, struct rvoice *v)
{
	if (v->stage == ST_NONE || v->stage == ST_SUSTAIN || !v->active)
		return;
	if (++v->blk < AUDIO_VOICE_ENV_STEP_FRAMES)
		return;
	v->blk = 0;
	v->k++;
	if (v->stage == ST_ATTACK && v->k >= v->ea)
		m_enter(v, ST_DECAY);
	else if (v->stage == ST_DECAY && v->k >= v->ed)
		v->stage = ST_SUSTAIN;
	else if (v->stage == ST_RELEASE && v->k >= v->er)
		m_end(m, v);
}

static void r_fade_out(struct rmodel *m, const struct rvoice *v)
{
	uint32_t i, slot = AUDIO_VOICE_FADE_SLOTS, oldest = 0;
	int32_t gl, gr;

	if (v->step == 0)
		return;
	r_gains(v, &gl, &gr);
	if (!v->stopping && ((gl == 0 && gr == 0) || m_mul(v) == 0))
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
	m->f[slot].tail = 1;
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
	v->stage = ST_NONE;
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
	v->active = 1;
	if (v->env_on) {
		v->ea = v->sa;
		v->ed = v->sd;
		v->es = v->ss;
		v->er = v->sr;
		m_enter(v, ST_ATTACK);
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
		if (c->flags == 0 || (c->flags & ~1U) != 0 ||
		    c->arg0 > AUDIO_VOICE_RAMP_MAX_FRAMES)
			m->rejected++;
		else
			m->ramp = c->arg0;
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
		if ((c->flags & AUDIO_VOICE_STOP_RELEASE) && v->active &&
		    v->stage != ST_NONE) {
			if (!v->stopping)
				m_keyoff(m, v);
		} else if (m->ramp && v->active) {
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
	case AUDIO_VOICE_OP_ENVELOPE:
		if (c->flags == 0 && c->arg0 == 0 && c->arg1 == 0) {
			v->env_on = 0;
		} else if (c->flags != 1 || (c->arg1 >> 16) > 32768) {
			m->rejected++;
		} else {
			v->env_on = 1;
			v->sa = (c->arg0 >> 16) * 3;
			v->sd = (c->arg0 & 0xFFFF) * 3;
			v->ss = c->arg1 >> 16;
			v->sr = (c->arg1 & 0xFFFF) * 3;
		}
		break;
	default:
		m->rejected++;
		break;
	}
}

static void r_frame(struct rmodel *m, int16_t *out)
{
	int64_t sum_l = 0, sum_r = 0;
	uint32_t k;

	for (k = 0; k < 64; k++) {
		struct rvoice *v = k < 32 ? &m->v[k] : &m->f[k - 32];
		int32_t sl, sr, vl, vr;
		uint32_t em;

		if (!v->active)
			continue;
		if (v->step == 0) {
			r_tick(v);
			m_env_tick(m, v);
			continue;
		}
		em = m_mul(v);
		vl = v->left ? (v->gl >> 8) : (int32_t)v->vl;
		vr = v->left ? (v->gr >> 8) : (int32_t)v->vr;
		vl = (int32_t)(((uint32_t)vl * em) >> 15);
		vr = (int32_t)(((uint32_t)vr * em) >> 15);
		sl = r_side(m, v, v->pos, 0);
		sr = v->stereo ? r_side(m, v, v->pos, 1) : sl;
		if (m->interp == AUDIO_VOICE_INTERP_LINEAR) {
			uint32_t n = r_neighbour(v);
			int32_t bl = r_side(m, v, n, 0);
			int32_t br = v->stereo ? r_side(m, v, n, 1) : bl;

			sl += (int32_t)(((int64_t)(bl - sl) * (int64_t)v->frac) >> 16);
			sr = v->stereo ?
				sr + (int32_t)(((int64_t)(br - sr) * (int64_t)v->frac) >> 16) :
				sl;
		}
		sum_l += (sl * vl) >> 16;
		sum_r += (sr * vr) >> 16;
		r_advance(v);
		r_tick(v);
		m_env_tick(m, v);
	}
	{
		int64_t l = (sum_l * (int64_t)m->master) >> 8;
		int64_t r = (sum_r * (int64_t)m->master) >> 8;

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
	    e->ramp_frames != m->ramp || e->ends != m->ends) {
		printf("FAIL seq %u %s: engine frame %u/%u late %u/%u rejected "
		       "%u/%u master %u/%u ramp %u/%u ends %u/%u\n", seq, when,
		       e->frame, m->frame, e->late, m->late, e->rejected,
		       m->rejected, e->master, m->master, e->ramp_frames, m->ramp,
		       e->ends, m->ends);
		return 0;
	}
	for (k = 0; k < 32; k++) {
		const struct audio_voice *a = &e->voice[k];
		const struct rvoice *b = &m->v[k];

		if (a->active != b->active ||
		    (a->active && (a->pos != b->pos || a->frac != b->frac ||
		                   a->end != b->end || a->backward != b->backward ||
		                   a->step != b->step || a->vol_l != b->vl ||
		                   a->vol_r != b->vr ||
		                   a->stopping != b->stopping ||
		                   a->env_stage != b->stage ||
		                   (b->stage != ST_NONE &&
		                    (uint64_t)a->env_level != m_level(b))))) {
			printf("FAIL seq %u %s: voice %u active %d/%d pos %u/%u frac "
			       "%u/%u env stage %u/%d level %u/%llu\n", seq, when, k,
			       a->active, b->active, a->pos, b->pos, a->frac, b->frac,
			       a->env_stage, b->stage, a->env_level,
			       (unsigned long long)m_level(b));
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
static int16_t g_out_e[2 * 4096], g_out_s[2 * 4096], g_out_m[2 * 4096];
static struct audio_voice_cmd g_cmds[1024];

static uint32_t pick_ramp(void)
{
	static const uint32_t r[] = { 1, 2, 3, 7, 16, 64, 100, 256, 511, 512, 0, 0 };

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

static uint32_t pick_ms(void)
{
	static const uint32_t t[] = { 0, 0, 1, 1, 2, 3, 5, 8, 12, 20, 40, 100 };

	return t[rnd(sizeof(t) / sizeof(t[0]))];
}

static uint32_t pick_sustain(void)
{
	static const uint32_t s[] = { 0, 1, 100, 8192, 16384, 32767, 32768,
	                              32768, 20000 };

	return s[rnd(sizeof(s) / sizeof(s[0]))];
}

static void set_cmd(struct audio_voice_cmd *c, uint32_t stamp, uint32_t voice,
                    uint32_t op, uint32_t flags, uint32_t a0, uint32_t a1)
{
	c->stamp = stamp;
	c->voice = (uint8_t)voice;
	c->op = (uint8_t)op;
	c->flags = (uint16_t)flags;
	c->arg0 = a0;
	c->arg1 = a1;
}

static void pick_envelope(struct audio_voice_cmd *c, uint32_t stamp,
                          uint32_t voice)
{
	uint32_t pick = rnd(10);

	if (pick == 0) {
		set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, 0, 0, 0);
	} else if (pick == 1) {
		/* invalid: unknown flag, flag 0 with arguments, sustain too high */
		switch (rnd(4)) {
		case 0: set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, 2,
		                pick_ms() << 16, pick_sustain() << 16); break;
		case 1: set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, 0, 1, 0);
			break;
		case 2: set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, 1,
		                pick_ms() << 16, (32769U + rnd(30000)) << 16); break;
		default: set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, 3, 5, 5);
			break;
		}
	} else {
		set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, 1,
		        (pick_ms() << 16) | pick_ms(),
		        (pick_sustain() << 16) | pick_ms());
	}
}

static struct {
	uint32_t renders, with_env, in_attack, in_decay, in_sustain, in_release,
	         ends, tails, ramping, audible, slot_env;
} st;

static void note_render(const struct audio_voice_engine *e,
                        const int16_t *out, uint32_t frames)
{
	uint32_t k, audible = 0, env = 0, a = 0, d = 0, s = 0, r = 0, ramping = 0;

	for (k = 0; k < 2 * frames; k++)
		audible |= out[k] != 0;
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		const struct audio_voice *v = &e->voice[k];

		if (!v->active)
			continue;
		env |= v->env_stage != 0;
		a |= v->env_stage == AUDIO_VOICE_ENV_ATTACK;
		d |= v->env_stage == AUDIO_VOICE_ENV_DECAY;
		s |= v->env_stage == AUDIO_VOICE_ENV_SUSTAIN;
		r |= v->env_stage == AUDIO_VOICE_ENV_RELEASE;
		ramping |= v->ramp_left != 0;
	}
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++)
		st.slot_env += e->fade[k].active && e->fade[k].env_stage != 0;
	st.renders++;
	st.with_env += env;
	st.in_attack += a;
	st.in_decay += d;
	st.in_sustain += s;
	st.in_release += r;
	st.ends = e->ends;
	st.tails += fades_in_use(e) != 0;
	st.ramping += ramping != 0;
	st.audible += audible;
}

/* Renders `frames` in random pieces; the records are the ones of the whole
 * call, handed on as the engine consumes them. */
static size_t render_split(struct audio_voice_engine *e, int16_t *out,
                           uint32_t frames, const struct audio_voice_cmd *cmds,
                           size_t n)
{
	uint32_t done = 0;
	size_t used = 0;

	while (done < frames) {
		uint32_t piece = 1 + rnd(rnd(3) ? 400 : 40);

		if (piece > frames - done)
			piece = frames - done;
		used += audio_voice_render(e, out + 2U * done, piece, cmds + used,
		                           n - used);
		done += piece;
	}
	return used;
}

static int run_sequence(uint32_t seq, uint64_t seed)
{
	static struct audio_voice_engine e, es;
	static struct rmodel m;
	uint32_t bank_len = 64 + rnd(sizeof(g_bank) - 64), i, nvoices, nsamp;
	uint32_t kind, interp, storm = rnd(5) == 0, ncmds = 0, frame = 0;
	uint32_t r, renders = 2 + rnd(4);
	uint32_t smp_len[8];

	rng = seed + (uint64_t)seq * 0xD1B54A32D192ED03ULL;
	(void)rnd(2);
	kind = rnd(4);
	/* a constant sample reads the same in every interpolation, so cubic
	 * is included for it */
	interp = kind == 3 ? rnd(3) : rnd(2);
	{
		int32_t walk = 0;
		uint8_t dc = (uint8_t)(1 + rnd(254));

		for (i = 0; i < bank_len; i++) {
			uint32_t v = rnd(256);

			walk += (int32_t)(v % 9) - 4;
			g_bank[i] = kind == 3 ? dc :
			            kind == 0 ? (uint8_t)(v & 1 ? 0x7F : 0x80) :
			            kind == 1 ? (uint8_t)v : (uint8_t)walk;
		}
	}
	audio_voice_init(&e, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_init(&es, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	r_init(&m, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	e.interp = es.interp = interp;
	m.interp = interp == AUDIO_VOICE_INTERP_LINEAR ? AUDIO_VOICE_INTERP_LINEAR :
	                                                 AUDIO_VOICE_INTERP_NEAREST;
	nsamp = 1 + rnd(8);
	for (i = 0; i < nsamp; i++) {
		uint32_t fmt = rnd(4), off = rnd(bank_len), len;
		int rc_e, rc_m;

		len = rnd(bank_len - off + 1);
		if (rnd(3) == 0)
			len = bank_len - off;
		rc_e = audio_voice_sample_set(&e, i, off, len, fmt);
		audio_voice_sample_set(&es, i, off, len, fmt);
		rc_m = r_sample_set(&m, i, off, len, fmt);
		if (rc_e != rc_m) {
			printf("FAIL seq %u sample_set rc\n", seq);
			return 0;
		}
		smp_len[i] = rc_e == 0 ? len : 0;
	}
	nvoices = storm ? 32 : 1 + rnd(32);

	for (r = 0; r < renders; r++) {
		uint32_t frames = 1 + rnd(storm ? 3000 : 2000), n = 0;
		uint32_t extra = storm ? 150 + rnd(400) : 10 + rnd(60);
		size_t ue, us, um;

		if (r == 0) {
			uint32_t v;

			set_cmd(&g_cmds[n++], frame, 0, AUDIO_VOICE_OP_CONFIG, 1,
			        pick_ramp(), 0);
			for (v = 0; v < nvoices && n + 5 < 300; v++) {
				uint32_t id = rnd(nsamp), stamp = frame + rnd(40);

				if (rnd(4) != 0)
					pick_envelope(&g_cmds[n++], stamp, v);
				set_cmd(&g_cmds[n++], stamp, v, AUDIO_VOICE_OP_SET,
				        AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
				        AUDIO_VOICE_SET_VOLUME, pick_hz(),
				        (pick_vol() << 16) | pick_vol());
				set_cmd(&g_cmds[n++], stamp, v, AUDIO_VOICE_OP_START, id,
				        rnd(3) ? 0 : rnd(smp_len[id] + 1), 0);
				if (rnd(2))
					set_cmd(&g_cmds[n++], stamp, v, AUDIO_VOICE_OP_LOOP, 0,
					        0, rnd(2) ? smp_len[id] : rnd(smp_len[id] + 1));
			}
		}
		for (i = 0; i < extra && n < 1000; i++) {
			uint32_t pick = rnd(100), v = rnd(storm ? 32 : nvoices + 1);
			uint32_t stamp = frame + rnd(frames);
			uint32_t id = rnd(nsamp);

			if (rnd(4) == 0)
				stamp = frame + rnd(8) * 16 + (rnd(2) ? 0 : rnd(16));
			if (stamp >= frame + frames)
				stamp = frame + frames - 1;
			if (pick < 18)
				set_cmd(&g_cmds[n], stamp, v, AUDIO_VOICE_OP_START, id,
				        rnd(3) ? 0 : rnd(smp_len[id] + 1),
				        rnd(5) ? 0 : rnd(smp_len[id] + 1));
			else if (pick < 28)
				set_cmd(&g_cmds[n], stamp, v, AUDIO_VOICE_OP_LOOP, 0,
				        rnd(smp_len[id] / 4 + 1), rnd(smp_len[id] + 1));
			else if (pick < 42)
				set_cmd(&g_cmds[n], stamp, v, AUDIO_VOICE_OP_SET,
				        (rnd(2) ? AUDIO_VOICE_SET_RATE |
				         (rnd(4) ? AUDIO_VOICE_RATE_HZ : 0) : 0) |
				        (rnd(2) ? AUDIO_VOICE_SET_VOLUME : 0),
				        rnd(5) ? pick_hz() : 5 + rnd(1000),
				        (pick_vol() << 16) | pick_vol());
			else if (pick < 55)
				set_cmd(&g_cmds[n], stamp, v, AUDIO_VOICE_OP_STOP,
				        rnd(3) ? 1 : rnd(4), 0, 0);
			else if (pick < 70)
				pick_envelope(&g_cmds[n], stamp, v);
			else if (pick < 76)
				set_cmd(&g_cmds[n], stamp, v, AUDIO_VOICE_OP_DIRECTION,
				        rnd(2), 0, 0);
			else if (pick < 80)
				set_cmd(&g_cmds[n], stamp, 0, AUDIO_VOICE_OP_MASTER, 0,
				        rnd(3) ? rnd(600) : rnd(70000), 0);
			else if (pick < 86)
				set_cmd(&g_cmds[n], stamp, 0, AUDIO_VOICE_OP_CONFIG, 1,
				        rnd(20) ? pick_ramp() : 513 + rnd(1000), 0);
			else
				set_cmd(&g_cmds[n], stamp, v, 10 + rnd(200), rnd(65536),
				        rnd(100), rnd(100));
			n++;
		}
		ncmds = n;
		/* stable insertion sort by stamp */
		for (i = 1; i < ncmds; i++) {
			struct audio_voice_cmd t = g_cmds[i];
			uint32_t j = i;

			while (j && g_cmds[j - 1].stamp > t.stamp) {
				g_cmds[j] = g_cmds[j - 1];
				j--;
			}
			g_cmds[j] = t;
		}
		memset(g_out_e, 0x5A, sizeof(g_out_e));
		memset(g_out_s, 0x5A, sizeof(g_out_s));
		memset(g_out_m, 0x5A, sizeof(g_out_m));
		ue = audio_voice_render(&e, g_out_e, frames, g_cmds, ncmds);
		us = render_split(&es, g_out_s, frames, g_cmds, ncmds);
		um = r_render(&m, g_out_m, frames, g_cmds, ncmds);
		if (ue != um || us != um) {
			printf("FAIL seq %u render %u consumed %zu/%zu/%zu\n", seq, r,
			       ue, us, um);
			return 0;
		}
		if (memcmp(g_out_e, g_out_m, sizeof(g_out_e)) != 0 ||
		    memcmp(g_out_s, g_out_m, sizeof(g_out_s)) != 0) {
			uint32_t f;

			for (f = 0; f < 2 * frames; f++)
				if (g_out_e[f] != g_out_m[f] || g_out_s[f] != g_out_m[f])
					break;
			printf("FAIL seq %u render %u (%u frames, %u records, interp %u) "
			       "differs at frame %u side %u: engine %d split %d model "
			       "%d\n", seq, r, frames, ncmds, interp, f / 2, f & 1,
			       g_out_e[f], g_out_s[f], g_out_m[f]);
			return 0;
		}
		if (!same_state(&e, &m, seq, "after render") ||
		    !same_state(&es, &m, seq, "after split render"))
			return 0;
		note_render(&e, g_out_e, frames);
		frame += frames;
	}
	return 1;
}

/* ---- directed cases: a constant sample, formulas worked out by hand ---- */

#define DC 32767
static uint8_t g_dc[64];
static struct audio_voice_engine g_e;

/* Output of one voice at volume vol (per side), as the engine rounds it. */
static int dc_out(uint32_t vol)
{
	return (DC * (int32_t)vol) >> 16;
}

/* The volume of a frame: base volume scaled by the Q15 multiplier. */
static uint32_t vol_of(uint32_t base, uint32_t mul)
{
	return (base * mul) >> 15;
}

static void dc_init(void)
{
	uint32_t i;

	for (i = 0; i < 32; i++) {
		g_dc[2 * i] = DC >> 8;
		g_dc[2 * i + 1] = DC & 0xFF;
	}
	audio_voice_init(&g_e, g_dc, sizeof(g_dc), AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, sizeof(g_dc), AUDIO_VOICE_FMT_S16);
}

/* A looping note on `voice` at 48 kHz, volume vol on both sides. */
static uint32_t note(struct audio_voice_cmd *c, uint32_t stamp, uint32_t voice,
                     uint32_t vol)
{
	set_cmd(&c[0], stamp, voice, AUDIO_VOICE_OP_SET,
	        AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ | AUDIO_VOICE_SET_VOLUME,
	        48000, (vol << 16) | vol);
	set_cmd(&c[1], stamp, voice, AUDIO_VOICE_OP_START, 0, 0, 0);
	set_cmd(&c[2], stamp, voice, AUDIO_VOICE_OP_LOOP, 0, 0, sizeof(g_dc));
	return 3;
}

static uint32_t envelope(struct audio_voice_cmd *c, uint32_t stamp,
                         uint32_t voice, uint32_t a, uint32_t d, uint32_t s,
                         uint32_t r)
{
	set_cmd(c, stamp, voice, AUDIO_VOICE_OP_ENVELOPE, AUDIO_VOICE_ENV_SET,
	        (a << 16) | d, (s << 16) | r);
	return 1;
}

static void keyoff(struct audio_voice_cmd *c, uint32_t stamp, uint32_t voice)
{
	set_cmd(c, stamp, voice, AUDIO_VOICE_OP_STOP, AUDIO_VOICE_STOP_RELEASE, 0,
	        0);
}

static int16_t g_buf[2 * 8192];

/* The Q15 multiplier of block k of an attack of n blocks: floor(k 2^30 / n)
 * >> 15. */
static uint32_t attack_mul(uint32_t k, uint32_t n)
{
	return (uint32_t)(((uint64_t)k * ONE / n) >> 15);
}

static void render_all(const struct audio_voice_cmd *c, size_t n,
                       uint32_t frames)
{
	size_t used = audio_voice_render(&g_e, g_buf, frames, c, n);

	CHECK(used == n, "all %zu records were consumed, got %zu", n, used);
}

/* Stage boundaries: the level is constant over blocks of 16 frames counted
 * from the key-on, and the stages change at the expected frames. */
static void test_stage_boundaries(void)
{
	struct audio_voice_cmd c[8];
	const uint32_t at = 37, a = 4, d = 6, s = 8192, r = 3;
	uint32_t n = 0, f;
	const uint32_t sus_vol = vol_of(65535, s);
	const uint32_t ab = a * 3, db = d * 3;

	dc_init();
	n += envelope(c + n, 0, 0, a, d, s, r);
	n += note(c + n, at, 0, 65535);
	render_all(c, n, 1000);
	for (f = 0; f < at; f++)
		CHECK(g_buf[2 * f] == 0, "silent before key-on (frame %u)", f);
	for (f = at; f < 1000; f++) {
		const uint32_t rel = f - at, blk = rel / 16;
		double level;
		int expect;

		if (blk < ab) {
			level = (double)blk / ab;
		} else if (blk < ab + db) {
			level = (double)s / 32768 + (1.0 - (double)s / 32768) *
			        pow(2.0, -16.0 * (double)(blk - ab) / db);
		} else {
			level = (double)s / 32768;
		}
		expect = dc_out((uint32_t)(65535.0 * floor(level * 32768.0) /
		                           32768.0));
		CHECK(abs(g_buf[2 * f] - expect) <= 3,
		      "frame %u (block %u): %d, real curve %d", f, blk, g_buf[2 * f],
		      expect);
		CHECK(g_buf[2 * f] == g_buf[2 * f + 1], "both sides equal");
		if (rel % 16 != 0)
			CHECK(g_buf[2 * f] == g_buf[2 * f - 2],
			      "the level is constant in a block (frame %u)", f);
		if (blk < ab && rel % 16 == 0 && blk != 0)
			CHECK(g_buf[2 * f] >= g_buf[2 * f - 2],
			      "attack is monotonic (frame %u)", f);
		if (blk >= ab && blk < ab + db && rel % 16 == 0 && blk != ab)
			CHECK(g_buf[2 * f] <= g_buf[2 * f - 2],
			      "decay is monotonic (frame %u)", f);
	}
	/* the attack's blocks are exact: block j is floor(j 2^30 / A) >> 15 */
	for (f = 0; f < ab; f++)
		CHECK(g_buf[2 * (at + 16 * f)] ==
		      dc_out(vol_of(65535, attack_mul(f, ab))),
		      "attack block %u: %d", f, g_buf[2 * (at + 16 * f)]);
	CHECK(g_buf[2 * (at + 16 * ab)] == dc_out(vol_of(65535, 32768)),
	      "the decay starts at full level (%d)", g_buf[2 * (at + 16 * ab)]);
	CHECK(g_buf[2 * (at + 16 * (ab + db))] == dc_out(sus_vol),
	      "the sustain level is exact (%d, want %d)",
	      g_buf[2 * (at + 16 * (ab + db))], dc_out(sus_vol));
	CHECK(g_buf[2 * (at + 16 * (ab + 2))] > dc_out(sus_vol),
	      "the decay is above the sustain level two blocks in");
	CHECK(g_buf[2 * 999] == dc_out(sus_vol), "sustain holds");
}

/* A key-off in each stage releases from the current level, in blocks of
 * 16 frames from the key-off frame, ends exactly 16 R frames later and
 * counts one end. */
static void check_release(const char *what, uint32_t keyoff_frame,
                          uint32_t mul_before, uint32_t r)
{
	const uint32_t rb = r * 3;
	uint32_t f, k;

	CHECK(g_buf[2 * keyoff_frame] == g_buf[2 * (keyoff_frame - 1)] ||
	      keyoff_frame % 16 == 0,
	      "%s: no jump at the key-off (%d after, %d before)", what,
	      g_buf[2 * keyoff_frame], g_buf[2 * (keyoff_frame - 1)]);
	for (k = 0; k < rb; k++) {
		const uint64_t y = (uint64_t)(mul_before) << 15;
		double level = (double)y / (double)ONE *
		               pow(2.0, -16.0 * (double)k / rb);
		const int expect = dc_out((uint32_t)(65535.0 *
		                          floor(level * 32768.0) / 32768.0));

		for (f = 0; f < 16; f++)
			CHECK(abs(g_buf[2 * (keyoff_frame + 16 * k + f)] - expect) <= 3,
			      "%s: release block %u frame %u: %d, real curve %d", what,
			      k, f, g_buf[2 * (keyoff_frame + 16 * k + f)], expect);
	}
	for (f = keyoff_frame + 16 * rb; f < keyoff_frame + 16 * rb + 64; f++)
		CHECK(g_buf[2 * f] == 0, "%s: silent after the release (%u)", what,
		      f);
}

static void test_release(void)
{
	struct audio_voice_cmd c[8];
	uint32_t n, idx, frac;

	/* during the attack: from the attack level, not the sustain level */
	dc_init();
	n = envelope(c, 0, 0, 10, 10, 16384, 4);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 16 * 7 + 5, 0);
	render_all(c, n, 1200);
	CHECK(g_buf[2 * (16 * 7 + 5)] == dc_out(vol_of(65535,
	      attack_mul(7, 30))), "key-off in the attack: level of block 7 (%d)",
	      g_buf[2 * (16 * 7 + 5)]);
	check_release("attack", 16 * 7 + 5, attack_mul(7, 30), 4);
	CHECK(g_e.ends == 1, "one end counted (%u)", g_e.ends);
	CHECK(audio_voice_position(&g_e, 0, &idx, &frac) == 0,
	      "the voice is inactive after the release");

	/* during the decay */
	dc_init();
	n = envelope(c, 0, 0, 2, 12, 4096, 5);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 6 * 16 + 16 * 9 + 3, 0);
	render_all(c, n, 1600);
	{
		const uint32_t f0 = 6 * 16 + 16 * 9 + 3;

		CHECK(g_buf[2 * f0] > dc_out(vol_of(65535, 4096)) &&
		      g_buf[2 * f0] < dc_out(65535),
		      "key-off in the decay: between sustain and full (%d)",
		      g_buf[2 * f0]);
		CHECK(g_e.ends == 1, "decay key-off counts one end");
		/* the release runs from this very block: its first block is the
		 * decay block's level */
		CHECK(g_buf[2 * f0] == g_buf[2 * (f0 + 15)],
		      "the first release block holds the level it started from");
		CHECK(g_buf[2 * (f0 + 16)] < g_buf[2 * f0],
		      "the second release block is lower");
		CHECK(g_buf[2 * (f0 + 16 * 15)] == 0,
		      "the release of 15 blocks has ended at the first frame after it");
	}

	/* from the sustain level, and on the block boundary of the key-off */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 16384, 3);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 480, 0);
	render_all(c, n, 900);
	CHECK(g_buf[2 * 479] == dc_out(vol_of(65535, 16384)) &&
	      g_buf[2 * 480] == dc_out(vol_of(65535, 16384)),
	      "sustain holds to the key-off (%d, %d)", g_buf[2 * 479],
	      g_buf[2 * 480]);
	check_release("sustain", 480, 16384, 3);
	CHECK(g_e.ends == 1, "sustain key-off counts one end");

	/* a second key-off changes nothing */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 32768, 4);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 100, 0);
	keyoff(c + n++, 140, 0);
	render_all(c, n, 800);
	check_release("double key-off", 100, 32768, 4);
	CHECK(g_e.ends == 1, "double key-off counts one end (%u)", g_e.ends);
}

/* SET volume in the sustain changes the base level only. */
static void test_base_volume(void)
{
	struct audio_voice_cmd c[12];
	uint32_t n, f;

	dc_init();
	n = envelope(c, 0, 0, 0, 0, 12000, 2);
	n += note(c + n, 0, 0, 30000);
	set_cmd(&c[n++], 100, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0,
	        (50000U << 16) | 50000U);
	keyoff(c + n++, 300, 0);
	render_all(c, n, 600);
	CHECK(g_buf[2 * 99] == dc_out(vol_of(30000, 12000)),
	      "before the volume change: %d", g_buf[2 * 99]);
	CHECK(g_buf[2 * 100] == dc_out(vol_of(50000, 12000)),
	      "the volume change is at its frame, scaled by the sustain: %d",
	      g_buf[2 * 100]);
	for (f = 100; f < 300; f++)
		CHECK(g_buf[2 * f] == dc_out(vol_of(50000, 12000)),
		      "the sustain level stays (frame %u)", f);
	/* the release runs from the sustain level at the new base volume */
	CHECK(g_buf[2 * 300] == dc_out(vol_of(50000, 12000)),
	      "release starts from the sustain level");

	/* in the attack the envelope keeps its schedule */
	dc_init();
	n = envelope(c, 0, 0, 3, 0, 32768, 2);
	n += note(c + n, 0, 0, 40000);
	set_cmd(&c[n++], 70, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0,
	        (60000U << 16) | 60000U);
	render_all(c, n, 300);
	for (f = 0; f < 300; f++) {
		const uint32_t blk = f / 16;
		const uint32_t mul = blk < 9 ? attack_mul(blk, 9) : 32768;
		const uint32_t base = f < 70 ? 40000 : 60000;

		CHECK(g_buf[2 * f] == dc_out(vol_of(base, mul)),
		      "attack with a volume change, frame %u: %d want %d", f,
		      g_buf[2 * f], dc_out(vol_of(base, mul)));
	}
}

/* The ramp and the envelope multiply, per frame, as documented. */
static void test_ramp_and_envelope(void)
{
	struct audio_voice_cmd c[12];
	uint32_t n, f;
	int32_t inc;
	const uint32_t ramp = 100;

	dc_init();
	set_cmd(&c[0], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, ramp,
	        0);
	n = 1;
	n += envelope(c + n, 0, 0, 2, 0, 20000, 3);
	n += note(c + n, 10, 0, 60000);
	set_cmd(&c[n++], 200, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0,
	        (20000U << 16) | 20000U);
	render_all(c, n, 500);
	/* START ramps from silence to 60000, SET ramps from there to 20000 */
	inc = (int32_t)((60000U << 8) / ramp);
	for (f = 10; f < 500; f++) {
		const uint32_t rel = f - 10;
		const uint32_t blk = rel / 16;
		const uint32_t mul = blk < 6 ? attack_mul(blk, 6) : 20000U;
		uint32_t base;
		int32_t gain;

		if (f < 110) {
			gain = (int32_t)(inc * (int32_t)(f - 10));
		} else if (f < 200) {
			gain = (int32_t)(60000U << 8);
		} else if (f < 300) {
			const int32_t ginc = ((int32_t)(20000U << 8) -
			                      (int32_t)(60000U << 8)) / (int32_t)ramp;

			gain = (int32_t)(60000U << 8) + ginc * (int32_t)(f - 200);
		} else {
			gain = (int32_t)(20000U << 8);
		}
		if (f == 110)
			gain = (int32_t)(60000U << 8);
		base = (uint32_t)(gain >> 8);
		CHECK(g_buf[2 * f] == dc_out(vol_of(base, mul)),
		      "ramp x envelope, frame %u: %d want %d", f, g_buf[2 * f],
		      dc_out(vol_of(base, mul)));
	}
}

/* Zero-length stages. */
static void test_zero_stages(void)
{
	struct audio_voice_cmd c[8];
	uint32_t n, idx, frac;

	/* all zero but the sustain: the level is the sustain from the first
	 * frame, and a key-off with no release ends the voice at that frame */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 8192, 0);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 50, 0);
	render_all(c, n, 100);
	CHECK(g_buf[0] == dc_out(vol_of(65535, 8192)) &&
	      g_buf[2 * 49] == dc_out(vol_of(65535, 8192)),
	      "no attack, no decay: the sustain level at once (%d)", g_buf[0]);
	CHECK(g_buf[2 * 50] == 0 && g_buf[2 * 99] == 0,
	      "release 0 ends at the key-off frame");
	CHECK(g_e.ends == 1 && audio_voice_position(&g_e, 0, &idx, &frac) == 0,
	      "release 0 counts one end");

	/* no attack: the decay starts at full level */
	dc_init();
	n = envelope(c, 0, 0, 0, 3, 8192, 1);
	n += note(c + n, 0, 0, 65535);
	render_all(c, n, 200);
	CHECK(g_buf[0] == dc_out(65535 * 32768U >> 15),
	      "no attack: the first block is full level (%d)", g_buf[0]);
	CHECK(g_buf[2 * 16 * 9] == dc_out(vol_of(65535, 8192)),
	      "decay of 3 ms is 9 blocks (%d)", g_buf[2 * 16 * 9]);

	/* no decay: the attack ends at the sustain level, the peak is skipped */
	dc_init();
	n = envelope(c, 0, 0, 2, 0, 8192, 1);
	n += note(c + n, 0, 0, 65535);
	render_all(c, n, 200);
	CHECK(g_buf[2 * (16 * 5 + 15)] == dc_out(vol_of(65535,
	      attack_mul(5, 6))), "attack block 5 (%d)", g_buf[2 * (16 * 5 + 15)]);
	CHECK(g_buf[2 * 16 * 6] == dc_out(vol_of(65535, 8192)),
	      "no decay: the sustain level follows the attack (%d)",
	      g_buf[2 * 16 * 6]);

	/* sustain 0: the voice sounds the decay, then stays silent but active */
	dc_init();
	n = envelope(c, 0, 0, 0, 1, 0, 1);
	n += note(c + n, 0, 0, 65535);
	render_all(c, n, 200);
	CHECK(g_buf[2 * 100] == 0 && audio_voice_position(&g_e, 0, &idx, &frac),
	      "sustain 0 is silent and the voice stays active");
	CHECK(g_e.ends == 0, "no end without a key-off");
}

/* A START restarts the attack; with ramping the old sound is a tail that
 * keeps its own envelope. */
static void test_retrigger(void)
{
	struct audio_voice_cmd c[12];
	uint32_t n, f;

	dc_init();
	n = envelope(c, 0, 0, 5, 0, 32768, 4);
	n += note(c + n, 0, 0, 65535);
	set_cmd(&c[n++], 400, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	set_cmd(&c[n++], 400, 0, AUDIO_VOICE_OP_LOOP, 0, 0, sizeof(g_dc));
	render_all(c, n, 800);
	CHECK(g_buf[2 * 399] == dc_out(65535 * 32768U >> 15), "full before");
	CHECK(g_buf[2 * 400] == 0, "the attack restarts from silence (%d)",
	      g_buf[2 * 400]);
	for (f = 400; f < 800; f++) {
		const uint32_t blk = (f - 400) / 16;

		CHECK(g_buf[2 * f] == dc_out(vol_of(65535,
		      blk < 15 ? attack_mul(blk, 15) : 32768)),
		      "second attack, frame %u: %d", f, g_buf[2 * f]);
	}

	/* with ramping: the tail plays on at the level of its envelope while
	 * its gain falls to 0; the new voice rises over the same frames */
	dc_init();
	set_cmd(&c[0], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, 64,
	        0);
	n = 1;
	n += envelope(c + n, 0, 0, 0, 0, 16384, 4);
	n += note(c + n, 0, 0, 40000);
	set_cmd(&c[n++], 400, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	set_cmd(&c[n++], 400, 0, AUDIO_VOICE_OP_LOOP, 0, 0, sizeof(g_dc));
	render_all(c, n, 800);
	/* the tail: gain 40000 << 8 down to 0 over 64 frames at 16384; the
	 * new voice: gain 0 up to 40000 << 8 at the new attack's level, A = 0 so
	 * the new voice is at the sustain level too */
	for (f = 400; f < 464; f++) {
		const int32_t down = -(int32_t)((40000U << 8) / 64);
		const int32_t up = (int32_t)((40000U << 8) / 64);
		const int32_t gt = (int32_t)(40000U << 8) + down * (int32_t)(f - 400);
		const int32_t gn = up * (int32_t)(f - 400);
		const int expect = dc_out(vol_of((uint32_t)(gt >> 8), 16384)) +
		                   dc_out(vol_of((uint32_t)(gn >> 8), 16384));

		CHECK(g_buf[2 * f] == expect, "tail plus new voice, frame %u: %d "
		      "want %d", f, g_buf[2 * f], expect);
	}
	CHECK(g_buf[2 * 464] == dc_out(vol_of(40000, 16384)),
	      "after the ramp only the new voice sounds");
}

/* Later records for the voice never touch the tail, including key-offs and
 * ENVELOPE records; a tail's release end is not an end. */
static void test_frozen_tail(void)
{
	static int16_t ref[2 * 600];
	struct audio_voice_cmd c[24];
	uint32_t n, base_n, f;

	/* the tail's sound is a looped DC at a rate; the new voice at volume 0
	 * is silent, so the output is the tail alone */
	dc_init();
	set_cmd(&c[0], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, 256,
	        0);
	n = 1;
	n += envelope(c + n, 0, 0, 0, 20, 12000, 20);
	n += note(c + n, 0, 0, 50000);
	set_cmd(&c[n++], 300, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0, 0);
	keyoff(c + n++, 299, 0);
	set_cmd(&c[n++], 400, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	set_cmd(&c[n++], 400, 0, AUDIO_VOICE_OP_LOOP, 0, 0, sizeof(g_dc));
	base_n = n;
	render_all(c, n, 800);
	memcpy(ref, g_buf, sizeof(ref));
	CHECK(g_e.ends == 0, "no end yet; the release was cut by the restart (%u)",
	      g_e.ends);

	dc_init();
	n = base_n;
	envelope(&c[n++], 410, 0, 1, 1, 100, 1);
	keyoff(&c[n++], 420, 0);
	set_cmd(&c[n++], 430, 0, AUDIO_VOICE_OP_LOOP, 0, 0, 16);
	set_cmd(&c[n++], 440, 0, AUDIO_VOICE_OP_DIRECTION, 1, 0, 0);
	set_cmd(&c[n++], 450, 0, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	/* the records after the restart address the new voice (silent: its
	 * volume was set to 0 before the START). The tail must be unchanged,
	 * so the output is the same as without them */
	render_all(c, n, 800);
	for (f = 400; f < 600; f++)
		CHECK(g_buf[2 * f] == ref[2 * f],
		      "the tail is unchanged by later records (frame %u)", f);
	CHECK(g_e.ends == 0, "a tail's release is not counted (%u)", g_e.ends);
}

/* Long releases of low gains end on time, not when the level rounds to 0. */
static void test_long_release(void)
{
	static const uint32_t ms[] = { 10000, 65535 };
	static const uint32_t vol[] = { 1, 65535 };
	uint32_t i, j;

	for (i = 0; i < 2; i++) {
		for (j = 0; j < 2; j++) {
			struct audio_voice_cmd c[6];
			const uint32_t keyoff_frame = 500;
			/* the last frame that sounds */
			const uint32_t last = keyoff_frame + 48U * ms[i] - 1U;
			uint32_t n, frame = 0, idx, frac;
			size_t used = 0;
			int alive = 1;

			dc_init();
			n = envelope(c, 0, 0, 0, 0, 32768, ms[i]);
			n += note(c + n, 0, 0, vol[j]);
			keyoff(c + n++, keyoff_frame, 0);
			while (frame < last) {
				const uint32_t len = last - frame < 4800 ? last - frame : 4800;

				used += audio_voice_render(&g_e, g_buf, len, c + used,
				                           n - used);
				frame += len;
				alive &= audio_voice_position(&g_e, 0, &idx, &frac) != 0;
			}
			CHECK(used == n, "release %u ms vol %u: records consumed", ms[i],
			      vol[j]);
			CHECK(alive && g_e.ends == 0,
			      "release %u ms vol %u: active until its last frame, no end "
			      "yet (%u)", ms[i], vol[j], g_e.ends);
			audio_voice_render(&g_e, g_buf, 1, NULL, 0);
			CHECK(audio_voice_position(&g_e, 0, &idx, &frac) == 0 &&
			      g_e.ends == 1,
			      "release %u ms vol %u: ends after its last frame (ends %u)",
			      ms[i], vol[j], g_e.ends);
		}
	}
}

/* Which ends count. */
static void test_end_counting(void)
{
	struct audio_voice_cmd c[24];
	uint32_t n, idx, frac;

	/* a plain STOP counts nothing */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 32768, 4);
	n += note(c + n, 0, 0, 65535);
	set_cmd(&c[n++], 100, 0, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	render_all(c, n, 400);
	CHECK(g_e.ends == 0 && g_buf[2 * 100] == 0,
	      "plain STOP silences at once and counts no end (%u)", g_e.ends);

	/* a one-shot sample that ends inside its release counts nothing */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 32768, 100);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_SET,
	        AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
	        AUDIO_VOICE_SET_VOLUME, 48000, (65535U << 16) | 65535U);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	keyoff(c + n++, 10, 0);
	render_all(c, n, 1000);
	CHECK(audio_voice_position(&g_e, 0, &idx, &frac) == 0 && g_e.ends == 0,
	      "a pass end inside the release is not a release end (%u)",
	      g_e.ends);

	/* a stop ramp that ends before the release counts nothing; a release
	 * that ends before the stop ramp counts one */
	dc_init();
	set_cmd(&c[0], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, 64,
	        0);
	n = 1;
	n += envelope(c + n, 0, 0, 0, 0, 32768, 100);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 500, 0);
	set_cmd(&c[n++], 510, 0, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	render_all(c, n, 900);
	CHECK(g_e.ends == 0 && audio_voice_position(&g_e, 0, &idx, &frac) == 0,
	      "a plain STOP during the release ends it without an end (%u)",
	      g_e.ends);

	dc_init();
	set_cmd(&c[0], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, 512,
	        0);
	n = 1;
	n += envelope(c + n, 0, 0, 0, 0, 32768, 1);
	n += note(c + n, 0, 0, 65535);
	set_cmd(&c[n++], 600, 0, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	keyoff(c + n++, 605, 0);
	render_all(c, n, 1300);
	CHECK(g_e.ends == 0, "a key-off during a stop ramp changes nothing (%u)",
	      g_e.ends);

	/* each voice counts its own end once, and a tail's release is no end */
	dc_init();
	n = 0;
	n += envelope(c + n, 0, 0, 0, 0, 32768, 1);
	n += envelope(c + n, 0, 1, 0, 0, 32768, 2);
	n += note(c + n, 0, 0, 20000);
	n += note(c + n, 0, 1, 20000);
	keyoff(c + n++, 100, 0);
	keyoff(c + n++, 100, 1);
	render_all(c, n, 600);
	CHECK(g_e.ends == 2, "two voices, two ends (%u)", g_e.ends);
	dc_init();
	n = 0;
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, 300,
	        0);
	n += envelope(c + n, 0, 0, 0, 0, 32768, 3);
	n += note(c + n, 0, 0, 30000);
	keyoff(c + n++, 100, 0);
	set_cmd(&c[n++], 130, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	render_all(c, n, 700);
	CHECK(g_e.ends == 0, "the released tail's end is not counted (%u)",
	      g_e.ends);
	CHECK(fades_in_use(&g_e) == 0, "the tail's slot is free once its release "
	      "ended");
}

/* ENVELOPE validation and when a setting acts. */
static void test_records(void)
{
	struct audio_voice_cmd c[24];
	uint32_t n;

	dc_init();
	n = 0;
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 1, 0, 0x8001U << 16);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 2, 0, 0);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 3, 0, 0);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 0, 1, 0);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 0, 0, 1);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 0x8000, 0, 0);
	set_cmd(&c[n++], 0, 32, AUDIO_VOICE_OP_ENVELOPE, 1, 0, 0);
	set_cmd(&c[n++], 0, 200, AUDIO_VOICE_OP_ENVELOPE, 0, 0, 0);
	/* valid: the unity sustain, the longest times, and the clearing record */
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 1,
	        (65535U << 16) | 65535U, (0x8000U << 16) | 65535U);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 1, 0, 0);
	set_cmd(&c[n++], 0, 1, AUDIO_VOICE_OP_ENVELOPE, 0, 0, 0);
	render_all(c, n, 16);
	CHECK(g_e.rejected == 8, "eight invalid records counted (%u)",
	      g_e.rejected);
	CHECK(g_e.voice[0].env_on == 1 && g_e.voice[0].env_set.sustain == 0 &&
	      g_e.voice[0].env_set.attack == 0,
	      "the last valid record is the setting");

	/* a rejected record leaves the setting; the setting acts at START */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 8192, 2);
	n += note(c + n, 0, 0, 65535);
	set_cmd(&c[n++], 50, 0, AUDIO_VOICE_OP_ENVELOPE, 1, 0, 0x9000U << 16);
	n += envelope(c + n, 60, 0, 0, 0, 32768, 2);
	set_cmd(&c[n++], 100, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	set_cmd(&c[n++], 100, 0, AUDIO_VOICE_OP_LOOP, 0, 0, sizeof(g_dc));
	render_all(c, n, 200);
	CHECK(g_buf[2 * 55] == dc_out(vol_of(65535, 8192)) &&
	      g_buf[2 * 99] == dc_out(vol_of(65535, 8192)),
	      "a sounding voice keeps the envelope it started with (%d %d)",
	      g_buf[2 * 55], g_buf[2 * 99]);
	CHECK(g_buf[2 * 100] == dc_out(vol_of(65535, 32768)),
	      "the next START uses the new setting (%d)", g_buf[2 * 100]);

	/* clearing: the next START plays at the plain volume, and a key-off
	 * is a plain STOP */
	dc_init();
	n = envelope(c, 0, 0, 0, 0, 8192, 2);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_ENVELOPE, 0, 0, 0);
	n += note(c + n, 0, 0, 65535);
	keyoff(c + n++, 100, 0);
	render_all(c, n, 200);
	CHECK(g_buf[0] == dc_out(65535) && g_buf[2 * 99] == dc_out(65535),
	      "no envelope: plain volume (%d)", g_buf[0]);
	CHECK(g_buf[2 * 100] == 0 && g_e.ends == 0,
	      "a key-off without an envelope is a plain STOP, no end");
}

/* The envelope setting does not depend on where a render call splits. */
static void test_split_boundaries(void)
{
	static int16_t whole[2 * 2400];
	static int16_t pieces[2 * 2400];
	struct audio_voice_cmd c[16];
	uint32_t n, p, f;
	static const uint32_t keyoffs[] = { 959, 960, 961, 1000, 1008, 1919 };
	uint32_t ko;

	for (ko = 0; ko < sizeof(keyoffs) / sizeof(keyoffs[0]); ko++) {
		dc_init();
		set_cmd(&c[0], 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP,
		        32, 0);
		n = 1;
		n += envelope(c + n, 0, 0, 12, 30, 9000, 20);
		n += note(c + n, 5, 0, 52000);
		set_cmd(&c[n++], 700, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME,
		        0, (21000U << 16) | 21000U);
		keyoff(c + n++, keyoffs[ko], 0);
		audio_voice_render(&g_e, whole, 2400, c, n);
		for (p = 1; p < 200; p++) {
			uint32_t done = 0;
			size_t used = 0;

			dc_init();
			while (done < 2400) {
				const uint32_t len = done == 0 ? p : (done + 977 <= 2400 ?
				                                      977 : 2400 - done);

				used += audio_voice_render(&g_e, pieces + 2 * done, len,
				                           c + used, n - used);
				done += len;
			}
			for (f = 0; f < 2 * 2400; f++)
				if (whole[f] != pieces[f])
					break;
			CHECK(f == 2 * 2400, "key-off %u split at %u: differs at "
			      "frame %u (%d vs %d)", keyoffs[ko], p, f / 2, whole[f],
			      pieces[f]);
			if (f != 2 * 2400)
				return;
		}
	}
}

/* A key-off with a flag and no envelope, and envelope records to voices that
 * never get a START: legacy output as the frozen engine. */
static void test_without_envelope_is_baseline(void)
{
	static struct audio_voice_baseline_engine b;
	static struct audio_voice_engine e;
	static int16_t ob[2 * 1024], oe[2 * 1024];
	static struct audio_voice_baseline_cmd bc[160];
	static struct audio_voice_cmd ec[160];
	uint32_t s;

	for (s = 0; s < 3000; s++) {
		uint32_t len = 64 + (s * 37) % 4000, i, n = 0, v, frames;

		rng = 0xE17E + s;
		for (i = 0; i < len; i++)
			g_bank[i] = (uint8_t)rnd(256);
		audio_voice_baseline_init(&b, g_bank, len, AUDIO_VOICE_CLOCK_PAL);
		audio_voice_init(&e, g_bank, len, AUDIO_VOICE_CLOCK_PAL);
		b.interp = e.interp = s & 1;
		audio_voice_baseline_sample_set(&b, 0, 0, len, s % 4);
		audio_voice_sample_set(&e, 0, 0, len, s % 4);
		for (v = 0; v < 6; v++) {
			const uint32_t stamp = rnd(60);

			set_cmd(&ec[n++], stamp, v, AUDIO_VOICE_OP_SET,
			        AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
			        AUDIO_VOICE_SET_VOLUME, pick_hz(),
			        (pick_vol() << 16) | pick_vol());
			set_cmd(&ec[n++], stamp, v, AUDIO_VOICE_OP_START, 0, 0, 0);
			set_cmd(&ec[n++], stamp, v, AUDIO_VOICE_OP_LOOP, 0,
			        rnd(len / 4 + 1), rnd(2) ? len : 0);
		}
		/* mixed in: STOPs with any flags, and records of the new ops that
		 * leave no voice with an envelope (the frozen engine rejects them,
		 * which only changes its counter) */
		for (i = 0; i < 30; i++) {
			uint32_t stamp = 60 + rnd(800), vv = rnd(6);

			if (rnd(2))
				set_cmd(&ec[n++], stamp, vv, AUDIO_VOICE_OP_STOP, rnd(65536),
				        0, 0);
			else
				set_cmd(&ec[n++], stamp, vv, AUDIO_VOICE_OP_ENVELOPE, 0, 0,
				        0);
		}
		for (i = 1; i < n; i++) {
			struct audio_voice_cmd t = ec[i];
			uint32_t j = i;

			while (j && ec[j - 1].stamp > t.stamp) {
				ec[j] = ec[j - 1];
				j--;
			}
			ec[j] = t;
		}
		for (i = 0; i < n; i++) {
			bc[i].stamp = ec[i].stamp;
			bc[i].voice = ec[i].voice;
			bc[i].op = ec[i].op;
			bc[i].flags = ec[i].flags;
			bc[i].arg0 = ec[i].arg0;
			bc[i].arg1 = ec[i].arg1;
		}
		frames = 1024;
		audio_voice_baseline_render(&b, ob, frames, bc, n);
		audio_voice_render(&e, oe, frames, ec, n);
		if (memcmp(ob, oe, sizeof(ob)) != 0) {
			CHECK(0, "sequence %u differs from the frozen engine", s);
			return;
		}
	}
}

/* Cubic reads a constant sample as that constant, so a cubic voice with a
 * constant envelope level equals a voice of the scaled volume exactly. */
static void test_cubic_scaling(void)
{
	static int16_t plain[2 * 400], enveloped[2 * 400];
	static struct audio_voice_engine e2;
	struct audio_voice_cmd c[8];
	uint32_t n, mode, f;

	for (mode = 0; mode < 3; mode++) {
		const uint32_t sus = 11000, base = 47000;
		const uint32_t ev = vol_of(base, sus);

		dc_init();
		g_e.interp = mode;
		n = envelope(c, 0, 0, 0, 0, sus, 2);
		n += note(c + n, 0, 0, base);
		audio_voice_render(&g_e, enveloped, 400, c, n);
		audio_voice_init(&e2, g_dc, sizeof(g_dc), AUDIO_VOICE_CLOCK_PAL);
		audio_voice_sample_set(&e2, 0, 0, sizeof(g_dc), AUDIO_VOICE_FMT_S16);
		e2.interp = mode;
		n = note(c, 0, 0, ev);
		audio_voice_render(&e2, plain, 400, c, n);
		for (f = 0; f < 2 * 400; f++)
			if (plain[f] != enveloped[f])
				break;
		CHECK(f == 2 * 400, "interp %u: a constant envelope equals the "
		      "scaled volume (frame %u: %d vs %d)", mode, f / 2, enveloped[f],
		      plain[f]);
	}
}

/* 32 voices and 32 tails with envelopes in every stage: the model agrees
 * with the engine on the sum. */
static void test_full_pool(void)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	static int16_t oe[2 * 1500], om[2 * 1500];
	static struct audio_voice_cmd c[4096];
	uint32_t v, n = 0, pass;

	rng = 0x5EED;
	for (v = 0; v < 64; v++)
		g_bank[v] = (uint8_t)(v * 7 + 3);
	audio_voice_init(&e, g_bank, 64, AUDIO_VOICE_CLOCK_PAL);
	r_init(&m, g_bank, 64, AUDIO_VOICE_CLOCK_PAL);
	e.interp = AUDIO_VOICE_INTERP_LINEAR;
	m.interp = AUDIO_VOICE_INTERP_LINEAR;
	audio_voice_sample_set(&e, 0, 0, 64, AUDIO_VOICE_FMT_S8);
	r_sample_set(&m, 0, 0, 64, AUDIO_VOICE_FMT_S8);
	set_cmd(&c[n++], 0, 0, AUDIO_VOICE_OP_CONFIG, 1, 256, 0);
	for (v = 0; v < 32; v++) {
		set_cmd(&c[n++], 0, v, AUDIO_VOICE_OP_ENVELOPE, 1,
		        ((1 + v % 5) << 16) | (2 + v % 7),
		        ((4000U + 700U * v) << 16) | (1 + v % 4));
		set_cmd(&c[n++], 0, v, AUDIO_VOICE_OP_SET,
		        AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
		        AUDIO_VOICE_SET_VOLUME, 8000 + 2000 * v,
		        (2000U << 16) | 1500U);
		set_cmd(&c[n++], 0, v, AUDIO_VOICE_OP_START, 0, 0, 0);
		set_cmd(&c[n++], 0, v, AUDIO_VOICE_OP_LOOP, 0, 0, 64);
	}
	for (pass = 1; pass < 27; pass++) {
		for (v = 0; v < 32; v++) {
			const uint32_t stamp = 256U * pass + 8U * v;

			set_cmd(&c[n++], stamp, v, AUDIO_VOICE_OP_START, 0, 0, 0);
			set_cmd(&c[n++], stamp, v, AUDIO_VOICE_OP_LOOP, 0, 0, 64);
			if ((v + pass) % 7 == 0)
				keyoff(&c[n++], stamp + 100, v);
		}
	}
	{
		uint32_t i, j;

		for (i = 1; i < n; i++) {
			struct audio_voice_cmd t = c[i];

			j = i;
			while (j && c[j - 1].stamp > t.stamp) {
				c[j] = c[j - 1];
				j--;
			}
			c[j] = t;
		}
	}
	{
		uint32_t frame = 0, used_e = 0, used_m = 0, pool = 0;

		while (frame < 7000) {
			used_e += (uint32_t)audio_voice_render(&e, oe, 1500, c + used_e,
			                                       n - used_e);
			used_m += (uint32_t)r_render(&m, om, 1500, c + used_m,
			                             n - used_m);
			CHECK(memcmp(oe, om, sizeof(oe)) == 0,
			      "the full pool agrees with the model at frame %u", frame);
			CHECK(same_state(&e, &m, 0, "full pool"),
			      "state at frame %u", frame);
			pool += fades_in_use(&e) >= 24;
			frame += 1500;
		}
		CHECK(pool != 0, "the pool was nearly full with envelopes running");
	}
}

int main(void)
{
	const char *env;
	uint32_t seqs = 6000, seq, done = 0;
	uint64_t seed = 1;

	if ((env = getenv("ZZ9K_VOICE_ENV_SEQS")) != NULL)
		seqs = (uint32_t)strtoul(env, NULL, 0);
	if ((env = getenv("ZZ9K_VOICE_ENV_SEED")) != NULL)
		seed = strtoull(env, NULL, 0);
#ifdef HAVE_ALARM
	alarm(120U + seqs / 50U);
#endif
	init_table();
	/* the table the model builds is the documented one */
	CHECK(g_tab[0] == 2147483648U && g_tab[1] == 2141676973U &&
	      g_tab[128] == 1518500250U && g_tab[256] == 1073741824U,
	      "the model's 2^x table");
	test_stage_boundaries();
	test_release();
	test_base_volume();
	test_ramp_and_envelope();
	test_zero_stages();
	test_retrigger();
	test_frozen_tail();
	test_long_release();
	test_end_counting();
	test_records();
	test_split_boundaries();
	test_cubic_scaling();
	test_full_pool();
	test_without_envelope_is_baseline();
	for (seq = 0; seq < seqs && failures < 5; seq++) {
		if (!run_sequence(seq, seed))
			failures++;
		done++;
	}
	if (failures) {
		printf("audio_voice_envelope_test: %d FAILURES\n", failures);
		return 1;
	}
	if (seqs >= 1000U && (st.with_env * 10U < st.renders ||
	    st.in_attack == 0U || st.in_decay == 0U || st.in_sustain == 0U ||
	    st.in_release == 0U || st.tails * 10U < st.renders ||
	    st.ramping * 5U < st.renders || st.audible * 2U < st.renders ||
	    st.slot_env == 0U)) {
		printf("audio_voice_envelope_test: corpus too weak (renders %u, with "
		       "an envelope %u, attack %u, decay %u, sustain %u, release %u, "
		       "tails %u, ramping %u, audible %u, slot envelopes %u)\n",
		       st.renders, st.with_env, st.in_attack, st.in_decay,
		       st.in_sustain, st.in_release, st.tails, st.ramping,
		       st.audible, st.slot_env);
		return 1;
	}
	printf("audio_voice_envelope_test: directed cases, the baseline and %u "
	       "random sequences against the model passed (seed %llu); %u "
	       "renders, %u with an envelope sounding, %u with tails, %u with a "
	       "ramp running, %u with a tail that has an envelope\n", done,
	       (unsigned long long)seed, st.renders, st.with_env, st.tails,
	       st.ramping, st.slot_env);
	return 0;
}
