# ZZ9000 card memory map

The firmware uses a 1 GiB DDR address space from `0x00000000` through
`0x3FFFFFFF`. Fixed reservations, the linker image, and the card pool divide
that space. `memorymap.h` defines fixed firmware addresses; `lscript.ld`
places the firmware image and its heap.

Ranges below use an exclusive end: `0x00000000`–`0x08000000` is 128 MiB.

## Shared Zorro III allocator

Every boundary below is half-open. On Zorro III, `0x00200000`–`0x08200000`
is one shared allocator interval. P96 CARDMEM allocations scan from the
bottom upward, so a screen at offset zero stays contiguous. Firmware-owned
surfaces, capture backing, SDK allocations, and overlay buffers scan from the
top downward. Neither owner can free the other's blocks.

The CARDMEM protocol is enabled only when firmware capability bit 13
(`ZZ_FW_CAP_P96_CARDMEM`) is advertised. A matched driver uses its accepted
flag and big-endian offset reply, including an accepted offset of zero. An
older firmware that lacks bit 13 keeps the driver's previous allocation path;
it does not attempt CARDMEM commands.

| Range | Size | Use |
|---|---:|---|
| `0x00000000`–`0x00100000` | 1 MiB | Low aperture/boot-reserved DDR. |
| `0x00100000`–`0x00200000` | 1 MiB | Low firmware image and board services. |
| `0x00200000`–`0x08200000` | 128 MiB | One host-visible allocator interval: P96 bottom-up; firmware top-down. |
| `0x08200000`–`0x08600000` | 4 MiB | Firmware image and static data. Qualification build `141c121` measured `__bss_end = 0x0848F670`, leaving 1,511,824 bytes of growth margin. |
| `0x08600000`–`0x08700000` | 1 MiB | Coherent dual-core task queue and shared session tables. |
| `0x08700000`–`0x08800000` | 1 MiB | Full-descending core-1 stack. |
| `0x08800000`–`0x09000000` | 8 MiB region | Newlib heap plus 22 KiB ARM exception-stack reservation. |
| `0x09000000`–`0x0A000000` | 16 MiB | Fixed external firmware-module arena: MPEG-2, WebM and WebP slots. |
| `0x0A000000`–`0x2FE00000` | 606 MiB | Permanent prefix of the card pool. |
| `0x2FE00000`–`0x3FE00000` | 256 MiB | Fast-RAM landing window when advertised; otherwise contiguous revocable card-pool tail. |
| `0x3FE00000`–`0x40000000` | 2 MiB | RTL-pinned audio, Ethernet, boot ROM, and USB/SD belt. |

`reserve_host_control_ranges()` reinstalls these immutable Zorro III
reservations after every allocator reset: GFXData
`0x033F0000`–`0x03400000`; template scratch
`0x03400000`–`0x03500000`; SDK host window
`0x080F0000`–`0x08100000`; SDK mailbox
`0x08100000`–`0x08110000`; direct-ring pages
`0x081C0000`–`0x081E0000`; and AHI/MHI scratch
`0x081E0000`–`0x08200000`. Capture backing is also allocated and retained
at reset before either owner receives dynamic allocations.

The host window is firmware-owned on Zorro III: `SDK_ALLOC_HOST_WINDOW`
allocations land in its fixed reservation, and the firmware publishes the
actual ARM address in allocation replies and `DIAG_MEMORY`. The
`0x005D0000` fallback remains exclusively for legacy Zorro II behavior.

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

The legacy mapped-I/O host window (`ZZ9K_MAPPED_IO_ARM_START`) is the
`0x3FF70000` USB/SD buffer. It must move with that buffer rather than alias a
card-local audio range.

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

