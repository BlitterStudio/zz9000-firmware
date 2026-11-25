# ZZ9000 card memory map

The firmware uses a 1 GiB DDR address space from `0x00000000` through
`0x3FFFFFFF`. Fixed reservations, the linker image, and the card pool divide
that space. `memorymap.h` defines fixed firmware addresses; `lscript.ld`
places the firmware image and its heap.

Ranges below use an exclusive end: `0x00000000`–`0x08000000` is 128 MiB.

## Current layout

Every boundary below is half-open. The Z3 main-board window holds two
allocators that must never share an address. rtg.library allocates screens
(and any bitmap the driver's AllocBitMap hook declines) itself inside the
MemorySize the RTG driver reports, and the firmware cannot see those
allocations. The driver therefore reports P96 VRAM up to the GFXData scratch
only, and everything the firmware hands out comes from the host-visible heap
above it.

| Range | Size | Use |
|---|---:|---|
| `0x00000000`–`0x00100000` | 1 MiB | Low aperture/boot-reserved DDR. |
| `0x00100000`–`0x00200000` | 1 MiB | Low exception vectors and MMU table only. |
| `0x00200000`–`0x033F0000` | 49.9375 MiB | P96 VRAM, managed by rtg.library. The RTG driver's MemorySize ends here. |
| `0x033F0000`–`0x08200000` | 78.0625 MiB | Firmware host-visible heap (one interval allocator): GFXData and template scratch, dynamic videocap backing, P96 off-screen surfaces from `ACC_OP_ALLOC_SURFACE`, SDK shared allocations, mailbox/direct-ring pages, the MiniGL wire-v9 transport window (one dynamic allocation, see below) and the AHI/MHI scratch. |
| `0x08200000`–`0x08600000` | 4 MiB | Firmware image and static data. App translation tables and process records live in the record arena in the card pool (see below). |
| `0x08600000`–`0x08700000` | 1 MiB | Coherent dual-core task queue and shared session tables. |
| `0x08700000`–`0x08800000` | 1 MiB | Full-descending core-1 stack. |
| `0x08800000`–`0x09000000` | 8 MiB region | Newlib heap plus 22 KiB ARM exception-stack reservation. |
| `0x09000000`–`0x0B000000` | 32 MiB | Fixed-slot firmware-module arena (`FIRMWARE_MODULE_ARENA_*` in `memorymap.h`). |
| `0x0B000000`–`0x2FE00000` | 590 MiB | Permanent prefix of the card pool. The record arena is taken from it at boot, before any app extent. |
| `0x2FE00000`–`0x3FE00000` | 256 MiB | Fast-RAM landing window when advertised; otherwise contiguous revocable card-pool tail. |
| `0x3FE00000`–`0x40000000` | 2 MiB | RTL-pinned audio, Ethernet, boot ROM, and USB/SD belt. |

### App kernel mappings

App-private frames have physical addresses in the permanent card-pool prefix;
they do not have a fixed DDR range. The app-frame allocator obtains 1 MiB
frames from 8 MiB-aligned pool extents and returns an empty extent to the pool.
`CARD_POOL_CLASS_APP_FRAMES` has no class cap: aggregate commit admission,
the system reserve, and the 512 MiB private-window bound limit it instead.

The kernel's tables, stacks, records and user API pages live in one permanent
card-pool block of class `CARD_POOL_CLASS_APP_ARENA`. Its sizing, board record
capacity, L2 reserve, cache attributes and recovery limits are described in
[app-runtime.md](app-runtime.md#record-arena).

Each app maps its private image, heap, and stack at
`0xC0000000`–`0xDFFFFFFF`. This 512 MiB virtual range is non-global and
private to the record's ASID; it is not an identity mapping. The low identity
map is privileged-only except for user grants to that app's host-visible
blocks and display surfaces.

App transport memory is not a fixed range. Each claim takes one 16 KiB, 8 KiB
aligned pair block (control block, submission and completion queues) from the
host-visible heap at `CLAIM`, and later up to four more blocks (service, data,
audio ring, load buffer), each a multiple of 8 KiB and 8 KiB aligned. They
are placed in the cacheable part of the heap, below the SDK mailbox section at
`0x08100000`, so no transport block lands in a page the firmware maps
non-cacheable. The pair, service and data blocks live until the client releases
the claim, the audio ring and load buffer have earlier ends, and a block the
client may still touch is kept until the release (see
[app-runtime.md](app-runtime.md#transport-and-claim-protocol)). Every
block counts one entry of the surface allocator's 4096-entry block table
(`SURFACE_ALLOCATOR_MAX_BLOCKS`).


### Zorro III SDK host window

`SDK_ALLOC_HOST_WINDOW` uses the fixed 64 KiB reservation
`0x080F0000`–`0x08100000` on Zorro III. It is in the firmware-owned
host-visible heap and is reserved before P96 or SDK allocations, so it cannot
overlap P96 VRAM. The former `0x005D0000` range remains the unchanged legacy
Zorro II fallback; the firmware publishes the active address in allocation
replies and `DIAG_MEMORY`.

The linker-managed image/static and heap regions cannot grow across the task
queue, core-1 stack, or first card-pool byte.

### MiniGL wire-v9 transport window

MiniGL's host-visible transport is allocated by the module's `host_window`
service on demand. Its placement and descriptor are described in the
[MiniGL module contract](minigl-module.md#transport-memory); the
[wire-v9 transport](minigl-v29-zz9000-transport.md#32-transport-window-layout)
defines the layout inside the window.

## RTL-pinned top belt

| Address or range | Use |
|---|---|
| `0x3FE00000`, `0x3FE20000` | Audio TX and RX formatter rings. |
| `0x3FE28000`–`0x3FE64000` | Two audio-fabric lease rings. |
| `0x3FEF0000`–`0x3FEF2000` | 8 KiB boot ROM image. |
| `0x3FF00000`, `0x3FF08000` | Ethernet TX and RX descriptor lists. |
| `0x3FF10000`, `0x3FF20000` | Ethernet TX and RX frame buffers. |
| `0x3FF30000`–`0x3FF70000` | Ethernet receive backlog. |
| `0x3FF70000`–`0x3FF76000` | Shared SD boot and USB proxy block buffer. |

The legacy mapped-I/O host window (`ZZ9K_MAPPED_IO_ARM_START`) is the
`0x3FF70000` USB/SD buffer. It must move with that buffer rather than alias a
card-local audio range.

## Card pool and Fast RAM

The card pool supplies 64 KiB pages, tracked by owner class and instance.
`FIRMWARE` and `APP_ARENA` allocations persist through an Amiga reset; media, image, input,
console, and ARM-only SDK-surface owners are reset-reclaimable. While Fast RAM
is withheld, the revocable tail is opened immediately after the permanent
prefix, forming one contiguous pool. Before re-advertising Fast RAM, firmware
revokes that tail and flushes cached ownership.

Fast RAM is advertised only when `fast_ram=on` is valid **and** RTL capability
bit `MNTZORRO_CAP_Z3_FASTRAM_DDR_2FE00000` is present in `MNTZORRO_REG7`.
This prevents new firmware from exposing Fast RAM on an old bitstream with a
different DDR landing base.

Firmware 2.9.0 requires the matching 2.9.0 bitstream. The Fast-RAM guard and
the relocated landing window are a single hardware/software contract.

## Cache attributes

DDR starts shareable write-back/write-allocate. The host-visible allocator
uses the normal mapping except for its mailbox, direct-ring, and other
non-coherent control pages, which use the existing second-level 4 KiB
non-cacheable mappings. The RTL-pinned audio belt is non-cacheable; Ethernet
descriptor/backlog storage uses its required strongly ordered mapping.

Zorro III accesses use the Zynq ACP, which is coherent with the ARM caches
for shareable write-back memory. That is why ordinary host-visible
allocations, including `aar_shared_alloc` blocks, need no ARM cache
maintenance for the 68k. The 68k's own caches are not covered; see
[app-runtime.md](app-runtime.md).

The PL310 is shared by both Cortex-A9 cores. Its r3p3 TRM “Background
operations” rule forbids a maintenance-register write while a way operation is
active. Firmware cache maintenance must therefore use `l2_maint_*`; the wrapper
keeps L1 work core-local and serializes L2 work in 4 KiB chunks, while vblank's
way flush takes priority after the current chunk. Modules request cache work
through the cache service rather than calling the BSP.

## Adding a fixed range

Use the card pool or the host-visible interval allocator unless hardware or
the Amiga protocol requires a stable address. A true fixed range must have
derived exclusive bounds, overlap assertions in `memorymap.h`, matching RTL
definitions where applicable, and an entry in this document.

The card-pool has 17 classes including `NONE`. Capped classes are `FIRMWARE`
64 MiB, `MEDIA` 128 MiB, `IMAGE` 72 MiB, `MEDIA_INPUT` 64 MiB,
`SDK_SURFACE` 128 MiB, `CONSOLE` 32 MiB, `MINIGL` 384 MiB, `APP` 96 MiB,
`MINIGL_MODULE` and `MINIGL_TRANSPORT` 4 MiB each, `MINIGL_CONTEXT`,
`MINIGL_APP_CONTEXT`, and `MINIGL_RIX` 384 MiB each, and `APP_DISPLAY`
32 MiB. `APP_FRAMES` and `APP_ARENA` have no class cap: for `APP_FRAMES`,
commit admission, the system reserve, and the private-window bound limit it;
`APP_ARENA` is one boot-time block sized by memory. Class `APP` holds only the
16 MiB staging block of an app between `LOAD_BEGIN` and `LOAD_END`.

