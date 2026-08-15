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

// Host-window heap: the ONLY SDK allocation region a Zorro 2 board can
// reach. The Z2 autoconfig window is 4 MB, so the shared heap at
// 0x03000000 (board offset ~46 MB) is unreachable from the host there.
// The RTG driver frees board 0x3E0000..0x3F0000 for this heap by
// shrinking Z2 VRAM 64 KB (its template scratch, placed at MemorySize,
// slides down with it); the AHI/MHI audio scratch starts right above at
// board 0x3F0000. ARM address = board offset + ADDR_ADJ. Allocations
// land here only when the client passes SDK_ALLOC_HOST_WINDOW --
// default allocations stay in the shared heap so Z2 crypto offload
// keeps failing fast into its software fallback instead of squeezing
// through this tiny region.
// These constants are now the compatibility fallback for Z3 and bitstreams
// without the generation-tagged aperture register. Generation-1 Z2
// bitstreams expose RAM_SIZE through AXI slot 7 and use the window-relative
// regions computed in sdk_aperture_layout.c; firmware publishes that dynamic
// heap only after the host acknowledges the matching contract. The existing
// SDK guard still prevents a legacy 2 MB host from using this 4 MB address.
#define SDK_HOST_WINDOW_HEAP_ADDRESS 0x005D0000
#define SDK_HOST_WINDOW_HEAP_SIZE    0x00010000
#define SDK_HOST_WINDOW_HEAP_END \
    (SDK_HOST_WINDOW_HEAP_ADDRESS + SDK_HOST_WINDOW_HEAP_SIZE)

#if SDK_HOST_WINDOW_HEAP_ADDRESS < FRAMEBUFFER_ADDRESS
#error "host-window heap must sit inside the board-window DDR region"
#endif
// Board offset of the heap end must fit the 4 MB Zorro 2 aperture.
#if (SDK_HOST_WINDOW_HEAP_END - ADDR_ADJ) > 0x00400000
#error "host-window heap exceeds the 4 MB Zorro 2 board window"
#endif
// Must not reach into the Z2 audio scratch (board 0x3F0000 = ARM 0x5E0000).
#if SDK_HOST_WINDOW_HEAP_END > (0x003F0000 + ADDR_ADJ)
#error "host-window heap overlaps the Z2 audio scratch"
#endif

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

// The legacy AHI/MHI drivers place their Zorro 3 TX/RX rings at the top of
// the 128 MB main board aperture. The buffer offset written to firmware is
// BoardSize - 0x20000, relative to FRAMEBUFFER_ADDRESS; TX occupies the first
// 30 KB and RX starts 32 KB later. The formatter RX DMA keeps writing its ring
// even when only playback is active, so this entire host-visible window must
// stay outside the linker-managed firmware image and newlib heap.
//
// lscript.ld deliberately starts ps7_ddr_hi at the next 1 MB boundary,
// FIRMWARE_HIGH_DDR_ADDRESS. Keep these values synchronized with its
// __z3_audio_scratch_* symbols and ASSERT.
#define Z3_MAIN_BOARD_WINDOW_SIZE       0x08000000
#define Z3_AUDIO_SCRATCH_ADDRESS \
    (FRAMEBUFFER_ADDRESS + Z3_MAIN_BOARD_WINDOW_SIZE - 0x00020000)
#define Z3_AUDIO_SCRATCH_SIZE           0x00010000
#define Z3_AUDIO_SCRATCH_END \
    (Z3_AUDIO_SCRATCH_ADDRESS + Z3_AUDIO_SCRATCH_SIZE)
#define FIRMWARE_HIGH_DDR_ADDRESS       0x08200000

#if SDK_LOCAL_SURFACE_HEAP_ADDRESS < LEGACY_SURFACE_HEAP_END
#error "SDK ARM-local heap overlaps legacy accelerator surface heap"
#endif

#if SDK_SHARED_HEAP_END > Z3_SCRATCH_ADDR
#error "SDK shared heap overlaps Z3 scratch area"
#endif

#if SDK_LOCAL_SURFACE_HEAP_END > SDK_LOW_DDR_RESERVED_END
#error "SDK ARM-local heap exceeds low DDR reservation"
#endif

#if Z3_AUDIO_SCRATCH_ADDRESS < SDK_LOW_DDR_RESERVED_END
#error "Z3 audio scratch overlaps low DDR allocators"
#endif

#if Z3_AUDIO_SCRATCH_END > FIRMWARE_HIGH_DDR_ADDRESS
#error "Z3 audio scratch overlaps linker-managed firmware DDR"
#endif

// Z3 fast-RAM DDR window. VARIANT_Z3_FASTRAM bitstreams map the 256 MB
// fast-RAM PIC to this FIXED range regardless of where AmigaOS autoconfig
// placed the board (mntzorro.v `Z3_FASTRAM_ARM_BASE`; bitstreams older than
// 2026-07 instead reused the main-window offset, which made the landing zone
// depend on relative board placement -- the hazard this pin removes).
// The Amiga owns every byte of it: NOTHING on the ARM side may live here.
#define Z3_FASTRAM_DDR_ADDRESS      0x20000000
#define Z3_FASTRAM_DDR_SIZE         0x10000000     // Z3_SIZE_256MB
#define Z3_FASTRAM_DDR_END \
    (Z3_FASTRAM_DDR_ADDRESS + Z3_FASTRAM_DDR_SIZE)

#if Z3_FASTRAM_DDR_ADDRESS < 0x18000000
#error "Z3 fast-RAM window must sit above the linker-managed DDR (ends 0x18000000)"
#endif
#if Z3_FASTRAM_DDR_END > 0x30000000
#error "Z3 fast-RAM window overlaps codec scratch at 0x30000000"
#endif

// Dual-core scheduler task-queue control region. A small SCU-coherent slab in
// the 0x18000000..0x20000000 carve -- above the linker-managed DDR
// (ps7_ddr_hi ends at 0x18000000) and below the Z3 fast-RAM DDR window at
// 0x20000000. Holds the taskq_shared_t control block and, at
// SDK_IMAGE_SESSIONS_OFFSET, the image-stream session table (both cores
// touch session state, and the coherent section removes the need for manual
// cache maintenance on it); crypto data buffers stay in SDK_SHARED_HEAP with
// core-0 cache management.
#define SDK_TASKQ_REGION_ADDRESS    0x18000000
#define SDK_TASKQ_REGION_SIZE       0x00100000     // 1 MB (one MMU section)
#define SDK_TASKQ_REGION_END \
    (SDK_TASKQ_REGION_ADDRESS + SDK_TASKQ_REGION_SIZE)

// Image-stream session table, inside the coherent region above. The offset
// leaves the queue control block its own space (guarded against
// sizeof(taskq_shared_t) in scheduler_arm.c); the table size is guarded in
// sdk_image_stream.c.
#define SDK_IMAGE_SESSIONS_OFFSET   0x00040000
#define SDK_IMAGE_SESSIONS_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_IMAGE_SESSIONS_OFFSET)
#define SDK_IMAGE_SESSIONS_MAX_BYTES \
    (SDK_VIDEO_SESSIONS_OFFSET - SDK_IMAGE_SESSIONS_OFFSET)

// Codec-neutral streaming video sessions. The table contains only session
// metadata and decoder pointers; codec working memory stays on the tracked
// newlib heap owned by core 1. Keeping this small carve immediately before
// the audio table preserves nearly all of the image-session allowance.
#define SDK_VIDEO_SESSIONS_OFFSET   0x000BF000
#define SDK_VIDEO_SESSIONS_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_VIDEO_SESSIONS_OFFSET)
#define SDK_VIDEO_SESSIONS_MAX_BYTES \
    (SDK_MEDIA_SESSION_OFFSET - SDK_VIDEO_SESSIONS_OFFSET)

// Singleton MPEG media-session state. Unlike the decoder graph (which remains
// core-1-owned), this small control block is read by core 0 for explicit
// presentation/status and by the AX audio ISR for card-local PCM playback.
// Keep it in the SCU-coherent slab so its published cursors are visible without
// trying to invalidate another core's dirty L1 lines.
#define SDK_MEDIA_SESSION_OFFSET    0x000BFC00
#define SDK_MEDIA_SESSION_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_MEDIA_SESSION_OFFSET)
#define SDK_MEDIA_SESSION_MAX_BYTES \
    (SDK_MEDIA_PROFILE_OFFSET - SDK_MEDIA_SESSION_OFFSET)

// Per-stage media pipeline timing (U7). Carved from the tail of the media
// session's 1 KB allowance rather than given its own region: it is tiny, and
// it must sit in this same SCU-coherent slab because core 1 accumulates the
// decode and pack stages while core 0 reads them for MEDIA_SESSION_STATUS.
#define SDK_MEDIA_PROFILE_OFFSET    0x000BFF00
#define SDK_MEDIA_PROFILE_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_MEDIA_PROFILE_OFFSET)
#define SDK_MEDIA_PROFILE_MAX_BYTES \
    (SDK_AUDIO_STREAMS_OFFSET - SDK_MEDIA_PROFILE_OFFSET)

// Audio-stream session table, also inside the coherent region: streams may
// be core-1-affine (feed/decode on the worker) while core 0 begins/closes
// them. No heap objects hang off these (the decoder state is embedded), so
// unlike the image sessions they need no fault-reclaim gating. Size guarded
// in sdk_mailbox.c.
#define SDK_AUDIO_STREAMS_OFFSET    0x000C0000
#define SDK_AUDIO_STREAMS_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_AUDIO_STREAMS_OFFSET)
#define SDK_AUDIO_STREAMS_MAX_BYTES \
    (SDK_TASKQ_REGION_SIZE - SDK_AUDIO_STREAMS_OFFSET)

#if SDK_TASKQ_REGION_ADDRESS < 0x18000000
#error "task-queue region must sit above the linker-managed DDR (ends 0x18000000)"
#endif
#if defined(SDK_JEDI_REGION_ADDRESS)
#if SDK_TASKQ_REGION_ADDRESS < (SDK_JEDI_REGION_ADDRESS + SDK_JEDI_REGION_SIZE)
#error "task-queue region overlaps the JEDI region"
#endif
#endif
#if SDK_TASKQ_REGION_END > Z3_FASTRAM_DDR_ADDRESS
#error "task-queue region overlaps the Z3 fast-RAM DDR window"
#endif

// Dedicated core-1 stack (dual-core scheduler). The Cortex-A9 stack is
// full-descending, so the CPU is given the TOP. Reserved in the
// 0x18000000..0x20000000 carve, immediately above the task-queue region and
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
#if SDK_CORE1_STACK_TOP > Z3_FASTRAM_DDR_ADDRESS
#error "core-1 stack overlaps the Z3 fast-RAM DDR window"
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
