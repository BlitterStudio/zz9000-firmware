#ifndef ZZM_LOADER_H
#define ZZM_LOADER_H

#include <stdint.h>
#include "zzm_format.h"

/* A32 UDF #0. Unlike an unallocated opcode, this encoding is permanently
 * undefined by the architecture, so a stale module entry faults reliably. */
#define ZZM_POISON_WORD 0xE7F000F0U


enum zzm_state {
    ZZM_STATE_LOADED = 0,
    ZZM_STATE_MISSING = 1,
    ZZM_STATE_REJECTED_ABI = 2,
    ZZM_STATE_REJECTED_HASH = 3,
    ZZM_STATE_REJECTED_SLOT = 4,
    ZZM_STATE_REJECTED_SERVICE = 5,
    ZZM_STATE_FAULTED = 6,
    ZZM_STATE_AVAILABLE = 7,
    ZZM_STATE_LOADING = 8,
    ZZM_STATE_LOAD_FAILED = 9,
};

enum zzm_load_reason {
    ZZM_REASON_NONE = 0,
    ZZM_REASON_FILE_OPEN = 1,
    ZZM_REASON_FILE_READ = 2,
    ZZM_REASON_IMAGE_INVALID = 3,
    ZZM_REASON_INIT = 4,
    ZZM_REASON_STORAGE_BUSY = 5,
    ZZM_REASON_DEPENDENCY = 6,
    ZZM_REASON_CYCLE = 7,
    ZZM_REASON_QUOTA = 8,
    ZZM_REASON_FAULT = 9,
};

typedef void (*zzm_sha256_fn)(const uint8_t *data, uint32_t length,
                              uint8_t digest[ZZM_SHA256_SIZE]);

typedef struct zzm_status {
    uint32_t id;
    uint32_t version;
    uint32_t abi_version;
    uint32_t load_address;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t state;
    uint32_t ref_count;
    uint32_t reason;
    char name[ZZM_NAME_SIZE];
} zzm_status_t;


/* Host-buildable verifier. `file_size` includes the header. */
enum zzm_state zzm_verify_image(const zzm_header_t *header,
                                const uint8_t *image, uint32_t file_size,
                                zzm_sha256_fn sha256);
void zzm_loader_boot_load(void);
int zzm_loader_ensure_loaded(uint32_t module_id);
int zzm_loader_acquire(uint32_t module_id);
void zzm_loader_release(uint32_t module_id);
void zzm_loader_poll(uint64_t now_ms);
void zzm_loader_mark_faulted(uint32_t module_id, const char *reason);
void zzm_loader_mark_load_failed(uint32_t module_id, uint32_t reason);
uint32_t zzm_loader_count(void);
int zzm_loader_status_by_id(uint32_t module_id, zzm_status_t *out);
int zzm_loader_status(uint32_t index, zzm_status_t *out);
int zzm_loader_can_register(uint32_t module_id);
int zzm_loader_service_declared(uint32_t module_id, uint32_t service_id,
                                uint32_t version);
uint32_t zzm_loader_service_declared_count(uint32_t module_id);
void zzm_loader_set_storage_idle(int (*storage_idle)(void));
int zzm_loader_ensure_service(uint32_t service_id, uint32_t min_version);
int zzm_loader_load_on_owner(uint32_t module_id);
void zzm_loader_set_load_dispatch(int (*dispatch)(uint32_t module_id));

#endif
