#include <stdint.h>
#include <string.h>
#include "sdk_webp_alloc.h"

static const zzm_services_t *module_services;
static struct zzm_webp_arena *active_arena;
static int active_failed;

void zzm_webp_alloc_init(const zzm_services_t *services)
{
	module_services = services;
}

void zzm_webp_alloc_select(struct zzm_webp_arena *arena)
{
	active_arena = arena;
	active_failed = 0;
}

int zzm_webp_alloc_failed(void)
{
	return active_failed;
}

void zzm_webp_alloc_release(struct zzm_webp_arena *arena)
{
	uint32_t i;

	if (!arena || !module_services)
		return;
	for (i = 0U; i < arena->region_count; ++i) {
		if (arena->region[i] != 0U)
			(void)module_services->card_free((uint32_t)arena->region[i],
			                                 arena->owner);
	}
	memset(arena, 0, sizeof(*arena));
	if (active_arena == arena)
		active_arena = 0;
}

void *sdk_webp_malloc(size_t size)
{
	struct zzm_webp_arena *arena = active_arena;
	uint32_t bytes;
	uint32_t region_bytes;
	uintptr_t region;

	if (!arena || !module_services || size == 0U || size > 0xffffffffU) {
		active_failed = 1;
		return 0;
	}
	bytes = ((uint32_t)size + 7U) & ~7U;
	if (bytes < (uint32_t)size || bytes > arena->limit - arena->held) {
		active_failed = 1;
		return 0;
	}
	if (arena->region_count != 0U) {
		uint32_t index = arena->region_count - 1U;
		if (bytes <= arena->region_bytes[index] - arena->region_used[index]) {
			void *ptr = (void *)(arena->region[index] + arena->region_used[index]);
			arena->region_used[index] += bytes;
			return ptr;
		}
	}
	if (arena->region_count >= ZZM_WEBP_ARENA_MAX_REGIONS) {
		active_failed = 1;
		return 0;
	}
	region_bytes = arena->region_size;
	if (region_bytes < bytes)
		region_bytes = bytes;
	if (region_bytes > arena->limit - arena->held)
		region_bytes = arena->limit - arena->held;
	if (region_bytes < bytes) {
		active_failed = 1;
		return 0;
	}
	region = (uintptr_t)module_services->card_alloc(region_bytes, arena->owner);
	if (region == 0U) {
		active_failed = 1;
		return 0;
	}
	arena->region[arena->region_count] = region;
	arena->region_bytes[arena->region_count] = region_bytes;
	arena->region_used[arena->region_count] = bytes;
	arena->region_count++;
	arena->held += region_bytes;
	return (void *)region;
}

void *sdk_webp_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count) {
		active_failed = 1;
		return 0;
	}
	ptr = sdk_webp_malloc(count * size);
	if (ptr)
		memset(ptr, 0, count * size);
	return ptr;
}

void sdk_webp_free(void *ptr)
{
	(void)ptr;
}
