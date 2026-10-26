#ifndef ZZM_RUNTIME_H
#define ZZM_RUNTIME_H

#include <stdint.h>
#include "zzm_format.h"

enum zzm_service_id {
    ZZM_SERVICE_CORE = 1U,
    ZZM_SERVICE_MEMORY = 2U,
    ZZM_SERVICE_CACHE = 3U,
    ZZM_SERVICE_RECLAIM = 4U,
    /* Reserved for core-1 worker dispatch. It is intentionally unavailable
     * in v2 so future parallel decoders do not require an ABI break. */
    ZZM_SERVICE_JOBS = 5U,
};

#define ZZM_SERVICE_CORE_V1 1U
#define ZZM_SERVICE_MEMORY_V1 1U
#define ZZM_SERVICE_CACHE_V1 1U
#define ZZM_SERVICE_RECLAIM_V1 1U
#define ZZM_SERVICE_JOBS_V1 1U

enum zzm_memory_class {
    ZZM_MEMORY_CLASS_MEDIA = 1U,
    ZZM_MEMORY_CLASS_MEDIA_INPUT = 2U,
    ZZM_MEMORY_CLASS_IMAGE = 3U,
    ZZM_MEMORY_CLASS_GENERAL = 4U,
};

typedef struct zzm_allocation_context {
    uint32_t owner;
    uint32_t memory_class;
    uint32_t quota;
} zzm_allocation_context_t;

typedef struct zzm_core_service {
    uint32_t version;
    uint32_t size;
    void (*log)(const char *message);
    void (*fault)(const char *reason);
    uint64_t (*time_us)(void);
    void (*module_fault)(uint32_t module_id, const char *reason);
} zzm_core_service_t;

typedef struct zzm_memory_service {
    uint32_t version;
    uint32_t size;
    uint32_t (*alloc)(uint32_t owner, uint32_t memory_class, uint32_t quota,
                      uint32_t bytes, uint32_t alignment);
    int (*free)(uint32_t address, uint32_t owner);
    uint32_t (*realloc)(uint32_t address, uint32_t owner, uint32_t bytes,
                        uint32_t alignment);
} zzm_memory_service_t;

typedef struct zzm_cache_service {
    uint32_t version;
    uint32_t size;
    void (*dcache_clean)(uint32_t address, uint32_t length);
    void (*icache_invalidate)(uint32_t address, uint32_t length);
} zzm_cache_service_t;

typedef struct zzm_reclaim_service {
    uint32_t version;
    uint32_t size;
    uint32_t (*release_owner)(uint32_t owner);
} zzm_reclaim_service_t;

typedef const void *(*zzm_get_service_fn)(uint32_t id, uint32_t min_version);

typedef struct zzm_services {
    uint32_t abi_version;
    uint32_t size;
    zzm_get_service_fn get_service;
    int (*register_ops)(uint32_t module_id, const void *ops, uint32_t ops_size);
    int (*register_service)(uint32_t module_id, uint32_t id, uint32_t version,
                            const void *interface, uint32_t interface_size);
} zzm_services_t;

typedef int (*zzm_init_fn)(const zzm_services_t *services);
typedef void (*zzm_fini_fn)(void);

const zzm_services_t *zzm_runtime_services(void);
int zzm_runtime_register_ops(uint32_t module_id, const void *ops,
                             uint32_t ops_size);
void zzm_runtime_module_fault(uint32_t module_id, const char *reason);
uint32_t zzm_runtime_release_module(uint32_t module_id);
int zzm_runtime_quota_available(uint32_t memory_class, uint32_t quota);

/* libzzm_rt public module-side binding and context API. */
int zzm_rt_bind(const zzm_services_t *services, uint32_t module_id,
                uint32_t memory_class, uint32_t quota);
void zzm_rt_set_context(uint32_t owner, uint32_t memory_class, uint32_t quota);
void zzm_rt_get_context(zzm_allocation_context_t *out);
void zzm_rt_log(const char *message);
void zzm_rt_fault(const char *reason);
void zzm_rt_dcache_clean(uint32_t address, uint32_t length);

/* Codec plug-in contracts. Payload addresses are card addresses; formats are
 * intentionally fixed to 8-bit 4:2:0 video and interleaved signed PCM. */
typedef struct zzm_packet {
    const uint8_t *data;
    uint32_t bytes;
    uint64_t pts90k;
    uint32_t flags;
} zzm_packet_t;
typedef struct zzm_video_frame {
    uint8_t *plane[3];
    uint32_t stride[3];
    uint32_t width;
    uint32_t height;
    uint64_t pts90k;
} zzm_video_frame_t;
typedef struct zzm_video_decoder_service {
    uint32_t version, size;
    int (*decode)(void *decoder, const zzm_packet_t *, zzm_video_frame_t *);
    int (*flush)(void *decoder, zzm_video_frame_t *);
    void (*seek_reset)(void *decoder);
} zzm_video_decoder_service_t;
typedef struct zzm_audio_decoder_service {
    uint32_t version, size, sample_rate, channels;
    int (*decode)(void *decoder, const zzm_packet_t *, int16_t *, uint32_t *);
    int (*flush)(void *decoder, int16_t *, uint32_t *);
    void (*seek_reset)(void *decoder);
} zzm_audio_decoder_service_t;
typedef struct zzm_demuxer_service {
    uint32_t version, size;
    int (*probe)(const uint8_t *, uint32_t);
    int (*open)(void *demuxer, const void *, uint32_t);
    int (*read_packet)(void *demuxer, zzm_packet_t *);
    int (*seek)(void *demuxer, uint64_t pts90k);
} zzm_demuxer_service_t;
#endif
