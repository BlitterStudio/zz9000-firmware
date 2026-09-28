/*
 * ZZ9000 SDK v2 module-backed service registry helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_SERVICE_REGISTRY_H
#define ZZ9K_SERVICE_REGISTRY_H

#include "zz9k/module.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZZ9K_SERVICE_REGISTRY_MAX_SERVICES 64U

typedef struct ZZ9KServiceRegistryEntry {
  const ZZ9KModuleManifest *manifest;
  const ZZ9KModuleServiceDesc *service;
} ZZ9KServiceRegistryEntry;

typedef struct ZZ9KServiceRegistry {
  ZZ9KServiceRegistryEntry entries[ZZ9K_SERVICE_REGISTRY_MAX_SERVICES];
  uint32_t count;
} ZZ9KServiceRegistry;

static inline void zz9k_service_registry_init(ZZ9KServiceRegistry *registry)
{
  if (registry) {
    memset(registry, 0, sizeof(*registry));
  }
}

static inline uint32_t zz9k_service_registry_count(
  const ZZ9KServiceRegistry *registry)
{
  return registry ? registry->count : 0U;
}

static inline int zz9k_service_registry_ranges_overlap(uint32_t a_base,
                                                       uint32_t a_count,
                                                       uint32_t b_base,
                                                       uint32_t b_count)
{
  uint64_t a_end;
  uint64_t b_end;

  if (a_count == 0U || b_count == 0U) {
    return 0;
  }

  a_end = (uint64_t)a_base + (uint64_t)a_count - 1U;
  b_end = (uint64_t)b_base + (uint64_t)b_count - 1U;
  return (uint64_t)a_base <= b_end && (uint64_t)b_base <= a_end;
}

static inline int zz9k_service_registry_services_conflict(
  const ZZ9KModuleServiceDesc *a,
  const ZZ9KModuleServiceDesc *b)
{
  if (!a || !b) {
    return 0;
  }

  if (a->service_id == b->service_id) {
    return 1;
  }

  return zz9k_service_registry_ranges_overlap(a->opcode_base,
                                              a->opcode_count,
                                              b->opcode_base,
                                              b->opcode_count);
}

static inline int zz9k_service_registry_add_manifest(
  ZZ9KServiceRegistry *registry,
  const ZZ9KModuleManifest *manifest,
  const ZZ9KModuleServiceDesc *services,
  uint32_t service_capacity)
{
  uint32_t i;
  uint32_t j;
  int status;

  if (!registry) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  status = zz9k_module_validate_manifest(manifest, services,
                                         service_capacity);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (registry->count + manifest->service_count >
      ZZ9K_SERVICE_REGISTRY_MAX_SERVICES) {
    return ZZ9K_STATUS_NO_MEMORY;
  }

  for (i = 0; i < manifest->service_count; i++) {
    for (j = 0; j < registry->count; j++) {
      if (zz9k_service_registry_services_conflict(
            services + i, registry->entries[j].service)) {
        return ZZ9K_STATUS_BAD_REQUEST;
      }
    }
    for (j = i + 1U; j < manifest->service_count; j++) {
      if (zz9k_service_registry_services_conflict(services + i,
                                                  services + j)) {
        return ZZ9K_STATUS_BAD_REQUEST;
      }
    }
  }

  for (i = 0; i < manifest->service_count; i++) {
    registry->entries[registry->count].manifest = manifest;
    registry->entries[registry->count].service = services + i;
    registry->count++;
  }

  return ZZ9K_STATUS_OK;
}

static inline const ZZ9KModuleServiceDesc *zz9k_service_registry_find_service(
  const ZZ9KServiceRegistry *registry,
  uint32_t service_id,
  const ZZ9KModuleManifest **owner)
{
  uint32_t i;

  if (owner) {
    *owner = 0;
  }
  if (!registry) {
    return 0;
  }

  for (i = 0; i < registry->count; i++) {
    if (registry->entries[i].service &&
        registry->entries[i].service->service_id == service_id) {
      if (owner) {
        *owner = registry->entries[i].manifest;
      }
      return registry->entries[i].service;
    }
  }

  return 0;
}

static inline const ZZ9KModuleServiceDesc *zz9k_service_registry_find_opcode(
  const ZZ9KServiceRegistry *registry,
  uint32_t opcode,
  uint32_t required_flags)
{
  uint32_t i;

  if (!registry) {
    return 0;
  }

  for (i = 0; i < registry->count; i++) {
    const ZZ9KModuleServiceDesc *service = registry->entries[i].service;
    uint64_t end;
    if (!service) {
      continue;
    }
    end = (uint64_t)service->opcode_base +
          (uint64_t)service->opcode_count;
    if ((uint64_t)opcode >= service->opcode_base &&
        (uint64_t)opcode < end &&
        (service->flags & required_flags) == required_flags) {
      return service;
    }
  }

  return 0;
}

static inline int zz9k_service_registry_make_info_payload(
  const ZZ9KModuleServiceDesc *service,
  ZZ9KServiceInfoPayload *payload)
{
  if (!service || !payload) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  memset(payload, 0, sizeof(*payload));
  zz9k_put_be32(payload->service_id, service->service_id);
  zz9k_put_be32(payload->version, service->version);
  zz9k_put_be32(payload->capability_bits, service->capability_bits);
  zz9k_put_be32(payload->flags, service->flags);
  zz9k_put_be32(payload->opcode_base, service->opcode_base);
  zz9k_put_be32(payload->opcode_count, service->opcode_count);
  zz9k_put_be32(payload->max_inline_payload,
                service->max_inline_payload);
  memcpy(payload->name, service->name, ZZ9K_MODULE_SERVICE_NAME_SIZE);
  return ZZ9K_STATUS_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_SERVICE_REGISTRY_H */
