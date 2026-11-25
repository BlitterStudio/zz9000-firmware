#ifndef ZZM_LOADER_H
#define ZZM_LOADER_H

#include <stdint.h>
#include "zzm_format.h"

/* A32 UDF #0. Unlike an unallocated opcode, this encoding is permanently
 * undefined by the architecture, so a stale module entry faults reliably. */
#define ZZM_POISON_WORD 0xE7F000F0U
/* Core-1 module loading is sliced to about 1 ms (333k global-timer ticks).
 * COUNTS_PER_SECOND is CPU/2, so the 666 MHz CPU's global timer is 333 MHz. */
#ifndef ZZM_LOAD_READ_HASH_CHUNK_BYTES
#ifdef ZZM_V3_READ_HASH_CHUNK_BYTES
#define ZZM_LOAD_READ_HASH_CHUNK_BYTES ZZM_V3_READ_HASH_CHUNK_BYTES
#else
#define ZZM_LOAD_READ_HASH_CHUNK_BYTES (12U * 1024U)
#endif
#endif
#ifndef ZZM_LOAD_MEMORY_CHUNK_BYTES
#ifdef ZZM_V3_MEMORY_CHUNK_BYTES
#define ZZM_LOAD_MEMORY_CHUNK_BYTES ZZM_V3_MEMORY_CHUNK_BYTES
#else
#define ZZM_LOAD_MEMORY_CHUNK_BYTES (64U * 1024U)
#endif
#endif
#ifndef ZZM_V3_READ_HASH_CHUNK_BYTES
#define ZZM_V3_READ_HASH_CHUNK_BYTES ZZM_LOAD_READ_HASH_CHUNK_BYTES
#endif
#ifndef ZZM_V3_MEMORY_CHUNK_BYTES
#define ZZM_V3_MEMORY_CHUNK_BYTES ZZM_LOAD_MEMORY_CHUNK_BYTES
#endif



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
    ZZM_STATE_REJECTED_CAPABILITY = 10,
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
enum zzm_load_result {
    ZZM_LOAD_FAILED = 0,
    ZZM_LOAD_READY = 1,
    ZZM_LOAD_IN_PROGRESS = 2,
};


typedef void (*zzm_sha256_fn)(const uint8_t *data, uint32_t length,
                              uint8_t digest[ZZM_SHA256_SIZE]);

typedef struct zzm_v3_capabilities {
    uint32_t image_flags;
    uint32_t wire_version;
    uint32_t transport_descriptor_major;
    uint32_t rasterix_csr_abi_major;
    uint32_t rasterix_csr_abi_minor;
    uint32_t host_window_layout_major;
} zzm_v3_capabilities_t;

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


/* Host-buildable verifiers. `file_size` includes the selected header. */
enum zzm_state zzm_verify_image(const zzm_header_t *header,
                                const uint8_t *image, uint32_t file_size,
                                zzm_sha256_fn sha256);
enum zzm_state zzm_verify_v3_image(const zzm_v3_header_t *header,
                                   const uint8_t *image, uint32_t file_size,
                                   const zzm_v3_capabilities_t *caps,
                                   zzm_sha256_fn sha256);
void zzm_loader_set_v3_capabilities(const zzm_v3_capabilities_t *caps);
void zzm_loader_boot_load(void);
/*
 * Keep the lookup, module callbacks, and reference acquire of a session
 * begin atomic with idle unloading.  Recursive: loader internals acquire it
 * as well, so a guarded begin can load a module without dropping the guard.
 *
 * Core 1 holds the guard across every SD-backed load step with local IRQs
 * masked, so core 0 never waits for it.  _enter returns 1 with the guard
 * held, or 0 on core 0 when core 1 holds it: the caller returns
 * SDK_TASK_IN_PROGRESS, which the Amiga side retries, before touching any
 * module state.  On any other core _enter waits and returns 1.  Every
 * successful _enter pairs with _unlock.
 */
int zzm_loader_session_begin_enter(void);
void zzm_loader_session_begin_unlock(void);
/* Core-1 fault recovery only. */
void zzm_loader_reset_session_lock_if_owned(void);
void zzm_loader_reset_session_lock(void);
/* Core 0 gets ZZM_LOAD_IN_PROGRESS instead of waiting for the guard; with a
 * load dispatcher set it also reports a queued load as in progress. */
int zzm_loader_ensure_loaded(uint32_t module_id);
/* Same core 0 rule as ensure_loaded. */
int zzm_loader_acquire(uint32_t module_id);
/* Core 0 never waits: a release that finds the guard held is applied by the
 * next zzm_loader_poll that gets it. */
void zzm_loader_release(uint32_t module_id);
void zzm_loader_poll(uint64_t now_ms);
void zzm_loader_mark_faulted(uint32_t module_id, const char *reason);
/* Core 0 only, while core1_cold_restart holds core 1 in reset. */
void zzm_loader_forget_core1_modules(void);
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
/* Core 1 (or a single-core build) only: runs one load step and waits for the
 * session-begin guard. */
int zzm_loader_load_on_owner(uint32_t module_id);
/* Core 1 only, like load_on_owner: cancel a resumable module load. The Amiga
 * reset path on core 0 flags the cancel and the core-1 task performs it. */
void zzm_loader_cancel_loading(uint32_t module_id);
void zzm_loader_set_load_dispatch(int (*dispatch)(uint32_t module_id));
#ifdef ZZM_LOADER_HOST_TEST
enum zzm_loader_test_work {
    ZZM_LOADER_TEST_WORK_NONE = 0,
    ZZM_LOADER_TEST_WORK_READ_HASH,
    ZZM_LOADER_TEST_WORK_BSS_ZERO,
    ZZM_LOADER_TEST_WORK_DCACHE_FLUSH,
    ZZM_LOADER_TEST_WORK_ICACHE_INVALIDATE,
    ZZM_LOADER_TEST_WORK_POISON,
};
uint32_t zzm_loader_test_work_bytes(void);
enum zzm_loader_test_work zzm_loader_test_work_kind(void);
#endif


#endif
