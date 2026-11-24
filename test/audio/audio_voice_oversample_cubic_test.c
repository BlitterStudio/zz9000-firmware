/*
 * 2x oversampling and the cubic interpolation of the voice engine: the
 * records that would combine them are rejected (AUDIO_VOICE_OVERSAMPLE_CUBIC
 * 0, the product), and with the macro at 1 (built for the qualification of
 * the engine only) the same records are accepted and the cubic voice mixes
 * at the oversampled rate.
 *
 * The rejection is atomic: the engine after a rejected record is the engine
 * that never saw it, bit for bit, but for the rejected count. That is
 * checked on the whole engine structure after the same renders with and
 * without the record, and on the output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_voice.h"

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

#define BANK_BYTES 16384U
#define DC_FRAMES  6000U
#define SMP_DC     0U /* S16, constant 12000 */
#define SMP_SAW    1U /* S16, a slow ramp: cubic and linear differ from nearest */
#define FRAMES     600U

static uint8_t g_bank[BANK_BYTES];

static void put16(uint32_t off, int32_t v)
{
	g_bank[off] = (uint8_t)((uint32_t)v >> 8);
	g_bank[off + 1U] = (uint8_t)v;
}

static void engine_init(struct audio_voice_engine *e)
{
	audio_voice_init(e, g_bank, BANK_BYTES, AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(e, SMP_DC, 0U, 2U * DC_FRAMES, AUDIO_VOICE_FMT_S16);
	audio_voice_sample_set(e, SMP_SAW, 2U * DC_FRAMES, 2000U,
	                       AUDIO_VOICE_FMT_S16);
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
#define DEF_INTERP(stamp, sel) \
	mk(stamp, 0, AUDIO_VOICE_OP_CONFIG, AUDIO_VOICE_CONFIG_INTERP, sel, 0)
#define VOICE_INTERP(stamp, v, sel) \
	mk(stamp, v, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP, sel, 0)
#define SET_HZ(stamp, v, hz, l, r) \
	mk(stamp, v, AUDIO_VOICE_OP_SET, \
	   AUDIO_VOICE_SET_RATE | AUDIO_VOICE_SET_VOLUME | AUDIO_VOICE_RATE_HZ, \
	   hz, ((uint32_t)(l) << 16) | (uint32_t)(r))
#define START(stamp, v, id) mk(stamp, v, AUDIO_VOICE_OP_START, id, 0, 0)
#define STOP(stamp, v) mk(stamp, v, AUDIO_VOICE_OP_STOP, 0, 0, 0)

#define CUBIC AUDIO_VOICE_INTERP_MODE_CUBIC
#define LINEAR AUDIO_VOICE_INTERP_MODE_LINEAR
#define NEAREST AUDIO_VOICE_INTERP_MODE_NEAREST

static void render(struct audio_voice_engine *e, int16_t *out, uint32_t frames,
                   const struct audio_voice_cmd *c, size_t n)
{
	const size_t used = audio_voice_render(e, out, frames, c, n);

	CHECK(used == n, "render consumed %zu of %zu records", used, n);
}

#if AUDIO_VOICE_OVERSAMPLE_CUBIC == 0

/* The records' stamps are relative to the engine's next frame. */
static void render_at(struct audio_voice_engine *e, int16_t *out,
                      uint32_t frames, struct audio_voice_cmd *c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		c[i].stamp += e->frame;
	render(e, out, frames, c, n);
}

/* Two engines that took the same records, the second without the ones the
 * bits of `skip` name (the rejected ones): the same output, and the same
 * engine structure but for the rejected count, which is `rej` higher on the
 * one that saw them. A record rejected before the engine synced, cut or
 * changed anything leaves no other trace. */
static void run_pair(const struct audio_voice_cmd *with, size_t n_with,
                     uint64_t skip, uint32_t rej, const char *what)
{
	static struct audio_voice_engine a, b;
	static int16_t out_a[FRAMES * 2U], out_b[FRAMES * 2U];
	struct audio_voice_cmd without[48];
	size_t i, n_without = 0;

	for (i = 0; i < n_with; i++) {
		if ((skip & ((uint64_t)1 << i)) == 0U)
			without[n_without++] = with[i];
	}
	engine_init(&a);
	engine_init(&b);
	render(&a, out_a, FRAMES, with, n_with);
	render(&b, out_b, FRAMES, without, n_without);
	CHECK(memcmp(out_a, out_b, sizeof(out_a)) == 0,
	      "%s: the output differs from the engine without the record", what);
	CHECK(a.rejected == b.rejected + rej, "%s: rejected %u, expected %u", what,
	      a.rejected, b.rejected + rej);
	a.rejected = b.rejected;
	{
		/* everything up to the scratch of the last render (chunk_out and
		 * what follows is no state between calls) */
		const size_t state = offsetof(struct audio_voice_engine, chunk_out);
		const uint8_t *pa = (const uint8_t *)&a, *pb = (const uint8_t *)&b;
		size_t at = 0;

		while (at < state && pa[at] == pb[at])
			at++;
		CHECK(at == state,
		      "%s: the engine is not the engine without the record (first "
		      "difference at byte %zu of %zu)", what, at, state);
	}
}

static void test_refusals(void)
{
	struct audio_voice_cmd w[48];
	size_t n;
	uint32_t v;

	/* The default is cubic: 2x is refused (record 1), and a voice that
	 * sounds later is a plain cubic voice. */
	n = 0;
	w[n++] = DEF_INTERP(0, CUBIC);
	w[n++] = OS_ON(20);
	w[n++] = SET_HZ(100, 0, 24000U, 0x4000U, 0x4000U);
	w[n++] = START(100, 0, SMP_SAW);
	run_pair(w, n, 1U << 1, 1U, "default cubic, then 2x");

	/* An explicit cubic voice that never played: 2x is refused. */
	n = 0;
	w[n++] = VOICE_INTERP(0, 7, CUBIC);
	w[n++] = OS_ON(20);
	w[n++] = SET_HZ(100, 0, 24000U, 0x4000U, 0x4000U);
	w[n++] = START(100, 0, SMP_SAW);
	run_pair(w, n, 1U << 1, 1U, "idle cubic voice, then 2x");

	/* The same for a voice that played and stopped: the selector stays. */
	n = 0;
	w[n++] = SET_HZ(0, 3, 24000U, 0x4000U, 0x4000U);
	w[n++] = VOICE_INTERP(0, 3, CUBIC);
	w[n++] = START(0, 3, SMP_SAW);
	w[n++] = STOP(50, 3);
	w[n++] = OS_ON(300);
	run_pair(w, n, 1U << 4, 1U, "stopped cubic voice, then 2x");

	/* Default cubic with every one of the 32 voices overridden: still
	 * refused (the default is cubic). */
	n = 0;
	w[n++] = DEF_INTERP(0, CUBIC);
	for (v = 0; v < AUDIO_VOICE_MAX_VOICES; v++)
		w[n++] = VOICE_INTERP(0, v, NEAREST);
	w[n++] = OS_ON(20);
	run_pair(w, n, (uint64_t)1 << (n - 1U), 1U,
	         "default cubic, every voice nearest, then 2x");

	/* 2x on, then the default cubic and a voice cubic: both refused, and
	 * the sound that follows is the sound of 2x with the old selectors. */
	n = 0;
	w[n++] = OS_ON(0);
	w[n++] = DEF_INTERP(20, CUBIC);
	w[n++] = VOICE_INTERP(30, 5, CUBIC);
	w[n++] = SET_HZ(100, 0, 24000U, 0x4000U, 0x4000U);
	w[n++] = START(100, 0, SMP_SAW);
	run_pair(w, n, (1U << 1) | (1U << 2), 2U, "cubic default and voice under 2x");
}

static void test_accepted(void)
{
	static struct audio_voice_engine e;
	static int16_t out[FRAMES * 2U];
	struct audio_voice_cmd c[12];
	size_t n;

	/* Everything but cubic is accepted around 2x. */
	engine_init(&e);
	n = 0;
	c[n++] = DEF_INTERP(0, LINEAR);
	c[n++] = VOICE_INTERP(0, 2, NEAREST);
	c[n++] = OS_ON(10);
	c[n++] = OS_ON(11);                 /* the value in force */
	c[n++] = DEF_INTERP(20, NEAREST);
	c[n++] = VOICE_INTERP(30, 2, 0U);   /* inherit */
	c[n++] = VOICE_INTERP(31, 2, LINEAR);
	c[n++] = OS_OFF(40);
	c[n++] = DEF_INTERP(41, CUBIC);     /* 2x is off: accepted */
	c[n++] = VOICE_INTERP(42, 9, CUBIC);
	render_at(&e, out, FRAMES, c, n);
	CHECK(e.rejected == 0U, "accepted records: rejected %u", e.rejected);
	CHECK(e.oversample == 0U && e.interp == AUDIO_VOICE_INTERP_CUBIC &&
	      e.voice[9].vinterp == CUBIC, "state after the accepted records");

	/* The cubic selectors are lifted, then 2x is accepted. */
	n = 0;
	c[n++] = DEF_INTERP(0, NEAREST);
	c[n++] = VOICE_INTERP(0, 9, 0U);
	c[n++] = OS_ON(10);
	render_at(&e, out, FRAMES, c, n);
	CHECK(e.rejected == 0U && e.oversample == 1U,
	      "2x after the cubic selectors are lifted: rejected %u oversample %u",
	      e.rejected, e.oversample);

	/* 2x goes off whatever the interpolation, and a rejected cubic leaves
	 * the setting alone. */
	n = 0;
	c[n++] = DEF_INTERP(0, CUBIC);                  /* rejected */
	c[n++] = VOICE_INTERP(0, 4, CUBIC);             /* rejected */
	c[n++] = OS_OFF(10);
	c[n++] = DEF_INTERP(20, CUBIC);                 /* now accepted */
	render_at(&e, out, FRAMES, c, n);
	CHECK(e.rejected == 2U && e.oversample == 0U &&
	      e.interp == AUDIO_VOICE_INTERP_CUBIC && e.voice[4].vinterp == 0U,
	      "after 2x off: rejected %u oversample %u interp %u voice4 %u",
	      e.rejected, e.oversample, e.interp, e.voice[4].vinterp);
}

#else /* AUDIO_VOICE_OVERSAMPLE_CUBIC */

/* A constant sample reads the same under every interpolation, whatever the
 * rate: with the macro at 1 a cubic voice under 2x is accepted and gives the
 * output of the nearest voice under 2x, bit for bit. */
static void test_compat(void)
{
	static struct audio_voice_engine e, n;
	static int16_t out_e[FRAMES * 2U], out_n[FRAMES * 2U];
	struct audio_voice_cmd c[8];
	size_t k = 0;
	uint32_t i;

	c[k++] = OS_ON(0);
	c[k++] = DEF_INTERP(1, CUBIC);
	c[k++] = VOICE_INTERP(1, 6, CUBIC);
	c[k++] = SET_HZ(50, 0, 31000U, 0x5000U, 0x3000U);
	c[k++] = START(50, 0, SMP_DC);
	engine_init(&e);
	render(&e, out_e, FRAMES, c, k);
	CHECK(e.rejected == 0U && e.oversample == 1U &&
	      e.interp == AUDIO_VOICE_INTERP_CUBIC && e.voice[6].vinterp == CUBIC,
	      "cubic under 2x: rejected %u oversample %u interp %u", e.rejected,
	      e.oversample, e.interp);

	k = 0;
	c[k++] = OS_ON(0);
	c[k++] = SET_HZ(50, 0, 31000U, 0x5000U, 0x3000U);
	c[k++] = START(50, 0, SMP_DC);
	engine_init(&n);
	render(&n, out_n, FRAMES, c, k);
	CHECK(n.rejected == 0U, "nearest under 2x: rejected %u", n.rejected);
	for (i = 0; i < FRAMES * 2U; i++)
		CHECK(out_e[i] == out_n[i], "sample %u: cubic %d, nearest %d", i,
		      out_e[i], out_n[i]);
	CHECK(out_e[2U * (FRAMES - 1U)] != 0, "the voice sounds");
}

#endif

int main(void)
{
	uint32_t i;

	for (i = 0; i < DC_FRAMES; i++)
		put16(2U * i, 12000);
	for (i = 0; i < 1000U; i++)
		put16(2U * DC_FRAMES + 2U * i, (int32_t)((i * 37U) % 20001U) - 10000);
#if AUDIO_VOICE_OVERSAMPLE_CUBIC == 0
	test_refusals();
	test_accepted();
#else
	test_compat();
#endif
	if (failures != 0) {
		printf("audio_voice_oversample_cubic_test: %d FAILURES\n", failures);
		return 1;
	}
	printf("audio_voice_oversample_cubic_test: AUDIO_VOICE_OVERSAMPLE_CUBIC=%d "
	       "all passed\n", AUDIO_VOICE_OVERSAMPLE_CUBIC);
	return 0;
}
