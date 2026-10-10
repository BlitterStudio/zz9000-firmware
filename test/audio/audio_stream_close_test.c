/*
 * Background close of FLAC/Vorbis streams whose decoder heap belongs to
 * core 1 (audio_stream_close.h): Close never answers BUSY, the core-1 task
 * only releases the heap, and only core 0 clears the slot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "audio_stream_close.h"

static int failures;

#define EXPECT(cond, what) do { if (!(cond)) { failures++; \
	printf("FAIL: %s\n", what); } } while (0)

/* Arguments: release_queued, release_task_live, io_tasks_live,
 * core1_available, core1_affine, holds_decoder. */
static enum audio_stream_close_action step(int rq, int rl, int io, int c1,
                                           int aff, int heap)
{
	return audio_stream_close_step(rq, rl, io, c1, aff, heap);
}

static void check_close_path(void)
{
	EXPECT(audio_stream_close_deferred(1, 1, 1), "native core-1 stream: background");
	EXPECT(!audio_stream_close_deferred(0, 1, 1), "MP3 keeps the synchronous close");
	EXPECT(!audio_stream_close_deferred(1, 0, 1), "core-0 stream: synchronous");
	EXPECT(!audio_stream_close_deferred(1, 1, 0), "core 1 down: synchronous");
}

static void check_lifecycle(void)
{
	/* The client's own FEED/READ still queued or running: wait. */
	EXPECT(step(0, 0, 1, 1, 1, 1) == AUDIO_STREAM_CLOSE_WAIT, "wait for io tasks");
	EXPECT(step(0, 0, 1, 1, 1, 0) == AUDIO_STREAM_CLOSE_WAIT,
	       "wait for io tasks even before the decoder exists");
	/* Heap on core 1: queue the release (retried while the queue is full,
	 * because release_queued stays 0 until enqueue succeeds). */
	EXPECT(step(0, 0, 0, 1, 1, 1) == AUDIO_STREAM_CLOSE_QUEUE, "queue release");
	/* Release task queued or running: the slot stays reserved. */
	EXPECT(step(1, 1, 0, 1, 1, 1) == AUDIO_STREAM_CLOSE_WAIT, "release in flight");
	/* Task left the queue (DONE or FAILED): core 0 clears the slot. */
	EXPECT(step(1, 0, 0, 1, 1, 0) == AUDIO_STREAM_CLOSE_FREE, "free after release");
	EXPECT(step(1, 0, 0, 0, 1, 0) == AUDIO_STREAM_CLOSE_FREE,
	       "stranded release failed by the single-core fallback");
	/* Release task failed on a core-1 fault before finishing: the
	 * reclaim must drop the heap before core 0 touches the slot. */
	EXPECT(step(1, 0, 0, 1, 1, 1) == AUDIO_STREAM_CLOSE_WAIT,
	       "failed release waits for the reclaim");
	/* No heap was ever created (closed before the first feed decoded). */
	EXPECT(step(0, 0, 0, 1, 1, 0) == AUDIO_STREAM_CLOSE_FREE, "nothing to release");
}

static void check_core1_loss(void)
{
	/* Core 1 died holding the heap: never free core-1 tracked blocks from
	 * core 0; the poison/reclaim pass drops the pointers first. */
	EXPECT(step(0, 0, 0, 0, 1, 1) == AUDIO_STREAM_CLOSE_WAIT, "wait for reclaim");
	EXPECT(step(0, 0, 0, 0, 1, 0) == AUDIO_STREAM_CLOSE_FREE, "free after reclaim");
}

int main(void)
{
	check_close_path();
	check_lifecycle();
	check_core1_loss();
	if (failures) {
		printf("audio_stream_close_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_stream_close_test: all passed\n");
	return 0;
}
