/*
 * Resumable audio-stream starvation drain contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>

#include "audio_stream_drain.h"

#define MIN_INPUT_BYTES 4096U

static int check_decode_gate(void)
{
	if (audio_stream_decode_may_run(1024U, MIN_INPUT_BYTES, 0, 0))
		return 0;
	if (!audio_stream_decode_may_run(1024U, MIN_INPUT_BYTES, 0, 1))
		return 0;
	if (!audio_stream_decode_may_run(1024U, MIN_INPUT_BYTES, 1, 0))
		return 0;
	return audio_stream_decode_may_run(
		MIN_INPUT_BYTES, MIN_INPUT_BYTES, 0, 0);
}

static int check_source_tail_state(void)
{
	if (audio_stream_source_tail_ready(0, 17U, 0, 0))
		return 0;
	if (!audio_stream_source_tail_ready(0, 17U, 1, 1))
		return 0;
	if (!audio_stream_source_tail_ready(1, 0U, 0, 0))
		return 0;
	return !audio_stream_source_tail_ready(1, 17U, 0, 0);
}

static int check_drain_input_state(void)
{
	if (audio_stream_drain_input_done(0, 1, 0U, 0))
		return 0;
	if (!audio_stream_drain_input_done(1, 1, 17U, 0))
		return 0;
	if (!audio_stream_drain_input_done(1, 0, 0U, 0))
		return 0;
	if (!audio_stream_drain_input_done(1, 0, 17U, 1))
		return 0;
	return !audio_stream_drain_input_done(1, 0, 17U, 0);
}

static int check_transient_drain_completion(void)
{
	if (audio_stream_transient_drained(0, 1, 0U, 0))
		return 0;
	if (audio_stream_transient_drained(1, 0, 0U, 0))
		return 0;
	if (audio_stream_transient_drained(1, 1, 4U, 0))
		return 0;
	if (audio_stream_transient_drained(1, 1, 0U, 1))
		return 0;
	return audio_stream_transient_drained(1, 1, 0U, 0);
}

static int check_refill_gate(void)
{
	if (!audio_stream_refill_may_run(17U, 0, 0, 0, 4U, 4U))
		return 0;
	if (audio_stream_refill_may_run(17U, 0, 0, 1, 4U, 4U))
		return 0;
	if (audio_stream_refill_may_run(17U, 0, 1, 0, 4U, 4U))
		return 0;
	return !audio_stream_refill_may_run(0U, 0, 0, 0, 4U, 4U);
}

int main(void)
{
	if (!check_decode_gate())
		return 1;
	if (!check_source_tail_state())
		return 2;
	if (!check_drain_input_state())
		return 3;
	if (!check_transient_drain_completion())
		return 4;
	if (!check_refill_gate())
		return 5;
	return 0;
}
