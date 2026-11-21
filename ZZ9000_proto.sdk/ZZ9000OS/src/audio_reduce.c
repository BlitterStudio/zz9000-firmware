/*
 * Audio stream reducer; see audio_reduce.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "audio_reduce.h"

uint32_t audio_reduce_divisor(uint32_t stream_hz, uint32_t output_hz)
{
	uint32_t d;

	if (output_hz == 0U)
		return 1U;
	for (d = 1U; d <= 2U; d *= 2U) {
		if ((uint64_t)stream_hz <= (uint64_t)output_hz * d)
			return d;
	}
	return 4U;
}

int audio_reduce_request(struct audio_reduce *r, uint32_t output_hz,
                         uint32_t output_channels)
{
	if (output_channels > AUDIO_REDUCE_MAX_CHANNELS)
		return -1;
	memset(r, 0, sizeof(*r));
	r->req_hz = output_hz;
	r->req_channels = output_channels;
	return 0;
}

int audio_reduce_set_input(struct audio_reduce *r, uint32_t hz,
                           uint32_t channels)
{
	uint32_t d;
	uint32_t sh;

	if (hz == 0U || channels == 0U)
		return 0;
	if (r->divisor != 0U && r->in_hz == hz && r->in_channels == channels)
		return 0;
	memset(r->stage, 0, sizeof(r->stage));
	r->in_hz = hz;
	r->in_channels = channels;
	if (channels > AUDIO_REDUCE_MAX_CHANNELS) {
		r->divisor = 1U;
		r->out_hz = hz;
		r->out_channels = channels;
		return 1;
	}
	d = audio_reduce_divisor(hz, r->req_hz);
	r->divisor = d;
	/* hz / d rounded to nearest, halves up; d is a power of two. */
	sh = d >> 1;
	r->out_hz = (hz >> sh) +
	            ((((hz & ((1U << sh) - 1U)) << 1) >= (1U << sh)) ? 1U : 0U);
	r->out_channels = r->req_channels != 0U ? r->req_channels : channels;
	return 1;
}

int audio_reduce_active(const struct audio_reduce *r)
{
	return r->divisor > 1U ||
	       (r->divisor != 0U && r->out_channels != r->in_channels);
}

uint32_t audio_reduce_raw_room(const struct audio_reduce *r,
                               uint32_t ring_free)
{
	/* Before the decoder format is known a stereo request may still
	 * expand a mono source, so size for it. */
	if (r->divisor == 0U) {
		return r->req_channels == 2U ? ring_free / 2U : ring_free;
	}
	if (r->divisor == 1U && r->in_channels == 1U && r->out_channels == 2U)
		return ring_free / 2U;
	return ring_free;
}

uint32_t audio_reduce_raw_bytes(const struct audio_reduce *r,
                                uint32_t ring_bytes)
{
	/* divisor is 1, 2 or 4 and the channel counts are 1 or 2, so the
	 * scaling is shifts only. */
	uint64_t raw = (uint64_t)ring_bytes;

	if (r->divisor == 0U)
		return ring_bytes;
	raw <<= r->divisor >> 1;
	if (r->in_channels > r->out_channels)
		raw <<= 1;
	else if (r->in_channels < r->out_channels)
		raw >>= 1;
	return raw > 0xFFFFFFFFU ? 0xFFFFFFFFU : (uint32_t)raw;
}

static int16_t audio_reduce_sat(int64_t v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (int16_t)v;
}

/* Push one frame into a stage; returns 1 and fills out[] on every second
 * push. The window w[0..TAPS-1] runs oldest to newest, centre at w[29]. */
static int audio_reduce_stage_push(struct audio_reduce_stage *s,
                                   uint32_t channels, const int16_t *in,
                                   int16_t *out)
{
	const uint32_t centre = (AUDIO_REDUCE_TAPS - 1U) / 2U;
	uint32_t pos = s->pos;
	uint32_t c;
	uint32_t i;

	for (c = 0U; c < channels; c++) {
		s->line[c][pos] = in[c];
		s->line[c][pos + AUDIO_REDUCE_TAPS] = in[c];
	}
	if (++pos == AUDIO_REDUCE_TAPS)
		pos = 0U;
	s->pos = (uint8_t)pos;
	s->phase ^= 1U;
	if (s->phase != 0U)
		return 0;
	for (c = 0U; c < channels; c++) {
		const int16_t *w = &s->line[c][pos];
		int64_t acc = (int64_t)AUDIO_REDUCE_CENTRE * w[centre];

		for (i = 0U; i < AUDIO_REDUCE_SIDES; i++) {
			acc += (int64_t)audio_reduce_side[i] *
			       ((int32_t)w[centre - (2U * i + 1U)] +
			        (int32_t)w[centre + (2U * i + 1U)]);
		}
		out[c] = audio_reduce_sat(
			(acc + ((int64_t)1 << (AUDIO_REDUCE_Q - 1))) >>
			AUDIO_REDUCE_Q);
	}
	return 1;
}

uint32_t audio_reduce_run(struct audio_reduce *r, const int16_t *in,
                          uint32_t frames, int16_t *out)
{
	const uint32_t in_ch = r->in_channels;
	const uint32_t out_ch = r->out_channels;
	const int mix = in_ch == 2U && out_ch == 1U;
	const uint32_t filt_ch = mix ? 1U : in_ch;
	uint32_t produced = 0U;
	uint32_t f;

	for (f = 0U; f < frames; f++) {
		int16_t x[AUDIO_REDUCE_MAX_CHANNELS] = { 0, 0 };
		int16_t y[AUDIO_REDUCE_MAX_CHANNELS] = { 0, 0 };
		uint32_t c;

		if (mix) {
			/* (L + R) / 2, rounding halves up. */
			x[0] = (int16_t)(((int32_t)in[2U * f] +
			                  (int32_t)in[2U * f + 1U] + 1) >> 1);
		} else {
			for (c = 0U; c < in_ch; c++)
				x[c] = in[in_ch * f + c];
		}
		if (r->divisor >= 2U) {
			if (!audio_reduce_stage_push(&r->stage[0], filt_ch, x,
			                             y))
				continue;
			if (r->divisor == 4U) {
				int16_t z[AUDIO_REDUCE_MAX_CHANNELS] = { 0, 0 };

				if (!audio_reduce_stage_push(&r->stage[1],
				                             filt_ch, y, z))
					continue;
				y[0] = z[0];
				y[1] = z[1];
			}
		} else {
			y[0] = x[0];
			y[1] = x[1];
		}
		if (out_ch == 2U && filt_ch == 1U) {
			out[2U * produced] = y[0];
			out[2U * produced + 1U] = y[0];
		} else {
			for (c = 0U; c < out_ch; c++)
				out[out_ch * produced + c] = y[c];
		}
		produced++;
	}
	return produced;
}
