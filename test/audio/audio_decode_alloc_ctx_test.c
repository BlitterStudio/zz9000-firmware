/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The core-1 SDK and the core-0 audio-ring decoder run libFLAC and Tremor at
 * the same time. Their allocator contexts (quota, owner, failure flag, arena
 * selection and out-of-memory jump target) must not leak between cores.
 * The tests drive the production allocators with a settable core id. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk_compression.h"
#include "sdk_flac_alloc.h"
#include "sdk_vorbis_alloc.h"

static int g_cpu = 1;
static int failures;

#define CHECK(ok, name) \
	do { \
		if (!(ok)) { \
			failures++; \
			printf("FAILED: %s (line %d)\n", (name), __LINE__); \
		} \
	} while (0)

int smp_cpu_id(void) { return g_cpu; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t s) { (void)s; }
void smp_raw_spin_lock(volatile uint32_t *w) { *w = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *w)
{
	if (*w)
		return 0;
	*w = 1U;
	return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *w) { *w = 0U; }

static uint32_t g_last_owner;
static unsigned g_allocs;

void *sdk_decode_heap_alloc_owned(size_t size, uint32_t owner)
{
	g_last_owner = owner;
	g_allocs++;
	return malloc(size);
}

void *sdk_decode_heap_alloc(size_t size)
{
	return sdk_decode_heap_alloc_owned(size, SDK_DECODE_POOL_OWNER);
}

void sdk_decode_heap_free(void *ptr) { free(ptr); }

static void test_flac(void)
{
	size_t used0 = 0U, peak0 = 0U, used1 = 0U, peak1 = 0U;
	void *a, *b, *c;

	g_cpu = 0;
	sdk_flac_alloc_set_quota(&used0, &peak0, 1000U);
	sdk_flac_alloc_set_owner(0x11U);
	g_cpu = 1;
	sdk_flac_alloc_set_quota(&used1, &peak1, 4000U);
	sdk_flac_alloc_set_owner(0x22U);

	g_cpu = 0;
	a = sdk_flac_malloc(100U);
	CHECK(a != NULL && g_last_owner == 0x11U, "core 0 keeps its owner");
	CHECK(used0 != 0U && peak0 == used0 && used1 == 0U,
	      "core 0 allocation is charged to core 0's quota");
	g_cpu = 1;
	b = sdk_flac_malloc(2000U);
	CHECK(b != NULL && g_last_owner == 0x22U, "core 1 keeps its owner");
	CHECK(used1 > 2000U && used0 < 1000U, "core 1 allocation is charged to core 1");

	g_cpu = 0;
	c = sdk_flac_malloc(2000U);
	CHECK(c == NULL, "core 0 limit is not replaced by core 1's");
	CHECK(sdk_flac_alloc_failed() == 1, "core 0 sees its failure");
	g_cpu = 1;
	CHECK(sdk_flac_alloc_failed() == 0, "core 0 failure is not core 1's");
	sdk_flac_alloc_set_quota(&used1, &peak1, 4000U);
	g_cpu = 0;
	CHECK(sdk_flac_alloc_failed() == 1,
	      "core 1 quota setup does not clear core 0's failure");
	CHECK(sdk_flac_realloc(a, 5000U) == NULL &&
	      sdk_flac_alloc_failed() == 1, "core 0 realloc failure");
	g_cpu = 1;
	CHECK(sdk_flac_alloc_failed() == 0, "realloc failure stays on core 0");

	/* A block frees against the quota it was charged to, from any core. */
	sdk_flac_free(a);
	CHECK(used0 == 0U, "free credits the owning quota");
	sdk_flac_free(b);
	CHECK(used1 == 0U, "free credits core 1's quota");
	g_cpu = 1;
}

static volatile int g_landed;

static void vorbis_oom_on_core1(struct sdk_vorbis_heap *heap)
{
	jmp_buf fail;

	g_cpu = 1;
	g_landed = 0;
	sdk_vorbis_heap_select(heap, &fail);
	if (setjmp(fail) == 0) {
		(void)sdk_vorbis_malloc(100000U);
		g_landed = -1;
	} else {
		g_landed = 1;
	}
}

static void test_vorbis(void)
{
	struct sdk_vorbis_heap h0, h1;
	jmp_buf fail0;
	void *a, *b;

	sdk_vorbis_heap_init(&h0, 65536U, 262144U, 0U);
	sdk_vorbis_heap_init(&h1, 4096U, 4096U, 0U);

	g_cpu = 0;
	sdk_vorbis_heap_select(&h0, &fail0);
	a = sdk_vorbis_malloc(100U);
	CHECK(a != NULL && sdk_vorbis_heap_regions(&h0) == 1U,
	      "core 0 allocates from its arena");

	vorbis_oom_on_core1(&h1);
	CHECK(g_landed == 1, "core 1 out of memory jumps to core 1's target");
	CHECK(sdk_vorbis_heap_regions(&h1) == 0U && sdk_vorbis_heap_regions(&h0) == 1U,
	      "core 1 failure leaves core 0's arena alone");

	/* Core 1 ends its call; that must not clear core 0's selection. */
	g_cpu = 1;
	sdk_vorbis_heap_select(NULL, NULL);
	g_cpu = 0;
	b = sdk_vorbis_malloc(100U);
	CHECK(b != NULL && sdk_vorbis_heap_regions(&h0) == 1U,
	      "core 1 deselect keeps core 0's arena");
	/* Core 1 selects h1 again and drops it; core 0 keeps h0. */
	g_cpu = 1;
	sdk_vorbis_heap_select(&h1, NULL);
	sdk_vorbis_heap_release(&h1);
	g_cpu = 0;
	sdk_vorbis_free(a);
	b = sdk_vorbis_realloc(b, 200U);
	CHECK(b != NULL, "releasing core 1's arena keeps core 0 routed to h0");
	sdk_vorbis_free(b);
	sdk_vorbis_heap_select(NULL, NULL);
	sdk_vorbis_heap_release(&h0);
	CHECK(sdk_vorbis_heap_regions(&h0) == 0U, "arena released");
	g_cpu = 1;
}

int main(void)
{
	test_flac();
	test_vorbis();
	if (failures == 0)
		puts("audio decode allocator context tests passed");
	return failures != 0;
}
