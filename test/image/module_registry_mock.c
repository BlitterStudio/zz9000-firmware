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
static int initialized;

static uint32_t host_card_alloc(uint32_t bytes, uint32_t owner)
{
    return card_pool_alloc(&card_pool, bytes, owner);
}

static int host_card_free(uint32_t address, uint32_t owner)
{
    return card_pool_free(&card_pool, address, owner);
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
        ZZM_MODULE_ABI_VERSION, sizeof(services), host_card_alloc,
        host_card_free, 0, 0, 0, host_register_ops,
    };

    if (!initialized) {
        initialized = 1;
        (void)zzm_module_init(&services);
    }
}

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
