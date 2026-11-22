/*
 * Behavioural test for the voice engine (audio_voice.c): voice position
 * and loop math, command timing, rate rules, mixing and saturation. Every
 * check drives the production audio_voice_* functions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

	if (failures) {
		printf("audio_voice_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_voice_test: all passed\n");
	return 0;
}
