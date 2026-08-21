/*
 * Host tests for the AHI per-period playback rate helpers and the
 * per-period call geometry through the shared conversion kernel.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_convert.h"
#include "audio_playback_rate.h"

static int failures;

static void check(int ok, const char *name)
{
	if (!ok) {
		failures++;
		printf("FAILED: %s\n", name);
	}
}

static int test_rate_derivation(void)
{
	static const struct {
		uint16_t buf_samples;
		uint32_t rate;
	} map[] = {
	    { 160U, 8000U },  { 240U, 12000U }, { 480U, 24000U },
	    { 640U, 32000U }, { 882U, 44100U }, { 960U, 48000U },
	};
	size_t i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		check(zz_audio_playback_rate(map[i].buf_samples) ==
		          map[i].rate,
		      "rate derivation");
	return failures == 0;
}

static int test_rate_changed_decision(void)
{
	/* First call after a full reset (last rate 0) initializes once. */
	check(zz_audio_playback_rate_changed(44100U, 0U) == 1,
	      "first use resets");
	/* Same rate continues the instance without touching it. */
	check(zz_audio_playback_rate_changed(44100U, 44100U) == 0,
	      "same rate continues");
	/* AHI SetAudioMode mid-stream resets exactly once. */
	check(zz_audio_playback_rate_changed(32000U, 44100U) == 1,
	      "rate change resets");
	check(zz_audio_playback_rate_changed(32000U, 32000U) == 0,
	      "reset fires once");
	return failures == 0;
}

static int test_per_period_geometry_is_exact(void)
{
	/* The AHI path converts rate/50 input frames to 960 output frames
	 * every period. Chained per-period calls through the shared kernel
	 * must be bit-identical to one big conversion (the AE2 property,
	 * proven at the module both paths delegate to). */
	static const uint16_t rates[5] = { 160U, 240U, 480U, 640U, 882U };
	int16_t chunked[960 * 2];
	int16_t whole[882 * 60 * 2];
	int16_t whole_out[960 * 60 * 2];
	int16_t chunked_out[960 * 60 * 2];
	uint32_t r, i, p;

	for (r = 0; r < 5; r++) {
		struct zz_audio_convert a, b;
		uint16_t in_n = rates[r];

		for (i = 0; i < (uint32_t)in_n * 60U * 2U; i++)
			whole[i] = (int16_t)(i * 13U + (i >> 3));

		zz_audio_convert_init(&a, (uint32_t)in_n * 50U, 48000U);
		zz_audio_convert_init(&b, (uint32_t)in_n * 50U, 48000U);
		for (p = 0; p < 60U; p++) {
			zz_audio_convert_stream(&a,
			        whole + (size_t)p * in_n * 2U, chunked,
			        in_n, 960U);
			memcpy(chunked_out + (size_t)p * 960U * 2U, chunked,
			       sizeof(chunked));
		}
		zz_audio_convert_stream(&b, whole, whole_out,
		                        (uint16_t)(in_n * 60U), 960U * 60U);
		check(memcmp(chunked_out, whole_out,
		             sizeof(chunked_out)) == 0,
		      "per-period geometry chains exactly");
	}
	return failures == 0;
}

int main(void)
{
	test_rate_derivation();
	test_rate_changed_decision();
	test_per_period_geometry_is_exact();

	if (failures == 0) {
		printf("audio_playback_rate_test: all tests passed\n");
		return 0;
	}
	printf("audio_playback_rate_test: %d failure(s)\n", failures);
	return 1;
}
