/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


// FIXME allocate this memory properly

#define AUDIO_NUM_PERIODS           8
#define AUDIO_BYTES_PER_PERIOD      3840

#define FRAMEBUFFER_ADDRESS         0x00200000
// Frame buffer/graphics memory starts at 64KB (relative to card address), leaving ample space for general purpose registers.
#define MNT_FB_BASE     			0x00010000
#define AUDIO_TX_BUFFER_SIZE        (AUDIO_BYTES_PER_PERIOD * AUDIO_NUM_PERIODS)

#define Z3_SCRATCH_ADDR             0x033F0000 // FIXME @ _Bnu
#define ADDR_ADJ                    (FRAMEBUFFER_ADDRESS - MNT_FB_BASE) // FIXME @ _Bnu

#define AUDIO_TX_BUFFER_ADDRESS     0x3FC00000 // default, changed by driver
#define AUDIO_RX_BUFFER_ADDRESS     0x3FC20000 // default, changed by driver
#define TX_BD_LIST_START_ADDRESS    0x3FD00000
#define RX_BD_LIST_START_ADDRESS    0x3FD08000
#define TX_FRAME_ADDRESS            0x3FD10000
#define RX_FRAME_ADDRESS            0x3FD20000
#define RX_BACKLOG_ADDRESS          0x3FE00000 // 128 * 2048 space (256 kB) — must match FRAME_MAX_BACKLOG
#define USB_BLOCK_STORAGE_ADDRESS   0x3FE40000 // legacy name; shared SD boot / USB proxy buffer
#define BOOT_ROM_ADDRESS            0x3FCF0000

#define RTG_TEMPLATE_SCRATCH_ADDRESS 0x03400000
#define RTG_TEMPLATE_SCRATCH_SIZE    0x00100000
#define RTG_TEMPLATE_SCRATCH_END \
    (RTG_TEMPLATE_SCRATCH_ADDRESS + RTG_TEMPLATE_SCRATCH_SIZE)

#define LEGACY_SURFACE_HEAP_ADDRESS RTG_TEMPLATE_SCRATCH_END
#define LEGACY_SURFACE_HEAP_SIZE    0x02B00000
#define LEGACY_SURFACE_HEAP_END \
    (LEGACY_SURFACE_HEAP_ADDRESS + LEGACY_SURFACE_HEAP_SIZE)

#define SDK_LOW_DDR_RESERVED_END     0x08000000

// SDK v2 host-visible heap. Keep this below the 0x033f0000 scratch area used
// by the firmware until the SDK owns a formally reserved allocator region.
// SDK_OP_DECOMPRESS_BATCH decodes entirely inside a host-provided arena
// allocated from this shared heap (plus the LZH decoder's private <=64 KB
// window) -- it reserves NO additional DDR region, so it cannot collide
// with the Z3 fast-RAM window or the video codec scratch at 0x30000000.
#define SDK_SHARED_HEAP_ADDRESS     0x03000000
#define SDK_SHARED_HEAP_SIZE        0x003F0000
#define SDK_SHARED_HEAP_END \
    (SDK_SHARED_HEAP_ADDRESS + SDK_SHARED_HEAP_SIZE)
// SDK ARM-local surface heap. This is card-side DDR only and intentionally
// starts after the RTG template scratch and legacy accelerator surface heap.
#define SDK_LOCAL_SURFACE_HEAP_ADDRESS 0x06000000
#define SDK_LOCAL_SURFACE_HEAP_SIZE    0x02000000
#define SDK_LOCAL_SURFACE_HEAP_END \
    (SDK_LOCAL_SURFACE_HEAP_ADDRESS + SDK_LOCAL_SURFACE_HEAP_SIZE)

#if SDK_LOCAL_SURFACE_HEAP_ADDRESS < LEGACY_SURFACE_HEAP_END
#error "SDK ARM-local heap overlaps legacy accelerator surface heap"
#endif

#if SDK_SHARED_HEAP_END > Z3_SCRATCH_ADDR
#error "SDK shared heap overlaps Z3 scratch area"
#endif

#if SDK_LOCAL_SURFACE_HEAP_END > SDK_LOW_DDR_RESERVED_END
#error "SDK ARM-local heap exceeds low DDR reservation"
#endif

// Dual-core scheduler task-queue control region. A small SCU-coherent slab in
// the otherwise-unclaimed 0x18000000..0x30000000 hole -- above the
// linker-managed DDR (ps7_ddr_hi ends at 0x18000000) and below the codec
// scratch buffers at 0x30000000. Holds the taskq_shared_t control block only;
// crypto data buffers stay in SDK_SHARED_HEAP with core-0 cache management.
#define SDK_TASKQ_REGION_ADDRESS    0x18000000
#define SDK_TASKQ_REGION_SIZE       0x00100000     // 1 MB (one MMU section)
#define SDK_TASKQ_REGION_END \
    (SDK_TASKQ_REGION_ADDRESS + SDK_TASKQ_REGION_SIZE)

#if SDK_TASKQ_REGION_ADDRESS < 0x18000000
#error "task-queue region must sit above the linker-managed DDR (ends 0x18000000)"
#endif
#if defined(SDK_JEDI_REGION_ADDRESS)
#if SDK_TASKQ_REGION_ADDRESS < (SDK_JEDI_REGION_ADDRESS + SDK_JEDI_REGION_SIZE)
#error "task-queue region overlaps the JEDI region"
#endif
#endif
#if SDK_TASKQ_REGION_END > 0x30000000
#error "task-queue region overlaps codec scratch at 0x30000000"
#endif

// Dedicated core-1 stack (dual-core scheduler). The Cortex-A9 stack is
// full-descending, so the CPU is given the TOP. Reserved in the unclaimed
// 0x18000000..0x30000000 hole, immediately above the task-queue region and
// clear of every heap/framebuffer/DMA buffer below 0x08000000 -- the previous
// hardcoded 0x06000000 core-1 SP sat on the seam of the surface heaps and
// descended into the legacy accelerator heap. SDK_CORE1_STACK_TOP is chosen as
// a valid ARM data-processing immediate (0x1C = 8-bit value, rotated) so the
// reset stub can load SP in a single `mov` with no literal pool.
#define SDK_CORE1_STACK_TOP     0x1C000000
#define SDK_CORE1_STACK_SIZE    0x00100000     // 1 MB
#define SDK_CORE1_STACK_BASE    (SDK_CORE1_STACK_TOP - SDK_CORE1_STACK_SIZE)

#if SDK_CORE1_STACK_BASE < SDK_TASKQ_REGION_END
#error "core-1 stack overlaps the task-queue region"
#endif
#if SDK_CORE1_STACK_TOP > 0x30000000
#error "core-1 stack overlaps codec scratch at 0x30000000"
#endif

// SDK v2 bootstrap mailbox. The Amiga side reaches this through the existing
// board window at 0xd000, inside the legacy 0xa000..0xffff shared I/O buffer.
#define SDK_MAILBOX_WINDOW_OFFSET   0x0000D000
#define SDK_MAILBOX_ADDRESS \
	(USB_BLOCK_STORAGE_ADDRESS + (SDK_MAILBOX_WINDOW_OFFSET - 0x0000A000))
#define RX_FRAME_PAD 4
#define FRAME_SIZE 2048

// Our address space is relative to the autoconfig base address (for example, it could be 0x600000)
#define MNT_REG_BASE    			0x00000000

// 0x2000 - 0x7fff   ETH RX
// 0x8000 - 0x9fff   ETH TX
// 0xa000 - 0xffff   shared IO buffer (legacy USB block window)
