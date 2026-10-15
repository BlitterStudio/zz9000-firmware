# ZZ9000 SDK modules

`include/zz9k/module.h` defines a fixed-size manifest format for describing
ZZ9000 SDK services. `include/zz9k/service_registry.h` provides an in-memory
registry for those manifests. The current firmware does not use either header
for service discovery; it uses `sdk_service_catalog.h` instead.

## Manifest format

A module manifest describes the module and its services. Keep the manifest and
its `ZZ9KModuleServiceDesc` array valid for as long as a
`ZZ9KServiceRegistry` contains them: the registry stores their pointers.

| Item | Definition |
| --- | --- |
| Module record | `ZZ9KModuleManifest`, 64 bytes |
| Service record | `ZZ9KModuleServiceDesc`, 64 bytes |
| Module name | 32-byte NUL-terminated field |
| Service name | 20-byte NUL-terminated field |
| Services in one manifest | At most `ZZ9K_MODULE_MAX_SERVICES` (`16`) |
| Services in one registry | At most `ZZ9K_SERVICE_REGISTRY_MAX_SERVICES` (`64`) |

The records contain only fixed-width integer fields and fixed-size character
arrays. `zz9k_module_manifest_init()` and `zz9k_module_service_init()` clear
the records, set their ABI and size fields where applicable, and NUL-terminate
the names.

A manifest records its module version, flags, and service count. Each service
record supplies a service ID, version, capability bits, flags, opcode range,
maximum inline payload, and name.

## Creating and registering a manifest

Include `zz9k/module.h`, define storage for the manifest and service records,
and initialize both. Add `zz9k/service_registry.h` when the manifest must be
placed in a registry.

```c
#include "zz9k/module.h"

static ZZ9KModuleManifest image_manifest;
static ZZ9KModuleServiceDesc image_services[1];

void image_module_init_manifest(void)
{
  zz9k_module_manifest_init(&image_manifest, "image", 0x00010000UL,
                            ZZ9K_MODULE_FLAG_FIRMWARE_BUILTIN, 1U);
  zz9k_module_service_init(&image_services[0], ZZ9K_SERVICE_IMAGE,
                           0x00010000UL, ZZ9K_CAP_IMAGE_SCALE,
                           ZZ9K_SERVICE_FLAG_ASYNC |
                             ZZ9K_SERVICE_FLAG_ZERO_COPY,
                           ZZ9K_OP_SCALE_IMAGE, 1U, 48U, "image");
}
```

Validate the manifest before registering it. `service_capacity` is the number
of records in the supplied array.

```c
if (zz9k_module_validate_manifest(&image_manifest, image_services, 1U) !=
    ZZ9K_STATUS_OK) {
  /* reject the module */
}
```

Initialize a `ZZ9KServiceRegistry`, then register the same manifest and array
with `zz9k_service_registry_add_manifest()`. The function validates the
manifest before adding it.

```c
#include "zz9k/service_registry.h"

ZZ9KServiceRegistry registry;

zz9k_service_registry_init(&registry);
if (zz9k_service_registry_add_manifest(&registry, &image_manifest,
                                       image_services, 1U) != ZZ9K_STATUS_OK) {
  /* reject the module */
}
```

Use `zz9k_service_registry_find_service()` to find a descriptor by service ID,
or `zz9k_service_registry_find_opcode()` to find one by opcode. The latter
also requires every bit in its `required_flags` argument to be set in the
service flags. `zz9k_service_registry_make_info_payload()` converts one
descriptor to the big-endian `ZZ9KServiceInfoPayload` format used by
`QUERY_SERVICE`.

## Validation and registry rules

`zz9k_module_validate_manifest()` rejects a manifest when any of these checks
fail:

- `magic`, `manifest_size`, or `service_desc_size` does not match the defined
  constants.
- `abi_major` differs from `ZZ9K_ABI_VERSION_MAJOR`.
- The service count exceeds `ZZ9K_MODULE_MAX_SERVICES` or `service_capacity`.
- A non-empty manifest has no service array.
- The module or a service name does not have a final NUL byte.
- A service has an `opcode_count` of zero.
- A service's `max_inline_payload` exceeds `ZZ9K_MAILBOX_ENTRY_SIZE - 16U`
  (`48` bytes with the current 64-byte mailbox entry).

The registry also rejects duplicate service IDs and overlapping opcode ranges,
including conflicts between services in the same manifest. It returns
`ZZ9K_STATUS_NO_MEMORY` when adding the manifest would exceed its 64-service
capacity.

Module flags include `ZZ9K_MODULE_FLAG_FIRMWARE_BUILTIN`,
`ZZ9K_MODULE_FLAG_LOADABLE`, and `ZZ9K_MODULE_FLAG_TRUSTED`. The format stores
these flags; the current registry does not give them special handling. For
vendor service ID and opcode-range conventions, see
[`zz9k-vendor-services.md`](zz9k-vendor-services.md).

## Current status

The manifest and registry are SDK header-only helpers with unit tests. The
firmware mailbox dispatcher currently answers `QUERY_SERVICE` from the static
catalogue in `ZZ9000_proto.sdk/ZZ9000OS/src/sdk_service_catalog.h`, rather
than from `ZZ9KModuleManifest` records. The source tree contains no integration
of the module registry with that dispatcher, and no implementation of module
loading, dynamic relocation, or runtime unloading.
