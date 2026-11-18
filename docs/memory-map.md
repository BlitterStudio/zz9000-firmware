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
| `0x08200000`–`0x08600000` | 4 MiB | Firmware image and static data. The image holds no app translation tables and no per-app arrays; those live in the record arena in the card pool (see below). |
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

The per-app kernel state is not in the image. One block of the permanent
prefix, class `CARD_POOL_CLASS_APP_ARENA`, holds it in parallel areas indexed
by process-record index: the kernel template L1 table and one 16 KiB L1 table
per record (16 KiB aligned), the shared pool of 1 KiB L2 tables, 8 KiB of user
API and GL table pages per record, a 32 KiB core-1 stack per record, and a
16 KiB record per record (`zzapp_rec.c:71-91`). It is taken at boot after
`card_pool_setup_card()` and before `zzapp_as_init()` (`main.c:1343-1361`),
when no app extent exists yet. Its class is permanent: never in the
revocable Fast-RAM tail, kept across an Amiga reset, never freed, and without
a class cap (`card_pool.c:181-184`, `card_pool.h:63-68,87`).

It is sized by memory. Records are
`(permanent bytes - 128 MiB system reserve) / 2 MiB`, bounded to 8 and 1024.
The L2 pool is 16 tables for the template, 16 reserved per record, and a
shared remainder of 2 tables per MiB of host-visible heap rounded up to a
multiple of 32. The block is rounded up to 1 MiB and, if the pool cannot
hold it, retried with half the records down to 8 (`zzapp_rec.c:55-91,155-179`).
On this board: 590 MiB permanent, 231 records, 3872 L2 tables (160 shared),
a 20 MiB arena, 3.4 percent of the prefix
(`test/zzapp/h/rec_test.c:94-114`). The commit ledger needs no extra
accounting, because the arena leaves the pool before any app extent. See
[app-runtime.md](app-runtime.md#record-arena) for the records, handles, L2
reserve, and ASIDs.

The arena stays Normal, shareable write-back, like the earlier per-slot
state in the task-queue section. There is no non-cacheable record prefix;
the arena offsets and size above are unchanged. A cooperative core-1 restart
writes its local dirty lines back before acknowledging the park request.
An unresponsive worker can lose its last updates on reset; see
[app-runtime.md](app-runtime.md#record-arena) for the cache and recovery limits.

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
non-cacheable, and they live until the client releases the claim (see
[app-runtime.md](app-runtime.md#transport-and-claim-protocol)). The former
four fixed 592 KiB launcher windows (`0x07EA0000`–`0x080F0000`) and the
boot loop that mapped them non-cacheable are gone: only the Z2 mailbox page
is still split into non-cacheable pages (`mmu_page.c`, one page table). Every
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

MiniGL's host-visible transport has a control/status page, two 256 KiB packet
slots, and a 64 KiB reply buffer (0x91000 bytes). It is one dynamic allocation
from the host-visible heap, taken by `minigl_transport_init()` at boot and
after every Amiga reset. There is deliberately no fixed MiniGL DDR
carve-out. The allocation is constrained (`surface_allocator_alloc_range`)
to `0x08110000`–`0x081C0000`, the free gap between the SDK mailbox and
direct-ring reservations inside the 1 MiB MMU section the mailbox already
maps Normal non-cacheable: both CPUs poll the control block, so the
window must not be cacheable, and confining it to that existing section
avoids new MMU work. The gap (704 KiB) exceeds the window with the
partition checks in `memorymap.h` holding. On allocation failure, or on
Zorro II where the host window cannot hold even a single-slot window, the
service reports unavailable and the Amiga library falls back exactly as
on firmware built without it.

The Amiga side discovers the window through a versioned big-endian
transport descriptor ('MGTD') published at offset 0x8000 inside the
64 KiB SDK mailbox reservation (descriptor plus both 64-entry rings end
at 0x2080, and no mailbox ABI user, firmware or host library, reads or
writes beyond them). The library locates the mailbox via
`REG_ZZ_SDK_MAILBOX_HI/LO`, validates the descriptor, and uses its
addresses verbatim.

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

