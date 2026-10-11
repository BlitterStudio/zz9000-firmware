#ifndef AUDIO_STREAM_CLOSE_H
#define AUDIO_STREAM_CLOSE_H

/*
 * Background close of native codec (FLAC/Vorbis) audio streams whose
 * decoder heap belongs to core 1.
 *
 * ZZ9KAudioStreamClose() on such a stream never answers BUSY: the handler
 * marks the slot closing (its id stays set, so it cannot be reallocated, but
 * every session lookup treats it as gone) and completes OK at once. The
 * core-0 main loop then retires it with audio_stream_close_step(): it waits
 * for the stream's own FEED/READ tasks, queues an internal core-1 task that
 * only releases the decoder heap (retrying while the task queue is full),
 * and once that task has left the queue core 0 alone clears the slot. Core 1
 * never writes the slot's id, so the core-0 allocator cannot race a slot
 * being cleared.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

enum audio_stream_close_action {
	AUDIO_STREAM_CLOSE_WAIT = 0,     /* try again next main-loop pass */
	AUDIO_STREAM_CLOSE_QUEUE,        /* enqueue the core-1 release task */
	AUDIO_STREAM_CLOSE_FREE          /* clear the slot on core 0 now */
};

/* Close takes the background path for a native codec stream that is
 * core-1-affine while core 1 runs; every other close is synchronous. */
static inline int audio_stream_close_deferred(int native_codec,
		int core1_affine, int core1_available)
{
	return native_codec && core1_affine && core1_available;
}

static inline enum audio_stream_close_action audio_stream_close_step(
		int release_queued, int release_task_live, int io_tasks_live,
		int core1_available, int core1_affine, int holds_decoder)
{
	/* The release task left the queue. If it failed with core 1 (a
	 * fault) the heap may be half released: wait until the restart's
	 * reclaim and poison pass have dropped it. */
	if (release_queued)
		return (release_task_live || holds_decoder) ?
		       AUDIO_STREAM_CLOSE_WAIT : AUDIO_STREAM_CLOSE_FREE;
	/* Core 1 died with the heap: wait for the poison/reclaim pass to
	 * drop it, never free core-1 tracked blocks from core 0. */
	if (core1_affine && holds_decoder && !core1_available)
		return AUDIO_STREAM_CLOSE_WAIT;
	if (core1_available && io_tasks_live)
		return AUDIO_STREAM_CLOSE_WAIT;
	if (core1_affine && holds_decoder)
		return AUDIO_STREAM_CLOSE_QUEUE;
	return AUDIO_STREAM_CLOSE_FREE;
}

#endif
