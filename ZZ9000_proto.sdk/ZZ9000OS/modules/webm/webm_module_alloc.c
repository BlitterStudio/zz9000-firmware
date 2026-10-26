#include "webm_module_alloc.h"

#include <stdlib.h>

static uint32_t runtime_region_alloc(void *context, uint32_t bytes, uint32_t owner)
{
    zzm_allocation_context_t allocation;
    (void)context;
    zzm_rt_get_context(&allocation);
    zzm_rt_set_context(owner, allocation.memory_class, allocation.quota);
    return (uint32_t)(uintptr_t)malloc(bytes);
}

static int runtime_region_free(void *context, uint32_t address, uint32_t owner)
{
    (void)context;
    (void)owner;
    free((void *)(uintptr_t)address);
    return 1;
}

void webm_module_heap_init(struct sdk_vorbis_heap *heap, uint32_t owner)
{
    sdk_vorbis_heap_init(heap, 4U * 1024U * 1024U, 64U * 1024U * 1024U,
                         owner);
    sdk_vorbis_heap_set_region_ops(heap, runtime_region_alloc, runtime_region_free,
                                   0);
}
