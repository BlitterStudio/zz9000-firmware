/*
 * CLIENT_RELEASE quiesce bookkeeping: per-task client ownership and the
 * shared-buffer pins that keep a deferred task's resolved raw addresses
 * valid until the task leaves the queue.
 *
 * Header-only production unit (the audio_stream_close.h pattern): the
 * mailbox compiles exactly one instance over its shared_buffers table and
 * the host tests compile their own over a local table, through these same
 * functions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_CLIENT_RELEASE_H
#define SDK_CLIENT_RELEASE_H

#include <stdint.h>
#include <string.h>

#include "sdk_mailbox.h"
#include "scheduler.h"

/*
 * Upper bound on the distinct shared buffers one deferred task can
 * reference: the scan below looks at every aligned op_params word plus the
 * descriptor's in_addr and out_addr, and shared buffers never overlap, so
 * each scanned word lands in at most one buffer. Sizing the pin table to
 * that bound means a reference is never left unpinned.
 */
#define SDK_CLIENT_TASK_MAX_PINS (TASKQ_OP_PARAM_BYTES / 4U + 2U)

_Static_assert(SDK_CLIENT_TASK_MAX_PINS <= 255U,
               "SDKClientTaskLifetime.pin_count is a uint8_t");

/*
 * The shared-buffer registry entry. Lives here so the pin logic compiles
 * on the host against a test-local table; sdk_mailbox.c defines the real
 * table with this shape.
 */
struct SDKSharedBuffer {
	uint32_t handle;
	uint32_t address;
	uint32_t length;
	uint32_t flags;
	uint32_t pin_count;
	uint32_t pool_owner;
	uint16_t owner_id;
	uint8_t in_use;
};

struct SDKClientTaskLifetime {
	uint16_t client_id;   /* 0 = internal task (never blocks a release) */
	uint8_t tracked;
	uint8_t pin_count;
	uint16_t pins[SDK_CLIENT_TASK_MAX_PINS]; /* indices into the buffer table */
};

struct SDKClientReleaseState {
	struct SDKClientTaskLifetime tasks[TASKQ_CAPACITY];
};

static inline int sdk_client_buffer_live(const struct SDKSharedBuffer *buffer)
{
	return buffer->in_use != 0U && buffer->handle != 0U;
}

static inline int sdk_client_buffer_owned(
	const struct SDKSharedBuffer *buffer, uint16_t client_id)
{
	return sdk_client_buffer_live(buffer) && buffer->owner_id == client_id;
}

/* True when any live buffer of this client still holds a pin -- FREE_SHARED
 * must answer BUSY for it, and CLIENT_RELEASE cannot free it yet. */
static inline int sdk_client_buffers_pinned(
	const struct SDKSharedBuffer *buffers, uint16_t client_id)
{
	uint32_t i;

	for (i = 0U; i < SDK_MAX_SHARED_BUFFERS; i++) {
		if (sdk_client_buffer_owned(&buffers[i], client_id) &&
		    buffers[i].pin_count != 0U)
			return 1;
	}
	return 0;
}

static inline int sdk_client_pin_buffer(struct SDKClientTaskLifetime *lifetime,
                                        struct SDKSharedBuffer *buffers,
                                        uint32_t index)
{
	struct SDKSharedBuffer *buffer = &buffers[index];
	uint32_t i;

	for (i = 0U; i < lifetime->pin_count; i++) {
		if (lifetime->pins[i] == (uint16_t)index)
			return 1;
	}
	if (lifetime->pin_count >= SDK_CLIENT_TASK_MAX_PINS ||
	    buffer->pin_count == 0xffffffffU)
		return 0;
	buffer->pin_count++;
	lifetime->pins[lifetime->pin_count] = (uint16_t)index;
	lifetime->pin_count++;
	return 1;
}

static inline int sdk_client_word_in_buffer(const struct SDKSharedBuffer *buffer,
                                            uint32_t word)
{
	return word >= buffer->address &&
	       word < buffer->address + buffer->length;
}

/*
 * Record the owner and pin every live shared buffer the task references:
 * any aligned word of the packed op_params, or the descriptor's
 * in_addr/out_addr, that lands inside a buffer's card range. The scan is
 * conservative by design -- a length or handle that happens to equal a
 * card address only costs one extra pin.
 *
 * Runs on core 0 right after taskq_enqueue, before the worker can claim
 * the slot, so the descriptor contents are stable.
 */
static inline void sdk_client_track_deferred(
	struct SDKClientReleaseState *state, struct SDKSharedBuffer *buffers,
	int slot, uint16_t client_id, const taskq_desc_t *task)
{
	struct SDKClientTaskLifetime *lifetime;
	uint32_t b;
	uint32_t word_offset;

	if (slot < 0 || slot >= (int)TASKQ_CAPACITY || !task)
		return;
	lifetime = &state->tasks[slot];
	memset(lifetime, 0, sizeof(*lifetime));
	lifetime->client_id = client_id;
	lifetime->tracked = 1U;

	for (b = 0U; b < SDK_MAX_SHARED_BUFFERS; b++) {
		int referenced = 0;

		if (!sdk_client_buffer_live(&buffers[b]))
			continue;
		if (task->in_addr != 0U &&
		    sdk_client_word_in_buffer(&buffers[b], task->in_addr))
			referenced = 1;
		if (!referenced && task->out_addr != 0U &&
		    sdk_client_word_in_buffer(&buffers[b], task->out_addr))
			referenced = 1;
		for (word_offset = 0U;
		     !referenced && word_offset + sizeof(uint32_t) <= TASKQ_OP_PARAM_BYTES;
		     word_offset += sizeof(uint32_t)) {
			uint32_t word;

			memcpy(&word, task->op_params + word_offset, sizeof(word));
			if (word != 0U && sdk_client_word_in_buffer(&buffers[b], word))
				referenced = 1;
		}
		if (referenced)
			(void)sdk_client_pin_buffer(lifetime, buffers, b);
	}
}

/* Drop the pins and the ownership record: the task's completion has been
 * committed (or the task was dropped as stale), so nothing on core 1
 * references its buffers anymore. */
static inline void sdk_client_deferred_done(
	struct SDKClientReleaseState *state, struct SDKSharedBuffer *buffers,
	int slot)
{
	struct SDKClientTaskLifetime *lifetime;
	uint32_t i;

	if (slot < 0 || slot >= (int)TASKQ_CAPACITY)
		return;
	lifetime = &state->tasks[slot];
	for (i = 0U; i < lifetime->pin_count; i++) {
		struct SDKSharedBuffer *buffer = &buffers[lifetime->pins[i]];

		if (buffer->pin_count != 0U)
			buffer->pin_count--;
	}
	memset(lifetime, 0, sizeof(*lifetime));
}

/*
 * True while any tracked task of this client still occupies a queue slot
 * (queued, claimed, or finished-but-unharvested). CLIENT_RELEASE must keep
 * answering BUSY until this clears: a Ctrl-C on the Amiga side removes the
 * host pending record, but the ARM task keeps running with the client's
 * resolved buffer addresses.
 */
static inline int sdk_client_deferred_pending(
	const struct SDKClientReleaseState *state, const taskq_t *queue,
	uint16_t client_id)
{
	uint32_t i;

	for (i = 0U; i < TASKQ_CAPACITY; i++) {
		if (state->tasks[i].tracked &&
		    state->tasks[i].client_id == client_id &&
		    queue->descs[i].state != TASK_FREE)
			return 1;
	}
	return 0;
}

#endif /* SDK_CLIENT_RELEASE_H */
