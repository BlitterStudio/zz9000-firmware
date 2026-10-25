#include "sdk_mailbox.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_registry.h"

static const void *module_ops[3];
static uint32_t module_ops_size[3];

static int module_index(uint32_t module_id)
{
    zzm_status_t status;

    for (uint32_t i = 0; i < zzm_loader_count(); i++) {
        if (zzm_loader_status(i, &status) && status.id == module_id)
            return (int)i;
    }
    return -1;
}


enum zzm_codec_availability zzm_registry_availability(uint32_t module_id)
{
    int index = module_index(module_id);
    zzm_status_t status;

    if (index >= 0 && zzm_loader_status((uint32_t)index, &status)) {
        return status.state == ZZM_STATE_LOADED ? ZZM_CODEC_AVAILABLE :
               status.state == ZZM_STATE_MISSING ? ZZM_CODEC_MISSING :
                                                    ZZM_CODEC_MISMATCH;
    }
    return ZZM_CODEC_MISMATCH;
}

uint16_t zzm_registry_sdk_status(uint32_t module_id)
{
    const enum zzm_codec_availability availability =
        zzm_registry_availability(module_id);
    return availability == ZZM_CODEC_AVAILABLE ? SDK_STATUS_OK :
           availability == ZZM_CODEC_MISSING ? SDK_STATUS_MODULE_MISSING :
                                                SDK_STATUS_MODULE_MISMATCH;
}

int zzm_registry_register_ops(uint32_t module_id, const void *ops,
                              uint32_t size)
{
    int index;

    if (!ops || size == 0U ||
        zzm_registry_availability(module_id) != ZZM_CODEC_AVAILABLE)
        return 0;
    index = module_index(module_id);
    if (index < 0 || (uint32_t)index >=
        sizeof(module_ops) / sizeof(module_ops[0]))
        return 0;
    module_ops[index] = ops;
    module_ops_size[index] = size;
    return 1;
}

const void *zzm_registry_ops(uint32_t module_id, uint32_t min_size)
{
    int index;

    if (zzm_registry_availability(module_id) != ZZM_CODEC_AVAILABLE)
        return 0;
    index = module_index(module_id);
    if (index < 0 || (uint32_t)index >=
        sizeof(module_ops) / sizeof(module_ops[0]) ||
        !module_ops[index] || module_ops_size[index] < min_size)
        return 0;
    return module_ops[index];
}
