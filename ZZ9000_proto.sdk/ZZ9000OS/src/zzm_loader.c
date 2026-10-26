#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "xil_cache.h"
#include "sdk_crypto.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_runtime.h"

static zzm_status_t module_status[6];
static zzm_header_t module_header[6];

static int service_requirements_met(const zzm_header_t *header, uint32_t *missing)
{
    const zzm_services_t *services = zzm_runtime_services();
    uint32_t i;

    if (!services || !services->get_service)
        return header->required_count == 0U;
    for (i = 0; i < header->required_count; i++) {
        if (!services->get_service(header->required[i].service_id,
                                   header->required[i].min_version)) {
            if (missing)
                *missing = header->required[i].service_id;
            return 0;
        }
    }
    return 1;
}

static enum zzm_state zzm_verify_header(const zzm_header_t *header,
                                        uint32_t file_size, uint32_t *missing)
{
    const zzm_slot_t *slot;
    uint32_t expected_size;
    uint32_t i;

    if (!header || file_size < ZZM_HEADER_SIZE ||
        memcmp(header->magic, ZZM_MAGIC, 4) != 0 ||
        header->header_size != ZZM_HEADER_SIZE ||
        header->format_version != ZZM_FORMAT_VERSION)
        return ZZM_STATE_REJECTED_SLOT;
    if (header->module_abi_version != ZZM_MODULE_ABI_VERSION)
        return ZZM_STATE_REJECTED_ABI;
    if (memchr(header->name, '\0', ZZM_NAME_SIZE) == 0 ||
        header->required_count > ZZM_MAX_SERVICE_REQUIREMENTS ||
        header->provided_count > ZZM_MAX_SERVICE_PROVISIONS ||
        header->load_policy > ZZM_LOAD_BOOT || header->memory_quota == 0U)
        return ZZM_STATE_REJECTED_SLOT;
    for (i = 0; i < sizeof(header->reserved); i++) {
        if (header->reserved[i] != 0U)
            return ZZM_STATE_REJECTED_SLOT;
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
    if (file_size != expected_size)
        return ZZM_STATE_REJECTED_SLOT;
    if (!service_requirements_met(header, missing))
        return ZZM_STATE_REJECTED_SERVICE;
    return ZZM_STATE_AVAILABLE;
}

enum zzm_state zzm_verify_image(const zzm_header_t *header,
                                const uint8_t *image, uint32_t file_size,
                                zzm_sha256_fn sha256)
{
    uint8_t digest[ZZM_SHA256_SIZE];
    enum zzm_state state = zzm_verify_header(header, file_size, 0);
    if (state != ZZM_STATE_AVAILABLE) return state;
    if (!image || !sha256) return ZZM_STATE_REJECTED_SLOT;
    sha256(image, header->image_size, digest);
    return memcmp(digest, header->image_sha256, sizeof(digest)) == 0
               ? ZZM_STATE_LOADED : ZZM_STATE_REJECTED_HASH;
}

static void status_from_slot(uint32_t index, enum zzm_state state,
                             const zzm_header_t *header, uint32_t reason)
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_status_t *status = &module_status[index];
    memset(status, 0, sizeof(*status));
    status->id = slot->id;
    status->load_address = slot->address;
    status->state = state;
    status->reason = reason;
    strncpy(status->name, slot->name, sizeof(status->name) - 1U);
    if (header) {
        status->version = header->module_version;
        status->abi_version = header->module_abi_version;
        status->image_size = header->image_size;
        status->bss_size = header->bss_size;
    }
}

static int module_index(uint32_t module_id)
{
    uint32_t i;
    for (i = 0; i < zzm_slot_count; i++)
        if (zzm_slots[i].id == module_id)
            return (int)i;
    return -1;
}

static int load_module(uint32_t index)
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_header_t *header = &module_header[index];
    FIL file;
    UINT read = 0;
    char path[32];
    enum zzm_state state;

    if (module_status[index].state == ZZM_STATE_LOADED) {
        module_status[index].ref_count++;
        return 1;
    }
    if (module_status[index].state != ZZM_STATE_AVAILABLE &&
        module_status[index].state != ZZM_STATE_LOAD_FAILED)
        return 0;
    module_status[index].state = ZZM_STATE_LOADING;
    snprintf(path, sizeof(path), "0:/ZZ9K/%s.ZZM", slot->name);
    if (f_open(&file, path, FA_READ) != FR_OK) {
        status_from_slot(index, ZZM_STATE_LOAD_FAILED, header, 1U);
        return 0;
    }
    /* Header was validated during the boot scan; read only the image now. */
    if (f_read(&file, (void *)(uintptr_t)header->slot_load_address,
               header->image_size, &read) != FR_OK || read != header->image_size) {
        f_close(&file);
        status_from_slot(index, ZZM_STATE_LOAD_FAILED, header, 2U);
        return 0;
    }
    f_close(&file);
    state = zzm_verify_image(header,
                              (const uint8_t *)(uintptr_t)header->slot_load_address,
                              ZZM_HEADER_SIZE + header->image_size, sdk_sha256);
    if (state != ZZM_STATE_LOADED) {
        status_from_slot(index, state == ZZM_STATE_REJECTED_HASH ? state :
                         ZZM_STATE_LOAD_FAILED, header, 3U);
        return 0;
    }
    memset((void *)(uintptr_t)(header->slot_load_address + header->image_size), 0,
           header->bss_size);
    Xil_DCacheFlushRange(header->slot_load_address, header->image_size + header->bss_size);
    Xil_ICacheInvalidateRange(header->slot_load_address, header->image_size);
    /* Registration is part of init, so make the module visible only for that
     * transaction; failed init immediately becomes LOAD_FAILED. */
    status_from_slot(index, ZZM_STATE_LOADED, header, 0U);
    if (((zzm_init_fn)(uintptr_t)header->init_entry)(zzm_runtime_services()) != 0) {
        status_from_slot(index, ZZM_STATE_LOAD_FAILED, header, 4U);
        return 0;
    }
    module_status[index].ref_count = 1U;
    printf("[ZZM] %s loaded v%lu\n", slot->name, (unsigned long)header->module_version);
    return 1;
}

void zzm_loader_boot_load(void)
{
    uint32_t i;
    for (i = 0; i < zzm_slot_count; i++) {
        const zzm_slot_t *slot = &zzm_slots[i];
        zzm_header_t header;
        FIL file;
        UINT read = 0;
        char path[32];
        FRESULT fr;
        uint32_t missing = 0U;
        enum zzm_state state;

        status_from_slot(i, ZZM_STATE_MISSING, 0, 0U);
        snprintf(path, sizeof(path), "0:/ZZ9K/%s.ZZM", slot->name);
        fr = f_open(&file, path, FA_READ);
        if (fr != FR_OK)
            continue;
        if (f_read(&file, &header, sizeof(header), &read) != FR_OK ||
            read != sizeof(header)) {
            f_close(&file);
            status_from_slot(i, ZZM_STATE_REJECTED_SLOT, 0, 0U);
            continue;
        }
        state = zzm_verify_header(&header, (uint32_t)f_size(&file), &missing);
        f_close(&file);
        status_from_slot(i, state, &header, missing);
        if (state == ZZM_STATE_AVAILABLE) {
            module_header[i] = header;
            printf("[ZZM] %s available v%lu\n", slot->name,
                   (unsigned long)header.module_version);
            if (header.load_policy == ZZM_LOAD_BOOT)
                (void)load_module(i);
        }
    }
}

int zzm_loader_ensure_loaded(uint32_t module_id)
{
    int index = module_index(module_id);
    return index >= 0 ? load_module((uint32_t)index) : 0;
}

void zzm_loader_release(uint32_t module_id)
{
    int index = module_index(module_id);
    if (index >= 0 && module_status[index].state == ZZM_STATE_LOADED &&
        module_status[index].ref_count != 0U)
        module_status[index].ref_count--;
}

void zzm_loader_mark_faulted(uint32_t module_id, const char *reason)
{
    int index = module_index(module_id);
    (void)reason;
    if (index >= 0)
        module_status[index].state = ZZM_STATE_FAULTED;
}

uint32_t zzm_loader_count(void) { return zzm_slot_count; }
int zzm_loader_status(uint32_t index, zzm_status_t *out)
{
    if (!out || index >= zzm_slot_count) return 0;
    *out = module_status[index];
    return 1;
}
