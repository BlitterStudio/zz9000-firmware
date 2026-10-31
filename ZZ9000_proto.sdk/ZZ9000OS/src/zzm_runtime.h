#ifndef ZZM_RUNTIME_H
#define ZZM_RUNTIME_H

#include <stdint.h>
#include "zzm_format.h"

/*
 * Service IDs are permanent.  V2 keeps its original values; v3 appends the
 * MiniGL resource services instead of repurposing a v2 contract.
 */
enum zzm_service_id {
    ZZM_SERVICE_CORE = 1U,
    ZZM_SERVICE_MEMORY = 2U,
    ZZM_SERVICE_CACHE = 3U,
    ZZM_SERVICE_RECLAIM = 4U,
    ZZM_SERVICE_JOBS = 5U,
    ZZM_SERVICE_HOST_WINDOW = 6U,
    ZZM_SERVICE_PALETTE = 7U,
    ZZM_SERVICE_APP_GL_HOST = 8U,
    ZZM_SERVICE_PROFILE = 9U,
};

#define ZZM_SERVICE_VERSION(major, minor) \
    ((((uint32_t)(major) & 0xffffU) << 16) | ((uint32_t)(minor) & 0xffffU))
#define ZZM_SERVICE_VERSION_MAJOR(version) ((uint32_t)(version) >> 16)
#define ZZM_SERVICE_VERSION_MINOR(version) ((uint32_t)(version) & 0xffffU)
#define ZZM_SERVICE_VERSION_COMPATIBLE(provided, required) \
    (ZZM_SERVICE_VERSION_MAJOR(provided) == ZZM_SERVICE_VERSION_MAJOR(required) && \
     ZZM_SERVICE_VERSION_MINOR(provided) >= ZZM_SERVICE_VERSION_MINOR(required))

#define ZZM_SERVICE_CORE_V1 1U
#define ZZM_SERVICE_MEMORY_V1 1U
#define ZZM_SERVICE_CACHE_V1 1U
#define ZZM_SERVICE_RECLAIM_V1 1U
#define ZZM_SERVICE_JOBS_V1 1U
#define ZZM_SERVICE_CORE_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_MEMORY_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_CACHE_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_RECLAIM_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_HOST_WINDOW_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_PALETTE_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_APP_GL_HOST_V3 ZZM_SERVICE_VERSION(3U, 0U)
#define ZZM_SERVICE_PROFILE_V3 ZZM_SERVICE_VERSION(3U, 0U)

enum zzm_memory_class {
    ZZM_MEMORY_CLASS_MEDIA = 1U,
    ZZM_MEMORY_CLASS_MEDIA_INPUT = 2U,
    ZZM_MEMORY_CLASS_IMAGE = 3U,
    ZZM_MEMORY_CLASS_GENERAL = 4U,
    ZZM_MEMORY_CLASS_MINIGL_MODULE = 5U,
    ZZM_MEMORY_CLASS_MINIGL_TRANSPORT = 6U,
    ZZM_MEMORY_CLASS_MINIGL_CONTEXT = 7U,
    ZZM_MEMORY_CLASS_MINIGL_APP_CONTEXT = 8U,
    ZZM_MEMORY_CLASS_MINIGL_RIX = 9U,
};

enum zzm_owner_class {
    ZZM_OWNER_CLASS_MINIGL_MODULE = 1U << 0,
    ZZM_OWNER_CLASS_MINIGL_TRANSPORT = 1U << 1,
    ZZM_OWNER_CLASS_MINIGL_CONTEXT = 1U << 2,
    ZZM_OWNER_CLASS_MINIGL_APP_CONTEXT = 1U << 3,
    ZZM_OWNER_CLASS_MINIGL_RIX = 1U << 4,
};

enum zzm_memory_attributes {
    ZZM_MEMORY_ATTR_NONE = 0U,
    ZZM_MEMORY_ATTR_HOST_VISIBLE = 1U << 0,
    ZZM_MEMORY_ATTR_NONCACHEABLE = 1U << 1,
};

/* Resident module allocations use a namespace disjoint from session owners.
 * The card pool keeps only 24 owner bits below its memory-class byte. */
#define ZZM_MODULE_OWNER_TAG 0x00800000U
#define ZZM_MODULE_OWNER(id) \
    (ZZM_MODULE_OWNER_TAG | ((uint32_t)(id) & (ZZM_MODULE_OWNER_TAG - 1U)))


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
    /* V3: the firmware suppresses repeated module diagnostics. */
    void (*log_limited)(uint32_t module_id, uint32_t level,
                        const char *message);
} zzm_core_service_t;

typedef struct zzm_memory_service {
    uint32_t version;
    uint32_t size;
    uint32_t (*alloc)(uint32_t owner, uint32_t memory_class, uint32_t quota,
                      uint32_t bytes, uint32_t alignment);
    int (*free)(uint32_t address, uint32_t owner);
    uint32_t (*realloc)(uint32_t address, uint32_t owner, uint32_t bytes,
                        uint32_t alignment);
    /* V3: attributes and typed owner classes are explicit. */
    uint32_t (*alloc_ex)(uint32_t owner, uint32_t owner_class,
                         uint32_t quota, uint32_t bytes, uint32_t alignment,
                         uint32_t attributes);
    uint32_t (*realloc_ex)(uint32_t address, uint32_t owner,
                           uint32_t owner_class, uint32_t quota,
                           uint32_t bytes, uint32_t alignment,
                           uint32_t attributes);
    uint32_t (*quota_used)(uint32_t owner, uint32_t owner_class);
} zzm_memory_service_t;

typedef struct zzm_cache_service {
    uint32_t version;
    uint32_t size;
    void (*dcache_clean)(uint32_t address, uint32_t length);
    void (*icache_invalidate)(uint32_t address, uint32_t length);
    /* V3 operations reject a range not owned by the supplied owner. */
    int (*dcache_clean_owned)(uint32_t owner, uint32_t address, uint32_t length);
    int (*dcache_invalidate_owned)(uint32_t owner, uint32_t address,
                                   uint32_t length);
    int (*instruction_sync_owned)(uint32_t owner, uint32_t address,
                                  uint32_t length);
} zzm_cache_service_t;

typedef struct zzm_reclaim_service {
    uint32_t version;
    uint32_t size;
    uint32_t (*release_owner)(uint32_t owner);
    int (*create_owner)(uint32_t module_id, uint32_t owner_class,
                        uint32_t parent_owner, uint32_t quota,
                        uint32_t *owner_out);
    uint32_t (*close_owner)(uint32_t owner);
    uint32_t (*force_reclaim)(uint32_t owner);
    uint32_t (*owner_refcount)(uint32_t owner);
} zzm_reclaim_service_t;

typedef struct zzm_host_window {
    uint32_t arm_address;
    uint32_t board_offset;
    uint32_t bytes;
    uint32_t attributes;
    uint32_t generation;
} zzm_host_window_t;

typedef struct zzm_host_window_service {
    uint32_t version;
    uint32_t size;
    int (*alloc)(uint32_t owner, uint32_t bytes, uint32_t alignment,
                 zzm_host_window_t *out);
    int (*free)(uint32_t owner, uint32_t arm_address, uint32_t generation);
} zzm_host_window_service_t;

typedef struct zzm_palette_service {
    uint32_t version;
    uint32_t size;
    uint32_t (*generation)(void);
    int (*snapshot)(uint32_t entries[256], uint32_t *generation);
    uint32_t (*lookup)(uint32_t index);
} zzm_palette_service_t;

typedef struct zzm_app_gl_host_service {
    uint32_t version;
    uint32_t size;
    void (*api_enter)(void);
    void (*api_leave)(void);
    int (*bound_display)(uint32_t *width, uint32_t *height, uint32_t *pitch,
                         uint32_t *format, uint32_t *card_front,
                         uint32_t *card_back);
    int (*flip_bound)(uint32_t generation);
    uint32_t (*generation)(void);
} zzm_app_gl_host_service_t;

typedef struct zzm_profile_service {
    uint32_t version;
    uint32_t size;
    int (*register_counter)(uint32_t module_id, const char *name,
                            uint32_t *counter_out);
    uint64_t (*sample_time_us)(void);
    int (*sample_counter)(uint32_t counter, uint64_t *value_out);
    int (*snapshot)(uint32_t module_id, void *out, uint32_t *bytes_inout);
} zzm_profile_service_t;

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
