# ZZ9000 card memory map

The firmware uses a 1 GiB DDR address space from `0x00000000` through
`0x3FFFFFFF`. Fixed reservations, the linker image, and the card pool divide
that space. `memorymap.h` defines fixed firmware addresses; `lscript.ld`
places the firmware image and its heap.

Ranges below use an exclusive end: `0x00000000`–`0x08000000` is 128 MiB.

## Current layout

| Range | Size | Use |
|---|---:|---|
| `0x00000000`–`0x08000000` | 128 MiB | Low firmware image and board-memory services: framebuffer and RTG surfaces, SDK shared heap, and ARM-local surface heap. |
| `0x08000000`–`0x08200000` | 2 MiB | Z3 SDK mailbox, direct audio rings, legacy AHI/MHI audio scratch, and the alignment gap before the high firmware region. |
| `0x08200000`–`0x18000000` | 254 MiB | High firmware image, newlib heap, and ARM exception stacks. The default `_HEAP_SIZE` is 224 MiB. |
| `0x18000000`–`0x18100000` | 1 MiB | Dual-core task queue and shared session tables (`SDK_TASKQ_REGION_*`). |
| `0x18100000`–`0x1BF00000` | 62 MiB | Card-pool range A1. |
| `0x1BF00000`–`0x1C000000` | 1 MiB | Core-1 stack (`SDK_CORE1_STACK_*`). |
| `0x1C000000`–`0x20000000` | 64 MiB | Card-pool range A2. |
| `0x20000000`–`0x30000000` | 256 MiB | Zorro III fast RAM while advertised; otherwise card-pool range B. |
| `0x30000000`–`0x3FC00000` | 252 MiB | Card-pool range C. |
| `0x3FC00000`–`0x40000000` | 4 MiB | Fixed audio, boot ROM, Ethernet, and SD/USB buffers. |

The linker keeps its low image below `0x00200000`. Its high region begins at
`0x08200000`, and an assertion prevents the image, heap, and stacks from
reaching `0x18000000`.

## Fixed buffers at the top of DDR

The top 4 MiB is not card-pool memory. `memorymap.h` assigns it to these
fixed users:

| Address | Use |
|---|---|
| `0x3FC00000` | Default audio TX ring (`AUDIO_TX_BUFFER_ADDRESS`). |
| `0x3FC20000` | Default audio RX ring (`AUDIO_RX_BUFFER_ADDRESS`). |
| `0x3FC28000` | First audio-fabric lease ring. The second follows the 122,880-byte first ring. |
| `0x3FCF0000`–`0x3FCF2000` | 8 KiB boot ROM image. |
| `0x3FD00000`, `0x3FD08000` | Ethernet TX and RX descriptor lists. |
| `0x3FD10000`, `0x3FD20000` | Ethernet TX and RX frame buffers. |
| `0x3FE00000` | 256 KiB Ethernet receive backlog. |
| `0x3FE40000` | Shared SD boot and USB proxy block buffer. |

## Card pool

The card pool supplies large firmware working areas in 64 KiB pages. Every
allocation has an owner class and instance, so a session can release all of
its allocations at once.

| Class | Limit |
|---|---:|
| `FIRMWARE` | 64 MiB |
| `MEDIA` | 128 MiB |
| `IMAGE` | 72 MiB |

At startup, ranges A1, A2, and C are open. Range B is registered as a
revocable range but stays closed while Zorro III fast RAM is advertised. It
opens when that window is available to firmware, including on Zorro II.
Before an Amiga reset changes that decision, firmware revokes range B and
flushes its data cache. Reset then releases every non-`FIRMWARE` owner.

WebM decoder arenas use `MEDIA` owners, and WebP decoder arenas use `IMAGE`
owners. The pool is preferred over new fixed carve-outs.

## Cache attributes

The BSP translation table initially maps DDR as shareable, write-back,
write-allocate normal memory. Firmware changes the following areas for
non-coherent users:

| Range or address | Attribute | Reason |
|---|---|---|
| `0x08100000`–`0x08200000` | Non-cacheable | Z3 SDK mailbox and direct-audio ring section. |
| `0x3FC00000`–`0x3FD00000` | Non-cacheable | Audio formatter and audio-fabric lease rings. |
| `0x3FD00000`, `0x3FE00000` | Strongly ordered | Ethernet descriptor list and receive backlog sections. |

With the Generation-2 aperture contract acknowledged, the Zorro II SDK
mailbox uses one non-cacheable 4 KiB page immediately below its audio scratch
range.

## Adding a fixed range

1. Define its start, size, and exclusive end in `memorymap.h`, deriving it
   from a neighbour where possible.
2. Add `#error` guards for both neighbours.
3. Update the layout and cache-attribute tables here.

Use the card pool unless hardware or the Amiga requires a stable address.
