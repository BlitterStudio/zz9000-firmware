/* Fixed on-card format for ZZ9000 firmware modules. */
#ifndef ZZM_FORMAT_H
#define ZZM_FORMAT_H

#include <stdint.h>

#define ZZM_MAGIC "ZZM2"
#define ZZM_FORMAT_VERSION 2U
#define ZZM_HEADER_SIZE 256U
#define ZZM_MODULE_ABI_VERSION 2U
#define ZZM_SHA256_SIZE 32U
#define ZZM_NAME_SIZE 16U
#define ZZM_MAX_SERVICE_REQUIREMENTS 4U
#define ZZM_MAX_SERVICE_PROVISIONS 4U

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
};

typedef struct __attribute__((packed)) zzm_service_requirement {
    uint32_t service_id;
    uint32_t min_version;
} zzm_service_requirement_t;

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
    uint8_t reserved[88];
} zzm_header_t;

typedef char zzm_header_size_must_be_256[(sizeof(zzm_header_t) == ZZM_HEADER_SIZE) ? 1 : -1];

#endif
