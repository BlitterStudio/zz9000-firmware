#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "sdk_video_mpeg2_module.h"
#include "zzm_format.h"
#include "zzm_runtime.h"

const struct SDKVideoDecoderOps *zzm_mpeg2_module_find_ops(
    uint32_t codec, uint32_t container);

void mpeg2_module_set_owner(uint32_t owner)
{
    zzm_allocation_context_t context;
    zzm_rt_get_context(&context);
    zzm_rt_set_context(owner, context.memory_class, context.quota);
}

void *mpeg2_module_alloc(uint32_t bytes) { return malloc(bytes); }
void mpeg2_module_free(void *ptr) { free(ptr); }

static const struct zzm_mpeg2_module_ops mpeg2_ops = {
    .abi_version = ZZM_MPEG2_MODULE_OPS_ABI_VERSION,
    .size = sizeof(mpeg2_ops),
    .find = zzm_mpeg2_module_find_ops,
};

const uint32_t zzm_module_descriptor = ZZM_MODULE_MPEG2;

int zzm_module_init(const zzm_services_t *services)
{
    if (!zzm_rt_bind(services, ZZM_MODULE_MPEG2, ZZM_MEMORY_CLASS_MEDIA,
                     32U << 20))
        return -1;
    return services->register_ops(ZZM_MODULE_MPEG2, &mpeg2_ops,
                                  sizeof(mpeg2_ops)) ? 0 : -1;
}
