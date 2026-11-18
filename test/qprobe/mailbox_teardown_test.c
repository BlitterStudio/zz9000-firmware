/*
 * The probe against the production mailbox code: the DIAG_QPROBE handler with
 * its shared-buffer lookup, and every path in sdk_mailbox.c that frees a
 * shared-buffer slot (FREE_SHARED, CLIENT_RELEASE's buffer step, the
 * allocator sanitiser) must detach a probe queue on the buffer first. The
 * Amiga-reset path (sdk_mailbox_init) is covered through sdk_qprobe_reset in
 * qprobe_test.c; it needs the whole firmware to run.
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

struct SDKMailboxEntry {
	uint8_t request_id[4];
	uint8_t opcode[2];
	uint8_t status[2];
	uint8_t flags[2];
	uint8_t payload_len[2];
	uint8_t user_cookie[4];
	uint8_t payload[48];
};

extern void sdk_mailbox_test_buffer_add(uint32_t handle, uint32_t address,
                                        uint32_t length, uint32_t flags,
                                        uint16_t owner_id);
extern uint16_t sdk_mailbox_test_free_shared(
    volatile struct SDKMailboxEntry *req,
    volatile struct SDKMailboxEntry *comp, uint16_t payload_len);
extern int sdk_mailbox_test_client_buffers_released(uint16_t client_id);
extern uint32_t sdk_mailbox_test_sanitize(void);
extern uint16_t sdk_mailbox_test_qprobe(volatile struct SDKMailboxEntry *req,
                                        volatile struct SDKMailboxEntry *comp,
                                        uint16_t payload_len);

static int checks;
static int failures;

#define CHECK_EQ(a, b) do { \
	unsigned long va_ = (unsigned long)(a); \
	unsigned long vb_ = (unsigned long)(b); \
	checks++; \
	if (va_ != vb_) { \
		failures++; \
		printf("FAIL %s:%d: %s == %s (%lu vs %lu)\n", __FILE__, __LINE__, \
		       #a, #b, va_, vb_); \
	} \
} while (0)

XTime qprobe_mock_time;
XTime qprobe_mock_tick;
unsigned qprobe_mock_cache_ops;
uint8_t qprobe_host_state[0x2000] __attribute__((aligned(64)));

void qprobe_mock_dmb(void) {}

static taskq_shared_t shared;

taskq_shared_t *scheduler_shared(void)
{
	return &shared;
}

int scheduler_core1_available(void)
{
	return 1;
}

#define FAKE_BASE 0x08000000U
#define FAKE_STRIDE 0x00010000U
static uint8_t fake_mem[4][FAKE_STRIDE] __attribute__((aligned(4096)));

volatile void *qprobe_host_ptr(uint32_t address)
{
	return fake_mem[(address - FAKE_BASE) / FAKE_STRIDE] +
	       (address - FAKE_BASE) % FAKE_STRIDE;
}

/* The allocator the mailbox frees into. */
static uint32_t freed[8];
static unsigned freed_count;
static uint32_t dead_address;

int surface_allocator_owns(uint32_t address, uint32_t length)
{
	(void)length;
	return address != dead_address;
}

int surface_allocator_free(uint32_t address)
{
	freed[freed_count++ % 8U] = address;
	return 1;
}

#define CLIENT_A 5U
#define CLIENT_B 9U

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t entry_status(const struct SDKMailboxEntry *entry)
{
	return ((uint32_t)entry->status[0] << 8) | entry->status[1];
}

static void begin_request(struct SDKMailboxEntry *req, uint32_t client)
{
	memset(req, 0, sizeof(*req));
	put32(req->request_id, 1U);
	req->flags[0] = (uint8_t)((client << 5) >> 8);
	req->flags[1] = (uint8_t)(client << 5);
}

static uint32_t qprobe(uint32_t client, uint32_t cmd, uint32_t a, uint32_t b,
                       uint32_t c, struct SDKMailboxEntry *comp)
{
	struct SDKMailboxEntry req;

	begin_request(&req, client);
	put32(req.payload, cmd);
	put32(req.payload + 4, a);
	put32(req.payload + 8, b);
	put32(req.payload + 12, c);
	req.payload_len[1] = 16U;
	memset(comp, 0, sizeof(*comp));
	return sdk_mailbox_test_qprobe(&req, comp, 16U);
}

static uint32_t free_shared(uint32_t client, uint32_t handle)
{
	struct SDKMailboxEntry req;
	struct SDKMailboxEntry comp;
	uint32_t status;

	begin_request(&req, client);
	put32(req.payload, handle);
	req.payload_len[1] = 4U;
	memset(&comp, 0, sizeof(comp));
	status = sdk_mailbox_test_free_shared(&req, &comp, 4U);
	CHECK_EQ(entry_status(&comp), status);
	return status;
}

static uint32_t queue_status(uint32_t queue)
{
	struct SDKMailboxEntry comp;

	return qprobe(CLIENT_B, ZZ9K_QP_CMD_STATUS, queue, 0U, 0U, &comp);
}

static void setup(void)
{
	memset(qprobe_host_state, 0, sizeof(qprobe_host_state));
	memset(&shared, 0, sizeof(shared));
	qprobe_mock_time = 0U;
	qprobe_mock_tick = (XTime)COUNTS_PER_SECOND / 1000U;
	freed_count = 0U;
	dead_address = 0U;
	(void)sdk_qprobe_reset();
}

static struct SDKQProbeQueue *queue_state(uint32_t index)
{
	return &SDK_QPROBE_STATE->queue[index];
}

int main(void)
{
	struct SDKMailboxEntry comp;
	uint32_t status;

	setup();
	sdk_mailbox_test_buffer_add(0x100U, FAKE_BASE, ZZ9K_QP_PAIR_BYTES, 0U,
	                            CLIENT_A);
	sdk_mailbox_test_buffer_add(0x101U, FAKE_BASE + FAKE_STRIDE,
	                            ZZ9K_QP_PAIR_BYTES, 0U, CLIENT_A);
	sdk_mailbox_test_buffer_add(0x102U, FAKE_BASE + 2U * FAKE_STRIDE,
	                            ZZ9K_QP_PAIR_BYTES, 0U, CLIENT_B);
	sdk_mailbox_test_buffer_add(0x103U, FAKE_BASE + 3U * FAKE_STRIDE,
	                            ZZ9K_QP_PAIR_BYTES, 0U, CLIENT_A);

	/* the handler: reply words, and the client check on the handle */
	status = qprobe(CLIENT_A, ZZ9K_QP_CMD_ATTACH, 0x100U, 0U,
	                ZZ9K_QP_OWNER_CORE0, &comp);
	CHECK_EQ(status, SDK_STATUS_OK);
	CHECK_EQ(entry_status(&comp), SDK_STATUS_OK);
	CHECK_EQ(comp.payload_len[1], 12U);
	CHECK_EQ(get32(comp.payload + 4U * ZZ9K_QP_ATTACH_ARM_ADDR), FAKE_BASE);
	CHECK_EQ(get32(comp.payload + 4U * ZZ9K_QP_ATTACH_PAIR_BYTES),
	         ZZ9K_QP_PAIR_BYTES);
	CHECK_EQ(qprobe(CLIENT_A, ZZ9K_QP_CMD_ATTACH, 0x102U, 1U,
	                ZZ9K_QP_OWNER_CORE0, &comp),
	         SDK_STATUS_BAD_HANDLE);
	CHECK_EQ(comp.payload_len[1], 0U);
	CHECK_EQ(qprobe(CLIENT_A, ZZ9K_QP_CMD_ATTACH, 0x555U, 1U,
	                ZZ9K_QP_OWNER_CORE0, &comp),
	         SDK_STATUS_BAD_HANDLE);
	CHECK_EQ(qprobe(CLIENT_A, ZZ9K_QP_CMD_ATTACH, 0x101U, 1U,
	                ZZ9K_QP_OWNER_CORE1, &comp),
	         SDK_STATUS_OK);
	CHECK_EQ(qprobe(CLIENT_A, ZZ9K_QP_CMD_KICK, 0U, 0U, 0U, &comp),
	         SDK_STATUS_OK);
	CHECK_EQ(comp.payload_len[1], 4U * ZZ9K_QP_KICK_WORDS);
	CHECK_EQ(get32(comp.payload + 4U * ZZ9K_QP_KICK_TIMER_HZ),
	         (uint32_t)COUNTS_PER_SECOND);

	/* FREE_SHARED detaches first: the queue is gone, the buffer freed */
	CHECK_EQ(free_shared(CLIENT_A, 0x100U), SDK_STATUS_OK);
	CHECK_EQ(freed_count, 1U);
	CHECK_EQ(freed[0], FAKE_BASE);
	CHECK_EQ(queue_status(0U), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(queue_status(1U), SDK_STATUS_OK);
	CHECK_EQ(SDK_QPROBE_STATE->summary.core0_mask, 0U);

	/* core 1 still inside the pair: the buffer stays, the client is told
	 * to retry */
	queue_state(1U)->busy = 1U;
	CHECK_EQ(free_shared(CLIENT_A, 0x101U), SDK_STATUS_BUSY);
	CHECK_EQ(freed_count, 1U);
	CHECK_EQ(queue_status(1U), SDK_STATUS_OK);
	CHECK_EQ(queue_state(1U)->attached, 0U);
	queue_state(1U)->busy = 0U;
	CHECK_EQ(free_shared(CLIENT_A, 0x101U), SDK_STATUS_OK);
	CHECK_EQ(freed_count, 2U);
	CHECK_EQ(queue_status(1U), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(free_shared(CLIENT_A, 0x101U), SDK_STATUS_BAD_HANDLE);

	/* CLIENT_RELEASE frees every buffer of the client, detaching each */
	CHECK_EQ(qprobe(CLIENT_A, ZZ9K_QP_CMD_ATTACH, 0x103U, 2U,
	                ZZ9K_QP_OWNER_CORE1, &comp),
	         SDK_STATUS_OK);
	CHECK_EQ(qprobe(CLIENT_B, ZZ9K_QP_CMD_ATTACH, 0x102U, 3U,
	                ZZ9K_QP_OWNER_DOORBELL, &comp),
	         SDK_STATUS_OK);
	queue_state(2U)->busy = 1U;
	CHECK_EQ(sdk_mailbox_test_client_buffers_released(CLIENT_A), 0);
	CHECK_EQ(freed_count, 2U);
	CHECK_EQ(queue_status(2U), SDK_STATUS_OK);
	queue_state(2U)->busy = 0U;
	CHECK_EQ(sdk_mailbox_test_client_buffers_released(CLIENT_A), 1);
	CHECK_EQ(freed_count, 3U);
	CHECK_EQ(freed[2], FAKE_BASE + 3U * FAKE_STRIDE);
	CHECK_EQ(queue_status(2U), SDK_STATUS_NOT_FOUND);
	CHECK_EQ(queue_status(3U), SDK_STATUS_OK); /* another client's queue */

	/* the allocator sanitiser drops a dead slot and its queue */
	dead_address = FAKE_BASE + 2U * FAKE_STRIDE;
	CHECK_EQ(sdk_mailbox_test_sanitize(), 1U);
	CHECK_EQ(queue_status(3U), SDK_STATUS_NOT_FOUND);

	if (failures != 0) {
		printf("mailbox_teardown_test: %d of %d checks failed\n", failures,
		       checks);
		return 1;
	}
	printf("mailbox_teardown_test: %d checks passed\n", checks);
	return 0;
}
