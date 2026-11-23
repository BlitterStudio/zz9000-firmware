/*
 * Exact comparison of the voice engine (audio_voice.c) against the frozen
 * copy of master dfa9b767 (baseline/audio_voice_baseline.c).
 *
 * A seeded random corpus of sequences drives both engines with the same
 * bank, sample table and records. Each sequence is a handful of render
 * calls of arbitrary length, so renders split anywhere, with records that
 * are late, exactly on a render boundary, past the render, out of order,
 * or malformed (the existing ops only). After every render the S16 output,
 * the number of records consumed and every engine and voice field must be
 * identical. The corpus covers all four sample formats, nearest and linear
 * sessions, master gain from 0 to above the cap, loops, one-shot ends,
 * backward play and direction changes, START on active voices, 1 to 32
 * voices, rates from 1 Hz to above 192 kHz and Paula periods down to the
 * minimum, and banks that end exactly at an unmapped guard page so a read
 * past the bank crashes the run.
 *
 * Environment:
 *   ZZ9K_VOICE_BASELINE_SEQS  random sequences to run (default 10000)
 *   ZZ9K_VOICE_BASELINE_SEED  base seed (default 1)
 *   ZZ9K_VOICE_BASELINE_ONLY  run only this sequence number, verbosely
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
#include <sys/mman.h>
#include <unistd.h>
#define HAVE_GUARD_PAGE 1
#endif

#include "audio_voice.h"
#include "audio_voice_baseline.h"

#define DEFAULT_SEQS 30000U
#define MAX_BANK     8192U
#define MAX_FRAMES   2600U
#define MAX_CMDS     256U
#define MAX_SAMPLES_USED 8U
#define SENTINEL     0x5A5A

typedef struct audio_voice_engine cur_engine;
typedef struct audio_voice_baseline_engine ref_engine;
typedef struct audio_voice_cmd cur_cmd;
typedef struct audio_voice_baseline_cmd ref_cmd;

static int failures;

/* ---- deterministic random numbers ---- */

static uint64_t rng_state;

static uint64_t rnd64(void)
{
	/* splitmix64 */
	uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static uint32_t rnd(uint32_t n)
{
	return n == 0U ? 0U : (uint32_t)(rnd64() >> 32) % n;
}

/* ---- guarded bank ---- */

static uint8_t *g_arena;
static size_t g_arena_len;
static uint8_t g_fallback[MAX_BANK + 64U];

static uint8_t *bank_alloc(uint32_t len)
{
#ifdef HAVE_GUARD_PAGE
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	size_t body = (MAX_BANK + page - 1U) / page * page;

	if (g_arena == NULL) {
		g_arena_len = body + 2U * page;
		g_arena = mmap(NULL, g_arena_len, PROT_NONE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (g_arena == MAP_FAILED) {
			g_arena = NULL;
			return g_fallback;
		}
		mprotect(g_arena + page, body, PROT_READ | PROT_WRITE);
	}
	/* The bank ends exactly where the unmapped page starts. */
	return g_arena + page + body - len;
#else
	(void)len;
	return g_fallback;
#endif
}

/* ---- sequence description ---- */

struct sample_def {
	uint32_t offset, length, format;
};

static struct {
	uint8_t *bank;
	uint32_t bank_len;
	uint32_t nsamples;
	struct sample_def smp[MAX_SAMPLES_USED];
	uint32_t nvoices;
	uint32_t clock;
	uint32_t interp;
	uint32_t seq;
	int verbose;
} S;

static uint32_t rand_hz(void)
{
	switch (rnd(14)) {
	case 0: return 1;
	case 1: return 1U + rnd(50);
	case 2: return 8363;
	case 3: return 22050;
	case 4: return 44100;
	case 5: return 47999U + rnd(3);
	case 6: return 96000;
	case 7: return 192000;
	case 8: return 192001U + rnd(100000);
	case 9: {
		/* log-uniform 1 .. 200000 */
		uint32_t bits = rnd(18);

		return 1U + (rnd(1U << bits) % 200000U);
	}
	case 10: return 48000U * (1U + rnd(4));
	case 11: return 24000U;
	case 12: return 1U + rnd(192000);
	default: return 12000U + rnd(40000);
	}
}

static uint32_t rand_period(void)
{
	switch (rnd(12)) {
	case 0: return 5;
	case 1: return 4;
	case 2: return 0;
	case 3: return 1;
	case 4: return 124;
	case 5: return 214;
	case 6: return 428;
	case 7: return 856;
	case 8: return 5U + rnd(1000);
	case 9: return 65535;
	case 10: return (uint32_t)rnd64();
	default: return 5U + rnd(65531);
	}
}

static uint32_t rand_vol16(void)
{
	switch (rnd(8)) {
	case 0: return 0;
	case 1: return 65535;
	case 2: return 65535;
	case 3: return 32768;
	case 4: return 1;
	default: return rnd(65536);
	}
}

static uint32_t rand_master(void)
{
	switch (rnd(14)) {
	case 0: return 0;
	case 1: return 1;
	case 2: return 255;
	case 3:
	case 4:
	case 5: return 256;
	case 6: return 257;
	case 7: return 512;
	case 8: return 1000;
	case 9: return 65535;
	case 10: return 65536;
	case 11: return 70000;
	case 12: return (uint32_t)rnd64();
	default: return rnd(2000);
	}
}

static uint32_t sample_bytes(uint32_t id)
{
	return id < S.nsamples ? S.smp[id].length : 0U;
}

static uint32_t rand_sample_id(void)
{
	uint32_t r = rnd(100);

	if (r < 90)
		return rnd(S.nsamples);
	if (r < 95)
		return S.nsamples + rnd(3);
	if (r < 98)
		return 255;
	return 256U + rnd(70000);
}

static void gen_cmd(cur_cmd *c, uint32_t stamp)
{
	uint32_t r = rnd(100);
	uint32_t id, len;

	memset(c, 0, sizeof(*c));
	c->stamp = stamp;
	if (rnd(100) < 93)
		c->voice = (uint8_t)rnd(S.nvoices);
	else
		c->voice = (uint8_t)rnd(256);
	if (r < 22) {
		c->op = AUDIO_VOICE_OP_START;
		id = rand_sample_id();
		len = sample_bytes(id);
		c->flags = (uint16_t)id;
		switch (rnd(6)) {
		case 0:
		case 1:
		case 2: c->arg0 = 0; break;
		case 3: c->arg0 = rnd(len + 1U); break;
		case 4: c->arg0 = len + rnd(4); break;
		default: c->arg0 = rnd(len + 4U); break;
		}
		switch (rnd(6)) {
		case 0:
		case 1:
		case 2: c->arg1 = 0; break;
		case 3: c->arg1 = 1U + rnd(4); break;
		case 4: c->arg1 = rnd(len + 8U); break;
		default: c->arg1 = (uint32_t)rnd64(); break;
		}
	} else if (r < 36) {
		c->op = AUDIO_VOICE_OP_LOOP;
		id = rnd(S.nsamples);
		len = sample_bytes(id);
		/* Use the byte length of the sample the voice probably plays. */
		switch (rnd(8)) {
		case 0: c->arg0 = 0; c->arg1 = 0; break;
		case 1: c->arg0 = 0; c->arg1 = len; break;
		case 2: c->arg0 = rnd(len + 1U); c->arg1 = 0; break;
		case 3: c->arg0 = rnd(len + 1U); c->arg1 = (uint32_t)rnd64(); break;
		case 4: c->arg0 = len / 2U; c->arg1 = len - len / 2U; break;
		case 5: c->arg0 = rnd(len + 1U); c->arg1 = 1U + rnd(8); break;
		case 6: c->arg0 = (uint32_t)rnd64(); c->arg1 = 1U + rnd(8); break;
		default: {
			uint32_t ls = rnd(len + 1U);

			c->arg0 = ls;
			c->arg1 = len > ls ? 1U + rnd(len - ls) : 0U;
			break;
		}
		}
	} else if (r < 70) {
		c->op = AUDIO_VOICE_OP_SET;
		switch (rnd(8)) {
		case 0:
			c->flags = AUDIO_VOICE_SET_VOLUME;
			break;
		case 1:
			c->flags = AUDIO_VOICE_SET_RATE;
			break;
		case 2:
			c->flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ;
			break;
		case 3:
			c->flags = (uint16_t)rnd(8);
			break;
		case 4:
		case 5:
			c->flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
			           AUDIO_VOICE_SET_VOLUME;
			break;
		default:
			c->flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME;
			break;
		}
		c->arg0 = (c->flags & AUDIO_VOICE_RATE_HZ) ? rand_hz() :
		          rand_period();
		c->arg1 = (rand_vol16() << 16) | rand_vol16();
	} else if (r < 76) {
		c->op = AUDIO_VOICE_OP_STOP;
	} else if (r < 88) {
		c->op = AUDIO_VOICE_OP_DIRECTION;
		c->flags = (uint16_t)(rnd(3) == 0 ? rnd(4) :
		           (rnd(2) ? AUDIO_VOICE_DIR_BACKWARD : 0U));
	} else if (r < 99) {
		c->op = AUDIO_VOICE_OP_MASTER;
		c->arg0 = rand_master();
	} else {
		static const uint8_t bad[] = { 0, 7, 8, 9, 200, 255 };

		c->op = bad[rnd(sizeof(bad))];
		c->flags = (uint16_t)rnd(65536);
		c->arg0 = (uint32_t)rnd64();
		c->arg1 = (uint32_t)rnd64();
	}
}

static uint32_t rand_frames(void)
{
	switch (rnd(12)) {
	case 0: return 0;
	case 1: return 1;
	case 2: return 1U + rnd(8);
	case 3: return 960;
	case 4: return 961;
	case 5: return 1920;
	case 6: return 2U * 960U + 1U + rnd(20);
	case 7: return 1U + rnd(100);
	case 8: return 480;
	default: return 1U + rnd(1500);
	}
}

/* ---- comparison ---- */

#define FAILF(...) do { \
	failures++; \
	printf("FAIL seq %u: ", S.seq); \
	printf(__VA_ARGS__); \
	printf("\n"); \
	return 0; \
} while (0)

#define SAME_FIELD(a, b, f) do { \
	if ((a).f != (b).f) \
		FAILF("%s voice %u: cur %u ref %u", #f, k, \
		      (unsigned)(a).f, (unsigned)(b).f); \
} while (0)

static int compare_state(const cur_engine *c, const ref_engine *r,
                         const char *when)
{
	uint32_t k;

	if (c->frame != r->frame || c->late != r->late ||
	    c->rejected != r->rejected || c->master != r->master ||
	    c->interp != r->interp)
		FAILF("%s: engine frame %u/%u late %u/%u rejected %u/%u "
		      "master %u/%u", when, c->frame, r->frame, c->late,
		      r->late, c->rejected, r->rejected, c->master, r->master);
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
		const struct audio_voice *a = &c->voice[k];
		const struct audio_voice_baseline *b = &r->voice[k];

		SAME_FIELD(*a, *b, active);
		SAME_FIELD(*a, *b, pos);
		SAME_FIELD(*a, *b, frac);
		SAME_FIELD(*a, *b, step);
		SAME_FIELD(*a, *b, vol_l);
		SAME_FIELD(*a, *b, vol_r);
		SAME_FIELD(*a, *b, end);
		SAME_FIELD(*a, *b, loopstart);
		SAME_FIELD(*a, *b, looplen);
		SAME_FIELD(*a, *b, backward);
		SAME_FIELD(*a, *b, shift);
		SAME_FIELD(*a, *b, wide);
		SAME_FIELD(*a, *b, stereo);
		SAME_FIELD(*a, *b, base);
		SAME_FIELD(*a, *b, len);
	}
	return 1;
}

#define OUT_INTS (2U * (MAX_FRAMES + 64U))
static int16_t g_out_cur[OUT_INTS];
static int16_t g_out_ref[OUT_INTS];

/* Stable insertion sort by stamp; the records of one stamp keep their order. */
static void sort_cmds(cur_cmd *c, uint32_t n)
{
	uint32_t i, j;

	for (i = 1; i < n; i++) {
		cur_cmd t = c[i];

		for (j = i; j > 0 && c[j - 1].stamp > t.stamp; j--)
			c[j] = c[j - 1];
		c[j] = t;
	}
}

/* What the corpus exercised, to catch a generator that went quiet. */
static struct {
	uint32_t renders, audible, clipped, linear, active_end, late;
} st;

static void note_render(const int16_t *out, uint32_t frames,
                        const ref_engine *r)
{
	uint32_t i, k, audible = 0, clipped = 0, active = 0;

	for (i = 0; i < 2U * frames; i++) {
		audible |= out[i] != 0;
		clipped |= out[i] == 32767 || out[i] == -32768;
	}
	for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++)
		active += r->voice[k].active;
	st.renders++;
	st.audible += audible;
	st.clipped += clipped;
	st.linear += r->interp == AUDIO_VOICE_INTERP_LINEAR;
	st.active_end += active != 0U;
	st.late += r->late != 0U;
}

static int run_sequence(uint32_t seq, uint64_t seed)
{
	static cur_engine ec;
	static ref_engine er;
	static cur_cmd pending[MAX_CMDS * 2U];
	static ref_cmd pending_ref[MAX_CMDS * 2U];
	uint32_t npending = 0, nrenders, ri, i, loud;
	uint32_t frame = 0;

	S.seq = seq;
	rng_state = seed + (uint64_t)seq * 0xD1B54A32D192ED03ULL;
	(void)rnd64();

	/* bank */
	switch (rnd(6)) {
	case 0: S.bank_len = 1U + rnd(16); break;
	case 1: S.bank_len = 1U + rnd(80); break;
	case 2: S.bank_len = MAX_BANK; break;
	default: S.bank_len = 16U + rnd(MAX_BANK - 16U); break;
	}
	S.bank = bank_alloc(S.bank_len);
	loud = rnd(8);
	{
		int32_t walk = 0;

		for (i = 0; i < S.bank_len; i++) {
			uint32_t v = (uint32_t)rnd64();

			if (loud == 0) {
				S.bank[i] = (uint8_t)((v & 1U) ? 0x7F : 0x80);
			} else if (loud == 1) {
				S.bank[i] = (uint8_t)((v & 7U) == 0 ? v >> 8 :
				            ((v & 8U) ? 0x7F : 0x80));
			} else if (loud <= 3) {
				S.bank[i] = (uint8_t)(v >> 8);
			} else if (loud <= 5) {
				/* quiet random walk */
				walk += (int32_t)((v >> 8) % 7U) - 3;
				if (walk > 40)
					walk = 40;
				if (walk < -40)
					walk = -40;
				S.bank[i] = (uint8_t)walk;
			} else {
				S.bank[i] = (uint8_t)((int32_t)((v >> 8) % 33U) - 16);
			}
		}
	}

	/* engines */
	switch (rnd(8)) {
	case 0: S.clock = AUDIO_VOICE_CLOCK_NTSC; break;
	case 1: S.clock = 1U + rnd(100000000U); break;
	default: S.clock = AUDIO_VOICE_CLOCK_PAL; break;
	}
	S.interp = rnd(2) ? AUDIO_VOICE_INTERP_LINEAR : AUDIO_VOICE_INTERP_NEAREST;
	audio_voice_init(&ec, S.bank, S.bank_len, S.clock);
	audio_voice_baseline_init(&er, S.bank, S.bank_len, S.clock);
	ec.interp = S.interp;
	er.interp = S.interp;

	/* samples */
	S.nsamples = 1U + rnd(MAX_SAMPLES_USED);
	for (i = 0; i < S.nsamples; i++) {
		struct sample_def *d = &S.smp[i];
		uint32_t fmt = rnd(30) == 0 ? 4U + rnd(3) : rnd(4);
		uint32_t shift = fmt == 3U ? 2U : (fmt == 1U || fmt == 2U) ? 1U : 0U;
		uint32_t span;
		int rc_c, rc_r;

		switch (rnd(5)) {
		case 0: d->offset = 0; break;
		case 1: d->offset = S.bank_len - 1U - rnd(S.bank_len < 8 ? S.bank_len : 8); break;
		default: d->offset = rnd(S.bank_len); break;
		}
		span = S.bank_len - d->offset;
		switch (rnd(7)) {
		case 0: d->length = span; break;
		case 1: d->length = rnd(8); break;
		case 2: d->length = (1U + rnd(6)) << shift; break;
		case 3: d->length = span + rnd(3); break;
		case 4: d->length = (rnd(span / 8U + 1U) << shift); break;
		default: d->length = rnd(span + 1U); break;
		}
		if (d->length > span && rnd(4) != 0)
			d->length = span;
		d->format = fmt;
		rc_c = audio_voice_sample_set(&ec, i, d->offset, d->length, fmt);
		rc_r = audio_voice_baseline_sample_set(&er, i, d->offset, d->length, fmt);
		if (rc_c != rc_r)
			FAILF("sample_set %u rc cur %d ref %d", i, rc_c, rc_r);
		if (rc_c != 0)
			d->length = 0; /* undefined: generator offsets stay small */
	}

	/* voices */
	switch (rnd(6)) {
	case 0: S.nvoices = 1; break;
	case 1: S.nvoices = 32; break;
	case 2: S.nvoices = 1U + rnd(4); break;
	case 3: S.nvoices = 9U + rnd(24); break;
	default: S.nvoices = 1U + rnd(AUDIO_VOICE_MAX_VOICES); break;
	}

	/* initial setup, then free-form records */
	nrenders = 1U + rnd(5);
	for (ri = 0; ri < nrenders; ri++) {
		uint32_t frames = rand_frames();
		uint32_t ncmds, nnew, sorted = rnd(10) != 0;
		size_t used_c, used_r;

		nnew = 0;
		if (ri == 0) {
			/* every voice gets a START, a rate and a volume */
			uint32_t v;

			for (v = 0; v < S.nvoices && npending < MAX_CMDS / 2U; v++) {
				cur_cmd *c;
				uint32_t id;

				if (rnd(5) == 0)
					continue;
				id = rnd(S.nsamples);
				c = &pending[npending++];
				memset(c, 0, sizeof(*c));
				c->stamp = frame;
				c->voice = (uint8_t)v;
				c->op = AUDIO_VOICE_OP_START;
				c->flags = (uint16_t)id;
				c->arg0 = rnd(3) ? 0U : rnd(sample_bytes(id) + 1U);
				c->arg1 = rnd(3) ? 0U : rnd(sample_bytes(id) + 4U);
				c = &pending[npending++];
				memset(c, 0, sizeof(*c));
				c->stamp = frame;
				c->voice = (uint8_t)v;
				c->op = AUDIO_VOICE_OP_SET;
				c->flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME |
				           AUDIO_VOICE_RATE_HZ;
				c->arg0 = rand_hz();
				c->arg1 = (rand_vol16() << 16) | rand_vol16();
				if (rnd(3) == 0) {
					c = &pending[npending++];
					memset(c, 0, sizeof(*c));
					c->stamp = frame;
					c->voice = (uint8_t)v;
					c->op = AUDIO_VOICE_OP_LOOP;
					c->arg1 = rnd(2) ? sample_bytes(id) :
					          rnd(sample_bytes(id) + 1U);
				}
			}
		}
		ncmds = rnd(24);
		if (rnd(8) == 0)
			ncmds += rnd(80);
		for (i = 0; i < ncmds && npending + nnew < MAX_CMDS; i++) {
			uint32_t stamp;

			switch (rnd(10)) {
			case 0: stamp = frame; break;
			case 1: stamp = frame + frames; break;
			case 2: stamp = frame + (frames ? frames - 1U : 0U); break;
			case 3: stamp = frame - rnd(5); break;
			case 4: stamp = frame + frames + rnd(40); break;
			default: stamp = frame + rnd(frames + 1U); break;
			}
			gen_cmd(&pending[npending + nnew], stamp);
			nnew++;
		}
		npending += nnew;
		if (sorted)
			sort_cmds(pending, npending);
		for (i = 0; i < npending; i++) {
			ref_cmd *rc = &pending_ref[i];

			rc->stamp = pending[i].stamp;
			rc->voice = pending[i].voice;
			rc->op = pending[i].op;
			rc->flags = pending[i].flags;
			rc->arg0 = pending[i].arg0;
			rc->arg1 = pending[i].arg1;
		}

		for (i = 0; i < sizeof(g_out_cur) / sizeof(g_out_cur[0]); i++)
			g_out_cur[i] = g_out_ref[i] = SENTINEL;
		used_c = audio_voice_render(&ec, g_out_cur, frames, pending, npending);
		used_r = audio_voice_baseline_render(&er, g_out_ref, frames,
		                                     pending_ref, npending);
		if (used_c != used_r)
			FAILF("render %u used cur %zu ref %zu", ri, used_c, used_r);
		if (memcmp(g_out_cur, g_out_ref, sizeof(g_out_cur)) != 0) {
			uint32_t f;

			for (f = 0; f < 2U * (MAX_FRAMES + 64U); f++)
				if (g_out_cur[f] != g_out_ref[f])
					break;
			FAILF("render %u (%u frames at engine frame %u, %u records) "
			      "output differs at frame %u side %u: cur %d ref %d",
			      ri, frames, frame, npending, f / 2U, f & 1U,
			      g_out_cur[f], g_out_ref[f]);
		}
		if (!compare_state(&ec, &er, "after render"))
			return 0;
		note_render(g_out_ref, frames, &er);
		/* keep what was not consumed */
		memmove(pending, pending + used_c,
		        (npending - used_c) * sizeof(pending[0]));
		npending -= (uint32_t)used_c;
		frame += frames;
		if (S.verbose)
			printf("seq %u render %u frames %u used %zu ok\n", seq, ri,
			       frames, used_c);
	}
	return 1;
}

/* ---- directed rate sweep: one looped voice, every format and direction --- */

static int run_sweep(void)
{
	static const uint32_t hz[] = {
		1, 2, 7, 100, 1000, 8363, 11025, 22050, 32000, 44100, 47999,
		48000, 48001, 60000, 95999, 96000, 96001, 144000, 191999, 192000,
		200000
	};
	static const uint32_t loops[][2] = {
		/* start frame, length in frames; length 0 = none */
		{ 0, 0 }, { 0, 100 }, { 3, 1 }, { 7, 2 }, { 0, 31 }, { 20, 60 }
	};
	uint32_t fmt, lin, back, h, l;
	uint32_t count = 0;
	static cur_engine ec;
	static ref_engine er;
	static cur_cmd cmds[8];
	static ref_cmd rcmds[8];

	for (fmt = 0; fmt < 4; fmt++)
	for (lin = 0; lin < 2; lin++)
	for (back = 0; back < 2; back++)
	for (h = 0; h < sizeof(hz) / sizeof(hz[0]); h++)
	for (l = 0; l < sizeof(loops) / sizeof(loops[0]); l++) {
		uint32_t shift = fmt == 3U ? 2U : (fmt == 1U || fmt == 2U) ? 1U : 0U;
		uint32_t frames_in_bank = 100;
		uint32_t bytes = frames_in_bank << shift;
		uint32_t i, r;

		S.seq = 100000U + count;
		count++;
		S.bank_len = bytes;
		S.bank = bank_alloc(bytes);
		rng_state = 0x1234567ULL + count;
		for (i = 0; i < bytes; i++)
			S.bank[i] = (uint8_t)(rnd64() >> 40);
		audio_voice_init(&ec, S.bank, S.bank_len, AUDIO_VOICE_CLOCK_PAL);
		audio_voice_baseline_init(&er, S.bank, S.bank_len,
		                          AUDIO_VOICE_CLOCK_PAL);
		ec.interp = er.interp = lin ? AUDIO_VOICE_INTERP_LINEAR :
		                              AUDIO_VOICE_INTERP_NEAREST;
		audio_voice_sample_set(&ec, 0, 0, bytes, fmt);
		audio_voice_baseline_sample_set(&er, 0, 0, bytes, fmt);
		memset(cmds, 0, sizeof(cmds));
		cmds[0].op = AUDIO_VOICE_OP_START;
		cmds[0].flags = 0;
		/* backward play starts near the end */
		cmds[0].arg0 = back ? (frames_in_bank - 5U) << shift : 0U;
		cmds[1].op = AUDIO_VOICE_OP_LOOP;
		cmds[1].arg0 = loops[l][0] << shift;
		cmds[1].arg1 = loops[l][1] << shift;
		cmds[2].op = AUDIO_VOICE_OP_SET;
		cmds[2].flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
		                AUDIO_VOICE_SET_VOLUME;
		cmds[2].arg0 = hz[h];
		cmds[2].arg1 = (65535U << 16) | 40000U;
		cmds[3].op = AUDIO_VOICE_OP_DIRECTION;
		cmds[3].flags = back ? AUDIO_VOICE_DIR_BACKWARD : 0U;
		for (i = 0; i < 4; i++) {
			rcmds[i].stamp = cmds[i].stamp;
			rcmds[i].voice = cmds[i].voice;
			rcmds[i].op = cmds[i].op;
			rcmds[i].flags = cmds[i].flags;
			rcmds[i].arg0 = cmds[i].arg0;
			rcmds[i].arg1 = cmds[i].arg1;
		}
		for (r = 0; r < 4; r++) {
			/* the records apply in the first render; the rest only play */
			uint32_t frames = r == 0 ? 960U : 1U + 800U * r;
			size_t nrec = r == 0 ? 4U : 0U;
			size_t uc, ur;

			for (i = 0; i < OUT_INTS; i++)
				g_out_cur[i] = g_out_ref[i] = SENTINEL;
			uc = audio_voice_render(&ec, g_out_cur, frames, cmds, nrec);
			ur = audio_voice_baseline_render(&er, g_out_ref, frames, rcmds,
			                                 nrec);
			if (uc != ur || memcmp(g_out_cur, g_out_ref,
			                       sizeof(g_out_cur)) != 0)
				FAILF("sweep fmt %u lin %u back %u hz %u loop %u render %u",
				      fmt, lin, back, hz[h], l, r);
			if (!compare_state(&ec, &er, "sweep"))
				return 0;
		}
	}
	return 1;
}

int main(void)
{
	const char *env;
	uint32_t seqs = DEFAULT_SEQS, only = 0, seq;
	uint64_t seed = 1;
	int have_only = 0;
	uint32_t done = 0;

	if ((env = getenv("ZZ9K_VOICE_BASELINE_SEQS")) != NULL)
		seqs = (uint32_t)strtoul(env, NULL, 0);
	if ((env = getenv("ZZ9K_VOICE_BASELINE_SEED")) != NULL)
		seed = strtoull(env, NULL, 0);
	if ((env = getenv("ZZ9K_VOICE_BASELINE_ONLY")) != NULL) {
		only = (uint32_t)strtoul(env, NULL, 0);
		have_only = 1;
		S.verbose = 1;
	}

#ifdef HAVE_GUARD_PAGE
	/* A mixer that stops advancing would loop forever: fail instead. */
	alarm(60U + seqs / 100U);
#endif
	if (have_only) {
		run_sequence(only, seed);
	} else {
		(void)run_sweep();
		for (seq = 0; seq < seqs && failures < 5; seq++) {
			(void)run_sequence(seq, seed);
			done++;
		}
	}
	if (failures) {
		printf("audio_voice_baseline_test: %d FAILURES\n", failures);
		return 1;
	}
	if (!have_only && seqs >= 1000U &&
	    (st.audible * 10U < st.renders * 7U || st.clipped * 50U < st.renders ||
	     st.active_end * 10U < st.renders * 2U || st.linear == 0U)) {
		printf("audio_voice_baseline_test: corpus too weak (renders %u "
		       "audible %u clipped %u linear %u still active %u)\n",
		       st.renders, st.audible, st.clipped, st.linear, st.active_end);
		return 1;
	}
	printf("audio_voice_baseline_test: %u random sequences + rate sweep "
	       "identical to the baseline engine (seed %llu); %u renders, %u "
	       "audible, %u clipped, %u with voices still sounding, %u with "
	       "records late\n", done, (unsigned long long)seed, st.renders,
	       st.audible, st.clipped, st.active_end, st.late);
	return 0;
}
