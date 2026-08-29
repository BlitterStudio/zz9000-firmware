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

static int check_decode_quantum(void)
{
	if (!audio_stream_decode_quantum_available(0U, 8192U, 4096U))
		return 0;
	if (!audio_stream_decode_quantum_available(1U, 8192U, 4096U))
		return 0;
	if (audio_stream_decode_quantum_available(2U, 8192U, 4096U))
		return 0;
	if (!audio_stream_decode_quantum_available(2U, 4096U, 4096U))
		return 0;
	return audio_stream_decode_quantum_available(3U, 2048U, 4096U);
}

static int check_feed_dirty_span(void)
{
	uint32_t offset;
	uint32_t length;

	if (audio_stream_feed_dirty_span(
		    100U, 200U, 0U, 1024U, &offset, &length))
		return 0;
	if (audio_stream_feed_dirty_span(
		    0U, 900U, 200U, 1024U, &offset, &length))
		return 0;
	if (!audio_stream_feed_dirty_span(
		    100U, 200U, 50U, 1024U, &offset, &length) ||
	    offset != 300U || length != 50U)
		return 0;
	if (!audio_stream_feed_dirty_span(
		    900U, 100U, 100U, 1024U, &offset, &length) ||
	    offset != 0U || length != 200U)
		return 0;
	return audio_stream_feed_dirty_span(
		       800U, 100U, 124U, 1024U, &offset, &length) &&
	       offset == 900U && length == 124U;
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
	if (!check_decode_quantum())
		return 2;
	if (!check_feed_dirty_span())
		return 3;
	if (!check_source_tail_state())
		return 4;
	if (!check_drain_input_state())
		return 5;
	if (!check_transient_drain_completion())
		return 6;
	if (!check_refill_gate())
		return 7;
	return 0;
}
