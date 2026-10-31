# MiniGL as `MINIGL.ZZM`

## Decision and scope

Move the complete card-side MiniGL implementation out of the firmware image into
one loadable module, `ZZ9K/MINIGL.ZZM`. The firmware image does not link the GL
front end or any MiniGL service, rasterizer, RasterIX, PL330, transport, or
present implementation. `MINIGL.ZZM` supplies both the wire-v9 service used by
68k `minigl.library` and the in-process GL implementation used by ARM apps.

This is a clean cutover, not a resident implementation plus a module-shaped
copy. The existing service is resident because v2 deliberately has no worker,
host-window, or cross-core lifetime contract [docs/minigl-v29-zz9000-transport.md:151-158].
The current firmware build also compiles the private-repository front end from
`MINIGL_LIB_DIR` [ZZ9000_proto.sdk/ZZ9000OS/Makefile:313-325]. That is both the
public-build dependency to remove and the immediate source of the 78 KiB
high-image overrun.

`MINIGL.ZZM` retains wire protocol v9, the existing 68k library ABI, the
in-process front-end behaviour, and the CPU fallback. It is Zorro III only for
the 68k transport: the current transport correctly reports unavailable on Zorro
II because the window cannot fit [ZZ9000_proto.sdk/ZZ9000OS/src/minigl_transport.c:55-65].
ARM apps remain supported on a Z3 system whenever the module is available.

This document uses `MUST`, `SHOULD`, and `MAY` as RFC 2119 terms. File and line
citations describe the `feature/app-runtime` baseline at `eaf28c69`.

## Baseline and constraints

The existing manifest reserves module id 4, `MINIGL`, at `0x09800000` with a
4 MiB image-and-BSS slot [ZZ9000_proto.sdk/ZZ9000OS/src/zzm_manifest.c:7-16].
The module arena is the fixed 32 MiB range `0x09000000`–`0x0B000000`; the card
pool begins immediately afterwards [ZZ9000_proto.sdk/ZZ9000OS/src/memorymap.h:377-391].
The image slot is not working memory: module working allocations are separately
owner-tagged card-pool allocations [docs/modules.md:161-169].

V2 is intentionally insufficient:

- it has a 256-byte `ZZM2` header, fixed placement, no relocations, and only
  core, memory, cache, and reclaim services [ZZ9000_proto.sdk/ZZ9000OS/src/zzm_format.h:7-18,
  docs/modules.md:24-29];
- its `jobs` service is reserved specifically because cancellation, core
  affinity, and unload ordering have no contract [docs/modules.md:108-111];
- it cannot allocate host-visible non-cacheable memory or describe the owner
  classes needed by a shared Zorro window and large MiniGL contexts;
- it has no rule for a core-1 callback, a 68k context, an ARM app binding, or an
  in-flight RasterIX/PL330 command while unload or Amiga reset begins.

The existing core-1 loop makes the missing worker contract concrete: it calls
`mw_service_poll()` directly, while core 0 only falls back to polling when core
1 is unavailable [ZZ9000_proto.sdk/ZZ9000OS/src/scheduler_arm.c:311-385,
ZZ9000_proto.sdk/ZZ9000OS/src/main.c:1180-1181].

## Module ABI v3

### Compatibility and image format

ABI v3 adds a new `ZZM3` format and a 512-byte v3 header. The v2 parser and
`ZZM2` modules remain supported unchanged during the ABI-v3 transition; a v3
loader dispatches by magic and format version. It MUST reject an image whose
format, ABI major, header size, module id/name, fixed slot, hash, image/BSS
bounds, required service major, requested quotas, or hardware requirements do
not match. This keeps v2's all-or-nothing admission rule [docs/modules.md:9-11]
and its observable missing/mismatch/fault states [docs/modules.md:154-159].

`ZZM3` remains fixed-address and non-relocatable. All allocated ELF sections
MUST fit the manifest slot, and `SHT_REL`, `SHT_RELA`, `SHT_RELR`, dynamic
linker sections, and firmware ELF symbol imports MUST be rejected. This is not
a limitation of MiniGL: the v2 packer rejects relocations today
[util/zzm_pack.c:18-21, ZZ9000_proto.sdk/ZZ9000OS/src/zzm_format.h:36-61].
The module calls firmware only through the v3 service tables, so it needs no
firmware-symbol relocation.

The v3 header retains the v2 identity, version, fixed-address, image/BSS,
init/fini, SHA-256, requirement/provision, load-policy, idle-timeout, and
quota fields. It adds the following fixed fields before the hash; all reserved
bytes are zero and covered by the header validation.

| Field | Meaning |
|---|---|
| `abi_major`, `abi_minor` | `3.0` initially. A firmware accepts the same major and a module minor no greater than its supported minor. |
| `image_flags` | `ZZM_IMAGE_NEEDS_CORE1`, `ZZM_IMAGE_NEEDS_HOST_VISIBLE`, `ZZM_IMAGE_NEEDS_VFP_NEON`, `ZZM_IMAGE_NEEDS_RASTERIX_EF`, and `ZZM_IMAGE_NEEDS_PL330`. |
| `required_caps` | Wire version, transport-descriptor major, RasterIX CSR ABI major/minor, and host-window layout major. |
| `resident_quota` | Bounded card-pool state that survives while loaded: module bookkeeping and transport descriptor state only. |
| `owner_class_mask` | Owner classes the module may create: `MINIGL_CONTEXT`, `MINIGL_APP_CONTEXT`, `MINIGL_TRANSPORT`, and `MINIGL_RIX`. |
| `worker_ops_version` | Version and size of the registered core-1 worker-ops table. |
| `hardware_requirement` | `CPU_ONLY` or `RASTERIX_EF_OPTIONAL`; `RASTERIX_EF_REQUIRED` is reserved and MUST be rejected until a product actually needs it. |

`NEEDS_RASTERIX_EF` means that the module requires a firmware booted with a
RasterIX EF image and the declared CSR ABI; `RASTERIX_EF_OPTIONAL` means it can
use RasterIX when the firmware exposes it and otherwise runs the existing CPU
path. The module MUST NOT probe a CSR itself to decide whether a standard image
contains RasterIX. The current module-side platform setup accesses CSR and FIFO
MMIO directly [ZZ9000_proto.sdk/ZZ9000OS/src/mw_ctx.c:205-255], but the
firmware has to own the image choice, boot-time AFI3 setup, and capability
reporting.

A v3 module link is freestanding and fixed-address: `-nostdlib`, the v3 module
linker script with `ZZM_SLOT` and `ZZM_SLOT_SIZE`, `libzzm_rt.a`, and the
necessary static `-lc -lgcc -lm` objects. The existing recipe establishes that
shape [docs/modules.md:31-49]. `libzzm_rt` remains the only allocation and
syscall bridge: `_sbrk` and unsupported host syscalls fail, while allocations
carry the active owner and memory class [docs/modules.md:57-70]. Static
constructors are allowed only under the existing serialized module rules;
exceptions, RTTI, TLS, background threads, dynamic loading, and a second C or
C++ runtime are forbidden [docs/modules.md:72-81].

MiniGL MAY use hard-float VFP and NEON with the firmware's Cortex-A9 ABI. It
MUST compile with the same `-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard`
contract used by the app runtime [docs/app-runtime.md:38-40]. Core-1 hosting
owns saving VFP/NEON state when it enters an ARM app; the module must not save
or restore another caller's state, mask interrupts, install vectors, or change
MMU mappings.

### Firmware-to-module services

Every v3 table begins `uint32_t version; uint32_t size;`; new minor versions
append fields, and consumers MUST check `size`. Tables are passed to `init`,
not recovered from firmware symbols. These are the exact firmware-to-module
interfaces for MiniGL.

| Service | v3 operations supplied by firmware | MiniGL use |
|---|---|---|
| `core` | rate-limited UART log, monotonic microseconds, module fault, wake-core-1, and service lookup | diagnostics, profiling timestamps, fault reporting |
| `memory` | `alloc/free/realloc(owner, class, bytes, align, attributes)` and quota query | resident state and large card-pool context allocations |
| `host_window` | allocate/free a host-visible, non-cacheable, Z3-only region; return ARM address, board offset, attributes, and generation | the wire-v9 584 KiB transport window |
| `cache` | ownership-checked clean, invalidate, and instruction synchronization | packet, present, RasterIX command, and texture hand-off |
| `worker` | register/unregister one core-1 worker; set work pending; report active/idle | bounded service polling and unloading barriers |
| `transport` | publish/retract the big-endian `MGTD` descriptor; install/remove the MiniGL doorbell target; publish MGL capability bits | 68k discovery without module-owned mailbox access |
| `graphics_hw` | immutable image capability record; RasterIX CSR/FIFO accessors; PL330 channel claim/start/abort/release; cache/barrier hooks; AFI3 status | optional RasterIX EF backend without raw firmware register policy |
| `palette` | generation, immutable CLUT snapshot, and indexed lookup from the firmware RTG palette shadow | preserve CLUT present/capture behavior without a second palette shadow |
| `app_gl_host` | `api_enter/leave`, bound-display query, and generation-qualified display flip | guard direct app GL-table calls and present into the app's currently bound surface |
| `profile` | counter registration, timestamp/counter sampling, and snapshot export | existing MiniGL profiling without a second clock |
| `reclaim` | create, close, and force-reclaim typed owners; report owner refcount | context and module teardown |

`host_window` returns no mapping on Zorro II and has a fixed non-cacheable
attribute. This replaces MiniGL's direct `surface_allocator_alloc_range()`
call, whose present placement is an implementation detail
[ZZ9000_proto.sdk/ZZ9000OS/src/minigl_transport.c:68-90]. The current
transport is 0x91000 bytes (584 KiB), outside the module image
[docs/minigl-v29-zz9000-transport.md:170-192]. It receives owner class
`MINIGL_TRANSPORT`. A window is freed only after the guest has completed an
explicit transport-detach handshake; see “68k acquisition and lifetime”
below.

`memory` adds explicit large classes rather than pretending they are resident:

| Owner class | Lifetime and quota policy |
|---|---|
| `MINIGL_MODULE` | One small resident allocation domain, released only after successful `fini`. |
| `MINIGL_TRANSPORT` | One host-window allocation, non-cacheable and host-visible; it survives context destruction and is released only after guest detach or Amiga reset. |
| `MINIGL_CONTEXT` | One 68k wire context: color/depth targets, texture pools, CPU and RasterIX command buffers. |
| `MINIGL_APP_CONTEXT` | One ARM app GL context; same resources, keyed to the app generation. |
| `MINIGL_RIX` | RasterIX-only context resources; it is a child of a context and cannot outlive it. |

The header declares the resident maximum and a per-context maximum; the runtime
also has a firmware-wide MiniGL cap. Allocation over either limit fails cleanly.
All large allocations are context-owned card-pool allocations, matching the
current `mw_ctx` resource model and the card-pool design rather than consuming
module BSS [ZZ9000_proto.sdk/ZZ9000OS/src/mw_ctx.c:414-435,
docs/modules.md:125-130].

`graphics_hw` exposes an opaque v1 platform table, not raw constants. Firmware
sets the AFI3 width once at boot, validates the selected image, maps the CSR and
FIFO with the correct attributes, and reserves the PL330 channel. The module
may submit through the table only after `has_rasterix_ef` and the CSR ABI range
match its header. On failure it releases its claim and changes only its own
context to CPU fallback. The existing backend already has a CPU fallback when
platform probing fails [ZZ9000_proto.sdk/ZZ9000OS/src/mw_ctx.c:437-492].

### Module-to-firmware registration and calls

During `MINIGL.ZZM` initialization the module registers one `zzm_minigl_ops_v1`
service. Firmware holds this table only while the module has a strong reference.
The table is the sole firmware-to-MiniGL call surface.

| Operation | Called by | Required semantics |
|---|---|---|
| `get_info(out)` | capability/status query | returns module version, wire v9, descriptor major, optional RasterIX state, and feature bits |
| `doorbell()` | core 0 mailbox/doorbell route | records work and wakes core 1; it does not parse packets on core 0 |
| `has_work()` / `poll(budget)` | core-1 scheduler | polls at most the declared bounded work and returns; no sleep or blocking storage operation |
| `open_68k()` / `close_68k(handle)` | accepted wire create / destroy | acquires/releases one strong context reference |
| `transport_acquire()` / `transport_release()` | firmware's pre-discovery MiniGL mailbox operation | starts or detaches the guest transport lease; acquire returns only after `MGTD` is published |
| `app_bind(surface, out_gl)` / `app_swap()` / `app_unbind()` / `app_reclaim(generation)` | `zzapp` runtime | exports the existing named `zzapp_gl` table and holds one app-context reference |
| `begin_reset()` / `reset_poll()` / `abort_reset()` | Amiga-reset coordinator | stops admission, drains or abandons hardware, retracts descriptor, and reports quiescence |
| `begin_unload()` / `fini()` | module loader on core 1 | unregisters worker/doorbell after quiescence, then releases resident state |

The module obtains services through the init table and calls only the operations
in the preceding firmware-to-module table. It MUST NOT call `printf`,
`Xil_*`, `Xil_SetTlbAttributes`, `surface_allocator_*`, `card_pool_*`,
`scheduler_*`, or a firmware global directly. The current implementation does
several of those things (for example direct cache and PL330 commands
[ZZ9000_proto.sdk/ZZ9000OS/src/mw_ctx.c:256-371]); v3 moves that authority into
services rather than leaving an accidental private ABI.

### Worker, references, unload, and reset

The worker service is deliberately smaller than a general job API.
`worker.register(ops, execution_pin)` takes an execution pin while registered.
`ops.poll()` runs only on core 1, is non-reentrant, has a packet or time
budget, and MUST return; `ops.has_work()` is a hint only. Core 0 may call only
`doorbell()` and `worker.wake()`. The scheduler calls registered handlers in a
stable order from `scheduler_core1_service_once` and from the app timer service
tick. It MUST NOT call a handler while `in_api` is non-zero. This preserves the
app-runtime rule that a 68k packet runs between GL calls, never inside one
[docs/app-runtime.md:44-61].

`execution_pin` is not a client reference and is not included in the loader's
idle-ref count. When the last external use reference becomes zero, the
30-second timer starts. At expiry the runtime enters draining, detaches the
worker on core 1, then drops `execution_pin`; only then can it call `fini`.
This avoids the circular rule in which a permanently registered worker prevents
the zero-ref unload condition in the current loader
[ZZ9000_proto.sdk/ZZ9000OS/src/zzm_loader.c:441-480].

A usage reference is held by each live 68k context, ARM app binding,
transport lease, reset/unload operation, and in-flight RasterIX/PL330
submission. A lookup or a published capability has no usage reference. The
existing loader's normal zero-ref idle timer and unregister-before-fini order
are the starting behavior [ZZ9000_proto.sdk/ZZ9000OS/src/zzm_loader.c:441-480],
but v3 adds these mandatory steps:

1. Mark the module draining and withdraw new 68k opens and app binds.
2. For a transport lease, run the guest detach handshake before retracting
   `MGTD`; do not free or reuse the host window merely because its last context
   closed.
3. Core 1 drains queued wire work, finishes or abandons RasterIX/PL330, closes
   every context owner, and releases the associated usage references.
4. Unregister the doorbell route and worker only after no poll is active.
5. Unregister `zzm_minigl_ops_v1`, run `fini`, reclaim the resident owner,
   poison the slot, and release dependencies.

A 30-second idle delay applies only after all context, app, and transport-lease
references are gone. Idle unload is cancelled by a new open/bind before step 1.
If a bounded reset or hardware drain cannot complete, firmware first stops or
cold-restarts core 1, proves no module poll can still execute, then invokes the
non-waiting PL330/RasterIX abort and reclaims owners. The existing emergency
path likewise cancels the MiniGL reset before abort/free
[ZZ9000_proto.sdk/ZZ9000OS/src/main.c:573-621]. It MUST NOT reclaim a context
while a still-running CPU rasterizer can write it.

On Amiga reset, ordering is fixed: stop new clients; call the module reset
operation on core 1; wait only for its bounded acknowledgement; on timeout
stop/cold-restart core 1 before aborting hardware; retract the descriptor and
capability; reclaim MiniGL owners; then continue normal transport/card-pool
reset. App hosting must first force-stop a bound app and call `app_reclaim`, as
it already requires app reset before transport teardown [docs/app-runtime.md:90-94].
A core-1 fault withdraws the module service without calling its code on the
faulting stack, resets the worker execution pin, uses the runtime's owner
recovery, and leaves the module faulted until reload/reboot.

## `MINIGL.ZZM` composition

The module contains the following production objects. Headers move with their
implementation or become v3 SDK headers; test-only `rix_bin.c` does not move.
The current firmware build list is the authority for the compiled baseline
[ZZ9000_proto.sdk/ZZ9000OS/Makefile:183-198, 320-325].

| Group | Objects moved into `MINIGL.ZZM` |
|---|---|
| Wire-v9 service | `mw_service.c`, `mw_parse.c`, `mw_draw.c`, `mw_texture.c`, `mw_ctx.c`, and their `mw_*` headers including `mw_host.h`, `mw_state.h`, and `mw_types.h` |
| Transport and presentation | `minigl_transport.c`, `minigl_present.c`, and headers; transport placement becomes calls to `host_window` and `transport` services |
| CPU renderer | `minigl_raster.c`, `minigl_lighting.c`, `minigl_profile.c`, `minigl_renderer.h`, `minigl_pool.h`, and headers |
| RasterIX EF | `minigl_rix_backend.c`, `rix_cmd.c`, `rix_drv.c`, `rix_setup.c`, `rix_texture.c`, and headers; PL330 code presently in `mw_ctx.c` is retained there but accesses hardware only through `graphics_hw` |
| ARM-app glue | the implementation presently in `zzapp_gl.c`: table construction, bind/unbind, in-process context, drain, present copy, and `zzapp_gl_*` hooks |
| Library front end | `amiberry_gl_state.c`, `amiberry_gl_draw.c`, `amiberry_gl_texture.c`, `amiberry_gl_light.c`, `amiberry_gl_light_get.c`, `amiberry_gl_list.c`, `amiberry_gl_buffer.c`, `amiberry_glu.c`, `amiberry_backend_inproc.c`, `amiberry_transport_inproc.c`, plus `mgl_platform.h`, `amiberry_types.h`, `mgl_debug_stub.h`, and required private headers |
| Module runtime | v3 `libzzm_rt.a`, module entry glue, linker script, and module manifest data |

The ARM front end excludes `amiberry_backend.c`, `amiberry_glu_quad.c`, and
`vendor/` today [ZZ9000_proto.sdk/ZZ9000OS/Makefile:317-323]. It continues to
exclude them. In particular, the SGI-derived quadric implementation and the
Hyperion framework are not part of the card module. The module build MUST also
remove the current ARM source closure's include of PolyForm-only
`amiberry_glu_quad.h` from `amiberry_glu.c`, or obtain an explicit compatible
grant before it claims a GPL-only closure; excluding only
`amiberry_glu_quad.c` is insufficient.

Firmware retains only infrastructure:

- the Zorro doorbell/mailbox interrupt path, module loader/registry, v3
  service implementations, boot scan, capability register publication, and
  core-1 dispatch;
- firmware-owned boot configuration: AFI3, RasterIX image selection and CSR
  identity discovery, MMU attributes, PL330 ownership, cache primitives,
  timers, rate-limited UART, profiling sink, and Amiga-reset coordination;
- the app runtime's non-GL API and a thin registry lookup for GL;
- no `mw_*`, `minigl_*`, `rix_*`, `zzapp_gl.c`, or private-library front-end
  object.

The in-process architecture does not change: it opens one front-end context,
uses `MiniGLTransport`, and submits synchronously through `mw_host_*`; it does
not create a Zorro window for an ARM app [docs/app-runtime.md:596-610].

`zzapp_api.c` stops defining weak MiniGL implementations. Its GL entries
perform an ABI-v3 registry lookup and `zzm_loader_acquire(MINIGL)` on first
bind; after a successful module `app_bind`, it returns the module's
size-prefixed `zzapp_gl` table. The reference lasts through `gl_unbind`, app
stop, app fault, and Amiga reset. The public pointer-returning `gl_bind`
returns `NULL` when the module is missing, rejected, incompatible, or faulted;
the internal bind helper reports `ZZ_ENOSYS`. This preserves the public app
table's optional-call contract [docs/app-runtime.md:404-405,
ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_api.c:39-42,88-99]. There is no
in-firmware GL fallback.

The `app_gl_host` table is essential because an app calls function pointers in
the returned GL table directly. Each wrapper brackets the front-end call with
`api_enter/leave`, so the runtime's `in_api` guard remains true; `app_swap`
gets the bound surface, performs the present copy, and invokes the
generation-qualified display flip. Those are the moved `zzapp_gl.c` behaviors
[ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_gl.c:180-218,489-529], not optional weak
hooks in the module.

### 68k acquisition and lifetime

An existing guest cannot ring the MiniGL doorbell until it has mapped `MGTD`,
so “load on first valid doorbell” cannot bootstrap an on-demand module. The
updated card transport therefore uses the already discovered SDK mailbox
(address read from the SDK registers) to send a bounded
`MINIGL_MODULE_ACQUIRE` request before it requires `MGTD`. Firmware performs
the core-1 load, binds the window, publishes capability and descriptor, replies
with the descriptor generation, and the library then rereads and validates the
descriptor. On unsupported or incompatible firmware the request fails and the
library takes its existing unavailable path. Trap-120 precedence is unchanged.

The library must send `MINIGL_MODULE_RELEASE` only after it has destroyed all
contexts, stopped traffic, unmapped/forgotten the cached descriptor pointers,
and invalidated its transport state. Until that acknowledgement, the transport
lease pins the module and its window even if the last GL context has closed.
The current transport retains cached control/status pointers across context
destruction, so freeing the window earlier would create use-after-free bus
access. On Amiga reset firmware may forcibly revoke the lease after the
core-1-stop barrier. This explicit acquire/release change is a required 68k
library/driver package update; it is why idle unload applies to ARM-only use
but normally not to an opened 68k library.

For 68k clients, `MINIGL_MODULE_ACQUIRE` schedules the on-demand core-1 load.
Firmware publishes `MGL9` capability and the `MGTD` descriptor only after
module init and transport bind succeed. If the module is missing or mismatched,
firmware publishes no descriptor/capability and the existing library's
unavailable path is used; it MUST NOT manufacture a partial transport window.
`MGL9` currently identifies wire version 9 and capability bits
[ZZ9000_proto.sdk/ZZ9000OS/src/mw_service.h:19-24,279-291].

The policy is header-only boot discovery from `ZZ9K/` followed by on-demand
image load. This retains v2's boot scan and avoids adding MiniGL back to the
firmware boot cost [docs/modules.md:132-149]. The module remains pinned while
any context, app binding, transport lease, worker operation, or hardware
submission exists; it otherwise follows the 30-second idle-unload policy. A
release MAY elect to preload a verified module after boot only for
troubleshooting, but boot loading is not the normal policy.

### Size and placement

There are no committed ARM object, ELF, or map artifacts from which a measured
module image can be claimed. The current source inputs total 528,521 bytes for
the 16 firmware MiniGL/GL-glue translation units and 333,200 bytes for the ten
front-end translation units: 861,721 source bytes before headers, generated
code, `libzzm_rt`, and libraries. The build list establishes the exact input
set [ZZ9000_proto.sdk/ZZ9000OS/Makefile:183-198, 320-325]. This is a sizing
inventory, not an object-size measurement.

The provisional image budget is 3 MiB text/rodata/data plus 512 KiB BSS, with
512 KiB fixed-slot margin. It is deliberately below the current 4 MiB slot.
The first module-build stage MUST record `arm-none-eabi-size -A` for every
object and the final ELF, and `check_zzm_conformance.sh` MUST prove fixed-slot
bounds. A measured `image + bss` above 4 MiB blocks the cutover.

The expected outcome is that the 4 MiB slot fits: the resident front end was
enough to push the unrelated 4 MiB firmware high-image budget by 78 KiB, while
the module gets its own 4 MiB slot and working buffers are outside it. This is
an inference, not a measured module size. Do not introduce variable placement
or relocations for that uncertainty. If the link gate fails, make the explicit
manifest change to one fixed 8 MiB MiniGL slot in unused arena space, move any
future slots once in the same manifest change, and keep the fixed-address model.

## Build location and licence

Three arrangements are viable.

| Option | Who builds what | Advantages | Costs and obligations |
|---|---|---|---|
| A. Library repository builds the module from pinned public firmware sources **(recommended)** | `BlitterStudio/amiberry-minigl-library` owns `MINIGL.ZZM` make target. It checks out a pinned `zz9000-firmware` commit for v3 SDK, module linker/packer, and GPL backend sources, and uses its own pinned front-end sources. Firmware builds only firmware. | Removes the private front-end dependency from public firmware CI; one owner compiles all front-end/platform variants; preserves public backend source authority; smallest source move. | The release must publish complete corresponding GPL source for the exact module: the public firmware commit plus the exact dual-licensed front-end revision/build files, patches, toolchain instructions, and module manifest. Private CI alone is not a source offer. |
| B. Move backend sources to the library repository or a new MiniGL repository | That repository builds module and owns all MiniGL source; firmware consumes only SDK headers. | Single source tree for the module. | Duplicates or transfers GPL backend authority, complicates public review/CI and future firmware interfaces, and makes the private repository the only place for GPL backend changes unless a public mirror/release source tree is maintained. |
| C. Keep the module build in the public firmware repository | Public CI builds the module. | One public build and source location. | It reintroduces the unavailable private front end into public CI, the constraint that caused the move. Vendoring the front end is a different source-ownership decision and is not justified here. |

Choose A. The library repository's existing CI remains responsible for REUSE and
68k `minigl.library` variants; add a module job that pins firmware `eaf28c69`
or its successor, builds `MINIGL.ZZM`, runs the host MiniGL suites, and emits
an SPDX/source manifest. Firmware CI builds the v3 SDK/loader, module-format
and ABI conformance tests, and proves that `ZZ9000OS.elf` has no MiniGL object
or `MINIGL_LIB_DIR` dependency. The module build consumes a versioned SDK
archive or public checked-out firmware revision, never an unpinned working
tree.

The GPL choice for the identified front-end files is the intended module
licence path; the source set is explicitly dual-licensed for this use in the
app-runtime contract [docs/app-runtime.md:526-547]. Do not add the library
shell, framework, quadric source, or other PolyForm-only Amiga/host transports.
Before distributing the module, E MUST either make the `amiberry_glu.c` ARM
closure independent of the PolyForm-only `amiberry_glu_quad.h` or obtain an
explicit compatible grant for that header. Until then, a claimed GPL module
source package is incomplete. The firmware backend files are GPL-3.0-or-later
(for example, the wire service header
[ZZ9000_proto.sdk/ZZ9000OS/src/mw_service.c:1-5]). The RasterIX RTL remains in
the bitstream and is not copied into the module. Its firmware driver is GPL;
`rix_drv.c` records that its bounded packet and texture-ordering rules follow
XACP librixgpu (0BSD) while the implementation is ZZ9000 firmware
[ZZ9000_proto.sdk/ZZ9000OS/src/rix_drv.c:1-7]. Git history for the moving
backend files attributes the commits to Dimitris Panokostas; the release source
manifest MUST nevertheless retain every file's SPDX/provenance notice and name
the librixgpu design-rule attribution. This is an engineering release
requirement, not legal advice.

Host test ownership after the split is:

| Suite | Home and responsibility |
|---|---|
| `test/minigl` | firmware tree; CPU raster, lighting, present, and backend unit/differential coverage, linked against the pinned module sources |
| `test/minigl_v9` | firmware tree; wire-v9 parser, service, transport guard, guest-client end-to-end, and RasterIX command mapping |
| `test/rix_xsim` and `test/rasterix` | firmware tree; RTL/EF and command-stream qualification; module build supplies the same driver sources |
| `test/zzapp` GL test | firmware tree; app loader/runtime binding and absent-module behavior, linked to the module host build rather than private source paths |
| library `tests/` | library tree; 68k library and Amiberry transports stay unchanged and must not require a card module build |

## Release and kit packaging

A firmware release contains no MiniGL implementation and is usable without the
module. The drivers installer and SD-card kit ship the matching
`ZZ9K/MINIGL.ZZM` beside the normal module packages. The 68k `minigl.library`
continues to ship in its own drivers/library package under its per-file licences;
it is not embedded in the firmware image.

Each kit has a small signed-or-hashed compatibility manifest containing:

- firmware build id and module ABI `3.major.minor`;
- module id/version, `ZZM3` hash, required wire version (9), transport
  descriptor major, and RasterIX CSR ABI range;
- the bitstream flavour and whether it is a RasterIX EF image;
- library release/version and the capability bits it expects.

The firmware boot scan records installed, incompatible, and absent module
states but does not load the image. On `MINIGL_MODULE_ACQUIRE` or ARM GL bind it
compares the module header requirements to the firmware's v3 tables and
immutable image capability record. A standard firmware with a CPU-capable
module loads it and publishes CPU capability. A module requiring RasterIX EF on
a standard image, a mismatched CSR ABI, bad hash, wrong module ABI, or wrong
wire/descriptor major remains unavailable. Firmware MUST publish neither `MGL9`
capability nor `MGTD` in that case; public app `gl_bind` returns `NULL` (its
internal bind status is `ZZ_ENOSYS`); 68k keeps its existing unavailable
result. It MUST NOT silently load a module with a different wire major or touch
a RasterIX CSR on a standard image.

A newer compatible minor version may append table fields and capability bits;
older consumers ignore them after checking table size. Major wire, descriptor,
module ABI, and RasterIX CSR changes require a matched kit. This follows the
existing service-directory same-major, provider-minor-at-least-required rule
[docs/modules.md:83-90]. `MGL9` publication remains the discovery signal, not
an assertion that a filename happened to be present.

## Migration stages

The preserved current in-firmware kit is the rollback and baseline artifact.
Because that image is already 78 KiB above its 4 MiB high-image budget, no
integration branch may link resident MiniGL and the v3/module implementation
together. The first flashable v3 firmware is the module cutover and MUST
reproduce the baseline before it replaces that kit.

1. **Freeze the baseline and measure it.** Record object and final-ELF section
   sizes for the exact object lists above, capture profile counters and host
   suite outputs, and preserve a current in-firmware kit. Acceptance: current
   `test/minigl`, `test/minigl_v9`, RasterIX/xsim, and `test/zzapp` GL suites
   have baseline results; CPU and RasterIX profile counters are captured.
2. **Build ABI v3 in isolation.** Add `ZZM3` parsing, services, typed owner
   classes, host-window allocator, core-1 registration, capability record,
   ref/unload/reset protocol, and conformance tests, using a test worker and
   host harness rather than resident MiniGL in the firmware image. Acceptance:
   v2 modules still load; the worker allocates/frees a host window, runs on
   core 1, holds/releases usage and execution pins separately, and is stopped
   before teardown after fault. The preserved baseline kit remains the hardware
   reference for this non-flashable intermediate.
3. **Build the complete host module.** Create the v3 module target under the
   recommended build arrangement. Link all backend and front-end inputs, prove
   fixed slot bounds/no relocations, and run the existing host suites against
   that artifact. Acceptance: module-size gate passes, CPU and RasterIX host
   images reproduce baseline pixels and profile counters within the existing
   test tolerances; missing hardware capability selects CPU rather than faults.
4. **First flashable module cutover: all MiniGL paths.** In one link change,
   remove every resident `mw_*`, `minigl_*`, `rix_*`, `zzapp_gl.c`, and private
   front-end object; replace direct scheduler calls with the registered handler;
   add `MINIGL_MODULE_ACQUIRE/RELEASE`; route app GL through
   `zzm_minigl_ops_v1`; and move transport bind/descriptor publication behind
   the module. The app's in-process front end cannot remain resident because it
   directly calls the backend being removed. Acceptance: the firmware meets its
   4 MiB high-image gate; the updated 68k library acquires, validates, uses,
   releases, and reacquires v9; the real app GL test draws and presents through
   the module; public `gl_bind` returns `NULL` and internal bind reports
   `ZZ_ENOSYS` when absent/mismatched; and CPU/RasterIX profiling matches the
   baseline.
5. **Qualify lifetime and recovery on hardware.** Exercise transport guard,
   packet validation, e2e clients, app and 68k concurrent contexts, context
   destruction, explicit guest release, idle unload, app fault/stop, Amiga
   reset, timeout recovery, and RasterIX CPU fallback. Acceptance: no guest
   accesses a released window; core 1 is stopped before emergency reclaim; an
   app failure reclaims only its app MiniGL owner without disturbing a 68k
   context.
6. **Package and enforce the clean cutover.** Publish matched standard and
   RasterIX EF kits plus source manifests. Acceptance: no firmware link input
   is a MiniGL object or private library path; fresh SD/installer kits give
   correct missing/mismatch behavior and a hardware run covers CPU, RasterIX
   EF, 68k acquire/release, ARM app, unload, and reset.

## Parallel implementation slices

The interfaces above are the shared contract. Each slice owns its listed files;
no slice edits another slice's ABI headers without an agreed v3 header change.

| Slice | Ownership | Interface delivered | Acceptance |
|---|---|---|---|
| A: v3 format/loader | `zzm_format.*`, `zzm_loader.*`, `zzm_manifest.*`, `zzm_registry.*`, packer and conformance tests | `ZZM3` parser/header, version/capability validation, fixed-slot policy | v2 regression plus malformed v3/mismatch/slot tests |
| B: v3 runtime resources | `zzm_runtime.*`, `libzzm_rt`, `card_pool.*`, host-visible allocator, cache/profile/palette/app-host service tests | memory, typed owners, host-window, cache, palette, app GL host, log/time/profile tables | quotas, non-cacheable window, palette parity, direct-table `in_api` guard, owner reclaim and rate-limit tests |
| C: scheduler/reset/hardware host | `scheduler_arm.*`, core-1 registration, `main.c` reset coordinator, boot capability record, PL330/RasterIX wrapper, pre-discovery mailbox route | worker registration, acquire/release dispatch, reset/unload barrier, opaque hardware table | core-1-only poll; no core-0 parser; fault/reset, timeout stop barrier, AFI3/CSR gating, and lease tests |
| D: MiniGL module backend | new module target; moving `mw_*`, `minigl_*`, `rix_*`; module-specific tests | `zzm_minigl_ops_v1`, v3 service consumption, wire transport | `test/minigl`, `test/minigl_v9`, CPU/RasterIX profile equivalence, slot gate |
| E: front end/module build | private library repo: ten listed front-end sources, platform glue, GLU-header closure fix or licence grant, module make/release manifest | GPL-mode front-end objects and reproducible `MINIGL.ZZM` build pinned to firmware SDK | REUSE, 68k suite unchanged, host in-process test, verified GPL source closure, and source-manifest artifact |
| F: app-runtime integration | `zzapp_api.*`, `zzapp_host.*`, SDK GL wrappers, `test/zzapp` | registry-based bind/unbind/swap/reclaim, `NULL` public absent path and internal `ZZ_ENOSYS` | app GL/present, fault/reset, direct-table guard, and concurrent 68k-context test |
| G: packaging/CI | firmware and library CI, installer/SD scripts, kit manifest | matched module and source package; firmware no-private-source build | fresh-kit standard/EF mismatch matrix and high-image size gate |

Slices A, B, and C share only the versioned SDK headers. D may start against
those headers with a host fake. E may start as soon as D publishes its source
list and build variables. F uses `zzm_minigl_ops_v1`, never private front-end
symbols. G starts once the manifest schema is fixed. The app-hosting timer,
fault-resume, and `.text` write-protect work remains in its own ownership area:
C integrates through its scheduler service-tick hook and F through
`app_reclaim`; neither duplicates that work.

## Risks and maintainer decisions

The real risks are bounded and should be decided before implementation:

1. **Build/release authority:** confirm option A: the private library repository
   builds `MINIGL.ZZM` from a pinned public firmware SDK/backend revision and
   releases complete corresponding GPL source with every binary.
2. **Slot policy:** accept the 4 MiB fixed slot as the initial hard gate. If the
   measured link exceeds it, approve a one-time fixed 8 MiB manifest layout;
   do not add relocatable or variable placement.
3. **Load and guest-lifetime policy:** confirm on-demand after header-only boot
   scan, entered through the new 68k acquire handshake; a 68k transport lease
   pins the module/window until explicit guest release or Amiga reset, while
   30-second idle unload applies after all other references are gone.
4. **Hardware matrix:** confirm that a CPU-capable module is valid on standard
   firmware, while RasterIX is opt-in and selected solely from the firmware
   image capability/CSR ABI record.
5. **Public compatibility:** confirm that no module means public app
   `gl_bind == NULL` (internal bind `ZZ_ENOSYS`) and existing 68k unavailable
   behavior, not a resident fallback or a second GL implementation.
6. **GL source closure:** confirm the chosen remedy for the current
   PolyForm-only `amiberry_glu_quad.h` dependency: an ARM conditional that
   removes it, or an explicit compatible grant before a GPL module ships.
7. **Kit source delivery:** confirm the exact public location and retention
   policy for module source bundles and pinned toolchain/container identifiers.

No further architectural decision is required to begin ABI v3 and the module
host build. The wire-v9 protocol, single MiniGL implementation, ARM app GL
through the module, CPU fallback, and firmware ownership of the doorbell,
boot configuration, and reset coordination are fixed by this design.
