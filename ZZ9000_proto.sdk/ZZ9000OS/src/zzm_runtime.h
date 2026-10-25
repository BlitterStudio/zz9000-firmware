#ifndef ZZM_RUNTIME_H
#define ZZM_RUNTIME_H

#include <stdint.h>
#include "zzm_format.h"

typedef struct zzm_services {
    uint32_t abi_version;
    uint32_t size;
    uint32_t (*card_alloc)(uint32_t bytes, uint32_t owner);
    int (*card_free)(uint32_t address, uint32_t owner);
    void (*dcache_clean)(uint32_t address, uint32_t length);
    void (*icache_invalidate)(uint32_t address, uint32_t length);
    void (*log)(const char *message);
    int (*register_ops)(uint32_t module_id, const void *ops, uint32_t ops_size);
} zzm_services_t;

typedef int (*zzm_init_fn)(const zzm_services_t *services);
const zzm_services_t *zzm_runtime_services(void);

#endif
