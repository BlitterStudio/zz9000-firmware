/*
 * Tests for the module-backed service registry.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/service_registry.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_u32(const char *name, unsigned long actual,
                       unsigned long expected)
{
  if (actual != expected) {
    printf("%s: got %lu expected %lu\n", name, actual, expected);
    failures++;
  }
}

static void expect_status(const char *name, int actual, int expected)
{
  if (actual != expected) {
    printf("%s: got %d expected %d\n", name, actual, expected);
    failures++;
  }
}

static void expect_ptr(const char *name, const void *actual,
                       const void *expected)
{
  if (actual != expected) {
    printf("%s: got %p expected %p\n", name, actual, expected);
    failures++;
  }
}

static void init_test_module(ZZ9KModuleManifest *manifest,
                             ZZ9KModuleServiceDesc *services)
{
  zz9k_module_manifest_init(manifest, "core-runtime", 0x00010000UL,
                            ZZ9K_MODULE_FLAG_FIRMWARE_BUILTIN, 2U);
  zz9k_module_service_init(&services[0], ZZ9K_SERVICE_IMAGE, 0x00020000UL,
                           ZZ9K_CAP_IMAGE_SCALE,
                           ZZ9K_SERVICE_FLAG_ASYNC |
                             ZZ9K_SERVICE_FLAG_ZERO_COPY,
                           ZZ9K_OP_SCALE_IMAGE, 4U, 48U, "image");
  zz9k_module_service_init(&services[1], ZZ9K_SERVICE_CRYPTO, 0x00010000UL,
                           ZZ9K_CAP_CRYPTO, ZZ9K_SERVICE_FLAG_ASYNC,
                           ZZ9K_SERVICE_CRYPTO, 8U, 48U, "crypto");
}

static void test_add_and_query_services(void)
{
  ZZ9KServiceRegistry registry;
  ZZ9KModuleManifest manifest;
  ZZ9KModuleServiceDesc services[2];
  const ZZ9KModuleServiceDesc *service;
  const ZZ9KModuleManifest *owner;
  ZZ9KServiceInfoPayload payload;

  init_test_module(&manifest, services);
  zz9k_service_registry_init(&registry);

  expect_status("add_manifest",
                zz9k_service_registry_add_manifest(&registry, &manifest,
                                                   services, 2U),
                ZZ9K_STATUS_OK);
  expect_u32("registry_count", zz9k_service_registry_count(&registry), 2U);

  service = zz9k_service_registry_find_service(&registry, ZZ9K_SERVICE_IMAGE,
                                               &owner);
  expect_ptr("service_ptr", service, &services[0]);
  expect_ptr("owner_ptr", owner, &manifest);

  service = zz9k_service_registry_find_opcode(&registry, ZZ9K_OP_DECODE_PNG,
                                              0);
  expect_ptr("opcode_ptr", service, &services[0]);

  memset(&payload, 0xff, sizeof(payload));
  expect_status("service_payload",
                zz9k_service_registry_make_info_payload(&services[0],
                                                        &payload),
                ZZ9K_STATUS_OK);
  expect_u32("payload_service_id", zz9k_get_be32(payload.service_id),
             ZZ9K_SERVICE_IMAGE);
  expect_u32("payload_version", zz9k_get_be32(payload.version),
             0x00020000UL);
  expect_u32("payload_caps", zz9k_get_be32(payload.capability_bits),
             ZZ9K_CAP_IMAGE_SCALE);
  expect_u32("payload_opcode_base", zz9k_get_be32(payload.opcode_base),
             ZZ9K_OP_SCALE_IMAGE);
  if (memcmp(payload.name, "image", 6U) != 0) {
    printf("payload_name: bad copy\n");
    failures++;
  }
}

static void test_rejects_duplicate_service_ids(void)
{
  ZZ9KServiceRegistry registry;
  ZZ9KModuleManifest first_manifest;
  ZZ9KModuleManifest second_manifest;
  ZZ9KModuleServiceDesc first_service;
  ZZ9KModuleServiceDesc second_service;

  zz9k_module_manifest_init(&first_manifest, "image-a", 0, 0, 1U);
  zz9k_module_service_init(&first_service, ZZ9K_SERVICE_IMAGE, 0,
                           ZZ9K_CAP_IMAGE_SCALE, 0,
                           ZZ9K_SERVICE_IMAGE, 2U, 48U, "image-a");
  zz9k_module_manifest_init(&second_manifest, "image-b", 0, 0, 1U);
  zz9k_module_service_init(&second_service, ZZ9K_SERVICE_IMAGE, 0,
                           ZZ9K_CAP_IMAGE_SCALE, 0,
                           ZZ9K_OP_DECODE_GIF, 1U, 48U, "image-b");

  zz9k_service_registry_init(&registry);
  expect_status("first_add",
                zz9k_service_registry_add_manifest(&registry,
                                                   &first_manifest,
                                                   &first_service, 1U),
                ZZ9K_STATUS_OK);
  expect_status("duplicate_add",
                zz9k_service_registry_add_manifest(&registry,
                                                   &second_manifest,
                                                   &second_service, 1U),
                ZZ9K_STATUS_BAD_REQUEST);
}

static void test_rejects_overlapping_opcode_ranges(void)
{
  ZZ9KServiceRegistry registry;
  ZZ9KModuleManifest first_manifest;
  ZZ9KModuleManifest second_manifest;
  ZZ9KModuleServiceDesc first_service;
  ZZ9KModuleServiceDesc second_service;

  zz9k_module_manifest_init(&first_manifest, "vendor-a", 0, 0, 1U);
  zz9k_module_service_init(&first_service, ZZ9K_SERVICE_VENDOR, 0, 0, 0,
                           ZZ9K_SERVICE_VENDOR, 4U, 48U, "vendor-a");
  zz9k_module_manifest_init(&second_manifest, "vendor-b", 0, 0, 1U);
  zz9k_module_service_init(&second_service, ZZ9K_SERVICE_VENDOR + 0x20U, 0,
                           0, 0, ZZ9K_SERVICE_VENDOR + 2U, 4U, 48U,
                           "vendor-b");

  zz9k_service_registry_init(&registry);
  expect_status("first_vendor_add",
                zz9k_service_registry_add_manifest(&registry,
                                                   &first_manifest,
                                                   &first_service, 1U),
                ZZ9K_STATUS_OK);
  expect_status("overlap_add",
                zz9k_service_registry_add_manifest(&registry,
                                                   &second_manifest,
                                                   &second_service, 1U),
                ZZ9K_STATUS_BAD_REQUEST);
}

int main(void)
{
  test_add_and_query_services();
  test_rejects_duplicate_service_ids();
  test_rejects_overlapping_opcode_ranges();

  return failures ? 1 : 0;
}
