#include <string.h>
#include "zzm_manifest.h"

/* This is the single slot authority for the packer, module linker scripts,
 * loader, and status query. Slots are 64 KiB aligned to match card-pool pages. */
const zzm_slot_t zzm_slots[] = {
    { ZZM_MODULE_MPEG2,  "MPEG2",  0x09000000U, 0x00400000U },
    { ZZM_MODULE_WEBM,   "WEBM",   0x09400000U, 0x00800000U },
    { ZZM_MODULE_WEBP,   "WEBP",   0x09C00000U, 0x00400000U },
    { ZZM_MODULE_MINIGL, "MINIGL", 0x0A000000U, 0x00400000U },
    { ZZM_MODULE_H264,   "H264",   0x0A400000U, 0x00400000U },
    { ZZM_MODULE_H265,   "H265",   0x0A800000U, 0x00400000U },
};
const uint32_t zzm_slot_count = sizeof(zzm_slots) / sizeof(zzm_slots[0]);

const zzm_slot_t *zzm_manifest_find(uint32_t id, const char *name)
{
    for (uint32_t i = 0; i < zzm_slot_count; i++) {
        if (zzm_slots[i].id == id && strcmp(zzm_slots[i].name, name) == 0)
            return &zzm_slots[i];
    }
    return 0;
}
