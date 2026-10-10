# ZZ9000 card memory map

The card has 1 GB of DDR at `0x00000000`–`0x3FFFFFFF`. The BSP translation table maps all of it as shareable, cacheable normal memory; code that changes that for a range (audio sections, Ethernet descriptors, the SDK mailbox) says so next to the range.

`ZZ9000_proto.sdk/ZZ9000OS/src/memorymap.h` is the authority for every fixed address, and `ZZ9000_proto.sdk/ZZ9000OS/src/lscript.ld` places the firmware image and heap. This page is the overview.

| Range | Size | Owner |
|---|---|---|
| `0x00000000`–`0x08000000` | 128 MB | What the Amiga can reach through the Zorro board window: low firmware sections, the framebuffer and RTG surfaces, the SDK shared and ARM-local heaps |
| `0x08000000`–`0x08200000` | 2 MB | Z3 SDK mailbox, audio direct rings and AHI/MHI audio scratch |
| `0x08200000`–`0x18000000` | 254 MB | High firmware code, the general newlib heap (224 MB, `_HEAP_SIZE`), the stacks, and room for the code to grow |
| `0x18000000`–`0x18100000` | 1 MB | Dual-core task queue and the shared session tables (`SDK_TASKQ_REGION_*`) |
| `0x18100000`–`0x1BF00000` | 62 MB | Card pool range A1 |
| `0x1BF00000`–`0x1C000000` | 1 MB | Core-1 stack (`SDK_CORE1_STACK_*`) |
| `0x1C000000`–`0x20000000` | 64 MB | Card pool range A2 |
| `0x20000000`–`0x30000000` | 256 MB | Zorro III fast RAM while it is advertised; card pool range B otherwise |
| `0x30000000`–`0x3FC00000` | 252 MB | Card pool range C |
| `0x3FC00000`–`0x40000000` | 4 MB | Default audio TX/RX rings, audio lease rings, boot ROM, Ethernet descriptors and frames, SD/USB block buffer |

## The card pool

Large working memory for firmware services comes from the card pool (`card_pool.h`), not from new fixed ranges or the general heap. The pool hands out 64 KB pages to an owner — a class and an instance — and keeps a byte limit per class, so one service cannot take memory another needs. A session releases everything it owns in one call, and the firmware releases every non-permanent owner on an Amiga reset.

Range B is the Zorro III fast RAM window. The pool uses it only while `fast_ram` is off in `ZZ9000.CFG` (and always on Zorro II, which has no fast RAM). On every Amiga reset the firmware takes range B back and cleans the ARM data caches before the fast RAM decision is re-read, so a warm reset that switches fast RAM on hands the Amiga memory no ARM code still touches.

## Adding a range

1. Define its start, size and end in `memorymap.h`, deriving it from a neighbour where you can.
2. Add an `#error` guard against each neighbour.
3. Update the table above.

Prefer asking the card pool for memory over adding a fixed range. A fixed range is only right when hardware or the Amiga needs a stable address.
