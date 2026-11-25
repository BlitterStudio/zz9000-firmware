/*
 * The retired legacy video-session ops 0x0b00..0x0b03 (begin, write, decode,
 * close), driven through the production dispatcher (handle_request).
 *
 * The numbers stay reserved: the firmware answers every one of them
 * SDK_STATUS_UNSUPPORTED, whatever the payload length or request id, and it
 * never defers them to core 1. The media-session ops next to them
 * (0x0b04..0x0b0d) and CLIENT_RELEASE keep their own routes; their
 * behaviour is covered by media_pump_release_test and test/media.
 *
 * Only the hardware seams are replaced, as in media_pump_release_test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "memorymap.h"
#include "scheduler.h"
#include "sdk_aperture_layout.h"
#include "sdk_mailbox.h"

struct SDKMailboxEntry {
	uint8_t request_id[4];
	uint8_t opcode[2];
	uint8_t status[2];
	uint8_t flags[2];
	uint8_t payload_len[2];
	uint8_t user_cookie[4];
	uint8_t payload[48];
};

extern uint16_t sdk_mailbox_test_handle_request(
	volatile struct SDKMailboxEntry *req,
	volatile struct SDKMailboxEntry *comp);

static int checks;
static int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

#define CLIENT 1U

/* ---- hardware and module seams ---- */

static taskq_shared_t g_shared;
taskq_shared_t *scheduler_shared(void) { return &g_shared; }
int scheduler_core1_available(void) { return 1; }
void scheduler_core1_publish(void) {}
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t saved) { (void)saved; }

uint32_t sdk_aperture_runtime_flags(void) { return SDK_APERTURE_FLAG_VALID; }
int sdk_aperture_runtime_is_zorro3(void) { return 1; }
int sdk_aperture_runtime_is_legacy(void) { return 0; }
uint32_t surface_allocator_total_bytes(void) { return 64U * 1024U * 1024U; }

/* Client release stages: nothing of the client's is open. */
int sdk_image_stream_release_client(uint16_t owner, uint32_t *close_session)
{
	(void)owner;
	*close_session = 0U;
	return 1;
}
int sdk_media_session_release_client(uint16_t owner, uint32_t *close_session)
{
	(void)owner;
	*close_session = 0U;
	return 1;
}
int sdk_media_session_owned_by(uint32_t session, uint16_t owner)
{
	(void)session;
	(void)owner;
	return 0;
}
int sdk_decompress_stream_release_client(uint16_t owner)
{
	(void)owner;
	return 1;
}
int audio_voice_session_active(void) { return 0; }

/* ---- request plumbing ---- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static struct SDKMailboxEntry g_comp;

static uint16_t call(uint16_t opcode, uint32_t request_id,
	const uint8_t payload[48], uint16_t payload_len)
{
	struct SDKMailboxEntry req;
	uint16_t flags = (uint16_t)(CLIENT << SDK_ENTRY_CLIENT_ID_SHIFT);

	memset(&req, 0, sizeof(req));
	memset(&g_comp, 0, sizeof(g_comp));
	put32(req.request_id, request_id);
	req.opcode[0] = (uint8_t)(opcode >> 8);
	req.opcode[1] = (uint8_t)opcode;
	req.flags[0] = (uint8_t)(flags >> 8);
	req.flags[1] = (uint8_t)flags;
	req.payload_len[0] = (uint8_t)(payload_len >> 8);
	req.payload_len[1] = (uint8_t)payload_len;
	if (payload != NULL)
		memcpy(req.payload, payload, 48U);
	return sdk_mailbox_test_handle_request(&req, &g_comp);
}

static uint16_t comp_status(void)
{
	return (uint16_t)(((uint16_t)g_comp.status[0] << 8) | g_comp.status[1]);
}

static uint16_t comp_opcode(void)
{
	return (uint16_t)(((uint16_t)g_comp.opcode[0] << 8) | g_comp.opcode[1]);
}

/* Every retired opcode, with the payload shapes the old handlers accepted
 * and refused, and with request id zero (reserved for the old deferred ops). */
static void test_retired_ops_answer_unsupported(void)
{
	static const uint16_t retired[] = { 0x0b00U, 0x0b01U, 0x0b02U, 0x0b03U };
	static const uint16_t lengths[] = { 0U, 8U, 48U };
	static const uint32_t request_ids[] = { 0U, 1U, 0x7fffffffU };
	uint8_t payload[48];
	unsigned i, j, k;

	memset(payload, 0, sizeof(payload));
	/* A well-formed old BEGIN: MPEG-1 in a Program Stream, 320x240. */
	put32(payload + 0, 1U);
	put32(payload + 4, 1U);
	put32(payload + 8, 320U);
	put32(payload + 12, 240U);
	put32(payload + 16, 1U);
	for (i = 0U; i < sizeof(retired) / sizeof(retired[0]); i++) {
		for (j = 0U; j < sizeof(lengths) / sizeof(lengths[0]); j++) {
			for (k = 0U;
			     k < sizeof(request_ids) / sizeof(request_ids[0]);
			     k++) {
				uint16_t status = call(retired[i], request_ids[k],
				                       payload, lengths[j]);

				CHECK(status == SDK_STATUS_UNSUPPORTED);
				CHECK(comp_status() == SDK_STATUS_UNSUPPORTED);
				CHECK(comp_opcode() == retired[i]);
			}
		}
	}
}

/* Nothing is queued for core 1 by a retired op. */
static void test_retired_ops_queue_nothing(void)
{
	uint8_t payload[48];
	unsigned i;
	unsigned queued = 0U;

	memset(payload, 0, sizeof(payload));
	put32(payload, 1U);
	for (i = 0U; i < 4U; i++)
		(void)call((uint16_t)(0x0b00U + i), 1U, payload, 48U);
	for (i = 0U; i < TASKQ_CAPACITY; i++) {
		if (g_shared.queue.descs[i].state != TASK_FREE)
			queued++;
	}
	CHECK(queued == 0U);
}

/* A client that never opened a video session still releases cleanly: the
 * release path has no video-session stage left to refuse it. */
static void test_client_release_without_video_stage(void)
{
	CHECK(call(SDK_OP_CLIENT_RELEASE, 1U, NULL, 0U) == SDK_STATUS_OK);
	CHECK(comp_status() == SDK_STATUS_OK);
}

int main(void)
{
	/* The audio-stream table lives at a fixed firmware address; the
	 * client-release chain reads it. */
	CHECK(mmap((void *)(uintptr_t)SDK_AUDIO_STREAMS_ADDRESS,
	           SDK_AUDIO_STREAMS_MAX_BYTES, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
	           0) != MAP_FAILED);
	if (failures != 0)
		return 1;
	taskq_init(&g_shared.queue);
	test_retired_ops_answer_unsupported();
	test_retired_ops_queue_nothing();
	test_client_release_without_video_stage();
	if (failures != 0) {
		printf("retired_video_session_test: %d of %d checks FAILED\n",
		       failures, checks);
		return 1;
	}
	printf("retired_video_session_test: %d checks passed\n", checks);
	return 0;
}
