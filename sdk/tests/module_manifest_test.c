/*
 * Tests for the ARM-runtime module manifest ABI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/module.h"
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

static void expect_str(const char *name, const char *actual,
                       const char *expected)
{
  if (strcmp(actual, expected) != 0) {
    printf("%s: got %s expected %s\n", name, actual, expected);
    failures++;
  }
}

static void test_layout_is_fixed(void)
{
  expect_u32("module_magic", ZZ9K_MODULE_MAGIC, 0x5a394d4fUL);
  expect_u32("module_manifest_size", sizeof(ZZ9KModuleManifest),
             ZZ9K_MODULE_MANIFEST_SIZE);
  expect_u32("module_service_size", sizeof(ZZ9KModuleServiceDesc),
             ZZ9K_MODULE_SERVICE_DESC_SIZE);
  expect_u32("module_name_size", ZZ9K_MODULE_NAME_SIZE, 32U);
  expect_u32("module_service_name_size", ZZ9K_MODULE_SERVICE_NAME_SIZE, 20U);
}

static void test_init_helpers_fill_expected_fields(void)
{
  ZZ9KModuleManifest manifest;
  ZZ9KModuleServiceDesc service;

  memset(&manifest, 0xff, sizeof(manifest));
  memset(&service, 0xff, sizeof(service));

  zz9k_module_manifest_init(&manifest, "image-codecs", 0x00010002UL,
                            ZZ9K_MODULE_FLAG_FIRMWARE_BUILTIN, 1U);
  zz9k_module_service_init(&service, ZZ9K_SERVICE_IMAGE, 0x00020003UL,
                           ZZ9K_CAP_IMAGE_SCALE,
                           ZZ9K_SERVICE_FLAG_ASYNC |
                             ZZ9K_SERVICE_FLAG_ZERO_COPY,
                           ZZ9K_OP_SCALE_IMAGE, 4U,
                           ZZ9K_MAILBOX_ENTRY_SIZE - 16U, "image");

  expect_u32("manifest_magic", manifest.magic, ZZ9K_MODULE_MAGIC);
  expect_u32("manifest_abi_major", manifest.abi_major,
             ZZ9K_ABI_VERSION_MAJOR);
  expect_u32("manifest_abi_minor", manifest.abi_minor,
             ZZ9K_ABI_VERSION_MINOR);
  expect_u32("manifest_size", manifest.manifest_size,
             ZZ9K_MODULE_MANIFEST_SIZE);
  expect_u32("manifest_service_desc_size", manifest.service_desc_size,
             ZZ9K_MODULE_SERVICE_DESC_SIZE);
  expect_u32("manifest_module_version", manifest.module_version,
             0x00010002UL);
  expect_u32("manifest_service_count", manifest.service_count, 1U);
  expect_str("manifest_name", manifest.name, "image-codecs");

  expect_u32("service_id", service.service_id, ZZ9K_SERVICE_IMAGE);
  expect_u32("service_version", service.version, 0x00020003UL);
  expect_u32("service_caps", service.capability_bits, ZZ9K_CAP_IMAGE_SCALE);
  expect_u32("service_flags", service.flags,
             ZZ9K_SERVICE_FLAG_ASYNC | ZZ9K_SERVICE_FLAG_ZERO_COPY);
  expect_u32("service_opcode_base", service.opcode_base, ZZ9K_OP_SCALE_IMAGE);
  expect_u32("service_opcode_count", service.opcode_count, 4U);
  expect_u32("service_max_inline", service.max_inline_payload,
             ZZ9K_MAILBOX_ENTRY_SIZE - 16U);
  expect_str("service_name", service.name, "image");
}

static void test_validation_rejects_bad_manifests(void)
{
  ZZ9KModuleManifest manifest;
  ZZ9KModuleServiceDesc service;

  zz9k_module_manifest_init(&manifest, "crypto", 0x00010000UL, 0, 1U);
  zz9k_module_service_init(&service, ZZ9K_SERVICE_CRYPTO, 0x00010000UL,
                           ZZ9K_CAP_CRYPTO, ZZ9K_SERVICE_FLAG_ASYNC,
                           ZZ9K_SERVICE_CRYPTO, 8U, 48U, "crypto");

  expect_status("valid_manifest",
                zz9k_module_validate_manifest(&manifest, &service, 1U),
                ZZ9K_STATUS_OK);

  manifest.magic = 0;
  expect_status("bad_magic",
                zz9k_module_validate_manifest(&manifest, &service, 1U),
                ZZ9K_STATUS_BAD_REQUEST);
  manifest.magic = ZZ9K_MODULE_MAGIC;

  manifest.abi_major = ZZ9K_ABI_VERSION_MAJOR + 1U;
  expect_status("bad_abi",
                zz9k_module_validate_manifest(&manifest, &service, 1U),
                ZZ9K_STATUS_UNSUPPORTED);
  manifest.abi_major = ZZ9K_ABI_VERSION_MAJOR;

  manifest.service_count = ZZ9K_MODULE_MAX_SERVICES + 1U;
  expect_status("too_many_services",
                zz9k_module_validate_manifest(&manifest, &service, 1U),
                ZZ9K_STATUS_BAD_REQUEST);
  manifest.service_count = 1U;

  service.max_inline_payload = 49U;
  expect_status("inline_too_large",
                zz9k_module_validate_manifest(&manifest, &service, 1U),
                ZZ9K_STATUS_BAD_REQUEST);
}

int main(void)
{
  test_layout_is_fixed();
  test_init_helpers_fill_expected_fields();
  test_validation_rejects_bad_manifests();

  return failures ? 1 : 0;
}
