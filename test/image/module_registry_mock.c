/* Host adapter that exercises the external WebP operation table. */
#include <stdint.h>

#include "card_pool.h"
#include "sdk_mailbox.h"
#include "sdk_vorbis_alloc.h"
#include "zzm_manifest.h"
#include "zzm_registry.h"
#include "zzm_runtime.h"

extern int zzm_module_init(const zzm_services_t *services);


static const void *webp_ops;
static uint32_t webp_ops_size;

static zzm_allocation_context_t host_context;

int zzm_rt_bind(const zzm_services_t *services, uint32_t module_id,
                uint32_t memory_class, uint32_t quota)
{
    if (!services ||
        (services->abi_version != ZZM_MODULE_ABI_VERSION &&
         services->abi_version != ZZM_V3_ABI_MAJOR) ||
        services->size < sizeof(*services) || !services->get_service)
        return 0;
    host_context.owner = module_id;
    host_context.memory_class = memory_class;
    host_context.quota = quota;
    return 1;
}

void zzm_rt_set_context(uint32_t owner, uint32_t memory_class, uint32_t quota)
{
    host_context.owner = owner;
    host_context.memory_class = memory_class;
    host_context.quota = quota;
}

void zzm_rt_get_context(zzm_allocation_context_t *out)
{
    if (out)
        *out = host_context;
}
static int initialized;
static uint32_t webp_refs;

static uint32_t host_card_alloc(uint32_t owner, uint32_t memory_class,
                                uint32_t quota, uint32_t bytes,
                                uint32_t alignment)
{
    (void)memory_class;
    (void)quota;
    (void)alignment;
    return card_pool_alloc(&card_pool, bytes, owner);
}

static int host_card_free(uint32_t address, uint32_t owner)
{
    return card_pool_free(&card_pool, address, owner);
}

static uint32_t host_card_realloc(uint32_t address, uint32_t owner,
                                  uint32_t bytes, uint32_t alignment)
{
    (void)address;
    (void)owner;
    (void)bytes;
    (void)alignment;
    return 0;
}

static void host_log(const char *message)
{
    (void)message;
}

static void host_fault(const char *reason)
{
    (void)reason;
}

static const zzm_core_service_t core_service = {
    ZZM_SERVICE_CORE_V1, sizeof(core_service), host_log, host_fault, 0, 0
};
static const zzm_memory_service_t memory_service = {
    ZZM_SERVICE_MEMORY_V1, sizeof(memory_service),
    host_card_alloc, host_card_free, host_card_realloc
};
static const zzm_cache_service_t cache_service = {
    ZZM_SERVICE_CACHE_V1, sizeof(cache_service), 0, 0
};

static const void *host_get_service(uint32_t id, uint32_t min_version)
{
    if (min_version != 1U)
        return 0;
    switch (id) {
    case ZZM_SERVICE_CORE: return &core_service;
    case ZZM_SERVICE_MEMORY: return &memory_service;
    case ZZM_SERVICE_CACHE: return &cache_service;
    default: return 0;
    }
}

static int host_register_ops(uint32_t id, const void *ops, uint32_t size)
{
    if (id != ZZM_MODULE_WEBP || !ops || size == 0U)
        return 0;
    webp_ops = ops;
    webp_ops_size = size;
    return 1;
}

static void ensure_initialized(void)
{
    static const zzm_services_t services = {
        ZZM_MODULE_ABI_VERSION, sizeof(services), host_get_service,
        host_register_ops, 0
    };

    if (!initialized) {
        initialized = 1;
        (void)zzm_module_init(&services);
    }
}

int zzm_loader_acquire(uint32_t id)
{
    ensure_initialized();
    if (id != ZZM_MODULE_WEBP || !webp_ops)
        return 0;
    webp_refs++;
    return 1;
}

void zzm_loader_release(uint32_t id)
{
    if (id == ZZM_MODULE_WEBP && webp_refs != 0U)
        webp_refs--;
}

void zzm_loader_session_begin_lock(void) {}
void zzm_loader_session_begin_unlock(void) {}

uint16_t zzm_registry_sdk_status(uint32_t id)
{
    ensure_initialized();
    return id == ZZM_MODULE_WEBP && webp_ops ? SDK_STATUS_OK :
        SDK_STATUS_MODULE_MISMATCH;
}

const void *zzm_registry_ops(uint32_t id, uint32_t min_size)
{
    ensure_initialized();
    return id == ZZM_MODULE_WEBP && webp_ops_size >= min_size ? webp_ops : 0;
}
