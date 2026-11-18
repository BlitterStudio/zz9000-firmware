/*
 * Host test of the queue coherence probe (sdk_qprobe.c). The production
 * code runs against a fake Amiga written with the wire contract header
 * (sdk/tools/zz9k-qprobe.h): it produces and consumes entries in the same
 * fake shared buffers and checks what the firmware wrote.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scheduler.h"
#include "sdk_mailbox.h"
#include "sdk_qprobe.h"
#include "xil_cache.h"
#include "xtime_l.h"

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

#define CHECK_EQ(a, b) do { \
	uint32_t va_ = (uint32_t)(a); \
	uint32_t vb_ = (uint32_t)(b); \
	checks++; \
	if (va_ != vb_) { \
		failures++; \
		printf("FAIL %s:%d: %s == %s (%lu vs %lu)\n", __FILE__, __LINE__, \
		       #a, #b, (unsigned long)va_, (unsigned long)vb_); \
	} \
} while (0)

/* ---- mocks for the firmware environment ---- */

XTime qprobe_mock_time;
XTime qprobe_mock_tick;
unsigned qprobe_mock_cache_ops;
uint8_t qprobe_host_state[0x2000] __attribute__((aligned(64)));

static unsigned barriers;
static void (*barrier_hook)(void);

void qprobe_mock_dmb(void)
{
	barriers++;
	if (barrier_hook)
		barrier_hook();
}

static taskq_shared_t shared;
static int core1_available;

taskq_shared_t *scheduler_shared(void)
{
	return &shared;
}

int scheduler_core1_available(void)
{
	return core1_available;
}

#define FAKE_BASE 0x08000000U
#define FAKE_STRIDE 0x00010000U
#define FAKE_BUFFERS 8U

static uint8_t fake_mem[FAKE_BUFFERS][FAKE_STRIDE]
	__attribute__((aligned(4096)));

volatile void *qprobe_host_ptr(uint32_t address)
{
	uint32_t index = (address - FAKE_BASE) / FAKE_STRIDE;

	if (address < FAKE_BASE || index >= FAKE_BUFFERS) {
		printf("FAIL: pair address %08lx outside the fake buffers\n",
		       (unsigned long)address);
		failures++;
		return fake_mem[0];
	}
	return fake_mem[index] + (address - FAKE_BASE) % FAKE_STRIDE;
}

struct FakeBuffer {
	uint32_t handle;
	uint32_t address;
	uint32_t length;
	uint32_t flags;
	uint16_t owner_id;
	int live;
};

static struct FakeBuffer buffers[FAKE_BUFFERS];

static int lookup(uint32_t handle, struct SDKQProbeBuffer *out)
{
	uint32_t i;

	for (i = 0U; i < FAKE_BUFFERS; i++) {
		if (buffers[i].live && buffers[i].handle == handle) {
			out->handle = buffers[i].handle;
			out->address = buffers[i].address;
			out->length = buffers[i].length;
			out->flags = buffers[i].flags;
			out->owner_id = buffers[i].owner_id;
			return 1;
		}
	}
	return 0;
}

#define CLIENT_A 5U
#define CLIENT_B 9U
#define TICK ((XTime)COUNTS_PER_SECOND / 1000U)

static void add_buffer(uint32_t i, uint16_t owner, uint32_t length,
                       uint32_t flags)
{
	buffers[i].handle = 0x100U + i;
	buffers[i].address = FAKE_BASE + i * FAKE_STRIDE;
	buffers[i].length = length;
	buffers[i].flags = flags;
	buffers[i].owner_id = owner;
	buffers[i].live = 1;
}

static void reset_all(void)
{
	memset(qprobe_host_state, 0, sizeof(qprobe_host_state));
	memset(fake_mem, 0, sizeof(fake_mem));
	memset(buffers, 0, sizeof(buffers));
	memset(&shared, 0, sizeof(shared));
	qprobe_mock_time = 0U;
	qprobe_mock_tick = TICK;
	barrier_hook = 0;
	core1_available = 1;
	CHECK(sdk_qprobe_reset() == 1);
	qprobe_mock_cache_ops = 0U;
}

/* ---- mailbox payload helpers ---- */

static uint16_t command(uint16_t client, unsigned words, uint32_t cmd,
                        uint32_t a, uint32_t b, uint32_t c, uint32_t *out,
                        unsigned *out_words)
{
	uint32_t in[4];
	uint8_t payload[16];
	uint8_t reply[48];
	uint16_t reply_len = 0xffffU;
	uint16_t status;
	unsigned i;

	in[0] = cmd;
	in[1] = a;
	in[2] = b;
	in[3] = c;
	for (i = 0U; i < 4U; i++) {
		payload[4U * i] = (uint8_t)(in[i] >> 24);
		payload[4U * i + 1U] = (uint8_t)(in[i] >> 16);
		payload[4U * i + 2U] = (uint8_t)(in[i] >> 8);
		payload[4U * i + 3U] = (uint8_t)in[i];
	}
	memset(reply, 0xee, sizeof(reply));
	status = sdk_qprobe_command(client, payload, (uint16_t)(4U * words),
	                            reply, &reply_len, lookup);
	if (status != SDK_STATUS_OK)
		CHECK_EQ(reply_len, 0U);
	if (out_words)
		*out_words = reply_len / 4U;
	for (i = 0U; out && i < reply_len / 4U; i++) {
		out[i] = ((uint32_t)reply[4U * i] << 24) |
		         ((uint32_t)reply[4U * i + 1U] << 16) |
		         ((uint32_t)reply[4U * i + 2U] << 8) | reply[4U * i + 3U];
	}
	return status;
}

static uint16_t attach(uint16_t client, uint32_t buffer, uint32_t queue,
                       uint32_t owner)
{
	return command(client, 4U, ZZ9K_QP_CMD_ATTACH, 0x100U + buffer, queue,
	               owner, 0, 0);
}

static uint16_t status_of(uint32_t queue, uint32_t *words)
{
	unsigned count = 0U;
	uint16_t status = command(CLIENT_B, 2U, ZZ9K_QP_CMD_STATUS, queue, 0, 0,
	                          words, &count);

	if (status == SDK_STATUS_OK)
		CHECK_EQ(count, ZZ9K_QP_STATUS_WORDS);
	return status;
}

static uint16_t detach(uint16_t client, uint32_t queue, uint32_t *words)
{
	return command(client, 2U, ZZ9K_QP_CMD_DETACH, queue, 0, 0, words, 0);
}

static uint32_t kick(uint32_t *words)
{
	unsigned count = 0U;

	CHECK(command(CLIENT_B, 1U, ZZ9K_QP_CMD_KICK, 0, 0, 0, words, &count) ==
	      SDK_STATUS_OK);
	CHECK_EQ(count, ZZ9K_QP_KICK_WORDS);
	return count;
}

static struct SDKQProbeQueue *queue_state(uint32_t index)
{
	return &SDK_QPROBE_STATE->queue[index];
}

/* ---- the fake Amiga ---- */

struct Amiga {
	uint32_t queue;
	volatile void *pair;
	uint32_t up_head;
	uint32_t down_tail;
	uint32_t origin_mask;
	uint32_t streams;
	uint32_t pongs;
	uint32_t completes;
	uint32_t bad;
	uint32_t pong_id[64];
};

static void am_init(struct Amiga *a, uint32_t queue, uint32_t buffer,
                    uint32_t origin)
{
	memset(a, 0, sizeof(*a));
	a->queue = queue;
	a->pair = qprobe_host_ptr(FAKE_BASE + buffer * FAKE_STRIDE);
	a->origin_mask = 1U << origin;
}

static void am_set_mode(struct Amiga *a, uint32_t mode)
{
	zz9k_qp_set(a->pair, ZZ9K_QP_HDR_MODE, mode);
}

/* Up entry: words, barrier, head. */
static void am_up(struct Amiga *a, uint32_t kind, uint32_t w3, uint32_t w4)
{
	volatile uint32_t *entry =
	    zz9k_qp_slot(a->pair, ZZ9K_QP_UP_RING_OFFSET, a->up_head);

	zz9k_qp_entry_write(entry, a->up_head + 1U, kind, a->queue,
	                    ZZ9K_QP_ORIGIN_AMIGA, w3, w4);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	a->up_head++;
	zz9k_qp_set(a->pair, ZZ9K_QP_UP_HEAD, a->up_head);
}

/* Takes up to `max` down entries, checks each and completes it. */
static uint32_t am_consume(struct Amiga *a, uint32_t max)
{
	uint32_t head = zz9k_qp_get(a->pair, ZZ9K_QP_DOWN_HEAD);
	int32_t available;
	uint32_t taken = 0U;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	available = zz9k_qp_available(head, a->down_tail);
	if (available < 0) {
		a->bad++;
		return 0U;
	}
	while (taken < (uint32_t)available && taken < max) {
		ZZ9KQpEntry entry;
		uint32_t seq = a->down_tail + 1U;
		uint32_t result = zz9k_qp_entry_check(
		    zz9k_qp_slot(a->pair, ZZ9K_QP_DOWN_RING_OFFSET, a->down_tail),
		    seq, a->queue, a->origin_mask, &entry);

		if (result != ZZ9K_QP_OK) {
			a->bad++;
		} else if (entry.kind == ZZ9K_QP_KIND_STREAM) {
			a->streams++;
		} else if (entry.kind == ZZ9K_QP_KIND_PONG) {
			if (a->pongs < 64U)
				a->pong_id[a->pongs] = entry.arg;
			a->pongs++;
		} else {
			a->bad++;
		}
		am_up(a, ZZ9K_QP_KIND_COMPLETE, 0U, seq);
		a->completes++;
		a->down_tail++;
		taken++;
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	zz9k_qp_set(a->pair, ZZ9K_QP_DOWN_TAIL, a->down_tail);
	return taken;
}

static uint32_t down_head(struct Amiga *a)
{
	return zz9k_qp_get(a->pair, ZZ9K_QP_DOWN_HEAD);
}

static uint32_t up_tail(struct Amiga *a)
{
	return zz9k_qp_get(a->pair, ZZ9K_QP_UP_TAIL);
}

/* Fetches one STATUS word of a queue. */
static uint32_t status_word(uint32_t queue, uint32_t word)
{
	uint32_t words[ZZ9K_QP_STATUS_WORDS];

	if (status_of(queue, words) != SDK_STATUS_OK)
		return 0xdeadbeefU;
	return words[word];
}

/* Barrier hook: every time the firmware calls dmb() while a pending entry is
 * complete but not yet published, count it. A producer that wrote the head
 * without a barrier in between would never show such a moment. */
static struct {
	volatile void *pair;
	uint32_t queue;
	uint32_t origin_mask;
	unsigned complete_unpublished;
} watch;

static void watch_producer_barrier(void)
{
	uint32_t head = zz9k_qp_get(watch.pair, ZZ9K_QP_DOWN_HEAD);

	if (zz9k_qp_entry_check(zz9k_qp_slot(watch.pair,
	                                     ZZ9K_QP_DOWN_RING_OFFSET, head),
	                        head + 1U, watch.queue, watch.origin_mask,
	                        0) == ZZ9K_QP_OK)
		watch.complete_unpublished++;
}

/* ---- tests ---- */

static void test_attach_validation(void)
{
	uint32_t words[12];
	unsigned count;
	uint32_t i;
	unsigned cache_ops;

	reset_all();
	add_buffer(0U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	add_buffer(1U, CLIENT_B, ZZ9K_QP_PAIR_BYTES, 0U);
	add_buffer(2U, CLIENT_A, ZZ9K_QP_PAIR_BYTES - ZZ9K_QP_PAIR_ALIGN, 0U);
	add_buffer(3U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	buffers[3].address += 0x10U;
	add_buffer(4U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, SDK_ALLOC_CARD_ONLY);
	add_buffer(5U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	add_buffer(6U, CLIENT_A, 2U * ZZ9K_QP_PAIR_BYTES, 0U);

	/* bad command, length, queue and owner */
	CHECK_EQ(command(CLIENT_A, 0U, ZZ9K_QP_CMD_STATUS, 0, 0, 0, 0, 0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(command(CLIENT_A, 4U, 99U, 0, 0, 0, 0, 0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(command(CLIENT_A, 3U, ZZ9K_QP_CMD_ATTACH, 0x100U, 0U, 0U, 0, 0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(attach(CLIENT_A, 0U, ZZ9K_QP_MAX_QUEUES, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(attach(CLIENT_A, 0U, 0U, ZZ9K_QP_OWNER_COUNT),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(command(CLIENT_A, 1U, ZZ9K_QP_CMD_STATUS, 0, 0, 0, 0, 0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(command(CLIENT_A, 1U, ZZ9K_QP_CMD_DETACH, 0, 0, 0, 0, 0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(command(CLIENT_A, 2U, ZZ9K_QP_CMD_STATUS, ZZ9K_QP_MAX_QUEUES, 0,
	                 0, 0, 0),
	         SDK_STATUS_BAD_REQUEST);

	/* bad buffers */
	CHECK_EQ(command(CLIENT_A, 4U, ZZ9K_QP_CMD_ATTACH, 0x999U, 0U,
	                 ZZ9K_QP_OWNER_CORE0, 0, 0),
	         SDK_STATUS_BAD_HANDLE);
	CHECK_EQ(attach(CLIENT_A, 1U, 0U, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BAD_HANDLE); /* another client's buffer */
	CHECK_EQ(attach(CLIENT_A, 2U, 0U, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BAD_REQUEST); /* too small */
	CHECK_EQ(attach(CLIENT_A, 3U, 0U, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BAD_REQUEST); /* not 4 KiB aligned */
	CHECK_EQ(attach(CLIENT_A, 4U, 0U, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BAD_REQUEST); /* not host visible */
	core1_available = 0;
	CHECK_EQ(attach(CLIENT_A, 0U, 0U, ZZ9K_QP_OWNER_CORE1),
	         SDK_STATUS_UNSUPPORTED); /* core 1 offline */
	core1_available = 1;
	CHECK_EQ(status_of(0U, words), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 0U);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core1_mask, 0U);

	/* a good attach clears the pair and writes the header */
	memset(fake_mem[0], 0xaa, sizeof(fake_mem[0]));
	cache_ops = qprobe_mock_cache_ops;
	CHECK_EQ(command(CLIENT_A, 4U, ZZ9K_QP_CMD_ATTACH, 0x100U, 7U,
	                 ZZ9K_QP_OWNER_CORE0, words, &count),
	         SDK_STATUS_OK);
	CHECK_EQ(count, ZZ9K_QP_ATTACH_WORDS);
	CHECK_EQ(words[ZZ9K_QP_ATTACH_ARM_ADDR], FAKE_BASE);
	CHECK_EQ(words[ZZ9K_QP_ATTACH_PAIR_BYTES], ZZ9K_QP_PAIR_BYTES);
	CHECK_EQ(words[ZZ9K_QP_ATTACH_VERSION], ZZ9K_QP_VERSION);
	CHECK_EQ(zz9k_qp_get(fake_mem[0], ZZ9K_QP_HDR_MAGIC), ZZ9K_QP_MAGIC);
	CHECK_EQ(zz9k_qp_get(fake_mem[0], ZZ9K_QP_HDR_VERSION), ZZ9K_QP_VERSION);
	CHECK_EQ(zz9k_qp_get(fake_mem[0], ZZ9K_QP_HDR_QUEUE), 7U);
	CHECK_EQ(zz9k_qp_get(fake_mem[0], ZZ9K_QP_HDR_OWNER),
	         ZZ9K_QP_OWNER_CORE0);
	CHECK_EQ(zz9k_qp_get(fake_mem[0], ZZ9K_QP_HDR_MODE), ZZ9K_QP_MODE_IDLE);
	for (i = 0x14U; i < ZZ9K_QP_PAIR_BYTES; i += 4U) {
		if (i == ZZ9K_QP_HDR_MAGIC || i == ZZ9K_QP_HDR_VERSION ||
		    i == ZZ9K_QP_HDR_QUEUE || i == ZZ9K_QP_HDR_OWNER)
			continue;
		CHECK_EQ(zz9k_qp_get(fake_mem[0], i), 0U);
	}
	CHECK_EQ(fake_mem[0][ZZ9K_QP_PAIR_BYTES], 0xaaU); /* nothing past it */
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 1U << 7);

	/* a busy queue, a second queue on the same buffer */
	CHECK_EQ(attach(CLIENT_A, 5U, 7U, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BUSY);
	CHECK_EQ(attach(CLIENT_A, 0U, 8U, ZZ9K_QP_OWNER_CORE0),
	         SDK_STATUS_BUSY);
	CHECK_EQ(attach(CLIENT_A, 5U, 8U, ZZ9K_QP_OWNER_DOORBELL),
	         SDK_STATUS_OK);
	/* a larger buffer is fine */
	CHECK_EQ(attach(CLIENT_A, 6U, 31U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);

	/* STATUS answers to anyone, DETACH only to the owning client */
	CHECK_EQ(status_of(7U, words), SDK_STATUS_OK);
	CHECK_EQ(words[ZZ9K_QP_STATUS_OWNER], ZZ9K_QP_OWNER_CORE0);
	CHECK_EQ(words[ZZ9K_QP_STATUS_TIMER_HZ], (uint32_t)COUNTS_PER_SECOND);
	CHECK_EQ(detach(CLIENT_B, 7U, 0), SDK_STATUS_BAD_HANDLE);
	CHECK_EQ(detach(CLIENT_A, 9U, 0), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(detach(CLIENT_A, ZZ9K_QP_MAX_QUEUES, 0),
	         SDK_STATUS_BAD_REQUEST);
	CHECK_EQ(detach(CLIENT_A, 7U, words), SDK_STATUS_OK);
	CHECK_EQ(status_of(7U, words), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 7U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);

	/* no cache maintenance anywhere: the ACP is what is under test */
	CHECK_EQ(qprobe_mock_cache_ops, cache_ops);
}

static void test_stream_fills_to_full_and_completions_free_space(void)
{
	struct Amiga am;
	uint32_t words[ZZ9K_QP_STATUS_WORDS];
	unsigned before;

	reset_all();
	add_buffer(0U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 2U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);
	am_init(&am, 2U, 0U, ZZ9K_QP_ORIGIN_CORE0);

	/* IDLE mode produces nothing */
	sdk_qprobe_service_core0();
	CHECK_EQ(down_head(&am), 0U);

	am_set_mode(&am, ZZ9K_QP_MODE_STREAM);
	watch.pair = am.pair;
	watch.queue = 2U;
	watch.origin_mask = 1U << ZZ9K_QP_ORIGIN_CORE0;
	watch.complete_unpublished = 0U;
	barrier_hook = watch_producer_barrier;
	before = barriers;
	sdk_qprobe_poll_core0();
	barrier_hook = 0;
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES);
	CHECK(barriers > before);
	/* each entry was complete at a barrier before its head was published */
	CHECK(watch.complete_unpublished >= ZZ9K_QP_ENTRIES);

	/* a full ring stops the producer and is counted once */
	sdk_qprobe_poll_core0();
	sdk_qprobe_poll_core0();
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_RING_FULL), 1U);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_DOWN_PRODUCED), ZZ9K_QP_ENTRIES);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_PASSES), 4U);

	/* ten consumed entries and their completions free ten slots */
	CHECK_EQ(am_consume(&am, 10U), 10U);
	CHECK_EQ(am.streams, 10U);
	sdk_qprobe_poll_core0();
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES + 10U);
	CHECK_EQ(up_tail(&am), 10U);
	CHECK_EQ(status_of(2U, words), SDK_STATUS_OK);
	CHECK_EQ(words[ZZ9K_QP_STATUS_UP_CHECKED], 10U);
	CHECK_EQ(words[ZZ9K_QP_STATUS_UP_ERRORS], 0U);
	CHECK_EQ(words[ZZ9K_QP_STATUS_FIRST_ERROR], ZZ9K_QP_OK);
	CHECK_EQ(words[ZZ9K_QP_STATUS_COMPLETED], 10U);
	CHECK_EQ(words[ZZ9K_QP_STATUS_RING_FULL], 2U); /* full again */

	/* everything the firmware wrote checks out, in order, across the wrap */
	CHECK_EQ(am_consume(&am, 1000U), ZZ9K_QP_ENTRIES);
	CHECK_EQ(am.streams, ZZ9K_QP_ENTRIES + 10U);
	CHECK_EQ(am.bad, 0U);
	sdk_qprobe_poll_core0();
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_COMPLETED),
	         ZZ9K_QP_ENTRIES + 10U);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_UP_ERRORS), 0U);

	/* back to IDLE: the ring drains and stays quiet */
	am_set_mode(&am, ZZ9K_QP_MODE_IDLE);
	am_consume(&am, 1000U);
	sdk_qprobe_poll_core0();
	{
		uint32_t head = down_head(&am);

		sdk_qprobe_poll_core0();
		CHECK_EQ(down_head(&am), head);
	}
}

static void test_ping_pong_with_full_ring(void)
{
	struct Amiga am;
	uint32_t round;
	uint32_t i;
	ZZ9KQpEntry entry;

	reset_all();
	add_buffer(0U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 4U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);
	am_init(&am, 4U, 0U, ZZ9K_QP_ORIGIN_CORE0);
	am_set_mode(&am, ZZ9K_QP_MODE_STREAM);
	sdk_qprobe_service_core0();
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES);

	/* the ring is full: the PING is taken, its PONG waits */
	am_up(&am, ZZ9K_QP_KIND_PING, 0x1234U, 0U);
	sdk_qprobe_service_core0();
	CHECK_EQ(status_word(4U, ZZ9K_QP_STATUS_UP_CHECKED), 1U);
	CHECK_EQ(status_word(4U, ZZ9K_QP_STATUS_PONGS), 0U);
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES);

	/* one slot frees up: the PONG goes first, with the id */
	CHECK_EQ(am_consume(&am, 1U), 1U);
	sdk_qprobe_service_core0();
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES + 1U);
	CHECK_EQ(zz9k_qp_entry_check(zz9k_qp_slot(am.pair,
	                                          ZZ9K_QP_DOWN_RING_OFFSET,
	                                          ZZ9K_QP_ENTRIES),
	                             ZZ9K_QP_ENTRIES + 1U, 4U,
	                             1U << ZZ9K_QP_ORIGIN_CORE0, &entry),
	         ZZ9K_QP_OK);
	CHECK_EQ(entry.kind, ZZ9K_QP_KIND_PONG);
	CHECK_EQ(entry.arg, 0x1234U);
	CHECK_EQ(status_word(4U, ZZ9K_QP_STATUS_PONGS), 1U);

	/* more PINGs than the firmware queues: the rest stays in the up ring */
	for (i = 1U; i <= 10U; i++)
		am_up(&am, ZZ9K_QP_KIND_PING, i, 0U);
	sdk_qprobe_service_core0();
	CHECK_EQ(up_tail(&am), 2U + SDK_QPROBE_PONG_DEPTH);
	CHECK_EQ(status_word(4U, ZZ9K_QP_STATUS_PONGS), 1U);

	for (round = 0U; round < 200U && am.pongs < 11U; round++) {
		am_consume(&am, 8U);
		sdk_qprobe_service_core0();
	}
	CHECK_EQ(am.pongs, 11U);
	CHECK_EQ(am.pong_id[0], 0x1234U);
	for (i = 1U; i <= 10U && i < 11U; i++)
		CHECK_EQ(am.pong_id[i], i);
	CHECK_EQ(am.bad, 0U);
	CHECK_EQ(status_word(4U, ZZ9K_QP_STATUS_UP_ERRORS), 0U);
	CHECK_EQ(status_word(4U, ZZ9K_QP_STATUS_PONGS), 11U);
}

/* Runs one bad-up-entry case on a fresh CORE0 queue. */
static void begin_error_case(struct Amiga *am)
{
	reset_all();
	add_buffer(0U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 6U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);
	am_init(am, 6U, 0U, ZZ9K_QP_ORIGIN_CORE0);
	am_set_mode(am, ZZ9K_QP_MODE_STREAM);
	sdk_qprobe_service_core0();
}

static void expect_error(uint32_t errors, uint32_t code, uint32_t seq)
{
	uint32_t words[ZZ9K_QP_STATUS_WORDS];

	sdk_qprobe_service_core0();
	CHECK_EQ(status_of(6U, words), SDK_STATUS_OK);
	CHECK_EQ(words[ZZ9K_QP_STATUS_UP_ERRORS], errors);
	CHECK_EQ(words[ZZ9K_QP_STATUS_FIRST_ERROR], code);
	CHECK_EQ(words[ZZ9K_QP_STATUS_FIRST_ERROR_SEQ], seq);
}

static void write_up_raw(struct Amiga *am, uint32_t kind, uint32_t queue,
                         uint32_t origin, uint32_t arg,
                         volatile uint32_t **entry_out)
{
	volatile uint32_t *entry =
	    zz9k_qp_slot(am->pair, ZZ9K_QP_UP_RING_OFFSET, am->up_head);

	zz9k_qp_entry_write(entry, am->up_head + 1U, kind, queue, origin, 0U,
	                    arg);
	if (entry_out)
		*entry_out = entry;
}

static void publish_up(struct Amiga *am)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	am->up_head++;
	zz9k_qp_set(am->pair, ZZ9K_QP_UP_HEAD, am->up_head);
}

static void test_up_entry_errors(void)
{
	struct Amiga am;
	volatile uint32_t *entry;

	/* COMPLETE out of order: 3 then 2 */
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, 3U);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, 2U);
	expect_error(1U, ZZ9K_QP_ERR_ORDER, 2U);
	CHECK_EQ(status_word(6U, ZZ9K_QP_STATUS_COMPLETED), 3U);
	CHECK_EQ(status_word(6U, ZZ9K_QP_STATUS_UP_CHECKED), 2U);

	/* a repeat is not strictly greater */
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, 5U);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, 5U);
	expect_error(1U, ZZ9K_QP_ERR_ORDER, 2U);

	/* ahead of what was produced (64 entries) */
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, ZZ9K_QP_ENTRIES + 1U);
	expect_error(1U, ZZ9K_QP_ERR_ORDER, 1U);
	CHECK_EQ(status_word(6U, ZZ9K_QP_STATUS_COMPLETED), 0U);

	/* zero is not past the initial zero */
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, 0U);
	expect_error(1U, ZZ9K_QP_ERR_ORDER, 1U);

	/* the last produced entry is a valid COMPLETE */
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, ZZ9K_QP_ENTRIES);
	expect_error(0U, ZZ9K_QP_OK, 0U);
	CHECK_EQ(status_word(6U, ZZ9K_QP_STATUS_COMPLETED), ZZ9K_QP_ENTRIES);

	/* torn: w15 is not ~w0 */
	begin_error_case(&am);
	write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_AMIGA, 1U,
	             &entry);
	entry[15] ^= zz9k_qp_swap(1U);
	publish_up(&am);
	expect_error(1U, ZZ9K_QP_ERR_TORN, 1U);

	/* head published before the data: the slot still holds nothing */
	begin_error_case(&am);
	publish_up(&am);
	expect_error(1U, ZZ9K_QP_ERR_STALE, 1U);
	/* the consumer carries on with the next entry */
	am_up(&am, ZZ9K_QP_KIND_COMPLETE, 0U, 1U);
	expect_error(1U, ZZ9K_QP_ERR_STALE, 1U);
	CHECK_EQ(status_word(6U, ZZ9K_QP_STATUS_COMPLETED), 1U);
	CHECK_EQ(status_word(6U, ZZ9K_QP_STATUS_UP_CHECKED), 2U);

	/* a stale slot from an earlier lap: valid entry, wrong seq */
	begin_error_case(&am);
	write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_AMIGA, 1U,
	             &entry);
	entry[0] = zz9k_qp_swap(1U - ZZ9K_QP_ENTRIES);
	entry[15] = zz9k_qp_swap(~(1U - ZZ9K_QP_ENTRIES));
	publish_up(&am);
	expect_error(1U, ZZ9K_QP_ERR_STALE, 1U);

	/* wrong pattern word */
	begin_error_case(&am);
	write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_AMIGA, 1U,
	             &entry);
	entry[7] ^= zz9k_qp_swap(0x100U);
	publish_up(&am);
	expect_error(1U, ZZ9K_QP_ERR_PATTERN, 1U);

	/* names another queue, or an origin the Amiga may not use */
	begin_error_case(&am);
	{
		volatile uint32_t *entry;

		/* the pattern stays valid; only w2 names queue 7 */
		write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_AMIGA, 1U,
		             &entry);
		entry[2] = zz9k_qp_swap((7U << 16) | ZZ9K_QP_ORIGIN_AMIGA);
	}
	publish_up(&am);
	expect_error(1U, ZZ9K_QP_ERR_QUEUE, 1U);
	begin_error_case(&am);
	write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_CORE1, 1U, 0);
	publish_up(&am);
	expect_error(1U, ZZ9K_QP_ERR_QUEUE, 1U);

	/* kinds the up direction never carries */
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_STREAM, 0U, 0U);
	expect_error(1U, ZZ9K_QP_ERR_KIND, 1U);
	begin_error_case(&am);
	am_up(&am, ZZ9K_QP_KIND_PONG, 0U, 0U);
	expect_error(1U, ZZ9K_QP_ERR_KIND, 1U);

	/* an index beyond the ring is counted once */
	begin_error_case(&am);
	zz9k_qp_set(am.pair, ZZ9K_QP_UP_HEAD, 100U);
	expect_error(1U, ZZ9K_QP_ERR_INDEX, 1U);
	sdk_qprobe_service_core0();
	expect_error(1U, ZZ9K_QP_ERR_INDEX, 1U);
	/* the consumer follows the producer's count */
	CHECK_EQ(up_tail(&am), 100U);

	/* the first error stays; later ones are counted */
	begin_error_case(&am);
	write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_AMIGA, 1U,
	             &entry);
	entry[15] ^= zz9k_qp_swap(1U);
	publish_up(&am);
	write_up_raw(&am, ZZ9K_QP_KIND_COMPLETE, 6U, ZZ9K_QP_ORIGIN_AMIGA, 2U,
	             &entry);
	entry[9] ^= zz9k_qp_swap(1U);
	publish_up(&am);
	am_up(&am, ZZ9K_QP_KIND_STREAM, 0U, 0U);
	expect_error(3U, ZZ9K_QP_ERR_TORN, 1U);
}

static void test_kick_serves_only_doorbell_queues(void)
{
	struct Amiga am[4];
	uint32_t words[ZZ9K_QP_KICK_WORDS];
	uint32_t i;
	uint32_t j;

	reset_all();
	for (i = 0U; i < 4U; i++)
		add_buffer(i, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 0U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);
	CHECK_EQ(attach(CLIENT_A, 1U, 1U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);
	CHECK_EQ(attach(CLIENT_A, 2U, 2U, ZZ9K_QP_OWNER_DOORBELL),
	         SDK_STATUS_OK);
	CHECK_EQ(attach(CLIENT_A, 3U, 3U, ZZ9K_QP_OWNER_DOORBELL),
	         SDK_STATUS_OK);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 1U);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core1_mask, 2U);
	CHECK(sdk_qprobe_core1_active());
	am_init(&am[0], 0U, 0U, ZZ9K_QP_ORIGIN_CORE0);
	am_init(&am[1], 1U, 1U, ZZ9K_QP_ORIGIN_CORE1);
	am_init(&am[2], 2U, 2U, ZZ9K_QP_ORIGIN_CORE0);
	am_init(&am[3], 3U, 3U, ZZ9K_QP_ORIGIN_CORE0);
	for (i = 0U; i < 4U; i++) {
		am_set_mode(&am[i], ZZ9K_QP_MODE_STREAM);
		for (j = 0U; j < 3U; j++)
			am_up(&am[i], ZZ9K_QP_KIND_PING, 100U + j, 0U);
	}

	kick(words);
	CHECK_EQ(words[ZZ9K_QP_KICK_QUEUES], 2U);
	CHECK_EQ(words[ZZ9K_QP_KICK_CONSUMED], 6U);
	CHECK(words[ZZ9K_QP_KICK_TICKS] > 0U);
	CHECK_EQ(words[ZZ9K_QP_KICK_TIMER_HZ], (uint32_t)COUNTS_PER_SECOND);
	for (i = 2U; i < 4U; i++) {
		CHECK_EQ(down_head(&am[i]), ZZ9K_QP_ENTRIES);
		CHECK_EQ(status_word(i, ZZ9K_QP_STATUS_PASSES), 1U);
		CHECK_EQ(status_word(i, ZZ9K_QP_STATUS_PONGS), 3U);
	}
	for (i = 0U; i < 2U; i++) {
		CHECK_EQ(down_head(&am[i]), 0U);
		CHECK_EQ(status_word(i, ZZ9K_QP_STATUS_PASSES), 0U);
		CHECK_EQ(status_word(i, ZZ9K_QP_STATUS_UP_CHECKED), 0U);
	}

	/* the core-0 loop serves CORE0 queues only, core 1 serves CORE1 only */
	sdk_qprobe_poll_core0();
	CHECK_EQ(status_word(0U, ZZ9K_QP_STATUS_PASSES), 1U);
	CHECK_EQ(status_word(0U, ZZ9K_QP_STATUS_UP_CHECKED), 3U);
	CHECK_EQ(status_word(1U, ZZ9K_QP_STATUS_PASSES), 0U);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_PASSES), 1U);
	sdk_qprobe_service_core1();
	CHECK_EQ(status_word(1U, ZZ9K_QP_STATUS_PASSES), 1U);
	CHECK_EQ(status_word(1U, ZZ9K_QP_STATUS_UP_CHECKED), 3U);
	CHECK_EQ(status_word(0U, ZZ9K_QP_STATUS_PASSES), 1U);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_PASSES), 1U);
	CHECK_EQ(status_word(3U, ZZ9K_QP_STATUS_PASSES), 1U);
	/* core 1 entries carry the core-1 origin and verify */
	CHECK_EQ(am_consume(&am[1], 1000U), ZZ9K_QP_ENTRIES);
	CHECK_EQ(am[1].bad, 0U);
	CHECK_EQ(am_consume(&am[0], 1000U), ZZ9K_QP_ENTRIES);
	CHECK_EQ(am[0].bad, 0U);

	/* a second KICK has nothing new to consume */
	kick(words);
	CHECK_EQ(words[ZZ9K_QP_KICK_QUEUES], 2U);
	CHECK_EQ(words[ZZ9K_QP_KICK_CONSUMED], 0U);
	CHECK_EQ(status_word(2U, ZZ9K_QP_STATUS_PASSES), 2U);
	CHECK_EQ(status_word(0U, ZZ9K_QP_STATUS_PASSES), 1U);

	/* no doorbell queue: nothing scanned */
	CHECK_EQ(detach(CLIENT_A, 2U, 0), SDK_STATUS_OK);
	CHECK_EQ(detach(CLIENT_A, 3U, 0), SDK_STATUS_OK);
	kick(words);
	CHECK_EQ(words[ZZ9K_QP_KICK_QUEUES], 0U);
	CHECK_EQ(words[ZZ9K_QP_KICK_CONSUMED], 0U);
}

static unsigned busy_misses;

/* Every barrier the firmware issues while it works on queue 5 of core 1
 * must happen with the busy word set. */
static void watch_core1_busy(void)
{
	if (queue_state(5U)->busy != 1U)
		busy_misses++;
}

static void test_core1_handshake(void)
{
	struct Amiga am;
	uint32_t words[ZZ9K_QP_STATUS_WORDS];
	unsigned before;
	uint32_t passes;

	reset_all();
	add_buffer(0U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 5U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);
	am_init(&am, 5U, 0U, ZZ9K_QP_ORIGIN_CORE1);
	am_set_mode(&am, ZZ9K_QP_MODE_STREAM);

	/* a pass sets busy around its work */
	busy_misses = 0U;
	before = barriers;
	barrier_hook = watch_core1_busy;
	sdk_qprobe_service_core1();
	barrier_hook = 0;
	CHECK(barriers > before);
	CHECK_EQ(busy_misses, 0U);
	CHECK_EQ(queue_state(5U)->busy, 0U);
	CHECK_EQ(down_head(&am), ZZ9K_QP_ENTRIES);

	/* detach while core 1 is inside a pass: BUSY after the bounded wait,
	 * and the owner no longer touches the pair */
	queue_state(5U)->busy = 1U;
	CHECK_EQ(detach(CLIENT_A, 5U, 0), SDK_STATUS_BUSY);
	CHECK_EQ(queue_state(5U)->attached, 0U);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core1_mask, 0U);
	CHECK(!sdk_qprobe_core1_active());
	passes = status_word(5U, ZZ9K_QP_STATUS_PASSES);
	sdk_qprobe_service_core1();
	CHECK_EQ(status_word(5U, ZZ9K_QP_STATUS_PASSES), passes);
	CHECK_EQ(detach(CLIENT_A, 5U, 0), SDK_STATUS_BUSY);
	CHECK_EQ(sdk_qprobe_detach_handle(0x100U), 0);

	/* core 1 leaves: the detach completes and returns the STATUS words */
	queue_state(5U)->busy = 0U;
	CHECK_EQ(detach(CLIENT_A, 5U, words), SDK_STATUS_OK);
	CHECK_EQ(words[ZZ9K_QP_STATUS_OWNER], ZZ9K_QP_OWNER_CORE1);
	CHECK_EQ(words[ZZ9K_QP_STATUS_DOWN_PRODUCED], ZZ9K_QP_ENTRIES);
	CHECK_EQ(words[ZZ9K_QP_STATUS_PASSES], passes);
	CHECK_EQ(words[ZZ9K_QP_STATUS_RING_FULL], 1U);
	CHECK_EQ(status_of(5U, words), SDK_STATUS_NOT_FOUND);

	/* core 1 parked: its busy word is stale, the pair is safe */
	CHECK_EQ(attach(CLIENT_A, 0U, 5U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);
	queue_state(5U)->busy = 1U;
	shared.core1_parked = 1U;
	CHECK_EQ(detach(CLIENT_A, 5U, 0), SDK_STATUS_OK);
	shared.core1_parked = 0U;

	/* core 1 offline */
	CHECK_EQ(attach(CLIENT_A, 0U, 5U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);
	queue_state(5U)->busy = 1U;
	core1_available = 0;
	CHECK_EQ(detach(CLIENT_A, 5U, 0), SDK_STATUS_OK);
	core1_available = 1;

	/* core 1 read the mask before the detach and arrives after it: the
	 * pass sees attached == 0 and leaves the pair alone */
	CHECK_EQ(attach(CLIENT_A, 0U, 5U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);
	passes = status_word(5U, ZZ9K_QP_STATUS_PASSES);
	queue_state(5U)->attached = 0U;
	memset(fake_mem[0], 0x5a, ZZ9K_QP_PAIR_BYTES);
	sdk_qprobe_service_core1();
	CHECK_EQ(status_word(5U, ZZ9K_QP_STATUS_PASSES), passes);
	CHECK_EQ(queue_state(5U)->busy, 0U);
	CHECK_EQ(fake_mem[0][0], 0x5aU);
	CHECK_EQ(fake_mem[0][ZZ9K_QP_UP_RING_OFFSET], 0x5aU);

	/* a halted core 1 leaves stale busy words behind */
	queue_state(5U)->busy = 1U;
	sdk_qprobe_core1_halted();
	CHECK_EQ(queue_state(5U)->busy, 0U);
}

static void test_teardown(void)
{
	struct Amiga am;
	uint32_t words[ZZ9K_QP_STATUS_WORDS];
	uint32_t before_head;

	reset_all();
	add_buffer(0U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	add_buffer(1U, CLIENT_A, ZZ9K_QP_PAIR_BYTES, 0U);
	add_buffer(2U, CLIENT_B, ZZ9K_QP_PAIR_BYTES, 0U);
	CHECK_EQ(attach(CLIENT_A, 0U, 0U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);
	CHECK_EQ(attach(CLIENT_A, 1U, 1U, ZZ9K_QP_OWNER_CORE1), SDK_STATUS_OK);
	CHECK_EQ(attach(CLIENT_B, 2U, 2U, ZZ9K_QP_OWNER_DOORBELL),
	         SDK_STATUS_OK);
	am_init(&am, 0U, 0U, ZZ9K_QP_ORIGIN_CORE0);
	am_set_mode(&am, ZZ9K_QP_MODE_STREAM);
	sdk_qprobe_poll_core0();
	before_head = down_head(&am);
	CHECK_EQ(before_head, ZZ9K_QP_ENTRIES);

	/* freeing a buffer: its queue is detached, the others stay */
	CHECK_EQ(sdk_qprobe_detach_handle(0x100U), 1);
	CHECK_EQ(status_of(0U, words), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(status_of(1U, words), SDK_STATUS_OK);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 0U);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core1_mask, 2U);
	CHECK_EQ(sdk_qprobe_detach_handle(0x777U), 1); /* no such queue */

	/* a detached queue no longer touches the memory */
	memset(fake_mem[0], 0x33, ZZ9K_QP_PAIR_BYTES);
	sdk_qprobe_poll_core0();
	CHECK_EQ(fake_mem[0][ZZ9K_QP_DOWN_HEAD], 0x33U);
	CHECK_EQ(fake_mem[0][ZZ9K_QP_DOWN_RING_OFFSET], 0x33U);
	CHECK_EQ(sdk_qprobe_detach_handle(0x100U), 1); /* idempotent */

	/* a stuck core 1 refuses the detach, and the queue stays taken */
	queue_state(1U)->busy = 1U;
	CHECK_EQ(sdk_qprobe_detach_handle(0x101U), 0);
	CHECK_EQ(status_of(1U, words), SDK_STATUS_OK);
	CHECK_EQ(attach(CLIENT_A, 1U, 1U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_BUSY);
	CHECK_EQ(sdk_qprobe_reset(), 0);
	CHECK_EQ(status_of(2U, words), SDK_STATUS_NOT_FOUND); /* others gone */
	sdk_qprobe_core1_halted();
	CHECK_EQ(sdk_qprobe_reset(), 1);
	CHECK_EQ(status_of(1U, words), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 0U);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core1_mask, 0U);

	/* the slots are reusable */
	CHECK_EQ(attach(CLIENT_A, 0U, 0U, ZZ9K_QP_OWNER_CORE0), SDK_STATUS_OK);
	CHECK_EQ(status_word(0U, ZZ9K_QP_STATUS_DOWN_PRODUCED), 0U);
}

int main(void)
{
	test_attach_validation();
	test_stream_fills_to_full_and_completions_free_space();
	test_ping_pong_with_full_ring();
	test_up_entry_errors();
	test_kick_serves_only_doorbell_queues();
	test_core1_handshake();
	test_teardown();

	if (failures != 0) {
		printf("qprobe_test: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("qprobe_test: %d checks passed\n", checks);
	return 0;
}
