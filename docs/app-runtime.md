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
work is limited so runnable apps retain a 5% floor. File I/O, console input,
event waits, sleep, vsync, and audio-space waits block the caller rather than
spinning; completion, timeout, and cancellation wake it with a result.

Core 0 does not wait for core 1. It publishes work and raises doorbell SGI 15
only when `service_pending` changes from zero to one. Core 1 drains a bounded
batch, rechecks the word before clearing it, and therefore cannot lose a wake
or require one SGI per task. Timer IRQ, doorbell SGI, and SVC handoff save the
current frame, choose the next thread, switch the address space when needed,
and return through that frame.

### Address spaces and SVC

Each slot has a private 16 KiB L1 table derived from a privileged kernel
template and a fixed ASID. The private virtual window is
`0xC0000000`–`0xDFFFFFFF`. Image, heap, and stack are mapped there as
non-global 1 MiB sections. The app sees only its private pages plus grants for
its launcher window, host-visible blocks, and owned display surfaces. The
rest of DDR, firmware, devices, and other apps remain privileged-only.

The app-facing API and GL tables retain their layout and `size` semantics, but
their entries point to user-RX `svc; bx lr` veneers. Every SVC handler checks
user pointers against the caller's mappings and permissions before touching
them. Invalid, overflowed, read-only-for-output, device, and other-slot
pointers return `ZZ_EFAULT`. Firmware code never becomes a user-call target.

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
terminal reason. A user-mode abort ends its own app. A privileged kernel or
firmware fault uses the existing core-1 cold restart and ends all live slots
with the restart reason.

## Slot protocol

The app service is `0x0c00`; its catalog version is `0x00030000`. Integers
in mailbox payloads and windows are big-endian. `ZZAPP_CAP_SLOTS` advertises
four slots. A client with that capability uses the extended request form for
all operations after allocation and carries the returned slot and generation.

An extended request is exactly 48 payload bytes. The selector is at byte 40
(`slot`) and byte 44 (`generation`). Existing fields retain their old offsets;
unused bytes are zero. A selector with an out-of-range slot or a generation
that no longer owns the slot fails with the stale ownership error and changes
nothing.

`QUERY` has the unchanged 16-byte legacy reply. Its extended reply appends
slot count, free slots, free commit, and total commit at bytes 16, 20, 24, and
28. Extended `ALLOC_WINDOW` atomically claims the lowest free slot and returns
the old 12-byte window reply followed by slot and generation at bytes 12 and
16. A claimed slot not loaded before its lease expires is released.

Extended `LOAD_BEGIN` carries the launcher memory limit in MiB at byte 0;
zero means no limit beyond the pool. `LOAD_CHUNK`, `LOAD_END`, `START`,
`STATUS`, and `DISPLAY_BIND` retain their old fields and append the selector.
Extended `STOP` is conditional on that selector. `DISPLAY_BIND` always carries
the spare-buffer field at byte 28; zero means none.

`LIST` (`0x0c09`) is extended-only. The selector names the slot and ignores
its generation. Its 48-byte reply reports state, generation, committed bytes,
limit, app CPU milliseconds, system-service milliseconds, block and terminal
reasons, overload flag, and a NUL-padded 20-byte basename.

### Legacy slot 0

A request whose payload is not 48 bytes is legacy. It addresses slot 0 only,
uses the API 0.2 request and reply shapes byte-for-byte, and has no selector.
Legacy `QUERY` reports slot 0 state; legacy `ALLOC_WINDOW` claims slot 0 only.
Therefore an unchanged API 0.2 ZZRun or `zz9k.library` client continues to
run one app without learning about the other slots. A client without
`ZZAPP_CAP_SLOTS` uses these legacy forms against older firmware.

### Launcher windows

Each slot gets a `ZZAPP_WINDOW_BYTES` (592 KiB) launcher window at the start
of its own 1 MiB section. The four windows occupy one section-aligned 4 MiB
host-visible region, mapped non-cacheable by whole section at boot. A window
is an identity-mapped user section only in its owner's address space. This
preserves the existing window layout while giving each app one TLB entry and
preventing another slot from reading it.

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
