# Firmware modules: runtime v2

Module runtime v2 replaces the boot-time-only module model with verified,
on-demand modules. A module remains a fixed-address `.ZZM` image, but it is not
resident merely because its file is installed. The manifest is the sole source
of module IDs, filenames, slot addresses and slot sizes; the packer, linker and
loader must consume the same manifest entry.

This document is the module-author contract. A module whose declared ABI,
services, quotas or image placement cannot be satisfied is unavailable; it
must never be partially initialized or called.

## Image and link recipe

A v2 `.ZZM` header identifies the module and declares:

- format and module-ABI versions, module ID/version, name, fixed load address,
  image/BSS sizes, init entry and SHA-256 of the image;
- a bounded list of required runtime services (ID and minimum interface
  version), and the services the module provides;
- the resident and per-session memory quotas and their memory classes; and
- lifecycle policy overrides, if any. Absent overrides use the defaults below.

The loader first validates the complete header, manifest match, image bounds,
hash, service requirements and quotas. It then maps/copies the image only to
its assigned slot, zeroes BSS, performs instruction-cache maintenance and
starts the module. There are no relocations, dynamic loading, symbol lookup or
imports from the firmware ELF. Reject an image that contains relocations or an
allocated section outside its slot.

Link every v2 module against `libzzm_rt.a`, not against an application-local
runtime or heap. The essential shape is:

```make
# Values come from the v2 manifest; do not copy addresses into module sources.
$(CC) $(CPU_FLAGS) -nostdlib -Wl,--gc-sections \
  -Wl,-T,util/zzm_module.ld \
  -Wl,--defsym,ZZM_SLOT=$(MODULE_SLOT) \
  -Wl,--defsym,ZZM_SLOT_SIZE=$(MODULE_SLOT_SIZE) \
  -o $(MODULE_ELF) $(MODULE_OBJECTS) libzzm_rt.a -lc -lgcc
$(ZZM_PACK) --manifest $(MODULE_MANIFEST) --module $(MODULE_ID) \
  $(MODULE_ELF) $(MODULE_ZZM)
```

The `-nostdlib` link does not make ordinary C unavailable: `libzzm_rt` supplies
the required allocation, syscall stubs and C/C++ runtime boundary. The module
linker script remains responsible for placing `.text`, read-only data, data
and BSS in the one manifest slot and asserting that their total fits. The
packer, rather than a filename convention, writes the authoritative header.

## `libzzm_rt` rules

`libzzm_rt` is the only supported path from a module to runtime resources.
Modules must not retain raw firmware function pointers beyond the lifetime of
the service binding supplied at initialization.

### Allocation and syscalls

- `malloc`, `calloc`, `realloc`, `free`, aligned-allocation variants and C++
  `new`/`delete` are routed to the **memory** service. Allocation carries the
  active session owner and requested memory class; it is not an untracked
  module heap.
- `_sbrk` always fails. A module cannot grow a newlib heap. Direct card-pool,
  firmware heap, MMIO and filesystem allocation are forbidden.
- File, socket, process, clock and terminal syscalls have no implicit host
  implementation. Unsupported syscalls fail with `ENOSYS`; logging goes
  through the **core** service. A module may not use a failed syscall as a
  fallback transport.
- Cache operations are requested through the **cache** service. A module may
  clean/invalidate only ranges it owns or ranges explicitly lent by the host.

C++ is optional, but must remain a freestanding module client: build with
exceptions, RTTI and thread-local storage disabled; do not use `atexit`,
process termination, dynamic loader facilities or background threads. Static
constructors run only as part of successful module start and static destructors
run during orderly unload. `libzzm_rt` supplies the required guards and
`new`/`delete` bridge; no second C++ runtime, private allocator or unwinder may
be linked. An allocation failure is reported by the configured no-exception
allocation path and must be handled by the module.

## Runtime service directory

The module runtime directory is independent of the public mailbox service
catalog. Its IDs are stable 16-bit values; an interface version is a
`major.minor` pair. A requirement matches only the same major version and a
provider minor version greater than or equal to the requested minor. Every
interface begins with `size`, so a newer minor version may append fields; a
consumer must test `size` before reading an optional field.

| ID | Interface | v2 version | Contract |
|---:|---|---:|---|
| `0x0000` | core | 1.0 | module start/stop context, structured logging, monotonic time, service lookup, fault reporting and service registration |
| `0x0100` | memory | 1.0 | owner-tagged allocation/free, quota accounting and explicit memory-class selection |
| `0x0101` | cache | 1.0 | ownership-checked data-cache clean/invalidate and instruction-cache synchronization |
| `0x0102` | reclaim/track | 1.0 | session owner creation, allocation/resource tracking, bulk reclaim and checked release of tracked handles |
| `0x0103` | jobs | reserved | no callable v2 interface; this ID is reserved for a future host-owned asynchronous job API |

A module declares every service it **requires** before load and every service
it **provides** after successful initialization. The loader binds requirements
before calling init. Registration is atomic: if initialization fails, no
provided interface is visible and all start-time resources are reclaimed. The
service directory must report the provider's module/version, interface
version, state and capability bits so callers can distinguish not installed,
incompatible and faulted from an unsupported codec feature.

The v2 runtime does not expose its scheduler to modules. `jobs` remains
reserved until cancellation, callback lifetime, core affinity and unload
ordering have one complete contract; modules must not reserve the ID privately
or invent an equivalent worker-thread API.

## Memory classes and quotas

The header declares both quotas; they are admission limits, not suggestions.
The loader refuses a module whose declared resident quota exceeds the manifest
allowance, and the memory service refuses any allocation that would exceed its
applicable quota.

| Class | Lifetime | Permitted use |
|---|---|---|
| `resident` | successful load until unload | module image/BSS, runtime bookkeeping and small bounded state required to expose an interface |
| `session` | one accepted client/media session | decoder state, input windows, frame/reference buffers, audio buffers and other reclaimable work memory |

Large memory is **session-only**. It cannot be charged to `resident`, retained
between sessions, or used to make a module permanently resident. The
reclaim/track service owns the session owner token; closing a session, refusing
it during a fault, or forcibly ending it reclaims every allocation and tracked
resource for that token. A module must not conceal a session allocation in a
static/global pointer after the session ends.

## Lifecycle, references and faults

A manifest entry begins **available** when its package is installed and passes
validation; it has no loaded image or registered interface. The first service
lookup that needs one of its provided interfaces starts an **on-demand load**.
Concurrent requests join that one load rather than loading the slot twice.
After image validation, constructor/start work and atomic interface
registration succeed, the module is **loaded** and may accept sessions.

An accepted session takes a strong module reference. A pending host-owned
callback or future job would also take a strong reference; a transient lookup
does not. On session close, release the reference only after all of that
session's tracked resources have been reclaimed. At reference count zero the
module becomes **idle**. The default idle-unload delay is **30 seconds**. A new
request during that interval cancels unloading and reuses the loaded module.
After the delay, the runtime stops the module, unregisters its interfaces,
runs orderly C++ teardown, reclaims resident resources and returns the slot to
available.

A start failure, contract violation or reported unrecoverable runtime error
moves the module to **faulted**. The runtime withdraws its interfaces, rejects
new sessions, drains/reclaims affected sessions and records the reason. It may
not call into the module after faulting. The default retry/back-off interval is
**30 seconds**; an explicit administrative reload may retry sooner after the
cause is corrected. Missing, hash/ABI/service mismatch, quota rejection and
fault are separate observable states.

## 32 MiB slots and H.264/H.265

v2 uses **32 MiB fixed slots**. Slot capacity covers the complete linked image:
code, read-only data, data and BSS. It is not decoder working memory and does
not replace a session quota. Each H.264 and H.265 implementation gets its own
32 MiB manifest slot, so the two codec images have 64 MiB of fixed-slot
capacity and cannot overwrite or evict each other. Their decoded frames,
reference pictures, bitstream windows and AAC buffers are session-class memory
and are reclaimed at session end.

This sizing establishes loadability headroom, not throughput. Estimates that a
software decoder may meet a particular resolution or frame rate are inference
until measured on the target card with the intended streams, display path and
audio active. There is no implied dedicated H.264/H.265 FPGA decoder. The FPGA
rasterizer remains present and continues to handle the display/raster path;
codec modules do not replace it.

Initial H.264/H.265 playback scope is deliberately narrow: 8-bit 4:2:0 video
in MP4, with AAC audio through the audio plugin contract. Higher bit depth,
4:2:2/4:4:4, HDR, arbitrary containers, subtitles and hardware-decoder claims
are outside that statement. Codec source licensing and an open-source notice
do **not** clear patent obligations. Distribution of H.264/H.265 support
requires a release-specific legal/patent review; this document makes no patent
license or clearance assertion.

## Codec plugin contracts

Codec modules expose typed services, not codec-specific calls from mailbox
handlers. All calls use caller-owned request/result structures whose first
fields are ABI version and size; unknown trailing fields are ignored and a
caller never reads beyond a provider-reported size.

- A **demuxer** probes a bounded byte prefix, opens a stream, publishes selected
  tracks and timed compressed access units, supports the declared seek/reset
  behavior, and closes without leaving borrowed input or session resources.
  It owns container parsing; it does not decode video or AAC.
- A **video decoder** accepts demuxed access units plus configuration, produces
  ordered 8-bit 4:2:0 frames or an explicit need-more-input/error result, and
  flushes/resets/releases all reference state on the session owner. It does not
  parse MP4 or control the rasterizer.
- An **audio decoder** accepts the selected audio access units, produces the
  negotiated PCM format with timestamps, reports delay/drain state, and
  releases all per-session state on close. AAC is selected through this
  contract, never by a direct H.264/H.265 module call.

The host owns session admission, timestamps, A/V synchronization, output
surfaces and the rasterizer hand-off. Plugins must return ordinary status codes
for malformed data, unsupported profiles and quota exhaustion; they must not
fault the module merely because one media file is invalid.

## Migration of existing modules

The current MPEG-2, WebM and WebP modules may retain their codec-specific glue
while they are first moved behind the v2 service directory. In that phase, the
existing glue is an adapter that implements the new decoder/demuxer/audio
contract and forwards allocation, cache and teardown to `libzzm_rt`; there is
one registry-facing path, not a parallel legacy path. Preserve tested
container/codec behavior while replacing direct `zzm_services_t` and
module-specific registry access with the v2 binding.

Once an adapter has equivalent v2 coverage, move only genuinely shared session,
tracking and output code to the host/runtime, then remove the superseded glue.
Do not rewrite a codec merely to remove a small adapter, and do not leave a
legacy direct call path after its v2 replacement is accepted.
