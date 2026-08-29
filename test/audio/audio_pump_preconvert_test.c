#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_pump_preconvert.h"
#include "sdk_mailbox.h"

#define RING_BYTES (AUDIO_PUMP_PRECONVERT_PERIOD_BYTES * 8U)

static int failures;

static void check(int condition, const char *label)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", label);
		failures++;
	}
}

static void test_44100_matches_converter(void)
{
	struct audio_pump_preconvert state;
	struct audio_pump_preconvert_source source;
	struct zz_audio_convert reference;
	uint8_t input[882U * 4U];
	uint8_t ring[RING_BYTES];
	int16_t expected[960U * 2U];
	uint32_t consumed = 0U;
	uint32_t i;

	for (i = 0U; i < 882U * 2U; i++)
		((int16_t *)input)[i] = (int16_t)(i * 17U - 12000U);
	memset(&source, 0, sizeof(source));
	source.ring = input;
	source.capacity = sizeof(input);
	source.produced = sizeof(input);
	source.sample_rate = 44100U;
	source.channels = 2U;
	source.sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	audio_pump_preconvert_reset(&state, ring, sizeof(ring));
	check(audio_pump_preconvert_fill(&state, &source, &consumed) == 1,
	      "44.1 fill");
	zz_audio_convert_init(&reference, 44100U, 48000U);
	zz_audio_convert_stream(&reference, (const int16_t *)input, expected,
	                        882U, 960U);
	check(consumed == sizeof(input), "44.1 consumes one source period");
	check(audio_pump_preconvert_used(&state) ==
	          AUDIO_PUMP_PRECONVERT_PERIOD_BYTES,
	      "44.1 publishes one output period");
	check(memcmp(ring, expected, sizeof(expected)) == 0,
	      "44.1 output matches qualified converter");
	check(audio_pump_preconvert_stage(
	          &state, AUDIO_PUMP_PRECONVERT_PERIOD_BYTES),
	      "stage converted period");
	check(audio_pump_preconvert_used(&state) == 0U,
	      "stage retires converted period");
}

static void test_48000_is_bit_exact(void)
{
	struct audio_pump_preconvert state;
	struct audio_pump_preconvert_source source;
	uint8_t input[AUDIO_PUMP_PRECONVERT_PERIOD_BYTES];
	uint8_t ring[RING_BYTES];
	uint32_t consumed = 0U;
	uint32_t i;

	for (i = 0U; i < sizeof(input); i++)
		input[i] = (uint8_t)(i * 29U + 3U);
	memset(&source, 0, sizeof(source));
	source.ring = input;
	source.capacity = sizeof(input);
	source.produced = sizeof(input);
	source.sample_rate = 48000U;
	source.channels = 2U;
	source.sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	audio_pump_preconvert_reset(&state, ring, sizeof(ring));
	check(audio_pump_preconvert_fill(&state, &source, &consumed) == 1,
	      "48 fill");
	check(consumed == sizeof(input), "48 consumes one source period");
	check(memcmp(ring, input, sizeof(input)) == 0,
	      "48 bypass remains bit exact");
}

static void test_shortage_waits_and_tail_drains(void)
{
	struct audio_pump_preconvert state;
	struct audio_pump_preconvert_source source;
	uint8_t input[100U * 4U];
	uint8_t ring[RING_BYTES];
	uint32_t consumed = 0U;

	memset(input, 0x35, sizeof(input));
	memset(&source, 0, sizeof(source));
	source.ring = input;
	source.capacity = sizeof(input);
	source.produced = sizeof(input);
	source.sample_rate = 44100U;
	source.channels = 2U;
	source.sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	audio_pump_preconvert_reset(&state, ring, sizeof(ring));
	check(audio_pump_preconvert_fill(&state, &source, &consumed) == 0,
	      "short live source waits");
	check(consumed == 0U && audio_pump_preconvert_used(&state) == 0U,
	      "short live source is untouched");
	source.done = 1;
	check(audio_pump_preconvert_fill(&state, &source, &consumed) == 1,
	      "short final tail converts zero padded");
	check(consumed == sizeof(input), "short final tail consumed");
}

static void test_cursor_wrap_rebases_both_cursors(void)
{
	struct audio_pump_preconvert state;
	struct audio_pump_preconvert_source source;
	uint8_t input[AUDIO_PUMP_PRECONVERT_PERIOD_BYTES];
	uint8_t ring[RING_BYTES];
	uint32_t consumed = 0U;
	uint32_t base;
	uint32_t produced_before;
	uint32_t staged_before;

	memset(input, 0x5a, sizeof(input));
	memset(&source, 0, sizeof(source));
	source.ring = input;
	source.capacity = sizeof(input);
	source.produced = sizeof(input);
	source.sample_rate = 48000U;
	source.channels = 2U;
	source.sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	audio_pump_preconvert_reset(&state, ring, sizeof(ring));

	/* Park the cursors inside the wrap margin with staged lagging
	 * produced by one period (the replay-keep bound) (PR #88 review: the ring capacity does
	 * not divide 2^32, so a natural wrap would tear the modulo
	 * positions apart). */
	base = 0U - 15U * AUDIO_PUMP_PRECONVERT_PERIOD_BYTES;
	base -= base % sizeof(ring);
	state.produced = base + 13U * AUDIO_PUMP_PRECONVERT_PERIOD_BYTES;
	state.staged = state.produced -
		AUDIO_PUMP_PRECONVERT_PERIOD_BYTES;
	produced_before = state.produced;
	staged_before = state.staged;

	check(audio_pump_preconvert_fill(&state, &source, &consumed) == 1,
	      "wrap fill converts");
	/* produced either advanced into the rebase window and rebased, or
	 * crossed normally; either way the invariant that matters is that
	 * produced - staged keeps advancing by exactly one period and the
	 * modulo position continues the sequence. */
	check(state.produced - state.staged ==
	      produced_before + AUDIO_PUMP_PRECONVERT_PERIOD_BYTES -
	      staged_before,
	      "wrap keeps used-bytes advancing one period");
	check(state.produced % sizeof(ring) ==
	      (produced_before +
	       AUDIO_PUMP_PRECONVERT_PERIOD_BYTES) % sizeof(ring),
	      "wrap preserves produced modulo position");
	check(state.staged % sizeof(ring) == staged_before % sizeof(ring),
	      "wrap preserves staged modulo position");
	check(state.produced <= produced_before ||
	      state.produced ==
	      produced_before + AUDIO_PUMP_PRECONVERT_PERIOD_BYTES,
	      "wrap produced either rebased or advanced one period");
}
int main(void)
{
	test_44100_matches_converter();
	test_48000_is_bit_exact();
	test_shortage_waits_and_tail_drains();
	test_cursor_wrap_rebases_both_cursors();
	test_cursor_wrap_rebases_both_cursors();
	if (failures)
		return 1;
	printf("audio_pump_preconvert_test: all checks passed\n");
	return 0;
}
