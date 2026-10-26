#include "webm_module_alloc.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>


#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#define WEBM_MODULE_ALLOC_MAGIC 0x57414c4cu

struct webm_module_allocation {
	void *base;
	size_t size;
	uint32_t magic;
};

static const zzm_services_t *module_services;

static uint32_t module_card_alloc(void *context, uint32_t bytes, uint32_t owner)
{
	const zzm_services_t *services = context;

	if (!services || !services->card_alloc)
		return 0U;
	return services->card_alloc(bytes, owner);
}

static int module_card_free(void *context, uint32_t address, uint32_t owner)
{
	const zzm_services_t *services = context;

	if (!services || !services->card_free)
		return 0;
	return services->card_free(address, owner);
}

void webm_module_alloc_init(const zzm_services_t *services)
{
	module_services = services;
}

void webm_module_heap_init(struct sdk_vorbis_heap *heap, uint32_t owner)
{
	sdk_vorbis_heap_init(heap, 4U * 1024U * 1024U, 64U * 1024U * 1024U,
	                     owner);
	sdk_vorbis_heap_set_region_ops(heap, module_card_alloc, module_card_free,
	                               (void *)module_services);
}

static int alignment_valid(size_t alignment)
{
	return alignment >= sizeof(void *) &&
	       (alignment & (alignment - 1U)) == 0U;
}

static void *module_alloc(size_t alignment, size_t size)
{
	struct webm_module_allocation *allocation;
	uintptr_t aligned;
	void *base;
	size_t overhead;
	size_t bytes;

	if (!alignment_valid(alignment) ||
	    alignment - 1U > SIZE_MAX - sizeof(*allocation))
		return 0;
	overhead = sizeof(*allocation) + alignment - 1U;
	if (size > SIZE_MAX - overhead)
		return 0;
	bytes = size + overhead;
	base = sdk_vorbis_malloc(bytes);
	if (!base)
		return 0;
	aligned = ((uintptr_t)base + sizeof(*allocation) + alignment - 1U) &
	          ~(uintptr_t)(alignment - 1U);
	allocation = (struct webm_module_allocation *)aligned - 1;
	allocation->base = base;
	allocation->size = size;
	allocation->magic = WEBM_MODULE_ALLOC_MAGIC;
	return (void *)aligned;
}

static struct webm_module_allocation *module_header(void *ptr)
{
	struct webm_module_allocation *allocation;

	if (!ptr)
		return 0;
	allocation = (struct webm_module_allocation *)ptr - 1;
	return allocation->magic == WEBM_MODULE_ALLOC_MAGIC ? allocation : 0;
}

void *__wrap_malloc(size_t size)
{
	return module_alloc(sizeof(void *), size);
}

void __wrap_free(void *ptr)
{
	struct webm_module_allocation *allocation = module_header(ptr);

	if (!allocation)
		return;
	allocation->magic = 0U;
	sdk_vorbis_free(allocation->base);
}

void *__wrap_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count)
		return 0;
	ptr = __wrap_malloc(count * size);
	if (ptr)
		memset(ptr, 0, count * size);
	return ptr;
}

void *__wrap_realloc(void *ptr, size_t size)
{
	struct webm_module_allocation *allocation;
	void *replacement;
	size_t copied;

	if (!ptr)
		return __wrap_malloc(size);
	if (size == 0U) {
		__wrap_free(ptr);
		return 0;
	}
	allocation = module_header(ptr);
	if (!allocation)
		return 0;
	replacement = __wrap_malloc(size);
	if (!replacement)
		return 0;
	copied = allocation->size < size ? allocation->size : size;
	memcpy(replacement, ptr, copied);
	__wrap_free(ptr);
	return replacement;
}

void *__wrap_memalign(size_t alignment, size_t size)
{
	return module_alloc(alignment, size);
}

int __wrap_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *ptr;

	if (!memptr || !alignment_valid(alignment))
		return EINVAL;
	ptr = module_alloc(alignment, size);
	if (!ptr)
		return ENOMEM;
	*memptr = ptr;
	return 0;
}

void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
	if (!alignment_valid(alignment) || size % alignment != 0U)
		return 0;
	return module_alloc(alignment, size);
}
