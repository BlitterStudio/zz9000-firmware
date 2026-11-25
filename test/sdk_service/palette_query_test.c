/*
 * SDK_OP_QUERY_PALETTE and SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY per bus,
 * through the production dispatcher (handle_request) and the real shadow in
 * sdk_palette.c. The shadow is filled by the Zorro III CLUT upload only, so
 * a Zorro II card must neither advertise the flag nor answer the opcode.
 * Only the hardware seams are replaced.
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
#include "sdk_palette.h"

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

static int g_zorro3;
static uint8_t *g_block_base;
static uint32_t g_block_length;

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
uint32_t sdk_aperture_host_window_address(void) { return 0U; }
uint32_t sdk_aperture_host_window_size(void) { return 0U; }

uint32_t surface_allocator_total_bytes(void) { return 64U * 1024U * 1024U; }

uint32_t surface_allocator_alloc_aligned(uint32_t size, uint32_t alignment)
{
	void *p;

	(void)alignment;
	if (g_block_base != NULL)
		return 0U;
	p = mmap(NULL, size, PROT_READ | PROT_WRITE,
	         MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (p == MAP_FAILED)
		return 0U;
	g_block_base = p;
	g_block_length = size;
	return (uint32_t)(uintptr_t)p;
}

int surface_allocator_free(uint32_t addr)
{
	if (g_block_base == NULL || (uint32_t)(uintptr_t)g_block_base != addr)
		return 0;
	munmap(g_block_base, g_block_length);
	g_block_base = NULL;
	return 1;
}

int surface_allocator_owns(uint32_t addr, uint32_t length)
{
	uint32_t base = (uint32_t)(uintptr_t)g_block_base;

	return g_block_base != NULL && addr >= base &&
	       length <= g_block_length && addr - base <= g_block_length - length;
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

static uint16_t call(uint16_t opcode, const uint8_t payload[48],
	uint16_t payload_len)
{
	struct SDKMailboxEntry req;
	uint16_t flags = (uint16_t)(1U << SDK_ENTRY_CLIENT_ID_SHIFT);

	memset(&req, 0, sizeof(req));
	memset(&g_comp, 0, sizeof(g_comp));
	put32(req.request_id, 1U);
	req.opcode[0] = (uint8_t)(opcode >> 8);
	req.opcode[1] = (uint8_t)opcode;
	req.flags[0] = (uint8_t)(flags >> 8);
	req.flags[1] = (uint8_t)flags;
	req.payload_len[0] = (uint8_t)(payload_len >> 8);
	req.payload_len[1] = (uint8_t)payload_len;
	memcpy(req.payload, payload, 48U);
	return sdk_mailbox_test_handle_request(&req, &g_comp);
}

static uint32_t surface_flags(void)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, SDK_SERVICE_SURFACE);
	CHECK(call(SDK_OP_QUERY_SERVICE, payload, 4U) == SDK_STATUS_OK);
	return get32(g_comp.payload + 12);
}

#define PALETTE_BYTES (SDK_PALETTE_ENTRIES * SDK_PALETTE_ENTRY_BYTES)

static uint32_t alloc_dst(void)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload, PALETTE_BYTES);
	put32(payload + 4, 64U);
	if (call(SDK_OP_ALLOC_SHARED, payload, 48U) != SDK_STATUS_OK)
		return 0U;
	return get32(g_comp.payload);
}

static uint16_t query_palette(uint32_t dst, uint32_t start, uint32_t count)
{
	uint8_t payload[48];

	memset(payload, 0, sizeof(payload));
	put32(payload + 4, start);
	put32(payload + 8, count);
	put32(payload + 12, dst);
	return call(SDK_OP_QUERY_PALETTE, payload, 24U);
}

static void test_zorro2(uint32_t dst)
{
	g_zorro3 = 0;
	sdk_palette_reset();
	sdk_palette_set(1U, 0x00112233U);

	/* No flag, and the opcode agrees with it, whatever the shadow holds. */
	CHECK((surface_flags() & SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY) == 0U);
	memset(g_block_base, 0xA5, PALETTE_BYTES);
	CHECK(query_palette(dst, 0U, 4U) == SDK_STATUS_UNSUPPORTED);
	CHECK(g_block_base[0] == 0xA5U && g_block_base[7] == 0xA5U);
	/* Bad arguments do not turn the answer into anything else. */
	CHECK(query_palette(dst, 0U, 0U) == SDK_STATUS_UNSUPPORTED);
	CHECK(query_palette(0U, 0U, 4U) == SDK_STATUS_UNSUPPORTED);
}

static void test_zorro3(uint32_t dst)
{
	g_zorro3 = 1;
	sdk_palette_reset();
	sdk_palette_set(1U, 0x00112233U);

	CHECK((surface_flags() & SDK_SERVICE_FLAG_SURFACE_PALETTE_QUERY) != 0U);
	memset(g_block_base, 0xA5, PALETTE_BYTES);
	CHECK(query_palette(dst, 0U, 4U) == SDK_STATUS_OK);
	CHECK(get32(g_block_base) == 0U);
	CHECK(get32(g_block_base + 4) == 0x00112233U);
	CHECK(get32(g_block_base + 8) == 0U);
	CHECK(get32(g_block_base + 12) == 0U);
	CHECK(g_block_base[16] == 0xA5U);
	/* Arguments are still validated on Zorro III. */
	CHECK(query_palette(dst, 0U, 0U) == SDK_STATUS_BAD_REQUEST);
	CHECK(query_palette(dst, 255U, 2U) == SDK_STATUS_BAD_REQUEST);
	CHECK(query_palette(0U, 0U, 4U) == SDK_STATUS_BAD_HANDLE);
}

int main(void)
{
	uint32_t dst;

	taskq_init(&g_shared.queue);
	g_zorro3 = 1;
	dst = alloc_dst();
	CHECK(dst != 0U);
	if (dst == 0U)
		return 1;
	test_zorro2(dst);
	test_zorro3(dst);
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
