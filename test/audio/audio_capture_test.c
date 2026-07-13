/*
 * Host tests for the ZZ9000AX capture conversion and publication helpers.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_capture.h"

#define PERIOD_BYTES (ZZ_AUDIO_CAPTURE_INPUT_FRAMES * 4U)

static uint8_t period[PERIOD_BYTES];

static void write_s16le(uint8_t *sample, int16_t value)
{
	uint16_t encoded = (uint16_t)value;

	sample[0] = (uint8_t)encoded;
	sample[1] = (uint8_t)(encoded >> 8);
}

static int16_t read_s16be(const uint8_t *sample)
{
	return (int16_t)(((uint16_t)sample[0] << 8) | sample[1]);
}

static void fill_ramp(void)
{
	uint32_t frame;

	for (frame = 0; frame < ZZ_AUDIO_CAPTURE_INPUT_FRAMES; frame++) {
		write_s16le(period + frame * 4U, (int16_t)(frame - 480));
		write_s16le(period + frame * 4U + 2U,
		            (int16_t)(1000 - (int32_t)frame));
	}
}

static int expect_frame(uint32_t frame, int16_t left, int16_t right)
{
	int16_t actual_left = read_s16be(period + frame * 4U);
	int16_t actual_right = read_s16be(period + frame * 4U + 2U);

	if (actual_left != left || actual_right != right) {
		printf("frame %u: got %d/%d, expected %d/%d\n",
		       frame, actual_left, actual_right, left, right);
		return 1;
	}

	return 0;
}

static int test_native_conversion(void)
{
	fill_ramp();
	if (zz_audio_capture_convert(period, 960U) != 960U)
		return 1;

	return expect_frame(0U, -480, 1000) |
	       expect_frame(480U, 0, 520) |
	       expect_frame(959U, 479, 41);
}

static int test_44100_conversion(void)
{
	fill_ramp();
	if (zz_audio_capture_convert(period, 882U) != 882U)
		return 1;

	/* Output frame 441 maps exactly to source frame 480. */
	return expect_frame(0U, -480, 1000) |
	       expect_frame(441U, 0, 520);
}

static int test_8000_conversion(void)
{
	fill_ramp();
	if (zz_audio_capture_convert(period, 160U) != 160U)
		return 1;

	/* 48 kHz to 8 kHz is an exact six-to-one downsample. */
	return expect_frame(1U, -474, 994) |
	       expect_frame(159U, 474, 46);
}

static int test_invalid_frame_count_falls_back(void)
{
	fill_ramp();
	return zz_audio_capture_convert(period, 0U) != 960U;
}

static int test_status_helpers(void)
{
	uint16_t status = zz_audio_rx_status_pack(6U, 0x1234U);

	if (!(status & ZZ_AUDIO_RX_STATUS_CAPABLE) ||
	    zz_audio_rx_status_period(status) != 6U ||
	    zz_audio_rx_status_sequence(status) != 0x234U)
		return 1;
	if (zz_audio_rx_sequence_distance(2U, 0xffeU) != 4U)
		return 1;

	return 0;
}

static int test_transfer_cursor_helpers(void)
{
	if (zz_audio_capture_completed_period(0U, PERIOD_BYTES) != 7U)
		return 1;
	if (zz_audio_capture_completed_period(PERIOD_BYTES, PERIOD_BYTES) != 0U)
		return 1;
	if (zz_audio_capture_completed_period(PERIOD_BYTES * 9U + 12U,
	                                      PERIOD_BYTES) != 0U)
		return 1;
	if (zz_audio_capture_period_distance(1U, 7U) != 2U)
		return 1;
	if (ZZ_AUDIO_CAPTURE_RESIDENT_PERIODS != 7U)
		return 1;

	return 0;
}

int main(void)
{
	int failed = 0;

	failed |= test_native_conversion();
	failed |= test_44100_conversion();
	failed |= test_8000_conversion();
	failed |= test_invalid_frame_count_falls_back();
	failed |= test_status_helpers();
	failed |= test_transfer_cursor_helpers();

	if (failed) {
		puts("audio capture tests FAILED");
		return 1;
	}

	puts("audio capture tests passed");
	return 0;
}
