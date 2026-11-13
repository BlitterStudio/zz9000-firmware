# ZZ9000 app runtime

This is the firmware and launcher contract for the API 0.3 app kernel. The
public app interface is [zzapp-api.md](../sdk/docs/zzapp-api.md); application
build and usage guidance is in
[zzapp-developer-guide.md](../sdk/docs/zzapp-developer-guide.md). The shared
wire definitions in `ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_proto.h` are the
source of truth for byte offsets and constants.

## Kernel architecture

Core 1 runs a small kernel. It has one highest-priority system thread and up
to four app threads. An app process owns its address space, memory commit,
resource ledger, launcher slot, and generation. Its thread owns the saved
user context, kernel stack, scheduler state, and CPU accounting. There is one
app thread per process today.

An exception frame is the saved context. It includes the interrupted user
registers, SYS `sp` and `lr`, SPSR, VFP registers, and FPSCR. A new app starts
from a synthetic frame with the ELF entry point, API table, `argc`, `argv`,
and user stack. The exception return restores the selected frame without a
nested call into app code.

### Scheduling and doorbell

The core-1 private timer gives apps a 5 ms quantum. The scheduler first runs
due system work—SDK tasks, MiniGL service, launcher work, and audio feeding—
then selects runnable app threads round-robin. In an overload window, service
work is limited so runnable apps retain a 5% floor.

File and directory operations, console read and backpressure, display open and
flip, non-host audio open and close, and sleep park the caller at the SVC
handoff. `zz_gl_swap` parks through its flip. Completion wakes the slot READY;
file and directory waits wake TIMEOUT after 30 seconds and return
`ZZ_ETIMEDOUT`. Stop marks a parked waiter CANCELLED; waiting service calls
return `ZZ_EIO`. `zz_input_poll` and `zz_display_vsync` do not park; the
latter only yields once.

Core 0 does not wait for core 1. It publishes work and raises doorbell SGI 15
only when `service_pending` changes from zero to one. Core 1 drains a bounded
batch, rechecks the word before clearing it, and therefore cannot lose a wake
or require one SGI per task. Timer IRQ, doorbell SGI, and SVC handoff save the
current frame, choose the next thread, switch the address space when needed,
and return through that frame.

### Performance counters and probes

`ZZAPP_PERF_COUNTERS=1` enables the kernel measurements. They are sampled on
the SVC cadence and on exception-driven switches; `ZZAPP_PERF_COUNTERS=0`
removes their code and storage. The PMU runs only in privileged core-1 code:
it supplies context-switch cycles and per-slot instruction/data TLB-refill
counts, and is also initialized when `ZZAPP_SVC_PROBES=1` with counters off.
The other scheduler latencies use the shared core timer.

While an app runs, periodic report requests are coalesced and emitted at most
once every ten seconds across all slots; terminal paths record a `forced=1`
report that is distinguished in the emitted line. The system-thread service
path builds the report while it holds the kernel lock, then releases that lock
before writing the resulting `[app] perf` line; neither exception handling nor
a context switch prints or touches the UART. Each `p50/p99/max` triplet is
reported as `p50/p99/max`. `switch` starts at the vector timestamp before the
full scheduler frame is saved and ends after the selected frame is fully
restored, immediately before the final exception-return instruction. It
includes vector frame save, IRQ dispatch, scheduler/context copies, address
space switching, and frame restoration; it excludes hardware exception entry
and the final return instruction. `sgi` is one doorbell to the start of the
next worker service pass, and `sdk` is each task's enqueue publication to its
start. Sampling the pass boundary preserves the hot-path publication protocol;
first-service attribution would require coupling every cross-core work claim
to the publisher.
`sgi_burst` is `SGIs/task publications`; `sdk_span`, `minigl_span`,
`launcher_span`, `audio_span`, and `svc_span` are the 99th-percentile/maximum
non-preemptible spans for SDK tasks, MiniGL, launcher deferred work, audio
pumping, and sampled API-lock-held SVC work.
`audio` is `low_water/starved/underruns`: low-water crossings on both AHI and
card-fabric routes, retryable fabric fill failures, and played underruns.
The zero-underrun gate uses only the final `underruns` value. `tlb` is
instruction/data refills.
Histogram percentiles are power-of-two buckets, so the exact maximum is the
final number in each pair or triplet.

`ZZAPP_SVC_PROBES=1` adds the `svcprobe` veneers used to characterize bare SVC
paths. It defaults to off and is measurement tooling, not an application ABI.
Build and run the dedicated probe firmware when enabling it; ordinary firmware
does not expose those probe numbers.

### Address spaces and SVC

Each slot has a private 16 KiB L1 table derived from a privileged kernel
template and a fixed ASID. The private virtual window is
`0xC0000000`–`0xDFFFFFFF`. Image, heap, and stack are mapped there as
non-global 1 MiB sections. The app sees only its private pages plus grants for
its launcher window, host-visible blocks, and owned display surfaces. The
rest of DDR, firmware, devices, and other apps remain privileged-only.

The app-facing API and GL tables retain their layout and `size` semantics, but
their entries point to user-RX `svc; bx lr` veneers. On a user veneer call,
the vector copies the exception frame to that slot's kernel stack, switches to
SYS mode, and enables IRQs before invoking the handler. Every SVC handler
checks user pointers against the caller's mappings and permissions before
touching them. Invalid, overflowed, read-only-for-output, device, and
other-slot pointers return `ZZ_EFAULT`. Firmware code never becomes a
user-call target.

GL client-array state is per slot. `DrawArrays` validates every enabled vertex,
colour, and client texture-coordinate element in its requested range.
`DrawElements` also validates its index buffer and every indexed element before
MiniGL is called. An unmapped or changed range returns `ZZ_EFAULT` and records
`GL_INVALID_VALUE` for `glGetError`.

### Frames, commit, and teardown

Private memory is allocated as zeroed 1 MiB frames from aligned 8 MiB
card-pool extents. Admission is atomic: aggregate commit plus the request
must fit in pool free memory after the system reserve and any per-app launcher
limit. API 0.1 and 0.2 images retain their 96 MiB load-time commit. API 0.3
images commit image, stack, and their requested minimum heap; `heap_grow` and
`heap_shrink` add or return whole frames while keeping the heap VA range
fixed.

All terminal paths call the same generation-guarded teardown: stop new calls,
wake and cancel waits, quiesce work and DMA holding app pointers, hand off or
unbind display, detach audio, release GL and decoder sessions, unmap grants,
return frames and host-visible blocks, release the slot, and publish one
terminal reason. `LIST` retains that reason after the slot returns to idle.
The reasons are none, normal exit, forced stop, user fault, stall watchdog,
stuck API call, lease loss, Amiga reset, and core-1 restart. Per-slot
30-second stall and 10-second stuck-call watchdogs stop only their slot. A
separate core-0 backstop cold-restarts core 1 after two seconds without both
progress and timer ticks; that ends every non-idle slot with the restart
reason.

## Slot protocol

The app service is `0x0c00`; its catalog version is `0x00030000`. Integers
in mailbox payloads and windows are big-endian. `ZZAPP_CAP_SLOTS` advertises
four slots. `ZZAPP_CAP_LOW_MEMORY` is append-only in the extended `QUERY`
capability word; a client with `ZZAPP_CAP_SLOTS` uses the extended request form
for all operations after allocation and carries the returned slot and generation.

An extended request is exactly 48 payload bytes. The selector is at byte 40
(`slot`) and byte 44 (`generation`). Existing fields retain their old offsets;
unused bytes are zero. A selector with an out-of-range slot or a generation
that no longer owns the slot fails with the stale ownership error and changes
nothing.

Extended `ALLOC_WINDOW` atomically claims the lowest free slot and returns the
old 12-byte window reply followed by slot and generation at bytes 12 and 16.
A `CLAIMED` slot that does not start loading expires after two seconds, returns
to idle, and records the lease-loss reason. Once running, the launcher must
renew the slot heartbeat within the same two-second lease.

Extended `LOAD_BEGIN` carries the launcher memory limit in MiB at byte 0;
zero means no limit beyond the pool. `LOAD_CHUNK`, `LOAD_END`, `START`,
`STATUS`, and `DISPLAY_BIND` retain their old fields and append the selector.
For API 0.3 images, the limit applies to the initial image, stack, and
minimum heap commit as well as later heap growth. `DISPLAY_BIND` always
carries the spare-buffer field at byte 28; zero means none.

`LIST` (`0x0c09`) is extended-only. The selector names the slot and ignores
its generation. Its 48-byte reply has state, generation, committed bytes,
limit, app CPU milliseconds, and kernel service milliseconds at bytes 0
through 23. Byte 24 is the block reason: `NONE` (0), `FILE` (1), `CONSOLE`
(2), `EVENT` (3), `SLEEP` (4), `VSYNC` (5), or `AUDIO` (6). Byte 25 is the
terminal reason. Bytes 26-27 hold the overload flag, and bytes 28-47 are the
app name. Terminal reasons are `NONE` (0), `EXIT` (1), `FORCED` (2),
`USER_FAULT` (3), `STALL` (4), `STUCK_CALL` (5), `LEASE` (6),
`AMIGA_RESET` (7), and `CORE_RESTART` (8).

### Low-memory crossings

Version 2's lifecycle block reserves the unused bytes after `terminal_reason`
at `0xA4`; `0xA8` is its first append-only word. Existing version-2 launchers
continue their exact-version check and ignore that unadvertised word. A launcher
that needs the counter first sends extended `QUERY` and requires its capability
bit.

```text
capability bit: ZZAPP_CAP_LOW_MEMORY = 0x00000040
launcher caps: extended QUERY reply, big-endian u32 at byte 12
counter: lifecycle window offset 0xA8, big-endian u32
semantics: zeroed when a slot generation is claimed; increments once per
free-pool low-water crossing while that slot is running; wraps modulo 2^32
```

The same crossing queues one `ZZ_EV_LOW_MEMORY` event for the app; no
additional crossing is counted or queued until the allocator has re-armed
above its high-water mark.

### Legacy slot 0

A request whose payload is not 48 bytes is legacy. It addresses slot 0 only,
uses the API 0.2 request and reply shapes byte-for-byte, and has no selector.
Legacy `QUERY` reports slot 0 state; legacy `ALLOC_WINDOW` claims slot 0 only.
Therefore an unchanged API 0.2 ZZRun or `zz9k.library` client continues to
run one app without learning about the other slots. A client without
`ZZAPP_CAP_SLOTS` uses these legacy forms against older firmware.

### Launcher windows

Each slot gets a `ZZAPP_WINDOW_BYTES` (592 KiB) launcher window with the
unchanged layout. The fixed ranges are slot 0
`0x0805C000`–`0x080F0000`, slot 1 `0x07FC8000`–`0x0805C000`, slot 2
`0x07F34000`–`0x07FC8000`, and slot 3
`0x07EA0000`–`0x07F34000`. Slot 0 is the legacy range and ends at the fixed
Z3 host window. Each range is mapped non-cacheable before core 1 runs; its
window is allocated when the launcher claims that slot.

## Translation attribute changes

The firmware links with `-Wl,--wrap=Xil_SetTlbAttributes`. Every BSP call,
including calls originating inside the BSP archive, enters
`__wrap_Xil_SetTlbAttributes` in `zzapp_as.c`. The wrapper calls the real BSP
function first, then copies the changed section descriptor into the address
space template and every live app table through the normal break-before-make
path. Without that wrap, a later BSP section-attribute change would leave
core 1's template stale. Template-only changes do not invalidate core 0;
changes to a table core 1 may walk perform the required cache, TLB, and branch
predictor maintenance.
