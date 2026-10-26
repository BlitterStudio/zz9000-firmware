#define _GNU_SOURCE

#include "card_pool.h"
#include "webm_module_alloc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifndef EINVAL
#define EINVAL 22
#endif

#define TEST_REGION_ADDRESS ((void *)0x18000000U)
#define TEST_REGION_BYTES (8U * 1024U * 1024U)

static uint8_t *region;
static uint32_t region_next;
static unsigned alloc_calls;
static unsigned free_calls;
static uint32_t last_owner;
static int fail_next_alloc;

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

static uint32_t fake_card_alloc(uint32_t bytes, uint32_t owner)
{
	uint32_t at = (region_next + 0xffffU) & ~0xffffU;

	if (fail_next_alloc || bytes > TEST_REGION_BYTES - at)
		return 0U;
	fail_next_alloc = 0;
	region_next = at + bytes;
	alloc_calls++;
	last_owner = owner;
	return (uint32_t)(uintptr_t)(region + at);
}

static int fake_card_free(uint32_t address, uint32_t owner)
{
	if (address < (uint32_t)(uintptr_t)region ||
	    address >= (uint32_t)(uintptr_t)(region + TEST_REGION_BYTES) ||
	    owner != last_owner)
		return 0;
	free_calls++;
	return 1;
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
	zzm_services_t services;
	struct sdk_vorbis_heap heap;
	void *ptr;
	void *aligned;
	void *kept;
	unsigned char *bytes;
	uint32_t owner = 0x02000037U;

	region = mmap(TEST_REGION_ADDRESS, TEST_REGION_BYTES, PROT_READ | PROT_WRITE,
	              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (!check(region != MAP_FAILED, "cannot map low card region"))
		return 1;
	memset(&services, 0, sizeof(services));
	services.abi_version = ZZM_MODULE_ABI_VERSION;
	services.size = sizeof(services);
	services.card_alloc = fake_card_alloc;
	services.card_free = fake_card_free;
	webm_module_alloc_init(&services);
	webm_module_heap_init(&heap, owner);
	sdk_vorbis_heap_select(&heap, 0);

	ptr = __wrap_calloc(32U, 4U);
	if (!check(ptr != 0 && alloc_calls == 1U && last_owner == owner,
	           "calloc did not use module card service"))
		return 1;
	bytes = ptr;
	for (unsigned i = 0U; i < 128U; i++)
		if (!check(bytes[i] == 0U, "calloc did not clear memory"))
			return 1;
	memset(ptr, 0x5a, 128U);
	kept = ptr;
	fail_next_alloc = 1;
	if (!check(__wrap_realloc(ptr, 5U * 1024U * 1024U) == 0 &&
	           ((unsigned char *)kept)[0] == 0x5a,
	           "failed realloc did not preserve the original block"))
		return 1;
	fail_next_alloc = 0;
	__wrap_free(ptr);

	aligned = __wrap_memalign(256U, 31U);
	if (!check(aligned != 0 && ((uintptr_t)aligned & 255U) == 0U,
	           "memalign did not honor alignment"))
		return 1;
	if (!check(__wrap_posix_memalign(&ptr, 3U, 32U) == EINVAL,
	           "posix_memalign accepted invalid alignment"))
		return 1;
	if (!check(__wrap_posix_memalign(&ptr, 64U, 17U) == 0 &&
	           ((uintptr_t)ptr & 63U) == 0U,
	           "posix_memalign did not use module allocator"))
		return 1;
	__wrap_free(ptr);
	__wrap_free(aligned);
	if (!check(free_calls == alloc_calls,
	           "module card regions were not released through the service"))
		return 1;
	if (!check(__wrap_aligned_alloc(64U, 65U) == 0,
	           "aligned_alloc accepted a non-multiple size"))
		return 1;
	sdk_vorbis_heap_release(&heap);
	if (munmap(region, TEST_REGION_BYTES) != 0)
		return 1;
	puts("webm_module_alloc_test: all passed");
	return 0;
}
