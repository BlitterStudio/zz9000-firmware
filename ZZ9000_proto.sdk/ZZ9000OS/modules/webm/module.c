#include "zzm_format.h"
#include "zzm_runtime.h"
#include "zzm_webm_ops.h"

extern const struct SDKVideoDecoderOps *zzm_webm_ops(uint32_t codec);

static const struct zzm_webm_ops webm_ops = {
    ZZM_WEBM_OPS_ABI_VERSION,
    sizeof(webm_ops),
    zzm_webm_ops,
};

const uint32_t zzm_module_descriptor = ZZM_MODULE_WEBM;

int zzm_module_init(const zzm_services_t *services)
{
    if (!zzm_rt_bind(services, ZZM_MODULE_WEBM, ZZM_MEMORY_CLASS_MEDIA,
                     64U << 20))
        return -1;
    return services->register_ops(ZZM_MODULE_WEBM, &webm_ops,
                                  sizeof(webm_ops)) ? 0 : -1;
}
