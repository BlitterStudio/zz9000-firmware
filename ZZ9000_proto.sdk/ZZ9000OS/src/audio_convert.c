/*
 * ZZ9000AX qualified fixed-point audio conversion core.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_convert.h"

#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static const struct zz_audio_convert_ratio *ratio_for(uint32_t in_rate,
                                                      uint32_t out_rate)
{
	switch (in_rate) {
	case 8000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_8000_48000;
		break;
	case 11025U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_11025_48000;
		break;
	case 12000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_12000_48000;
		break;
	case 16000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_16000_48000;
		break;
	case 22050U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_22050_48000;
		break;
	case 24000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_24000_48000;
		break;
	case 32000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_32000_48000;
		break;
	case 44100U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_44100_48000;
		break;
	case 88200U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_88200_48000;
		break;
	case 96000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_96000_48000;
		break;
	case 48000U:
		switch (out_rate) {
		case 8000U:
			return &zz_audio_convert_ratio_48000_8000;
		case 12000U:
			return &zz_audio_convert_ratio_48000_12000;
		case 24000U:
			return &zz_audio_convert_ratio_48000_24000;
		case 32000U:
			return &zz_audio_convert_ratio_48000_32000;
		case 44100U:
			return &zz_audio_convert_ratio_48000_44100;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return NULL;
}

void zz_audio_convert_reset(struct zz_audio_convert *ctx)
{
	ctx->pos_int = 0U;
	ctx->pos_frac = 0U;
	ctx->history_len = 0U;
	/* clips is diagnostic state and survives a reset-by-design only when
	 * the caller asks for a full init; a mid-stream reset clears it with
	 * the rest so reset stays one atomic event. */
	ctx->clips = 0U;
	memset(ctx->history, 0, sizeof(ctx->history));
}

int zz_audio_convert_init(struct zz_audio_convert *ctx, uint32_t in_rate,
                          uint32_t out_rate)
{
	if (in_rate == 0U || out_rate == 0U) {
		/* The documented contract says init always resets; a failed
		 * init must also disarm any previously armed ratio so a
		 * zero-rate caller cannot keep converting at a stale ratio. */
		ctx->ratio = NULL;
		zz_audio_convert_reset(ctx);
		return -1;
	}

	ctx->ratio = ratio_for(in_rate, out_rate);
	zz_audio_convert_reset(ctx);
	return ctx->ratio ? 0 : 1;
}

/* Full-range taps need a 64-bit accumulator (sum|taps|*32767 exceeds
 * int32); each output is re-normalized by the phase's Q16 reciprocal
 * (16384/scale) before the final round-shift. */
static int16_t saturate_round(int64_t acc, uint32_t recip,
                              uint32_t *clips)
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

/* An output's window is taps input frames ending at its base, newest
 * first: frames base, base-1, ... from the current input while they
 * exist, then the carried history, then zeros before the stream start.
 * Each part is contiguous, so it is summed without a per-tap branch.
 * Integer sums are exact in any order, so every path below gives the
 * same int64 result as the plain tap loop. */

/* acc[ch] += sum over j < count of coef[j] * in[(last - j) * 2 + ch]. */
static inline void dot_input(const int16_t *coef, const int16_t *in,
                             int32_t last, uint32_t count, int64_t acc[2])
{
	uint32_t j = 0U;
	int64_t left = 0;
	int64_t right = 0;

#if defined(__ARM_NEON)
	{
		/* vmull: exact 32-bit products (|p| <= 2^30); vpadal: exact
		 * pairwise widening into 64-bit lanes. */
		int64x2_t vl = vdupq_n_s64(0);
		int64x2_t vr = vdupq_n_s64(0);

		for (; j + 4U <= count; j += 4U) {
			/* Frames last-j-3 .. last-j, oldest first, against
			 * coef[j+3] .. coef[j]. */
			int16x4x2_t f = vld2_s16(in + (last - (int32_t)j - 3) * 2);
			int16x4_t c = vrev64_s16(vld1_s16(coef + j));

			vl = vpadalq_s32(vl, vmull_s16(f.val[0], c));
			vr = vpadalq_s32(vr, vmull_s16(f.val[1], c));
		}
		left = vgetq_lane_s64(vl, 0) + vgetq_lane_s64(vl, 1);
		right = vgetq_lane_s64(vr, 0) + vgetq_lane_s64(vr, 1);
	}
#endif
	for (; j < count; j++) {
		const int16_t *frame = in + (last - (int32_t)j) * 2;

		left += (int64_t)coef[j] * frame[0];
		right += (int64_t)coef[j] * frame[1];
	}
	acc[0] += left;
	acc[1] += right;
}

/* acc[ch] += sum over j < count of coef[j] * history[ch][last - j]. */
static inline void dot_history(const int16_t *coef,
                               const struct zz_audio_convert *ctx,
                               int32_t last, uint32_t count, int64_t acc[2])
{
	const int16_t *hist_left = ctx->history[0];
	const int16_t *hist_right = ctx->history[1];
	uint32_t j = 0U;
	int64_t left = 0;
	int64_t right = 0;

#if defined(__ARM_NEON)
	{
		int64x2_t vl = vdupq_n_s64(0);
		int64x2_t vr = vdupq_n_s64(0);

		for (; j + 4U <= count; j += 4U) {
			int32_t first = last - (int32_t)j - 3;
			int16x4_t c = vrev64_s16(vld1_s16(coef + j));

			vl = vpadalq_s32(vl,
			                 vmull_s16(vld1_s16(hist_left + first), c));
			vr = vpadalq_s32(vr,
			                 vmull_s16(vld1_s16(hist_right + first), c));
		}
		left = vgetq_lane_s64(vl, 0) + vgetq_lane_s64(vl, 1);
		right = vgetq_lane_s64(vr, 0) + vgetq_lane_s64(vr, 1);
	}
#endif
	for (; j < count; j++) {
		left += (int64_t)coef[j] * hist_left[last - (int32_t)j];
		right += (int64_t)coef[j] * hist_right[last - (int32_t)j];
	}
	acc[0] += left;
	acc[1] += right;
}

/* Sample the virtual input stream at absolute-negative index idx
 * (idx < 0): history when covered, zero before the stream started. */
static inline int16_t history_sample(const struct zz_audio_convert *ctx,
                                     uint16_t channel, int32_t idx)
{
	int32_t h = idx + (int32_t)ctx->history_len;

	if (h >= 0)
		return ctx->history[channel][h];
	return 0;
}

void zz_audio_convert_stream(struct zz_audio_convert *ctx,
                             const int16_t *in, int16_t *out,
                             uint16_t in_frames, uint16_t out_frames)
{
	const struct zz_audio_convert_ratio *ratio = ctx->ratio;
	uint32_t n;

	if (ratio == NULL || ratio->phases == 0U) {
		/* Identity / off-table passthrough: copy what exists and
		 * zero the remainder so no stale bytes survive a short
		 * source (e.g. an off-table-rate period). */
		uint32_t frames = in_frames < out_frames ? in_frames
		                                         : out_frames;
		memcpy(out, in, (size_t)frames * 4U);
		if (out_frames > frames)
			memset(out + frames * 2U, 0,
			       (size_t)(out_frames - frames) * 4U);
		return;
	}

	for (n = 0U; n < out_frames; n++) {
		const uint32_t phase = ctx->pos_frac % ratio->phases;
		const int16_t *coef = ratio->coefs[phase];
		const uint32_t recip = ratio->recip[phase];
		const uint32_t taps = ratio->taps;
		const uint32_t base = ctx->pos_int;
		/* Taps on the current input, then on the history (the
		 * rest of the window is before the stream start). */
		const uint32_t from_in = base + 1U < taps ? base + 1U : taps;
		uint32_t from_history = taps - from_in;
		int64_t acc[2] = { 0, 0 };

		if (from_history > ctx->history_len)
			from_history = ctx->history_len;
		dot_input(coef, in, (int32_t)base, from_in, acc);
		if (from_history != 0U)
			dot_history(coef + from_in, ctx,
			            (int32_t)ctx->history_len - 1,
			            from_history, acc);
		out[n * 2U] = saturate_round(acc[0], recip, &ctx->clips);
		out[n * 2U + 1U] = saturate_round(acc[1], recip, &ctx->clips);

		ctx->pos_int += ratio->step_int;
		ctx->pos_frac += ratio->step_num;
		if (ctx->pos_frac >= ratio->step_den) {
			ctx->pos_frac -= ratio->step_den;
			ctx->pos_int += 1U;
		}
	}

	/* Rebase the rational position onto the next call's window. The
	 * exact per-period counts land the position on (in_frames, 0) by
	 * construction, so arming the next window at (0, 0) is the normal
	 * path. Any other landing is a contract violation (wrong frame
	 * counts, or in_frames == 0); re-arm at the window start rather
	 * than wrap into indices outside the caller's buffer. */
	ctx->pos_int = 0U;
	ctx->pos_frac = 0U;

	/* Carry the newest (taps-1) input frames into history. Frames older
	 * than the previous history drop off; frames before the stream
	 * start were zeros already. */
	{
		const uint16_t keep = ratio->taps - 1U;
		int32_t j;

		for (j = 0; j < (int32_t)keep; j++) {
			/* history[j] := frame (-(keep) + j) relative to the
			 * next call, i.e. index (in_frames - keep + j) into
			 * the current input when non-negative. */
			int32_t src = (int32_t)in_frames - (int32_t)keep + j;
			uint16_t ch;

			for (ch = 0U; ch < 2U; ch++) {
				if (src >= 0)
					ctx->history[ch][j] =
					    in[(uint32_t)src * 2U + ch];
				else
					ctx->history[ch][j] =
					    history_sample(ctx, ch, src);
			}
		}
		ctx->history_len = keep;
	}
}
