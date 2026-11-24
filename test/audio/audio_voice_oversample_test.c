/*
 * 2x oversampling of the voice engine (CONFIG OVERSAMPLE, "Oversampling" in
 * audio_voice.h).
 *
 * References, none of them the engine's own decimator:
 *
 *   - A direct convolution of the whole 96 kHz stream with the 59 taps of
 *     audio_reduce_table.h, built tap by tap from the table, zero before
 *     the first frame, one output per two input frames, no history
 *     buffers, no chunks. The Q8.8 master is applied per stretch to the
 *     wide stream before it, and the clamp (or the soft knee) after it.
 *   - The 96 kHz stream itself comes from the engine without oversampling
 *     run on a time base of half a frame: every stamp doubled, every rate
 *     halved, every ramp doubled. That is the same voice arithmetic (U4's
 *     engine, checked against the frozen baseline and its models) on 96 kHz
 *     frames, so the test compares the oversampled engine's retimed steps,
 *     ramps, fade slots, chunking and record application against it, and
 *     leaves the decimation and the final stage to the convolution.
 *   - Directed cases worked out by hand: where an impulse lands (the
 *     delay), DC levels, a signal that clips before the filter but not after
 *     it, the master at a frame of a period, the longest ramp, the alias
 *     energy of a voice above 48 kHz, the acceptance rules of the record,
 *     the history after a stop and a reset.
 *
 * Environment: ZZ9K_VOICE_OS_SEQS (default 3000), ZZ9K_VOICE_OS_SEED.
 * The random numbers come from a xorshift generator drawn in an explicit
 * order, so every platform sees the same sequences.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_reduce_table.h"
#include "audio_voice.h"
#include "zz9k/voice.h"

/* The selectors of a random scenario: nearest and linear, and cubic only in
 * the build where the engine accepts it under 2x (the compat build, see the
 * Makefile); the product rejects cubic with 2x, which
 * audio_voice_oversample_cubic_test.c covers. */
#define RANDOM_INTERPS (AUDIO_VOICE_OVERSAMPLE_CUBIC ? 3U : 2U)

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		if (failures < 40) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} \
} while (0)

/* ---- random numbers ---- */

static uint32_t g_rng = 1U;

static uint32_t rnd(uint32_t n)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng % n;
}

/* ---- the bank ---- */

#define BANK_BYTES 65536U

enum { S_S8 = 0, S_S16, S_S8S, S_S16S, S_TINY16, S_TINY8, S_DC, S_ALT,
       S_IMP_EVEN, S_IMP_ODD, S_COUNT };
#define RANDOM_SAMPLES 6U

struct smp_def {
	uint32_t off, bytes, fmt;
};

static const struct smp_def g_smp[S_COUNT] = {
	{ 0U, 3000U, AUDIO_VOICE_FMT_S8 },
	{ 4096U, 5000U, AUDIO_VOICE_FMT_S16 },
	{ 12288U, 3600U, AUDIO_VOICE_FMT_S8S },
	{ 16384U, 6000U, AUDIO_VOICE_FMT_S16S },
	{ 24576U, 14U, AUDIO_VOICE_FMT_S16 },
	{ 25000U, 64U, AUDIO_VOICE_FMT_S8 },
	{ 26000U, 8000U, AUDIO_VOICE_FMT_S16 },  /* DC 32767, 4000 frames */
	{ 34100U, 8000U, AUDIO_VOICE_FMT_S16 },  /* +8000, -8000, 4000 frames */
	{ 42200U, 8000U, AUDIO_VOICE_FMT_S16 },  /* 32767 then zeros */
	{ 50300U, 8000U, AUDIO_VOICE_FMT_S16 },  /* 0, 32767 then zeros */
};

static uint8_t g_bank[BANK_BYTES];

static void put16(uint32_t off, int32_t v)
{
	g_bank[off] = (uint8_t)((uint32_t)v >> 8);
	g_bank[off + 1U] = (uint8_t)v;
}

static void fill_dc(void)
{
	uint32_t i;

	for (i = 0; i < 4000U; i++)
		put16(g_smp[S_DC].off + 2U * i, 32767);
}

static void bank_fill(void)
{
	uint32_t i;

	for (i = 0; i < BANK_BYTES; i++)
		g_bank[i] = (uint8_t)rnd(256U);
	fill_dc();
	for (i = 0; i < 4000U; i++) {
		put16(g_smp[S_ALT].off + 2U * i, (i & 1U) ? -8000 : 8000);
		put16(g_smp[S_IMP_EVEN].off + 2U * i, i == 0U ? 32767 : 0);
		put16(g_smp[S_IMP_ODD].off + 2U * i, i == 1U ? 32767 : 0);
	}
}

static void engine_init(struct audio_voice_engine *e)
{
	uint32_t i;

	audio_voice_init(e, g_bank, BANK_BYTES, AUDIO_VOICE_CLOCK_PAL);
	for (i = 0; i < S_COUNT; i++)
		audio_voice_sample_set(e, i, g_smp[i].off, g_smp[i].bytes,
		                       g_smp[i].fmt);
}

static struct audio_voice_cmd mk(uint32_t stamp, uint32_t voice, uint32_t op,
                                 uint32_t flags, uint32_t a0, uint32_t a1)
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

#define OS_ON(stamp) \
	mk(stamp, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE, \
	   AUDIO_VOICE_OVERSAMPLE_2X, 0)
#define OS_OFF(stamp) \
	mk(stamp, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE, \
	   AUDIO_VOICE_OVERSAMPLE_OFF, 0)
#define SET_HZ(stamp, v, hz, l, r) \
	mk(stamp, v, AUDIO_VOICE_OP_SET, \
	   AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME | AUDIO_VOICE_RATE_HZ, \
	   hz, ((uint32_t)(l) << 16) | (uint32_t)(r))
#define START(stamp, v, id) mk(stamp, v, AUDIO_VOICE_OP_START, id, 0, 0)
#define STOP(stamp, v) mk(stamp, v, AUDIO_VOICE_OP_STOP, 0, 0, 0)
#define MASTER(stamp, m) mk(stamp, 0, AUDIO_VOICE_OP_MASTER, 0, m, 0)
#define RAMP(stamp, r) \
	mk(stamp, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, r, 0)

/* ---- the reference decimator ---- */

static int64_t g_h[AUDIO_REDUCE_TAPS];

static void taps_init(void)
{
	const uint32_t c = (AUDIO_REDUCE_TAPS - 1U) / 2U;
	uint32_t i;

	memset(g_h, 0, sizeof(g_h));
	g_h[c] = AUDIO_REDUCE_CENTRE;
	for (i = 0; i < AUDIO_REDUCE_SIDES; i++) {
		g_h[c - (2U * i + 1U)] = audio_reduce_side[i];
		g_h[c + (2U * i + 1U)] = audio_reduce_side[i];
	}
}

/* Output k of the stream w: the window of 59 frames whose newest is frame
 * 2k + 1, taken from a zero past, rounded half up. */
static int64_t ref_fir(const int32_t *w, uint32_t k)
{
	int64_t acc = 0;
	uint32_t j;

	for (j = 0; j < AUDIO_REDUCE_TAPS; j++) {
		const int64_t idx = (int64_t)2 * k + 1 -
		                    (int64_t)(AUDIO_REDUCE_TAPS - 1U) + (int64_t)j;

		if (idx >= 0)
			acc += g_h[j] * (int64_t)w[idx];
	}
	return (acc + ((int64_t)1 << (AUDIO_REDUCE_Q - 1))) >> AUDIO_REDUCE_Q;
}

static int16_t ref_clamp(int64_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* The rational curve of the soft knee. */
static double ref_limit(int64_t v)
{
	const double a = (double)(v < 0 ? -v : v);
	double r;

	if (a <= 24576.0)
		return (double)v;
	r = 24576.0 + 8191.0 * (a - 24576.0) / (8191.0 + (a - 24576.0));
	return v < 0 ? -r : r;
}

/* ---- running the engines ---- */

#define MAXF 8192U
#define MAXREC 700U
#define MAXTOTAL 6000U

struct scenario {
	struct audio_voice_cmd cmd[MAXREC];
	uint8_t extra[MAXREC];  /* rejected by the oversampled engine only */
	size_t n;
	uint32_t calls[16];
	size_t ncalls;
};

static struct audio_voice_engine g_e;   /* oversampled */
static struct audio_voice_engine g_s;   /* the 96 kHz time base */
static int16_t g_out[MAXF * 2U];
static int16_t g_sub[MAXF * 4U];
static uint32_t g_eff[MAXREC];
static int32_t g_wide[2][MAXF * 2U];
static uint32_t g_total;

static struct audio_voice_cmd sub_record(const struct audio_voice_cmd *c)
{
	struct audio_voice_cmd s = *c;

	s.stamp = c->stamp << 1;
	if (c->op == AUDIO_VOICE_OP_SET && (c->flags & AUDIO_VOICE_SET_RATE)) {
		s.arg0 = c->arg0 / 2U;
	} else if (c->op == AUDIO_VOICE_OP_CONFIG &&
	           c->flags == AUDIO_VOICE_CONFIG_RAMP) {
		s.arg0 = c->arg0 * 2U;
	} else if (c->op == AUDIO_VOICE_OP_MASTER ||
	           (c->op == AUDIO_VOICE_OP_CONFIG &&
	            (c->flags & (AUDIO_VOICE_CONFIG_LIMITER |
	                         AUDIO_VOICE_CONFIG_OVERSAMPLE)) != 0U)) {
		/* the master, the limiter and the oversampling are the
		 * reference's own: a unity master keeps the order and lateness */
		s = mk(s.stamp, 0, AUDIO_VOICE_OP_MASTER, 0, 256U, 0);
	}
	return s;
}

/* Render the scenario in the calls given; the effective frame of every
 * record the oversampled engine consumed is left in g_eff. Returns the
 * number consumed. */
static size_t run_os(const struct scenario *sc, const uint32_t *calls,
                     size_t ncalls, int16_t *out)
{
	size_t pos = 0, c, j;
	uint32_t frame = 0;

	engine_init(&g_e);
	for (c = 0; c < ncalls; c++) {
		const size_t used = audio_voice_render(&g_e, out + 2U * frame,
		                                       calls[c], sc->cmd + pos,
		                                       sc->n - pos);
		uint32_t at = 0;

		for (j = 0; j < used; j++) {
			int32_t rel = (int32_t)(sc->cmd[pos + j].stamp - frame);

			if (rel < 0)
				rel = 0;
			if ((uint32_t)rel > at)
				at = (uint32_t)rel;
			g_eff[pos + j] = frame + at;
		}
		pos += used;
		frame += calls[c];
	}
	return pos;
}

static size_t run_sub(const struct scenario *sc, const uint32_t *calls,
                      size_t ncalls, int16_t *out)
{
	static struct audio_voice_cmd sub[MAXREC];
	size_t pos = 0, c, j;
	uint32_t frame = 0;

	for (j = 0; j < sc->n; j++)
		sub[j] = sub_record(&sc->cmd[j]);
	engine_init(&g_s);
	for (c = 0; c < ncalls; c++) {
		pos += audio_voice_render(&g_s, out + 4U * frame, 2U * calls[c],
		                          sub + pos, sc->n - pos);
		frame += calls[c];
	}
	return pos;
}

/* Check g_out against the 96 kHz stream of g_sub and the records the
 * oversampled engine consumed (g_eff). */
static void compare(const struct scenario *sc, size_t used, const char *what,
                    unsigned long seq)
{
	const uint32_t n = g_total;
	uint32_t mrec[MAXREC], mval[MAXREC], lrec[MAXREC], lval[MAXREC];
	uint32_t nm = 0, nl = 0, lim = 0, master, mi, li, i, k;
	size_t j;
	int side;

	for (j = 0; j < used; j++) {
		const struct audio_voice_cmd *c = &sc->cmd[j];

		if (c->op == AUDIO_VOICE_OP_MASTER) {
			mrec[nm] = g_eff[j];
			mval[nm++] = c->arg0 > AUDIO_VOICE_MASTER_MAX ?
			             AUDIO_VOICE_MASTER_MAX : c->arg0;
		} else if (c->op == AUDIO_VOICE_OP_CONFIG &&
		           c->flags == AUDIO_VOICE_CONFIG_LIMITER) {
			lrec[nl] = g_eff[j];
			lval[nl++] = c->arg1;
		}
	}
	for (i = 0; i < 4U * n; i++) {
		if (g_sub[i] >= 32767 || g_sub[i] <= -32768) {
			CHECK(0, "%s seq %lu: the 96 kHz reference clipped at %u", what,
			      seq, i / 2U);
			return;
		}
	}
	for (side = 0; side < 2; side++) {
		master = 256U;
		mi = 0;
		for (i = 0; i < 2U * n; i++) {
			while (mi < nm && mrec[mi] * 2U <= i)
				master = mval[mi++];
			g_wide[side][i] = (int32_t)(
				((int64_t)g_sub[2U * i + (uint32_t)side] * (int64_t)master) >>
				8);
		}
	}
	li = 0;
	for (k = 0; k < n; k++) {
		while (li < nl && lrec[li] <= k)
			lim = lval[li++];
		for (side = 0; side < 2; side++) {
			const int64_t v = ref_fir(g_wide[side], k);
			const int out = g_out[2U * k + (uint32_t)side];

			if (lim != 0U) {
				const double want = ref_limit(v);

				if (fabs((double)out - want) > 2.0) {
					CHECK(0, "%s seq %lu: frame %u side %d limited %d, "
					      "reference %.2f (wide %lld)", what, seq, k, side,
					      out, want, (long long)v);
					return;
				}
			} else if (out != ref_clamp(v)) {
				CHECK(0, "%s seq %lu: frame %u side %d is %d, reference %d "
				      "(wide %lld)", what, seq, k, side, out,
				      ref_clamp(v), (long long)v);
				return;
			}
		}
	}
}

/* One partition of the scenario into render calls, against the reference. */
static void check_partition(const struct scenario *sc, const uint32_t *calls,
                            size_t ncalls, const char *what,
                            unsigned long seq)
{
	uint32_t total = 0;
	size_t i, used, used_sub;
	unsigned extra = 0;

	for (i = 0; i < ncalls; i++)
		total += calls[i];
	g_total = total;
	used = run_os(sc, calls, ncalls, g_out);
	used_sub = run_sub(sc, calls, ncalls, g_sub);
	CHECK(used == used_sub, "seq %lu: records consumed %zu, time base %zu",
	      seq, used, used_sub);
	for (i = 0; i < used; i++)
		extra += sc->extra[i];
	CHECK(g_e.late == g_s.late, "seq %lu: late %u, time base %u", seq,
	      g_e.late, g_s.late);
	CHECK(g_e.rejected == g_s.rejected + extra,
	      "seq %lu: rejected %u, time base %u + %u", seq, g_e.rejected,
	      g_s.rejected, extra);
	CHECK(g_e.frame == total && g_s.frame == 2U * total,
	      "seq %lu: frames %u, %u", seq, g_e.frame, g_s.frame);
	for (i = 0; i < AUDIO_VOICE_MAX_VOICES; i++) {
		uint32_t ia, fa, ib, fb;
		const int aa = audio_voice_position(&g_e, (uint32_t)i, &ia, &fa);
		const int ab = audio_voice_position(&g_s, (uint32_t)i, &ib, &fb);

		CHECK(aa == ab && (!aa || (ia == ib && fa == fb)),
		      "seq %lu: voice %zu at %u.%04x/%d, time base %u.%04x/%d", seq,
		      i, ia, fa, aa, ib, fb, ab);
	}
	compare(sc, used, what, seq);
}

/* The scenario in its own calls, and in one call: no chunk or call
 * boundary in common. */
static void check_scenario(const struct scenario *sc, unsigned long seq)
{
	uint32_t total = 0, one[1];
	size_t i;

	for (i = 0; i < sc->ncalls; i++)
		total += sc->calls[i];
	check_partition(sc, sc->calls, sc->ncalls, "calls", seq);
	one[0] = total;
	check_partition(sc, one, 1, "one call", seq);
}

/* ---- random scenarios ---- */

static void add(struct scenario *sc, uint32_t stamp, uint32_t voice,
                uint32_t op, uint32_t flags, uint32_t a0, uint32_t a1,
                unsigned extra)
{
	if (sc->n >= MAXREC)
		return;
	sc->extra[sc->n] = (uint8_t)extra;
	sc->cmd[sc->n++] = mk(stamp, voice, op, flags, a0, a1);
}

static uint32_t pick_hz(void)
{
	static const uint32_t table[] = {
		2U, 100U, 5512U, 8000U, 11024U, 22050U, 32000U, 44100U, 47998U,
		48000U, 48002U, 60000U, 72000U, 88200U, 96000U, 96002U, 144000U,
		176400U, 191998U, 192000U
	};
	const uint32_t which = rnd(4U);
	const uint32_t any = 2U * (1U + rnd(96000U));
	const uint32_t listed = table[rnd((uint32_t)(sizeof(table) /
	                                            sizeof(table[0])))];

	return which == 0U ? any : listed;
}

static void gen_scenario(struct scenario *sc)
{
	static const uint32_t sizes[] = {
		1U, 2U, 3U, 7U, 100U, 479U, 480U, 481U, 959U, 960U, 961U, 1500U
	};
	static const uint32_t ramps[] = { 1U, 2U, 3U, 50U, 100U, 255U, 256U };
	static const uint32_t masters[] = {
		0U, 1U, 100U, 128U, 255U, 256U, 257U, 300U, 512U, 0x400U
	};
	const int ramped = rnd(3U) == 0U;
	const uint32_t nv = 1U + rnd(ramped ? 6U : 3U);
	const uint32_t maxvol = ramped ? 0x300U : 0x2000U;
	uint32_t total = 0, st = 0, nrec, i;

	memset(sc, 0, sizeof(*sc));
	sc->ncalls = 1U + rnd(5U);
	for (i = 0; i < sc->ncalls; i++) {
		const uint32_t big = rnd(3U);
		const uint32_t any = 1U + rnd(1800U);
		const uint32_t fixed = sizes[rnd((uint32_t)(sizeof(sizes) /
		                                            sizeof(sizes[0])))];
		uint32_t f = big == 0U ? any : fixed;

		if (total + f > MAXTOTAL)
			f = MAXTOTAL - total;
		if (f == 0U) {
			sc->ncalls = i;
			break;
		}
		sc->calls[i] = f;
		total += f;
	}
	add(sc, 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE,
	    AUDIO_VOICE_OVERSAMPLE_2X, 0, 0);
	if (ramped) {
		const uint32_t r = ramps[rnd((uint32_t)(sizeof(ramps) /
		                                       sizeof(ramps[0])))];

		add(sc, 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP, r, 0,
		    0);
	}
	if (rnd(2U) != 0U) {
		const uint32_t interp = 1U + rnd(RANDOM_INTERPS);

		add(sc, 0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP,
		    interp, 0, 0);
	}
	for (i = 0; i < nv; i++) {
		const uint32_t l = rnd(maxvol + 1U);
		const uint32_t r = rnd(maxvol + 1U);
		const uint32_t hz = pick_hz();

		add(sc, 0, i, AUDIO_VOICE_OP_SET,
		    AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME |
		    AUDIO_VOICE_RATE_HZ, hz, (l << 16) | r, 0);
	}
	nrec = 20U + rnd(100U);
	for (i = 0; i < nrec; i++) {
		const uint32_t pick = rnd(100U);
		const uint32_t roll = rnd(100U);
		const uint32_t back = rnd(20U);
		const uint32_t cut = rnd(50U);
		uint32_t v = rnd(nv);
		uint32_t stamp;

		if (roll < 25U) {
			/* the stamp of the record before */
		} else if (roll < 85U) {
			st += rnd(120U);
		} else {
			st += rnd(1200U);
		}
		stamp = st;
		if (back == 0U)
			stamp = st > 300U ? st - rnd(300U) : 0U;
		if (cut == 0U)
			v = 32U + rnd(4U);
		if (pick < 24U) {
			const uint32_t good = rnd(100U);
			const uint32_t id = good < 94U ? rnd(RANDOM_SAMPLES) :
			                    200U + rnd(50U);
			const uint32_t off = rnd(4000U);
			const uint32_t lenp = rnd(3U);
			const uint32_t len = rnd(3000U);

			add(sc, stamp, v, AUDIO_VOICE_OP_START, id, off,
			    lenp == 0U ? len : 0U, 0);
		} else if (pick < 36U) {
			const uint32_t ls = rnd(3000U);
			const uint32_t on = rnd(2U);
			const uint32_t ll = rnd(2500U);

			add(sc, stamp, v, AUDIO_VOICE_OP_LOOP, 0, ls, on != 0U ? ll : 0U,
			    0);
		} else if (pick < 60U) {
			const uint32_t form = rnd(3U);
			const uint32_t l = rnd(maxvol + 1U);
			const uint32_t r = rnd(maxvol + 1U);
			const uint32_t hz = pick_hz();
			uint32_t flags = AUDIO_VOICE_RATE_HZ;

			if (form != 1U)
				flags |= AUDIO_VOICE_SET_VOLUME;
			if (form != 0U)
				flags |= AUDIO_VOICE_SET_RATE;
			add(sc, stamp, v, AUDIO_VOICE_OP_SET, flags, hz, (l << 16) | r,
			    0);
		} else if (pick < 68U) {
			add(sc, stamp, v, AUDIO_VOICE_OP_STOP, 0, 0, 0, 0);
		} else if (pick < 76U) {
			const uint32_t dir = rnd(2U);

			add(sc, stamp, v, AUDIO_VOICE_OP_DIRECTION, dir, 0, 0, 0);
		} else if (pick < 82U) {
			const uint32_t badflags = rnd(20U);
			const uint32_t flagv = rnd(4U);
			const uint32_t badsel = rnd(20U);
			const uint32_t sel = rnd(RANDOM_INTERPS + 1U);

			add(sc, stamp, v, AUDIO_VOICE_OP_MODE,
			    badflags == 0U ? flagv : AUDIO_VOICE_MODE_INTERP,
			    badsel == 0U ? 4U : sel, 0, 0);
		} else if (pick < 87U) {
			const uint32_t m = masters[rnd((uint32_t)(sizeof(masters) /
			                                         sizeof(masters[0])))];

			add(sc, stamp, 0, AUDIO_VOICE_OP_MASTER, 0, m, 0, 0);
		} else if (pick < 91U) {
			const uint32_t on = rnd(2U);

			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG,
			    AUDIO_VOICE_CONFIG_LIMITER, 0, on, 0);
		} else if (pick < 94U) {
			const uint32_t interp = 1U + rnd(RANDOM_INTERPS);

			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG,
			    AUDIO_VOICE_CONFIG_INTERP, interp, 0, 0);
		} else if (pick < 96U && ramped) {
			const uint32_t off = rnd(4U);
			const uint32_t r = ramps[rnd((uint32_t)(sizeof(ramps) /
			                                       sizeof(ramps[0])))];

			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_RAMP,
			    off == 0U ? 0U : r, 0, 0);
		} else if (pick < 97U) {
			/* the same setting again: accepted, changes nothing */
			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG,
			    AUDIO_VOICE_CONFIG_OVERSAMPLE, AUDIO_VOICE_OVERSAMPLE_2X, 0,
			    0);
		} else if (pick < 98U) {
			/* refused, whole, by the oversampled engine alone */
			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG,
			    AUDIO_VOICE_CONFIG_OVERSAMPLE | AUDIO_VOICE_CONFIG_RAMP,
			    AUDIO_VOICE_OVERSAMPLE_2X, 0, 1);
		} else if (pick < 99U) {
			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG,
			    AUDIO_VOICE_CONFIG_OVERSAMPLE, 3U, 0, 1);
		} else {
			const uint32_t zero = rnd(2U);

			add(sc, stamp, 0, AUDIO_VOICE_OP_CONFIG,
			    zero != 0U ? 0U : 0x10U, 1U, 0, 0);
		}
	}
}

static void test_random(unsigned long seqs)
{
	static struct scenario sc;
	unsigned long s;

	for (s = 0; s < seqs && failures < 5; s++) {
		gen_scenario(&sc);
		check_scenario(&sc, s);
	}
	printf("audio_voice_oversample_test: %lu random scenarios equal the "
	       "convolution of the 96 kHz time base\n", seqs);
}

/* ---- directed cases ---- */

static void render_one(struct audio_voice_engine *e, int16_t *out,
                       uint32_t frames, const struct audio_voice_cmd *c,
                       size_t n)
{
	const size_t used = audio_voice_render(e, out, frames, c, n);

	CHECK(used == n, "render consumed %zu of %zu records", used, n);
}

/* Compare a render with the convolution of w (the same on both sides). */
static void check_wide(const char *name, const int16_t *out, uint32_t frames,
                       const int32_t *w)
{
	uint32_t k;

	for (k = 0; k < frames; k++) {
		const int16_t want = ref_clamp(ref_fir(w, k));

		if (out[2U * k] != want || out[2U * k + 1U] != want) {
			CHECK(0, "%s: frame %u is %d/%d, convolution %d", name, k,
			      out[2U * k], out[2U * k + 1U], want);
			return;
		}
	}
}

/* An impulse on an even 96 kHz frame, the first of the pair of output frame
 * F, has its centre tap alone at output F + 14; one on an odd frame spreads
 * over the odd taps, F to F + 29. */
static void test_delay(void)
{
	static int16_t out[1200 * 2];
	static int32_t w[2400];
	struct audio_voice_cmd c[4];
	const uint32_t F = 100U;
	const int64_t a = (32767 * 65535) >> 16;
	uint32_t k;
	int16_t want;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 96000U, 65535U, 65535U);
	c[2] = START(F, 0, S_IMP_EVEN);
	render_one(&g_e, out, 1200, c, 3);
	want = (int16_t)((a * AUDIO_REDUCE_CENTRE + ((int64_t)1 << 29)) >> 30);
	for (k = 0; k < 1200U; k++) {
		const int16_t expect = k == F + AUDIO_VOICE_OVERSAMPLE_DELAY_FRAMES ?
		                       want : 0;

		CHECK(out[2U * k] == expect && out[2U * k + 1U] == expect,
		      "even impulse: output %u is %d, expected %d", k, out[2U * k],
		      expect);
	}
	CHECK(want > 16000 && want < 16500, "centre tap gain %d", want);

	engine_init(&g_e);
	c[2] = START(F, 0, S_IMP_ODD);
	render_one(&g_e, out, 1200, c, 3);
	memset(w, 0, sizeof(w));
	w[2U * F + 1U] = (int32_t)a;
	check_wide("odd impulse", out, 1200, w);
	for (k = 0; k < 1200U; k++) {
		const int d = (int)k - (int)(F + AUDIO_VOICE_OVERSAMPLE_DELAY_FRAMES);
		/* output F + 14 + d reads tap 30 - 2d for the frame 2F + 1 */
		const int tap = 30 - 2 * d;
		int64_t expect = 0;

		if (tap >= 0 && tap < (int)AUDIO_REDUCE_TAPS)
			expect = (a * g_h[tap] + ((int64_t)1 << 29)) >> 30;
		CHECK(out[2U * k] == (int16_t)expect,
		      "odd impulse: output %u is %d, tap %d gives %lld", k,
		      out[2U * k], tap, (long long)expect);
	}
}

static void test_delay_status(void)
{
	static int16_t out[400];
	struct audio_voice_engine e;
	struct audio_voice_cmd c[2];

	engine_init(&e);
	CHECK(audio_voice_output_delay(&e) == 0U, "delay off is %u",
	      audio_voice_output_delay(&e));
	c[0] = OS_ON(10);
	render_one(&e, out, 100, c, 1);
	CHECK(audio_voice_output_delay(&e) == AUDIO_VOICE_OVERSAMPLE_DELAY_FRAMES,
	      "delay on is %u", audio_voice_output_delay(&e));
	c[0] = OS_OFF(10);
	render_one(&e, out, 100, c, 1);
	CHECK(audio_voice_output_delay(&e) == 0U, "delay off again is %u",
	      audio_voice_output_delay(&e));
}

/* Two full-scale voices at master 0.5 come out near 32766 (the sum is formed
 * before the master and never clamped before the filter); and a sum that
 * clips only if it is clamped before the filter. */
static void test_levels(void)
{
	static int16_t out[1000 * 2];
	static int32_t w[2000];
	struct audio_voice_cmd c[8];
	uint32_t k;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = MASTER(0, 128);
	c[2] = SET_HZ(0, 0, 24000U, 65535U, 65535U);
	c[3] = SET_HZ(0, 1, 24000U, 65535U, 65535U);
	c[4] = START(0, 0, S_DC);
	c[5] = START(0, 1, S_DC);
	render_one(&g_e, out, 1000, c, 6);
	for (k = 29; k < 1000U; k++)
		CHECK(out[2U * k] == 32766 && out[2U * k + 1U] == 32766,
		      "two voices at master 0.5: frame %u is %d", k, out[2U * k]);

	/* DC 32766 plus a 48 kHz alternation of +-8000 at 96 kHz: the peaks
	 * of 40765 clip if clamped early; the filter removes the alternation
	 * and the final stage clamps what is left. */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 96000U, 65535U, 65535U);
	c[2] = SET_HZ(0, 1, 96000U, 65535U, 65535U);
	c[3] = START(0, 0, S_DC);
	c[4] = START(0, 1, S_ALT);
	render_one(&g_e, out, 1000, c, 5);
	for (k = 0; k < 2000U; k++) {
		const int32_t dc = (32767 * 65535) >> 16;
		const int32_t alt = ((k & 1U) ? -8000 * 65535 : 8000 * 65535) >> 16;

		w[k] = dc + alt;
	}
	check_wide("dc plus alternation", out, 1000, w);
	for (k = 60; k < 1000U; k++)
		CHECK(out[2U * k] >= 32764,
		      "dc plus alternation: frame %u is %d (an early clamp gives "
		      "about 28800)", k, out[2U * k]);

	/* a master of 4 on a loud voice: wide values far above S16, clamped
	 * once, after the filter */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = MASTER(0, 0x400);
	c[2] = SET_HZ(0, 0, 24000U, 65535U, 65535U);
	c[3] = START(0, 0, S_DC);
	render_one(&g_e, out, 1000, c, 4);
	for (k = 0; k < 2000U; k++)
		w[k] = (int32_t)(((int64_t)((32767 * 65535) >> 16) * 0x400) >> 8);
	check_wide("master 4", out, 1000, w);
	CHECK(out[2U * 999U] == 32767, "master 4 clamps to %d", out[2U * 999U]);
}

/* A MASTER at frame 480 of a period: outputs before it are those without
 * it, the step response follows the convolution, the level settles 29
 * frames on, and the middle of the step is 14 frames after it. */
static void test_master_mid_period(void)
{
	static int16_t a[960 * 2], b[960 * 2];
	static int32_t w[1920];
	struct audio_voice_cmd c[4];
	uint32_t k;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 24000U, 0x8000U, 0x8000U);
	c[2] = START(0, 0, S_DC);
	render_one(&g_e, a, 960, c, 3);
	engine_init(&g_e);
	c[3] = MASTER(480, 128);
	render_one(&g_e, b, 960, c, 4);
	for (k = 0; k < 1920U; k++)
		w[k] = k < 960U ? 16383 : 8191;
	check_wide("master at 480", b, 960, w);
	for (k = 0; k < 480U; k++)
		CHECK(a[2U * k] == b[2U * k], "frame %u changed before the master",
		      k);
	for (k = 29; k < 480U; k++)
		CHECK(b[2U * k] == 16383, "frame %u is %d before the master", k,
		      b[2U * k]);
	for (k = 480U + 29U; k < 960U; k++)
		CHECK(b[2U * k] == 8191, "frame %u is %d after the step", k,
		      b[2U * k]);
	/* the centre tap and the taps after it see the new level: 3/4 of the
	 * step at frame 494, not all of it as a pure shift would show */
	CHECK(abs(b[2U * 494U] - 10239) <= 3, "frame 494 is %d, a step response "
	      "gives 10239", b[2U * 494U]);
}

/* The longest ramp, 512 frames, lasts 1024 frames at 96 kHz. */
static void test_max_ramp(void)
{
	static int16_t out[1500 * 2];
	static int32_t w[3000];
	struct audio_voice_cmd c[5];
	const int32_t vol = 0x4000;
	const int32_t inc = ((int32_t)(vol << 8)) / 1024;
	uint32_t k;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = RAMP(0, 512);
	c[2] = SET_HZ(0, 0, 24000U, vol, vol);
	c[3] = START(0, 0, S_DC);
	render_one(&g_e, out, 1500, c, 4);
	CHECK(g_e.rejected == 0U && g_e.ramp_frames == 512U, "ramp 512 rejected");
	for (k = 0; k < 3000U; k++) {
		const int32_t g = k < 1024U ? (int32_t)k * inc : vol << 8;

		w[k] = (int32_t)(((int64_t)32767 * (g >> 8)) >> 16);
	}
	check_wide("ramp 512", out, 1500, w);
}

/* The decimator's silent fast path never drops the filter's tail or keeps a
 * stale one: after the last sound the tail rings out through 29 output
 * frames, then the output is exactly zero and the history is zero. The stop
 * is in the second of three calls, inside the tail the third. */
static void test_history_drain(void)
{
	static int16_t out[600 * 2];
	static int32_t w[1200];
	struct audio_voice_cmd c[4];
	const uint32_t stop = 200U;
	uint32_t k, last_nonzero = 0, i;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 24000U, 65535U, 65535U);
	c[2] = START(0, 0, S_DC);
	c[3] = STOP(stop, 0);
	render_one(&g_e, out, 128, c, 3);
	render_one(&g_e, out + 2 * 128, 200, c + 3, 1);
	render_one(&g_e, out + 2 * 328, 272, c + 4, 0);
	for (k = 0; k < 1200U; k++)
		w[k] = k < 2U * stop ? ((32767 * 65535) >> 16) : 0;
	check_wide("tail after the stop", out, 600, w);
	for (k = 0; k < 600U; k++)
		if (out[2U * k] != 0)
			last_nonzero = k;
	CHECK(last_nonzero >= stop + 14U && last_nonzero <= stop + 28U,
	      "the tail ends at frame %u, stop at %u", last_nonzero, stop);
	for (k = stop + 29U; k < 600U; k++)
		CHECK(out[2U * k] == 0 && out[2U * k + 1U] == 0,
		      "frame %u after the tail is %d", k, out[2U * k]);
	CHECK(g_e.dec_quiet >= AUDIO_VOICE_DEC_HISTORY, "history not drained");
	for (i = 0; i < AUDIO_VOICE_DEC_HISTORY; i++)
		CHECK(g_e.dec_hist[0][i] == 0 && g_e.dec_hist[1][i] == 0,
		      "stale history at %u", i);
}

/* The tail rings on into the next chunk, which no voice touches, and that
 * chunk is cut into stretches shorter than the history by records: the
 * stretches that only hold zeros must still run the filter until the
 * history itself is zero. */
static void test_tail_in_silent_chunk(void)
{
	static int16_t out[800 * 2];
	static int32_t w[1600];
	struct audio_voice_cmd c[40];
	const uint32_t stop = 470U;     /* the chunk ends at output frame 480 */
	size_t n = 0;
	uint32_t k;

	engine_init(&g_e);
	c[n++] = OS_ON(0);
	c[n++] = SET_HZ(0, 0, 24000U, 65535U, 65535U);
	c[n++] = START(0, 0, S_DC);
	c[n++] = STOP(stop, 0);
	for (k = 485; k < 600U; k += 5U)
		c[n++] = MASTER(k, 256);
	render_one(&g_e, out, 800, c, n);
	for (k = 0; k < 1600U; k++)
		w[k] = k < 2U * stop ? ((32767 * 65535) >> 16) : 0;
	check_wide("tail in a silent chunk", out, 800, w);
	for (k = stop + 29U; k < 800U; k++)
		CHECK(out[2U * k] == 0, "frame %u after the tail is %d", k,
		      out[2U * k]);
	CHECK(out[2U * (stop + 14U)] != 0 && out[2U * 485U] != 0,
	      "the tail is missing: %d %d", out[2U * (stop + 14U)],
	      out[2U * 485U]);
}

/* The history a reset or a re-enable leaves behind is zero, not the end of
 * the sound that was cut: the next sound is filtered from a zero past. */
static void test_history_cleared(void)
{
	static int16_t out[600 * 2];
	static int32_t w1[1200], w2[1200];
	struct audio_voice_cmd c[8];
	uint32_t k;
	const int32_t dc = (32767 * 65535) >> 16;

	/* RESET at frame 200 while a sound plays, a new one from 205 */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 24000U, 65535U, 65535U);
	c[2] = START(0, 0, S_DC);
	c[3] = START(205, 0, S_DC);
	render_one(&g_e, out, 200, c, 3);
	audio_voice_silence(&g_e);
	render_one(&g_e, out + 2 * 200, 400, c + 3, 1);
	for (k = 0; k < 1200U; k++) {
		w1[k] = k < 400U ? dc : 0;
		w2[k] = k >= 410U ? dc : 0;
	}
	for (k = 0; k < 600U; k++) {
		const int16_t want = ref_clamp(ref_fir(k < 200U ? w1 : w2, k));

		CHECK(out[2U * k] == want, "reset: frame %u is %d, expected %d", k,
		      out[2U * k], want);
	}

	/* off and on again 5 frames after a stop, inside the filter's tail:
	 * the tail is dropped by the off, and the history is zero after the
	 * on */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 24000U, 65535U, 65535U);
	c[2] = START(0, 0, S_DC);
	c[3] = STOP(100, 0);
	c[4] = OS_OFF(105);
	c[5] = OS_ON(106);
	c[6] = START(110, 0, S_DC);
	render_one(&g_e, out, 600, c, 7);
	CHECK(g_e.rejected == 0U, "off and on: rejected %u", g_e.rejected);
	for (k = 0; k < 1200U; k++) {
		w1[k] = k < 200U ? dc : 0;
		w2[k] = k >= 220U ? dc : 0;
	}
	for (k = 0; k < 600U; k++) {
		int16_t want;

		if (k < 105U)
			want = ref_clamp(ref_fir(w1, k));
		else if (k == 105U)
			want = 0;
		else
			want = ref_clamp(ref_fir(w2, k));
		CHECK(out[2U * k] == want, "off and on: frame %u is %d, expected %d",
		      k, out[2U * k], want);
	}
}

/* RESET (audio_voice_silence) clears the history: no tail rings after it,
 * where a stop leaves one; the setting survives it. */
static void test_reset_history(void)
{
	static int16_t a[400 * 2], b[400 * 2];
	struct audio_voice_cmd c[4];
	uint32_t k;
	int nz = 0;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 96000U, 65535U, 65535U);
	c[2] = START(0, 0, S_DC);
	c[3] = STOP(200, 0);
	render_one(&g_e, a, 200, c, 3);
	audio_voice_silence(&g_e);
	render_one(&g_e, a + 400, 200, c, 0);
	for (k = 200; k < 400U; k++)
		nz += a[2U * k] != 0 || a[2U * k + 1U] != 0;
	CHECK(nz == 0, "%d frames after a reset are not silent", nz);
	CHECK(g_e.dec_quiet >= AUDIO_VOICE_DEC_HISTORY, "reset kept the history");
	CHECK(g_e.oversample == 1U, "the setting did not survive the reset");
	engine_init(&g_e);
	render_one(&g_e, b, 400, c, 4);
	nz = 0;
	for (k = 200; k < 400U; k++)
		nz += b[2U * k] != 0;
	CHECK(nz > 0, "the control (a stop without a reset) did not ring");
}

/* The acceptance rules of the CONFIG record. */
static void test_acceptance(void)
{
	static int16_t out[3000 * 2], ref[3000 * 2];
	struct audio_voice_cmd c[24];
	size_t n;
	uint32_t k;

	/* invalid records change nothing and each counts one rejection */
	engine_init(&g_e);
	n = 0;
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE,
	            0, 0);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE,
	            3, 0);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE,
	            2, 1);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_OVERSAMPLE,
	            2, 0xFFFFFFFFU);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
	            AUDIO_VOICE_CONFIG_OVERSAMPLE | AUDIO_VOICE_CONFIG_RAMP, 2, 0);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
	            AUDIO_VOICE_CONFIG_OVERSAMPLE | AUDIO_VOICE_CONFIG_LIMITER, 2,
	            1);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
	            AUDIO_VOICE_CONFIG_OVERSAMPLE | AUDIO_VOICE_CONFIG_INTERP, 2,
	            0);
	c[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
	            AUDIO_VOICE_CONFIG_OVERSAMPLE | 0x10U, 2, 0);
	render_one(&g_e, out, 100, c, n);
	CHECK(g_e.rejected == n && g_e.oversample == 0U && g_e.ramp_frames == 0U &&
	      g_e.limiter == 0U && g_e.interp == AUDIO_VOICE_INTERP_NEAREST,
	      "invalid records: rejected %u of %zu, oversample %u ramp %u "
	      "limiter %u", g_e.rejected, n, g_e.oversample, g_e.ramp_frames,
	      g_e.limiter);

	/* the same setting again is accepted */
	engine_init(&g_e);
	c[0] = OS_OFF(0);
	c[1] = OS_ON(0);
	c[2] = OS_ON(0);
	render_one(&g_e, out, 100, c, 3);
	CHECK(g_e.rejected == 0U && g_e.oversample == 1U, "valid records");

	/* a sounding voice refuses the change in both directions */
	engine_init(&g_e);
	n = 0;
	c[n++] = SET_HZ(0, 0, 24000U, 0x4000U, 0x4000U);
	c[n++] = START(0, 0, S_DC);
	c[n++] = OS_ON(10);
	render_one(&g_e, out, 100, c, n);
	CHECK(g_e.rejected == 1U && g_e.oversample == 0U,
	      "sounding voice: rejected %u oversample %u", g_e.rejected,
	      g_e.oversample);
	engine_init(&g_e);
	n = 0;
	c[n++] = OS_ON(0);
	c[n++] = SET_HZ(0, 0, 24000U, 0x4000U, 0x4000U);
	c[n++] = START(5, 0, S_DC);
	c[n++] = OS_OFF(10);
	render_one(&g_e, out, 100, c, n);
	CHECK(g_e.rejected == 1U && g_e.oversample == 1U,
	      "sounding voice, off: rejected %u oversample %u", g_e.rejected,
	      g_e.oversample);

	/* a refused record leaves no trace: the render equals the one without
	 * it, and the voice keeps its step */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(0, 0, 44100U, 0x4000U, 0x4000U);
	c[2] = START(5, 0, S_DC);
	c[3] = OS_OFF(300);
	c[4] = OS_ON(301);
	render_one(&g_e, out, 1000, c, 5);
	CHECK(g_e.rejected == 1U, "off while sounding, then on: %u", g_e.rejected);
	engine_init(&g_e);
	render_one(&g_e, ref, 1000, c, 3);
	CHECK(memcmp(out, ref, 1000U * 4U) == 0,
	      "a refused record changed the render");
	CHECK(g_e.voice[0].step == 30105U && g_e.voice[0].rate == 60211U,
	      "step %u rate %u", g_e.voice[0].step, g_e.voice[0].rate);

	/* a stopping voice is sounding until its ramp is over */
	engine_init(&g_e);
	n = 0;
	c[n++] = RAMP(0, 100);
	c[n++] = SET_HZ(0, 0, 24000U, 0x4000U, 0x4000U);
	c[n++] = START(0, 0, S_DC);
	c[n++] = STOP(300, 0);
	c[n++] = OS_ON(350);     /* in the stop ramp: refused */
	c[n++] = OS_ON(420);     /* the ramp is over: accepted */
	render_one(&g_e, out, 1000, c, n);
	CHECK(g_e.rejected == 1U && g_e.oversample == 1U,
	      "stop tail: rejected %u oversample %u", g_e.rejected,
	      g_e.oversample);
	/* so is a fade tail alone: the restart hands the old sound to a slot
	 * and the new voice is stopped at once, before its ramp has raised
	 * its gain */
	engine_init(&g_e);
	n = 0;
	c[n++] = RAMP(0, 100);
	c[n++] = SET_HZ(0, 0, 24000U, 0x4000U, 0x4000U);
	c[n++] = START(0, 0, S_DC);
	c[n++] = START(300, 0, S_DC);
	c[n++] = STOP(300, 0);
	c[n++] = OS_ON(350);     /* the slot is fading to 400: refused */
	c[n++] = OS_ON(450);     /* accepted */
	render_one(&g_e, out, 1000, c, n);
	CHECK(g_e.rejected == 1U && g_e.oversample == 1U,
	      "fade tail: rejected %u oversample %u", g_e.rejected,
	      g_e.oversample);

	/* the setting mid-chunk and in a split render equals the setting at 0 */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = SET_HZ(1000, 0, 44100U, 0x4000U, 0x2000U);
	c[2] = START(1010, 0, S_S16S);
	c[3] = SET_HZ(1200, 1, 30000U, 0x4000U, 0x4000U);
	c[4] = START(1300, 1, S_S8);
	render_one(&g_e, ref, 3000, c, 5);
	for (k = 0; k < 2U; k++) {
		static const uint32_t at[2] = { 333U, 960U };
		const uint32_t first = at[k] + 30U;

		engine_init(&g_e);
		c[0] = OS_ON(at[k]);
		render_one(&g_e, out, first, c, 1);
		render_one(&g_e, out + 2U * first, 3000U - first, c + 1, 4);
		CHECK(memcmp(out, ref, 3000U * 4U) == 0,
		      "setting on at %u differs from on at 0", at[k]);
	}
}

/* Switching off returns the engine to the 48 kHz arithmetic, byte for byte:
 * what plays after it equals what a fresh engine plays. */
static void test_back_to_48k(void)
{
	static int16_t out[4000 * 2], ref[4000 * 2];
	struct audio_voice_cmd c[12], d[8];
	size_t n = 0, m = 0;
	uint32_t k;

	engine_init(&g_e);
	c[n++] = OS_ON(0);
	c[n++] = SET_HZ(0, 0, 44100U, 0x4000U, 0x4000U);
	c[n++] = START(0, 0, S_DC);
	c[n++] = STOP(100, 0);
	c[n++] = OS_OFF(2000);
	c[n++] = SET_HZ(2100, 1, 44100U, 0x6000U, 0x2000U);
	c[n++] = START(2100, 1, S_S16S);
	c[n++] = SET_HZ(2200, 2, 30001U, 0x5000U, 0x5000U);
	c[n++] = START(2300, 2, S_S8);
	render_one(&g_e, out, 4000, c, n);
	CHECK(g_e.rejected == 0U && g_e.oversample == 0U, "off: rejected %u",
	      g_e.rejected);
	engine_init(&g_e);
	d[m++] = c[5];
	d[m++] = c[6];
	d[m++] = c[7];
	d[m++] = c[8];
	render_one(&g_e, ref, 4000, d, m);
	for (k = 2000; k < 4000U; k++)
		CHECK(out[2U * k] == ref[2U * k] &&
		      out[2U * k + 1U] == ref[2U * k + 1U],
		      "frame %u after switching off is %d, a fresh engine %d", k,
		      out[2U * k], ref[2U * k]);
}

/* Positions advance by (hz << 16) / 96000 per 96 kHz frame, whatever the
 * period of the rate: odd substeps, the 192 kHz limit, rates by period. */
static void test_steps(void)
{
	static int16_t out[1000 * 2];
	static const uint32_t hz[] = { 44100U, 48000U, 96000U, 192000U, 300000U,
	                               11025U, 22050U, 1U, 3U };
	struct audio_voice_cmd c[4];
	uint32_t i, idx, frac;

	for (i = 0; i < sizeof(hz) / sizeof(hz[0]); i++) {
		const uint32_t used_hz = hz[i] > 192000U ? 192000U : hz[i];
		const uint32_t step = (uint32_t)(((uint64_t)used_hz << 16) / 96000U);
		const uint64_t pos = (uint64_t)960U * step;

		engine_init(&g_e);
		c[0] = OS_ON(0);
		c[1] = SET_HZ(0, 0, hz[i], 0x1000U, 0x1000U);
		c[2] = START(0, 0, S_DC);
		render_one(&g_e, out, 480, c, 3);
		audio_voice_position(&g_e, 0, &idx, &frac);
		CHECK(zz9k_voice_rate_step_oversampled(hz[i]) == step, "SDK step %u",
		      hz[i]);
		CHECK(g_e.voice[0].step == step, "hz %u: step %u, expected %u",
		      hz[i], g_e.voice[0].step, step);
		CHECK(idx == (uint32_t)(pos >> 16) && frac == (uint32_t)(pos & 0xFFFF),
		      "hz %u: position %u.%04x after 480 frames, expected %u.%04x",
		      hz[i], idx, frac, (uint32_t)(pos >> 16),
		      (uint32_t)(pos & 0xFFFF));
	}
	/* a Paula period: the rate is the 48 kHz step, the step half of it */
	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = mk(0, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE, 124, 0);
	render_one(&g_e, out, 10, c, 2);
	CHECK(g_e.voice[0].rate == (uint32_t)(((uint64_t)(AUDIO_VOICE_CLOCK_PAL /
	      124U) << 16) / 48000U) &&
	      g_e.voice[0].step == g_e.voice[0].rate / 2U, "period rate %u/%u",
	      g_e.voice[0].rate, g_e.voice[0].step);
}

/* ---- alias energy ---- */

static double goertzel(const int16_t *x, uint32_t n, double hz)
{
	const double w = 2.0 * 3.14159265358979323846 * hz / 48000.0;
	const double coeff = 2.0 * cos(w);
	double s1 = 0.0, s2 = 0.0;
	uint32_t i;

	for (i = 0; i < n; i++) {
		const double s0 = (double)x[2U * i] + coeff * s1 - s2;

		s2 = s1;
		s1 = s0;
	}
	return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / ((double)n * (double)n);
}

static void set_tone(uint32_t off, uint32_t frames, double cycles_per_frame)
{
	uint32_t i;

	for (i = 0; i < frames; i++)
		put16(off + 2U * i, (int32_t)floor(
			16000.0 * sin(2.0 * 3.14159265358979323846 * cycles_per_frame *
			              (double)i) + 0.5));
}

/* A 30 kHz tone stored at 72 kHz and played at 72 kHz: without oversampling
 * the 48 kHz output folds it to 18 kHz; with it the decimator removes it
 * before the fold. A 5 kHz tone passes alike in both. */
static void test_alias(void)
{
	static int16_t plain[2600 * 2], over[2600 * 2];
	static const double tones[2] = { 30000.0, 5000.0 };
	struct audio_voice_cmd c[4];
	double energy[2][2];
	uint32_t t, mode;

	for (t = 0; t < 2U; t++) {
		set_tone(g_smp[S_DC].off, 4000U, tones[t] / 72000.0);
		for (mode = 0; mode < 2U; mode++) {
			struct audio_voice_cmd *p = c;
			int16_t *o = mode ? over : plain;

			engine_init(&g_e);
			if (mode)
				*p++ = OS_ON(0);
			*p++ = SET_HZ(0, 0, 72000U, 0x8000U, 0x8000U);
			*p++ = START(0, 0, S_DC);
			*p++ = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP,
			          AUDIO_VOICE_INTERP_MODE_LINEAR, 0);
			render_one(&g_e, o, 2600, c, (size_t)(p - c));
			energy[t][mode] = goertzel(o + 2 * 200, 2000,
			                           t == 0U ? 18000.0 : 5000.0);
		}
	}
	{
		const double db = 10.0 * log10((energy[0][0] + 1e-9) /
		                               (energy[0][1] + 1e-9));
		const double dp = 10.0 * log10(energy[1][1] / energy[1][0]);

		printf("audio_voice_oversample_test: 72 kHz voice, 30 kHz tone: "
		       "energy at 18 kHz %.1f dB lower with oversampling; 5 kHz "
		       "tone %+.2f dB\n", db, dp);
		CHECK(db > 15.0, "alias at 18 kHz only %.1f dB lower", db);
		CHECK(fabs(dp) < 1.0, "5 kHz tone changes by %.2f dB", dp);
	}
	fill_dc();
}

/* ---- the soft knee after the filter ---- */

static void test_limiter(void)
{
	static int16_t out[1000 * 2];
	static int32_t w[2000];
	struct audio_voice_cmd c[8];
	uint32_t k;

	engine_init(&g_e);
	c[0] = OS_ON(0);
	c[1] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_LIMITER, 0, 1);
	c[2] = MASTER(0, 0x400);
	c[3] = SET_HZ(0, 0, 24000U, 0x6000U, 0x6000U);
	c[4] = START(0, 0, S_DC);
	render_one(&g_e, out, 1000, c, 5);
	for (k = 0; k < 2000U; k++)
		w[k] = (int32_t)(((int64_t)((32767 * 0x6000) >> 16) * 0x400) >> 8);
	for (k = 0; k < 1000U; k++) {
		const double want = ref_limit(ref_fir(w, k));

		CHECK(fabs((double)out[2U * k] - want) <= 2.0 && out[2U * k] < 32767,
		      "limited frame %u: %d, curve %.1f", k, out[2U * k], want);
	}
	CHECK(out[2U * 999U] > 30000, "limited level %d", out[2U * 999U]);
}

int main(void)
{
	unsigned long seqs = 3000, seed = 1;
	const char *s;

	s = getenv("ZZ9K_VOICE_OS_SEQS");
	if (s != NULL && *s != '\0')
		seqs = strtoul(s, NULL, 10);
	s = getenv("ZZ9K_VOICE_OS_SEED");
	if (s != NULL && *s != '\0')
		seed = strtoul(s, NULL, 10);
	g_rng = (uint32_t)((seed * 2654435761UL) & 0xFFFFFFFFUL) | 1U;
	taps_init();
	bank_fill();
	test_delay();
	test_delay_status();
	test_levels();
	test_master_mid_period();
	test_max_ramp();
	test_history_drain();
	test_tail_in_silent_chunk();
	test_history_cleared();
	test_reset_history();
	test_acceptance();
	test_back_to_48k();
	test_steps();
	test_alias();
	test_limiter();
	test_random(seqs);
	if (failures != 0) {
		printf("audio_voice_oversample_test: %d FAILURES\n", failures);
		return 1;
	}
	printf("audio_voice_oversample_test: all passed (seed %lu)\n", seed);
	return 0;
}
