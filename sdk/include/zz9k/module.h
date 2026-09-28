/*
 * ZZ9000 SDK v2 ARM-runtime module manifest ABI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_MODULE_H
#define ZZ9K_MODULE_H

#include "zz9k/abi.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZZ9K_MODULE_MAGIC 0x5a394d4fUL /* "Z9MO" */
#define ZZ9K_MODULE_MANIFEST_SIZE 64U
#define ZZ9K_MODULE_SERVICE_DESC_SIZE 64U
#define ZZ9K_MODULE_NAME_SIZE 32U
#define ZZ9K_MODULE_SERVICE_NAME_SIZE 20U
#define ZZ9K_MODULE_MAX_SERVICES 16U

enum ZZ9KModuleFlags {
  ZZ9K_MODULE_FLAG_FIRMWARE_BUILTIN = 1U << 0,
  ZZ9K_MODULE_FLAG_LOADABLE = 1U << 1,
  ZZ9K_MODULE_FLAG_TRUSTED = 1U << 2
};

typedef struct ZZ9KModuleManifest {
  uint32_t magic;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t manifest_size;
  uint32_t service_desc_size;
  uint32_t module_version;
  uint32_t flags;
  uint32_t service_count;
  char name[ZZ9K_MODULE_NAME_SIZE];
  uint32_t reserved;
} ZZ9KModuleManifest;

typedef struct ZZ9KModuleServiceDesc {
  uint32_t service_id;
  uint32_t version;
  uint32_t capability_bits;
  uint32_t flags;
  uint32_t opcode_base;
  uint32_t opcode_count;
  uint32_t max_inline_payload;
  char name[ZZ9K_MODULE_SERVICE_NAME_SIZE];
  uint32_t reserved[4];
} ZZ9KModuleServiceDesc;

typedef char ZZ9KModuleManifest_must_be_64_bytes[
  (sizeof(ZZ9KModuleManifest) == ZZ9K_MODULE_MANIFEST_SIZE) ? 1 : -1
];
typedef char ZZ9KModuleServiceDesc_must_be_64_bytes[
  (sizeof(ZZ9KModuleServiceDesc) == ZZ9K_MODULE_SERVICE_DESC_SIZE) ? 1 : -1
];

static inline void zz9k_module_copy_name(char *dst, uint32_t capacity,
                                         const char *src)
{
  uint32_t i;

  if (!dst || capacity == 0U) {
    return;
  }

  if (!src) {
    src = "";
  }

  for (i = 0; i + 1U < capacity && src[i]; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
  for (i++; i < capacity; i++) {
    dst[i] = '\0';
  }
}

static inline void zz9k_module_manifest_init(ZZ9KModuleManifest *manifest,
                                             const char *name,
                                             uint32_t module_version,
                                             uint32_t flags,
                                             uint32_t service_count)
{
  if (!manifest) {
    return;
  }

  memset(manifest, 0, sizeof(*manifest));
  manifest->magic = ZZ9K_MODULE_MAGIC;
  manifest->abi_major = ZZ9K_ABI_VERSION_MAJOR;
  manifest->abi_minor = ZZ9K_ABI_VERSION_MINOR;
  manifest->manifest_size = ZZ9K_MODULE_MANIFEST_SIZE;
  manifest->service_desc_size = ZZ9K_MODULE_SERVICE_DESC_SIZE;
  manifest->module_version = module_version;
  manifest->flags = flags;
  manifest->service_count = service_count;
  zz9k_module_copy_name(manifest->name, ZZ9K_MODULE_NAME_SIZE, name);
}

static inline void zz9k_module_service_init(ZZ9KModuleServiceDesc *service,
                                            uint32_t service_id,
                                            uint32_t version,
                                            uint32_t capability_bits,
                                            uint32_t flags,
                                            uint32_t opcode_base,
                                            uint32_t opcode_count,
                                            uint32_t max_inline_payload,
                                            const char *name)
{
  if (!service) {
    return;
  }

  memset(service, 0, sizeof(*service));
  service->service_id = service_id;
  service->version = version;
  service->capability_bits = capability_bits;
  service->flags = flags;
  service->opcode_base = opcode_base;
  service->opcode_count = opcode_count;
  service->max_inline_payload = max_inline_payload;
  zz9k_module_copy_name(service->name, ZZ9K_MODULE_SERVICE_NAME_SIZE, name);
}

static inline int zz9k_module_name_is_terminated(const char *name,
                                                 uint32_t capacity)
{
  return name && capacity > 0U && name[capacity - 1U] == '\0';
}

static inline int zz9k_module_validate_manifest(
  const ZZ9KModuleManifest *manifest,
  const ZZ9KModuleServiceDesc *services,
  uint32_t service_capacity)
{
  uint32_t i;

  if (!manifest) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (manifest->magic != ZZ9K_MODULE_MAGIC ||
      manifest->manifest_size != ZZ9K_MODULE_MANIFEST_SIZE ||
      manifest->service_desc_size != ZZ9K_MODULE_SERVICE_DESC_SIZE) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (manifest->abi_major != ZZ9K_ABI_VERSION_MAJOR) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (manifest->service_count > ZZ9K_MODULE_MAX_SERVICES ||
      manifest->service_count > service_capacity) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (manifest->service_count > 0U && !services) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (!zz9k_module_name_is_terminated(manifest->name,
                                      ZZ9K_MODULE_NAME_SIZE)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  for (i = 0; i < manifest->service_count; i++) {
    const ZZ9KModuleServiceDesc *service = &services[i];
    if (!zz9k_module_name_is_terminated(service->name,
                                        ZZ9K_MODULE_SERVICE_NAME_SIZE)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    if (service->opcode_count == 0U ||
        service->max_inline_payload > ZZ9K_MAILBOX_ENTRY_SIZE - 16U) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
  }

  return ZZ9K_STATUS_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_MODULE_H */
