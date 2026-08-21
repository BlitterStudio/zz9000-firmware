/*
 * ZZ9000AX capture-period conversion.
 *
 * The qualified path converts each completed 48 kHz S16LE stereo DMA
 * period through the shared fixed-point kernel (audio_convert.c) with a
 * CPU-side scratch copy -- a long kernel reads inputs that early
 * outputs would overwrite in place. Filter history carries across
 * periods inside the private instance; the exact per-period counts keep
 * the rational phase landing on every period boundary. Off-table frame
 * counts fall back to native 48 kHz identity (one endian swap), matching
 * the register contract's six supported counts.
 *
 * Reset discipline (audio_convert instance + last-count tracker): record
 * start, capture frame-count VALUE change, or RX buffer reassignment.
 * Routine same-value frame-count writes -- the AHI driver issues one per
 * playback period -- must not reset, or a full-duplex recording never
 * reaches steady state.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_capture.h"

#include <string.h>

#include "audio_convert.h"

static struct zz_audio_convert zz_audio_capture_ctx;
static uint16_t zz_audio_capture_last_frames;

/* Scratch: in = native-endian frames decoded from the S16LE period;
 * out = converter output before the S16BE write-back. */
static int16_t zz_audio_capture_in[ZZ_AUDIO_CAPTURE_INPUT_FRAMES * 2U];
static int16_t zz_audio_capture_out[ZZ_AUDIO_CAPTURE_INPUT_FRAMES * 2U];

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

static uint16_t identity_be(uint8_t *period)
{
	uint16_t frame;

	for (frame = 0U; frame < ZZ_AUDIO_CAPTURE_INPUT_FRAMES; frame++) {
		int16_t left = read_s16le(period + frame * 4U);
		int16_t right = read_s16le(period + frame * 4U + 2U);

		write_s16be(period + frame * 4U, left);
		write_s16be(period + frame * 4U + 2U, right);
	}
	return ZZ_AUDIO_CAPTURE_INPUT_FRAMES;
}

static int supported_count(uint16_t frames)
{
	switch (frames) {
	case 160U:
	case 240U:
	case 480U:
	case 640U:
	case 882U:
		return 1;
	default:
		return 0;
	}
}

void zz_audio_capture_reset(void)
{
	zz_audio_capture_last_frames = 0U;
	zz_audio_convert_reset(&zz_audio_capture_ctx);
}

uint16_t zz_audio_capture_convert(uint8_t *period, uint16_t output_frames)
{
	uint32_t rate;
	uint16_t frame;

	if (output_frames == 0U ||
	    output_frames > ZZ_AUDIO_CAPTURE_INPUT_FRAMES)
		output_frames = ZZ_AUDIO_CAPTURE_INPUT_FRAMES;

	/* Native 48 kHz and every off-table count (register contract
	 * allows 1-960) convert as identity: endian swap only. */
	if (output_frames == ZZ_AUDIO_CAPTURE_INPUT_FRAMES ||
	    !supported_count(output_frames))
		return identity_be(period);

	rate = (uint32_t)output_frames * 50U;
	if (output_frames != zz_audio_capture_last_frames) {
		zz_audio_capture_last_frames = output_frames;
		zz_audio_convert_init(&zz_audio_capture_ctx, 48000U, rate);
	}

	for (frame = 0U; frame < ZZ_AUDIO_CAPTURE_INPUT_FRAMES; frame++) {
		zz_audio_capture_in[frame * 2U] =
		    read_s16le(period + frame * 4U);
		zz_audio_capture_in[frame * 2U + 1U] =
		    read_s16le(period + frame * 4U + 2U);
	}

	zz_audio_convert_stream(&zz_audio_capture_ctx, zz_audio_capture_in,
	                        zz_audio_capture_out,
	                        ZZ_AUDIO_CAPTURE_INPUT_FRAMES,
	                        output_frames);

	for (frame = 0U; frame < output_frames; frame++) {
		write_s16be(period + frame * 4U,
		            zz_audio_capture_out[frame * 2U]);
		write_s16be(period + frame * 4U + 2U,
		            zz_audio_capture_out[frame * 2U + 1U]);
	}
	return output_frames;
}
