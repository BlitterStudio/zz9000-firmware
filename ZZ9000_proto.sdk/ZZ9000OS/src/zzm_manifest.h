#ifndef ZZM_MANIFEST_H
#define ZZM_MANIFEST_H

#include <stdint.h>
#include "zzm_format.h"

#define ZZM_ARENA_ADDRESS 0x09000000U
#define ZZM_ARENA_SIZE    0x02000000U
#define ZZM_ARENA_END     (ZZM_ARENA_ADDRESS + ZZM_ARENA_SIZE)

typedef struct zzm_slot {
    uint32_t id;
    const char *name;
    uint32_t address;
    uint32_t size;
} zzm_slot_t;

extern const zzm_slot_t zzm_slots[];
extern const uint32_t zzm_slot_count;
const zzm_slot_t *zzm_manifest_find(uint32_t id, const char *name);

#endif
