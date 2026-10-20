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
| `0x00100000`–`0x00200000` | 1 MiB | Low firmware image and board services. |
| `0x00200000`–`0x033F0000` | 49.9375 MiB | P96 VRAM, managed by rtg.library. The RTG driver's MemorySize ends here. |
| `0x033F0000`–`0x08200000` | 78.0625 MiB | Firmware host-visible heap (one interval allocator): GFXData and template scratch, dynamic videocap backing, P96 off-screen surfaces from `ACC_OP_ALLOC_SURFACE`, SDK shared allocations, mailbox/direct-ring pages and the AHI/MHI scratch. |
| `0x08200000`–`0x08600000` | 4 MiB | Firmware image and static data. Qualification build `141c121` measured `__bss_end = 0x0848F670`, leaving 1,511,824 bytes of growth margin. |
| `0x08600000`–`0x08700000` | 1 MiB | Coherent dual-core task queue and shared session tables. |
| `0x08700000`–`0x08800000` | 1 MiB | Full-descending core-1 stack. |
| `0x08800000`–`0x09000000` | 8 MiB region | Newlib heap plus 22 KiB ARM exception-stack reservation. `_sbrk` reports the 8,366,080-byte break reservation and peak use at reset. This is the 8 MiB minimum after a sub-1 MiB service-path peak, providing more than 2× margin. |
| `0x09000000`–`0x2FE00000` | 510 MiB | Permanent prefix of the card pool. |
| `0x2FE00000`–`0x3FE00000` | 256 MiB | Fast-RAM landing window when advertised; otherwise contiguous revocable card-pool tail. |
| `0x3FE00000`–`0x40000000` | 2 MiB | RTL-pinned audio, Ethernet, boot ROM, and USB/SD belt. |

The linker gives image/static and heap their own memory regions, so neither
can grow across the task queue, core-1 stack, or first card-pool byte.

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

## Card pool and Fast RAM

The card pool supplies 64 KiB pages, tracked by owner class and instance.
`FIRMWARE` allocations persist through an Amiga reset; media, image, input,
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

## Adding a fixed range

Use the card pool or the host-visible interval allocator unless hardware or
the Amiga protocol requires a stable address. A true fixed range must have
derived exclusive bounds, overlap assertions in `memorymap.h`, matching RTL
definitions where applicable, and an entry in this document.

