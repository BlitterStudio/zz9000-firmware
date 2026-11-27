# MiniGL as `MINIGL.ZZM`

`ZZ9K/MINIGL.ZZM` supplies the card-side MiniGL implementation: the wire-v9
service used by 68k `minigl.library`, the CPU renderer, the RasterIX backend,
and the in-process GL front end used by AAR apps. The firmware links the
module host, loader and resource services, but no MiniGL renderer or private
front-end objects. The build inventory is
`ZZ9000_proto.sdk/ZZ9000OS/modules/minigl/module.mk`; firmware CI checks its
ELF with `util/check_no_resident_minigl.sh`.

The [wire-v9 transport](minigl-v29-zz9000-transport.md) defines the 68k wire.
[app-runtime.md](app-runtime.md#address-spaces-and-svc) describes the app's
user-mode GL veneers and pointer checks.

## Image and services

MiniGL uses module format `ZZM3`, ABI 3.0, with a 512-byte header. The manifest
assigns module id 4, name `MINIGL`, a fixed load address of `0x09800000` and
a 4 MiB image-and-BSS slot. Working memory is allocated separately. The
packer and linker reject relocation sections and enforce the slot bounds;
there is no firmware-symbol import interface.

The loader checks format, ABI, module identity, slot, file size, image SHA-256,
service requirements, quotas and hardware capabilities before calling init.
It rejects nonzero reserved header bytes, unknown image flags and owner
classes, and incompatible wire, descriptor, host-window or RasterIX CSR
versions. `RASTERIX_EF_REQUIRED` is reserved and rejected.

The image flags can require core 1, host-visible memory, VFP/NEON, RasterIX
EF and PL330. A module requiring RasterIX EF cannot load on standard firmware.
A CPU-capable module uses the firmware's graphics capability table to decide
whether RasterIX is available; it does not probe optional MMIO itself.

Modules obtain services from `zzm_services_t.get_service`. Each table starts
with `version` and `size`; consumers check both before using its callbacks.
The MiniGL resource tables are declared in
`ZZ9000_proto.sdk/ZZ9000OS/src/zzm_runtime.h`:

| Service ID | Table | Version | Use |
| ---: | --- | --- | --- |
| 1 | core | 3.0 | logging, monotonic time and fault reporting |
| 2 | memory | 3.0 | owner-tagged allocation, reallocation and quota use |
| 3 | cache | 3.0 | owned-range data-cache and instruction synchronization |
| 4 | reclaim | 3.0 | typed owner creation, close, reclaim and quarantine |
| 6 | host window | 3.0 | allocate/free a Z3 host-visible non-cacheable block |
| 7 | palette | 3.0 | generation, CLUT snapshot and indexed lookup |
| 8 | app GL host | 4.0 | API guard, bound display, generation-qualified flip and flip wait |
| 9 | profile | 3.0 | counter registration, time samples and snapshots |
| 10 | transport | 3.0 | descriptor, doorbell and capability publication |
| 11 | graphics hardware | 3.0 | capability record, CSR/FIFO access, PL330 and fence publication |

Service 5 (`jobs`) has no callable implementation. Worker registration is
part of the MiniGL registration envelope, `zzm_minigl_registration_v1_t`,
which contains the module's context, operation table and worker table.
`zzm_minigl_register` requires the reset-protocol-v2 field and the app callbacks.
The worker table has `has_work` and `poll`; it is not a general thread API.

## Transport memory

The host-window service allocates only on Zorro III, from the host-visible
heap between `SDK_MAILBOX_Z3_ADDRESS` (`0x08100000`) and
`HOST_VISIBLE_HEAP_END` (`0x08200000`), avoiding allocations already reserved
there. The result has a board offset, ARM address, size, attributes and
generation. The service invalidates any old PL310 lines before the module
writes through the non-cacheable mapping.

The module requests the full Z3 window, binds it, and publishes a big-endian
`MGTD` descriptor through the transport service. The 68k client uses the
addresses and capacities it reports. See
[descriptor discovery](minigl-v29-zz9000-transport.md#23-discovery-sequence)
and the [window layout](minigl-v29-zz9000-transport.md#32-transport-window-layout)
for the descriptor location, offsets and sizes.

Before requiring `MGTD`, a 68k client sends `MINIGL_MODULE_ACQUIRE` through
the SDK mailbox. Core 1 loads the module and allocates the window; success
returns the descriptor generation. A missing module returns
`SDK_STATUS_MODULE_MISSING`; failure to bind its transport returns
`SDK_STATUS_MODULE_MISMATCH`. Zorro II has no module transport window.

The client sends `MINIGL_MODULE_RELEASE` only after it has stopped traffic,
destroyed its contexts and forgotten the cached window pointers. A context
close alone does not release the window. The firmware holds a module
reference for each successful acquire and releases it at the matching release
or Amiga reset.

## Contexts and AAR apps

Wire contexts use typed `MINIGL_CONTEXT` owners and generation-qualified
handles; see [wire context creation](minigl-v29-zz9000-transport.md#61-context-creation-bridge_create).
An AAR GL binding uses a `MINIGL_APP_CONTEXT` owner, and the transport has a `MINIGL_TRANSPORT` owner.
The firmware also provides `MINIGL_MODULE` and `MINIGL_RIX` memory classes.
Per-context quotas and the shared MiniGL card-pool budget bound allocations;
see [memory-map.md](memory-map.md#adding-a-fixed-range) for the class caps.
Texture arenas grow on demand; their limits are defined in `minigl_pool.h`.

The runtime admits one app GL binding at a time. The
[app runtime contract](app-runtime.md#address-spaces-and-svc) describes its
display-id checks. The runtime acquires the module, validates its registration and binds the
context. Failure returns `NULL` to the app; the internal helper reports
`AAR_E_NOSYS` for an unavailable module and `AAR_E_BUSY` for an existing
binding. There is no resident GL fallback.

The module's app wrappers bracket front-end calls with the firmware API guard.
Swap uses the app-host display and flip callbacks. Unbind, app stop and fault
reclaim release the binding and its module reference. The runtime exposes
the GL table through SVC veneers rather than making module addresses callable
from user mode.

`glGetString` returns an app-readable string that stays valid until GL unbind,
even after queries for other names. `GL_EXTENSIONS` returns an empty string:
the AAR binding does not advertise the 68k MiniGL guest extensions.

App GL supports six user clip planes, initially disabled.
`glClipPlane` takes four doubles and captures the equation using the
current modelview matrix. Use `glEnable` and `glDisable` with
`GL_CLIP_PLANE0` through `GL_CLIP_PLANE5`. The ARM geometry stage clips
before either the CPU or RasterIX rasterizer draws the primitive.
This does not add clip planes to the 68k wire protocol.

App GL reports `GL_STENCIL_BITS` as 0. There is no stencil buffer, and the
app's `glStencil*` calls have no effect.

The binding draws into and reads the swap target. In copy mode it rotates
three colour targets and presents the back one; the front target is the frame
the last swap queued. The front end opens an in-process context drawing and
reading the front target, so the module selects `GL_BACK` for both at bind and
maps `glDrawBuffer` and `glReadBuffer` of `GL_FRONT` and `GL_FRONT_LEFT` to it.
Without that, each frame's draws land in the previous frame's buffer and the
buffer a swap seals keeps its initial `0x000000ff` fill or an older frame.

The [app runtime's extension table](app-runtime.md#app-interface) describes
firmware and module build-id reporting.

## Worker, unload and reset

MiniGL polling runs on core 1. Core 0 routes the doorbell through the host;
it does not parse packets. The host prevents reentrant polling and skips the
worker while its `in_api` argument is set. When an app GL call is parked
waiting for RasterIX, the module can feed and retire its work, but defers
wire-command dispatch until that call returns.

The loader's use-reference count is separate from the registered worker.
At the final core-1 release, a v3 module can unload immediately: unregister
its services, run fini, detach the MiniGL registration, reclaim module
resources, poison the slot and release dependencies. Unload is deferred when
the module's own worker poll is still active. Core 0 only marks a zero-ref
v3 module idle; its periodic loader poll does not enter module fini or poison
core-1 code. The v2 idle timer in [modules.md](modules.md#lifecycle-references-and-faults)
does not describe this v3 release path.

Amiga reset first posts app reset and cancels new MiniGL acquisitions.
Firmware requests the module reset on core 1 and checks its acknowledgement
with a bounded deadline. The module reports the last wire activity, stops
the rendering engines, releases its app and transport owners, and resets
the wire service. It reports `FAILED` if the engines cannot be stopped.

A failed or late reset takes the cold-restart recovery path before module
code or owners can be reclaimed beside a running worker. The firmware also
checks the published RasterIX fence before returning card-pool memory;
unresolved GPU-visible allocations can be quarantined. A core-1 restart
forgets loaded or partially loaded core-1 modules without entering their
code; a later acquire reloads them from SD.

## Build and distribution

`BlitterStudio/amiberry-minigl-library` builds the module from a pinned
firmware checkout and its own front end. The firmware's `modules/minigl/module.mk`
lists the public backend and entry sources, the ten front-end inputs, slot
parameters, header requirements and Cortex-A9 hard-float build flags.
`modules/libzzm_rt` supplies the allocation and syscall bridge.

The module build excludes the Amiga library shell, external transports and
quadric implementation. ARM builds exclude the quadric header dependency.
Per-file licences and the SDK linking rules are described in
[LICENSING.md](../LICENSING.md); a binary kit must include the matching source
bundle and compatibility manifest.

[BUILD.md](../BUILD.md#publish-a-release) describes `util/minigl-kit.pin` and
release staging. The pin can be `none`, in which case firmware packages ship
without MiniGL. Firmware works without the module; GL bind or transport
acquire then reports it unavailable.

