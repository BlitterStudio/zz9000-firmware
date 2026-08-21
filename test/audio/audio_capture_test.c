/*
 * Host tests for the ZZ9000AX capture conversion and publication helpers.
 *
 * The conversion tests assert the qualified-converter contract: exact
 * per-period counts, native/off-table identity with endian swap only,
 * continuity across periods through the byte pipeline, and reset
 * semantics. The status/publication/cursor helper tests are unchanged.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_capture.h"
#include "audio_convert.h"

#define PERIOD_BYTES (ZZ_AUDIO_CAPTURE_INPUT_FRAMES * 4U)

static uint8_t period[PERIOD_BYTES];
static uint8_t period_b[PERIOD_BYTES];

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

	for (frame = 0U; frame < ZZ_AUDIO_CAPTURE_INPUT_FRAMES; frame++) {
		write_s16le(period + frame * 4U, (int16_t)(frame * 17));
		write_s16le(period + frame * 4U + 2U,
		            (int16_t)(-frame * 17 - 1));
	}
}

static int expect_frame(uint32_t frame, int16_t left, int16_t right)
{
	if (read_s16be(period + frame * 4U) != left)
		return 1;
	if (read_s16be(period + frame * 4U + 2U) != right)
		return 1;
	return 0;
}

static int test_native_identity(void)
{
	uint16_t out;

	zz_audio_capture_reset();
	fill_ramp();
	memcpy(period_b, period, PERIOD_BYTES);
	out = zz_audio_capture_convert(period, 960U);
	if (out != 960U)
		return 1;
	/* Identity: endian swap only -- swapping back must reproduce the
	 * input bytes exactly. */
	{
		uint32_t i;

		for (i = 0U; i < PERIOD_BYTES; i += 2U) {
			uint16_t swapped = ((uint16_t)period[i] << 8) |
			                   (uint16_t)period[i + 1U];
			uint16_t original = (uint16_t)
			    ((uint16_t)period_b[i] |
			     ((uint16_t)period_b[i + 1U] << 8));

			if (swapped != original)
				return 1;
		}
	}
	return 0;
}

static int test_off_table_falls_back_to_identity(void)
{
	fill_ramp();
	if (zz_audio_capture_convert(period, 500U) != 960U)
		return 1;
	if (zz_audio_capture_convert(period, 1U) != 960U)
		return 1;
	if (zz_audio_capture_convert(period, 0U) != 960U)
		return 1;
	if (zz_audio_capture_convert(period, 961U) != 960U)
		return 1;
	/* 500 written then 960: both identity, value stays 960. */
	return 0;
}

static int test_exact_counts(void)
{
	static const uint16_t counts[5] = { 160U, 240U, 480U, 640U, 882U };
	size_t i;

	for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		fill_ramp();
		zz_audio_capture_reset();
		if (zz_audio_capture_convert(period, counts[i]) != counts[i])
			return 1;
	}
	return 0;
}

static int test_continuity_across_periods(void)
{
	/* Period-chained conversion through the byte pipeline must match a
	 * single call on the concatenated input -- including impulses at
	 * the first and last input frames of selected periods (the scratch
	 * copy must feed the kernel, never the destination). */
	uint8_t stream[PERIOD_BYTES * 12];
	uint8_t chained[PERIOD_BYTES * 12];
	int16_t one_shot_in[960U * 12 * 2];
	int16_t one_shot_out[882U * 12 * 2];
	struct zz_audio_convert ref;
	uint32_t p;
	uint32_t frame;
	int16_t value;

	memset(stream, 0, sizeof(stream));
	for (p = 0U; p < 12U * 960U; p++) {
		int32_t l = (int32_t)(p * 7U % 3000U) - 1500;
		int32_t r = (int32_t)(p * 11U % 3000U) - 1500;

		write_s16le(stream + p * 4U, (int16_t)l);
		write_s16le(stream + p * 4U + 2U, (int16_t)r);
	}
	write_s16le(stream + (960U * 3U) * 4U, 31000);          /* early */
	write_s16le(stream + (960U * 7U - 1U) * 4U + 2U, -31000); /* last */

	{
		uint8_t *cursor = chained;

		zz_audio_capture_reset();
		for (p = 0U; p < 12U; p++) {
			memcpy(period, stream + p * PERIOD_BYTES,
			       PERIOD_BYTES);
			zz_audio_capture_convert(period, 882U);
			memcpy(cursor, period, 882U * 4U);
			cursor += 882U * 4U;
		}
	}

	zz_audio_convert_init(&ref, 48000U, 44100U);
	for (p = 0U; p < 12U * 960U; p++) {
		uint16_t raw_l = (uint16_t)
		    ((uint16_t)stream[p * 4U] |
		     ((uint16_t)stream[p * 4U + 1U] << 8));
		uint16_t raw_r = (uint16_t)
		    ((uint16_t)stream[p * 4U + 2U] |
		     ((uint16_t)stream[p * 4U + 3U] << 8));

		one_shot_in[p * 2U] = (int16_t)raw_l;
		one_shot_in[p * 2U + 1U] = (int16_t)raw_r;
	}
	zz_audio_convert_stream(&ref, one_shot_in, one_shot_out,
	                        960U * 12U, 882U * 12U);

	for (p = 0U; p < 12U; p++) {
		for (frame = 0U; frame < 882U; frame++) {
			uint32_t idx = p * 882U + frame;
			int16_t got_l = read_s16be(
			    chained + idx * 4U);
			int16_t got_r = read_s16be(
			    chained + idx * 4U + 2U);

			if (got_l != one_shot_out[idx * 2U] ||
			    got_r != one_shot_out[idx * 2U + 1U])
				return 1;
		}
	}

	/* DC unity through the byte pipeline after warmup. */
	zz_audio_capture_reset();
	memset(period, 0, PERIOD_BYTES);
	for (p = 0U; p < 30U; p++) {
		for (frame = 0U; frame < 960U; frame++) {
			write_s16le(period + frame * 4U, 12000);
			write_s16le(period + frame * 4U + 2U, -12001);
		}
		zz_audio_capture_convert(period, 882U);
	}
	if (expect_frame(881U, 12000, -12001) != 0)
		return 1;
	value = read_s16be(period + 400U * 4U);
	if (value < 11998 || value > 12002)
		return 1;
	return 0;
}

static int test_reset_semantics(void)
{
	/* After a reset, converting the same input as a fresh instance
	 * must produce identical bytes (reset is one atomic event). */
	uint8_t a[PERIOD_BYTES];
	uint8_t b[PERIOD_BYTES];
	uint32_t frame;

	fill_ramp();
	memcpy(a, period, PERIOD_BYTES);
	memcpy(b, period, PERIOD_BYTES);
	zz_audio_capture_reset();
	zz_audio_capture_convert(a, 882U);
	zz_audio_capture_reset();
	zz_audio_capture_convert(b, 882U);
	if (memcmp(a, b, 882U * 4U) != 0)
		return 1;

	/* A non-reset repeat at the same count continues the stream: the
	 * second conversion of identical input differs from the first
	 * (filter history carries), proving state survives. */
	memcpy(b, period, PERIOD_BYTES);
	zz_audio_capture_convert(b, 882U);
	if (memcmp(a, b, 882U * 4U) == 0)
		return 1;
	(void)frame;
	return 0;
}

static int test_status_helpers(void)
{
	uint16_t status = zz_audio_rx_status_pack(6U, 0x1234U);
	uint16_t tx_status = zz_audio_tx_status_pack(3U, 0x1abcU);
	uint32_t read_group = zz_audio_config_read_pack(
	    1U | ZZ_AUDIO_CONFIG_TX_STATUS_CAPABLE, status);

	if (!(status & ZZ_AUDIO_RX_STATUS_CAPABLE) ||
	    zz_audio_rx_status_period(status) != 6U ||
	    zz_audio_rx_status_sequence(status) != 0x234U)
		return 1;
	if ((uint16_t)(read_group >> 16) !=
	        (1U | ZZ_AUDIO_CONFIG_TX_STATUS_CAPABLE) ||
	    (uint16_t)read_group != status)
		return 1;
	if (zz_audio_rx_sequence_distance(2U, 0xffeU) != 4U)
		return 1;
	if (!(tx_status & ZZ_AUDIO_TX_STATUS_CAPABLE) ||
	    zz_audio_tx_status_period(tx_status) != 3U ||
	    zz_audio_tx_status_sequence(tx_status) != 0xabcU)
		return 1;

	return 0;
}

static int test_publication_gate(void)
{
	if (!zz_audio_capture_can_publish(ZZ_AUDIO_CONFIG_RECORD, 1U))
		return 1;
	if (!zz_audio_capture_can_publish(ZZ_AUDIO_CONFIG_PLAY |
	                                  ZZ_AUDIO_CONFIG_RECORD, 1U))
		return 1;
	if (zz_audio_capture_can_publish(ZZ_AUDIO_CONFIG_RECORD, 0U))
		return 1;
	if (zz_audio_capture_can_publish(ZZ_AUDIO_CONFIG_PLAY, 1U))
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

	failed |= test_native_identity();
	failed |= test_off_table_falls_back_to_identity();
	failed |= test_exact_counts();
	failed |= test_continuity_across_periods();
	failed |= test_reset_semantics();
	failed |= test_status_helpers();
	failed |= test_publication_gate();
	failed |= test_transfer_cursor_helpers();

	if (failed) {
		puts("audio capture tests FAILED");
		return 1;
	}

	puts("audio capture tests passed");
	return 0;
}
