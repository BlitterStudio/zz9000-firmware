/*
 * ZZ9000AX capture-period conversion.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_capture.h"

static int16_t read_s16le(const uint8_t *sample)
{
	uint16_t value = (uint16_t)sample[0] | ((uint16_t)sample[1] << 8);

	return (int16_t)value;
}

static void write_s16be(uint8_t *sample, int16_t value)
{
	uint16_t encoded = (uint16_t)value;

	sample[0] = (uint8_t)(encoded >> 8);
	sample[1] = (uint8_t)encoded;
}

static int16_t interpolate(int16_t first, int16_t second, uint16_t fraction)
{
	int32_t delta = (int32_t)second - (int32_t)first;

	return (int16_t)((int32_t)first +
	                 (delta * (int32_t)fraction) / 32768);
}

uint16_t zz_audio_capture_convert(uint8_t *period, uint16_t output_frames)
{
	uint32_t output;

	if (output_frames == 0U ||
	    output_frames > ZZ_AUDIO_CAPTURE_INPUT_FRAMES)
		output_frames = ZZ_AUDIO_CAPTURE_INPUT_FRAMES;

	for (output = 0U; output < output_frames; output++) {
		uint32_t numerator = output * ZZ_AUDIO_CAPTURE_INPUT_FRAMES;
		uint32_t source = numerator / output_frames;
		uint32_t remainder = numerator % output_frames;
		uint16_t fraction =
		    (uint16_t)((remainder * 32768U) / output_frames);
		uint32_t next = source + 1U;
		const uint8_t *first;
		const uint8_t *second;
		int16_t left;
		int16_t right;

		if (next >= ZZ_AUDIO_CAPTURE_INPUT_FRAMES)
			next = source;

		first = period + source * 4U;
		second = period + next * 4U;
		left = interpolate(read_s16le(first), read_s16le(second),
		                   fraction);
		right = interpolate(read_s16le(first + 2U),
		                    read_s16le(second + 2U), fraction);

		write_s16be(period + output * 4U, left);
		write_s16be(period + output * 4U + 2U, right);
	}

	return output_frames;
}
