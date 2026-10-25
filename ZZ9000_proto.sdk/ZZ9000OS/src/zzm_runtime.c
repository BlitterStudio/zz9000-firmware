#include <stdio.h>
#include "xil_cache.h"
#include "card_pool.h"
#include "zzm_runtime.h"

static uint32_t module_card_alloc(uint32_t bytes, uint32_t owner)
{
    return card_pool_alloc(&card_pool, bytes, owner);
}

static int module_card_free(uint32_t address, uint32_t owner)
{
    return card_pool_free(&card_pool, address, owner);
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

static const zzm_services_t services = {
    ZZM_MODULE_ABI_VERSION, sizeof(services), module_card_alloc, module_card_free,
    module_dcache_clean, module_icache_invalidate, module_log,
};

const zzm_services_t *zzm_runtime_services(void) { return &services; }
