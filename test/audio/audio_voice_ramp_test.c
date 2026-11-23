/*
 * Volume ramping and fade slots of the voice engine (CONFIG ramp, "Ramping"
 * in audio_voice.h).
 *
 * The reference is a model written from the rules in that comment, frame by
 * frame, with none of the engine's run planning, int32 blend or vector
 * code: per frame it reads each voice (nearest, or linear with the 64-bit
 * blend), adds (sample * volume) >> 16, steps the position by the original
 * advance rules and the ramp by one. Three kinds of check use it:
 *
 *   - the model with ramping off against the frozen baseline engine, which
 *     ties the model to the engine that shipped;
 *   - random sequences, with CONFIG ramp, restarts that fill the fade pool,
 *     stops and volume changes, against the engine: byte-identical PCM,
 *     and equal voice state and fade slots in use after every render;
 *   - directed cases with expected values worked out by hand (ramp
 *     shape, restart tail, pool exhaustion, records that must not touch a
 *     tail, CONFIG validation).
 *
 * Environment: ZZ9K_VOICE_RAMP_SEQS (default 20000), ZZ9K_VOICE_RAMP_SEED.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

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

struct rvoice {
	int active, backward, stopping, wide, stereo;
	uint32_t shift, base, len, pos, frac, end, ls, ll, step, vl, vr, left;
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
	v->active = 1;
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

		if (!v->active)
			continue;
		if (v->step == 0) {
			r_tick(v);
			continue;
		}
		vl = v->left ? (v->gl >> 8) : (int32_t)v->vl;
		vr = v->left ? (v->gr >> 8) : (int32_t)v->vr;
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
	/* The engine applies a command whose stamp falls in this call at that
	 * frame; one stamped before the call applies at frame 0 and is late,
	 * which the loop above does (rel < 0 only at frame 0 of a call: the
	 * frame counter then matches the stamp). */
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
	    e->ramp_frames != m->ramp) {
		printf("FAIL seq %u %s: engine frame %u/%u late %u/%u rejected "
		       "%u/%u master %u/%u ramp %u/%u\n", seq, when, e->frame,
		       m->frame, e->late, m->late, e->rejected, m->rejected,
		       e->master, m->master, e->ramp_frames, m->ramp);
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
		                   a->stopping != b->stopping))) {
			printf("FAIL seq %u %s: voice %u active %d/%d pos %u/%u frac "
			       "%u/%u\n", seq, when, k, a->active, b->active, a->pos,
			       b->pos, a->frac, b->frac);
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
static int16_t g_out_e[2 * 4096], g_out_m[2 * 4096];
static struct audio_voice_cmd g_cmds[1024];

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
	uint32_t renders, with_fade, full_pool, ramping, stopping, audible;
} st;

static void note_render(const struct audio_voice_engine *e,
                        const int16_t *out, uint32_t frames)
{
	uint32_t k, ramping = 0, stopping = 0, audible = 0;

	for (k = 0; k < 2 * frames; k++)
		audible |= out[k] != 0;
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		ramping |= e->voice[k].active && e->voice[k].ramp_left != 0;
		stopping |= e->voice[k].active && e->voice[k].stopping;
	}
	st.renders++;
	st.with_fade += fades_in_use(e) != 0;
	st.full_pool += fades_in_use(e) == AUDIO_VOICE_FADE_SLOTS;
	st.ramping += ramping != 0;
	st.stopping += stopping != 0;
	st.audible += audible;
}

static int run_sequence(uint32_t seq, uint64_t seed)
{
	static struct audio_voice_engine e;
	static struct rmodel m;
	uint32_t bank_len = 64 + rnd(sizeof(g_bank) - 64), i, nvoices, nsamp;
	uint32_t interp = rnd(2), storm = rnd(5) == 0, ncmds = 0, frame = 0;
	uint32_t r, renders = 2 + rnd(4);
	uint32_t smp_len[8];

	rng = seed + (uint64_t)seq * 0xD1B54A32D192ED03ULL;
	(void)rnd(2);
	{
		int32_t walk = 0;
		uint32_t kind = rnd(4);

		for (i = 0; i < bank_len; i++) {
			uint32_t v = rnd(256);

			walk += (int32_t)(v % 9) - 4;
			g_bank[i] = kind == 0 ? (uint8_t)(v & 1 ? 0x7F : 0x80) :
			            kind == 1 ? (uint8_t)v : (uint8_t)walk;
		}
	}
	audio_voice_init(&e, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	r_init(&m, g_bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	e.interp = m.interp = interp ? AUDIO_VOICE_INTERP_LINEAR :
	                              AUDIO_VOICE_INTERP_NEAREST;
	nsamp = 1 + rnd(8);
	for (i = 0; i < nsamp; i++) {
		uint32_t fmt = rnd(4), off = rnd(bank_len), len;
		int rc_e, rc_m;

		len = rnd(bank_len - off + 1);
		if (rnd(3) == 0)
			len = bank_len - off;
		rc_e = audio_voice_sample_set(&e, i, off, len, fmt);
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
		uint32_t extra = storm ? 150 + rnd(400) : rnd(60);
		size_t ue, um;

		if (r == 0) {
			/* every voice: rate, volume, start, loop; and the ramp */
			uint32_t v;

			memset(&g_cmds[n], 0, sizeof(g_cmds[n]));
			g_cmds[n].stamp = frame;
			g_cmds[n].op = AUDIO_VOICE_OP_CONFIG;
			g_cmds[n].flags = 1;
			g_cmds[n].arg0 = pick_ramp();
			n++;
			for (v = 0; v < nvoices && n + 4 < 300; v++) {
				uint32_t id = rnd(nsamp);

				memset(&g_cmds[n], 0, sizeof(g_cmds[n]));
				g_cmds[n].stamp = frame + rnd(40);
				g_cmds[n].voice = v;
				g_cmds[n].op = AUDIO_VOICE_OP_SET;
				g_cmds[n].flags = AUDIO_VOICE_SET_RATE |
				                  AUDIO_VOICE_RATE_HZ |
				                  AUDIO_VOICE_SET_VOLUME;
				g_cmds[n].arg0 = pick_hz();
				g_cmds[n].arg1 = (pick_vol() << 16) | pick_vol();
				n++;
				memset(&g_cmds[n], 0, sizeof(g_cmds[n]));
				g_cmds[n].stamp = g_cmds[n - 1].stamp;
				g_cmds[n].voice = v;
				g_cmds[n].op = AUDIO_VOICE_OP_START;
				g_cmds[n].flags = id;
				g_cmds[n].arg0 = rnd(3) ? 0 : rnd(smp_len[id] + 1);
				n++;
				if (rnd(2)) {
					memset(&g_cmds[n], 0, sizeof(g_cmds[n]));
					g_cmds[n].stamp = g_cmds[n - 1].stamp;
					g_cmds[n].voice = v;
					g_cmds[n].op = AUDIO_VOICE_OP_LOOP;
					g_cmds[n].arg0 = 0;
					g_cmds[n].arg1 = rnd(2) ? smp_len[id] :
					                 rnd(smp_len[id] + 1);
					n++;
				}
			}
		}
		for (i = 0; i < extra && n < 1000; i++) {
			struct audio_voice_cmd *c = &g_cmds[n++];
			uint32_t pick = rnd(100);
			uint32_t id = rnd(nsamp);

			memset(c, 0, sizeof(*c));
			c->stamp = frame + rnd(frames + 1) - (rnd(20) == 0 && frame ? 1 : 0);
			c->voice = (uint8_t)(rnd(30) ? rnd(nvoices) : rnd(40));
			if (pick < (storm ? 55 : 24)) {
				c->op = AUDIO_VOICE_OP_START;
				c->flags = rnd(40) ? id : 200 + rnd(100);
				c->arg0 = rnd(3) ? 0 : rnd(smp_len[id] + 2);
				c->arg1 = rnd(4) ? 0 : rnd(smp_len[id] + 3);
			} else if (pick < 50) {
				c->op = AUDIO_VOICE_OP_SET;
				c->flags = 1 + rnd(7);
				c->arg0 = c->flags & AUDIO_VOICE_RATE_HZ ? pick_hz() :
				          rnd(3) ? 124 + rnd(900) : rnd(10);
				c->arg1 = (pick_vol() << 16) | pick_vol();
			} else if (pick < 64) {
				c->op = AUDIO_VOICE_OP_STOP;
			} else if (pick < 74) {
				c->op = AUDIO_VOICE_OP_LOOP;
				c->arg0 = rnd(smp_len[id] + 1);
				c->arg1 = rnd(3) ? rnd(smp_len[id] - c->arg0 + 1) : 0;
			} else if (pick < 82) {
				c->op = AUDIO_VOICE_OP_DIRECTION;
				c->flags = (uint16_t)rnd(2);
			} else if (pick < 87) {
				c->op = AUDIO_VOICE_OP_MASTER;
				c->arg0 = rnd(2) ? 256 : rnd(600);
			} else if (pick < 95) {
				c->op = AUDIO_VOICE_OP_CONFIG;
				c->flags = rnd(12) ? 1 : rnd(4);
				c->arg0 = rnd(20) ? pick_ramp() : 513 + rnd(1000);
			} else {
				c->op = (uint8_t)(rnd(2) ? 7 + rnd(2) : 10 + rnd(200));
				c->flags = (uint16_t)rnd(65536);
			}
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
		memset(g_out_m, 0x5A, sizeof(g_out_m));
		ue = audio_voice_render(&e, g_out_e, frames, g_cmds, ncmds);
		um = r_render(&m, g_out_m, frames, g_cmds, ncmds);
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
			printf("FAIL seq %u render %u (%u frames, %u records, %s) "
			       "differs at frame %u side %u: engine %d model %d\n", seq,
			       r, frames, ncmds, interp ? "linear" : "nearest", f / 2,
			       f & 1, g_out_e[f], g_out_m[f]);
			return 0;
		}
		if (!same_state(&e, &m, seq, "after render"))
			return 0;
		note_render(&e, g_out_e, frames);
		frame += frames;
	}
	return 1;
}

/* ---- the model against the frozen engine, ramping off ---- */

static int model_vs_baseline(uint32_t sequences)
{
	static struct audio_voice_baseline_engine b;
	static struct rmodel m;
	static int16_t ob[2 * 2048], om[2 * 2048];
	static struct audio_voice_baseline_cmd bc[400];
	static struct audio_voice_cmd mc[400];
	uint32_t s;

	for (s = 0; s < sequences; s++) {
		uint32_t len = 64 + (s * 37) % 4000, i, n = 0, frames, vnum;

		rng = 0xBEEF + s;
		for (i = 0; i < len; i++)
			g_bank[i] = (uint8_t)rnd(256);
		audio_voice_baseline_init(&b, g_bank, len, AUDIO_VOICE_CLOCK_PAL);
		r_init(&m, g_bank, len, AUDIO_VOICE_CLOCK_PAL);
		b.interp = m.interp = s & 1;
		audio_voice_baseline_sample_set(&b, 0, 0, len, s % 4);
		r_sample_set(&m, 0, 0, len, s % 4);
		for (vnum = 0; vnum < 4; vnum++) {
			mc[n].stamp = rnd(100);
			mc[n].voice = vnum;
			mc[n].op = AUDIO_VOICE_OP_SET;
			mc[n].flags = 7;
			mc[n].arg0 = pick_hz();
			mc[n].arg1 = (pick_vol() << 16) | pick_vol();
			n++;
			mc[n] = mc[n - 1];
			mc[n].stamp += 1;
			mc[n].op = AUDIO_VOICE_OP_START;
			mc[n].flags = 0;
			mc[n].arg0 = 0;
			mc[n].arg1 = 0;
			n++;
			mc[n] = mc[n - 1];
			mc[n].op = AUDIO_VOICE_OP_LOOP;
			mc[n].arg0 = rnd(len / 4 + 1);
			mc[n].arg1 = rnd(2) ? len - mc[n].arg0 : 0;
			n++;
		}
		for (i = 0; i < n; i++) {
			bc[i].stamp = mc[i].stamp;
			bc[i].voice = mc[i].voice;
			bc[i].op = mc[i].op;
			bc[i].flags = mc[i].flags;
			bc[i].arg0 = mc[i].arg0;
			bc[i].arg1 = mc[i].arg1;
		}
		/* sorted by stamp: insertion sort both */
		for (i = 1; i < n; i++) {
			struct audio_voice_cmd t = mc[i];
			struct audio_voice_baseline_cmd tb = bc[i];
			uint32_t j = i;

			while (j && mc[j - 1].stamp > t.stamp) {
				mc[j] = mc[j - 1];
				bc[j] = bc[j - 1];
				j--;
			}
			mc[j] = t;
			bc[j] = tb;
		}
		frames = 2000;
		audio_voice_baseline_render(&b, ob, frames, bc, n);
		r_render(&m, om, frames, mc, n);
		if (memcmp(ob, om, 4U * frames) != 0) {
			CHECK(0, "model differs from the baseline engine (sequence %u)",
			      s);
			return 0;
		}
	}
	return 1;
}

/* ---- directed cases ---- */

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

/* A bank of constant samples: sample id k is 16 frames of S16 value
 * values[k], looped. */
static void dc_engine(struct audio_voice_engine *e, const int16_t *values,
                      uint32_t n)
{
	uint32_t i, k;

	for (k = 0; k < n; k++)
		for (i = 0; i < 16; i++) {
			g_bank[(k * 16 + i) * 2] = (uint8_t)((uint16_t)values[k] >> 8);
			g_bank[(k * 16 + i) * 2 + 1] = (uint8_t)values[k];
		}
	audio_voice_init(e, g_bank, n * 32, AUDIO_VOICE_CLOCK_PAL);
	for (k = 0; k < n; k++)
		audio_voice_sample_set(e, k, k * 32, 32, AUDIO_VOICE_FMT_S16);
}

#define SET48(v, vol) \
	mk(0, (v), AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE | \
	   AUDIO_VOICE_RATE_HZ | AUDIO_VOICE_SET_VOLUME, 48000, \
	   ((uint32_t)(vol) << 16) | (vol))

static void test_ramp_shape(void)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 600];
	const int16_t v[1] = { 16000 };
	struct audio_voice_cmd c[5];
	uint32_t j;
	int32_t inc, bad = 0;

	dc_engine(&e, v, 1);
	c[0] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 1, 100, 0);
	c[1] = SET48(0, 0x8000);
	c[2] = mk(0, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	c[3] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	c[4] = mk(300, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0,
	          (0x4000U << 16) | 0x4000U);
	audio_voice_render(&e, out, 600, c, 5);
	/* start: 0 to 0x8000 over 100 frames, then 0x8000 until 300, then
	 * down to 0x4000 over 100 frames */
	inc = ((int32_t)(0x8000 << 8)) / 100;
	for (j = 0; j < 600; j++) {
		int32_t vol, want;

		if (j < 100)
			vol = (inc * (int32_t)j) >> 8;
		else if (j < 300)
			vol = 0x8000;
		else if (j < 400)
			vol = (((int32_t)(0x8000 << 8)) +
			       ((int32_t)((0x4000 - 0x8000) * 256) / 100) *
			       (int32_t)(j - 300)) >> 8;
		else
			vol = 0x4000;
		want = (16000 * vol) >> 16;
		if (want > 32767)
			want = 32767;
		if (out[2 * j] != want || out[2 * j + 1] != want)
			bad++;
	}
	CHECK(bad == 0, "ramp shape: %d of 600 frames differ from the hand model",
	      bad);
	CHECK(out[0] == 0 && out[2 * 99] != 0 && out[2 * 100] == 16000 * 0x8000 / 65536,
	      "ramp shape: starts from silence and reaches the target at frame R");
}

static void test_restart_tail(void)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 400];
	const int16_t v[2] = { 20000, -10000 };
	struct audio_voice_cmd c[7];
	uint32_t j;
	int bad = 0;

	dc_engine(&e, v, 2);
	c[0] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 1, 50, 0);
	c[1] = SET48(0, 0x4000);
	c[2] = mk(0, 0, AUDIO_VOICE_OP_START, 0, 0, 0);
	c[3] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	/* restart at frame 100, when the first sound has been at full volume
	 * for 50 frames */
	c[4] = mk(100, 0, AUDIO_VOICE_OP_START, 1, 0, 0);
	c[5] = mk(100, 0, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	audio_voice_render(&e, out, 400, c, 6);
	CHECK(fades_in_use(&e) == 0, "restart: the tail slot is free after R frames");
	for (j = 100; j < 400; j++) {
		int32_t tail = 0, fresh;
		int32_t tinc = -(int32_t)(0x4000 << 8) / 50;
		int32_t finc = (int32_t)(0x4000 << 8) / 50;
		uint32_t k = j - 100;

		if (k < 50)
			tail = (20000 * (((0x4000 << 8) + tinc * (int32_t)k) >> 8)) >> 16;
		fresh = (-10000 * (k < 50 ? (finc * (int32_t)k) >> 8 : 0x4000)) >> 16;
		if (out[2 * j] != tail + fresh)
			bad++;
	}
	CHECK(bad == 0, "restart: %d frames differ from tail + new sound", bad);
}

static void test_pool_exhaustion(void)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 1200];
	int16_t v[40];
	struct audio_voice_cmd c[200];
	uint32_t i, n = 0, found = 0, k;

	for (i = 0; i < 40; i++)
		v[i] = (int16_t)(100 * (i + 1));
	dc_engine(&e, v, 40);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 1, 512, 0);
	c[n++] = SET48(0, 0xFFFF);
	/* 34 starts of the same voice, 8 frames apart, each on its own sample:
	 * start 1 has no tail, starts 2 to 34 hand over 33 tails to 32 slots,
	 * so the tail of start 1's sound (sample 0) is the oldest and is cut */
	for (i = 0; i < 34; i++) {
		c[n++] = mk(i * 8, 0, AUDIO_VOICE_OP_START, i, 0, 0);
		c[n++] = mk(i * 8, 0, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	}
	audio_voice_render(&e, out, 300, c, n);
	CHECK(fades_in_use(&e) == AUDIO_VOICE_FADE_SLOTS,
	      "pool: all %u slots in use, got %u", AUDIO_VOICE_FADE_SLOTS,
	      fades_in_use(&e));
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++)
		if (e.fade[k].base == 0U && e.fade[k].active)
			found++;
	CHECK(found == 0, "pool: the oldest tail (sample 0) was cut, %u slots "
	      "still hold it", found);
	found = 0;
	for (k = 0; k < AUDIO_VOICE_FADE_SLOTS; k++)
		if (e.fade[k].base == 32U * 32U)
			found++;
	CHECK(found == 1, "pool: the newest tail but one (sample 32) is held");
	/* the tails run out R frames after the last hand-over */
	audio_voice_render(&e, out, 600, NULL, 0);
	CHECK(fades_in_use(&e) == 0, "pool: every slot is free after the tails");
}

static int render_tail_only(const struct audio_voice_cmd *extra, uint32_t nextra,
                            int16_t *out)
{
	static struct audio_voice_engine e;
	const int16_t v[2] = { 12000, 0 };
	struct audio_voice_cmd c[16];
	uint32_t n = 0, i;

	dc_engine(&e, v, 2);
	c[n++] = mk(0, 3, AUDIO_VOICE_OP_CONFIG, 1, 200, 0);
	c[n++] = SET48(3, 0x6000);
	c[n++] = mk(0, 3, AUDIO_VOICE_OP_START, 0, 0, 0);
	c[n++] = mk(0, 3, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	/* restart onto the silent sample at 300: only the tail sounds */
	c[n++] = mk(300, 3, AUDIO_VOICE_OP_START, 1, 0, 0);
	c[n++] = mk(300, 3, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	for (i = 0; i < nextra; i++)
		c[n++] = extra[i];
	audio_voice_render(&e, out, 700, c, n);
	return 1;
}

static void test_records_leave_tail(void)
{
	static int16_t a[2 * 700], b[2 * 700];
	struct audio_voice_cmd x[6];
	uint32_t j;
	int differ = 0;

	render_tail_only(NULL, 0, a);
	x[0] = mk(320, 3, AUDIO_VOICE_OP_LOOP, 0, 4, 8);
	x[1] = mk(330, 3, AUDIO_VOICE_OP_DIRECTION, AUDIO_VOICE_DIR_BACKWARD, 0, 0);
	x[2] = mk(340, 3, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE |
	          AUDIO_VOICE_RATE_HZ, 12000, 0);
	x[3] = mk(350, 3, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	x[4] = mk(360, 3, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0, 0x10001U);
	render_tail_only(x, 5, b);
	for (j = 0; j < 1400; j++)
		differ += a[j] != b[j];
	CHECK(differ == 0, "tail: LOOP, DIRECTION, SET and STOP after a restart "
	      "changed %d samples of the tail", differ);
	CHECK(a[2 * 310] != 0, "tail: the tail sounds after the restart");
}

static void test_config(void)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 8];
	struct audio_voice_cmd c[8];

	audio_voice_init(&e, g_bank, 64, AUDIO_VOICE_CLOCK_PAL);
	c[0] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 1, 512, 0);
	c[1] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 1, 513, 0);
	c[2] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 0, 100, 0);
	c[3] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, 2, 100, 0);
	c[4] = mk(0, 9, AUDIO_VOICE_OP_CONFIG, 3, 100, 0);
	c[5] = mk(0, 200, AUDIO_VOICE_OP_CONFIG, 1, 7, 0);
	c[6] = mk(0, 0, AUDIO_VOICE_OP_MODE, 1, 7, 0);
	c[7] = mk(0, 0, AUDIO_VOICE_OP_ENVELOPE, 1, 7, 0);
	audio_voice_render(&e, out, 8, c, 8);
	CHECK(e.ramp_frames == 7, "config: the voice field is ignored, last good "
	      "ramp wins (%u)", e.ramp_frames);
	CHECK(e.rejected == 6, "config: refused records counted (%u)", e.rejected);
}

static void test_silence_clears_tails(void)
{
	static struct audio_voice_engine e;
	static int16_t out[2 * 200];
	const int16_t v[2] = { 9000, 9000 };
	struct audio_voice_cmd c[7];

	dc_engine(&e, v, 2);
	c[0] = mk(0, 1, AUDIO_VOICE_OP_CONFIG, 1, 400, 0);
	c[1] = SET48(1, 0x8000);
	c[2] = mk(0, 1, AUDIO_VOICE_OP_START, 0, 0, 0);
	c[3] = mk(0, 1, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	c[4] = mk(100, 1, AUDIO_VOICE_OP_START, 1, 0, 0);
	c[5] = mk(100, 1, AUDIO_VOICE_OP_LOOP, 0, 0, 32);
	audio_voice_render(&e, out, 200, c, 6);
	CHECK(fades_in_use(&e) == 1, "silence: a tail is running");
	audio_voice_silence(&e);
	CHECK(fades_in_use(&e) == 0 && !e.voice[1].active,
	      "silence: voices and tails are gone");
	audio_voice_render(&e, out, 200, NULL, 0);
	CHECK(out[0] == 0 && out[2 * 199] == 0, "silence: the output is silent");
}

int main(void)
{
	const char *env;
	uint32_t seqs = 20000, seq, done = 0;
	uint64_t seed = 1;

	if ((env = getenv("ZZ9K_VOICE_RAMP_SEQS")) != NULL)
		seqs = (uint32_t)strtoul(env, NULL, 0);
	if ((env = getenv("ZZ9K_VOICE_RAMP_SEED")) != NULL)
		seed = strtoull(env, NULL, 0);
#ifdef HAVE_ALARM
	alarm(60U + seqs / 100U);
#endif
	test_config();
	test_ramp_shape();
	test_restart_tail();
	test_pool_exhaustion();
	test_records_leave_tail();
	test_silence_clears_tails();
	model_vs_baseline(2000);
	for (seq = 0; seq < seqs && failures < 5; seq++) {
		if (!run_sequence(seq, seed))
			failures++;
		done++;
	}
	if (failures) {
		printf("audio_voice_ramp_test: %d FAILURES\n", failures);
		return 1;
	}
	if (seqs >= 1000U && (st.with_fade * 10U < st.renders ||
	    st.full_pool == 0U || st.ramping * 5U < st.renders ||
	    st.stopping == 0U || st.audible * 2U < st.renders)) {
		printf("audio_voice_ramp_test: corpus too weak (renders %u, with "
		       "tails %u, full pool %u, ramping %u, stopping %u, audible "
		       "%u)\n", st.renders, st.with_fade, st.full_pool, st.ramping,
		       st.stopping, st.audible);
		return 1;
	}
	printf("audio_voice_ramp_test: directed cases, model against baseline "
	       "(2000 sequences) and %u random sequences against the engine "
	       "passed (seed %llu); %u renders, %u with tails, %u with a full "
	       "pool, %u with a ramp running, %u with a stop in progress\n",
	       done, (unsigned long long)seed, st.renders, st.with_fade,
	       st.full_pool, st.ramping, st.stopping);
	return 0;
}
