#include <string.h>
#include "zzm_manifest.h"

/* This is the single slot authority for the packer, module linker scripts,
 * loader, and status query. Slots reserve image+BSS only; working memory is
 * owner-tagged card-pool allocation under the module quota. */
const zzm_slot_t zzm_slots[] = {
    { ZZM_MODULE_MPEG2,  "MPEG2",  0x09000000U, 0x00200000U },
    { ZZM_MODULE_WEBM,   "WEBM",   0x09200000U, 0x00400000U },
    { ZZM_MODULE_WEBP,   "WEBP",   0x09600000U, 0x00200000U },
    { ZZM_MODULE_MINIGL, "MINIGL", ZZM_MINIGL_SLOT_ADDRESS, ZZM_MINIGL_SLOT_SIZE },
    { ZZM_MODULE_H264,   "H264",   0x09c00000U, 0x00400000U },
    { ZZM_MODULE_H265,   "H265",   0x0a000000U, 0x00400000U },
    { ZZM_MODULE_MP4,    "MP4",    0x0a400000U, 0x00100000U },
    { ZZM_MODULE_AAC,    "AAC",    0x0a500000U, 0x00100000U },
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
