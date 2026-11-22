/*
 * Behavioural test for the voice engine (audio_voice.c): voice position
 * and loop math, command timing, rate rules, mixing and saturation. Every
 * check drives the production audio_voice_* functions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define HAVE_GUARD_PAGE 1
#endif

#include "audio_voice.h"
#include "zz9k/voice.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define PERIOD_FRAMES 960U
#define BANK_SIZE 4096U
#define VOL_FULL 65535U

static uint8_t g_bank[BANK_SIZE];
static struct audio_voice_engine g_e;
static int16_t g_out[2U * 4096U];

static void setup(uint32_t clock)
{
	memset(g_bank, 0, sizeof(g_bank));
	audio_voice_init(&g_e, g_bank, sizeof(g_bank), clock);
}

static struct audio_voice_cmd cmd(uint32_t stamp, uint8_t voice, uint8_t op,
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

/* The command builders take stamp 0; run_now() restamps a batch to the
 * frame about to be rendered, which is what most scenarios mean. */
static struct audio_voice_cmd c_start(uint8_t voice, uint16_t smp,
                                      uint32_t off, uint32_t len)
{
	return cmd(0, voice, AUDIO_VOICE_OP_START, smp, off, len);
}

static struct audio_voice_cmd c_loop(uint8_t voice, uint32_t ls, uint32_t ll)
{
	return cmd(0, voice, AUDIO_VOICE_OP_LOOP, 0, ls, ll);
}

static struct audio_voice_cmd c_period(uint8_t voice, uint32_t period)
{
	return cmd(0, voice, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE, period, 0);
}

static struct audio_voice_cmd c_hz(uint8_t voice, uint32_t hz)
{
	return cmd(0, voice, AUDIO_VOICE_OP_SET,
	           AUDIO_VOICE_SET_RATE | AUDIO_VOICE_RATE_HZ, hz, 0);
}

static struct audio_voice_cmd c_vol(uint8_t voice, uint32_t l, uint32_t r)
{
	return cmd(0, voice, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_VOLUME, 0,
	           (l << 16) | r);
}

static struct audio_voice_cmd c_stop(uint8_t voice)
{
	return cmd(0, voice, AUDIO_VOICE_OP_STOP, 0, 0, 0);
}

static struct audio_voice_cmd c_dir(uint8_t voice, int back)
{
	return cmd(0, voice, AUDIO_VOICE_OP_DIRECTION,
	           back ? AUDIO_VOICE_DIR_BACKWARD : 0, 0, 0);
}

static struct audio_voice_cmd c_master(uint32_t gain)
{
	return cmd(0, 0, AUDIO_VOICE_OP_MASTER, 0, gain, 0);
}

static size_t run(uint32_t frames, const struct audio_voice_cmd *cmds,
                  size_t n)
{
	return audio_voice_render(&g_e, g_out, frames, cmds, n);
}

static size_t run_now(uint32_t frames, struct audio_voice_cmd *cmds, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		cmds[i].stamp = g_e.frame;
	return run(frames, cmds, n);
}

static int check_pos(const char *what, uint32_t voice, int want_active,
                     uint32_t want_pos, uint32_t want_frac)
{
	int a;
	uint32_t p, f;
	int ok;

	a = audio_voice_position(&g_e, voice, &p, &f);
	ok = a == want_active && p == want_pos && f == want_frac;
	CHECK(ok, "%s: active %d pos %u frac %u, want %d %u %u", what, a, p, f,
	      want_active, want_pos, want_frac);
	return ok;
}

/* Set a full-volume voice at hz, start it and render `frames`. */
static void play(uint32_t voice, uint16_t smp, uint32_t off, uint32_t len,
                 uint32_t hz, uint32_t frames)
{
	struct audio_voice_cmd c[3];

	c[0] = c_hz((uint8_t)voice, hz);
	c[1] = c_vol((uint8_t)voice, VOL_FULL, VOL_FULL);
	CHECK(run_now(1, c, 2) == 2, "play setup consumed");
	c[0] = c_start((uint8_t)voice, smp, off, len);
	CHECK(run_now(frames, c, 1) == 1, "play start consumed");
}

/* The engine's per-voice scale: (s16 * vol) >> 16 at full volume. */
static int32_t scaled(int32_t s16)
{
	return (s16 * (int32_t)VOL_FULL) >> 16;
}

/* Period 428 PAL: hz = 3546895 / 428 = 8287, step = (8287 << 16) / 48000 =
 * 11314 (0x2C32). 960 frames advance 960 * 11314 = 10861440 = 165 * 65536 +
 * 48000. */
static void test_ramp_position(void)
{
	struct audio_voice_cmd c[3];
	uint32_t i;
	int ok = 1;

	setup(AUDIO_VOICE_CLOCK_PAL);
	for (i = 0; i < 256U; i++)
		g_bank[i] = (uint8_t)(int8_t)((int)i - 128);
	CHECK(audio_voice_sample_set(&g_e, 0, 0, 256, AUDIO_VOICE_FMT_S8) == 0,
	      "sample_set");
	c[0] = c_period(0, 428);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 0, 0);
	CHECK(run_now(PERIOD_FRAMES, c, 3) == 3, "ramp: commands consumed");
	check_pos("ramp after 960 frames", 0, 1, 165, 48000);
	for (i = 0; i < PERIOD_FRAMES; i++) {
		uint32_t pos = (uint32_t)(((uint64_t)i * 11314U) >> 16);
		int32_t want = scaled(((int32_t)pos - 128) * 256);

		if (g_out[2U * i] != want || g_out[2U * i + 1U] != want) {
			ok = 0;
			CHECK(0, "ramp frame %u: %d/%d want %d", i, g_out[2U * i],
			      g_out[2U * i + 1U], want);
			break;
		}
	}
	CHECK(ok, "ramp output follows the position");
	CHECK(g_e.frame == PERIOD_FRAMES, "frame counter %u", g_e.frame);
}

/* NTSC: hz = 3579545 / 428 = 8363, step = (8363 << 16) / 48000 = 11418
 * (0x2C9A); 960 * 11418 = 10961280 = 167 * 65536 + 16768. */
static void test_ntsc_clock(void)
{
	struct audio_voice_cmd c[3];

	setup(AUDIO_VOICE_CLOCK_NTSC);
	audio_voice_sample_set(&g_e, 0, 0, 256, AUDIO_VOICE_FMT_S8);
	c[0] = c_period(0, 428);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 0, 0);
	run_now(PERIOD_FRAMES, c, 3);
	check_pos("ntsc ramp after 960 frames", 0, 1, 167, 16768);
}

static void test_oneshot_stops(void)
{
	uint32_t i;

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 100, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10, AUDIO_VOICE_FMT_S8);
	/* 24000 Hz: half a sample per frame, 20 frames in the sample. */
	play(0, 0, 0, 0, 24000, 40);
	for (i = 0; i < 20U; i++)
		CHECK(g_out[2U * i] == scaled(100 * 256) &&
		      g_out[2U * i + 1U] == scaled(100 * 256),
		      "one-shot frame %u: %d", i, g_out[2U * i]);
	for (i = 20; i < 40U; i++)
		CHECK(g_out[2U * i] == 0 && g_out[2U * i + 1U] == 0,
		      "one-shot frame %u after the end: %d", i, g_out[2U * i]);
	check_pos("one-shot end", 0, 0, 10, 0);

	/* A first-pass length shorter than the sample ends the voice there. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 100, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10, AUDIO_VOICE_FMT_S8);
	play(0, 0, 2, 4, 48000, 8);
	CHECK(g_out[2 * 3] != 0 && g_out[2 * 4] == 0, "length-4 pass: frame 3 %d "
	      "frame 4 %d", g_out[2 * 3], g_out[2 * 4]);
	check_pos("length-4 pass end", 0, 0, 6, 0);

	/* A length past the sample end clamps to the sample end. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 100, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10, AUDIO_VOICE_FMT_S8);
	play(0, 0, 8, 100, 48000, 4);
	check_pos("clamped pass end", 0, 0, 10, 0);
}

static void test_loop_wrap(void)
{
	struct audio_voice_cmd c[2];

	/* 144000 Hz is exactly 3 samples per frame. Positions 0, 3, 6, 9;
	 * 12 is 2 past the end of 10, so it wraps to loopstart 2 + 2 = 4 and
	 * the loop end becomes 6. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 10, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 144000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	c[0] = c_start(0, 0, 0, 0);
	c[1] = c_loop(0, 2, 4);
	run_now(4, c, 2);
	check_pos("loop overshoot wraps to loopstart + over", 0, 1, 4, 0);
	/* 4 -> 7 is 1 past the new end 6: 2 + 1 = 3. */
	run(1, NULL, 0);
	check_pos("second wrap uses the loop end", 0, 1, 3, 0);

	/* Overshoot of 2 against a loop of length 1: over >= looplen restarts
	 * at the loop start. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 10, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 144000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	c[0] = c_start(0, 0, 0, 0);
	c[1] = c_loop(0, 2, 1);
	run_now(4, c, 2);
	check_pos("overshoot beyond the loop restarts at loopstart", 0, 1, 2, 0);
}

static void test_loop_after_first_pass(void)
{
	struct audio_voice_cmd c[2];
	uint32_t i;

	/* The first pass runs offset 0 with length 6 even though the loop
	 * starts earlier; the loop applies once the pass ends. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	for (i = 0; i < 10U; i++)
		g_bank[i] = (uint8_t)(i + 1U);
	audio_voice_sample_set(&g_e, 0, 0, 10, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	c[0] = c_start(0, 0, 0, 6);
	c[1] = c_loop(0, 3, 3);
	run_now(10, c, 2);
	for (i = 0; i < 10U; i++) {
		/* pass 0..5, then the loop 3..5 repeating */
		uint32_t idx = i < 6U ? i : 3U + ((i - 6U) % 3U);

		CHECK(g_out[2U * i] == scaled((int32_t)(idx + 1U) * 256),
		      "first pass then loop, frame %u: %d want index %u", i,
		      g_out[2U * i], idx);
	}
}

static void test_backward(void)
{
	struct audio_voice_cmd c[5];

	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 20, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 10, 0);
	c[3] = c_loop(0, 4, 6);
	c[4] = c_dir(0, 1);
	run_now(1, c, 5);
	check_pos("backward 1 frame", 0, 1, 9, 0);
	run(5, NULL, 0);
	check_pos("backward reaches the loop start", 0, 1, 4, 0);
	/* 3 is below loopstart 4: under 1, end 10, pos = 10 - 1 - 1 = 8. */
	run(1, NULL, 0);
	check_pos("backward wraps at the loop start", 0, 1, 8, 0);
	/* 7, 6, 5, 4 then 3 wraps to 8 again. */
	run(4, NULL, 0);
	check_pos("backward before wrap", 0, 1, 4, 0);
	run(1, NULL, 0);
	check_pos("backward wraps again", 0, 1, 8, 0);

	/* Half-step fraction: pos 5 frac 0 -> frac 0x8000 with a borrow. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 20, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 24000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 5, 0);
	c[3] = c_dir(0, 1);
	run_now(1, c, 4);
	check_pos("backward half step borrows", 0, 1, 4, 0x8000);
	run(1, NULL, 0);
	check_pos("backward second half step", 0, 1, 4, 0);

	/* An under of a whole loop or more collapses to 0: pos = end - 1. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 20, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 144000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 5, 0);
	c[3] = c_loop(0, 4, 2);
	c[4] = c_dir(0, 1);
	run_now(1, c, 5);
	/* 5 - 3 = 2: under 2 >= looplen 2 -> 0; end 6; pos = 6 - 0 - 1 = 5. */
	check_pos("backward under beyond the loop", 0, 1, 5, 0);
}

static void test_backward_oneshot(void)
{
	struct audio_voice_cmd c[5];

	/* Without a loop the loop start is the floor: the voice stops once
	 * it passes below it. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 20);
	audio_voice_sample_set(&g_e, 0, 0, 20, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 6, 0);
	c[3] = c_loop(0, 3, 0);
	c[4] = c_dir(0, 1);
	run_now(8, c, 5);
	/* Frames play positions 6, 5, 4, 3; the step to 2 stops the voice. */
	CHECK(g_out[2 * 3] != 0 && g_out[2 * 4] == 0, "backward one-shot "
	      "frames 3/4: %d %d", g_out[2 * 3], g_out[2 * 4]);
	check_pos("backward one-shot stopped", 0, 0, 2, 0);

	/* Stepping below index 0 stops a backward one-shot as well. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 20);
	audio_voice_sample_set(&g_e, 0, 0, 20, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 1, 0);
	c[3] = c_dir(0, 1);
	run_now(6, c, 4);
	CHECK(g_out[2 * 1] != 0 && g_out[2 * 2] == 0, "backward to zero "
	      "frames 1/2: %d %d", g_out[2 * 1], g_out[2 * 2]);
	check_pos("backward below index 0 stops", 0, 0, 0, 0);
}

static void test_direction(void)
{
	struct audio_voice_cmd c[3];

	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 20, AUDIO_VOICE_FMT_S8);
	/* On a silent voice the command does nothing. */
	c[0] = c_dir(0, 1);
	run_now(1, c, 1);
	CHECK(g_e.voice[0].backward == 0, "direction on a silent voice");
	CHECK(g_e.rejected == 0, "direction on a silent voice is not rejected");
	/* On a running voice it flips direction without a restart. */
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 10, 0);
	run_now(2, c, 3);
	check_pos("forward before the flip", 0, 1, 12, 0);
	c[0] = c_dir(0, 1);
	run_now(2, c, 1);
	check_pos("backward after the flip", 0, 1, 10, 0);
	c[0] = c_dir(0, 0);
	run_now(2, c, 1);
	check_pos("forward after the flip back", 0, 1, 12, 0);
	/* START always plays forward. */
	c[0] = c_dir(0, 1);
	c[1] = c_start(0, 0, 5, 0);
	run_now(1, c, 2);
	check_pos("start resets the direction", 0, 1, 6, 0);
}

static void test_rejected(void)
{
	struct audio_voice_cmd c[3];

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 16);
	audio_voice_sample_set(&g_e, 0, 0, 16, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[2] = c_start(0, 0, 16, 0);
	run_now(4, c, 3);
	check_pos("offset at the length", 0, 0, 0, 0);
	CHECK(g_e.rejected == 1, "rejected %u", g_e.rejected);
	CHECK(g_out[0] == 0, "rejected start is silent");

	/* An undefined sample id is rejected too, and silences the voice. */
	c[0] = c_start(0, 0, 0, 0);
	run_now(1, c, 1);
	check_pos("playing", 0, 1, 1, 0);
	c[0] = c_start(0, 7, 0, 0);
	run_now(1, c, 1);
	check_pos("undefined sample", 0, 0, 1, 0);
	CHECK(g_e.rejected == 2, "rejected %u", g_e.rejected);

	/* Sample ids past the table, voices past 32 and unknown ops. */
	c[0] = c_start(0, 0xFFFF, 0, 0);
	c[1] = c_start(32, 0, 0, 0);
	c[2] = cmd(0, 0, 99, 0, 0, 0);
	run_now(1, c, 3);
	CHECK(g_e.rejected == 5, "rejected %u", g_e.rejected);

	/* A loop outside the sample is refused and the old loop stays. */
	c[0] = c_start(0, 0, 0, 0);
	c[1] = c_loop(0, 4, 8);
	run_now(1, c, 2);
	CHECK(g_e.voice[0].loopstart == 4 && g_e.voice[0].looplen == 8,
	      "loop set");
	c[0] = c_loop(0, 12, 8);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 6, "rejected %u", g_e.rejected);
	CHECK(g_e.voice[0].loopstart == 4 && g_e.voice[0].looplen == 8,
	      "refused loop leaves the old loop");

	/* Sample definitions outside the bank are refused. */
	CHECK(audio_voice_sample_set(&g_e, 1, BANK_SIZE - 4, 8,
	                             AUDIO_VOICE_FMT_S8) == -1, "past bank");
	CHECK(audio_voice_sample_set(&g_e, AUDIO_VOICE_MAX_SAMPLES, 0, 4,
	                             AUDIO_VOICE_FMT_S8) == -1, "id range");
	CHECK(audio_voice_sample_set(&g_e, 255, 0, 4, AUDIO_VOICE_FMT_S8) == 0,
	      "id 255");
}

static void test_rates(void)
{
	struct audio_voice_cmd c[2];

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 1024);
	audio_voice_sample_set(&g_e, 0, 0, 1024, AUDIO_VOICE_FMT_S8);

	/* A voice that never got a rate is silent and does not advance. */
	c[0] = c_vol(0, VOL_FULL, VOL_FULL);
	c[1] = c_start(0, 0, 0, 0);
	run_now(4, c, 2);
	CHECK(g_out[0] == 0 && g_out[6] == 0, "no rate is silent");
	check_pos("no rate holds position", 0, 1, 0, 0);

	/* Period 4 is ignored, so the voice stays without a rate. */
	c[0] = c_period(0, 4);
	run_now(4, c, 1);
	CHECK(g_out[0] == 0, "period 4 is silent");
	check_pos("period 4 holds position", 0, 1, 0, 0);

	/* Period 5 gives 709379 Hz, capped at 192000 Hz: step 4.0. */
	c[0] = c_period(0, 5);
	run_now(1, c, 1);
	check_pos("period 5 is capped", 0, 1, 4, 0);

	/* A later period below 5 leaves the previous step in place. */
	c[0] = c_period(0, 4);
	run_now(1, c, 1);
	check_pos("period 4 keeps the step", 0, 1, 8, 0);

	/* Hz input is capped the same way. */
	c[0] = c_hz(0, 1000000);
	run_now(1, c, 1);
	check_pos("hz cap", 0, 1, 12, 0);
	c[0] = c_hz(0, 192000);
	run_now(1, c, 1);
	check_pos("192000 hz is step 4", 0, 1, 16, 0);
	c[0] = c_hz(0, 0);
	run_now(1, c, 1);
	check_pos("0 hz keeps the step", 0, 1, 20, 0);
	c[0] = c_hz(0, 24000);
	run_now(2, c, 1);
	check_pos("24000 hz is step 0.5", 0, 1, 21, 0);
}

static void test_impulse_timing(void)
{
	struct audio_voice_cmd c[4];
	uint32_t i, base;

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 256);
	audio_voice_sample_set(&g_e, 0, 0, 256, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	base = g_e.frame;
	c[0] = c_start(0, 0, 0, 0);
	c[0].stamp = base + 500U;
	c[1] = c_stop(0);
	c[1].stamp = base + 501U;
	CHECK(run(PERIOD_FRAMES, c, 2) == 2, "impulse commands consumed");
	for (i = 0; i < PERIOD_FRAMES; i++) {
		int want = i == 500U ? scaled(64 * 256) : 0;

		if (g_out[2U * i] != want || g_out[2U * i + 1U] != want) {
			CHECK(0, "impulse frame %u: %d/%d want %d", i, g_out[2U * i],
			      g_out[2U * i + 1U], want);
			break;
		}
	}

	/* Two commands one frame apart: voice 1 joins on the next frame. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 256);
	audio_voice_sample_set(&g_e, 0, 0, 256, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_hz(1, 48000);
	c[2] = c_vol(0, VOL_FULL, VOL_FULL);
	c[3] = c_vol(1, VOL_FULL, VOL_FULL);
	run_now(1, c, 4);
	base = g_e.frame;
	c[0] = c_start(0, 0, 0, 0);
	c[0].stamp = base + 10U;
	c[1] = c_start(1, 0, 0, 0);
	c[1].stamp = base + 11U;
	CHECK(run(16, c, 2) == 2, "two-start commands consumed");
	CHECK(g_out[2 * 9] == 0, "before the first start: %d", g_out[2 * 9]);
	CHECK(g_out[2 * 10] == scaled(64 * 256), "first start frame: %d",
	      g_out[2 * 10]);
	CHECK(g_out[2 * 11] == 2 * scaled(64 * 256), "second start frame: %d",
	      g_out[2 * 11]);
}

static void test_late_and_horizon(void)
{
	struct audio_voice_cmd c[3];
	uint32_t i;

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 256);
	audio_voice_sample_set(&g_e, 0, 0, 256, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(100, c, 2);
	CHECK(g_e.frame == 100, "frame %u", g_e.frame);
	CHECK(g_e.late == 0, "no late before the test: %u", g_e.late);

	/* Stamp 50 was rendered long ago: it applies at frame 0 of this
	 * call. Stamp 110 lands inside it; 1060 is the next period's first
	 * frame and stays queued. */
	c[0] = c_start(0, 0, 0, 0);
	c[0].stamp = 50;
	c[1] = c_stop(0);
	c[1].stamp = 110;
	c[2] = c_start(0, 0, 0, 0);
	c[2].stamp = 100U + PERIOD_FRAMES;
	CHECK(run(PERIOD_FRAMES, c, 3) == 2, "consumed count");
	CHECK(g_e.late == 1, "late %u", g_e.late);
	for (i = 0; i < PERIOD_FRAMES; i++) {
		int want = i < 10U ? scaled(64 * 256) : 0;

		if (g_out[2U * i] != want) {
			CHECK(0, "late frame %u: %d want %d", i, g_out[2U * i], want);
			break;
		}
	}
	/* The unconsumed command applies on its exact frame next call. */
	CHECK(run(4, &c[2], 1) == 1, "queued command consumed");
	CHECK(g_e.late == 1, "late stays %u", g_e.late);
	CHECK(g_out[0] == scaled(64 * 256), "queued command at frame 0: %d",
	      g_out[0]);
}

static void test_saturation(void)
{
	struct audio_voice_cmd c[2];
	struct audio_voice_cmd starts[AUDIO_VOICE_MAX_VOICES];
	uint32_t v;

	setup(AUDIO_VOICE_CLOCK_PAL);
	/* Sample 0: +full scale, sample 1: -full scale, big-endian 16 bit. */
	g_bank[0] = 0x7F;
	g_bank[1] = 0xFF;
	g_bank[2] = 0x7F;
	g_bank[3] = 0xFF;
	g_bank[4] = 0x80;
	g_bank[5] = 0x00;
	g_bank[6] = 0x80;
	g_bank[7] = 0x00;
	audio_voice_sample_set(&g_e, 0, 0, 4, AUDIO_VOICE_FMT_S16);
	audio_voice_sample_set(&g_e, 1, 4, 4, AUDIO_VOICE_FMT_S16);
	for (v = 0; v < AUDIO_VOICE_MAX_VOICES; v++) {
		c[0] = c_hz((uint8_t)v, 24000);
		c[1] = c_vol((uint8_t)v, VOL_FULL, VOL_FULL);
		run_now(1, c, 2);
	}
	for (v = 0; v < AUDIO_VOICE_MAX_VOICES; v++)
		starts[v] = c_start((uint8_t)v, 0, 0, 0);
	run_now(2, starts, AUDIO_VOICE_MAX_VOICES);
	CHECK(g_out[0] == 32767 && g_out[1] == 32767, "positive saturation "
	      "%d/%d", g_out[0], g_out[1]);
	CHECK(g_out[2] == 32767, "positive saturation holds %d", g_out[2]);

	for (v = 0; v < AUDIO_VOICE_MAX_VOICES; v++)
		starts[v] = c_start((uint8_t)v, 1, 0, 0);
	run_now(2, starts, AUDIO_VOICE_MAX_VOICES);
	CHECK(g_out[0] == -32768 && g_out[1] == -32768, "negative saturation "
	      "%d/%d", g_out[0], g_out[1]);

	/* 16-bit big-endian read: a single voice scales by the volume. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	g_bank[0] = 0x12;
	g_bank[1] = 0x34;
	audio_voice_sample_set(&g_e, 0, 0, 2, AUDIO_VOICE_FMT_S16);
	play(0, 0, 0, 0, 48000, 1);
	CHECK(g_out[0] == scaled(0x1234), "s16 read %d want %d", g_out[0],
	      scaled(0x1234));
}

static void test_volume_and_master(void)
{
	struct audio_voice_cmd c[3];

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 16);
	audio_voice_sample_set(&g_e, 0, 0, 16, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, 0x8000, 0x4000);
	c[2] = c_start(0, 0, 0, 0);
	run_now(1, c, 3);
	CHECK(g_out[0] == (16384 * 0x8000) >> 16, "left volume %d", g_out[0]);
	CHECK(g_out[1] == (16384 * 0x4000) >> 16, "right volume %d", g_out[1]);

	/* Master gain is Q8.8: 128 halves, 512 doubles. */
	c[0] = c_master(128);
	run_now(1, c, 1);
	CHECK(g_out[0] == ((16384 * 0x8000 >> 16) * 128) >> 8,
	      "master 0.5: %d", g_out[0]);
	c[0] = c_master(512);
	run_now(1, c, 1);
	CHECK(g_out[0] == ((16384 * 0x8000 >> 16) * 512) >> 8,
	      "master 2.0: %d", g_out[0]);
	/* Silenced voices leave no residue. */
	audio_voice_silence(&g_e);
	run(1, NULL, 0);
	CHECK(g_out[0] == 0 && g_out[1] == 0, "silence");
}

static void test_interpolation(void)
{
	struct audio_voice_cmd c[3];

	/* Sample {0, 64, 0, ...} at half a sample per frame. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	g_bank[1] = 64;
	audio_voice_sample_set(&g_e, 0, 0, 8, AUDIO_VOICE_FMT_S8);
	play(0, 0, 0, 0, 24000, 4);
	/* Nearest repeats the sample at the integer position. */
	CHECK(g_out[2 * 0] == 0 && g_out[2 * 1] == 0, "nearest frames 0/1: "
	      "%d %d", g_out[0], g_out[2]);
	CHECK(g_out[2 * 2] == scaled(64 * 256) && g_out[2 * 3] == scaled(64 * 256),
	      "nearest frames 2/3: %d %d", g_out[4], g_out[6]);

	setup(AUDIO_VOICE_CLOCK_PAL);
	g_bank[1] = 64;
	audio_voice_sample_set(&g_e, 0, 0, 8, AUDIO_VOICE_FMT_S8);
	g_e.interp = AUDIO_VOICE_INTERP_LINEAR;
	play(0, 0, 0, 0, 24000, 4);
	CHECK(g_out[0] == 0, "linear frame 0: %d", g_out[0]);
	CHECK(g_out[2] == scaled(8192), "linear half step: %d want %d", g_out[2],
	      scaled(8192));
	CHECK(g_out[4] == scaled(16384), "linear frame 2: %d", g_out[4]);
	/* Half way back down to 0 between index 1 and 2. */
	CHECK(g_out[6] == scaled(8192), "linear falling half step: %d", g_out[6]);

	/* At the last sample the neighbour is the sample itself. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 4);
	audio_voice_sample_set(&g_e, 0, 0, 4, AUDIO_VOICE_FMT_S8);
	g_e.interp = AUDIO_VOICE_INTERP_LINEAR;
	play(0, 0, 3, 0, 24000, 2);
	CHECK(g_out[0] == scaled(16384) && g_out[2] == scaled(16384),
	      "linear at the sample end: %d %d", g_out[0], g_out[2]);

	/* Backward linear interpolates toward index - 1: at index 1 plus
	 * a half fraction, between sample 1 (0) and sample 0 (64). */
	setup(AUDIO_VOICE_CLOCK_PAL);
	g_bank[0] = 64;
	audio_voice_sample_set(&g_e, 0, 0, 8, AUDIO_VOICE_FMT_S8);
	g_e.interp = AUDIO_VOICE_INTERP_LINEAR;
	c[0] = c_hz(0, 24000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	c[0] = c_start(0, 0, 2, 0);
	c[1] = c_dir(0, 1);
	run_now(2, c, 2);
	CHECK(g_out[0] == 0, "backward linear start %d", g_out[0]);
	CHECK(g_out[2] == scaled(8192), "backward linear at 0.5: %d", g_out[2]);
}

/* The neighbour a linear read blends toward follows the loop: past the
 * pass end it is the loop start going forward and the loop end going
 * backward, not the sample itself. */
static void test_interpolation_loop(void)
{
	struct audio_voice_cmd c[3];

	/* Forward: {0, 64} looping over both samples, half a sample per
	 * frame. Frame 3 sits half way between sample 1 and the loop start. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	g_bank[1] = 64;
	audio_voice_sample_set(&g_e, 0, 0, 2, AUDIO_VOICE_FMT_S8);
	g_e.interp = AUDIO_VOICE_INTERP_LINEAR;
	c[0] = c_hz(0, 24000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	c[0] = c_start(0, 0, 0, 0);
	c[1] = c_loop(0, 0, 2);
	run_now(6, c, 2);
	CHECK(g_out[0] == 0, "forward loop frame 0: %d", g_out[0]);
	CHECK(g_out[2] == scaled(8192), "forward loop frame 1: %d", g_out[2]);
	CHECK(g_out[4] == scaled(16384), "forward loop frame 2: %d", g_out[4]);
	CHECK(g_out[6] == scaled(8192),
	      "forward loop wrap frame: %d want %d", g_out[6], scaled(8192));
	CHECK(g_out[8] == 0, "forward loop frame 4: %d", g_out[8]);
	CHECK(g_out[10] == scaled(8192), "forward loop frame 5: %d", g_out[10]);

	/* Backward from sample 1: at index 0 plus a half fraction the
	 * neighbour is the loop end (sample 1). */
	setup(AUDIO_VOICE_CLOCK_PAL);
	g_bank[1] = 64;
	audio_voice_sample_set(&g_e, 0, 0, 2, AUDIO_VOICE_FMT_S8);
	g_e.interp = AUDIO_VOICE_INTERP_LINEAR;
	c[0] = c_hz(0, 24000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	c[0] = c_start(0, 0, 1, 0);
	c[1] = c_loop(0, 0, 2);
	c[2] = c_dir(0, 1);
	run_now(4, c, 3);
	CHECK(g_out[0] == scaled(16384), "backward loop frame 0: %d", g_out[0]);
	CHECK(g_out[2] == scaled(8192),
	      "backward loop wrap frame: %d want %d", g_out[2], scaled(8192));
	CHECK(g_out[4] == 0, "backward loop frame 2: %d", g_out[4]);
	CHECK(g_out[6] == scaled(16384), "backward loop frame 3: %d", g_out[6]);
}

static void test_stamp_wrap(void)
{
	struct audio_voice_cmd c[3];
	uint32_t i;

	setup(AUDIO_VOICE_CLOCK_PAL);
	memset(g_bank, 64, 256);
	audio_voice_sample_set(&g_e, 0, 0, 256, AUDIO_VOICE_FMT_S8);
	c[0] = c_hz(0, 48000);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, 2);
	g_e.frame = 0xFFFFFFF0U;
	/* Frame 10 of the call is stamp 0xFFFFFFFA; frame 18 is stamp 2; stamp
	 * 0x20 is frame 48, past the 32 rendered. */
	c[0] = c_start(0, 0, 0, 0);
	c[0].stamp = 0xFFFFFFFAU;
	c[1] = c_stop(0);
	c[1].stamp = 2U;
	c[2] = c_start(0, 0, 0, 0);
	c[2].stamp = 0x20U;
	CHECK(run(32, c, 3) == 2, "wrap consumed count");
	for (i = 0; i < 32U; i++) {
		int want = (i >= 10U && i < 18U) ? scaled(64 * 256) : 0;

		if (g_out[2U * i] != want) {
			CHECK(0, "wrap frame %u: %d want %d", i, g_out[2U * i], want);
			break;
		}
	}
	CHECK(g_e.frame == 16U, "frame counter wrapped to %u", g_e.frame);
	CHECK(g_e.late == 0, "no late command across the wrap: %u", g_e.late);
	/* The leftover applies on its frame in the next call (stamp 0x20 is
	 * frame 16 of a call starting at 16). */
	CHECK(run(32, &c[2], 1) == 1, "wrapped leftover consumed");
	CHECK(g_out[2 * 15] == 0 && g_out[2 * 16] == scaled(64 * 256),
	      "leftover frame: %d %d", g_out[2 * 15], g_out[2 * 16]);
}

/* ---- mono output golden ----
 *
 * A fixed script over mono S8 and S16 samples (loops, both directions, rate
 * and volume changes, master, a stop and a restart), hashed over the
 * rendered output. The expected hashes were taken from the engine before the
 * stereo formats existed, so a change to the mono path shows up here. */

static uint32_t fnv_i16(uint32_t h, const int16_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= (uint32_t)((uint16_t)p[i] & 0xFFU);
		h *= 16777619U;
		h ^= (uint32_t)(((uint16_t)p[i] >> 8) & 0xFFU);
		h *= 16777619U;
	}
	return h;
}

static struct audio_voice_cmd at(uint32_t stamp, struct audio_voice_cmd c)
{
	c.stamp = stamp;
	return c;
}

static uint32_t golden_mono(uint32_t interp)
{
	struct audio_voice_cmd s[40];
	size_t n = 0, used = 0;
	uint32_t x = 12345U;
	uint32_t h = 2166136261U;
	uint32_t i;

	setup(AUDIO_VOICE_CLOCK_PAL);
	for (i = 0; i < 3072U; i++) {
		x = x * 1664525U + 1013904223U;
		g_bank[i] = (uint8_t)(x >> 24);
	}
	audio_voice_sample_set(&g_e, 0, 0, 1024, AUDIO_VOICE_FMT_S8);
	audio_voice_sample_set(&g_e, 1, 1024, 2048, AUDIO_VOICE_FMT_S16);
	g_e.interp = interp;

	s[n++] = at(0, c_hz(0, 22050));
	s[n++] = at(0, c_vol(0, 40000, 20000));
	s[n++] = at(0, c_start(0, 0, 0, 0));
	s[n++] = at(0, c_loop(0, 100, 300));
	s[n++] = at(0, c_hz(1, 8363));
	s[n++] = at(0, c_vol(1, 65535, 0));
	s[n++] = at(0, c_start(1, 1, 0, 0));
	s[n++] = at(0, c_loop(1, 20, 500));
	s[n++] = at(0, c_hz(2, 31000));
	s[n++] = at(0, c_vol(2, 1000, 65535));
	s[n++] = at(0, c_start(2, 0, 900, 0));
	s[n++] = at(0, c_loop(2, 50, 250));
	s[n++] = at(0, c_dir(2, 1));
	s[n++] = at(0, c_hz(3, 96000));
	s[n++] = at(0, c_vol(3, 30000, 30000));
	s[n++] = at(0, c_start(3, 1, 100, 1500));
	s[n++] = at(500, c_master(512));
	s[n++] = at(700, c_hz(0, 11025));
	s[n++] = at(900, c_hz(2, 44100));
	s[n++] = at(1200, c_vol(1, 0, 65535));
	s[n++] = at(1500, c_stop(3));
	s[n++] = at(1600, c_start(3, 1, 0, 0));
	s[n++] = at(2000, c_dir(1, 1));
	s[n++] = at(2500, c_loop(0, 0, 0));
	s[n++] = at(3000, c_master(256));
	for (i = 0; i < 5U; i++) {
		used += audio_voice_render(&g_e, g_out, 960, s + used, n - used);
		h = fnv_i16(h, g_out, 1920);
	}
	CHECK(used == n, "golden: all %zu commands consumed, got %zu", n, used);
	return h;
}

static void test_mono_golden(void)
{
	uint32_t nearest = golden_mono(AUDIO_VOICE_INTERP_NEAREST);
	uint32_t linear = golden_mono(AUDIO_VOICE_INTERP_LINEAR);

	CHECK(nearest == 0xDA17EFBDU, "golden nearest hash %08x", nearest);
	CHECK(linear == 0x9342318EU, "golden linear hash %08x", linear);
}

/* ---- stereo sample formats ----
 *
 * S8S and S16S hold interleaved frames, left sample then right sample. A
 * frame is 2 or 4 bytes, and every byte quantity of START and LOOP is
 * divided into frames, rounding down. The left volume scales the left
 * sample and the right volume the right sample. */

static uint32_t fshift(uint32_t fmt)
{
	if (fmt == AUDIO_VOICE_FMT_S16S)
		return 2U;
	return (fmt == AUDIO_VOICE_FMT_S16 || fmt == AUDIO_VOICE_FMT_S8S) ?
	       1U : 0U;
}

static uint32_t fbytes(uint32_t fmt)
{
	return 1U << fshift(fmt);
}

static int is_wide(uint32_t fmt)
{
	return fmt == AUDIO_VOICE_FMT_S16 || fmt == AUDIO_VOICE_FMT_S16S;
}

/* Raw 8 or 16-bit values of frame i of the test pattern. */
static int32_t raw_l(uint32_t fmt, uint32_t i)
{
	return is_wide(fmt) ? 1000 + 700 * (int32_t)i : 10 + 3 * (int32_t)i;
}

static int32_t raw_r(uint32_t fmt, uint32_t i)
{
	return is_wide(fmt) ? -(500 + 300 * (int32_t)i) : -(5 + 2 * (int32_t)i);
}

/* What the engine reads from a raw value: 8-bit data is promoted by 8 bits. */
static int32_t eng(uint32_t fmt, int32_t raw)
{
	return is_wide(fmt) ? raw : raw * 256;
}

static int32_t vscale(int32_t s, uint32_t vol)
{
	return (s * (int32_t)vol) >> 16;
}

static void put_raw(uint32_t fmt, uint8_t *p, int32_t raw)
{
	if (is_wide(fmt)) {
		p[0] = (uint8_t)((uint32_t)raw >> 8);
		p[1] = (uint8_t)raw;
	} else {
		p[0] = (uint8_t)raw;
	}
}

/* Write frames 0 .. n - 1 of the pattern at byte offset base of bank. */
static void put_pattern(uint8_t *bank, uint32_t fmt, uint32_t base,
                        uint32_t n)
{
	uint32_t w = is_wide(fmt) ? 2U : 1U;
	uint32_t i;

	for (i = 0; i < n; i++) {
		uint8_t *p = bank + base + i * fbytes(fmt);

		put_raw(fmt, p, raw_l(fmt, i));
		put_raw(fmt, p + w, raw_r(fmt, i));
	}
}

/* Give voice 0 a rate of 48 kHz and the volumes, then start sample 0 and
 * render `frames` frames. */
static void stereo_start(uint32_t vl, uint32_t vr, uint32_t off, uint32_t len,
                         int have_loop, uint32_t ls, uint32_t ll, int back,
                         uint32_t frames)
{
	struct audio_voice_cmd c[5];
	size_t n = 0;

	c[n++] = c_hz(0, 48000);
	c[n++] = c_vol(0, vl, vr);
	CHECK(run_now(1, c, n) == n, "stereo setup consumed");
	n = 0;
	c[n++] = c_start(0, 0, off, len);
	if (have_loop)
		c[n++] = c_loop(0, ls, ll);
	if (back)
		c[n++] = c_dir(0, 1);
	CHECK(run_now(frames, c, n) == n, "stereo start consumed");
}

/* Output frame f must be pattern frame seq[f] of fmt at the volumes, or
 * silence for a negative index. */
static void check_seq(const char *what, uint32_t fmt, const int32_t *seq,
                      uint32_t n, uint32_t vl, uint32_t vr)
{
	uint32_t f;

	for (f = 0; f < n; f++) {
		int32_t wl = 0;
		int32_t wr = 0;

		if (seq[f] >= 0) {
			wl = vscale(eng(fmt, raw_l(fmt, (uint32_t)seq[f])), vl);
			wr = vscale(eng(fmt, raw_r(fmt, (uint32_t)seq[f])), vr);
		}
		if (g_out[2U * f] != wl || g_out[2U * f + 1U] != wr) {
			CHECK(0, "%s: format %u frame %u: %d/%d, want %d/%d "
			      "(pattern frame %d)", what, fmt, f, g_out[2U * f],
			      g_out[2U * f + 1U], wl, wr, seq[f]);
			return;
		}
	}
}

static void stereo_sides(uint32_t fmt)
{
	static const uint32_t vols[][2] = {
		{VOL_FULL, 0}, {0, VOL_FULL}, {VOL_FULL, VOL_FULL},
		{30000, 50000}, {0, 0}
	};
	static const int32_t seq[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	size_t i;

	for (i = 0; i < sizeof(vols) / sizeof(vols[0]); i++) {
		setup(AUDIO_VOICE_CLOCK_PAL);
		put_pattern(g_bank, fmt, 0, 8);
		audio_voice_sample_set(&g_e, 0, 0, 8U * fbytes(fmt), fmt);
		stereo_start(vols[i][0], vols[i][1], 0, 0, 0, 0, 0, 0, 8);
		check_seq("sides", fmt, seq, 8, vols[i][0], vols[i][1]);
		check_pos("sides end", 0, 0, 8, 0);
		if (vols[i][0] == VOL_FULL && vols[i][1] == 0) {
			CHECK(g_out[0] != 0 && g_out[1] == 0 && g_out[15] == 0,
			      "left only, format %u: %d/%d", fmt, g_out[0],
			      g_out[1]);
		}
		if (vols[i][0] == 0 && vols[i][1] == VOL_FULL) {
			CHECK(g_out[0] == 0 && g_out[1] != 0 && g_out[14] == 0,
			      "right only, format %u: %d/%d", fmt, g_out[0],
			      g_out[1]);
		}
	}
}

static void test_stereo_sides(void)
{
	stereo_sides(AUDIO_VOICE_FMT_S8S);
	stereo_sides(AUDIO_VOICE_FMT_S16S);
}

/* Loops, first-pass length and backward play count frames. `j` is the
 * largest rounding slack inside a frame, added to every byte quantity: a
 * value that is not a multiple of the frame size rounds down. */
static void stereo_frames(uint32_t fmt)
{
	static const int32_t fwd_loop[18] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 2, 3, 4, 5, 2, 3, 4, 5
	};
	static const int32_t first_pass[15] = {
		0, 1, 2, 2, 3, 4, 5, 2, 3, 4, 5, 2, 3, 4, 5
	};
	static const int32_t unaligned[8] = {3, 4, 1, 2, 1, 2, 1, 2};
	static const int32_t back_loop[11] = {6, 5, 4, 3, 2, 4, 3, 2, 4, 3, 2};
	static const int32_t back_once[7] = {3, 2, 1, 0, -1, -1, -1};
	uint32_t fb = fbytes(fmt);
	uint32_t j = fb - 1U;

	/* Forward, to the sample end, then the loop [2, 6). */
	setup(AUDIO_VOICE_CLOCK_PAL);
	put_pattern(g_bank, fmt, 0, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10U * fb + j, fmt);
	stereo_start(VOL_FULL, VOL_FULL, 0, 0, 1, 2U * fb, 4U * fb, 0, 18);
	check_seq("forward loop", fmt, fwd_loop, 18, VOL_FULL, VOL_FULL);

	/* A first pass of 3 frames, then the loop. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	put_pattern(g_bank, fmt, 0, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10U * fb, fmt);
	stereo_start(VOL_FULL, VOL_FULL, 0, 3U * fb, 1, 2U * fb, 4U * fb, 0, 15);
	check_seq("first pass length", fmt, first_pass, 15, VOL_FULL, VOL_FULL);

	/* Offsets inside a frame round down: start at frame 3, a first pass of
	 * 2 frames, then the loop [1, 3). */
	setup(AUDIO_VOICE_CLOCK_PAL);
	put_pattern(g_bank, fmt, 0, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10U * fb + j, fmt);
	stereo_start(VOL_FULL, VOL_FULL, 3U * fb + j, 2U * fb + j, 1, fb + j,
	             2U * fb + j, 0, 1);
	check_pos("unaligned start rounds down", 0, 1, 4, 0);
	setup(AUDIO_VOICE_CLOCK_PAL);
	put_pattern(g_bank, fmt, 0, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10U * fb + j, fmt);
	stereo_start(VOL_FULL, VOL_FULL, 3U * fb + j, 2U * fb + j, 1, fb + j,
	             2U * fb + j, 0, 8);
	check_seq("unaligned offsets", fmt, unaligned, 8, VOL_FULL, VOL_FULL);

	/* Backward with the loop [2, 6): wraps from below 2 to 6 - 1 - 1. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	put_pattern(g_bank, fmt, 0, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10U * fb, fmt);
	stereo_start(VOL_FULL, VOL_FULL, 6U * fb + j, 0, 1, 2U * fb + j,
	             4U * fb + j, 1, 11);
	check_seq("backward loop", fmt, back_loop, 11, VOL_FULL, VOL_FULL);

	/* Backward without a loop stops after frame 0. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	put_pattern(g_bank, fmt, 0, 10);
	audio_voice_sample_set(&g_e, 0, 0, 10U * fb, fmt);
	stereo_start(VOL_FULL, VOL_FULL, 3U * fb + j, 0, 0, 0, 0, 1, 7);
	check_seq("backward once", fmt, back_once, 7, VOL_FULL, VOL_FULL);
	check_pos("backward once stops", 0, 0, 0, 0);
}

static void test_stereo_frames(void)
{
	stereo_frames(AUDIO_VOICE_FMT_S8S);
	stereo_frames(AUDIO_VOICE_FMT_S16S);
}

/* Linear interpolation on a stereo sample is the mono result per side: a
 * mono sample holding the left channel, one holding the right channel and
 * the stereo sample play at the same position and must output the same
 * frames. Left-only stereo against the left mono sample, right-only against
 * the right one. */
struct cmp_case {
	uint32_t hz;
	int back;
	uint32_t start;
	int loop;
	uint32_t ls;
	uint32_t ll;
};

#define CMP_FRAMES 16U
#define CMP_RENDER 64U
#define CMP_MONO_L 0U
#define CMP_MONO_R 256U
#define CMP_STEREO 512U

static void fill_cmp_bank(uint32_t sfmt)
{
	uint32_t mono = is_wide(sfmt) ? AUDIO_VOICE_FMT_S16 : AUDIO_VOICE_FMT_S8;
	uint32_t mb = fbytes(mono);
	uint32_t sb = fbytes(sfmt);
	uint32_t x = 777U;
	uint32_t i;

	for (i = 0; i < CMP_FRAMES; i++) {
		int32_t l;
		int32_t r;

		x = x * 1664525U + 1013904223U;
		l = is_wide(sfmt) ? (int32_t)(int16_t)(x >> 16) :
		    (int32_t)(int8_t)(x >> 24);
		x = x * 1664525U + 1013904223U;
		r = is_wide(sfmt) ? (int32_t)(int16_t)(x >> 16) :
		    (int32_t)(int8_t)(x >> 24);
		put_raw(mono, g_bank + CMP_MONO_L + i * mb, l);
		put_raw(mono, g_bank + CMP_MONO_R + i * mb, r);
		put_raw(sfmt, g_bank + CMP_STEREO + i * sb, l);
		put_raw(sfmt, g_bank + CMP_STEREO + i * sb + mb, r);
	}
}

static void render_cmp(int16_t *out, uint32_t interp, uint32_t sfmt,
                       uint32_t fmt, uint32_t base, uint32_t vl, uint32_t vr,
                       const struct cmp_case *cc)
{
	struct audio_voice_cmd c[3];
	uint32_t fb = fbytes(fmt);
	size_t n = 0;

	setup(AUDIO_VOICE_CLOCK_PAL);
	fill_cmp_bank(sfmt);
	audio_voice_sample_set(&g_e, 0, base, CMP_FRAMES * fb, fmt);
	g_e.interp = interp;
	c[0] = c_hz(0, cc->hz);
	c[1] = c_vol(0, vl, vr);
	run_now(1, c, 2);
	c[n++] = c_start(0, 0, cc->start * fb, 0);
	if (cc->loop)
		c[n++] = c_loop(0, cc->ls * fb, cc->ll * fb);
	if (cc->back)
		c[n++] = c_dir(0, 1);
	CHECK(run_now(CMP_RENDER, c, n) == n, "compare start consumed");
	memcpy(out, g_out, CMP_RENDER * 4U);
}

static void test_stereo_interpolation(void)
{
	static const struct cmp_case cases[] = {
		{24000, 0, 0, 0, 0, 0},
		{17000, 0, 0, 1, 3, 7},
		{30000, 1, 14, 1, 2, 10},
		{9000, 1, 15, 0, 0, 0},
		{96000, 0, 1, 1, 0, 16},
		{11111, 0, 5, 1, 15, 1},
		{19999, 1, 3, 1, 0, 4},
	};
	static const uint32_t sfmts[2] = {AUDIO_VOICE_FMT_S8S,
	                                  AUDIO_VOICE_FMT_S16S};
	int16_t a[2U * CMP_RENDER];
	int16_t b[2U * CMP_RENDER];
	size_t s, k, side;
	uint32_t interp;

	for (s = 0; s < 2; s++) {
		uint32_t sfmt = sfmts[s];
		uint32_t mono = is_wide(sfmt) ? AUDIO_VOICE_FMT_S16 :
		                AUDIO_VOICE_FMT_S8;

		for (interp = 0; interp < 2; interp++) {
			for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
				for (side = 0; side < 2; side++) {
					uint32_t vl = side == 0 ? VOL_FULL : 0U;
					uint32_t vr = side == 0 ? 0U : VOL_FULL;
					size_t f;
					int nonzero = 0;

					render_cmp(a, interp, sfmt, mono,
					           side == 0 ? CMP_MONO_L : CMP_MONO_R,
					           vl, vr, &cases[k]);
					render_cmp(b, interp, sfmt, sfmt,
					           CMP_STEREO, vl, vr, &cases[k]);
					CHECK(memcmp(a, b, sizeof(a)) == 0,
					      "stereo vs mono: format %u interp %u "
					      "case %zu side %zu", sfmt, interp, k,
					      side);
					for (f = 0; f < 2U * CMP_RENDER; f++) {
						if ((f & 1U) == (side == 0 ? 1U : 0U))
							CHECK(b[f] == 0, "other side silent");
						else if (b[f] != 0)
							nonzero = 1;
					}
					CHECK(nonzero, "case %zu plays something", k);
				}
			}
		}
	}
}

/* A stereo sample whose last frame ends at the bank end plays that frame
 * and stops without reading past the bank. With mmap the bank ends at an
 * inaccessible page, so a read past the end faults. */
static struct audio_voice_engine g_ge;

static uint8_t *guard_bank_alloc(uint32_t len, void **map, size_t *maplen)
{
#ifdef HAVE_GUARD_PAGE
	size_t ps = (size_t)sysconf(_SC_PAGESIZE);
	size_t body = ((len + ps - 1U) / ps) * ps;
	uint8_t *m = mmap(NULL, body + ps, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (m == MAP_FAILED)
		return NULL;
	if (mprotect(m + body, ps, PROT_NONE) != 0) {
		munmap(m, body + ps);
		return NULL;
	}
	*map = m;
	*maplen = body + ps;
	return m + body - len;
#else
	static uint8_t plain[256];

	(void)map;
	(void)maplen;
	return len <= sizeof(plain) ? plain : NULL;
#endif
}

static void guard_bank_free(void *map, size_t maplen)
{
#ifdef HAVE_GUARD_PAGE
	if (map)
		munmap(map, maplen);
#else
	(void)map;
	(void)maplen;
#endif
}

static void bank_end_case(uint32_t fmt, uint32_t len, uint32_t interp,
                          int back, int loop, uint32_t hz)
{
	struct audio_voice_cmd c[4];
	void *map = NULL;
	size_t maplen = 0;
	uint8_t *bank = guard_bank_alloc(len, &map, &maplen);
	uint32_t fb = fbytes(fmt);
	uint32_t frames = len >> fshift(fmt);
	uint32_t last = frames - 1U;
	size_t n = 0;
	size_t i;
	uint32_t f, idx, fr;
	int32_t wl;
	int32_t wr;

	if (!bank) {
		CHECK(0, "guard bank allocation");
		return;
	}
	memset(bank, 0, len);
	put_pattern(bank, fmt, 0, frames);
	audio_voice_init(&g_ge, bank, len, AUDIO_VOICE_CLOCK_PAL);
	g_ge.interp = interp;
	audio_voice_sample_set(&g_ge, 0, 0, len, fmt);
	c[0] = c_hz(0, hz);
	c[1] = c_vol(0, VOL_FULL, VOL_FULL);
	c[0].stamp = c[1].stamp = g_ge.frame;
	audio_voice_render(&g_ge, g_out, 1, c, 2);
	c[n++] = c_start(0, 0, last * fb, 0);
	if (loop)
		c[n++] = c_loop(0, last * fb, fb);
	if (back)
		c[n++] = c_dir(0, 1);
	for (i = 0; i < n; i++)
		c[i].stamp = g_ge.frame;
	CHECK(audio_voice_render(&g_ge, g_out, 4, c, n) == n,
	      "bank end start consumed");
	wl = vscale(eng(fmt, raw_l(fmt, last)), VOL_FULL);
	wr = vscale(eng(fmt, raw_r(fmt, last)), VOL_FULL);
	CHECK(g_out[0] == wl && g_out[1] == wr,
	      "bank end: last frame plays: format %u len %u interp %u: %d/%d "
	      "want %d/%d", fmt, len, interp, g_out[0], g_out[1], wl, wr);
	if (hz == 48000U) {
		/* One frame per output frame: the last frame again when the
		 * loop is that one frame, silence once a forward pass ends,
		 * and the frames below it going backward. */
		for (f = 1; f < 4; f++) {
			int32_t el = loop ? wl : 0;
			int32_t er = loop ? wr : 0;

			if (back && !loop) {
				el = vscale(eng(fmt, raw_l(fmt, last - f)), VOL_FULL);
				er = vscale(eng(fmt, raw_r(fmt, last - f)), VOL_FULL);
			}
			CHECK(g_out[2U * f] == el && g_out[2U * f + 1U] == er,
			      "bank end: frame %u after the last: %d/%d, want %d/%d",
			      f, g_out[2U * f], g_out[2U * f + 1U], el, er);
		}
	}
	if (!loop && !back)
		CHECK(audio_voice_position(&g_ge, 0, &idx, &fr) == 0,
		      "bank end: the voice stopped");
	guard_bank_free(map, maplen);
}

static void test_stereo_bank_end(void)
{
	static const uint32_t fmts[2] = {AUDIO_VOICE_FMT_S8S,
	                                 AUDIO_VOICE_FMT_S16S};
	static const uint32_t hzs[2] = {48000U, 192000U};
	size_t s, h;
	uint32_t interp;
	int back, loop, slack;

	for (s = 0; s < 2; s++) {
		for (slack = 0; slack < 2; slack++) {
			uint32_t fb = fbytes(fmts[s]);
			uint32_t len = 16U * fb + (slack ? fb - 1U : 0U);

			for (interp = 0; interp < 2; interp++)
				for (back = 0; back < 2; back++)
					for (loop = 0; loop < 2; loop++)
						for (h = 0; h < 2; h++)
							bank_end_case(fmts[s], len, interp,
							              back, loop, hzs[h]);
		}
	}
}

/* DEFINE accepts the four formats and nothing else, and START and LOOP
 * measure the sample in frames. */
static void test_stereo_define(void)
{
	struct audio_voice_sample before;
	struct audio_voice_cmd c[1];

	setup(AUDIO_VOICE_CLOCK_PAL);
	CHECK(AUDIO_VOICE_FMT_COUNT == 4U, "four formats");
	CHECK(audio_voice_sample_set(&g_e, 2, 0, 64, AUDIO_VOICE_FMT_S8S) == 0,
	      "define S8S");
	CHECK(audio_voice_sample_set(&g_e, 3, 64, 64, AUDIO_VOICE_FMT_S16S) == 0,
	      "define S16S");
	CHECK(g_e.sample[2].format == AUDIO_VOICE_FMT_S8S &&
	      g_e.sample[2].defined == 1 &&
	      g_e.sample[3].format == AUDIO_VOICE_FMT_S16S &&
	      g_e.sample[3].length == 64U, "formats and lengths are stored");
	CHECK(audio_voice_sample_set(&g_e, 4, BANK_SIZE - 4, 8,
	                             AUDIO_VOICE_FMT_S16S) == -1,
	      "a stereo range past the bank is refused");

	/* Format 4 and above change nothing: a defined id keeps its geometry. */
	before = g_e.sample[3];
	CHECK(audio_voice_sample_set(&g_e, 3, 128, 32, AUDIO_VOICE_FMT_COUNT) ==
	      -1 && memcmp(&before, &g_e.sample[3], sizeof(before)) == 0,
	      "format 4 leaves a defined id alone");
	CHECK(audio_voice_sample_set(&g_e, 3, 128, 32, 0xFFFFFFFFU) == -1 &&
	      memcmp(&before, &g_e.sample[3], sizeof(before)) == 0,
	      "format 0xFFFFFFFF is refused");
	CHECK(audio_voice_sample_set(&g_e, 9, 0, 16, AUDIO_VOICE_FMT_COUNT) ==
	      -1 && g_e.sample[9].defined == 0, "format 4 defines nothing");
	CHECK(audio_voice_sample_set(&g_e, 9, 0, 16, 5) == -1 &&
	      g_e.sample[9].defined == 0, "format 5 defines nothing");

	/* An 8-byte S16S sample is 2 frames, a 7-byte one is 1 frame. */
	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 8, AUDIO_VOICE_FMT_S16S);
	audio_voice_sample_set(&g_e, 1, 0, 7, AUDIO_VOICE_FMT_S16S);
	c[0] = c_start(0, 0, 8, 0);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 1U && g_e.voice[0].active == 0,
	      "offset 8 of 8 bytes is frame 2 of 2: rejected %u", g_e.rejected);
	c[0] = c_start(0, 0, 7, 0);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 1U && g_e.voice[0].active == 1 &&
	      g_e.voice[0].len == 2U && g_e.voice[0].pos == 1U,
	      "offset 7 of 8 bytes is frame 1: rejected %u", g_e.rejected);
	c[0] = c_start(1, 1, 4, 0);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 2U, "7 bytes are 1 frame, offset 4 is frame 1: "
	      "rejected %u", g_e.rejected);
	c[0] = c_start(1, 0, 0, 3);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 3U, "a first pass of 3 bytes is no frame: "
	      "rejected %u", g_e.rejected);
	c[0] = c_start(1, 0, 0, 4);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 3U && g_e.voice[1].active == 1 &&
	      g_e.voice[1].end == 1U, "a first pass of 4 bytes is 1 frame: "
	      "rejected %u end %u", g_e.rejected, g_e.voice[1].end);

	/* The loop of voice 0 (2 frames): start 2 with no length is allowed,
	 * start 3 is not, and start + length must stay within 2 frames. */
	c[0] = c_loop(0, 8, 0);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 3U, "loop start at the end: rejected %u",
	      g_e.rejected);
	c[0] = c_loop(0, 12, 0);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 4U, "loop start past the end: rejected %u",
	      g_e.rejected);
	c[0] = c_loop(0, 4, 8);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 5U, "loop past the end: rejected %u",
	      g_e.rejected);
	c[0] = c_loop(0, 4, 4);
	run_now(1, c, 1);
	CHECK(g_e.rejected == 5U && g_e.voice[0].loopstart == 1U &&
	      g_e.voice[0].looplen == 1U, "loop of 1 frame at 1: rejected %u",
	      g_e.rejected);
}

/* ---- the SDK timing helpers (sdk/include/zz9k/voice.h) ----
 *
 * zz9k_voice_rate_step() and zz9k_voice_pass_frames() are the one published
 * source of the engine's rate and pass arithmetic, which AHI drivers mirror
 * on the 68k. The engine here is the reference: these tests fail when the
 * engine and the helpers disagree. */

static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1664525U + 1013904223U;
	return *s >> 8;
}

static void test_pinned_constants(void)
{
	uint32_t fmt;

	CHECK(AUDIO_VOICE_FMT_S8 == ZZ9K_VOICE_FORMAT_S8 &&
	      AUDIO_VOICE_FMT_S16 == ZZ9K_VOICE_FORMAT_S16 &&
	      AUDIO_VOICE_FMT_S8S == ZZ9K_VOICE_FORMAT_S8S &&
	      AUDIO_VOICE_FMT_S16S == ZZ9K_VOICE_FORMAT_S16S &&
	      AUDIO_VOICE_FMT_COUNT == ZZ9K_VOICE_FORMAT_COUNT,
	      "format numbers differ from the SDK");
	CHECK(AUDIO_VOICE_MAX_HZ == ZZ9K_VOICE_RATE_MAX_HZ &&
	      AUDIO_VOICE_OUT_HZ == ZZ9K_VOICE_OUTPUT_RATE &&
	      AUDIO_VOICE_MAX_VOICES == ZZ9K_VOICE_MAX_VOICES &&
	      AUDIO_VOICE_MAX_SAMPLES == ZZ9K_VOICE_MAX_SAMPLES,
	      "limits differ from the SDK");
	CHECK(AUDIO_VOICE_OP_START == ZZ9K_VOICE_OP_START &&
	      AUDIO_VOICE_OP_LOOP == ZZ9K_VOICE_OP_LOOP &&
	      AUDIO_VOICE_OP_SET == ZZ9K_VOICE_OP_SET &&
	      AUDIO_VOICE_OP_STOP == ZZ9K_VOICE_OP_STOP &&
	      AUDIO_VOICE_OP_MASTER == ZZ9K_VOICE_OP_MASTER &&
	      AUDIO_VOICE_OP_DIRECTION == ZZ9K_VOICE_OP_DIRECTION &&
	      AUDIO_VOICE_SET_RATE == ZZ9K_VOICE_SET_RATE &&
	      AUDIO_VOICE_SET_VOLUME == ZZ9K_VOICE_SET_VOLUME &&
	      AUDIO_VOICE_RATE_HZ == ZZ9K_VOICE_SET_RATE_HZ &&
	      AUDIO_VOICE_DIR_BACKWARD == ZZ9K_VOICE_DIR_BACKWARD &&
	      AUDIO_VOICE_MASTER_UNITY == ZZ9K_VOICE_MASTER_UNITY,
	      "op numbers differ from the SDK");

	/* The engine's sample length in frames is the byte length shifted by
	 * the helper's frame shift. */
	for (fmt = 0; fmt < AUDIO_VOICE_FMT_COUNT; fmt++) {
		struct audio_voice_cmd c[1];

		setup(AUDIO_VOICE_CLOCK_PAL);
		audio_voice_sample_set(&g_e, 0, 0, 101, fmt);
		c[0] = c_start(0, 0, 0, 0);
		run_now(1, c, 1);
		CHECK(g_e.voice[0].len == 101U >> zz9k_voice_format_frame_shift(fmt),
		      "format %u: %u frames, the SDK shift gives %u", fmt,
		      g_e.voice[0].len, 101U >> zz9k_voice_format_frame_shift(fmt));
	}
}

/* A voice that plays 500 frames from index 0 sits at 500 * step in 16.16. */
static void check_rate_step(uint32_t hz, uint32_t prior_hz)
{
	struct audio_voice_cmd c[3];
	uint32_t step = zz9k_voice_rate_step(hz != 0U ? hz : prior_hz);
	uint64_t want = (uint64_t)500U * step;
	size_t n = 0;

	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, 0, 4000, AUDIO_VOICE_FMT_S8);
	if (prior_hz != 0U)
		c[n++] = c_hz(0, prior_hz);
	c[n++] = c_hz(0, hz);
	c[n++] = c_vol(0, VOL_FULL, VOL_FULL);
	run_now(1, c, n);
	c[0] = c_start(0, 0, 0, 0);
	run_now(500, c, 1);
	check_pos("rate step", 0, 1, (uint32_t)(want >> 16),
	          (uint32_t)(want & 0xFFFFU));
}

static void test_pinned_rate_step(void)
{
	static const uint32_t extra[] = {
		4000U, 8000U, 8363U, 22050U, 44100U, 48000U, 96000U, 192000U,
		192001U, 250000U, 0xFFFFFFFFU
	};
	uint32_t hz;
	size_t i;

	for (hz = 4000U; hz <= 192000U; hz += 997U)
		check_rate_step(hz, 0U);
	for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
		check_rate_step(extra[i], 0U);
	/* The engine ignores 0 Hz, which the helper reads as no change. */
	CHECK(zz9k_voice_rate_step(0U) == 0U, "0 Hz is step 0");
	check_rate_step(0U, 8000U);
	check_rate_step(0U, 192000U);
}

static int pass_failed;

static void check_pass_case(uint32_t *seed, uint32_t hz)
{
	struct audio_voice_cmd c[12];
	size_t n = 0;
	uint32_t fmt = rnd(seed) & 3U;
	uint32_t shift = zz9k_voice_format_frame_shift(fmt);
	uint32_t fb = 1U << shift;
	uint32_t nframes = 4U + rnd(seed) % 397U;
	uint32_t base = rnd(seed) % 64U;
	int back = (int)(rnd(seed) & 1U);
	uint32_t pos0 = rnd(seed) % nframes;
	uint32_t ls = rnd(seed) % nframes;
	uint32_t ll = 1U + rnd(seed) % (nframes - ls);
	uint32_t step = zz9k_voice_rate_step(hz);
	uint32_t lim = 0U;
	uint32_t arg1 = 0U;
	uint32_t bound, t0, j, m, k, v;
	uint32_t p0, f0, p1, f1, pt, ft, expect;
	int a0, a1;

	if (!back && rnd(seed) % 3U != 0U) {
		lim = 1U + rnd(seed) % (nframes - pos0);
		arg1 = (lim << shift) + rnd(seed) % fb;
	}
	bound = back ? ls :
	        ((lim != 0U && pos0 + lim < nframes) ? pos0 + lim : nframes);

	/* Voice 0 loops, voice 1 is the same voice without the loop: it
	 * stops where voice 0 wraps (backward, its loop start is a plain
	 * lower bound). */
	setup(AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&g_e, 0, base, (nframes << shift) + rnd(seed) % fb,
	                       fmt);
	for (v = 0; v < 2U; v++) {
		c[n++] = c_hz((uint8_t)v, hz);
		c[n++] = c_vol((uint8_t)v, VOL_FULL, VOL_FULL);
	}
	for (v = 0; v < 2U; v++)
		c[n++] = c_start((uint8_t)v, 0, (pos0 << shift) + rnd(seed) % fb,
		                 arg1);
	c[n++] = c_loop(0, (ls << shift) + rnd(seed) % fb,
	                (ll << shift) + rnd(seed) % fb);
	if (back) {
		c[n++] = c_loop(1, (ls << shift) + rnd(seed) % fb, 0);
		c[n++] = c_dir(0, 1);
		c[n++] = c_dir(1, 1);
	}
	t0 = zz9k_voice_pass_frames(back, pos0, 0U, bound, step);
	if (t0 == ZZ9K_VOICE_PASS_NEVER || t0 > 100000U) {
		CHECK(0, "pass frames %u for hz %u", t0, hz);
		pass_failed = 1;
		return;
	}
	j = 1U + rnd(seed) % t0;
	CHECK(run_now(j, c, n) == n, "pass: commands consumed");

	if (j < t0) {
		/* Mid pass: the helper from the position the engine reached
		 * must predict the frames left, and the frame the twin stops
		 * on must be where it says. */
		a0 = audio_voice_position(&g_e, 0, &p0, &f0);
		a1 = audio_voice_position(&g_e, 1, &p1, &f1);
		if (!a0 || !a1 || p0 != p1 || f0 != f1) {
			CHECK(0, "pass: hz %u format %u back %d: mid state %d %d "
			      "pos %u/%u frac %u/%u after %u of %u", hz, fmt, back,
			      a0, a1, p0, p1, f0, f1, j, t0);
			pass_failed = 1;
			return;
		}
		k = zz9k_voice_pass_frames(back, p1, f1, bound, step);
		for (m = 1U; m <= 100000U; m++) {
			audio_voice_render(&g_e, g_out, 1, NULL, 0);
			if (!audio_voice_position(&g_e, 1, &pt, &ft))
				break;
		}
		if (m != k || j + m != t0) {
			CHECK(0, "pass: hz %u format %u back %d pos %u frac %u "
			      "bound %u: helper %u, engine %u, first call %u after "
			      "%u", hz, fmt, back, p1, f1, bound, k, m, t0, j);
			pass_failed = 1;
			return;
		}
	} else {
		a1 = audio_voice_position(&g_e, 1, &pt, &ft);
		if (a1) {
			CHECK(0, "pass: hz %u format %u back %d: the voice ran "
			      "past the %u frames of the first call", hz, fmt, back,
			      t0);
			pass_failed = 1;
			return;
		}
	}

	/* The looping voice wrapped on that same frame. */
	a0 = audio_voice_position(&g_e, 0, &p0, &f0);
	if (!back) {
		uint32_t over = pt - bound;

		expect = ls + (over >= ll ? 0U : over);
	} else {
		uint32_t under = ls - pt;

		expect = ls + ll - (under >= ll ? 0U : under) - 1U;
	}
	if (!a0 || p0 != expect || g_e.rejected != 0U) {
		CHECK(0, "pass: hz %u format %u back %d: the loop voice is at %u "
		      "(active %d), want %u; rejected %u", hz, fmt, back, p0, a0,
		      expect, g_e.rejected);
		pass_failed = 1;
	}
}

static void test_pinned_pass_frames(void)
{
	uint32_t seed = 0x1badcafeU;
	uint32_t i;

	for (i = 0; i < 4000U && !pass_failed; i++) {
		uint32_t hz = (i % 8U == 7U) ? 192000U : 4000U + 1597U * (i % 118U);

		check_pass_case(&seed, hz);
	}
}

int main(void)
{
	test_ramp_position();
	test_ntsc_clock();
	test_oneshot_stops();
	test_loop_wrap();
	test_loop_after_first_pass();
	test_backward();
	test_backward_oneshot();
	test_direction();
	test_rejected();
	test_rates();
	test_impulse_timing();
	test_late_and_horizon();
	test_saturation();
	test_volume_and_master();
	test_interpolation();
	test_interpolation_loop();
	test_stamp_wrap();
	test_mono_golden();
	test_stereo_sides();
	test_stereo_frames();
	test_stereo_interpolation();
	test_stereo_bank_end();
	test_stereo_define();
	test_pinned_constants();
	test_pinned_rate_step();
	test_pinned_pass_frames();

	if (failures) {
		printf("audio_voice_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_voice_test: all passed\n");
	return 0;
}
