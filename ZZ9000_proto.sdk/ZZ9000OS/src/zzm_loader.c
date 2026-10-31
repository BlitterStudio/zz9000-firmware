#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "xil_cache.h"
#include "sdk_crypto.h"
#include "sdk_smp_lock.h"
#include "zzm_loader.h"
#include "zzm_manifest.h"
#include "zzm_registry.h"
#include "zzm_runtime.h"
#define ZZM_MODULE_MAX 8U
#define ZZM_MODULE_COUNT ZZM_MODULE_MAX

/* The loader is also linked into its small host verifier.  Keep registry and
 * allocator cleanup optional there; firmware links the strong definitions. */
extern void zzm_registry_unregister_module(uint32_t module_id) __attribute__((weak));
extern int zzm_registry_module_services_ready(uint32_t module_id) __attribute__((weak));
extern const void *zzm_registry_get_service(uint32_t service_id,
                                             uint32_t min_version) __attribute__((weak));
extern int zzm_runtime_quota_available(uint32_t memory_class,
                                       uint32_t quota) __attribute__((weak));
extern uint32_t zzm_runtime_release_module(uint32_t module_id) __attribute__((weak));
extern const zzm_services_t *zzm_runtime_services_v3(void) __attribute__((weak));

static zzm_status_t module_status[ZZM_MODULE_MAX];
static zzm_header_t module_header[ZZM_MODULE_MAX];
static zzm_v3_header_t module_v3_header[ZZM_MODULE_MAX];
static uint8_t module_format[ZZM_MODULE_MAX];
static uint8_t module_header_valid[ZZM_MODULE_MAX];
static uint8_t module_dependencies[ZZM_MODULE_MAX][ZZM_V3_MAX_SERVICE_REQUIREMENTS];
static uint8_t module_dependency_count[ZZM_MODULE_MAX];
static uint8_t module_dependencies_held[ZZM_MODULE_MAX];
static uint64_t module_idle_since[ZZM_MODULE_MAX];
static uint64_t last_poll_ms;
static int (*storage_idle)(void);
static int (*load_dispatch)(uint32_t module_id);
static zzm_v3_capabilities_t v3_capabilities;

/* Session-begin module callbacks run on core 1 while core 0 services idle
 * unloading.  This lock prevents a callback from observing a reclaimed
 * module image; it is recursive because load/init paths call back into the
 * loader. */
static sdk_smp_lock_t session_begin_lock = SDK_SMP_LOCK_INIT;

void zzm_loader_session_begin_lock(void)
{
    sdk_smp_lock_acquire(&session_begin_lock);
}

void zzm_loader_session_begin_unlock(void)
{
    sdk_smp_lock_release(&session_begin_lock);
}

void zzm_loader_reset_session_lock_if_owned(void)
{
    sdk_smp_lock_reset_if_owned(&session_begin_lock);
}

void zzm_loader_reset_session_lock(void)
{
    sdk_smp_lock_reset(&session_begin_lock);
}

static int module_index(uint32_t module_id)
{
    uint32_t i;
    for (i = 0; i < zzm_slot_count; i++)
        if (zzm_slots[i].id == module_id)
            return (int)i;
    return -1;
}

static void status_from_slot(uint32_t index, enum zzm_state state,
                             const zzm_header_t *header, uint32_t reason)
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_status_t *status = &module_status[index];
    uint32_t refs = status->ref_count;

    memset(status, 0, sizeof(*status));
    status->id = slot->id;
    status->load_address = slot->address;
    status->state = state;
    status->reason = reason;
    status->ref_count = refs;
    strncpy(status->name, slot->name, sizeof(status->name) - 1U);
    if (header) {
        status->version = header->module_version;
        status->abi_version = header->module_abi_version;
        status->image_size = header->image_size;
        status->bss_size = header->bss_size;
    }
}


static void status_from_v3_slot(uint32_t index, enum zzm_state state,
                                const zzm_v3_header_t *header,
                                uint32_t reason)
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_status_t *status = &module_status[index];
    uint32_t refs = status->ref_count;

    memset(status, 0, sizeof(*status));
    status->id = slot->id;
    status->load_address = slot->address;
    status->state = state;
    status->reason = reason;
    status->ref_count = refs;
    strncpy(status->name, slot->name, sizeof(status->name) - 1U);
    if (header) {
        status->version = header->module_version;
        status->abi_version = ((uint32_t)header->abi_major << 16) |
                              header->abi_minor;
        status->image_size = header->image_size;
        status->bss_size = header->bss_size;
    }
}
static int runtime_service_present(uint32_t id, uint32_t version)
{
    const zzm_services_t *services = zzm_runtime_services();
    return services && services->get_service && services->get_service(id, version);
}

static enum zzm_state verify_header_shape(const zzm_header_t *header,
                                          uint32_t file_size)
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
        header->load_policy > ZZM_LOAD_BOOT || header->memory_quota == 0U ||
        header->idle_unload_ms == 0U ||
        header->memory_class < ZZM_MEMORY_CLASS_MEDIA ||
        header->memory_class > ZZM_MEMORY_CLASS_GENERAL)
        return ZZM_STATE_REJECTED_SLOT;
    for (i = 0; i < header->required_count; i++) {
        if (!header->required[i].service_id || !header->required[i].min_version)
            return ZZM_STATE_REJECTED_SERVICE;
    }
    for (i = 0; i < header->provided_count; i++) {
        if (!header->provided[i].service_id || !header->provided[i].min_version)
            return ZZM_STATE_REJECTED_SERVICE;
    }
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
        (header->fini_entry && (header->fini_entry < slot->address ||
                                header->fini_entry >= slot->address + header->image_size)) ||
        header->image_size > UINT32_MAX - ZZM_HEADER_SIZE)
        return ZZM_STATE_REJECTED_SLOT;
    expected_size = ZZM_HEADER_SIZE + header->image_size;
    return file_size == expected_size ? ZZM_STATE_AVAILABLE : ZZM_STATE_REJECTED_SLOT;
}

enum zzm_state zzm_verify_image(const zzm_header_t *header,
                                const uint8_t *image, uint32_t file_size,
                                zzm_sha256_fn sha256)
{
    uint8_t digest[ZZM_SHA256_SIZE];
    enum zzm_state state = verify_header_shape(header, file_size);
    uint32_t i;

    if (state != ZZM_STATE_AVAILABLE) return state;
    for (i = 0; i < header->required_count; i++) {
        if (!runtime_service_present(header->required[i].service_id,
                                     header->required[i].min_version) &&
            (!zzm_registry_get_service ||
             !zzm_registry_get_service(header->required[i].service_id,
                                       header->required[i].min_version)))
            return ZZM_STATE_REJECTED_SERVICE;
    }
    if (!image || !sha256) return ZZM_STATE_REJECTED_SLOT;
    sha256(image, header->image_size, digest);
    return memcmp(digest, header->image_sha256, sizeof(digest)) == 0
               ? ZZM_STATE_LOADED : ZZM_STATE_REJECTED_HASH;
}

static enum zzm_state verify_v3_header_shape(const zzm_v3_header_t *header,
                                             uint32_t file_size,
                                             const zzm_v3_capabilities_t *caps)
{
    const zzm_slot_t *slot;
    const uint32_t known_image_flags = ZZM_IMAGE_NEEDS_CORE1 |
        ZZM_IMAGE_NEEDS_HOST_VISIBLE | ZZM_IMAGE_NEEDS_VFP_NEON |
        ZZM_IMAGE_NEEDS_RASTERIX_EF | ZZM_IMAGE_NEEDS_PL330;
    const uint32_t known_owner_classes = ZZM_OWNER_CLASS_MINIGL_CONTEXT |
        ZZM_OWNER_CLASS_MINIGL_APP_CONTEXT | ZZM_OWNER_CLASS_MINIGL_TRANSPORT |
        ZZM_OWNER_CLASS_MINIGL_RIX;
    uint32_t expected_size;
    uint32_t i;

    if (!header || !caps || file_size < ZZM_V3_HEADER_SIZE ||
        memcmp(header->magic, ZZM_V3_MAGIC, 4) != 0 ||
        header->header_size != ZZM_V3_HEADER_SIZE ||
        header->format_version != ZZM_V3_FORMAT_VERSION)
        return ZZM_STATE_REJECTED_SLOT;
    if (header->abi_major != ZZM_V3_ABI_MAJOR ||
        header->abi_minor > ZZM_V3_ABI_MINOR)
        return ZZM_STATE_REJECTED_ABI;
    if (memchr(header->name, '\0', ZZM_NAME_SIZE) == 0 ||
        header->required_count > ZZM_V3_MAX_SERVICE_REQUIREMENTS ||
        header->provided_count > ZZM_V3_MAX_SERVICE_PROVISIONS ||
        header->load_policy > ZZM_LOAD_BOOT || header->idle_unload_ms == 0U ||
        header->resident_quota == 0U || header->per_context_quota == 0U ||
        header->owner_class_mask == 0U ||
        (header->owner_class_mask & ~known_owner_classes) != 0U ||
        (header->image_flags & ~known_image_flags) != 0U ||
        header->hardware_requirement > ZZM_HARDWARE_RASTERIX_EF_REQUIRED ||
        header->worker_ops_version == 0U)
        return ZZM_STATE_REJECTED_SLOT;
    if (header->hardware_requirement == ZZM_HARDWARE_RASTERIX_EF_REQUIRED ||
        (header->image_flags & ~caps->image_flags) != 0U ||
        header->required_caps.wire_version != caps->wire_version ||
        header->required_caps.transport_descriptor_major !=
            caps->transport_descriptor_major ||
        header->required_caps.host_window_layout_major !=
            caps->host_window_layout_major)
        return ZZM_STATE_REJECTED_CAPABILITY;
    if (header->image_flags & ZZM_IMAGE_NEEDS_RASTERIX_EF) {
        if (header->hardware_requirement == ZZM_HARDWARE_CPU_ONLY ||
            header->required_caps.rasterix_csr_abi_major !=
                caps->rasterix_csr_abi_major ||
            header->required_caps.rasterix_csr_abi_minor >
                caps->rasterix_csr_abi_minor)
            return ZZM_STATE_REJECTED_CAPABILITY;
    }
    for (i = 0; i < header->required_count; i++) {
        if (!header->required[i].service_id || !header->required[i].min_version)
            return ZZM_STATE_REJECTED_SERVICE;
    }
    for (i = 0; i < header->provided_count; i++) {
        if (!header->provided[i].service_id || !header->provided[i].min_version)
            return ZZM_STATE_REJECTED_SERVICE;
    }
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
        (header->fini_entry && (header->fini_entry < slot->address ||
                                header->fini_entry >= slot->address + header->image_size)) ||
        header->image_size > UINT32_MAX - ZZM_V3_HEADER_SIZE)
        return ZZM_STATE_REJECTED_SLOT;
    expected_size = ZZM_V3_HEADER_SIZE + header->image_size;
    return file_size == expected_size ? ZZM_STATE_AVAILABLE : ZZM_STATE_REJECTED_SLOT;
}

enum zzm_state zzm_verify_v3_image(const zzm_v3_header_t *header,
                                   const uint8_t *image, uint32_t file_size,
                                   const zzm_v3_capabilities_t *caps,
                                   zzm_sha256_fn sha256)
{
    uint8_t digest[ZZM_SHA256_SIZE];
    enum zzm_state state = verify_v3_header_shape(header, file_size, caps);
    uint32_t i;

    if (state != ZZM_STATE_AVAILABLE) return state;
    for (i = 0; i < header->required_count; i++) {
        if (!runtime_service_present(header->required[i].service_id,
                                     header->required[i].min_version) &&
            (!zzm_registry_get_service ||
             !zzm_registry_get_service(header->required[i].service_id,
                                       header->required[i].min_version)))
            return ZZM_STATE_REJECTED_SERVICE;
    }
    if (!image || !sha256) return ZZM_STATE_REJECTED_SLOT;
    sha256(image, header->image_size, digest);
    return memcmp(digest, header->image_sha256, sizeof(digest)) == 0
               ? ZZM_STATE_LOADED : ZZM_STATE_REJECTED_HASH;
}

void zzm_loader_set_v3_capabilities(const zzm_v3_capabilities_t *caps)
{
    if (caps)
        v3_capabilities = *caps;
    else
        memset(&v3_capabilities, 0, sizeof(v3_capabilities));
}

static int find_provider(uint32_t consumer, uint32_t service_id,
                         uint32_t min_version)
{
    uint32_t i;
    uint32_t j;
    for (i = 0; i < zzm_slot_count; i++) {
        if (i == consumer || !module_header_valid[i] ||
            module_status[i].state != ZZM_STATE_AVAILABLE)
            continue;
        for (j = 0; j < module_header[i].provided_count; j++) {
            if (module_header[i].provided[j].service_id == service_id &&
                module_header[i].provided[j].min_version >= min_version)
                return (int)i;
        }
    }
    return -1;
}

/* Run once after the header-only scan.  Firmware-owned services are already
 * callable; module-provided ones become load-order edges. */
static void resolve_dependencies(void)
{
    uint32_t i;
    uint32_t j;
    for (i = 0; i < zzm_slot_count; i++) {
        module_dependency_count[i] = 0U;
        if (module_status[i].state != ZZM_STATE_AVAILABLE)
            continue;
        if (module_format[i] == ZZM_V3_FORMAT_VERSION) {
            for (j = 0; j < module_v3_header[i].required_count; j++) {
                if (!runtime_service_present(
                        module_v3_header[i].required[j].service_id,
                        module_v3_header[i].required[j].min_version)) {
                    status_from_v3_slot(i, ZZM_STATE_REJECTED_SERVICE,
                                        &module_v3_header[i],
                                        module_v3_header[i].required[j].service_id);
                    break;
                }
            }
            continue;
        }
        for (j = 0; j < module_header[i].required_count; j++) {
            int provider;
            if (runtime_service_present(module_header[i].required[j].service_id,
                                        module_header[i].required[j].min_version))
                continue;
            provider = find_provider(i, module_header[i].required[j].service_id,
                                     module_header[i].required[j].min_version);
            if (provider < 0) {
                status_from_slot(i, ZZM_STATE_REJECTED_SERVICE, &module_header[i],
                                 module_header[i].required[j].service_id);
                break;
            }
            module_dependencies[i][module_dependency_count[i]++] = (uint8_t)provider;
        }
    }
}

static void poison_slot(uint32_t index)
{
    const zzm_header_t *header = &module_header[index];
    uint32_t bytes = header->image_size + header->bss_size;
    uint32_t *word = (uint32_t *)(uintptr_t)header->slot_load_address;
    uint32_t count = bytes / sizeof(*word);
    uint32_t i;

    if (!module_header_valid[index] || bytes == 0U)
        return;
    for (i = 0; i < count; i++)
        word[i] = ZZM_POISON_WORD;
    if (bytes & (sizeof(*word) - 1U))
        memset((uint8_t *)(void *)(word + count), 0xf0, bytes & 3U);
    Xil_DCacheFlushRange(header->slot_load_address, bytes);
    Xil_ICacheInvalidateRange(header->slot_load_address, bytes);
}

static void unregister_module(uint32_t index)
{
    if (zzm_registry_unregister_module)
        zzm_registry_unregister_module(module_status[index].id);
}

static void unregister_and_reclaim(uint32_t index, int run_fini)
{
    const zzm_header_t *header = &module_header[index];

    unregister_module(index);
    if (run_fini && header->fini_entry)
        ((zzm_fini_fn)(uintptr_t)header->fini_entry)();
    if (zzm_runtime_release_module)
        (void)zzm_runtime_release_module(module_status[index].id);
    poison_slot(index);
}

static void release_dependencies(uint32_t index)
{
    uint32_t dependency;

    for (dependency = 0; dependency < module_dependencies_held[index]; dependency++)
        zzm_loader_release(zzm_slots[module_dependencies[index][dependency]].id);
    module_dependencies_held[index] = 0U;
}

static void load_failed(uint32_t index, enum zzm_state state, uint32_t reason)
{
    unregister_and_reclaim(index, 0);
    release_dependencies(index);
    module_status[index].ref_count = 0U;
    status_from_slot(index, state, &module_header[index], reason);
}

static void poison_v3_slot(uint32_t index)
{
    const zzm_v3_header_t *header = &module_v3_header[index];
    uint32_t bytes = header->image_size + header->bss_size;
    uint32_t *word = (uint32_t *)(uintptr_t)header->slot_load_address;
    uint32_t count = bytes / sizeof(*word);
    uint32_t i;

    if (!module_header_valid[index] || bytes == 0U)
        return;
    for (i = 0; i < count; i++)
        word[i] = ZZM_POISON_WORD;
    if (bytes & (sizeof(*word) - 1U))
        memset((uint8_t *)(void *)(word + count), 0xf0, bytes & 3U);
    Xil_DCacheFlushRange(header->slot_load_address, bytes);
    Xil_ICacheInvalidateRange(header->slot_load_address, bytes);
}

static void load_v3_failed(uint32_t index, enum zzm_state state, uint32_t reason)
{
    unregister_module(index);
    if (zzm_runtime_release_module)
        (void)zzm_runtime_release_module(module_status[index].id);
    poison_v3_slot(index);
    module_status[index].ref_count = 0U;
    status_from_v3_slot(index, state, &module_v3_header[index], reason);
}

static int load_v3_module(uint32_t index)
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_v3_header_t *header = &module_v3_header[index];
    FIL file;
    UINT read = 0;
    char path[32];

    if (module_status[index].state == ZZM_STATE_LOADED)
        return 1;
    if (module_status[index].state != ZZM_STATE_AVAILABLE ||
        !module_header_valid[index])
        return 0;
    if (storage_idle && !storage_idle()) {
        status_from_v3_slot(index, ZZM_STATE_LOAD_FAILED, header,
                            ZZM_REASON_STORAGE_BUSY);
        return 0;
    }
    module_status[index].state = ZZM_STATE_LOADING;

    if (zzm_runtime_quota_available &&
        !zzm_runtime_quota_available(ZZM_MEMORY_CLASS_MINIGL_MODULE,
                                     header->resident_quota)) {
        load_v3_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_QUOTA);
        return 0;
    }
    snprintf(path, sizeof(path), "0:/ZZ9K/%s.ZZM", slot->name);
    if (f_open(&file, path, FA_READ) != FR_OK) {
        load_v3_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_FILE_OPEN);
        return 0;
    }
    if (f_lseek(&file, ZZM_V3_HEADER_SIZE) != FR_OK ||
        f_read(&file, (void *)(uintptr_t)header->slot_load_address,
               header->image_size, &read) != FR_OK || read != header->image_size) {
        (void)f_close(&file);
        load_v3_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_FILE_READ);
        return 0;
    }
    (void)f_close(&file);
    if (zzm_verify_v3_image(header,
                            (const uint8_t *)(uintptr_t)header->slot_load_address,
                            ZZM_V3_HEADER_SIZE + header->image_size,
                            &v3_capabilities, sdk_sha256) != ZZM_STATE_LOADED) {
        load_v3_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_IMAGE_INVALID);
        return 0;
    }
    memset((void *)(uintptr_t)(header->slot_load_address + header->image_size), 0,
           header->bss_size);
    Xil_DCacheFlushRange(header->slot_load_address,
                         header->image_size + header->bss_size);
    Xil_ICacheInvalidateRange(header->slot_load_address, header->image_size);
    if (!zzm_runtime_services_v3 ||
        ((int (*)(const void *))(uintptr_t)header->init_entry)(
            zzm_runtime_services_v3()) != 0 ||
        (zzm_registry_module_services_ready &&
         !zzm_registry_module_services_ready(module_status[index].id))) {
        load_v3_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_INIT);
        return 0;
    }
    status_from_v3_slot(index, ZZM_STATE_LOADED, header, ZZM_REASON_NONE);
    module_idle_since[index] = last_poll_ms;
    printf("[ZZM] %s loaded v%lu\n", slot->name,
           (unsigned long)header->module_version);
    return 1;
}
static int load_module(uint32_t index, uint8_t visiting[ZZM_MODULE_COUNT])
{
    const zzm_slot_t *slot = &zzm_slots[index];
    zzm_header_t *header = &module_header[index];
    FIL file;
    UINT read = 0;
    char path[32];
    uint32_t i;
    if (module_format[index] == ZZM_V3_FORMAT_VERSION)
        return load_v3_module(index);


    if (module_status[index].state == ZZM_STATE_LOADED)
        return 1;
    if (module_status[index].state == ZZM_STATE_LOADING || visiting[index]) {
        status_from_slot(index, ZZM_STATE_REJECTED_SERVICE, header, ZZM_REASON_CYCLE);
        return 0;
    }
    if (module_status[index].state == ZZM_STATE_LOAD_FAILED)
        status_from_slot(index, ZZM_STATE_AVAILABLE, header, ZZM_REASON_NONE);
    if (module_status[index].state != ZZM_STATE_AVAILABLE || !module_header_valid[index])
        return 0;
    if (storage_idle && !storage_idle()) {
        status_from_slot(index, ZZM_STATE_LOAD_FAILED, header, ZZM_REASON_STORAGE_BUSY);
        return 0;
    }

    visiting[index] = 1U;
    module_status[index].state = ZZM_STATE_LOADING;
    for (i = 0; i < module_dependency_count[index]; i++) {
        uint32_t dependency = module_dependencies[index][i];
        if (!load_module(dependency, visiting)) {
            uint32_t reason = module_status[dependency].reason;
            load_failed(index, reason == ZZM_REASON_CYCLE ?
                        ZZM_STATE_REJECTED_SERVICE : ZZM_STATE_LOAD_FAILED,
                        reason == ZZM_REASON_CYCLE ? reason : ZZM_REASON_DEPENDENCY);
            visiting[index] = 0U;
            return 0;
        }
        module_status[dependency].ref_count++;
        module_dependencies_held[index] = (uint8_t)(i + 1U);
    }
    if (zzm_runtime_quota_available &&
        !zzm_runtime_quota_available(header->memory_class, header->memory_quota)) {
        load_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_QUOTA);
        visiting[index] = 0U;
        return 0;
    }
    snprintf(path, sizeof(path), "0:/ZZ9K/%s.ZZM", slot->name);
    if (f_open(&file, path, FA_READ) != FR_OK) {
        load_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_FILE_OPEN);
        visiting[index] = 0U;
        return 0;
    }
    if (f_lseek(&file, ZZM_HEADER_SIZE) != FR_OK ||
        f_read(&file, (void *)(uintptr_t)header->slot_load_address,
               header->image_size, &read) != FR_OK || read != header->image_size) {
        (void)f_close(&file);
        load_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_FILE_READ);
        visiting[index] = 0U;
        return 0;
    }
    (void)f_close(&file);
    if (zzm_verify_image(header,
                         (const uint8_t *)(uintptr_t)header->slot_load_address,
                         ZZM_HEADER_SIZE + header->image_size, sdk_sha256) != ZZM_STATE_LOADED) {
        load_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_IMAGE_INVALID);
        visiting[index] = 0U;
        return 0;
    }
    memset((void *)(uintptr_t)(header->slot_load_address + header->image_size), 0,
           header->bss_size);
    Xil_DCacheFlushRange(header->slot_load_address, header->image_size + header->bss_size);
    Xil_ICacheInvalidateRange(header->slot_load_address, header->image_size);
    if (((zzm_init_fn)(uintptr_t)header->init_entry)(zzm_runtime_services()) != 0 ||
        (zzm_registry_module_services_ready &&
         !zzm_registry_module_services_ready(module_status[index].id))) {
        load_failed(index, ZZM_STATE_LOAD_FAILED, ZZM_REASON_INIT);
        visiting[index] = 0U;
        return 0;
    }
    status_from_slot(index, ZZM_STATE_LOADED, header, ZZM_REASON_NONE);
    module_idle_since[index] = last_poll_ms;
    visiting[index] = 0U;
    printf("[ZZM] %s loaded v%lu\n", slot->name, (unsigned long)header->module_version);
    return 1;
}

void zzm_loader_boot_load(void)
{
    uint32_t i;

    for (i = 0; i < zzm_slot_count; i++) {
        const zzm_slot_t *slot = &zzm_slots[i];
        zzm_header_t header;
        zzm_v3_header_t v3_header;
        uint8_t prefix[8];
        FIL file;
        UINT read = 0;
        char path[32];
        enum zzm_state state;

        module_header_valid[i] = 0U;
        module_format[i] = 0U;
        module_idle_since[i] = 0U;
        module_dependencies_held[i] = 0U;
        status_from_slot(i, ZZM_STATE_MISSING, 0, ZZM_REASON_NONE);
        if (storage_idle && !storage_idle()) {
            status_from_slot(i, ZZM_STATE_LOAD_FAILED, 0, ZZM_REASON_STORAGE_BUSY);
            continue;
        }
        snprintf(path, sizeof(path), "0:/ZZ9K/%s.ZZM", slot->name);
        if (f_open(&file, path, FA_READ) != FR_OK)
            continue;
        if (f_read(&file, prefix, sizeof(prefix), &read) != FR_OK ||
            read != sizeof(prefix) || f_lseek(&file, 0U) != FR_OK) {
            (void)f_close(&file);
            status_from_slot(i, ZZM_STATE_REJECTED_SLOT, 0, ZZM_REASON_FILE_READ);
            continue;
        }
        if (memcmp(prefix, ZZM_V3_MAGIC, 4) == 0) {
            if (f_read(&file, &v3_header, sizeof(v3_header), &read) != FR_OK ||
                read != sizeof(v3_header)) {
                (void)f_close(&file);
                status_from_slot(i, ZZM_STATE_REJECTED_SLOT, 0,
                                 ZZM_REASON_FILE_READ);
                continue;
            }
            state = verify_v3_header_shape(&v3_header,
                                           (uint32_t)f_size(&file),
                                           &v3_capabilities);
            if (state == ZZM_STATE_AVAILABLE &&
                (v3_header.module_id != slot->id ||
                 strcmp(v3_header.name, slot->name) != 0))
                state = ZZM_STATE_REJECTED_SLOT;
            (void)f_close(&file);
            status_from_v3_slot(i, state, &v3_header, ZZM_REASON_NONE);
            if (state == ZZM_STATE_AVAILABLE) {
                module_v3_header[i] = v3_header;
                module_format[i] = ZZM_V3_FORMAT_VERSION;
                module_header_valid[i] = 1U;
                printf("[ZZM] %s available v%lu\n", slot->name,
                       (unsigned long)v3_header.module_version);
            }
        } else {
            if (f_read(&file, &header, sizeof(header), &read) != FR_OK ||
                read != sizeof(header)) {
                (void)f_close(&file);
                status_from_slot(i, ZZM_STATE_REJECTED_SLOT, 0,
                                 ZZM_REASON_FILE_READ);
                continue;
            }
            state = verify_header_shape(&header, (uint32_t)f_size(&file));
            if (state == ZZM_STATE_AVAILABLE &&
                (header.module_id != slot->id ||
                 strcmp(header.name, slot->name) != 0))
                state = ZZM_STATE_REJECTED_SLOT;
            (void)f_close(&file);
            status_from_slot(i, state, &header, ZZM_REASON_NONE);
            if (state == ZZM_STATE_AVAILABLE) {
                module_header[i] = header;
                module_format[i] = ZZM_FORMAT_VERSION;
                module_header_valid[i] = 1U;
                printf("[ZZM] %s available v%lu\n", slot->name,
                       (unsigned long)header.module_version);
            }
        }
    }
    resolve_dependencies();
    /* Boot validation intentionally reads headers only. Module code and
     * FatFs payload reads are deferred to the first requesting service. */
}

static int load_on_owner_locked(uint32_t module_id)
{
    uint8_t visiting[ZZM_MODULE_COUNT] = { 0 };
    int index = module_index(module_id);

    if (index < 0)
        return 0;
    return load_module((uint32_t)index, visiting);
}

int zzm_loader_load_on_owner(uint32_t module_id)
{
    int loaded;

    zzm_loader_session_begin_lock();
    loaded = load_on_owner_locked(module_id);
    zzm_loader_session_begin_unlock();
    return loaded;
}

int zzm_loader_ensure_loaded(uint32_t module_id)
{
    int loaded;

    zzm_loader_session_begin_lock();
    loaded = load_dispatch ? load_dispatch(module_id) :
        load_on_owner_locked(module_id);
    zzm_loader_session_begin_unlock();
    return loaded;
}

int zzm_loader_acquire(uint32_t module_id)
{
    int index;
    int acquired = 0;

    zzm_loader_session_begin_lock();
    index = module_index(module_id);
    if (index >= 0 && zzm_loader_ensure_loaded(module_id)) {
        module_status[index].ref_count++;
        module_idle_since[index] = 0U;
        acquired = 1;
    }
    zzm_loader_session_begin_unlock();
    return acquired;
}

void zzm_loader_release(uint32_t module_id)
{
    int index;

    zzm_loader_session_begin_lock();
    index = module_index(module_id);
    if (index >= 0 && module_status[index].state == ZZM_STATE_LOADED &&
        module_status[index].ref_count != 0U) {
        module_status[index].ref_count--;
        if (module_status[index].ref_count == 0U)
            module_idle_since[index] = last_poll_ms;
    }
    zzm_loader_session_begin_unlock();
}

void zzm_loader_poll(uint64_t now_ms)
{
    uint32_t i;

    /* Core 0 must never wait with its IRQs masked for an SD-backed core-1
     * load. A live begin holds this guard, so defer unloading one pass. */
    if (!sdk_smp_lock_try_acquire(&session_begin_lock))
        return;
    last_poll_ms = now_ms;
    for (i = 0; i < zzm_slot_count; i++) {
        uint32_t idle_ms;
        if (module_status[i].state != ZZM_STATE_LOADED ||
            module_status[i].ref_count != 0U)
            continue;
        idle_ms = module_format[i] == ZZM_V3_FORMAT_VERSION ?
            module_v3_header[i].idle_unload_ms : module_header[i].idle_unload_ms;
        if (idle_ms == 0U)
            idle_ms = ZZM_DEFAULT_IDLE_UNLOAD_MS;
        if (now_ms - module_idle_since[i] < idle_ms)
            continue;
        if (module_format[i] == ZZM_V3_FORMAT_VERSION) {
            unregister_module(i);
            if (module_v3_header[i].fini_entry)
                ((zzm_fini_fn)(uintptr_t)module_v3_header[i].fini_entry)();
            if (zzm_runtime_release_module)
                (void)zzm_runtime_release_module(module_status[i].id);
            poison_v3_slot(i);
            status_from_v3_slot(i, ZZM_STATE_AVAILABLE, &module_v3_header[i],
                                ZZM_REASON_NONE);
        } else {
            unregister_and_reclaim(i, 1);
            status_from_slot(i, ZZM_STATE_AVAILABLE, &module_header[i],
                             ZZM_REASON_NONE);
        }
        module_idle_since[i] = 0U;
        release_dependencies(i);
        printf("[ZZM] %s unloaded\n", zzm_slots[i].name);
    }
    zzm_loader_session_begin_unlock();
}

void zzm_loader_mark_faulted(uint32_t module_id, const char *reason)
{
    int index = module_index(module_id);

    (void)reason;
    if (index < 0 || module_status[index].state == ZZM_STATE_FAULTED)
        return;
    /* A fault can be reported from module code. Withdraw every callable
     * entry first, but never poison the slot on that stack; it remains
     * faulted until reboot. */
    unregister_module((uint32_t)index);
    if (zzm_runtime_release_module)
        (void)zzm_runtime_release_module(module_status[index].id);
    module_status[index].ref_count = 0U;
    if (module_format[index] == ZZM_V3_FORMAT_VERSION)
        status_from_v3_slot((uint32_t)index, ZZM_STATE_FAULTED,
                            &module_v3_header[index], ZZM_REASON_FAULT);
    else
        status_from_slot((uint32_t)index, ZZM_STATE_FAULTED,
                         &module_header[index], ZZM_REASON_FAULT);
    release_dependencies((uint32_t)index);
}
void zzm_loader_mark_load_failed(uint32_t module_id, uint32_t reason)
{
    int index = module_index(module_id);
    if (index >= 0 && module_status[index].state == ZZM_STATE_AVAILABLE) {
        if (module_format[index] == ZZM_V3_FORMAT_VERSION)
            status_from_v3_slot((uint32_t)index, ZZM_STATE_LOAD_FAILED,
                                &module_v3_header[index], reason);
        else
            status_from_slot((uint32_t)index, ZZM_STATE_LOAD_FAILED,
                             &module_header[index], reason);
    }
}
int zzm_loader_can_register(uint32_t module_id)
{
    int index = module_index(module_id);
    return index >= 0 && module_status[index].state == ZZM_STATE_LOADING;
}

int zzm_loader_service_declared(uint32_t module_id, uint32_t service_id,
                                uint32_t version)
{
    int index = module_index(module_id);
    uint32_t i;
    if (index < 0 || !module_header_valid[index])
        return 0;
    if (module_format[index] == ZZM_V3_FORMAT_VERSION) {
        for (i = 0; i < module_v3_header[index].provided_count; i++) {
            if (module_v3_header[index].provided[i].service_id == service_id &&
                module_v3_header[index].provided[i].min_version <= version)
                return 1;
        }
        return 0;
    }
    for (i = 0; i < module_header[index].provided_count; i++) {
        if (module_header[index].provided[i].service_id == service_id &&
            module_header[index].provided[i].min_version <= version)
            return 1;
    }
    return 0;
}

uint32_t zzm_loader_service_declared_count(uint32_t module_id)
{
    int index = module_index(module_id);
    if (index < 0 || !module_header_valid[index])
        return 0U;
    return module_format[index] == ZZM_V3_FORMAT_VERSION ?
        module_v3_header[index].provided_count : module_header[index].provided_count;
}

int zzm_loader_ensure_service(uint32_t service_id, uint32_t min_version)
{
    uint32_t i;
    uint32_t j;
    for (i = 0; i < zzm_slot_count; i++) {
        if (!module_header_valid[i] ||
            (module_status[i].state != ZZM_STATE_AVAILABLE &&
             module_status[i].state != ZZM_STATE_LOADED))
            continue;
        if (module_format[i] == ZZM_V3_FORMAT_VERSION) {
            for (j = 0; j < module_v3_header[i].provided_count; j++) {
                if (module_v3_header[i].provided[j].service_id == service_id &&
                    module_v3_header[i].provided[j].min_version >= min_version)
                    return zzm_loader_ensure_loaded(module_status[i].id);
            }
            continue;
        }
        for (j = 0; j < module_header[i].provided_count; j++) {
            if (module_header[i].provided[j].service_id == service_id &&
                module_header[i].provided[j].min_version >= min_version)
                return zzm_loader_ensure_loaded(module_status[i].id);
        }
    }
    return 0;
}

void zzm_loader_set_load_dispatch(int (*dispatch)(uint32_t module_id))
{
    load_dispatch = dispatch;
}
void zzm_loader_set_storage_idle(int (*is_idle)(void)) { storage_idle = is_idle; }
uint32_t zzm_loader_count(void) { return zzm_slot_count; }
int zzm_loader_status_by_id(uint32_t module_id, zzm_status_t *out)
{
    int index = module_index(module_id);

    return index >= 0 && zzm_loader_status((uint32_t)index, out);
}
int zzm_loader_status(uint32_t index, zzm_status_t *out)
{
    if (!out || index >= zzm_slot_count) return 0;
    *out = module_status[index];
    return 1;
}
