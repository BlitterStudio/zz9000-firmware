# Firmware modules: runtime v2

MPEG-2, WebM and WebP are fixed-address `.ZZM` modules loaded on demand from
`ZZ9K/` on the SD card. A boot scan reads their headers; an installed file
does not make its image resident. MiniGL uses the v3 module format and
core-1 host described in [minigl-module.md](minigl-module.md).

The slot manifest in `ZZ9000_proto.sdk/ZZ9000OS/src/zzm_manifest.c` is shared
by the packer and loader. It assigns module ids, names, addresses and sizes.
Modules use runtime service tables rather than importing firmware ELF symbols.
This runtime is separate from the public mailbox service catalog and the
[SDK manifest helpers](../sdk/docs/zz9k-modules.md).

## Image and link recipe

A v2 image has a 256-byte `ZZM2` header containing:

- format and module-ABI versions, module id/version, name and fixed address;
- image and BSS sizes, init/fini entries and image SHA-256;
- up to four required services and four provided services;
- load policy, idle-unload timeout, memory class and one memory quota.

`zzm_format.h` defines the layout. V2 has one quota, not separate resident
and per-session quota fields. The loader checks header shape, ABI, exact
file size, manifest slot, required services and available quota, then verifies
the image hash. A failure leaves the module unavailable.

The packer reads little-endian ELF32 input. It rejects `SHT_REL`, `SHT_RELA`
and `SHT_RELR` sections and allocated sections outside the assigned slot.
The linker script is `util/zzm_module.ld`. The shared link recipe in
`ZZ9000_proto.sdk/ZZ9000OS/modules/module.mk` links `libzzm_rt.a` with
`-nostdlib`, fixed `ZZM_SLOT` and `ZZM_SLOT_SIZE` symbols and the module's
static dependencies.

The packer interface for v2 is:

```text
zzm_pack MODULE_ID MODULE_VERSION INPUT.elf OUTPUT.ZZM
```

The MPEG-2, WebM and WebP make fragments under `modules/` supply the slot and
module arguments. Build them from the firmware source tree with:

```bash
make -C ZZ9000_proto.sdk/ZZ9000OS modules
```

`util/check_zzm_conformance.sh` checks a linked module's entry, slot,
relocations and runtime symbols. See [BUILD.md](../BUILD.md) for the toolchain
and dependencies.

## Allocation and C runtime

`modules/libzzm_rt/zzm_rt.c` provides allocation, aligned allocation and C++
`new`/`delete` through the firmware memory service. Each allocation carries
the active owner, memory class and quota. Use `zzm_rt_set_context` to select
the session owner before allocating session resources. A module must not
substitute an untracked private heap or call the card pool directly.

Use `zzm_rt_log` for diagnostics. File and process syscall stubs such as
`_open`, `_read`, `_close`, `_lseek` and `_kill` return `ENOSYS`; `_write`
discards output and returns its length. `_gettimeofday` and `clock` use the
core service's monotonic clock, not an Amiga wall clock.

Build C++ modules without exceptions or RTTI. The entry wrapper runs the
preinit and init arrays before module init; the exit wrapper calls module
fini and then the fini array in reverse. `__cxa_guard_*` is single-threaded.
`__cxa_atexit` does not register destructors, so code must not rely on it
for cleanup. Resource teardown belongs in the module's explicit fini and
session-close paths.

## Runtime services

`zzm_services_t` supplies `get_service`, `register_ops` and
`register_service`. Service tables start with `version` and `size`; check
the size before reading an appended callback.

| ID | Service | V2 request version | Operations |
| ---: | --- | --- | --- |
| 1 | core | 1 | log, monotonic time and fault reporting |
| 2 | memory | 1 | owner-tagged allocation, free and reallocation |
| 3 | cache | 1 | data-cache clean and instruction-cache invalidate |
| 4 | reclaim | 1 | release an owner's resources |
| 5 | jobs | reserved | no callable implementation |

The firmware accepts the v2 request version `1` for these service tables.
Versioned v3 requests use a packed `major.minor` value and require equal
major and a provider minor at least as large. The module-provided service
registry uses the registered service version as a minimum numeric version;
it is separate from the runtime's built-in service lookup.

MiniGL's additional service ids and table versions are in
[minigl-module.md](minigl-module.md#image-and-services). Modules have no
general-purpose job or thread service.

## Lifecycle, references and faults

The boot scan records each manifest slot as available, missing or rejected.
First use loads dependencies, verifies and copies the image, zeroes BSS,
synchronizes caches and calls init. Codec init binds `libzzm_rt` and
registers its typed operation table. MPEG-2 and WebM expose decoder glue;
WebP exposes its image operations. Their adapters select the current owner
before work is allocated.

`zzm_loader_acquire` increments the use count only after load succeeds.
`zzm_loader_release` decrements it. A v2 module with no references stays
loaded until its header's idle timeout expires; the packer writes a default
of 30 seconds. A new acquire cancels that idle interval. Unload unregisters
interfaces, calls fini, reclaims module allocations, poisons the slot with
ARM undefined instructions and releases dependency references.

Load and fault status is available through `zzm_loader_status` and
`SDK_OP_MODULE_STATUS`. States distinguish missing, available, loading, loaded,
load failure, fault, and rejection for ABI, hash, slot, service or capability.
Reasons distinguish file open/read, invalid image, init, storage contention,
dependency, cycle, quota and fault failures.

V3 release and reset ordering differ from the v2 idle path; see
[the MiniGL host contract](minigl-module.md#worker-unload-and-reset).

## Fixed arena and codec slots

The [memory map](memory-map.md) reserves the module image arena separately
from card-pool working memory. Slots contain code, read-only data, data and
BSS. Decoded frames, reference pictures, bitstream windows and audio buffers
are owner-tagged allocations outside those slots.

The manifest assigns MPEG-2, WebM, WebP and MiniGL slots and reserves slots
for H.264, H.265, MP4 and AAC. A reserved slot does not imply an installed
codec implementation or a measured playback rate. The firmware build
includes module recipes for MPEG-2, WebM and WebP; MiniGL is built separately.

Codec source licences and notices do not clear patent obligations.
Distribution of H.264/H.265 support requires a release-specific legal and
patent review; this document makes no patent-clearance assertion.

