/*
 * The optional static soft knee of the voice engine (CONFIG limiter,
 * "Limiting" in audio_voice.h), driven through the public command and render
 * interface only.
 *
 * The reference is written from the rule in that comment and shares nothing
 * with the engine: a model of the mix (nearest read of a constant-rate voice,
 * (sample * volume) >> 16 per voice, the sum scaled by the Q8.8 master with a
 * floored 64-bit shift) gives the sum x the engine must see for every frame,
 * and the output is judged against
 *
 *     |x| <= 24576:  identity
 *     above:         sign * (24576 + 8191 * d / (8191 + d)),  d = |x| - 24576
 *
 * as an exact rational, with 2 LSB of tolerance, never a generated table.
 * With the limiter off the output is the S16 clamp of x. The checks are:
 *
 *   - every sample of dense sweeps (a 65536-frame ramp on top of up to 31
 *     constant voices, both signs, so that every integer x of about
 *     +-1.05 million occurs) is identity inside the knee and within
 *     tolerance of the curve above it, both signs, below either rail;
 *   - the outputs seen over the sweeps are one function of x, odd, monotonic
 *     and the identity through +-24576 (the table of every x up to +-150000);
 *   - directed edges: for each of the 4096 segment boundaries of the 8191 + d
 *     axis (and one frame either side) in both signs, a master and a
 *     sum that make x exactly that value;
 *   - the maximum master, also as an out-of-range word, with 32 voices;
 *   - 32 voices and 32 full-gain fade tails (CONFIG ramp, restart of every
 *     voice) at the maximum master never reach a rail, and reach one with the
 *     limiter off, so the case is loud enough to mean something;
 *   - CONFIG on and off at stamps that are not periods, in the same stamp
 *     in both orders, with MASTER and with rejected records around them,
 *     against the model, and the render split every way (one frame, odd
 *     sizes, at every stamp and either side of it) byte-identical to one call;
 *   - a CONFIG matrix of flags, arg0 and arg1 values from a fresh and from a
 *     configured engine: valid records change exactly what they name,
 *     invalid ones change nothing and are counted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_voice.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		if (++failures <= 40) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} \
} while (0)

#define KNEE   24576
#define HEAD   8191
#define MAXOUT 32766

#define MAXF 65536U
#define TAB_R 150000
#define TAB_NONE 0x7FFFFFFF

/* ---- sample bank: host copy for the model, big-endian bytes for the engine */

#define ID_RAMP  0U
#define ID_POS   1U
#define ID_NEG   2U
#define ID_NOISE 3U
#define ID_ST    4U
#define NSAMPLES 5U

struct smp {
	uint32_t pcm;    /* first int16 in g_pcm */
	uint32_t frames;
	int stereo;
};

static const struct smp g_smp[NSAMPLES] = {
	{ 0U, 65536U, 0 },
	{ 65536U, 65536U, 0 },
	{ 131072U, 65536U, 0 },
	{ 196608U, 65536U, 0 },
	{ 262144U, 32767U, 1 },
};

#define PCM_INTS 327680U
static int16_t g_pcm[PCM_INTS];
static uint8_t g_bank[2U * PCM_INTS];

static void fill_bank(void)
{
	uint32_t i, s = 0x2545F491U;

	for (i = 0; i < 65536U; i++) {
		g_pcm[g_smp[ID_RAMP].pcm + i] = (int16_t)((int)i - 32768);
		g_pcm[g_smp[ID_POS].pcm + i] = 32767;
		g_pcm[g_smp[ID_NEG].pcm + i] = -32768;
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		g_pcm[g_smp[ID_NOISE].pcm + i] = (int16_t)(s >> 16);
	}
	/* left a, right -a, a even from -32766 to 32766 */
	for (i = 0; i < 32767U; i++) {
		int a = 2 * ((int)i - 16383);

		g_pcm[g_smp[ID_ST].pcm + 2U * i] = (int16_t)a;
		g_pcm[g_smp[ID_ST].pcm + 2U * i + 1U] = (int16_t)-a;
	}
	for (i = 0; i < PCM_INTS; i++) {
		uint16_t u = (uint16_t)g_pcm[i];

		g_bank[2U * i] = (uint8_t)(u >> 8);
		g_bank[2U * i + 1U] = (uint8_t)u;
	}
}

static struct audio_voice_engine g_e;

static void setup(void)
{
	uint32_t id;

	audio_voice_init(&g_e, g_bank, sizeof(g_bank), AUDIO_VOICE_CLOCK_PAL);
	for (id = 0; id < NSAMPLES; id++) {
		const struct smp *s = &g_smp[id];
		int r = audio_voice_sample_set(&g_e, id, 2U * s->pcm,
		                               s->frames * (s->stereo ? 4U : 2U),
		                               s->stereo ? AUDIO_VOICE_FMT_S16S :
		                               AUDIO_VOICE_FMT_S16);

		CHECK(r == 0, "setup: sample %u", id);
	}
}

/* ---- records ---- */

static struct audio_voice_cmd mk(uint32_t stamp, unsigned voice, unsigned op,
                                 unsigned flags, uint32_t a0, uint32_t a1)
{
	struct audio_voice_cmd c;

	memset(&c, 0, sizeof(c));
	c.stamp = stamp;
	c.voice = (uint8_t)voice;
	c.op = (uint8_t)op;
	c.flags = (uint16_t)flags;
	c.arg0 = a0;
	c.arg1 = a1;
	return c;
}

static struct audio_voice_cmd cfg(uint32_t stamp, unsigned flags, uint32_t a0,
                                  uint32_t a1)
{
	return mk(stamp, 0, AUDIO_VOICE_OP_CONFIG, flags, a0, a1);
}

#define LIM_ON(stamp)  cfg(stamp, AUDIO_VOICE_CONFIG_LIMITER, 0U, 1U)
#define LIM_OFF(stamp) cfg(stamp, AUDIO_VOICE_CONFIG_LIMITER, 0U, 0U)

/* A voice of the model: one sample read at 48 kHz from frame off, volumes. */
struct mvoice {
	uint32_t id, off, vl, vr;
};

static size_t add_voice(struct audio_voice_cmd *c, size_t n, uint32_t stamp,
                        unsigned voice, const struct mvoice *m)
{
	const uint32_t frame_bytes = g_smp[m->id].stereo ? 4U : 2U;

	c[n++] = mk(stamp, voice, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE |
	            AUDIO_VOICE_SET_VOLUME | AUDIO_VOICE_RATE_HZ, 48000U,
	            (m->vl << 16) | m->vr);
	c[n++] = mk(stamp, voice, AUDIO_VOICE_OP_START, m->id,
	            frame_bytes * m->off, 0U);
	return n;
}

/* ---- the reference ---- */

/* Floor of v / 2^n without relying on how >> treats a negative number. */
static int64_t floor_shift(int64_t v, unsigned n)
{
	const int64_t bias = ((int64_t)1 << n) - 1;

	if (v >= 0)
		return v >> n;
	return -((-v + bias) >> n);
}

static int64_t clamp16(int64_t x)
{
	return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* The output o for the sum x is the knee of the rule, within 2 LSB. */
static int knee_ok(int64_t x, int32_t o)
{
	const int64_t ax = x < 0 ? -x : x;
	const int64_t ao = o < 0 ? -(int64_t)o : (int64_t)o;
	int64_t d, r, t;

	if ((x < 0) != (o < 0))
		return 0;
	if (ax <= KNEE)
		return ao == ax;
	d = ax - KNEE;
	r = HEAD + d;
	t = (ao - KNEE) * r - (int64_t)HEAD * d;
	if (t < 0)
		t = -t;
	return ao <= MAXOUT && t <= 2 * r;
}

static int32_t pcm_at(const struct mvoice *v, uint32_t idx, int side)
{
	const struct smp *s = &g_smp[v->id];

	return s->stereo ? g_pcm[s->pcm + 2U * idx + (uint32_t)side] :
	       g_pcm[s->pcm + idx];
}

/* Per frame sums of the voices per side, all started at frame 0. */
static int64_t g_sl[MAXF], g_sr[MAXF];

static void model_sums(const struct mvoice *v, unsigned nv, uint32_t frames)
{
	unsigned k;
	uint32_t f;

	memset(g_sl, 0, frames * sizeof(g_sl[0]));
	memset(g_sr, 0, frames * sizeof(g_sr[0]));
	for (k = 0; k < nv; k++) {
		for (f = 0; f < frames; f++) {
			const uint32_t idx = v[k].off + f;

			if (idx >= g_smp[v[k].id].frames)
				break;
			g_sl[f] += floor_shift((int64_t)pcm_at(&v[k], idx, 0) *
			                       v[k].vl, 16);
			g_sr[f] += floor_shift((int64_t)pcm_at(&v[k], idx, 1) *
			                       v[k].vr, 16);
		}
	}
}

struct ref_state {
	uint32_t master, ramp, limiter, rejected;
};

static void ref_init(struct ref_state *s)
{
	memset(s, 0, sizeof(*s));
	s->master = AUDIO_VOICE_MASTER_UNITY;
}

/* The settled CONFIG rules; 1 when the record is taken. */
static int ref_config(struct ref_state *s, unsigned flags, uint32_t a0,
                      uint32_t a1)
{
	const int ramp = (flags & 1U) != 0U;
	const int lim = (flags & 2U) != 0U;

	if (flags == 0U || (flags & ~3U) != 0U)
		return 0;
	if (ramp && a0 > 512U)
		return 0;
	if (lim && a1 > 1U)
		return 0;
	if (lim && !ramp && a0 != 0U)
		return 0;
	if (ramp)
		s->ramp = a0;
	if (lim)
		s->limiter = a1;
	return 1;
}

static void ref_apply(struct ref_state *s, const struct audio_voice_cmd *c)
{
	if (c->op == AUDIO_VOICE_OP_MASTER)
		s->master = c->arg0 > 0xFFFFU ? 0xFFFFU : c->arg0;
	else if (c->op == AUDIO_VOICE_OP_CONFIG &&
	         !ref_config(s, c->flags, c->arg0, c->arg1))
		s->rejected++;
}

/* The sum x after the master and the limiter state, per frame, with the
 * records applied in array order at their stamp (cmds sorted by stamp). */
static int64_t g_xl[MAXF], g_xr[MAXF];
static uint8_t g_lim[MAXF];

static void ref_run(struct ref_state *s, const struct audio_voice_cmd *c,
                    size_t n, uint32_t frames)
{
	size_t k = 0;
	uint32_t f;

	for (f = 0; f < frames; f++) {
		while (k < n && c[k].stamp <= f)
			ref_apply(s, &c[k++]);
		g_xl[f] = floor_shift(g_sl[f] * (int64_t)s->master, 8);
		g_xr[f] = floor_shift(g_sr[f] * (int64_t)s->master, 8);
		g_lim[f] = s->limiter != 0U;
	}
}

/* ---- the output seen as a function of x ---- */

static int32_t g_tab[2 * TAB_R + 1];
static unsigned tab_conflict;
static unsigned long lim_rails, off_rails;
static unsigned long frames_checked;

static void tab_note(int64_t x, int32_t o)
{
	if (x < -TAB_R || x > TAB_R)
		return;
	if (g_tab[x + TAB_R] != TAB_NONE && g_tab[x + TAB_R] != o)
		tab_conflict++;
	g_tab[x + TAB_R] = o;
}

/* Judge one render against the model: limiter frames by the knee, others by
 * the clamp; adjacent limiter frames monotonic in x; x and -x on the two
 * sides of a frame odd. */
static void check_run(const char *name, const int16_t *out, uint32_t frames)
{
	unsigned long bad = 0, badm = 0, bado = 0;
	char first[128] = "";
	uint32_t f;
	int side;

	for (f = 0; f < frames; f++) {
		for (side = 0; side < 2; side++) {
			const int64_t x = side ? g_xr[f] : g_xl[f];
			const int32_t o = out[2U * f + (uint32_t)side];
			int ok;

			if (g_lim[f]) {
				ok = knee_ok(x, o);
				tab_note(x, o);
				if (o == 32767 || o == -32768)
					lim_rails++;
			} else {
				ok = o == clamp16(x);
				if (o == 32767 || o == -32768)
					off_rails++;
			}
			if (!ok) {
				if (bad == 0)
					snprintf(first, sizeof(first),
					         "frame %u side %d x %lld out %d",
					         (unsigned)f, side, (long long)x, (int)o);
				bad++;
			}
			if (f != 0U && g_lim[f] && g_lim[f - 1U]) {
				const int64_t px = side ? g_xr[f - 1U] : g_xl[f - 1U];
				const int32_t po = out[2U * (f - 1U) + (uint32_t)side];

				if ((x > px && o < po) || (x < px && o > po) ||
				    (x == px && o != po))
					badm++;
			}
		}
		if (g_lim[f] && g_xr[f] == -g_xl[f] && out[2U * f + 1U] !=
		    -out[2U * f])
			bado++;
	}
	frames_checked += frames;
	CHECK(bad == 0, "%s: %lu samples off the reference, first at %s", name,
	      bad, first);
	CHECK(badm == 0, "%s: %lu monotonicity violations", name, badm);
	CHECK(bado == 0, "%s: %lu frames not odd (x, -x)", name, bado);
}

/* ---- scenarios of voices at frame 0 ---- */

static int16_t g_out[2U * MAXF];
static int16_t g_alt[2U * MAXF];

struct scen {
	struct mvoice v[32];
	unsigned nv;
	uint32_t master, frames;
	int limiter;
};

static void run_scen(const char *name, const struct scen *s)
{
	struct audio_voice_cmd c[80];
	struct ref_state st;
	size_t n = 0, used;
	unsigned i;

	if (s->limiter)
		c[n++] = LIM_ON(0);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, s->master, 0);
	for (i = 0; i < s->nv; i++)
		n = add_voice(c, n, 0, i, &s->v[i]);
	setup();
	used = audio_voice_render(&g_e, g_out, s->frames, c, n);
	CHECK(used == n, "%s: %zu of %zu records consumed", name, used, n);
	model_sums(s->v, s->nv, s->frames);
	ref_init(&st);
	ref_run(&st, c, n, s->frames);
	check_run(name, g_out, s->frames);
	CHECK(g_e.rejected == st.rejected, "%s: rejected %u, model %u", name,
	      g_e.rejected, st.rejected);
}

/* k constant voices (all of id) and the 65536-frame ramp. */
static void window(struct scen *s, uint32_t id, unsigned k, uint32_t master)
{
	unsigned i;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < k; i++) {
		s->v[s->nv].id = id;
		s->v[s->nv].vl = s->v[s->nv].vr = 0xFFFFU;
		s->nv++;
	}
	s->v[s->nv].id = ID_RAMP;
	s->v[s->nv].vl = s->v[s->nv].vr = 0xFFFFU;
	s->nv++;
	s->master = master;
	s->frames = MAXF;
	s->limiter = 1;
}

static void test_dense_sweeps(void)
{
	static struct scen s;
	static const uint32_t masters[] = {
		0U, 1U, 0x40U, 0xFFU, 0x101U, 0x180U, 0x1FFU, 0x200U, 0x300U,
		0x555U, 0x1000U, 0x7FFFU, 0xFFFFU
	};
	char name[64];
	unsigned k, m;

	/* Every integer x from -1048576 to 1048512 at unity master. */
	for (k = 0; k < 32U; k++) {
		snprintf(name, sizeof(name), "sweep +%u consts, master 256", k);
		window(&s, ID_POS, k, 256U);
		run_scen(name, &s);
		snprintf(name, sizeof(name), "sweep -%u consts, master 256", k);
		window(&s, ID_NEG, k, 256U);
		run_scen(name, &s);
	}
	/* Many x at other masters, up to 2^28. */
	for (m = 0; m < sizeof(masters) / sizeof(masters[0]); m++) {
		snprintf(name, sizeof(name), "sweep +31 consts, master %#x",
		         (unsigned)masters[m]);
		window(&s, ID_POS, 31U, masters[m]);
		run_scen(name, &s);
		snprintf(name, sizeof(name), "sweep -31 consts, master %#x",
		         (unsigned)masters[m]);
		window(&s, ID_NEG, 31U, masters[m]);
		run_scen(name, &s);
		snprintf(name, sizeof(name), "sweep 5 consts, master %#x",
		         (unsigned)masters[m]);
		window(&s, ID_POS, 5U, masters[m]);
		run_scen(name, &s);
	}
	/* Limiter off: the clamp of the first engine. */
	window(&s, ID_POS, 31U, 0xFFFFU);
	s.limiter = 0;
	run_scen("clamp +31 consts, master 0xffff", &s);
	window(&s, ID_NEG, 3U, 256U);
	s.limiter = 0;
	run_scen("clamp -3 consts, master 256", &s);
	window(&s, ID_RAMP, 0U, 256U);
	s.limiter = 0;
	s.nv = 1;
	run_scen("clamp ramp, master 256", &s);
}

/* x and -x on the two sides of the frame: stereo +a / -a at volume 1/2 and
 * a master that is a multiple of 256 make the sums exact negatives. */
static void test_odd_pairs(void)
{
	static const struct { unsigned k; uint32_t m; } cases[] = {
		{ 1, 1 }, { 2, 1 }, { 1, 3 }, { 4, 3 }, { 32, 1 }, { 32, 255 },
		{ 7, 16 }, { 3, 4 }, { 5, 51 }, { 16, 100 }
	};
	static struct scen s;
	char name[64];
	unsigned c, i;
	uint32_t f;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		memset(&s, 0, sizeof(s));
		for (i = 0; i < cases[c].k; i++) {
			s.v[i].id = ID_ST;
			s.v[i].vl = s.v[i].vr = 0x8000U;
		}
		s.nv = cases[c].k;
		s.master = 256U * cases[c].m;
		s.frames = g_smp[ID_ST].frames;
		s.limiter = 1;
		snprintf(name, sizeof(name), "odd pairs k=%u m=%u", cases[c].k,
		         cases[c].m);
		run_scen(name, &s);
		/* the pairs are exact negatives, and the engine output is too */
		for (f = 0; f < s.frames; f++) {
			if (g_out[2U * f + 1U] != -g_out[2U * f]) {
				CHECK(0, "%s: frame %u: %d and %d are not negatives",
				      name, (unsigned)f, g_out[2U * f],
				      g_out[2U * f + 1U]);
				break;
			}
		}
	}
}

/* The knee is the identity through 24576 and the curve above, and the
 * outputs over all the sweeps are one function of x. */
static void test_table(void)
{
	long x, missing = 0;
	int32_t prev = TAB_NONE;

	CHECK(tab_conflict == 0, "table: %u x values with two outputs",
	      tab_conflict);
	for (x = -TAB_R; x <= TAB_R; x++) {
		const int32_t o = g_tab[x + TAB_R];
		const long ax = x < 0 ? -x : x;

		if (o == TAB_NONE) {
			missing++;
			continue;
		}
		if (ax <= KNEE && o != x) {
			CHECK(0, "table: x %ld out %d is not the identity", x, (int)o);
			break;
		}
		if (o > MAXOUT || o < -MAXOUT) {
			CHECK(0, "table: x %ld out %d reaches a rail", x, (int)o);
			break;
		}
		if (prev != TAB_NONE && o < prev) {
			CHECK(0, "table: x %ld out %d below the output %d at x-1", x,
			      (int)o, (int)prev);
			break;
		}
		if (g_tab[-x + TAB_R] != TAB_NONE && g_tab[-x + TAB_R] != -o) {
			CHECK(0, "table: x %ld out %d, x %ld out %d are not odd", x,
			      (int)o, -x, (int)g_tab[-x + TAB_R]);
			break;
		}
		prev = o;
	}
	CHECK(missing == 0, "table: %ld x values of +-%d were never seen",
	      missing, TAB_R);
	CHECK(g_tab[KNEE + TAB_R] == KNEE && g_tab[-KNEE + TAB_R] == -KNEE &&
	      g_tab[KNEE + 1 + TAB_R] >= KNEE && g_tab[-KNEE - 1 + TAB_R] <= -KNEE,
	      "table: the knee edges +-24576 and +-24577");
	CHECK(lim_rails == 0, "limiter on: %lu samples at a rail", lim_rails);
	CHECK(off_rails > 0, "limiter off: the sweeps never clamp (%lu)",
	      off_rails);
}

/* A sum x made exactly: master m and the sum s of k constants and a ramp
 * sample. 0 when no master of Q8.8 gives it with at most 32 voices. */
static int solve_sum(int64_t t, uint32_t *master, int64_t *sum)
{
	uint32_t m;

	for (m = 256U; m <= 0xFFFFU; m++) {
		const int64_t s0 = t >= 0 ? (t * 256) / (int64_t)m :
		                   -((-t * 256 + (int64_t)m - 1) / (int64_t)m);
		int cand;

		for (cand = 0; cand < 2; cand++) {
			const int64_t s = s0 + cand;

			if (s > 1048512 || s < -1048576)
				continue;
			if (floor_shift(s * (int64_t)m, 8) != t)
				continue;
			*master = m;
			*sum = s;
			return 1;
		}
	}
	return 0;
}

static void edge_target(int64_t t, unsigned long *done, unsigned long *skipped)
{
	static struct scen s;
	uint32_t m;
	int64_t sum, k, r;
	char name[64];

	if (!solve_sum(t, &m, &sum)) {
		(*skipped)++;
		return;
	}
	memset(&s, 0, sizeof(s));
	if (sum > 0) {
		k = (sum - 1) / 32766;
		r = sum - k * 32766;
	} else if (sum < 0) {
		k = (-sum - 1) / 32768;
		r = sum + k * 32768;
	} else {
		k = 0;
		r = 0;
	}
	for (; s.nv < (unsigned)k; s.nv++) {
		s.v[s.nv].id = sum > 0 ? ID_POS : ID_NEG;
		s.v[s.nv].vl = s.v[s.nv].vr = 0xFFFFU;
	}
	s.v[s.nv].id = ID_RAMP;
	s.v[s.nv].off = (uint32_t)(r >= 0 ? r + 1 + 32768 : r + 32768);
	s.v[s.nv].vl = s.v[s.nv].vr = 0xFFFFU;
	s.nv++;
	s.master = m;
	s.frames = 1;
	s.limiter = 1;
	snprintf(name, sizeof(name), "edge x=%lld", (long long)t);
	run_scen(name, &s);
	CHECK(g_xl[0] == t && g_xr[0] == t, "%s: the model built x=%lld",
	      name, (long long)g_xl[0]);
	(*done)++;
}

/* The segment boundaries of the 8191 + d axis, one frame either side. */
static void test_edges(void)
{
	unsigned long done = 0, skipped = 0;
	unsigned oct, seg;
	int sgn;
	int64_t d;

	for (oct = 0; oct < 16U; oct++) {
		for (seg = 0; seg < 256U; seg++) {
			const int64_t node = (int64_t)(8192U + 32U * seg) << oct;

			for (d = -1; d <= 1; d++) {
				for (sgn = -1; sgn <= 1; sgn += 2)
					edge_target(sgn * (node - HEAD + KNEE + d), &done,
					            &skipped);
			}
		}
	}
	CHECK(done > 20000UL, "edges: only %lu targets reachable (%lu skipped)",
	      done, skipped);
	printf("  edges: %lu targets run, %lu beyond 32 voices and the "
	       "maximum master\n", done, skipped);
}

/* ---- the maximum master ---- */

static void test_master_max(void)
{
	static const uint32_t masters[] = {
		0xFFFFU, 0x10000U, 0x7FFFFFFFU, 0xFFFFFFFFU
	};
	static struct scen s;
	unsigned m, i, side;
	char name[64];

	for (m = 0; m < sizeof(masters) / sizeof(masters[0]); m++) {
		for (side = 0; side < 3U; side++) {
			memset(&s, 0, sizeof(s));
			for (i = 0; i < 32U; i++) {
				s.v[i].id = side == 0U ? ID_POS : side == 1U ? ID_NEG :
				            (i & 1U) ? ID_POS : ID_NEG;
				s.v[i].off = i;
				s.v[i].vl = s.v[i].vr = 0xFFFFU;
			}
			s.nv = 32;
			s.master = masters[m];
			s.frames = 64;
			s.limiter = 1;
			snprintf(name, sizeof(name), "master %#x, 32 voices, kind %u",
			         (unsigned)masters[m], side);
			run_scen(name, &s);
			if (side < 2U) {
				const int o = g_out[0] * (side == 0U ? 1 : -1);

				/* the rational oracle in check_run fixes the value; here
				 * the sign, the rails and that full scale is loud */
				CHECK(o > 32000 && o <= MAXOUT,
				      "%s: full scale gives %d (sign, rail or level)",
				      name, g_out[0]);
			}
		}
	}
	/* The cap: 0x10000 and a word of ones sound as 0xFFFF. */
	memset(&s, 0, sizeof(s));
	for (i = 0; i < 32U; i++) {
		s.v[i].id = ID_NOISE;
		s.v[i].off = 100U * i;
		s.v[i].vl = s.v[i].vr = 0xFFFFU;
	}
	s.nv = 32;
	s.frames = 4096;
	s.limiter = 1;
	s.master = 0xFFFFU;
	run_scen("noise x32 at master 0xffff", &s);
	memcpy(g_alt, g_out, sizeof(int16_t) * 2U * s.frames);
	s.master = 0xFFFFFFFFU;
	run_scen("noise x32 at master 0xffffffff", &s);
	CHECK(memcmp(g_alt, g_out, sizeof(int16_t) * 2U * s.frames) == 0,
	      "an oversized master word sounds as 0xffff");
}

/* ---- 32 voices and 32 fade tails ---- */

/*
 * 32 voices play at full volume, then every one restarts, which hands the 32
 * sounds to 32 fade slots. full_new 0: the ramp is on from the first frame,
 * so the new voices rise while the tails fall (the sum of the pair stays
 * near one voice). full_new 1: the ramp is off while the old voices start;
 * a CONFIG ramp precedes the restart, and at the stamp of the restart a
 * CONFIG ramp of 0 and a SET of the volume put every new voice at full
 * volume at once while the tails still fall from full gain: up to 64 full
 * contributors, the legal bound of the sum.
 */
static void scenario_tails(const char *name, uint32_t old_id, uint32_t new_id,
                           int limiter, int full_new)
{
	enum { R = 512, T = 1000, FR = T + R + 200 };
	const int64_t co = g_pcm[g_smp[old_id].pcm];
	const int64_t cn = g_pcm[g_smp[new_id].pcm];
	const int64_t target = (int64_t)0xFFFF << 8;
	const int64_t step = target / R;
	struct audio_voice_cmd c[160];
	struct ref_state st;
	size_t n = 0, used;
	unsigned v;
	int64_t f;
	const unsigned long rails_before = lim_rails, off_before = off_rails;

	if (full_new)
		c[n++] = cfg(0, AUDIO_VOICE_CONFIG_LIMITER, 0U, limiter ? 1U : 0U);
	else
		c[n++] = cfg(0, AUDIO_VOICE_CONFIG_RAMP | AUDIO_VOICE_CONFIG_LIMITER,
		             R, limiter ? 1U : 0U);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0xFFFFU, 0);
	for (v = 0; v < 32U; v++) {
		c[n++] = mk(0, v, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE |
		            AUDIO_VOICE_SET_VOLUME | AUDIO_VOICE_RATE_HZ, 48000U,
		            0xFFFFFFFFU);
		c[n++] = mk(0, v, AUDIO_VOICE_OP_START, old_id, 0, 0);
	}
	if (full_new)
		c[n++] = cfg(T, AUDIO_VOICE_CONFIG_RAMP, R, 0U);
	for (v = 0; v < 32U; v++)
		c[n++] = mk(T, v, AUDIO_VOICE_OP_START, new_id, 0, 0);
	if (full_new) {
		c[n++] = cfg(T, AUDIO_VOICE_CONFIG_RAMP, 0U, 0U);
		for (v = 0; v < 32U; v++)
			c[n++] = mk(T, v, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME,
			            0, 0xFFFFFFFFU);
	}
	/* Per voice: the gain rises by target / R per frame to its target, and a
	 * restart sends the sound to a fade slot that falls by the same step
	 * over R frames (a ramp that runs when the length becomes 0 runs out). */
	for (f = 0; f < FR; f++) {
		int64_t s;

		if (f < T) {
			const int64_t vol = (f < R && !full_new) ? (f * step) >> 8 :
			                    0xFFFF;

			s = 32 * floor_shift(co * vol, 16);
		} else {
			const int64_t j = f - T;
			const int64_t tail = j < R ? (target - j * step) >> 8 : 0;
			const int64_t rise = (j < R && !full_new) ? (j * step) >> 8 :
			                     0xFFFF;

			s = 32 * (floor_shift(co * tail, 16) +
			          floor_shift(cn * rise, 16));
		}
		g_sl[f] = g_sr[f] = s;
	}
	setup();
	used = audio_voice_render(&g_e, g_out, FR, c, n);
	CHECK(used == n, "%s: %zu of %zu records consumed", name, used, n);
	ref_init(&st);
	ref_run(&st, c, n, FR);
	check_run(name, g_out, FR);
	CHECK(g_e.rejected == 0U, "%s: %u rejected", name, g_e.rejected);
	CHECK(g_e.ramp_frames == (full_new ? 0U : (uint32_t)R),
	      "%s: ramp setting %u", name, g_e.ramp_frames);
	if (limiter) {
		CHECK(lim_rails == rails_before,
		      "%s: %lu samples at a rail", name, lim_rails - rails_before);
		if (full_new && co == cn) {
			const int o = g_out[2 * T] * (co < 0 ? -1 : 1);

			/* the rational oracle in check_run fixes the value; here the
			 * sign, the rails and that the frame is loud */
			CHECK(o > 32000 && o <= MAXOUT,
			      "%s: 64 full voices give %d (sign, rail or level)",
			      name, o);
		}
	} else {
		CHECK(off_rails > off_before,
		      "%s: the loud case never reaches a rail without the limiter",
		      name);
	}
}

static void test_tails(void)
{
	scenario_tails("tails + to +, limiter", ID_POS, ID_POS, 1, 0);
	scenario_tails("tails - to -, limiter", ID_NEG, ID_NEG, 1, 0);
	scenario_tails("tails + to -, limiter", ID_POS, ID_NEG, 1, 0);
	scenario_tails("tails - to +, limiter", ID_NEG, ID_POS, 1, 0);
	scenario_tails("tails + to +, off", ID_POS, ID_POS, 0, 0);
	scenario_tails("tails - to -, off", ID_NEG, ID_NEG, 0, 0);
	scenario_tails("full tails + to +, limiter", ID_POS, ID_POS, 1, 1);
	scenario_tails("full tails - to -, limiter", ID_NEG, ID_NEG, 1, 1);
	scenario_tails("full tails + to -, limiter", ID_POS, ID_NEG, 1, 1);
	scenario_tails("full tails - to +, limiter", ID_NEG, ID_POS, 1, 1);
	scenario_tails("full tails + to +, off", ID_POS, ID_POS, 0, 1);
	scenario_tails("full tails - to -, off", ID_NEG, ID_NEG, 0, 1);
}

/* ---- CONFIG on and off in time, and the render split ---- */

static void sort_by_stamp(struct audio_voice_cmd *c, size_t n)
{
	size_t i, j;

	for (i = 1; i < n; i++) {
		const struct audio_voice_cmd x = c[i];

		for (j = i; j > 0 && c[j - 1U].stamp > x.stamp; j--)
			c[j] = c[j - 1U];
		c[j] = x;
	}
}

static size_t render_chunks(const struct audio_voice_cmd *c, size_t n,
                            uint32_t frames, const uint32_t *chunks,
                            unsigned nch, int16_t *out)
{
	uint32_t done = 0;
	unsigned i = 0;
	size_t k = 0;

	setup();
	while (done < frames) {
		uint32_t len = chunks[i % nch];

		if (len > frames - done)
			len = frames - done;
		k += audio_voice_render(&g_e, out + 2U * done, len, c + k, n - k);
		done += len;
		i++;
	}
	return k;
}

/* Chunk lengths that end the calls at every stamp and one frame either side. */
static unsigned boundary_chunks(const struct audio_voice_cmd *c, size_t n,
                                uint32_t frames, uint32_t *chunks, unsigned max)
{
	uint32_t last = 0;
	unsigned nch = 0;
	size_t i;
	int d;

	for (i = 0; i < n; i++) {
		for (d = -1; d <= 1; d++) {
			const int64_t p = (int64_t)c[i].stamp + d;

			if (p <= (int64_t)last || p >= (int64_t)frames || nch >= max)
				continue;
			chunks[nch++] = (uint32_t)p - last;
			last = (uint32_t)p;
		}
	}
	chunks[nch++] = frames - last;
	return nch;
}

static void check_splits(const char *name, const struct audio_voice_cmd *c,
                         size_t n, uint32_t frames, const int16_t *whole,
                         uint32_t rejected)
{
	static uint32_t bounds[1024];
	static uint32_t rnd[64];
	static const uint32_t one[] = { 1U };
	static const uint32_t seven[] = { 7U };
	static const uint32_t period[] = { 64U };
	static const uint32_t big[] = { 960U };
	static const uint32_t chunk4096[] = { 4096U };
	static const uint32_t odd[] = { 1237U, 1U, 2U, 3U, 4093U, 5U };
	struct {
		const char *what;
		const uint32_t *chunks;
		unsigned nch;
	} sets[8];
	unsigned i, nsets = 0, nb;
	uint32_t s = 0x1234567U;
	char label[96];

	for (i = 0; i < 64U; i++) {
		s = s * 1664525U + 1013904223U;
		rnd[i] = 1U + (s >> 16) % 3000U;
	}
	nb = boundary_chunks(c, n, frames, bounds, 1024U);
	sets[nsets].what = "1 frame";
	sets[nsets].chunks = one;
	sets[nsets++].nch = 1;
	sets[nsets].what = "7 frames";
	sets[nsets].chunks = seven;
	sets[nsets++].nch = 1;
	sets[nsets].what = "64 frames";
	sets[nsets].chunks = period;
	sets[nsets++].nch = 1;
	sets[nsets].what = "960 frames";
	sets[nsets].chunks = big;
	sets[nsets++].nch = 1;
	sets[nsets].what = "4096 frames";
	sets[nsets].chunks = chunk4096;
	sets[nsets++].nch = 1;
	sets[nsets].what = "odd sizes";
	sets[nsets].chunks = odd;
	sets[nsets++].nch = 6;
	sets[nsets].what = "at each stamp +-1";
	sets[nsets].chunks = bounds;
	sets[nsets++].nch = nb;
	sets[nsets].what = "random sizes";
	sets[nsets].chunks = rnd;
	sets[nsets++].nch = 64;
	for (i = 0; i < nsets; i++) {
		const size_t used = render_chunks(c, n, frames, sets[i].chunks,
		                                  sets[i].nch, g_alt);

		snprintf(label, sizeof(label), "%s split in %s", name, sets[i].what);
		CHECK(memcmp(whole, g_alt, sizeof(int16_t) * 2U * frames) == 0,
		      "%s: output differs from one call", label);
		CHECK(used == n && g_e.rejected == rejected && g_e.late == 0U &&
		      g_e.frame == frames,
		      "%s: consumed %zu/%zu rejected %u/%u late %u frame %u", label,
		      used, n, g_e.rejected, rejected, g_e.late, g_e.frame);
	}
}

static void test_toggle(void)
{
	enum { FR = 20000 };
	static const struct mvoice voices[3] = {
		{ ID_NOISE, 0U, 0xFFFFU, 0x9000U },
		{ ID_NOISE, 5000U, 0xC000U, 0xFFFFU },
		{ ID_NOISE, 9999U, 0xFFFFU, 0xFFFFU }
	};
	struct audio_voice_cmd c[48];
	struct ref_state st;
	size_t n = 0, used;
	unsigned i;
	unsigned long on_frames = 0;
	uint32_t f;

	c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0x180U, 0);
	c[n++] = LIM_OFF(0);
	for (i = 0; i < 3U; i++)
		n = add_voice(c, n, 0, i, &voices[i]);
	c[n++] = LIM_ON(1237);
	/* the same stamp in both orders: the last of them wins */
	c[n++] = LIM_ON(3001);
	c[n++] = LIM_OFF(3001);
	c[n++] = mk(3001, 0, AUDIO_VOICE_OP_MASTER, 0, 0x300U, 0);
	c[n++] = LIM_OFF(4999);
	c[n++] = LIM_ON(4999);
	/* rejected records change nothing */
	c[n++] = cfg(5003, AUDIO_VOICE_CONFIG_LIMITER, 7U, 1U);
	/* both settings, then the ramp alone with a legacy arg1 */
	c[n++] = cfg(6001, 3U, 100U, 0U);
	c[n++] = cfg(7919, 1U, 0U, 0xFFFFFFFFU);
	c[n++] = cfg(8209, 3U, 513U, 1U);
	c[n++] = LIM_ON(9001);
	c[n++] = mk(9001, 0, AUDIO_VOICE_OP_MASTER, 0, 0x12345U, 0);
	c[n++] = cfg(12011, 3U, 0U, 2U);
	c[n++] = LIM_OFF(14003);
	c[n++] = mk(14003, 0, AUDIO_VOICE_OP_MASTER, 0, 0xA0U, 0);
	c[n++] = LIM_ON(17001);
	c[n++] = LIM_ON(17001);
	c[n++] = LIM_OFF(17001);
	c[n++] = LIM_ON(17001);
	sort_by_stamp(c, n);

	setup();
	used = audio_voice_render(&g_e, g_out, FR, c, n);
	CHECK(used == n, "toggle: %zu of %zu records consumed", used, n);
	model_sums(voices, 3U, FR);
	ref_init(&st);
	ref_run(&st, c, n, FR);
	for (f = 0; f < FR; f++)
		on_frames += g_lim[f];
	CHECK(on_frames > 5000UL && on_frames < 15000UL,
	      "toggle: the limiter is on for %lu of %d frames", on_frames, FR);
	CHECK(st.rejected == 3U, "toggle: the model rejects %u, not 3",
	      st.rejected);
	check_run("toggle", g_out, FR);
	CHECK(g_e.rejected == st.rejected && g_e.late == 0U,
	      "toggle: rejected %u (model %u), late %u", g_e.rejected,
	      st.rejected, g_e.late);
	check_splits("toggle", c, n, FR, g_out, st.rejected);
}

/* The same with ramps: 32 voices on three samples restarted at stamps that
 * fit no period, then stopped, with the limiter toggled throughout. The
 * render is the same split every way and the limiter frames stay inside. */
static void test_toggle_tails(void)
{
	enum { FR = 5000 };
	static const uint32_t ids[3] = { ID_POS, ID_NEG, ID_NOISE };
	struct audio_voice_cmd c[160];
	struct ref_state st;
	size_t n = 0, used;
	unsigned v;
	uint32_t f;
	unsigned long over = 0;

	c[n++] = cfg(0, 3U, 300U, 0U);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0x400U, 0);
	for (v = 0; v < 32U; v++) {
		c[n++] = mk(0, v, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE |
		            AUDIO_VOICE_SET_VOLUME | AUDIO_VOICE_RATE_HZ,
		            24000U + 997U * v, (0xFFFFU << 16) | 0x6000U);
		c[n++] = mk(0, v, AUDIO_VOICE_OP_START, ids[v % 3U], 100U * v, 0);
		c[n++] = mk(1500U + 7U * v, v, AUDIO_VOICE_OP_START,
		            ids[(v + 1U) % 3U], 40U * v, 0);
		c[n++] = mk(3500U + 13U * v, v, AUDIO_VOICE_OP_STOP, 0, 0, 0);
	}
	c[n++] = LIM_ON(1333);
	c[n++] = LIM_OFF(2005);
	c[n++] = LIM_ON(2750);
	c[n++] = LIM_OFF(2750);
	c[n++] = LIM_ON(3187);
	c[n++] = cfg(3300, 3U, 513U, 0U);
	sort_by_stamp(c, n);

	setup();
	used = audio_voice_render(&g_e, g_out, FR, c, n);
	CHECK(used == n, "toggle tails: %zu of %zu records consumed", used, n);
	memset(g_sl, 0, sizeof(g_sl));
	memset(g_sr, 0, sizeof(g_sr));
	ref_init(&st);
	ref_run(&st, c, n, FR);
	for (f = 0; f < FR; f++) {
		if (g_lim[f] && (g_out[2U * f] > MAXOUT || g_out[2U * f] < -MAXOUT ||
		                 g_out[2U * f + 1U] > MAXOUT ||
		                 g_out[2U * f + 1U] < -MAXOUT))
			over++;
	}
	CHECK(over == 0UL, "toggle tails: %lu limiter frames at a rail", over);
	CHECK(g_e.rejected == st.rejected && st.rejected == 1U,
	      "toggle tails: rejected %u, model %u", g_e.rejected, st.rejected);
	check_splits("toggle tails", c, n, FR, g_out, st.rejected);
}

/* audio_voice_silence keeps the CONFIG settings, the limiter with them. */
static void test_silence_keeps(void)
{
	static const struct mvoice noise = { ID_NOISE, 0U, 0xFFFFU, 0xFFFFU };
	struct audio_voice_cmd c[8], at[8];
	struct ref_state st;
	size_t n = 0, i;
	const uint32_t at_frame = 10U;

	setup();
	c[0] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0x300U, 0);
	c[1] = LIM_ON(0);
	audio_voice_render(&g_e, g_out, at_frame, c, 2);
	audio_voice_silence(&g_e);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0x300U, 0);
	n = add_voice(c, n, 0, 0, &noise);
	for (i = 0; i < n; i++) {
		at[i] = c[i];
		at[i].stamp = at_frame;
	}
	audio_voice_render(&g_e, g_out, 512, at, n);
	model_sums(&noise, 1U, 512);
	ref_init(&st);
	st.limiter = 1;
	ref_run(&st, c, n, 512);
	check_run("limiter kept by silence", g_out, 512);
}

/* ---- the CONFIG matrix ---- */

static void test_config_matrix(void)
{
	static const unsigned flags[] = {
		0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0x100U, 0x8000U, 0x8002U,
		0x8003U, 0xFFFFU
	};
	static const uint32_t arg0s[] = {
		0U, 1U, 100U, 512U, 513U, 0xFFFFFFFFU
	};
	static const uint32_t arg1s[] = { 0U, 1U, 2U, 0xFFFFFFFFU };
	static const struct mvoice noise = { ID_NOISE, 0U, 0xFFFFU, 0xFFFFU };
	unsigned preset, fi, a0i, a1i;
	unsigned long cases = 0, accepted = 0;
	char name[96];

	for (preset = 0; preset < 2U; preset++) {
		for (fi = 0; fi < sizeof(flags) / sizeof(flags[0]); fi++) {
			for (a0i = 0; a0i < sizeof(arg0s) / sizeof(arg0s[0]); a0i++) {
				for (a1i = 0; a1i < sizeof(arg1s) / sizeof(arg1s[0]);
				     a1i++) {
					struct audio_voice_cmd c[8];
					struct ref_state st;
					size_t n = 0;

					c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0x200U, 0);
					n = add_voice(c, n, 0, 0, &noise);
					if (preset)
						c[n++] = cfg(0, 3U, 100U, 1U);
					c[n++] = cfg(0, flags[fi], arg0s[a0i], arg1s[a1i]);
					setup();
					audio_voice_render(&g_e, g_out, 256, c, n);
					model_sums(&noise, 1U, 256);
					ref_init(&st);
					ref_run(&st, c, n, 256);
					snprintf(name, sizeof(name),
					         "config %s flags %#x arg0 %#x arg1 %#x",
					         preset ? "on+ramp100" : "fresh", flags[fi],
					         (unsigned)arg0s[a0i], (unsigned)arg1s[a1i]);
					check_run(name, g_out, 256);
					CHECK(g_e.ramp_frames == st.ramp,
					      "%s: ramp %u, model %u", name, g_e.ramp_frames,
					      st.ramp);
					CHECK(g_e.rejected == st.rejected,
					      "%s: rejected %u, model %u", name, g_e.rejected,
					      st.rejected);
					cases++;
					accepted += st.rejected == 0U;
				}
			}
		}
	}
	CHECK(accepted > 40UL && accepted < cases - 200UL,
	      "config matrix: %lu of %lu records taken", accepted, cases);
}

/* The limiter state by sound: the next 64 frames of loud noise against the
 * model with the limiter as it should be (the master is 0x200). */
static void probe(const char *name, int on)
{
	struct mvoice m = { ID_NOISE, 0U, 0xFFFFU, 0xFFFFU };
	struct ref_state st;

	m.off = g_e.frame;
	model_sums(&m, 1U, 64);
	ref_init(&st);
	st.master = 0x200U;
	st.limiter = on ? 1U : 0U;
	ref_run(&st, NULL, 0, 64);
	audio_voice_render(&g_e, g_out, 64, NULL, 0);
	check_run(name, g_out, 64);
}

/* The directed rules by name, so a failure names the rule. */
static void test_config_rules(void)
{
	static const struct mvoice noise = { ID_NOISE, 0U, 0xFFFFU, 0xFFFFU };
	struct audio_voice_cmd c[12];
	size_t n = 0;

	setup();
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_MASTER, 0, 0x200U, 0);
	n = add_voice(c, n, 0, 0, &noise);
	c[n++] = cfg(0, 3U, 200U, 1U);
	audio_voice_render(&g_e, g_out, 1, c, n);
	CHECK(g_e.ramp_frames == 200U && g_e.rejected == 0U,
	      "rules: ramp and limiter in one record (%u, %u rejected)",
	      g_e.ramp_frames, g_e.rejected);
	probe("rules: both set, limiter on", 1);

	/* a bad ramp leaves the limiter and a bad limiter leaves the ramp */
	n = 0;
	c[n++] = cfg(g_e.frame, 3U, 513U, 0U);
	c[n++] = cfg(g_e.frame, 3U, 50U, 2U);
	c[n++] = cfg(g_e.frame, 2U, 1U, 0U);
	c[n++] = cfg(g_e.frame, 2U, 0U, 2U);
	c[n++] = cfg(g_e.frame, 4U, 0U, 0U);
	c[n++] = cfg(g_e.frame, 0U, 0U, 0U);
	audio_voice_render(&g_e, g_out, 1, c, n);
	CHECK(g_e.ramp_frames == 200U && g_e.rejected == 6U,
	      "rules: six bad records change nothing (ramp %u, rejected %u)",
	      g_e.ramp_frames, g_e.rejected);
	probe("rules: bad records leave the limiter on", 1);

	/* the limiter alone leaves the ramp, the ramp alone the limiter */
	n = 0;
	c[n++] = cfg(g_e.frame, 2U, 0U, 0U);
	audio_voice_render(&g_e, g_out, 1, c, n);
	CHECK(g_e.ramp_frames == 200U && g_e.rejected == 6U,
	      "rules: limiter off keeps the ramp (%u, %u rejected)",
	      g_e.ramp_frames, g_e.rejected);
	probe("rules: limiter off", 0);
	n = 0;
	c[n++] = cfg(g_e.frame, 2U, 0U, 1U);
	c[n++] = cfg(g_e.frame, 1U, 8U, 0xFFFFFFFFU);
	audio_voice_render(&g_e, g_out, 1, c, n);
	CHECK(g_e.ramp_frames == 8U && g_e.rejected == 6U,
	      "rules: ramp alone ignores arg1 (%u, %u rejected)",
	      g_e.ramp_frames, g_e.rejected);
	probe("rules: ramp alone keeps the limiter on", 1);
}

int main(void)
{
	unsigned i;

	for (i = 0; i < 2U * TAB_R + 1U; i++)
		g_tab[i] = TAB_NONE;
	fill_bank();

	test_config_rules();
	test_config_matrix();
	test_dense_sweeps();
	test_odd_pairs();
	test_table();
	test_edges();
	test_master_max();
	test_tails();
	test_toggle();
	test_toggle_tails();
	test_silence_keeps();

	if (failures != 0) {
		printf("audio_voice_limiter_test: %d FAILED\n", failures);
		return 1;
	}
	printf("audio_voice_limiter_test: all passed (%lu frames checked)\n",
	       frames_checked);
	return 0;
}
