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

static int test_bypass_transition_rearms(void)
{
	/* 44.1 -> 48 (bypass) -> 44.1: the site resets the converter on
	 * the 48-kHz transition, so the return must convert identically
	 * to a fresh instance (no stale history from before the bypass).
	 * The site rule: entering the bypass retires the converter. */
	struct zz_audio_convert a, fresh;
	int16_t in1[882U * 2];
	int16_t out_return[960U * 2];
	int16_t out_fresh[960U * 2];
	uint32_t i;

	for (i = 0U; i < 882U * 2U; i++)
		in1[i] = (int16_t)(i * 29U % 9000U - 4500);

	/* Session at 44.1 kHz converts a few periods... */
	zz_audio_convert_init(&a, 44100U, 48000U);
	for (i = 0U; i < 5U; i++)
		zz_audio_convert_stream(&a, in1, out_return, 882U, 960U);
	/* ...then the bypass: the site resets and re-inits on return. */
	zz_audio_convert_reset(&a);
	zz_audio_convert_init(&a, 44100U, 48000U);
	zz_audio_convert_stream(&a, in1, out_return, 882U, 960U);

	zz_audio_convert_init(&fresh, 44100U, 48000U);
	zz_audio_convert_stream(&fresh, in1, out_fresh, 882U, 960U);

	check(memcmp(out_return, out_fresh, 960U * 4U) == 0,
	      "bypass transition re-arms converter");
	return failures == 0;
}

int main(void)
{
	test_rate_derivation();
	test_per_period_geometry_is_exact();
	test_bypass_transition_rearms();

	if (failures == 0) {
		printf("audio_playback_rate_test: all tests passed\n");
		return 0;
	}
	printf("audio_playback_rate_test: %d failure(s)\n", failures);
	return 1;
}
