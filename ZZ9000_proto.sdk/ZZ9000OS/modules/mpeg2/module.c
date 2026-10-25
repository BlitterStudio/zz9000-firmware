#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdk_video_mpeg2_module.h"
#include "zzm_format.h"
#include "zzm_runtime.h"

struct mpeg2_module_allocation {
	uint32_t bytes;
	uint32_t owner;
};

static const zzm_services_t *mpeg2_services;
static uint32_t mpeg2_owner;

const struct SDKVideoDecoderOps *zzm_mpeg2_module_find_ops(
	uint32_t codec, uint32_t container);

void mpeg2_module_set_owner(uint32_t owner)
{
	mpeg2_owner = owner;
}

const zzm_services_t *mpeg2_module_services(void)
{
	return mpeg2_services;
}

void *mpeg2_module_alloc(uint32_t bytes)
{
	struct mpeg2_module_allocation *allocation;
	uint32_t address;

	if (!mpeg2_services || !mpeg2_services->card_alloc ||
	    bytes > UINT32_MAX - sizeof(*allocation))
		return 0;
	address = mpeg2_services->card_alloc(bytes + sizeof(*allocation),
	                                     mpeg2_owner);
	if (address == 0U)
		return 0;
	allocation = (struct mpeg2_module_allocation *)(uintptr_t)address;
	allocation->bytes = bytes;
	allocation->owner = mpeg2_owner;
	return allocation + 1;
}

void mpeg2_module_free(void *ptr)
{
	struct mpeg2_module_allocation *allocation;

	if (!ptr || !mpeg2_services || !mpeg2_services->card_free)
		return;
	allocation = ((struct mpeg2_module_allocation *)ptr) - 1;
	(void)mpeg2_services->card_free((uint32_t)(uintptr_t)allocation,
	                                allocation->owner);
}

void *__wrap_malloc(size_t bytes)
{
	if (bytes > UINT32_MAX)
		return 0;
	return mpeg2_module_alloc((uint32_t)bytes);
}

void __wrap_free(void *ptr)
{
	mpeg2_module_free(ptr);
}

void *__wrap_calloc(size_t count, size_t bytes)
{
	void *ptr;

	if (count != 0U && bytes > UINT32_MAX / count)
		return 0;
	ptr = mpeg2_module_alloc((uint32_t)(count * bytes));
	if (ptr)
		memset(ptr, 0, count * bytes);
	return ptr;
}

void *__wrap_realloc(void *ptr, size_t bytes)
{
	struct mpeg2_module_allocation *allocation;
	void *replacement;
	size_t copied;

	if (!ptr)
		return __wrap_malloc(bytes);
	if (bytes == 0U) {
		__wrap_free(ptr);
		return 0;
	}
	allocation = ((struct mpeg2_module_allocation *)ptr) - 1;
	if (bytes > UINT32_MAX)
		return 0;
	replacement = mpeg2_module_alloc((uint32_t)bytes);
	if (!replacement)
		return 0;
	copied = allocation->bytes < bytes ? allocation->bytes : bytes;
	memcpy(replacement, ptr, copied);
	mpeg2_module_free(ptr);
	return replacement;
}

static const struct zzm_mpeg2_module_ops mpeg2_ops = {
	.abi_version = ZZM_MPEG2_MODULE_OPS_ABI_VERSION,
	.size = sizeof(mpeg2_ops),
	.find = zzm_mpeg2_module_find_ops,
};

int zzm_module_init(const zzm_services_t *services)
{
	if (!services || services->abi_version != ZZM_MODULE_ABI_VERSION ||
	    services->size < offsetof(zzm_services_t, register_ops) +
	                     sizeof(services->register_ops) ||
	    !services->card_alloc || !services->card_free || !services->register_ops)
		return -1;
	mpeg2_services = services;
	return services->register_ops(ZZM_MODULE_MPEG2, &mpeg2_ops,
	                              sizeof(mpeg2_ops)) ? 0 : -1;
}
