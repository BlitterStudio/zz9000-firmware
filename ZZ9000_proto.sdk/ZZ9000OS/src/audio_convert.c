/*
 * ZZ9000AX qualified fixed-point audio conversion core.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_convert.h"

#include <string.h>

static const struct zz_audio_convert_ratio *ratio_for(uint32_t in_rate,
                                                      uint32_t out_rate)
{
	switch (in_rate) {
	case 8000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_8000_48000;
		break;
	case 12000U:
		if (out_rate == 48000U)
			return &zz_audio_convert_ratio_12000_48000;
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
 * (16384/scale) before the final round-shift. SMLAL makes the 64-bit
 * accumulate native on the Cortex-A9. */
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
		const uint16_t taps = ratio->taps;
		int32_t base = (int32_t)ctx->pos_int;
		uint16_t ch;

		for (ch = 0U; ch < 2U; ch++) {
			int64_t acc = 0;
			int32_t k;

			for (k = 0; k < (int32_t)taps; k++) {
				int32_t idx = base - k;
				int16_t sample;

				if (idx >= 0)
					sample = in[(uint32_t)idx * 2U + ch];
				else
					sample = history_sample(ctx, ch,
					                        idx);
				acc += (int64_t)coef[k] * (int64_t)sample;
			}
			out[n * 2U + ch] =
			    saturate_round(acc, recip, &ctx->clips);
		}

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
