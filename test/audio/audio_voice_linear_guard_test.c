/*
 * The linear reader of the voice engine reads exactly the two frames of each
 * blend and mixes what an independent model says.
 *
 * The linear read of the vector path loads the two frames of a blend as one
 * pair at the lower address, four frames at a time, so a read of any byte
 * outside the pair is a fault in this test, not an unused lane that happens
 * not to change the sound. Every scenario plays one voice linearly and
 *
 *   - the sample sits flush against a PROT_NONE page (the end of the bank
 *     page for forward play, its start for backward play), or the bank IS
 *     the sample (its base is the first byte of the asset and its length the
 *     exact bytes of 1, 2 or 3 frames), flush against the lower and against
 *     the upper protected page, so that the page of the bank allocation
 *     never helps; a fault is caught and reported with the scenario;
 *   - the PCM is compared sample for sample with a model written here from
 *     the documented rules, not from the engine: per frame the blend
 *     (a * (65536 - f) + b * f) >> 16 (>> 8 for 8-bit data) of the frame at
 *     the position and its neighbour in the direction of play (the loop
 *     start at the end of a pass, the frame itself where there is no loop),
 *     the volume (sample * volume) >> 16, a ramp of R frames from 0 (the
 *     volume (inc * j) >> 8 with inc = (volume << 8) / R for frame j, the
 *     target from frame R), and the position, loop and direction rules of
 *     audio_voice.h. With 2x oversampling the same model runs on the 96 kHz
 *     time base (the step halved, the ramp doubled) and its stream is
 *     convolved tap by tap with audio_reduce_table.h, one output per two
 *     frames, rounded half up, and clamped: no part of the engine's
 *     decimator is used.
 *
 * Groups (all four formats, both directions):
 *   1. samples of 1 to 45 frames, ramps 0 (the scalar path), 1, 2, 3, 5 and
 *      64 (the vector path whose blocks of four leave 1, 2 and 3 frames for
 *      the scalar tail), loops and one-shot ends on every remainder, three
 *      volume pairs with one silent side;
 *   2. the registered bank of exactly 1, 2 or 3 frames at both pages;
 *   3. long ramps of 256 and 512 frames with volumes whose increments have
 *      fractions, over many blocks, and a drift of the fraction vector
 *      shows in the PCM;
 *   4. 2x oversampling with its own steps and ramps.
 * The rates run from 6 kHz to 192 kHz and include 8363 Hz and 22050 Hz, whose
 * steps are not a binary fraction.
 *
 * Linux only (mmap, mprotect, SIGSEGV); elsewhere it reports a skip. The
 * vector path exists only on ARM with NEON: on any other CPU the engine runs
 * its scalar reader and this test proves that one, not the vector reader.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include "audio_reduce_table.h"
#include "audio_voice.h"

#define FRAMES 400U

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		if (failures <= 20) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} \
} while (0)

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

static sigjmp_buf g_jump;
static void *volatile g_fault_addr;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)ctx;
	g_fault_addr = si->si_addr;
	siglongjmp(g_jump, 1);
}

static struct audio_voice_engine g_engine;
static int16_t g_out[2 * FRAMES];
static int16_t g_want[2 * FRAMES];
static struct audio_voice_cmd g_cmds[10];

static const char *const fmt_name[4] = { "S8", "S16", "S8S", "S16S" };
static const uint32_t fmt_shift[4] = { 0, 1, 1, 2 };

/* Where the sample sits. */
enum { PLACE_FLUSH = 0, /* in a page, flush with the guard it ends at */
       PLACE_EXACT_LOW, /* the bank is the sample, at the lower guard */
       PLACE_EXACT_HIGH /* the bank is the sample, at the upper guard */ };

struct scenario {
	uint32_t fmt, back, hz, len, ramp, loop, vol_l, vol_r, os, place;
};

/* ---- the model ---- */

static void frame_at(const uint8_t *s, uint32_t fmt, uint32_t idx,
                     int64_t *l, int64_t *r)
{
	switch (fmt) {
	case 0:
		*l = *r = (int8_t)s[idx];
		break;
	case 1:
		*l = *r = (int16_t)(uint16_t)((s[2U * idx] << 8) | s[2U * idx + 1U]);
		break;
	case 2:
		*l = (int8_t)s[2U * idx];
		*r = (int8_t)s[2U * idx + 1U];
		break;
	default:
		*l = (int16_t)(uint16_t)((s[4U * idx] << 8) | s[4U * idx + 1U]);
		*r = (int16_t)(uint16_t)((s[4U * idx + 2U] << 8) | s[4U * idx + 3U]);
		break;
	}
}

static int16_t clamp16(int64_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* The voice per frame of its mixing unit, before any clamp: nframes frames
 * of the 48 kHz time base, or twice that of the 96 kHz one. */
static int32_t g_wl[2 * FRAMES], g_wr[2 * FRAMES];

static void model_voice(const struct scenario *s, const uint8_t *smp,
                        uint32_t nframes)
{
	const uint32_t wide = s->fmt == 1 || s->fmt == 3;
	const uint32_t stereo = s->fmt >= 2;
	const uint64_t step = (((uint64_t)s->hz << 16) / 48000U) >> s->os;
	const uint32_t rl = s->ramp << s->os; /* ramp length in mixing frames */
	uint64_t pos = s->back ? (uint64_t)(s->len - 1U) << 16 : 0U; /* 16.16 */
	uint32_t end = s->len, loopstart = 0, looplen = s->loop ? s->len : 0U;
	int active = 1;
	uint32_t f;
	int32_t inc_l = 0, inc_r = 0;

	if (rl) {
		inc_l = (int32_t)(s->vol_l << 8) / (int32_t)rl;
		inc_r = (int32_t)(s->vol_r << 8) / (int32_t)rl;
	}
	memset(g_wl, 0, sizeof(g_wl));
	memset(g_wr, 0, sizeof(g_wr));
	for (f = 0; f < nframes && active; f++) {
		const uint32_t idx = (uint32_t)(pos >> 16);
		const int64_t fr = (int64_t)(pos & 0xFFFFU);
		uint32_t nb;
		int64_t al, ar, bl, br, l, r, vl, vr;

		if (!s->back) {
			if (idx + 1U < end)
				nb = idx + 1U;
			else
				nb = looplen ? loopstart : idx;
		} else {
			if (idx > loopstart)
				nb = idx - 1U;
			else
				nb = looplen ? loopstart + looplen - 1U : idx;
		}
		frame_at(smp, s->fmt, idx, &al, &ar);
		frame_at(smp, s->fmt, nb, &bl, &br);
		if (wide) {
			l = (al * (65536 - fr) + bl * fr) >> 16;
			r = (ar * (65536 - fr) + br * fr) >> 16;
		} else {
			l = (al * (65536 - fr) + bl * fr) >> 8;
			r = (ar * (65536 - fr) + br * fr) >> 8;
		}
		if (!stereo)
			r = l;
		if (rl && (s->vol_l | s->vol_r) != 0U && f < rl) {
			vl = ((int64_t)inc_l * f) >> 8;
			vr = ((int64_t)inc_r * f) >> 8;
		} else {
			vl = s->vol_l;
			vr = s->vol_r;
		}
		g_wl[f] = (int32_t)((l * vl) >> 16);
		g_wr[f] = (int32_t)((r * vr) >> 16);

		/* advance */
		if (!s->back) {
			pos += step;
			if ((uint32_t)(pos >> 16) >= end) {
				if (!looplen) {
					active = 0;
				} else {
					uint32_t over = (uint32_t)(pos >> 16) - end;

					if (over >= looplen)
						over = 0;
					pos = ((uint64_t)(loopstart + over) << 16) |
					      (pos & 0xFFFFU);
					end = loopstart + looplen;
				}
			}
		} else {
			int below = 0;

			if (step > pos) {
				pos = 0;
				below = 1;
			} else {
				pos -= step;
			}
			if (below || (uint32_t)(pos >> 16) < loopstart) {
				if (!looplen) {
					active = 0;
				} else {
					uint32_t under = loopstart - (uint32_t)(pos >> 16);

					if (under >= looplen)
						under = 0;
					end = loopstart + looplen;
					pos = ((uint64_t)(end - under - 1U) << 16) |
					      (pos & 0xFFFFU);
				}
			}
		}
	}
}

/* The decimator of 2x oversampling written out tap by tap: output k is the
 * sum of h[j] * x[2k - 57 + j] over the 59 taps, zero before the first
 * frame, rounded half up. */
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

static void model(const struct scenario *s, const uint8_t *smp)
{
	uint32_t f;

	if (!s->os) {
		model_voice(s, smp, FRAMES);
		for (f = 0; f < FRAMES; f++) {
			g_want[2U * f] = clamp16(g_wl[f]);
			g_want[2U * f + 1U] = clamp16(g_wr[f]);
		}
		return;
	}
	model_voice(s, smp, 2U * FRAMES);
	for (f = 0; f < FRAMES; f++) {
		g_want[2U * f] = clamp16(ref_fir(g_wl, f));
		g_want[2U * f + 1U] = clamp16(ref_fir(g_wr, f));
	}
}

/* Returns 0, 1 when the engine touched a protected page, 2 when the PCM
 * differs from the model. */
static int run_scenario(const struct scenario *s, uint8_t *bank,
                        uint32_t page, uint32_t *first_bad)
{
	const uint32_t shift = fmt_shift[s->fmt];
	const uint32_t bytes = s->len << shift;
	/* The sample sits flush with the page it ends at, or is the bank. */
	const uint32_t off = s->place == PLACE_EXACT_LOW ? 0U :
	                     s->place == PLACE_EXACT_HIGH ? page - bytes :
	                     s->back ? 0U : page - bytes;
	uint8_t *volatile abank = bank + off;
	size_t n = 0, i;

	model(s, abank);
	if (sigsetjmp(g_jump, 1) != 0)
		return 1;
	if (s->place == PLACE_FLUSH) {
		audio_voice_init(&g_engine, bank, page, 3546895U);
		if (audio_voice_sample_set(&g_engine, 0, off, bytes, s->fmt) != 0) {
			printf("FAIL sample_set\n");
			failures++;
			return 0;
		}
	} else {
		audio_voice_init(&g_engine, abank, bytes, 3546895U);
		if (audio_voice_sample_set(&g_engine, 0, 0, bytes, s->fmt) != 0) {
			printf("FAIL sample_set\n");
			failures++;
			return 0;
		}
	}
	if (s->os)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
		                 AUDIO_VOICE_CONFIG_OVERSAMPLE,
		                 AUDIO_VOICE_OVERSAMPLE_2X, 0);
	if (s->ramp)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
		                 AUDIO_VOICE_CONFIG_RAMP, s->ramp, 0);
	g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP,
	                 AUDIO_VOICE_INTERP_MODE_LINEAR, 0);
	g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE |
	                 AUDIO_VOICE_RATE_HZ | AUDIO_VOICE_SET_VOLUME, s->hz,
	                 (s->vol_l << 16) | s->vol_r);
	g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_START, 0,
	                 s->back ? (s->len - 1U) << shift : 0U, 0);
	if (s->loop)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, bytes);
	if (s->back)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_DIRECTION,
		                 AUDIO_VOICE_DIR_BACKWARD, 0, 0);
	memset(g_out, 0, sizeof(g_out));
	audio_voice_render(&g_engine, g_out, FRAMES, g_cmds, n);
	for (i = 0; i < 2U * FRAMES; i++) {
		if (g_out[i] != g_want[i]) {
			*first_bad = (uint32_t)i;
			return 2;
		}
	}
	return 0;
}

static const uint32_t g_rates[] = { 6000U, 8363U, 12000U, 22050U, 24000U,
                                    36000U, 48000U, 72000U, 96000U, 144000U,
                                    192000U };
#define NRATES (sizeof(g_rates) / sizeof(g_rates[0]))

static const uint32_t g_vols[3][2] = { { 40000U, 30000U }, { 40000U, 0U },
                                       { 0U, 30000U } };
/* odd volumes: the ramp increments (volume << 8) / R have fractions */
static const uint32_t g_vols_odd[3][2] = { { 33333U, 21845U },
                                           { 65535U, 1U }, { 1U, 65535U } };

struct group {
	const char *name;
	const uint32_t *lens;
	size_t nlens;
	const uint32_t *ramps;
	size_t nramps;
	const uint32_t (*vols)[2];
	uint32_t os;
	uint32_t place_lo, place_hi; /* PLACE_* range */
};

static uint8_t *g_bank;
static uint32_t g_page;
static uint64_t g_runs, g_faults, g_mismatches, g_silent;

static void run_group(const struct group *g)
{
	uint32_t fmt, back, place;
	size_t h, li, ri, v;
	uint32_t loop;
	uint64_t runs = 0;

	for (fmt = 0; fmt < 4U; fmt++)
	for (back = 0; back < 2U; back++)
	for (h = 0; h < NRATES; h++)
	for (place = g->place_lo; place <= g->place_hi; place++)
	for (li = 0; li < g->nlens; li++)
	for (ri = 0; ri < g->nramps; ri++)
	for (loop = 0; loop < 2U; loop++)
	for (v = 0; v < 3U; v++) {
		struct scenario s;
		uint32_t bad = 0;
		int rc;

		s.fmt = fmt;
		s.back = back;
		s.hz = g_rates[h];
		s.len = g->lens[li];
		s.ramp = g->ramps[ri];
		s.loop = loop;
		s.vol_l = g->vols[v][0];
		s.vol_r = g->vols[v][1];
		s.os = g->os;
		s.place = place;
		runs++;
		rc = run_scenario(&s, g_bank, g_page, &bad);
		if (rc == 1) {
			g_faults++;
			CHECK(0, "%s: %s %s %u Hz len %u ramp %u loop %u vol %u/%u "
			      "place %u: read at %p outside the bank (page %p..%p)",
			      g->name, fmt_name[fmt], back ? "backward" : "forward",
			      s.hz, s.len, s.ramp, loop, s.vol_l, s.vol_r, place,
			      g_fault_addr, (void *)g_bank,
			      (void *)(g_bank + g_page));
		} else if (rc == 2) {
			g_mismatches++;
			CHECK(0, "%s: %s %s %u Hz len %u ramp %u loop %u vol %u/%u "
			      "place %u: PCM differs at frame %u side %u: %d, model %d",
			      g->name, fmt_name[fmt], back ? "backward" : "forward",
			      s.hz, s.len, s.ramp, loop, s.vol_l, s.vol_r, place,
			      bad / 2U, bad & 1U, g_out[bad], g_want[bad]);
		} else if (v == 0U && s.len >= 6U) {
			uint32_t k;
			int any = 0;

			for (k = 0; k < 2U * FRAMES; k++)
				any |= g_want[k] != 0;
			if (!any)
				g_silent++;
		}
	}
	g_runs += runs;
	printf("audio_voice_linear_guard_test: %-24s %7llu scenarios\n", g->name,
	       (unsigned long long)runs);
}

int main(void)
{
	static uint32_t lens_all[45];
	static const uint32_t lens_tiny[3] = { 1U, 2U, 3U };
	static const uint32_t lens_long[3] = { 20U, 45U, 130U };
	static const uint32_t lens_os[6] = { 1U, 2U, 3U, 7U, 45U, 130U };
	static const uint32_t ramps_all[6] = { 0U, 1U, 2U, 3U, 5U, 64U };
	static const uint32_t ramps_long[2] = { 256U, 512U };
	static const uint32_t ramps_os[5] = { 0U, 1U, 5U, 64U, 256U };
	const long ps = sysconf(_SC_PAGESIZE);
	uint8_t *map;
	struct sigaction sa;
	uint32_t seed = 0x2468ACEU, i;
	struct group groups[4];

	g_page = ps > 0 ? (uint32_t)ps : 4096U;
	for (i = 0; i < 45U; i++)
		lens_all[i] = i + 1U;
	map = mmap(NULL, 3U * (size_t)g_page, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) {
		printf("audio_voice_linear_guard_test: mmap failed\n");
		return 1;
	}
	g_bank = map + g_page;
	for (i = 0; i < g_page; i++) {
		seed = seed * 1664525U + 1013904223U;
		g_bank[i] = (uint8_t)(seed >> 24);
	}
	if (mprotect(map, g_page, PROT_NONE) != 0 ||
	    mprotect(map + 2U * (size_t)g_page, g_page, PROT_NONE) != 0) {
		printf("audio_voice_linear_guard_test: mprotect failed\n");
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	taps_init();

	groups[0] = (struct group){ "flush, 1..45 frames", lens_all, 45U,
	                            ramps_all, 6U, g_vols, 0U, PLACE_FLUSH,
	                            PLACE_FLUSH };
	groups[1] = (struct group){ "exact bank, 1..3 frames", lens_tiny, 3U,
	                            ramps_all, 6U, g_vols, 0U, PLACE_EXACT_LOW,
	                            PLACE_EXACT_HIGH };
	groups[2] = (struct group){ "long ramps 256/512", lens_long, 3U,
	                            ramps_long, 2U, g_vols_odd, 0U, PLACE_FLUSH,
	                            PLACE_FLUSH };
	groups[3] = (struct group){ "2x oversampling", lens_os, 6U, ramps_os, 5U,
	                            g_vols, 1U, PLACE_FLUSH, PLACE_FLUSH };
	for (i = 0; i < 4U; i++)
		run_group(&groups[i]);
	/* a 2x bank that is the sample, both ends */
	groups[3].lens = lens_tiny;
	groups[3].nlens = 3U;
	groups[3].name = "2x exact bank";
	groups[3].place_lo = PLACE_EXACT_LOW;
	groups[3].place_hi = PLACE_EXACT_HIGH;
	run_group(&groups[3]);

	CHECK(g_silent <= 16U, "%llu scenarios with a silent model: the sweep is "
	      "not exercising the reader", (unsigned long long)g_silent);
	if (failures != 0) {
		printf("audio_voice_linear_guard_test: %d failures in %llu scenarios "
		       "(%llu faulted, %llu PCM mismatches)\n", failures,
		       (unsigned long long)g_runs, (unsigned long long)g_faults,
		       (unsigned long long)g_mismatches);
		return 1;
	}
	printf("audio_voice_linear_guard_test: %llu scenarios, 4 formats, both "
	       "directions, PCM equals the independent model, no read outside "
	       "the bank\n", (unsigned long long)g_runs);
	return 0;
}
#else
int main(void)
{
	printf("audio_voice_linear_guard_test: skipped (needs Linux mmap)\n");
	return 0;
}
#endif
