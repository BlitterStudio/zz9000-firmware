# ZZ9000 SDK Modules

SDK v2 services are firmware-built today, but they use module-style metadata.
That keeps the ABI stable now and leaves a clear path to loadable modules
later.

The shared module manifest lives in `include/zz9k/module.h`. Current packages
do not load modules dynamically; this document describes the metadata shape
used by built-in services now and by a future loader later.

## Manifest Shape

Each module has one `ZZ9KModuleManifest` and an adjacent array of
`ZZ9KModuleServiceDesc` records.

The manifest is deliberately pointer-free:

- It can be copied, inspected, or checksummed without relocation.
- It has fixed 64-byte records.
- It uses fixed-width integers only.
- Names are fixed-size, NUL-terminated fields.

The current runtime uses these manifests for statically linked services. A
later loader can find the manifest symbol in an ELF module without changing the
service registry layout.

## Example

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

Before registering a manifest, validate it:

```c
if (zz9k_module_validate_manifest(&image_manifest, image_services, 1U) !=
    ZZ9K_STATUS_OK) {
  /* reject the module */
}
```

## Current Scope

This is metadata only. It does not add an ELF loader, dynamic relocation,
runtime unloading, or a public plugin ABI yet. Handler function tables should
remain runtime-private; the manifest only describes service identity,
capabilities, opcode ranges, and payload limits.

## Service Registry

`include/zz9k/service_registry.h` adds a small manifest-backed registry for the
runtime dispatcher:

- Validate and register a module manifest.
- Reject duplicate service IDs.
- Reject overlapping opcode ranges.
- Find a service by service ID.
- Find a service by opcode.
- Build the big-endian `ZZ9KServiceInfoPayload` used by `QUERY_SERVICE`.

The registry stores pointers to static manifests and descriptors. It is runtime
state, not mailbox ABI, and should stay on the ARM side. The payload builder is
the boundary helper that converts registry metadata into the wire format seen by
Amiga callers.
