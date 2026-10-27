#define _GNU_SOURCE

#include "webm_module_alloc.h"
#include "card_pool.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define TEST_REGION_ADDRESS ((void *)0x18000000U)
#define TEST_REGION_BYTES (8U * 1024U * 1024U)

static uint8_t *region;
static uint32_t region_next;
static unsigned alloc_calls;
static unsigned free_calls;
card_pool_t card_pool;

int smp_cpu_id(void) { return 0; }
void sdk_decode_track(void *ptr) { (void)ptr; }
void sdk_decode_untrack(void *ptr) { (void)ptr; }
unsigned sdk_decode_tracked_count(void) { return 0U; }
void *sdk_decode_heap_alloc(size_t size) { (void)size; return 0; }
void sdk_decode_heap_free(void *ptr) { (void)ptr; }
uint32_t card_pool_alloc(card_pool_t *pool, uint32_t bytes, uint32_t owner)
{
	(void)pool;
	(void)bytes;
	(void)owner;
	return 0U;
}
int card_pool_free(card_pool_t *pool, uint32_t address, uint32_t owner)
{
	(void)pool;
	(void)address;
	(void)owner;
	return 0;
}

static zzm_allocation_context_t context = {
	.owner = 0x02000001U,
	.memory_class = ZZM_MEMORY_CLASS_MEDIA,
	.quota = 4U * 1024U * 1024U,
};
static uint32_t last_context_owner;

void zzm_rt_get_context(zzm_allocation_context_t *out)
{
	if (out)
		*out = context;
}

void zzm_rt_set_context(uint32_t owner, uint32_t memory_class, uint32_t quota)
{
	last_context_owner = owner;
	context.owner = owner;
	context.memory_class = memory_class;
	context.quota = quota;
}

void *__wrap_malloc(size_t bytes)
{
	uint32_t aligned = (region_next + 7U) & ~7U;

	if (bytes > TEST_REGION_BYTES - aligned)
		return 0;
	region_next = aligned + (uint32_t)bytes;
	alloc_calls++;
	return region + aligned;
}

void __wrap_free(void *ptr)
{
	if (ptr >= (void *)region && ptr < (void *)(region + TEST_REGION_BYTES))
		free_calls++;
}

static int check(int condition, const char *message)
{
	if (condition)
		return 1;
	fprintf(stderr, "%s\n", message);
	return 0;
}

int main(void)
{
	struct sdk_vorbis_heap heap;
	void *first;
	void *second;
	uint32_t owner = 0x02000037U;

	region = mmap(TEST_REGION_ADDRESS, TEST_REGION_BYTES, PROT_READ | PROT_WRITE,
	              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (!check(region != MAP_FAILED, "cannot map low card region"))
		return 1;
	webm_module_heap_init(&heap, owner);
	sdk_vorbis_heap_select(&heap, 0);
	first = sdk_vorbis_malloc(128U);
	second = sdk_vorbis_malloc(256U);
	if (!check(first != 0 && second != 0 && alloc_calls == 1U,
	           "WebM heap did not obtain one runtime region"))
		return 1;
	if (!check(last_context_owner == owner,
	           "WebM runtime region did not retain its module owner"))
		return 1;
	memset(first, 0x5a, 128U);
	sdk_vorbis_free(first);
	if (!check(alloc_calls == 1U && free_calls == 0U,
	           "individual WebM frees must retain the arena region"))
		return 1;
	if (!check(sdk_vorbis_malloc(5U * 1024U * 1024U) == 0,
	           "WebM heap exceeded its configured quota"))
		return 1;
	sdk_vorbis_heap_release(&heap);
	if (!check(free_calls == alloc_calls,
	           "WebM heap did not release its runtime region"))
		return 1;
	if (munmap(region, TEST_REGION_BYTES) != 0)
		return 1;
	puts("webm_module_alloc_test: all passed");
	return 0;
}
