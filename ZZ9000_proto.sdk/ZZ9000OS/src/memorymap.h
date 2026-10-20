/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


// Card DDR map: every fixed range the firmware uses is defined in this file,
// and docs/memory-map.md shows the whole 1 GB. Add a new range here first,
// with an #error guard against its neighbours, then document it. Large
// working memory for firmware services comes from the card pool
// (CARD_POOL_* below), not from new fixed carve-outs.

#define AUDIO_NUM_PERIODS           8
#define AUDIO_BYTES_PER_PERIOD      3840

#define FRAMEBUFFER_ADDRESS         0x00200000
// Frame buffer/graphics memory starts at 64KB (relative to card address), leaving ample space for general purpose registers.
#define MNT_FB_BASE     			0x00010000
#define AUDIO_TX_BUFFER_SIZE        (AUDIO_BYTES_PER_PERIOD * AUDIO_NUM_PERIODS)

// Framebuffer and all host-visible runtime allocations begin at the board
// aperture's DDR base. The top 2 MiB is the RTL-pinned peripheral belt.
#define Z3_SCRATCH_ADDR             0x033F0000
#define ADDR_ADJ                    (FRAMEBUFFER_ADDRESS - MNT_FB_BASE)

#define AUDIO_TX_BUFFER_ADDRESS     0x3FE00000
#define AUDIO_RX_BUFFER_ADDRESS     0x3FE20000
#define TX_BD_LIST_START_ADDRESS    0x3FF00000
#define RX_BD_LIST_START_ADDRESS    0x3FF08000
#define TX_FRAME_ADDRESS            0x3FF10000
#define RX_FRAME_ADDRESS            0x3FF20000
#define RX_BACKLOG_ADDRESS          0x3FF30000
#define USB_BLOCK_STORAGE_ADDRESS   0x3FF70000
#define BOOT_ROM_ADDRESS            0x3FEF0000
#define BOOT_ROM_DDR_SIZE           0x00002000
#define ETH_BD_LIST_SIZE            0x00008000
#define ETH_FRAME_BUFFER_SIZE       0x00010000
#define ETH_RX_BACKLOG_SIZE         0x00040000
#define USB_BLOCK_STORAGE_SIZE       0x00006000

// Audio fabric lease-plane card-side source rings (plan U3): one ring per
// leaseable compositor slot. They are hardware-pinned in the top belt.
#define AUDIO_FABRIC_LEASE_RING_PERIODS 32U
#define AUDIO_FABRIC_LEASE_RING_BYTES \
    (AUDIO_FABRIC_LEASE_RING_PERIODS * AUDIO_BYTES_PER_PERIOD)
#define AUDIO_FABRIC_LEASE_RING1_ADDRESS 0x3FE28000
#define AUDIO_FABRIC_LEASE_RING2_ADDRESS \
    (AUDIO_FABRIC_LEASE_RING1_ADDRESS + AUDIO_FABRIC_LEASE_RING_BYTES)
#define AUDIO_FABRIC_LEASE_RINGS_END \
    (AUDIO_FABRIC_LEASE_RING2_ADDRESS + AUDIO_FABRIC_LEASE_RING_BYTES)

#if AUDIO_FABRIC_LEASE_RING1_ADDRESS < \
    (AUDIO_RX_BUFFER_ADDRESS + AUDIO_NUM_PERIODS * AUDIO_BYTES_PER_PERIOD)
#error "fabric lease ring 1 overlaps the audio RX formatter ring"
#endif
#if AUDIO_FABRIC_LEASE_RINGS_END > BOOT_ROM_ADDRESS
#error "fabric lease rings overlap the boot ROM DDR window"
#endif

#if AUDIO_TX_BUFFER_ADDRESS + AUDIO_TX_BUFFER_SIZE > AUDIO_RX_BUFFER_ADDRESS || \
    AUDIO_RX_BUFFER_ADDRESS + AUDIO_TX_BUFFER_SIZE > \
        AUDIO_FABRIC_LEASE_RING1_ADDRESS || \
    AUDIO_FABRIC_LEASE_RINGS_END > BOOT_ROM_ADDRESS || \
    BOOT_ROM_ADDRESS + BOOT_ROM_DDR_SIZE > TX_BD_LIST_START_ADDRESS || \
    TX_BD_LIST_START_ADDRESS + ETH_BD_LIST_SIZE > RX_BD_LIST_START_ADDRESS || \
    RX_BD_LIST_START_ADDRESS + ETH_BD_LIST_SIZE > TX_FRAME_ADDRESS || \
    TX_FRAME_ADDRESS + ETH_FRAME_BUFFER_SIZE > RX_FRAME_ADDRESS || \
    RX_FRAME_ADDRESS + ETH_FRAME_BUFFER_SIZE > RX_BACKLOG_ADDRESS || \
    RX_BACKLOG_ADDRESS + ETH_RX_BACKLOG_SIZE > USB_BLOCK_STORAGE_ADDRESS || \
    USB_BLOCK_STORAGE_ADDRESS + USB_BLOCK_STORAGE_SIZE > 0x40000000
#error "RTL-pinned top-belt ranges overlap"
#endif

/*
 * These legacy scratch addresses are temporary reservations in the unified
 * host allocator until the matched-driver descriptor replacement is active.
 */
#define RTG_TEMPLATE_SCRATCH_ADDRESS 0x03400000
#define RTG_TEMPLATE_SCRATCH_SIZE    0x00100000
#define RTG_TEMPLATE_SCRATCH_END \
    (RTG_TEMPLATE_SCRATCH_ADDRESS + RTG_TEMPLATE_SCRATCH_SIZE)

// The Z3 main-board window holds two allocators that must never share an
// address. P96 manages its own board memory: rtg.library places screens and
// any bitmap the driver's AllocBitMap hook declines anywhere in
// [MemoryBase, MemoryBase + MemorySize), and the firmware never sees those
// allocations. The RTG driver therefore reports exactly
// Z3_P96_VRAM_ADDRESS..Z3_P96_VRAM_END as MemorySize. Everything the firmware
// hands out (off-screen surfaces via ACC_OP_ALLOC_SURFACE, the capture
// backing, SDK shared allocations, GFXData/template scratch, mailbox, direct
// rings, AHI scratch) comes from the host-visible heap above it.
#define Z3_P96_VRAM_ADDRESS         FRAMEBUFFER_ADDRESS
#define Z3_P96_VRAM_END             Z3_SCRATCH_ADDR
#define HOST_VISIBLE_HEAP_ADDRESS   Z3_P96_VRAM_END
#define HOST_VISIBLE_HEAP_END       0x08200000
#define HOST_VISIBLE_HEAP_SIZE \
    (HOST_VISIBLE_HEAP_END - HOST_VISIBLE_HEAP_ADDRESS)

#if Z3_P96_VRAM_END != HOST_VISIBLE_HEAP_ADDRESS || \
    HOST_VISIBLE_HEAP_END != 0x08200000
#error "P96 VRAM and the host-visible heap must tile the aperture below the firmware image"
#endif
#if RTG_TEMPLATE_SCRATCH_ADDRESS < HOST_VISIBLE_HEAP_ADDRESS
#error "RTG template scratch must sit in the firmware host-visible heap"
#endif

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
// without the generation-tagged aperture register. Generation-2 Z2
// bitstreams expose RAM_SIZE through AXI slot 7 and use the window-relative
// regions computed in sdk_aperture_layout.c; firmware publishes that dynamic
// heap only after the host acknowledges the matching contract. The existing
// SDK guard still prevents a legacy 2 MB host from using this 4 MB address.
// Generation 2 carves the top 48 KiB of the 64 KiB fallback heap for
// direct rings. Keep the fallback size intact for Z3 and legacy Z2.
#define SDK_HOST_WINDOW_HEAP_ADDRESS 0x005D0000
#define SDK_HOST_WINDOW_HEAP_SIZE    0x00010000
#define SDK_HOST_WINDOW_HEAP_GEN2_SIZE 0x00004000
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

// SDK shared allocations are drawn from HOST_VISIBLE_HEAP_* together with
// P96 off-screen surfaces. ARM-local SDK surfaces use the card pool.

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
#define Z3_AUDIO_SCRATCH_SIZE           0x00020000
#define Z3_AUDIO_SCRATCH_END \
    (Z3_AUDIO_SCRATCH_ADDRESS + Z3_AUDIO_SCRATCH_SIZE)
#define FIRMWARE_HIGH_DDR_ADDRESS       0x08200000

#if Z3_SCRATCH_ADDR != Z3_P96_VRAM_END
#error "Z3 GFXData scratch must start the firmware host-visible heap"
#endif

#if Z3_AUDIO_SCRATCH_END > HOST_VISIBLE_HEAP_END
#error "Z3 audio scratch must fit the host-visible interval"
#endif

#if Z3_AUDIO_SCRATCH_END > FIRMWARE_HIGH_DDR_ADDRESS
#error "Z3 audio scratch overlaps linker-managed firmware DDR"
#endif

// Audio fabric direct-ring plane (U2): host-visible PCM rings the Amiga
// writes directly -- direct data never travels the sync copy-submit path.
// One 128-byte seqlock control block (SDK_AUDIO_RING_CONTROL_SIZE, two
// 64-byte lines at SDK_AUDIO_RING_CONTROL_ALIGN in sdk_mailbox.h) sits at
// each slot block's base, the ring at +0x80. Slot 0 is the firmware pump
// and keeps the card-side lease rings above; only slots 1..N are granted.
//
// Zorro III direct rings occupy 4 KiB non-cacheable pages in the common
// host-visible interval; matched drivers obtain their address in the mailbox.
#define SDK_AUDIO_DIRECT_RING_Z3_SLOTS              2U
#define SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS    0x081C0000
#define SDK_AUDIO_DIRECT_RING_Z3_RESERVE_SIZE       0x00020000
#define SDK_AUDIO_DIRECT_RING_Z3_RESERVE_END \
    (SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS + \
     SDK_AUDIO_DIRECT_RING_Z3_RESERVE_SIZE)
#define SDK_AUDIO_DIRECT_RING_Z3_SLOT_STRIDE        0x00010000
#define SDK_AUDIO_DIRECT_RING_Z3_PERIODS            16U
#define SDK_AUDIO_DIRECT_RING_Z3_CAPACITY_BYTES \
    (SDK_AUDIO_DIRECT_RING_Z3_PERIODS * AUDIO_BYTES_PER_PERIOD)
#define SDK_AUDIO_DIRECT_RING_Z3_CONTROL1_ADDRESS   0x081C0000
#define SDK_AUDIO_DIRECT_RING_Z3_RING1_ADDRESS      0x081C0080
#define SDK_AUDIO_DIRECT_RING_Z3_CONTROL2_ADDRESS   0x081D0000
#define SDK_AUDIO_DIRECT_RING_Z3_RING2_ADDRESS      0x081D0080

#if SDK_AUDIO_DIRECT_RING_Z3_RESERVE_END != Z3_AUDIO_SCRATCH_ADDRESS
#error "Z3 direct-ring reservation must end at the audio scratch base"
#endif
#if SDK_AUDIO_DIRECT_RING_Z3_CONTROL2_ADDRESS != \
    (SDK_AUDIO_DIRECT_RING_Z3_CONTROL1_ADDRESS + \
     SDK_AUDIO_DIRECT_RING_Z3_SLOT_STRIDE)
#error "Z3 direct-ring slot blocks must be one stride apart"
#endif
// Capacities are whole 3840-byte periods (firmware consumes complete
// periods only).
#if (SDK_AUDIO_DIRECT_RING_Z3_CAPACITY_BYTES % AUDIO_BYTES_PER_PERIOD) != 0
#error "Z3 direct-ring capacity must be a whole number of periods"
#endif
// Each control line owns exactly one 64-byte cache line.
#if (SDK_AUDIO_DIRECT_RING_Z3_CONTROL1_ADDRESS % 64) != 0 || \
    (SDK_AUDIO_DIRECT_RING_Z3_CONTROL2_ADDRESS % 64) != 0 || \
    (SDK_AUDIO_DIRECT_RING_Z3_RING1_ADDRESS % 64) != 0 || \
    (SDK_AUDIO_DIRECT_RING_Z3_RING2_ADDRESS % 64) != 0
#error "Z3 direct-ring control/ring bases must be 64-byte aligned"
#endif
#if (SDK_AUDIO_DIRECT_RING_Z3_RING1_ADDRESS + \
     SDK_AUDIO_DIRECT_RING_Z3_CAPACITY_BYTES) > \
    SDK_AUDIO_DIRECT_RING_Z3_CONTROL2_ADDRESS
#error "Z3 direct-ring slot 1 overlaps slot 2"
#endif
#if (SDK_AUDIO_DIRECT_RING_Z3_RING2_ADDRESS + \
     SDK_AUDIO_DIRECT_RING_Z3_CAPACITY_BYTES) > \
    SDK_AUDIO_DIRECT_RING_Z3_RESERVE_END
#error "Z3 direct-ring slot 2 exceeds the reservation"
#endif

// Zorro II: exactly one compact grant. The generation-2 aperture layout
// (sdk_aperture_layout.c) carves this 48 KiB reservation from the top of
// the old 64 KiB host-window heap, leaving 16 KiB of allocator below it.
// Window-relative base for any Z2 aperture is layout->audio.base minus
// this size (ARM = board + ADDR_ADJ); the constants below pin the 4 MB
// legacy-fallback addresses firmware would use without a runtime layout.
// Z2 grants exist only while the generation-2 contract is acknowledged.
#define SDK_AUDIO_DIRECT_RING_Z2_SLOTS              1U
#define SDK_AUDIO_DIRECT_RING_Z2_RESERVE_SIZE       0x0000C000
#define SDK_AUDIO_DIRECT_RING_Z2_RESERVE_ADDRESS \
    (SDK_HOST_WINDOW_HEAP_ADDRESS + SDK_HOST_WINDOW_HEAP_GEN2_SIZE)
#define SDK_AUDIO_DIRECT_RING_Z2_RESERVE_END \
    (SDK_AUDIO_DIRECT_RING_Z2_RESERVE_ADDRESS + \
     SDK_AUDIO_DIRECT_RING_Z2_RESERVE_SIZE)
#define SDK_AUDIO_DIRECT_RING_Z2_PERIODS            12U
#define SDK_AUDIO_DIRECT_RING_Z2_CAPACITY_BYTES \
    (SDK_AUDIO_DIRECT_RING_Z2_PERIODS * AUDIO_BYTES_PER_PERIOD)
#define SDK_AUDIO_DIRECT_RING_Z2_CONTROL_ADDRESS \
    SDK_AUDIO_DIRECT_RING_Z2_RESERVE_ADDRESS
#define SDK_AUDIO_DIRECT_RING_Z2_RING_ADDRESS \
    (SDK_AUDIO_DIRECT_RING_Z2_CONTROL_ADDRESS + 0x80)

#if SDK_AUDIO_DIRECT_RING_Z2_RESERVE_END != (0x003F0000 + ADDR_ADJ)
#error "Z2 direct-ring reservation must end at the audio scratch base"
#endif
#if (SDK_AUDIO_DIRECT_RING_Z2_CAPACITY_BYTES % AUDIO_BYTES_PER_PERIOD) != 0
#error "Z2 direct-ring capacity must be a whole number of periods"
#endif
#if (SDK_AUDIO_DIRECT_RING_Z2_CONTROL_ADDRESS % 64) != 0 || \
    (SDK_AUDIO_DIRECT_RING_Z2_RING_ADDRESS % 64) != 0
#error "Z2 direct-ring control/ring bases must be 64-byte aligned"
#endif
#if (SDK_AUDIO_DIRECT_RING_Z2_RING_ADDRESS + \
     SDK_AUDIO_DIRECT_RING_Z2_CAPACITY_BYTES) > \
    SDK_AUDIO_DIRECT_RING_Z2_RESERVE_END
#error "Z2 direct-ring slot exceeds the reservation"
#endif
// Deterministic second-slot impossibility: after the single grant, the
// leftover cannot hold even one more control block plus one period. The
// Z2 SDK mailbox owns that leftover (below). If a future layout change
// grows this reservation past that bound, the Z2 slot count must be
// revisited explicitly, never silently granted twice.
#if (SDK_AUDIO_DIRECT_RING_Z2_RESERVE_SIZE - 0x80 - \
     SDK_AUDIO_DIRECT_RING_Z2_CAPACITY_BYTES) >= \
    (0x80 + AUDIO_BYTES_PER_PERIOD)
#error "Z2 direct-ring reservation can admit a second slot; revisit the Z2 slot count"
#endif

// Zorro II SDK v2 mailbox: the 0xB80-byte tail of the direct-ring
// reservation that the single grant leaves unused (its control block and
// ring end at +0xB480). A 128-byte descriptor plus two rings of
// SDK_MAILBOX_Z2_RING_ENTRIES 64-byte entries fills that tail exactly and
// ends at the audio scratch base. Like the grant it is window-relative,
// ARM = layout->audio.base - SDK_MAILBOX_Z2_SIZE + ADDR_ADJ, and it exists
// only while the generation-2 contract is acknowledged.
//
// The Amiga writes the mailbox through non-coherent AXI and the descriptor
// packs host- and firmware-owned cursors into the same cache lines, so the
// ARM must map it non-cacheable. Its 1 MB section also holds VRAM and the
// PIP pool, which the ARM writes cached, so mmu_page.c makes only the 4 KiB
// page directly below the audio scratch base non-cacheable. That page also
// holds the last 0x480 bytes of the grant's ring; the lease code's
// per-range cache maintenance there is harmless on non-cacheable memory.
#define SDK_MAILBOX_Z2_RING_ENTRIES    22U
#define SDK_MAILBOX_Z2_SIZE \
    (0x80U + 2U * SDK_MAILBOX_Z2_RING_ENTRIES * 0x40U)
#define SDK_MAILBOX_Z2_RESERVE_OFFSET \
    (SDK_AUDIO_DIRECT_RING_Z2_RESERVE_SIZE - SDK_MAILBOX_Z2_SIZE)
#define SDK_MAILBOX_Z2_PAGE_SIZE       0x1000U

#if (0x80 + SDK_AUDIO_DIRECT_RING_Z2_CAPACITY_BYTES) > \
    SDK_MAILBOX_Z2_RESERVE_OFFSET
#error "Z2 SDK mailbox overlaps the direct-ring grant"
#endif
#if (SDK_MAILBOX_Z2_RESERVE_OFFSET % 64) != 0
#error "Z2 SDK mailbox must be 64-byte aligned"
#endif
#if SDK_MAILBOX_Z2_SIZE > SDK_MAILBOX_Z2_PAGE_SIZE
#error "Z2 SDK mailbox must fit the non-cacheable page below the audio scratch"
#endif

// Z3 fast-RAM DDR landing window. A matched RTL bitstream maps the 256 MB
// autoconfig PIC here; while advertised the ARM must not allocate this tail.
#define Z3_FASTRAM_DDR_ADDRESS      0x2FE00000
#define Z3_FASTRAM_DDR_SIZE         0x10000000     // Z3_SIZE_256MB
#define Z3_FASTRAM_DDR_END \
    (Z3_FASTRAM_DDR_ADDRESS + Z3_FASTRAM_DDR_SIZE)

// The 4 MiB high-image region ends 1,511,824 bytes above the measured
// 0x0848f670 image/BSS high-water (qualification build 141c121).
#define FIRMWARE_IMAGE_END          0x08600000
#define SDK_TASKQ_REGION_ADDRESS    FIRMWARE_IMAGE_END
#define SDK_TASKQ_REGION_SIZE       0x00100000
#define SDK_TASKQ_REGION_END \
    (SDK_TASKQ_REGION_ADDRESS + SDK_TASKQ_REGION_SIZE)

// Image/session offsets are inside the coherent task-queue section.
#define SDK_IMAGE_SESSIONS_OFFSET   0x00040000
#define SDK_IMAGE_SESSIONS_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_IMAGE_SESSIONS_OFFSET)
#define SDK_IMAGE_SESSIONS_MAX_BYTES \
    (SDK_VIDEO_SESSIONS_OFFSET - SDK_IMAGE_SESSIONS_OFFSET)
#define SDK_VIDEO_SESSIONS_OFFSET   0x000BF000
#define SDK_VIDEO_SESSIONS_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_VIDEO_SESSIONS_OFFSET)
#define SDK_VIDEO_SESSIONS_MAX_BYTES \
    (SDK_MEDIA_SESSION_OFFSET - SDK_VIDEO_SESSIONS_OFFSET)
#define SDK_MEDIA_SESSION_OFFSET    0x000BFC00
#define SDK_MEDIA_SESSION_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_MEDIA_SESSION_OFFSET)
#define SDK_MEDIA_SESSION_MAX_BYTES \
    (SDK_MEDIA_PROFILE_OFFSET - SDK_MEDIA_SESSION_OFFSET)
#define SDK_MEDIA_PROFILE_OFFSET    0x000BFF00
#define SDK_MEDIA_PROFILE_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_MEDIA_PROFILE_OFFSET)
#define SDK_MEDIA_PROFILE_MAX_BYTES \
    (SDK_AUDIO_STREAMS_OFFSET - SDK_MEDIA_PROFILE_OFFSET)
#define SDK_AUDIO_STREAMS_OFFSET    0x000C0000
#define SDK_AUDIO_STREAMS_ADDRESS \
    (SDK_TASKQ_REGION_ADDRESS + SDK_AUDIO_STREAMS_OFFSET)
#define SDK_AUDIO_STREAMS_MAX_BYTES \
    (SDK_TASKQ_REGION_SIZE - SDK_AUDIO_STREAMS_OFFSET)

// Core 1 uses a full-descending stack. Its top is encodable by the naked
// reset stub's single ARM `mov sp, #immediate`.
#define SDK_CORE1_STACK_BASE         SDK_TASKQ_REGION_END
#define SDK_CORE1_STACK_SIZE         0x00100000
#define SDK_CORE1_STACK_TOP \
    (SDK_CORE1_STACK_BASE + SDK_CORE1_STACK_SIZE)

// Measured service-path peak is below 1 MiB after decoder, console, and
// media working sets moved to the card pool. Keep the 8 MiB minimum
// reservation (including exception stacks) as the clear 2x margin.
#define NEWLIB_HEAP_ADDRESS          SDK_CORE1_STACK_TOP
#define NEWLIB_HEAP_SIZE             0x00800000
#define NEWLIB_HEAP_END \
    (NEWLIB_HEAP_ADDRESS + NEWLIB_HEAP_SIZE)

// Card service storage is one physical pool: a permanent prefix and a
// revocable Fast-RAM tail. The tail opens only while Fast RAM is withheld.
#define CARD_POOL_PAGE_SIZE          0x00010000
#define CARD_POOL_PERMANENT_ADDRESS  NEWLIB_HEAP_END
#define CARD_POOL_PERMANENT_END      Z3_FASTRAM_DDR_ADDRESS
#define CARD_POOL_REVOCABLE_ADDRESS  Z3_FASTRAM_DDR_ADDRESS
#define CARD_POOL_REVOCABLE_END      Z3_FASTRAM_DDR_END

// Complete target partition. Every interval is half-open and each end starts
// its successor; changing a boundary without its neighbour stops the build.
#define DDR_ADDRESS_START            0x00000000
#define DDR_LOW_APERTURE_END         0x00100000
#define FIRMWARE_LOW_DDR_ADDRESS     DDR_LOW_APERTURE_END
#define FIRMWARE_LOW_DDR_END         FRAMEBUFFER_ADDRESS
#define DDR_TOP_BELT_ADDRESS         AUDIO_TX_BUFFER_ADDRESS
#define DDR_ADDRESS_END              0x40000000

#if DDR_ADDRESS_START != 0x00000000 || DDR_ADDRESS_END != 0x40000000
#error "DDR target partition must cover exactly 1 GiB"
#endif
#if DDR_LOW_APERTURE_END != FIRMWARE_LOW_DDR_ADDRESS || \
    FIRMWARE_LOW_DDR_END != Z3_P96_VRAM_ADDRESS || \
    Z3_P96_VRAM_END != HOST_VISIBLE_HEAP_ADDRESS || \
    HOST_VISIBLE_HEAP_END != FIRMWARE_HIGH_DDR_ADDRESS || \
    FIRMWARE_IMAGE_END != SDK_TASKQ_REGION_ADDRESS || \
    SDK_TASKQ_REGION_END != SDK_CORE1_STACK_BASE || \
    SDK_CORE1_STACK_TOP != NEWLIB_HEAP_ADDRESS || \
    NEWLIB_HEAP_END != CARD_POOL_PERMANENT_ADDRESS || \
    CARD_POOL_PERMANENT_END != CARD_POOL_REVOCABLE_ADDRESS || \
    CARD_POOL_REVOCABLE_END != DDR_TOP_BELT_ADDRESS || \
    Z3_FASTRAM_DDR_END != DDR_TOP_BELT_ADDRESS
#error "DDR target partition has a gap or overlap"
#endif
#if (CARD_POOL_PERMANENT_ADDRESS | CARD_POOL_PERMANENT_END | \
     CARD_POOL_REVOCABLE_ADDRESS | CARD_POOL_REVOCABLE_END) & \
    (CARD_POOL_PAGE_SIZE - 1)
#error "card pool bounds must be 64 KiB aligned"
#endif
#if CARD_POOL_PERMANENT_ADDRESS >= CARD_POOL_PERMANENT_END || \
    CARD_POOL_REVOCABLE_ADDRESS >= CARD_POOL_REVOCABLE_END
#error "card pool range is empty or inverted"
#endif
#if SDK_CORE1_STACK_TOP != 0x08800000
#error "core-1 stack top must remain a single-instruction ARM immediate"
#endif

// SDK v2 bootstrap mailbox (descriptor plus both rings; firmware publishes
// the chosen ARM address through REG_ZZ_SDK_MAILBOX_HI/LO and the Amiga
// library maps whatever it reads). The placement is per bus; neither bus
// uses the 0xa000..0xffff shared I/O buffer, which USB proxy transfers,
// zzsd block I/O and firmware-update chunks overwrite (issue #129).
//
// Zorro III: a 64 KiB control reservation inside the common host-visible
// allocator. main.c reserves it before any P96/SDK client allocation. It
// shares the second-level non-cacheable mapping with the direct-ring pages.
//
// Zorro II: the tail of the direct-ring reservation (SDK_MAILBOX_Z2_* above).
#define SDK_MAILBOX_Z3_ADDRESS      0x08100000
#define SDK_MAILBOX_Z3_RESERVE_SIZE 0x00010000
#define SDK_MAILBOX_Z3_RESERVE_END \
	(SDK_MAILBOX_Z3_ADDRESS + SDK_MAILBOX_Z3_RESERVE_SIZE)

#if SDK_MAILBOX_Z3_ADDRESS < HOST_VISIBLE_HEAP_ADDRESS || \
    SDK_MAILBOX_Z3_RESERVE_END > HOST_VISIBLE_HEAP_END
#error "Z3 SDK mailbox must fit the common host-visible allocator"
#endif
#if SDK_MAILBOX_Z3_RESERVE_END > SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS
#error "Z3 SDK mailbox overlaps the Z3 direct-ring reservation"
#endif
#if (SDK_MAILBOX_Z3_ADDRESS & ~0x000FFFFF) != \
    (SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS & ~0x000FFFFF)
#error "Z3 SDK mailbox must share the non-cacheable direct-ring MMU section"
#endif
#if (SDK_MAILBOX_Z3_RESERVE_END - ADDR_ADJ) > Z3_MAIN_BOARD_WINDOW_SIZE
#error "Z3 SDK mailbox lies outside the Zorro III main board aperture"
#endif
#define RX_FRAME_PAD 4
#define FRAME_SIZE 2048

// Our address space is relative to the autoconfig base address (for example, it could be 0x600000)
#define MNT_REG_BASE    			0x00000000

// 0x2000 - 0x7fff   ETH RX
// 0x8000 - 0x9fff   ETH TX
// 0xa000 - 0xffff   shared IO buffer (legacy USB block window)
