/* Fixed on-card format for ZZ9000 firmware modules. */
#ifndef ZZM_FORMAT_H
#define ZZM_FORMAT_H

#include <stdint.h>

#define ZZM_MAGIC "ZZM2"
#define ZZM_FORMAT_VERSION 2U
#define ZZM_HEADER_SIZE 256U
#define ZZM_MODULE_ABI_VERSION 2U
#define ZZM_V3_MAGIC "ZZM3"
#define ZZM_V3_FORMAT_VERSION 3U
#define ZZM_V3_HEADER_SIZE 512U
#define ZZM_V3_ABI_MAJOR 3U
#define ZZM_V3_ABI_MINOR 0U
#define ZZM_SHA256_SIZE 32U
#define ZZM_NAME_SIZE 16U
#define ZZM_MAX_SERVICE_REQUIREMENTS 4U
#define ZZM_MAX_SERVICE_PROVISIONS 4U
#define ZZM_V3_MAX_SERVICE_REQUIREMENTS 16U
#define ZZM_V3_MAX_SERVICE_PROVISIONS 4U

#define ZZM_LOAD_ON_DEMAND 0U
#define ZZM_LOAD_BOOT 1U
#define ZZM_DEFAULT_IDLE_UNLOAD_MS 30000U

enum zzm_module_id {
    ZZM_MODULE_MPEG2 = 1,
    ZZM_MODULE_WEBM = 2,
    ZZM_MODULE_WEBP = 3,
    ZZM_MODULE_MINIGL = 4,
    ZZM_MODULE_H264 = 5,
    ZZM_MODULE_H265 = 6,
    ZZM_MODULE_MP4 = 7,
    ZZM_MODULE_AAC = 8,
};

typedef struct __attribute__((packed)) zzm_service_requirement {
    uint32_t service_id;
    uint32_t min_version;
} zzm_service_requirement_t;

/* v3 image requirements are compared against firmware-owned capability
 * records; the module never probes optional hardware directly. */
#define ZZM_IMAGE_NEEDS_CORE1        (1U << 0)
#define ZZM_IMAGE_NEEDS_HOST_VISIBLE (1U << 1)
#define ZZM_IMAGE_NEEDS_VFP_NEON     (1U << 2)
#define ZZM_IMAGE_NEEDS_RASTERIX_EF  (1U << 3)
#define ZZM_IMAGE_NEEDS_PL330        (1U << 4)

#define ZZM_OWNER_CLASS_MINIGL_CONTEXT     (1U << 0)
#define ZZM_OWNER_CLASS_MINIGL_APP_CONTEXT (1U << 1)
#define ZZM_OWNER_CLASS_MINIGL_TRANSPORT   (1U << 2)
#define ZZM_OWNER_CLASS_MINIGL_RIX         (1U << 3)

enum zzm_hardware_requirement {
    ZZM_HARDWARE_CPU_ONLY = 0U,
    ZZM_HARDWARE_RASTERIX_EF_OPTIONAL = 1U,
    ZZM_HARDWARE_RASTERIX_EF_REQUIRED = 2U,
};

typedef struct __attribute__((packed)) zzm_v3_required_caps {
    uint32_t wire_version;
    uint32_t transport_descriptor_major;
    uint32_t rasterix_csr_abi_major;
    uint32_t rasterix_csr_abi_minor;
    uint32_t host_window_layout_major;
} zzm_v3_required_caps_t;

typedef struct __attribute__((packed)) zzm_v3_header {
    uint8_t magic[4];
    uint16_t header_size;
    uint16_t format_version;
    uint32_t module_id;
    uint32_t module_version;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t slot_load_address;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t init_entry;
    uint32_t fini_entry;
    char name[ZZM_NAME_SIZE];
    uint32_t load_policy;
    uint32_t idle_unload_ms;
    uint32_t resident_quota;
    uint32_t per_context_quota;
    uint32_t owner_class_mask;
    uint32_t image_flags;
    uint32_t hardware_requirement;
    uint32_t worker_ops_version;
    zzm_v3_required_caps_t required_caps;
    uint16_t required_count;
    uint16_t provided_count;
    zzm_service_requirement_t required[ZZM_V3_MAX_SERVICE_REQUIREMENTS];
    zzm_service_requirement_t provided[ZZM_V3_MAX_SERVICE_PROVISIONS];
    uint8_t image_sha256[ZZM_SHA256_SIZE];
    uint8_t reserved[208];
} zzm_v3_header_t;

typedef struct __attribute__((packed)) zzm_header {
    uint8_t magic[4];
    uint16_t header_size;
    uint16_t format_version;
    uint32_t module_id;
    uint32_t module_version;
    uint32_t module_abi_version;
    uint32_t slot_load_address;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t init_entry;
    uint32_t fini_entry;
    char name[ZZM_NAME_SIZE];
    uint32_t load_policy;
    uint32_t idle_unload_ms;
    uint32_t memory_class;
    uint32_t memory_quota;
    uint16_t required_count;
    uint16_t provided_count;
    zzm_service_requirement_t required[ZZM_MAX_SERVICE_REQUIREMENTS];
    zzm_service_requirement_t provided[ZZM_MAX_SERVICE_PROVISIONS];
    uint8_t image_sha256[ZZM_SHA256_SIZE];
    uint8_t reserved[84];
} zzm_header_t;

typedef char zzm_v3_header_size_must_be_512[
    (sizeof(zzm_v3_header_t) == ZZM_V3_HEADER_SIZE) ? 1 : -1];
typedef char zzm_header_size_must_be_256[(sizeof(zzm_header_t) == ZZM_HEADER_SIZE) ? 1 : -1];

#endif
