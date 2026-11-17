/*
 * CLIENT_RELEASE quiesce regression against the production bookkeeping in
 * sdk_client_release.h and the real task queue in scheduler.c.
 *
 * Drives the exact functions handle_client_release and the deferred
 * completion dispatcher call: a deferred task records its client owner,
 * pins every shared buffer its packed addresses reference, and the client
 * stays BUSY until the task has been harvested and its completion posted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdk_client_release.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

#define CLIENT_A 5U
#define CLIENT_B 9U

#define BUFFER_A_ADDR 0x01000000U
#define BUFFER_A_LEN  0x00010000U
#define BUFFER_B_ADDR 0x02000000U
#define BUFFER_B_LEN  0x00008000U

static struct SDKSharedBuffer buffers[SDK_MAX_SHARED_BUFFERS];
static struct SDKClientReleaseState release_state;
static taskq_t queue;

static void reset_fixture(void)
{
	memset(buffers, 0, sizeof(buffers));
	memset(&release_state, 0, sizeof(release_state));
	taskq_init(&queue);

	buffers[0].handle = 0x1001U;
	buffers[0].address = BUFFER_A_ADDR;
	buffers[0].length = BUFFER_A_LEN;
	buffers[0].owner_id = CLIENT_A;
	buffers[0].in_use = 1U;

	buffers[1].handle = 0x1002U;
	buffers[1].address = BUFFER_B_ADDR;
	buffers[1].length = BUFFER_B_LEN;
	buffers[1].owner_id = CLIENT_B;
	buffers[1].in_use = 1U;
}

static void track_enqueued(int slot, uint16_t client_id)
{
	sdk_client_track_deferred(&release_state, buffers, slot, client_id,
	                          &queue.descs[slot]);
}

static int complete_task(int slot)
{
	int harvested;

	taskq_complete(&queue, slot, SDK_STATUS_OK, 0, 0);
	harvested = taskq_harvest(&queue);
	CHECK(harvested == slot);
	if (harvested != slot)
		return -1;
	sdk_client_deferred_done(&release_state, buffers, slot);
	taskq_release(&queue, slot);
	return 0;
}

/* A deferred JPEG-style task carries resolved raw src/dst addresses inside
 * its packed op_params. While it sits in the queue, the owner's release
 * must stay BUSY and the referenced buffer must refuse a free (its pin
 * count is nonzero); once the completion has been posted, both clear. */
static void test_deferred_task_pins_owner_buffers(void)
{
	struct jpeg_params {
		uint32_t src_addr;
		uint32_t src_len;
		uint32_t dst_addr;
		uint32_t output_format;
	} p;
	int slot;

	reset_fixture();
	p.src_addr = BUFFER_A_ADDR + 0x1234U;
	p.src_len = 0x2000U;
	p.dst_addr = 0x03000000U; /* surface, not a shared buffer */
	p.output_format = 0U;
	slot = taskq_enqueue(&queue, 0x1234U, TASK_LONG, 0U, 0x2000U, 0U, 0U,
	                     0x11U, 0U, &p, sizeof(p));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_A);

	CHECK(sdk_client_deferred_pending(&release_state, &queue, CLIENT_A));
	CHECK(!sdk_client_deferred_pending(&release_state, &queue, CLIENT_B));
	CHECK(buffers[0].pin_count == 1U);
	CHECK(sdk_client_buffers_pinned(buffers, CLIENT_A));
	/* The surface destination is not a shared buffer: no phantom pin on
	 * the unrelated owner-B buffer either. */
	CHECK(buffers[1].pin_count == 0U);
	CHECK(!sdk_client_buffers_pinned(buffers, CLIENT_B));

	CHECK(complete_task(slot) == 0);
	CHECK(!sdk_client_deferred_pending(&release_state, &queue, CLIENT_A));
	CHECK(buffers[0].pin_count == 0U);
	CHECK(!sdk_client_buffers_pinned(buffers, CLIENT_A));
}

/* Cross-owner pin: client B's in-flight task references client A's buffer
 * (the documented mixed-consumer hazard). A's release stays BUSY through
 * the pin even though A itself has nothing queued, and proceeds only after
 * B's task completed -- the P1-d scenario at the mailbox layer. */
static void test_foreign_task_pins_block_release(void)
{
	struct jpeg_params {
		uint32_t src_addr;
		uint32_t src_len;
		uint32_t dst_addr;
		uint32_t output_format;
	} p;
	int slot;

	reset_fixture();
	p.src_addr = BUFFER_A_ADDR;
	p.src_len = 0x100U;
	p.dst_addr = BUFFER_B_ADDR;
	p.output_format = 0U;
	slot = taskq_enqueue(&queue, 0x2222U, TASK_LONG, 0U, 0x100U, 0U, 0U,
	                     0x22U, 0U, &p, sizeof(p));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_B);

	/* A has no queued task of its own... */
	CHECK(!sdk_client_deferred_pending(&release_state, &queue, CLIENT_A));
	/* ...but its buffer is pinned by B's task, so freeing must wait. */
	CHECK(sdk_client_buffers_pinned(buffers, CLIENT_A));
	CHECK(buffers[0].pin_count == 1U);

	CHECK(complete_task(slot) == 0);
	CHECK(!sdk_client_buffers_pinned(buffers, CLIENT_A));
}

/* A decompress-batch task carries its arena only in the descriptor's
 * in_addr/out_addr fields; the pin scan must catch those too. */
static void test_descriptor_range_pins(void)
{
	int slot;

	reset_fixture();
	slot = taskq_enqueue(&queue, 0x3333U, TASK_LONG,
	                     BUFFER_A_ADDR + 0x10U, 0x1000U, 0U, 0U,
	                     0x33U, 0U, 0, 0U);
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_A);

	CHECK(buffers[0].pin_count == 1U);
	CHECK(sdk_client_deferred_pending(&release_state, &queue, CLIENT_A));
	CHECK(complete_task(slot) == 0);
	CHECK(buffers[0].pin_count == 0U);
}

/* An internal task (client id 0) never blocks a client release: the AX
 * refill enqueues exactly this shape. */
static void test_internal_task_never_blocks(void)
{
	struct {
		uint32_t session;
		uint32_t src_addr;
		uint32_t src_len;
		uint32_t flags;
	} refill;
	int slot;

	reset_fixture();
	refill.session = 7U;
	refill.src_addr = 0U;
	refill.src_len = 0U;
	refill.flags = 0U;
	slot = taskq_enqueue(&queue, 0x4444U, TASK_LONG, 0U, 0U, 0U, 0U,
	                     0U, 0U, &refill, sizeof(refill));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, 0U);

	CHECK(!sdk_client_deferred_pending(&release_state, &queue, CLIENT_A));
	CHECK(!sdk_client_buffers_pinned(buffers, CLIENT_A));
	CHECK(complete_task(slot) == 0);
}

/* Slot reuse: after a task's completion dropped its pins, a second task on
 * the same slot starts from a clean lifetime record -- no pin or owner
 * leaks from the previous occupant. */
static void test_slot_reuse_restarts_clean(void)
{
	uint32_t word = BUFFER_A_ADDR + 4U;
	int slot;

	reset_fixture();
	slot = taskq_enqueue(&queue, 0x5555U, TASK_LONG, 0U, 0U, 0U, 0U,
	                     0x55U, 0U, &word, sizeof(word));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_A);
	CHECK(buffers[0].pin_count == 1U);
	CHECK(complete_task(slot) == 0);
	CHECK(buffers[0].pin_count == 0U);

	slot = taskq_enqueue(&queue, 0x5556U, TASK_LONG, 0U, 0U, 0U, 0U,
	                     0x56U, 0U, &word, sizeof(word));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_B);
	CHECK(buffers[0].pin_count == 1U);
	CHECK(!sdk_client_deferred_pending(&release_state, &queue, CLIENT_A));
	CHECK(sdk_client_deferred_pending(&release_state, &queue, CLIENT_B));
	CHECK(complete_task(slot) == 0);
	CHECK(buffers[0].pin_count == 0U);
}

/* A stale mailbox generation drops a task without posting; its pins must
 * still be released (sdk_mailbox_task_forget path). */
static void test_forgotten_task_releases_pins(void)
{
	uint32_t word = BUFFER_B_ADDR + 0x40U;
	int slot;

	reset_fixture();
	slot = taskq_enqueue(&queue, 0x6666U, TASK_LONG, 0U, 0U, 0U, 0U,
	                     0x66U, 0U, &word, sizeof(word));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_B);
	CHECK(buffers[1].pin_count == 1U);

	taskq_fail(&queue, slot, SDK_STATUS_INTERNAL_ERROR);
	CHECK(taskq_harvest(&queue) == slot);
	sdk_client_deferred_done(&release_state, buffers, slot); /* forget */
	taskq_release(&queue, slot);

	CHECK(buffers[1].pin_count == 0U);
	CHECK(!sdk_client_deferred_pending(&release_state, &queue, CLIENT_B));
}

/* Multiple buffers in one op (crypto stream: src, dst, key, nonce) all get
 * pinned, and each exactly once even when a range word repeats. */
static void test_multi_buffer_op_pins_each_once(void)
{
	struct {
		uint32_t algorithm;
		uint32_t src_addr;
		uint32_t src_length;
		uint32_t key_addr;
		uint32_t nonce_addr;
		uint32_t counter;
		uint32_t dst_addr;
	} p;
	int slot;

	reset_fixture();
	buffers[2].handle = 0x1003U;
	buffers[2].address = 0x04000000U;
	buffers[2].length = 0x1000U;
	buffers[2].owner_id = CLIENT_A;
	buffers[2].in_use = 1U;

	p.algorithm = 1U;
	p.src_addr = BUFFER_A_ADDR + 8U;
	p.src_length = BUFFER_A_ADDR + 8U; /* duplicate range word */
	p.key_addr = 0x04000000U + 0x10U;
	p.nonce_addr = BUFFER_A_ADDR + 12U;
	p.counter = 0U;
	p.dst_addr = BUFFER_B_ADDR;
	slot = taskq_enqueue(&queue, 0x7777U, TASK_LONG, 0U, 0x100U, 0U, 0U,
	                     0x77U, 0U, &p, sizeof(p));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_A);

	CHECK(buffers[0].pin_count == 1U);
	CHECK(buffers[1].pin_count == 1U);
	CHECK(buffers[2].pin_count == 1U);
	CHECK(complete_task(slot) == 0);
	CHECK(buffers[0].pin_count == 0U);
	CHECK(buffers[1].pin_count == 0U);
	CHECK(buffers[2].pin_count == 0U);
}

/* A task whose every op_params word and both descriptor ranges land in a
 * different live buffer pins all of them: no reference may stay unpinned
 * however many buffers one op touches. */
static void test_every_scanned_reference_is_pinned(void)
{
	uint32_t words[TASKQ_OP_PARAM_BYTES / 4U];
	uint32_t total = TASKQ_OP_PARAM_BYTES / 4U + 2U;
	uint32_t i;
	int slot;

	reset_fixture();
	memset(buffers, 0, sizeof(buffers));
	for (i = 0U; i < total; i++) {
		buffers[i].handle = 0x2000U + i;
		buffers[i].address = 0x10000000U + i * 0x10000U;
		buffers[i].length = 0x1000U;
		buffers[i].owner_id = CLIENT_A;
		buffers[i].in_use = 1U;
	}
	for (i = 0U; i < TASKQ_OP_PARAM_BYTES / 4U; i++)
		words[i] = buffers[i].address + 4U;
	slot = taskq_enqueue(&queue, 0x8888U, TASK_LONG,
	                     buffers[total - 2U].address, 0x100U,
	                     buffers[total - 1U].address, 0x100U,
	                     0x88U, 0U, words, sizeof(words));
	CHECK(slot >= 0);
	if (slot < 0)
		return;
	track_enqueued(slot, CLIENT_A);

	for (i = 0U; i < total; i++)
		CHECK(buffers[i].pin_count == 1U);
	CHECK(complete_task(slot) == 0);
	for (i = 0U; i < total; i++)
		CHECK(buffers[i].pin_count == 0U);
}

int main(void)
{
	test_deferred_task_pins_owner_buffers();
	test_foreign_task_pins_block_release();
	test_descriptor_range_pins();
	test_internal_task_never_blocks();
	test_slot_reuse_restarts_clean();
	test_forgotten_task_releases_pins();
	test_multi_buffer_op_pins_each_once();
	test_every_scanned_reference_is_pinned();
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
