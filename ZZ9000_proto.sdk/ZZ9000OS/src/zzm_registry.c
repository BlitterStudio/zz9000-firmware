#include "sdk_mailbox.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_registry.h"

enum zzm_codec_availability zzm_registry_availability(uint32_t module_id)
{
    zzm_status_t status;
    for (uint32_t i = 0; i < zzm_loader_count(); i++) {
        if (zzm_loader_status(i, &status) && status.id == module_id) {
            return status.state == ZZM_STATE_LOADED ? ZZM_CODEC_AVAILABLE :
                   status.state == ZZM_STATE_MISSING ? ZZM_CODEC_MISSING :
                                                        ZZM_CODEC_MISMATCH;
        }
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
