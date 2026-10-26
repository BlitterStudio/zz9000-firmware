#ifndef ZZM_REGISTRY_H
#define ZZM_REGISTRY_H

#include <stdint.h>
#include "zzm_format.h"

/* Optional codec paths must ask this registry before entering module-only
 * glue. It makes the missing/mismatch outcome explicit at each mailbox edge. */
enum zzm_codec_availability {
    ZZM_CODEC_AVAILABLE = 0,
    ZZM_CODEC_MISSING = 1,
    ZZM_CODEC_MISMATCH = 2,
};

enum zzm_codec_availability zzm_registry_availability(uint32_t module_id);
int zzm_registry_register_ops(uint32_t module_id, const void *ops,
                              uint32_t ops_size);
int zzm_registry_register_service(uint32_t module_id, uint32_t service_id,
                                  uint32_t version, const void *interface,
                                  uint32_t interface_size);
const void *zzm_registry_get_service(uint32_t service_id,
                                     uint32_t min_version);
const void *zzm_registry_ops(uint32_t module_id, uint32_t min_size);
uint16_t zzm_registry_sdk_status(uint32_t module_id);

#endif
