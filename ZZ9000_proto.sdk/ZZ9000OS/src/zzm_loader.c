#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "xil_cache.h"
#include "sdk_crypto.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_runtime.h"

static zzm_status_t module_status[3];

static enum zzm_state zzm_verify_header(const zzm_header_t *header,
                                        uint32_t file_size)
{
    const zzm_slot_t *slot;
    uint32_t expected_size;

    if (!header || file_size < ZZM_HEADER_SIZE ||
        memcmp(header->magic, ZZM_MAGIC, 4) != 0 ||
        header->header_size != ZZM_HEADER_SIZE ||
        header->format_version != ZZM_FORMAT_VERSION)
        return ZZM_STATE_REJECTED_SLOT;
    if (header->module_abi_version != ZZM_MODULE_ABI_VERSION)
        return ZZM_STATE_REJECTED_ABI;
    if (memchr(header->name, '\0', ZZM_NAME_SIZE) == 0)
        return ZZM_STATE_REJECTED_SLOT;
    for (uint32_t i = 0; i < sizeof(header->reserved); i++) {
        if (header->reserved[i] != 0) return ZZM_STATE_REJECTED_SLOT;
    }
    slot = zzm_manifest_find(header->module_id, header->name);
    if (!slot || header->slot_load_address != slot->address ||
        header->image_size > slot->size ||
        header->bss_size > slot->size - header->image_size ||
        header->init_entry < slot->address ||
        header->init_entry >= slot->address + header->image_size ||
        header->image_size > UINT32_MAX - ZZM_HEADER_SIZE)
        return ZZM_STATE_REJECTED_SLOT;
    expected_size = ZZM_HEADER_SIZE + header->image_size;
    return file_size == expected_size ? ZZM_STATE_LOADED : ZZM_STATE_REJECTED_SLOT;
}

enum zzm_state zzm_verify_image(const zzm_header_t *header,
                                const uint8_t *image, uint32_t file_size,
                                zzm_sha256_fn sha256)
{
    uint8_t digest[ZZM_SHA256_SIZE];
    enum zzm_state state = zzm_verify_header(header, file_size);
    if (state != ZZM_STATE_LOADED) return state;
    if (!image || !sha256) return ZZM_STATE_REJECTED_SLOT;
    sha256(image, header->image_size, digest);
    return memcmp(digest, header->image_sha256, sizeof(digest)) == 0
               ? ZZM_STATE_LOADED : ZZM_STATE_REJECTED_HASH;
}

static void status_from_slot(uint32_t index, enum zzm_state state,
                             const zzm_header_t *header)
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_status_t *status = &module_status[index];
    memset(status, 0, sizeof(*status));
    status->id = slot->id;
    status->load_address = slot->address;
    status->state = state;
    strncpy(status->name, slot->name, sizeof(status->name) - 1);
    if (header) {
        status->version = header->module_version;
        status->abi_version = header->module_abi_version;
        status->image_size = header->image_size;
        status->bss_size = header->bss_size;
    }
}

void zzm_loader_boot_load(void)
{
    for (uint32_t i = 0; i < zzm_slot_count; i++) {
        const zzm_slot_t *slot = &zzm_slots[i];
        zzm_header_t header;
        FIL file;
        UINT read = 0;
        char path[32];
        FRESULT fr;
        enum zzm_state state;

        status_from_slot(i, ZZM_STATE_MISSING, 0);
        snprintf(path, sizeof(path), "0:/ZZ9K/%s.ZZM", slot->name);
        fr = f_open(&file, path, FA_READ);
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            printf("[ZZM] %s missing\n", slot->name);
            continue;
        }
        if (fr != FR_OK) {
            printf("[ZZM] %s rejected: open %u\n", slot->name, (unsigned)fr);
            status_from_slot(i, ZZM_STATE_REJECTED_SLOT, 0);
            continue;
        }
        if (f_read(&file, &header, sizeof(header), &read) != FR_OK ||
            read != sizeof(header)) {
            printf("[ZZM] %s rejected: truncated\n", slot->name);
            status_from_slot(i, ZZM_STATE_REJECTED_SLOT, 0);
            f_close(&file);
            continue;
        }
        state = zzm_verify_header(&header, (uint32_t)f_size(&file));
        /* Only a header that names its assigned slot can direct the SD DMA.
         * The second pass below validates the digest of those exact bytes. */
        if (state == ZZM_STATE_LOADED &&
            f_read(&file, (void *)(uintptr_t)header.slot_load_address,
                   header.image_size, &read) == FR_OK && read == header.image_size)
            state = zzm_verify_image(&header,
                (const uint8_t *)(uintptr_t)header.slot_load_address,
                (uint32_t)f_size(&file), sdk_sha256);
        else if (state == ZZM_STATE_LOADED)
            state = ZZM_STATE_REJECTED_SLOT;
        f_close(&file);
        status_from_slot(i, state, &header);
        if (state != ZZM_STATE_LOADED) {
            printf("[ZZM] %s rejected: %u\n", slot->name, (unsigned)state);
            continue;
        }
        memset((void *)(uintptr_t)(header.slot_load_address + header.image_size), 0,
               header.bss_size);
        Xil_DCacheFlushRange(header.slot_load_address, header.image_size + header.bss_size);
        Xil_ICacheInvalidateRange(header.slot_load_address, header.image_size);
        if (((zzm_init_fn)(uintptr_t)header.init_entry)(zzm_runtime_services()) != 0) {
            status_from_slot(i, ZZM_STATE_REJECTED_ABI, &header);
            printf("[ZZM] %s rejected: init\n", slot->name);
            continue;
        }
        printf("[ZZM] %s loaded v%lu\n", slot->name, (unsigned long)header.module_version);
    }
}

uint32_t zzm_loader_count(void) { return zzm_slot_count; }
int zzm_loader_status(uint32_t index, zzm_status_t *out)
{
    if (!out || index >= zzm_slot_count) return 0;
    *out = module_status[index];
    return 1;
}
