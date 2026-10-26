#include <stdio.h>
#include "xil_cache.h"
#include "xtime_l.h"
#include "card_pool.h"
#include "zzm_registry.h"
#include "zzm_runtime.h"

static uint32_t module_card_alloc(uint32_t owner, uint32_t memory_class,
                                  uint32_t quota, uint32_t bytes,
                                  uint32_t alignment)
{
    (void)quota;
    (void)alignment; /* Card-pool pages are 64 KiB aligned. */
    switch (memory_class) {
    case ZZM_MEMORY_CLASS_MEDIA:
        owner = (owner & 0x00ffffffU) | (CARD_POOL_CLASS_MEDIA << 24);
        break;
    case ZZM_MEMORY_CLASS_MEDIA_INPUT:
        owner = (owner & 0x00ffffffU) | (CARD_POOL_CLASS_MEDIA_INPUT << 24);
        break;
    case ZZM_MEMORY_CLASS_IMAGE:
        owner = (owner & 0x00ffffffU) | (CARD_POOL_CLASS_IMAGE << 24);
        break;
    default:
        owner = (owner & 0x00ffffffU) | (CARD_POOL_CLASS_FIRMWARE << 24);
        break;
    }
    return card_pool_alloc(&card_pool, bytes, owner);
}

static int module_card_free(uint32_t address, uint32_t owner)
{
    for (uint32_t i = 0; i < card_pool.block_count; i++) {
        if (card_pool.blocks[i].addr == address &&
            (card_pool.blocks[i].owner & 0x00ffffffU) ==
                (owner & 0x00ffffffU))
            return card_pool_free(&card_pool, address, card_pool.blocks[i].owner);
    }
    return 0;
}

static uint32_t module_card_realloc(uint32_t address, uint32_t owner,
                                    uint32_t bytes, uint32_t alignment)
{
    (void)address;
    (void)owner;
    (void)bytes;
    (void)alignment;
    return 0U; /* libzzm_rt preserves blocks with allocate/copy/free. */
}

static void module_dcache_clean(uint32_t address, uint32_t length)
{
    Xil_DCacheFlushRange(address, length);
}

static void module_icache_invalidate(uint32_t address, uint32_t length)
{
    Xil_ICacheInvalidateRange(address, length);
}

static void module_log(const char *message)
{
    printf("[ZZM] %s\n", message ? message : "");
}

static void module_fault(const char *reason)
{
    printf("[ZZM] module fault: %s\n", reason ? reason : "");
}

static uint64_t module_time_us(void)
{
    XTime now;
    XTime_GetTime(&now);
    return (uint64_t)now / (COUNTS_PER_SECOND / 1000000U);
}

static uint32_t module_release_owner(uint32_t owner)
{
    return card_pool_release_owner(&card_pool, owner);
}

static int module_register_ops(uint32_t module_id, const void *ops,
                               uint32_t ops_size)
{
    return zzm_registry_register_ops(module_id, ops, ops_size);
}

static int module_register_service(uint32_t module_id, uint32_t id,
                                   uint32_t version, const void *interface,
                                   uint32_t interface_size)
{
    return zzm_registry_register_service(module_id, id, version, interface,
                                         interface_size);
}

static const zzm_core_service_t core_service = {
    ZZM_SERVICE_CORE_V1, sizeof(core_service), module_log, module_fault,
    module_time_us
};
static const zzm_memory_service_t memory_service = {
    ZZM_SERVICE_MEMORY_V1, sizeof(memory_service), module_card_alloc,
    module_card_free, module_card_realloc
};
static const zzm_cache_service_t cache_service = {
    ZZM_SERVICE_CACHE_V1, sizeof(cache_service), module_dcache_clean,
    module_icache_invalidate
};
static const zzm_reclaim_service_t reclaim_service = {
    ZZM_SERVICE_RECLAIM_V1, sizeof(reclaim_service), module_release_owner
};

static const void *module_get_service(uint32_t id, uint32_t min_version)
{
    switch (id) {
    case ZZM_SERVICE_CORE:
        return min_version <= core_service.version ? &core_service : 0;
    case ZZM_SERVICE_MEMORY:
        return min_version <= memory_service.version ? &memory_service : 0;
    case ZZM_SERVICE_CACHE:
        return min_version <= cache_service.version ? &cache_service : 0;
    case ZZM_SERVICE_RECLAIM:
        return min_version <= reclaim_service.version ? &reclaim_service : 0;
    default:
        return 0;
    }
}

static const zzm_services_t services = {
    ZZM_MODULE_ABI_VERSION, sizeof(services), module_get_service,
    module_register_ops, module_register_service
};

const zzm_services_t *zzm_runtime_services(void) { return &services; }

int zzm_runtime_register_ops(uint32_t module_id, const void *ops,
                             uint32_t ops_size)
{
    return module_register_ops(module_id, ops, ops_size);
}

void zzm_runtime_module_fault(uint32_t module_id, const char *reason)
{
    (void)module_id;
    module_fault(reason);
}
