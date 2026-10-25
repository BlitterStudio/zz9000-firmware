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
uint16_t zzm_registry_sdk_status(uint32_t module_id);

#endif
