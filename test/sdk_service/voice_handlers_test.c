/*
 * Voice session mailbox handlers: OPEN, CLOSE, RESET, UPLOAD and the
 * client-release stage, driven through the production dispatcher
 * (handle_request) and the real shared-buffer table in sdk_mailbox.c, with
 * the real session and engine modules behind them. Only the hardware seams
 * are replaced: the surface allocator (anonymous low memory), the host
 * window (a mapped block), the aperture queries and the audio fabric
 * producer calls (counted).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "audio_fabric.h"
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

/* ---- hardware seams ---- */

#define WINDOW_SIZE 0x10000U
#define MAX_BLOCKS 16U

static uint8_t *g_window;
static int g_zorro3;

static struct {
	uint8_t *base;
	uint32_t length;
	int live;
} g_block[MAX_BLOCKS];
static unsigned g_alloc_calls;
static unsigned g_free_calls;
static int g_alloc_fail;

static taskq_shared_t g_shared;
taskq_shared_t *scheduler_shared(void) { return &g_shared; }
int scheduler_core1_available(void) { return 1; }
void scheduler_core1_publish(void) {}
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t saved) { (void)saved; }

uint32_t sdk_aperture_runtime_flags(void) { return SDK_APERTURE_FLAG_VALID; }
int sdk_aperture_runtime_is_zorro3(void) { return g_zorro3; }
int sdk_aperture_runtime_is_legacy(void) { return 0; }
uint32_t sdk_aperture_host_window_address(void)
{
	return (uint32_t)(uintptr_t)g_window;
}
uint32_t sdk_aperture_host_window_size(void) { return WINDOW_SIZE; }

uint32_t surface_allocator_total_bytes(void) { return 64U * 1024U * 1024U; }

uint32_t surface_allocator_alloc_aligned(uint32_t size, uint32_t alignment)
{
	unsigned i;
	void *p;

	(void)alignment;
	if (g_alloc_fail)
		return 0U;
	for (i = 0U; i < MAX_BLOCKS; i++) {
		if (g_block[i].live)
			continue;
		p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (p == MAP_FAILED)
			return 0U;
		g_block[i].base = p;
		g_block[i].length = size;
		g_block[i].live = 1;
		g_alloc_calls++;
		return (uint32_t)(uintptr_t)p;
	}
	return 0U;
}

int surface_allocator_free(uint32_t addr)
{
	unsigned i;

	for (i = 0U; i < MAX_BLOCKS; i++) {
		if (g_block[i].live &&
		    (uint32_t)(uintptr_t)g_block[i].base == addr) {
			munmap(g_block[i].base, g_block[i].length);
			g_block[i].live = 0;
			g_free_calls++;
			return 1;
		}
	}
	return 0;
}

int surface_allocator_owns(uint32_t addr, uint32_t length)
{
	unsigned i;

	for (i = 0U; i < MAX_BLOCKS; i++) {
		uint32_t base = (uint32_t)(uintptr_t)g_block[i].base;

		if (g_block[i].live && addr >= base &&
		    length <= g_block[i].length &&
		    addr - base <= g_block[i].length - length)
			return 1;
	}
	return 0;
}

/* Client release stages other than the voice one: nothing of theirs open. */
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
int sdk_video_stream_release_client(uint16_t owner, uint32_t *close_session)
{
	(void)owner;
	*close_session = 0U;
	return 1;
}
int sdk_decompress_stream_release_client(uint16_t owner)
{
	(void)owner;
	return 1;
}

/* Audio fabric producer seam: counted. */
static unsigned g_attach_calls, g_detach_calls, g_rebuild_calls;
static int g_attach_fail;

int audio_fabric_producer_attach(uint32_t slot,
	const struct audio_fabric_producer_ops *ops)
{
	(void)slot;
	(void)ops;
	if (g_attach_fail)
		return 0;
	g_attach_calls++;
	return 1;
}
void audio_fabric_producer_rate_set(uint32_t slot, uint32_t rate)
{
	(void)slot;
	(void)rate;
}
uint32_t audio_fabric_slot_underruns(uint32_t slot)
{
	(void)slot;
	return 0U;
}
void audio_fabric_request_rebuild(uint32_t slot)
{
	(void)slot;
	g_rebuild_calls++;
}
void audio_fabric_producer_detach(uint32_t slot)
{
	(void)slot;
	g_detach_calls++;
}
void audio_fabric_producer_clear(uint32_t slot) { (void)slot; }
/* Fill-ahead seam: the last request, 0 being the default. */
static unsigned g_fill_ahead;
void audio_fabric_fill_ahead_set(uint32_t periods)
{
	g_fill_ahead = periods;
}
int audio_fabric_producer_go_live_when_ready(uint32_t slot, int ready)
{
	(void)slot;
	(void)ready;
	return 1;
}

/* ---- request plumbing ---- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static struct SDKMailboxEntry g_comp;

static uint16_t call(uint16_t opcode, uint16_t client,
	const uint8_t payload[48], uint16_t payload_len)
{
	struct SDKMailboxEntry req;
	uint16_t flags = (uint16_t)(client << SDK_ENTRY_CLIENT_ID_SHIFT);

	memset(&req, 0, sizeof(req));
	memset(&g_comp, 0, sizeof(g_comp));
	put32(req.request_id, 1U);
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

struct buffer {
	uint32_t handle;
	uint8_t *ptr;
	uint32_t length;
};

static struct buffer alloc_buffer_aligned(uint16_t client, uint32_t length,
	uint32_t flags, uint32_t alignment)
{
	uint8_t payload[48];
	struct buffer b = { 0U, NULL, 0U };

	memset(payload, 0, sizeof(payload));
	put32(payload, length);
	put32(payload + 4, alignment);
	put32(payload + 8, flags);
	if (call(SDK_OP_ALLOC_SHARED, client, payload, 48U) ==
	    SDK_STATUS_OK) {
		b.handle = get32(g_comp.payload);
		b.ptr = (uint8_t *)(uintptr_t)get32(g_comp.payload + 4);
		b.length = length;
	}
	return b;
}

/* The SDK allocates every voice buffer at 64 bytes. */
static struct buffer alloc_buffer(uint16_t client, uint32_t length,
	uint32_t flags)
{
	return alloc_buffer_aligned(client, length, flags, 64U);
}

static uint16_t free_buffer(uint16_t client, uint32_t handle)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, handle);
	return call(SDK_OP_FREE_SHARED, client, payload, 48U);
}

static uint16_t release_client(uint16_t client)
{
	return call(SDK_OP_CLIENT_RELEASE, client, NULL, 0U);
}

/* Both clients start and end every test with nothing. */
static void cleanup(void)
{
	(void)release_client(1U);
	(void)release_client(2U);
}

#define RING_LEN 4112U
#define STATUS_LEN 64U
#define STAGING_LEN 4096U
#define BANK_BYTES 8192U

struct fixture {
	struct buffer ring;
	struct buffer status;
	struct buffer staging;
};

static struct fixture make_fixture(uint16_t client, uint32_t flags)
{
	struct fixture f;

	f.ring = alloc_buffer(client, RING_LEN, flags);
	f.status = alloc_buffer(client, STATUS_LEN, flags);
	f.staging = alloc_buffer(client, STAGING_LEN, flags);
	return f;
}

static void open_payload(uint8_t payload[48], const struct fixture *f,
	uint32_t bank, uint32_t lead, uint32_t flags, uint32_t voices)
{
	memset(payload, 0, 48U);
	put32(payload, f->ring.handle);
	put32(payload + 4, f->status.handle);
	put32(payload + 8, f->staging.handle);
	put32(payload + 12, bank);
	put32(payload + 16, lead);
	put32(payload + 20, flags);
	put32(payload + 24, voices);
}

static uint16_t do_open(uint16_t client, const struct fixture *f,
	uint32_t bank, uint32_t lead, uint32_t flags, uint32_t voices)
{
	uint8_t payload[48];

	open_payload(payload, f, bank, lead, flags, voices);
	return call(SDK_OP_AUDIO_VOICE_OPEN, client, payload, 48U);
}

static uint16_t session_op(uint16_t opcode, uint16_t client,
	uint32_t session)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, session);
	return call(opcode, client, payload, 48U);
}

/* Nothing pinned and no bank outstanding: every buffer of the fixture can
 * be freed (a pinned one answers BUSY), and then every allocator block has
 * been given back. */
static int nothing_held(uint16_t client, const struct fixture *f)
{
	uint16_t a = free_buffer(client, f->ring.handle);
	uint16_t b = free_buffer(client, f->status.handle);
	uint16_t c = free_buffer(client, f->staging.handle);

	if (a != SDK_STATUS_OK || b != SDK_STATUS_OK || c != SDK_STATUS_OK ||
	    g_alloc_calls != g_free_calls)
		printf("held: free %u %u %u allocs %u frees %u\n", a, b, c,
		       g_alloc_calls, g_free_calls);
	return a == SDK_STATUS_OK && b == SDK_STATUS_OK &&
	       c == SDK_STATUS_OK && g_alloc_calls == g_free_calls;
}

/* ---- scenarios ---- */

static void test_open_close(void)
{
	struct fixture f;
	uint32_t session;
	uint16_t rc;
	unsigned allocs_before = g_alloc_calls;

	g_zorro3 = 0;
	cleanup();
	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);
	CHECK(f.ring.handle != 0U && f.status.handle != 0U &&
	      f.staging.handle != 0U);
	rc = do_open(1U, &f, BANK_BYTES, 100U, 0U, 8U);
	CHECK(rc == SDK_STATUS_OK);
	session = get32(g_comp.payload);
	CHECK(session != 0U);
	CHECK(get32(g_comp.payload + 4) == 8U);
	CHECK(get32(g_comp.payload + 8) == BANK_BYTES);
	CHECK(get32(g_comp.payload + 12) == 7680U);
	CHECK(get32(g_comp.payload + 16) == 7680U);
	CHECK(get32(g_comp.payload + 20) == 24000U);
	CHECK(get32(g_comp.payload + 24) == 256U);
	CHECK(get32(g_comp.payload + 28) == 48000U);
	CHECK(get32(g_comp.payload + 32) == 256U);
	CHECK(get32(g_comp.payload + 36) == 0U);
	CHECK(g_alloc_calls == allocs_before + 1U && g_attach_calls >= 1U);
	/* Pinned for the life of the session. */
	CHECK(free_buffer(1U, f.ring.handle) == SDK_STATUS_BUSY);
	CHECK(free_buffer(1U, f.status.handle) == SDK_STATUS_BUSY);
	CHECK(free_buffer(1U, f.staging.handle) == SDK_STATUS_BUSY);
	/* A second open is busy and allocates nothing. */
	{
		struct fixture g = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);

		CHECK(do_open(1U, &g, BANK_BYTES, 0U, 0U, 4U) ==
		      SDK_STATUS_BUSY);
		CHECK(g_alloc_calls == allocs_before + 1U);
		CHECK(free_buffer(1U, g.ring.handle) == SDK_STATUS_OK);
		CHECK(free_buffer(1U, g.status.handle) == SDK_STATUS_OK);
		CHECK(free_buffer(1U, g.staging.handle) == SDK_STATUS_OK);
	}
	/* Only the owner, with the right id, closes. */
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session + 1U) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 2U, session) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_RESET, 2U, session) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(free_buffer(1U, f.ring.handle) == SDK_STATUS_BUSY);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_RESET, 1U, session) ==
	      SDK_STATUS_OK);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_OK);
	CHECK(g_free_calls >= 1U && g_alloc_calls == g_free_calls);
	CHECK(g_detach_calls >= 1U);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(nothing_held(1U, &f));
	cleanup();
}

/* The open flag reaches the session: the grant reports the short floor and
 * the fabric is asked for the short fill-ahead, which close and the owner's
 * release give back. A session without the flag asks for nothing. */
static void test_open_low_latency(void)
{
	struct fixture f;
	uint32_t session;

	g_zorro3 = 0;
	cleanup();
	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);
	g_fill_ahead = 0U;
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 4U, 8U) == SDK_STATUS_OK);
	session = get32(g_comp.payload);
	CHECK(get32(g_comp.payload + 12) == 2880U);
	CHECK(get32(g_comp.payload + 16) == 2880U);
	CHECK(g_fill_ahead == 2U);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_OK);
	CHECK(g_fill_ahead == 0U);
	CHECK(nothing_held(1U, &f));

	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 4U, 8U) == SDK_STATUS_OK);
	CHECK(g_fill_ahead == 2U);
	CHECK(release_client(1U) == SDK_STATUS_OK);
	CHECK(g_fill_ahead == 0U);

	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 8U) == SDK_STATUS_OK);
	session = get32(g_comp.payload);
	CHECK(get32(g_comp.payload + 16) == 7680U);
	CHECK(g_fill_ahead == 0U);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_OK);
	CHECK(nothing_held(1U, &f));
	cleanup();
}

static void test_open_refused(void)
{
	struct fixture f;
	struct fixture other;
	struct fixture card_only;
	uint8_t payload[48];
	struct buffer small;
	struct buffer heap_status;

	g_zorro3 = 0;
	cleanup();
	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);

	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 8U, 4U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 0U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 33U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(do_open(1U, &f, 0U, 0U, 0U, 4U) == SDK_STATUS_BAD_REQUEST);
	CHECK(do_open(1U, &f, 32U * 1024U * 1024U + 1U, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_REQUEST);
	/* Reserved bytes and a short payload. */
	open_payload(payload, &f, BANK_BYTES, 0U, 0U, 4U);
	payload[47] = 1U;
	CHECK(call(SDK_OP_AUDIO_VOICE_OPEN, 1U, payload, 48U) ==
	      SDK_STATUS_BAD_REQUEST);
	open_payload(payload, &f, BANK_BYTES, 0U, 0U, 4U);
	CHECK(call(SDK_OP_AUDIO_VOICE_OPEN, 1U, payload, 24U) ==
	      SDK_STATUS_BAD_REQUEST);
	/* Another client's buffers. */
	CHECK(do_open(2U, &f, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_HANDLE);
	/* A handle used twice, or one that does not exist. */
	other = f;
	other.status = f.ring;
	CHECK(do_open(1U, &other, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_HANDLE);
	other = f;
	other.staging.handle = 0x7777U;
	CHECK(do_open(1U, &other, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_HANDLE);
	/* Not host-window on Zorro II, or declared card-only. */
	other = f;
	heap_status = alloc_buffer(1U, STATUS_LEN, 0U);
	other.status = heap_status;
	CHECK(heap_status.handle != 0U);
	CHECK(do_open(1U, &other, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_HANDLE);
	card_only = f;
	card_only.staging = alloc_buffer(1U, STAGING_LEN,
		SDK_ALLOC_HOST_WINDOW | SDK_ALLOC_CARD_ONLY);
	CHECK(card_only.staging.handle != 0U);
	CHECK(do_open(1U, &card_only, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_HANDLE);
	/* A status block under 64 bytes. */
	small = alloc_buffer(1U, 32U, SDK_ALLOC_HOST_WINDOW);
	other = f;
	other.status = small;
	CHECK(do_open(1U, &other, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_REQUEST);
	/* A status block that starts inside a cache line: the firmware
	 * flushes it, so it would write a neighbour's line back. */
	{
		struct buffer pad[8];
		unsigned pads = 0U;
		unsigned p;

		other = f;
		for (;;) {
			other.status = alloc_buffer_aligned(1U, STATUS_LEN,
				SDK_ALLOC_HOST_WINDOW, 16U);
			if (other.status.handle == 0U ||
			    ((uintptr_t)other.status.ptr & 31U) != 0U ||
			    pads + 2U > 8U)
				break;
			pad[pads++] = other.status;
			pad[pads++] = alloc_buffer_aligned(1U, 16U,
				SDK_ALLOC_HOST_WINDOW, 16U);
		}
		CHECK(other.status.handle != 0U &&
		      ((uintptr_t)other.status.ptr & 31U) == 16U);
		CHECK(do_open(1U, &other, BANK_BYTES, 0U, 0U, 4U) ==
		      SDK_STATUS_BAD_REQUEST);
		CHECK(free_buffer(1U, other.status.handle) == SDK_STATUS_OK);
		for (p = 0U; p < pads; p++)
			CHECK(free_buffer(1U, pad[p].handle) == SDK_STATUS_OK);
	}
	/* A ring under 16 records. */
	small = alloc_buffer(1U, 16U + 15U * 16U, SDK_ALLOC_HOST_WINDOW);
	other = f;
	other.ring = small;
	CHECK(do_open(1U, &other, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_REQUEST);
	/* No memory for the bank; no fabric slot. */
	g_alloc_fail = 1;
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_NO_MEMORY);
	g_alloc_fail = 0;
	g_attach_fail = 1;
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 4U) == SDK_STATUS_BUSY);
	g_attach_fail = 0;
	/* The test's own extra buffers go back first. */
	CHECK(free_buffer(1U, card_only.staging.handle) == SDK_STATUS_OK);
	CHECK(free_buffer(1U, heap_status.handle) == SDK_STATUS_OK);
	/* Nothing was allocated, pinned or left open by any of that. */
	CHECK(nothing_held(1U, &f));
	cleanup();
	CHECK(g_alloc_calls == g_free_calls);
}

static void test_zorro3_buffers(void)
{
	struct fixture f;
	struct fixture card_only;
	uint32_t session;

	g_zorro3 = 1;
	cleanup();
	/* zz9k.library strips HOST_WINDOW on Zorro III: plain shared buffers. */
	f = make_fixture(1U, 0U);
	CHECK(f.ring.handle != 0U);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 4U) == SDK_STATUS_OK);
	session = get32(g_comp.payload);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_OK);
	card_only = f;
	card_only.ring = alloc_buffer(1U, RING_LEN, SDK_ALLOC_CARD_ONLY);
	CHECK(card_only.ring.handle != 0U);
	CHECK(do_open(1U, &card_only, BANK_BYTES, 0U, 0U, 4U) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(free_buffer(1U, card_only.ring.handle) == SDK_STATUS_OK);
	CHECK(nothing_held(1U, &f));
	g_zorro3 = 0;
	cleanup();
}

/* QUERY_SERVICE for the audio service through the production dispatcher:
 * the voice flag follows the bus and the opcode count covers the range. */
static void test_discovery_flag(void)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, SDK_SERVICE_AUDIO);

	g_zorro3 = 1;
	CHECK(call(SDK_OP_QUERY_SERVICE, 1U, payload, 4U) == SDK_STATUS_OK);
	CHECK((get32(g_comp.payload + 12) & SDK_SERVICE_FLAG_AUDIO_VOICE) != 0U);
	CHECK(get32(g_comp.payload + 20) == 28U);

	g_zorro3 = 0;
	CHECK(call(SDK_OP_QUERY_SERVICE, 1U, payload, 4U) == SDK_STATUS_OK);
	CHECK((get32(g_comp.payload + 12) & SDK_SERVICE_FLAG_AUDIO_VOICE) == 0U);
	CHECK(get32(g_comp.payload + 20) == 28U);
}

static void test_upload(void)
{
	struct fixture f;
	uint8_t payload[48];
	uint32_t session;
	uint8_t *bank = NULL;
	unsigned i;

	g_zorro3 = 0;
	cleanup();
	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 4U) == SDK_STATUS_OK);
	session = get32(g_comp.payload);
	for (i = 0U; i < MAX_BLOCKS; i++) {
		if (g_block[i].live && g_block[i].length == BANK_BYTES)
			bank = g_block[i].base;
	}
	CHECK(bank != NULL);
	if (bank == NULL)
		return;
	for (i = 0U; i < 64U; i++)
		f.staging.ptr[i] = (uint8_t)(i + 1U);

	memset(payload, 0, sizeof(payload));
	put32(payload, session);
	put32(payload + 4, 3U); /* COPY | DEFINE */
	put32(payload + 16, 64U);
	put32(payload + 20, 9U);
	put32(payload + 28, 64U);
	put32(payload + 32, 0U);
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 48U) ==
	      SDK_STATUS_OK);
	CHECK(memcmp(bank, f.staging.ptr, 64U) == 0);
	/* The stereo formats are accepted; format 4 is refused and copies
	 * nothing. */
	put32(payload + 32, 3U);
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 48U) ==
	      SDK_STATUS_OK);
	memset(bank, 0, 64U);
	put32(payload + 32, 4U);
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 48U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(bank[0] == 0U && bank[63] == 0U);
	put32(payload + 32, 0U);
	/* Out of range: refused, nothing copied. */
	memset(bank, 0, BANK_BYTES);
	put32(payload + 12, BANK_BYTES - 8U);
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 48U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(bank[BANK_BYTES - 8U] == 0U);
	put32(payload + 12, 0U);
	/* Reserved bytes, the wrong client, the wrong session. */
	payload[47] = 1U;
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 48U) ==
	      SDK_STATUS_BAD_REQUEST);
	payload[47] = 0U;
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 2U, payload, 48U) ==
	      SDK_STATUS_BAD_HANDLE);
	put32(payload, session + 1U);
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 48U) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(call(SDK_OP_AUDIO_VOICE_UPLOAD, 1U, payload, 12U) ==
	      SDK_STATUS_BAD_REQUEST);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_OK);
	cleanup();
}

static void test_client_release(void)
{
	struct fixture f;
	uint32_t session;
	unsigned frees_before;

	g_zorro3 = 0;
	cleanup();
	f = make_fixture(1U, SDK_ALLOC_HOST_WINDOW);
	CHECK(do_open(1U, &f, BANK_BYTES, 0U, 0U, 4U) == SDK_STATUS_OK);
	session = get32(g_comp.payload);
	frees_before = g_free_calls;
	/* Freeing the ring while the session is open is busy. */
	CHECK(free_buffer(1U, f.ring.handle) == SDK_STATUS_BUSY);
	/* Another client's release does not touch the session. */
	CHECK(release_client(2U) == SDK_STATUS_OK);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_RESET, 1U, session) ==
	      SDK_STATUS_OK);
	CHECK(g_free_calls == frees_before);
	/* The owner's release closes it, frees the bank and the buffers. */
	CHECK(release_client(1U) == SDK_STATUS_OK);
	CHECK(g_free_calls > frees_before && g_alloc_calls == g_free_calls);
	CHECK(session_op(SDK_OP_AUDIO_VOICE_CLOSE, 1U, session) ==
	      SDK_STATUS_BAD_HANDLE);
	CHECK(free_buffer(1U, f.ring.handle) == SDK_STATUS_BAD_HANDLE);
}

int main(void)
{
	g_window = mmap(NULL, WINDOW_SIZE, PROT_READ | PROT_WRITE,
	                MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	CHECK(g_window != MAP_FAILED);
	/* The audio-stream table lives at a fixed firmware address; the
	 * client-release chain reads it. */
	CHECK(mmap((void *)(uintptr_t)SDK_AUDIO_STREAMS_ADDRESS,
	           SDK_AUDIO_STREAMS_MAX_BYTES, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
	           0) != MAP_FAILED);
	if (failures != 0)
		return 1;
	taskq_init(&g_shared.queue);
	test_open_close();
	test_open_low_latency();
	test_open_refused();
	test_zorro3_buffers();
	test_upload();
	test_discovery_flag();
	test_client_release();
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
