/* Fixed on-card format for ZZ9000 firmware modules. */
#ifndef ZZM_FORMAT_H
#define ZZM_FORMAT_H

#include <stdint.h>

#define ZZM_MAGIC "ZZM1"
#define ZZM_FORMAT_VERSION 1U
#define ZZM_HEADER_SIZE 128U
#define ZZM_MODULE_ABI_VERSION 1U
#define ZZM_SHA256_SIZE 32U
#define ZZM_NAME_SIZE 16U

enum zzm_module_id {
    ZZM_MODULE_MPEG2 = 1,
    ZZM_MODULE_WEBM = 2,
    ZZM_MODULE_WEBP = 3,
};

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
    uint8_t image_sha256[ZZM_SHA256_SIZE];
    uint8_t reserved[44];
} zzm_header_t;

typedef char zzm_header_size_must_be_128[(sizeof(zzm_header_t) == ZZM_HEADER_SIZE) ? 1 : -1];

#endif
