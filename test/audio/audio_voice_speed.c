/*
 * Host timing of the voice engine render path (not a pass/fail test, not
 * part of `make test`; build and run it with `make -C test/audio speed`).
 *
 * It reproduces the card's LOAD run (sdk/tools/zz9k-voiceplay LOAD): one
 * looping 16-bit sample of 4096 frames, mono or interleaved stereo, on N
 * voices at rates spread from half to twice RATE, volume 65535 / N per side,
 * rendered in 960-frame periods. It prints the nanoseconds per period of the
 * frozen master engine and of the current engine, and checks that both
 * produce the same PCM while it times them.
 *
 * Host numbers show the direction and the share of each change. The card's
 * Cortex-A9 has small caches, no out-of-order depth worth the name and no
 * hardware divide, so absolute figures and ratios there differ.
 *
 * Environment:
 *   ZZ9K_VOICE_SPEED_PERIODS  periods per trial (default 1000)
 *   ZZ9K_VOICE_SPEED_TRIALS   trials, the best is reported (default 7)
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
#include <time.h>

#include "audio_voice.h"
#include "audio_voice_baseline.h"

#define PERIOD_FRAMES 960U
#define LOAD_FRAMES 4096U
#define LOAD_PEAK 20000L

static uint8_t g_bank[LOAD_FRAMES * 4U];
static int16_t g_out_cur[2U * PERIOD_FRAMES];
static int16_t g_out_ref[2U * PERIOD_FRAMES];

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* The same sine and noise as zz9k-voiceplay LOAD. */
static long load_sine(uint32_t phase)
{
	const unsigned long long half = LOAD_FRAMES / 2U;
	unsigned long long p = phase % half;
	unsigned long long a = p * (half - p);
	long value = (long)(((unsigned long long)LOAD_PEAK * 16ULL * a) /
	                    (5ULL * half * half - 4ULL * a));

	return phase >= half ? -value : value;
}

static uint32_t make_sample(uint32_t stereo)
{
	uint32_t noise = 0x2545F491UL, out = 0, i, side;

	for (i = 0; i < LOAD_FRAMES; i++) {
		for (side = 0; side <= stereo; side++) {
			uint32_t cycles = side == 0U ? 8U : 11U;
			long value;
			uint16_t raw;

			noise = noise * 1664525UL + 1013904223UL;
			value = load_sine((i * cycles) % LOAD_FRAMES) +
			        (long)((noise >> 16) & 0x1FFU) - 256L;
			raw = (uint16_t)(int16_t)value;
			g_bank[out++] = (uint8_t)(raw >> 8);
			g_bank[out++] = (uint8_t)(raw & 0xFFU);
		}
	}
	return out;
}

static uint32_t voice_rate(uint32_t rate, uint32_t voices, uint32_t voice)
{
	if (voices > 1U)
		rate = rate / 2U + (uint32_t)(((uint64_t)(rate + rate / 2U) * voice) /
		                              (voices - 1U));
	if (rate > 192000U)
		rate = 192000U;
	return rate == 0U ? 1U : rate;
}

struct scenario {
	uint32_t voices, linear, stereo, rate;
};

static uint32_t g_periods = 1000, g_trials = 7;

static uint64_t checksum(const int16_t *p, uint32_t n)
{
	uint64_t h = 1469598103934665603ULL;
	uint32_t i;

	for (i = 0; i < n; i++)
		h = (h ^ (uint16_t)p[i]) * 1099511628211ULL;
	return h;
}

/* Returns best ns per period over the trials; hash is the PCM checksum. */
static double time_cur(const struct scenario *sc, uint32_t bytes,
                       uint64_t *hash)
{
	static struct audio_voice_engine e;
	struct audio_voice_cmd cmds[3U * AUDIO_VOICE_MAX_VOICES];
	double best = 1e30;
	uint32_t t, v, n = 0, p;

	for (v = 0; v < sc->voices; v++) {
		uint32_t vol = 0xFFFFU / sc->voices;

		memset(&cmds[n], 0, sizeof(cmds[n]));
		cmds[n].voice = (uint8_t)v;
		cmds[n].op = AUDIO_VOICE_OP_SET;
		cmds[n].flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
		                AUDIO_VOICE_SET_VOLUME;
		cmds[n].arg0 = voice_rate(sc->rate, sc->voices, v);
		cmds[n].arg1 = (vol << 16) | vol;
		n++;
		memset(&cmds[n], 0, sizeof(cmds[n]));
		cmds[n].voice = (uint8_t)v;
		cmds[n].op = AUDIO_VOICE_OP_START;
		n++;
		memset(&cmds[n], 0, sizeof(cmds[n]));
		cmds[n].voice = (uint8_t)v;
		cmds[n].op = AUDIO_VOICE_OP_LOOP;
		cmds[n].arg1 = bytes;
		n++;
	}
	for (t = 0; t < g_trials; t++) {
		uint64_t t0, t1;

		audio_voice_init(&e, g_bank, bytes, AUDIO_VOICE_CLOCK_PAL);
		e.interp = sc->linear ? AUDIO_VOICE_INTERP_LINEAR :
		                        AUDIO_VOICE_INTERP_NEAREST;
		audio_voice_sample_set(&e, 0, 0, bytes,
		                       sc->stereo ? AUDIO_VOICE_FMT_S16S :
		                                    AUDIO_VOICE_FMT_S16);
		audio_voice_render(&e, g_out_cur, PERIOD_FRAMES, cmds, n);
		t0 = now_ns();
		for (p = 0; p < g_periods; p++)
			audio_voice_render(&e, g_out_cur, PERIOD_FRAMES, NULL, 0);
		t1 = now_ns();
		if ((double)(t1 - t0) / g_periods < best)
			best = (double)(t1 - t0) / g_periods;
		*hash = checksum(g_out_cur, 2U * PERIOD_FRAMES);
	}
	return best;
}

static double time_ref(const struct scenario *sc, uint32_t bytes,
                       uint64_t *hash)
{
	static struct audio_voice_baseline_engine e;
	struct audio_voice_baseline_cmd cmds[3U * AUDIO_VOICE_MAX_VOICES];
	double best = 1e30;
	uint32_t t, v, n = 0, p;

	for (v = 0; v < sc->voices; v++) {
		uint32_t vol = 0xFFFFU / sc->voices;

		memset(&cmds[n], 0, sizeof(cmds[n]));
		cmds[n].voice = (uint8_t)v;
		cmds[n].op = AUDIO_VOICE_BASELINE_OP_SET;
		cmds[n].flags = AUDIO_VOICE_BASELINE_SET_RATE |
		                AUDIO_VOICE_BASELINE_RATE_HZ |
		                AUDIO_VOICE_BASELINE_SET_VOLUME;
		cmds[n].arg0 = voice_rate(sc->rate, sc->voices, v);
		cmds[n].arg1 = (vol << 16) | vol;
		n++;
		memset(&cmds[n], 0, sizeof(cmds[n]));
		cmds[n].voice = (uint8_t)v;
		cmds[n].op = AUDIO_VOICE_BASELINE_OP_START;
		n++;
		memset(&cmds[n], 0, sizeof(cmds[n]));
		cmds[n].voice = (uint8_t)v;
		cmds[n].op = AUDIO_VOICE_BASELINE_OP_LOOP;
		cmds[n].arg1 = bytes;
		n++;
	}
	for (t = 0; t < g_trials; t++) {
		uint64_t t0, t1;

		audio_voice_baseline_init(&e, g_bank, bytes, AUDIO_VOICE_CLOCK_PAL);
		e.interp = sc->linear ? AUDIO_VOICE_BASELINE_INTERP_LINEAR :
		                        AUDIO_VOICE_BASELINE_INTERP_NEAREST;
		audio_voice_baseline_sample_set(&e, 0, 0, bytes,
			sc->stereo ? AUDIO_VOICE_BASELINE_FMT_S16S :
			             AUDIO_VOICE_BASELINE_FMT_S16);
		audio_voice_baseline_render(&e, g_out_ref, PERIOD_FRAMES, cmds, n);
		t0 = now_ns();
		for (p = 0; p < g_periods; p++)
			audio_voice_baseline_render(&e, g_out_ref, PERIOD_FRAMES, NULL, 0);
		t1 = now_ns();
		if ((double)(t1 - t0) / g_periods < best)
			best = (double)(t1 - t0) / g_periods;
		*hash = checksum(g_out_ref, 2U * PERIOD_FRAMES);
	}
	return best;
}

/*
 * Churn: every 8 frames the next voice (of 32) is restarted, so each voice
 * restarts every 256 frames and, with a ramp of 256, the fade pool stays
 * full. The records are built before the timed loop. Returns ns per period
 * of the current engine with ramp frames `ramp` (0 = no CONFIG record).
 */
#define CHURN_MAX_PERIODS 2000U
static struct audio_voice_cmd g_churn[CHURN_MAX_PERIODS][2U * 120U];

static double time_churn(const struct scenario *sc, uint32_t bytes,
                         uint32_t ramp, int over)
{
	static struct audio_voice_engine e;
	struct audio_voice_cmd setup[3U * AUDIO_VOICE_MAX_VOICES + 2U];
	double best = 1e30;
	uint32_t t, v, n = 0, p, s, periods = g_periods < CHURN_MAX_PERIODS ?
	                                      g_periods : CHURN_MAX_PERIODS;

	if (over) {
		memset(&setup[n], 0, sizeof(setup[n]));
		setup[n].op = AUDIO_VOICE_OP_CONFIG;
		setup[n].flags = AUDIO_VOICE_CONFIG_OVERSAMPLE;
		setup[n].arg0 = AUDIO_VOICE_OVERSAMPLE_2X;
		n++;
	}
	if (ramp != 0U) {
		memset(&setup[n], 0, sizeof(setup[n]));
		setup[n].op = AUDIO_VOICE_OP_CONFIG;
		setup[n].flags = AUDIO_VOICE_CONFIG_RAMP;
		setup[n].arg0 = ramp;
		n++;
	}
	for (v = 0; v < sc->voices; v++) {
		uint32_t vol = 0xFFFFU / sc->voices;

		memset(&setup[n], 0, sizeof(setup[n]));
		setup[n].voice = (uint8_t)v;
		setup[n].op = AUDIO_VOICE_OP_SET;
		setup[n].flags = AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ |
		                AUDIO_VOICE_SET_VOLUME;
		setup[n].arg0 = voice_rate(sc->rate, sc->voices, v);
		setup[n].arg1 = (vol << 16) | vol;
		n++;
		memset(&setup[n], 0, sizeof(setup[n]));
		setup[n].voice = (uint8_t)v;
		setup[n].op = AUDIO_VOICE_OP_START;
		n++;
		memset(&setup[n], 0, sizeof(setup[n]));
		setup[n].voice = (uint8_t)v;
		setup[n].op = AUDIO_VOICE_OP_LOOP;
		setup[n].arg1 = bytes;
		n++;
	}
	/* period p starts at engine frame (p + 1) * PERIOD_FRAMES: the first
	 * period carries the setup */
	for (p = 0; p < periods; p++)
		for (s = 0; s < 120U; s++) {
			struct audio_voice_cmd *c = &g_churn[p][2U * s];
			uint32_t stamp = (p + 1U) * PERIOD_FRAMES + 8U * s;
			uint8_t voice = (uint8_t)((p * 120U + s) % sc->voices);

			memset(c, 0, 2U * sizeof(*c));
			c[0].stamp = c[1].stamp = stamp;
			c[0].voice = c[1].voice = voice;
			c[0].op = AUDIO_VOICE_OP_START;
			c[1].op = AUDIO_VOICE_OP_LOOP;
			c[1].arg1 = bytes;
		}
	for (t = 0; t < g_trials; t++) {
		uint64_t t0, t1;

		audio_voice_init(&e, g_bank, bytes, AUDIO_VOICE_CLOCK_PAL);
		e.interp = sc->linear ? AUDIO_VOICE_INTERP_LINEAR :
		                        AUDIO_VOICE_INTERP_NEAREST;
		audio_voice_sample_set(&e, 0, 0, bytes,
		                       sc->stereo ? AUDIO_VOICE_FMT_S16S :
		                                    AUDIO_VOICE_FMT_S16);
		audio_voice_render(&e, g_out_cur, PERIOD_FRAMES, setup, n);
		t0 = now_ns();
		for (p = 0; p < periods; p++)
			audio_voice_render(&e, g_out_cur, PERIOD_FRAMES, g_churn[p],
			                   2U * 120U);
		t1 = now_ns();
		if ((double)(t1 - t0) / periods < best)
			best = (double)(t1 - t0) / periods;
	}
	return best;
}

int main(void)
{
	static const uint32_t voices[] = { 1, 8, 16, 32 };
	static const uint32_t rates[] = { 22050, 96000 };
	const char *env;
	uint32_t vi, ri, lin, st;
	int bad = 0;
	uint64_t h_cur, h_ref;
	double c0, r0;

	if ((env = getenv("ZZ9K_VOICE_SPEED_PERIODS")) != NULL)
		g_periods = (uint32_t)strtoul(env, NULL, 0);
	if ((env = getenv("ZZ9K_VOICE_SPEED_TRIALS")) != NULL)
		g_trials = (uint32_t)strtoul(env, NULL, 0);

	printf("ns per 960-frame period, best of %u trials of %u periods\n",
	       g_trials, g_periods);
	printf("%-34s %12s %12s %8s\n", "scenario", "baseline", "current",
	       "speedup");

	{
		struct scenario sc = { 0, 0, 0, 22050 };
		uint32_t bytes = make_sample(0);

		r0 = time_ref(&sc, bytes, &h_ref);
		c0 = time_cur(&sc, bytes, &h_cur);
		printf("%-34s %12.0f %12.0f %7.2fx%s\n", "0 voices (fixed cost)", r0,
		       c0, r0 / c0, h_ref == h_cur ? "" : "  PCM DIFFERS");
		bad |= h_ref != h_cur;
	}
	for (st = 0; st < 2; st++) {
		uint32_t bytes = make_sample(st);

		for (lin = 0; lin < 2; lin++)
		for (ri = 0; ri < 2; ri++)
		for (vi = 0; vi < 4; vi++) {
			struct scenario sc = { voices[vi], lin, st, rates[ri] };
			char name[64];
			double r1, c1;

			r1 = time_ref(&sc, bytes, &h_ref);
			c1 = time_cur(&sc, bytes, &h_cur);
			snprintf(name, sizeof(name), "%2u voices %s %s %u Hz",
			         voices[vi], st ? "stereo" : "mono  ",
			         lin ? "linear " : "nearest", rates[ri]);
			printf("%-34s %12.0f %12.0f %7.2fx%s\n", name, r1, c1, r1 / c1,
			       h_ref == h_cur ? "" : "  PCM DIFFERS");
			bad |= h_ref != h_cur;
		}
	}
	printf("\nrestart churn (32 voices, a restart every 8 frames in turn, "
	       "22050 Hz), ns per period, current engine only\n");
	printf("%-34s %12s %12s %12s\n", "scenario", "ramp off", "ramp 256",
	       "ramp 256 2x");
	for (st = 0; st < 2; st++)
	for (lin = 0; lin < 2; lin++) {
		uint32_t bytes = make_sample(st);
		struct scenario sc = { 32, lin, st, 22050 };
		char name[64];
		double off = time_churn(&sc, bytes, 0, 0);
		double on = time_churn(&sc, bytes, 256, 0);
		double over = time_churn(&sc, bytes, 256, 1);

		snprintf(name, sizeof(name), "32 voices %s %s",
		         st ? "stereo" : "mono  ", lin ? "linear " : "nearest");
		printf("%-34s %12.0f %12.0f %12.0f\n", name, off, on, over);
	}
	return bad;
}
