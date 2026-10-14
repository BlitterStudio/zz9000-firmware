/*
 * zz_audio_convert_stream must stay bit-identical to the plain tap loop it
 * replaced (one int64 multiply-add per tap, history read per tap): same
 * output samples, clip count, position and carried history after every
 * call, for every ratio, from a cold start through chained periods.
 * Built for the host (scalar path) and, by test/audio/convert_neon_run.sh,
 * for the Cortex-A9 under qemu-arm (NEON path).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio_convert.h"

#include <stdio.h>
#include <string.h>

#define PERIODS 12U
#define MAX_IN 1920U
#define MAX_OUT 1920U

/* The kernel as it was before the window split, kept as the reference. */
static int16_t ref_saturate(int64_t acc, uint32_t recip, uint32_t *clips)
{
	int64_t normalized = (acc * (int64_t)recip + (1 << 15)) >> 16;
	int64_t value =
	    (normalized + (1 << (ZZ_AUDIO_CONVERT_SHIFT - 1))) >>
	    ZZ_AUDIO_CONVERT_SHIFT;

	if (value > 32767) {
		value = 32767;
		(*clips)++;
	} else if (value < -32768) {
		value = -32768;
		(*clips)++;
	}
	return (int16_t)value;
}

static int16_t ref_history(const struct zz_audio_convert *ctx,
                           uint16_t channel, int32_t idx)
{
	int32_t h = idx + (int32_t)ctx->history_len;

	return h >= 0 ? ctx->history[channel][h] : 0;
}

static void ref_stream(struct zz_audio_convert *ctx, const int16_t *in,
                       int16_t *out, uint16_t in_frames, uint16_t out_frames)
{
	const struct zz_audio_convert_ratio *ratio = ctx->ratio;
	uint32_t n;
	uint16_t keep;
	int32_t j;

	for (n = 0U; n < out_frames; n++) {
		const uint32_t phase = ctx->pos_frac % ratio->phases;
		const int16_t *coef = ratio->coefs[phase];
		int32_t base = (int32_t)ctx->pos_int;
		uint16_t ch;

		for (ch = 0U; ch < 2U; ch++) {
			int64_t acc = 0;
			int32_t k;

			for (k = 0; k < (int32_t)ratio->taps; k++) {
				int32_t idx = base - k;
				int16_t s = idx >= 0
				    ? in[(uint32_t)idx * 2U + ch]
				    : ref_history(ctx, ch, idx);

				acc += (int64_t)coef[k] * (int64_t)s;
			}
			out[n * 2U + ch] =
			    ref_saturate(acc, ratio->recip[phase], &ctx->clips);
		}
		ctx->pos_int += ratio->step_int;
		ctx->pos_frac += ratio->step_num;
		if (ctx->pos_frac >= ratio->step_den) {
			ctx->pos_frac -= ratio->step_den;
			ctx->pos_int += 1U;
		}
	}
	ctx->pos_int = 0U;
	ctx->pos_frac = 0U;
	keep = ratio->taps - 1U;
	for (j = 0; j < (int32_t)keep; j++) {
		int32_t src = (int32_t)in_frames - (int32_t)keep + j;
		uint16_t ch;

		for (ch = 0U; ch < 2U; ch++)
			ctx->history[ch][j] = src >= 0
			    ? in[(uint32_t)src * 2U + ch]
			    : ref_history(ctx, ch, src);
	}
	ctx->history_len = keep;
}

static uint32_t rng = 0x12345678U;

static int16_t next_sample(uint32_t period, uint32_t i)
{
	rng = rng * 1664525U + 1013904223U;
	switch (period % 4U) {
	case 0:
		return (int16_t)(rng >> 16); /* full-scale noise */
	case 1:
		/* Full-scale square wave (64-frame halves): its filtered
		 * edges overshoot, so every ratio reaches the clip path. */
		return ((i / 2U / 64U) & 1U) ? (int16_t)32767 : (int16_t)-32768;
	case 2:
		return (int16_t)((int32_t)(rng >> 16) >> 6); /* quiet */
	default:
		return (rng & 0x80000000U) ? (int16_t)-32768 : (int16_t)32767;
	}
}

struct pair {
	uint32_t in_rate, out_rate;
	uint16_t in_frames, out_frames;
};

/* The per-call frame pairs the sites use: 20 ms periods (11025 Hz
 * converts 40 ms quanta, see audio_pump_preconvert.c). */
static const struct pair pairs[] = {
	{ 8000U, 48000U, 160U, 960U },    { 11025U, 48000U, 441U, 1920U },
	{ 12000U, 48000U, 240U, 960U },   { 16000U, 48000U, 320U, 960U },
	{ 22050U, 48000U, 441U, 960U },   { 24000U, 48000U, 480U, 960U },
	{ 32000U, 48000U, 640U, 960U },   { 44100U, 48000U, 882U, 960U },
	{ 88200U, 48000U, 1764U, 960U },  { 96000U, 48000U, 1920U, 960U },
	{ 48000U, 8000U, 960U, 160U },    { 48000U, 12000U, 960U, 240U },
	{ 48000U, 24000U, 960U, 480U },   { 48000U, 32000U, 960U, 640U },
	{ 48000U, 44100U, 960U, 882U },
};

int main(void)
{
	static int16_t in[MAX_IN * 2U];
	static int16_t out[MAX_OUT * 2U];
	static int16_t ref_out[MAX_OUT * 2U];
	static struct zz_audio_convert ctx, ref;
	unsigned failures = 0U;
	uint32_t p, period, i;

	for (p = 0U; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
		const struct pair *t = &pairs[p];
		uint32_t total_clips = 0U;

		if (zz_audio_convert_init(&ctx, t->in_rate, t->out_rate) != 0 ||
		    zz_audio_convert_init(&ref, t->in_rate, t->out_rate) != 0) {
			printf("FAIL %lu->%lu: no ratio\n",
			       (unsigned long)t->in_rate,
			       (unsigned long)t->out_rate);
			failures++;
			continue;
		}
		for (period = 0U; period < PERIODS; period++) {
			const uint16_t keep = ctx.ratio->taps - 1U;
			uint16_t ch;
			int same_history = 1;

			for (i = 0U; i < t->in_frames * 2U; i++)
				in[i] = next_sample(period, i);
			memset(out, 0x55, sizeof(out));
			memset(ref_out, 0xAA, sizeof(ref_out));
			zz_audio_convert_stream(&ctx, in, out, t->in_frames,
			                        t->out_frames);
			ref_stream(&ref, in, ref_out, t->in_frames,
			           t->out_frames);
			for (ch = 0U; ch < 2U; ch++)
				if (memcmp(ctx.history[ch], ref.history[ch],
				           (size_t)keep * 2U) != 0)
					same_history = 0;
			if (memcmp(out, ref_out,
			           (size_t)t->out_frames * 4U) != 0 ||
			    ctx.clips != ref.clips ||
			    ctx.pos_int != ref.pos_int ||
			    ctx.pos_frac != ref.pos_frac ||
			    ctx.history_len != ref.history_len ||
			    !same_history) {
				printf("FAIL %lu->%lu period %lu: output, "
				       "clips (%lu vs %lu) or state differ\n",
				       (unsigned long)t->in_rate,
				       (unsigned long)t->out_rate,
				       (unsigned long)period,
				       (unsigned long)ctx.clips,
				       (unsigned long)ref.clips);
				failures++;
				break;
			}
		}
		total_clips = ctx.clips;
		if (total_clips == 0U) {
			/* The extreme periods must reach the clip path, or
			 * saturation was never compared. */
			printf("FAIL %lu->%lu: no clipping exercised\n",
			       (unsigned long)t->in_rate,
			       (unsigned long)t->out_rate);
			failures++;
		}
	}
	if (failures) {
		printf("audio_convert_exact_test: %u failure(s)\n", failures);
		return 1;
	}
	printf("audio_convert_exact_test: all %u ratios bit-identical\n",
	       (unsigned)(sizeof(pairs) / sizeof(pairs[0])));
	return 0;
}
