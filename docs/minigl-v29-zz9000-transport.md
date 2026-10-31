<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 MiniGL Transport Specification (Wire Protocol v9 / minigl.library 29.1)

## 1. Overview and Architectural Convergence

### 1.1 Goal
This specification defines the hardware transport contract connecting the converged AmigaOS 68k `minigl.library` (v29.1, dispatch ABI 5, 205 vectors) to the MNT ZZ9000 graphics and coprocessor card.

Under the MiniGL convergence decision:
1. **One Guest Library**: The Amiga uses a single `minigl.library` binary across both software emulation (Amiberry) and real hardware (ZZ9000). The library speaks wire protocol version 9 (`MW_VERSION = 9`).
2. **Dual Transport**: At runtime, `minigl.library` selects between the Amiberry hypervisor trap (Trap 120 via `uae.resource`) and the physical ZZ9000 card.
3. **Independent GPL-3.0-or-later Firmware Implementation**: The ZZ9000 ARM firmware implements a cleanroom wire-v9 front end that dispatches commands to the existing ZZ9000 rendering pipeline (FPGA RasterIX hardware acceleration with software rasterizer fallback). The firmware declares its own structures and constants from this protocol specification; it does not copy or incorporate any PolyForm-Noncommercial source code (including `minigl_wire.h`).
4. **Clean Cutover**: The legacy private MGL5 ABI (compact tokens, 64-byte vertex records, 124-byte state snapshots) is retired. Standard OpenGL 1.1 tokens and wire-v9 packed draws are used natively.

### 1.2 System Architecture

```
+-------------------------------------------------------------------------+
| AmigaOS 68k Application (e.g. GLQuake v29)                             |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| minigl.library 29.1 (Dispatch ABI 5, Standard GL Tokens)                |
| - Query mirror & state cache                                            |
| - Packed vertex encoder (MW_DRAW_PACKED: 24-32 bytes/vertex)            |
| - Transport Abstraction VTable                                          |
+-------------------------------------------------------------------------+
         |                                                 |
         | (Precedence 1: UAE Trap 120)                    | (Precedence 2: ConfigDev)
         v                                                 v
+---------------------------+             +-------------------------------+
| Amiberry Trap Transport   |             | ZZ9000 Hardware Transport     |
| (uae.resource, host GPU)  |             | - Card-side double-buffer ring|
|                           |             | - 68k CacheClearE maintenance |
|                           |             | - MNTZorro early-DTACK doorbell|
+---------------------------+             +-------------------------------+
                                                           |
====================== Zorro II / Zorro III Bus ===========|===============
                                                           v
                                          +-------------------------------+
                                          | FPGA / MNTZorro Logic         |
                                          | - Early DTACK on doorbell     |
                                          | - Direct register aperture    |
                                          +-------------------------------+
                                                           |
                                                           v
                                          +-------------------------------+
                                          | ARM Core 0: Housekeeping/Zorro|
                                          | - ConfigDev registers         |
                                          | - Passes doorbell to Core 1   |
                                          +-------------------------------+
                                                           |
                                                           v
                                          +-------------------------------+
                                          | ARM Core 1: MiniGL Service    |
                                          | - Wire-v9 packet parser       |
                                          | - Lighting, clipping, setup   |
                                          | - Direct P96 VRAM blit        |
                                          +-------------------------------+
                                             /                         \
                                            v                           v
                             +------------------------+  +-----------------------+
                             | PL: RasterIX 3D Engine |  | ARM Software Raster   |
                             | (RGB565 / Z16, TMU0)   |  | (Fallback / Reference)|
                             +------------------------+  +-----------------------+
```

---

## 2. Discovery and Precedence

### 2.1 Precedence Rule
When `minigl.library` initializes (`MGLInit()` or library open), it must determine the active backend. The precedence order is strict:

1. **Amiberry Hypervisor Trap (Trap 120)**:
   - The library queries `OpenResource("uae.resource")`.
   - If present and `rom_base` is non-NULL, it tests trap selector 120 with opcode 0 (`BRIDGE_QUERY`).
   - If the call succeeds and returns `MW_VERSION` (9), the library binds the Amiberry transport and returns immediately.
   - *Rationale*: An emulated Amiga in Amiberry may have an emulated ZZ9000 RTG board attached, but Amiberry does not emulate the dual-core ARM Cortex-A9 CPU or run ZZ9000 firmware. If the ZZ9000 were probed first in an emulator, the library would find the board but freeze or fail when signaling the absent ARM. Furthermore, Trap 120 connects directly to host OpenGL/Vulkan, delivering maximum host performance.

2. **ZZ9000 Physical Hardware Transport**:
   - Only if Trap 120 is absent, unconfigured (`native_code=false`), or returns version 0 / mismatch, the library proceeds to probe for a ZZ9000 card.

### 2.2 ZZ9000 Board Probing
The library probes for the ZZ9000 using `expansion.library`:

```c
struct ConfigDev *cd = NULL;
/* Probe Zorro III first (Product 4), then Zorro II (Product 3) */
cd = FindConfigDev(NULL, 0x6d6e, 4);
if (!cd) {
    cd = FindConfigDev(NULL, 0x6d6e, 3);
}
```

- **Manufacturer ID**: `0x6d6e` (`ZZ9K_MNT_MANUFACTURER`, ASCII `'mn'`).
- **Product ID**: `4` for Zorro III (`ZZ9K_PRODUCT_Z3`), `3` for Zorro II (`ZZ9K_PRODUCT_Z2`).
- **Board Base Address**: `cd->cd_BoardAddr`.
- **Board Size**: `cd->cd_BoardSize` (128 MiB on Z3; 2 MiB, 4 MiB, or 8 MiB on Z2).

### 2.3 Side-Effect-Free Capability & Version Inspection
To confirm the board runs firmware capable of Wire Protocol v9 without triggering side effects or perturbing other services:

1. **SDK Magic Register Probe**:
   Read 16-bit register at board offset `0x0100` (`ZZ9K_REG_SDK_MAGIC`).
   - Must return `0x5a39` (`ZZ9K_REG_SDK_MAGIC_VALUE`).
2. **SDK ABI Version Probe**:
3. **SDK Mailbox Location**:
   Read the 16-bit register pair at board offsets `0x0104`/`0x0106`
   (`REG_ZZ_SDK_MAILBOX_HI/LO`). The combined 32-bit value is the ARM
   address of the SDK mailbox reservation; its board offset is
   `arm − 0x001F0000`. A zero or out-of-aperture value means the firmware
   has no mailbox (Zorro II, or an unacknowledised generation-2 aperture):
   the MiniGL service is unavailable and the transport fails its probe.
4. **Transport Allocation Descriptor**:
   At mailbox board offset + `0x8000` the firmware publishes a versioned
   **big-endian** descriptor (magic `'MGTD'`, `descriptor_version` 1)
   holding the transport window address/size, `address_adjust`, the
   control/status/slot/reply offsets and capacities, the required
   alignment, cache rules, the BLIT/CAPTURE VRAM limit, and an allocation
   generation. The library must reject an absent, invalid, or
   newer-version descriptor and fall back — it must never compute
   addresses by arithmetic on a fixed base. The live control block may
   narrow the descriptor geometry (a constrained session) but never
   widen it.
5. **MiniGL Wire-v9 Capability Bits**:
   Firmware advertises Wire-v9 support and optional transport extensions in
   `ZZ9KMiniGLStatus.capabilities`:
   - `#define ZZ9K_CAP_MINIGL_WIRE_V9 (1U << 18)`
   - `#define ZZ9K_CAP_MINIGL_LATEST_READBACK_ROWS (1U << 19)`
     makes Op 3 use `op_arg1 = first_row`, `op_arg2 = row_count` and return
     top-row-first RGBA8 rows through the bounded reply window.
   - `#define ZZ9K_CAP_MINIGL_ASYNC_PRESENT (1U << 20)` permits the additive
     `MW_PRESENT_SPAN` packet command. A library MUST emit it only when this
     bit is observed; otherwise it uses the compatible synchronous Op 6 path.
     An old library ignores the bit and continues using Op 6, while a new
     library paired with old firmware sees no bit and does the same.
     Without bit 19, the library uses the compatible `MW_READ_PIXELS` path.
6. **Transport Handshake (`BRIDGE_QUERY`)**:
   The library inspects the MiniGL Control Block at the descriptor's
   address:
   - `magic` must equal `0x4D474C39UL` (ASCII `'MGL9'`).
   - `wire_version` must equal `9` (`MW_VERSION`).
   - If `wire_version != 9`, initialization aborts with `GL_FALSE`.

### 2.4 Firmware residency (2.9.0)

The wire-v9 service is **resident** firmware (core-1 worker owned), not a
`ZZ9K/MINIGL.ZZM` module: module runtime v2 has no core-1/jobs execution
contract (`docs/modules.md` reserves the jobs service), no surface-allocator
memory class for a host-visible transport window, and no cross-core
doorbell/unload ordering. The manifest slot stays reserved for a future
module once those runtime features exist.
## 3. Shared Memory Architecture (Zorro III and Zorro II)

### 3.1 Address Mapping
Card DDR is mapped to the Amiga Zorro bus through an address adjustment:
- ARM DDR Framebuffer Base: `0x00200000` (`FRAMEBUFFER_ADDRESS`)
- Board Aperture Offset: `0x00010000` (`MNT_FB_BASE`, 64 KiB register headroom)
- Address Adjustment Constant: `ADDR_ADJ = (0x00200000 - 0x00010000) = 0x001F0000`
- `Board_Offset = ARM_Address - 0x001F0000`
- `ARM_Address = Board_Offset + 0x001F0000`


### 3.2 Transport Window Layout (2.9.0)
On Zorro III the transport is **one dynamic allocation** from the firmware
host-visible heap (`surface_allocator_alloc_range`, 0x91000 bytes, 4 KiB
aligned), constrained to `0x08110000..0x081C0000` — the free gap between
the SDK mailbox and direct-ring reservations inside the 1 MiB MMU section
the mailbox already maps Normal non-cacheable. The window's exact address
is published in the transport descriptor (§2.3); nothing computes it from
a fixed base. Offsets inside the window:

| Region | Window offset | Size | Ownership | Purpose |
|---|---|---|---|---|
| **Control Block** | `0x0000` | 128 B | FW (0..63) / Host (64..127) | Ring cursors, fences, sequence numbers |
| **Status Block** | `0x0080` | 128 B | FW (0..63) / Host (64..127) | Heartbeat, caps, error state, telemetry |
| **Packet Slot 0** | `0x1000` | 256 KiB | Host producer, FW consumer | Wire-v9 command packet submission buffer 0 |
| **Packet Slot 1** | `0x41000` | 256 KiB | Host producer, FW consumer | Wire-v9 command packet submission buffer 1 |
| **Reply Buffer** | `0x81000` | 64 KiB | FW producer, Host consumer | Staging buffer for bounded REQUEST / READ_PIXELS |

*Note*: Render targets, depth buffers, texture storage, and draw scratch
are card-pool allocations (`CARD_POOL_CLASS_MINIGL`, 128 MiB class limit)
created with the context and released on destroy/fault/reset. Texture tables,
pixels, indexed data, and mip chains share one 24 MiB context arena, so they
consume one card-pool allocation record rather than one record per level.
They consume no host-visible heap at firmware boot.
### 3.3 Zorro II: service unavailable
Zorro II cannot host the transport: the shared host window is 64 KiB total
(16 KiB for clients on generation-2 bitstreams once the audio direct-ring
grant takes its 48 KiB), while even a single-slot layout needs 68 KiB
(control page + 64 KiB slot + 16 KiB reply), and Z2 has no SDK mailbox
reservation to publish the descriptor through. The firmware therefore
binds no window, publishes no descriptor, and reports the service
unavailable; the library's probe fails with
`ZZ9K_MINIGL_PROBE_MAILBOX_REGISTERS`/`_TRANSPORT_DESCRIPTOR` and the
backend falls back exactly as on firmware built without the service.

### 3.4 Ownership Blocks and Alignment

**Byte order**: every multi-byte field in the Control and Status Blocks is
a big-endian wire value, matching the transport descriptor and the wire
packet framing. The little-endian ARM firmware serializes them with
byte-order-explicit stores; the 68k library reads and writes them
natively.

Cache line sharing between the 68k (up to 16 bytes on 68040/060; 64 bytes for bursting) and ARM Cortex-A9 (32-byte L1, 32-byte L2) presents false-sharing risks.
- **Rule**: All shared control structures are divided into independent 64-byte blocks.
- **Firmware Block (Bytes 0..63)**: Written only by ARM; read-only to 68k.
- **Host Block (Bytes 64..127)**: Written only by 68k; read-only to ARM.
- Flushing a dirty line on one side never overwrites concurrent progress fields on the other.

```c
/* Control Block: Exactly 128 bytes, 64-byte ownership partition */
typedef struct ZZ9KMiniGLControlBlock {
    /* Bytes 0..63: Firmware Ownership (Firmware writes, host reads) */
    uint32_t magic;              /* 0x4D474C39 ('MGL9') */
    uint16_t transport_version;  /* Transport revision (1) */
    uint16_t wire_version;       /* Wire protocol version (9) */
    uint32_t consumer_seq;       /* Retired packet sequence */
    uint32_t completion_fence;   /* Completed frame/draw fence */
    uint32_t slot_capacity;      /* Max packet size (256K on Z3, 64K on Z2) */
    uint32_t slot_count;         /* Number of packet slots (1 or 2) */
    uint32_t firmware_heartbeat; /* Incremented by Core 1 service loop */
    uint32_t op_status;          /* Return status for synchronous ops */
    uint32_t op_reply_bytes;     /* Actual reply bytes written */
    uint32_t op_done_seq;        /* Echoed op_seq indicating completion */
    uint32_t op_reply_handle;    /* Context handle returned by CREATE */
    uint8_t  firmware_reserved[20];

    /* Bytes 64..127: Host 68k Ownership (Host writes, firmware reads only) */
    uint32_t producer_seq;       /* Submitted packet sequence */
    uint32_t active_slot;        /* Index of slot being submitted */
    uint32_t host_generation;    /* Current host session generation */
    uint32_t op_code;            /* 0=none/submit, 1=create, 3=latest rows, 4=destroy, 5=request, 6=blit */
    uint32_t op_handle;          /* Context handle */
    uint32_t op_arg1;            /* Width / request bytes / Op 3 first row */
    uint32_t op_arg2;            /* Height / card offset / Op 3 row count */
    uint32_t op_arg3;            /* Reply capacity / span total */
    uint32_t op_seq;             /* Sequence counter incremented on each synchronous op */
    uint8_t  host_reserved[28];
} __attribute__((packed, aligned(64))) ZZ9KMiniGLControlBlock;

#### Synchronous Operation Completion Handshake:
1. The host sets `op_code`, `op_handle`, `op_arg1..3`, and increments `op_seq` in the Host Block (bytes 64..127).
2. The host flushes bytes 64..127 (`CacheClearE`) and pulses the doorbell register.
3. Firmware observes `op_seq != op_done_seq`, executes the requested operation, and publishes completion results (`op_status`, `op_reply_bytes`, `op_reply_handle`, and `op_done_seq = op_seq`) strictly in the Firmware Block (bytes 0..63).
4. The host polls until `op_done_seq == op_seq`, invalidating ONLY the 64-byte Firmware Block on each retry.
5. Firmware NEVER stores to bytes 64..127. This eliminates false sharing, dirty cache-line collisions, and race conditions between 68040/060 copyback caches and the ARM Cortex-A9.
/* Status Block: Exactly 128 bytes, 64-byte ownership partition */
typedef struct ZZ9KMiniGLStatus {
    /* Bytes 0..63: Firmware Ownership */
    uint32_t generation;                  /* Session generation counter */
    uint32_t error;                       /* Error code (0 none, 1 packet, 2 resource, 3 reset) */
    uint32_t error_sequence_hi;           /* High 32-bit sequence of last error */
    uint32_t error_sequence_lo;           /* Low 32-bit sequence of last error */
    uint32_t firmware_epoch;              /* Epoch sequence counter */
    uint32_t heartbeat;                   /* Incremented by Core 1 service loop */
    uint32_t capabilities;                /* Capability bitmask (bit 18: Wire v9) */
    uint32_t trace_frame;
    uint32_t trace_transform_clip_ticks;
    uint32_t trace_raster_ticks;
    uint32_t trace_present_ticks;
    uint32_t trace_idle_ticks;
    uint32_t trace_clock_hz;
    uint32_t trace_draws_hardware;
    uint32_t trace_draws_software;
    uint32_t trace_fallback_reason;

    /* Bytes 64..127: Host 68k Ownership */
    uint32_t host_generation;             /* Acknowledged host generation */
    uint32_t producer_epoch;              /* Host producer epoch */
    uint32_t trace_enabled;               /* Host trace enable flag */
    uint8_t  host_reserved[52];
} __attribute__((packed, aligned(64))) ZZ9KMiniGLStatus;
```

### 3.5 Cache Rules & Coherence Maintenance

#### Amiga 68k Side:
- 68020/68030 processors have no copyback data cache; stores go straight to the bus.
- 68040 and 68060 processors have copyback data caches. Stores to card memory can remain in CPU cache lines until flushed.
- **Before Ringing Doorbell**: The 68k library MUST push dirty cache lines to DDR:
  ```c
  CacheClearE((APTR)packet_address, packet_bytes, CACRF_ClearD);
  ```
- **Before Reading Status or Reply**: The 68k library MUST invalidate its cache to discard stale lines:
  ```c
  CacheClearE((APTR)status_address, sizeof(status), CACRF_ClearD);
  ```
- Memory barriers: An explicit compiler barrier (`__asm__ volatile ("" ::: "memory")`) follows all cache sync operations.

#### ARM Cortex-A9 Side:
- The transport window lives inside the SDK mailbox MMU section, which the firmware maps **Normal Non-Cacheable** (`NORM_NONCACHE`) at mailbox placement time (`Xil_SetTlbAttributes`); the descriptor publishes this in `cache_rules`.
- Packet slots and reply buffers in DDR are cacheable by ARM for fast command parsing and SIMD operations:
  - **Before Parsing a Submitted Packet**: Core 1 invalidates the packet range in its data cache so it reads fresh DDR contents written by the 68k over Zorro:
    ```c
    Xil_DCacheInvalidateRange((INTPTR)packet_address, packet_bytes);
    ```
  - **After Writing In-Place Reply or VRAM Frame**: Core 1 cleans (flushes) dirty lines back to DDR so the 68k or FPGA video scanout observes them:
    ```c
    Xil_DCacheFlushRange((INTPTR)target_address, target_bytes);
    ```
  - Data memory barriers (`dmb()`, `dsb()`) are executed before updating consumer sequence numbers.

---

## 4. Submission Mechanism & Flow Control

### 4.1 Wire-v9 Packet Framing
Command packets are submitted in the wire-v9 format:
- **Packet Header (16 bytes)**:
  - `0x00`: Magic `0x414D3344` (`MW_MAGIC`, big-endian ASCII `'AM3D'`).
  - `0x04`: Wire Version `9` (`MW_VERSION`).
  - `0x08`: Total packet length in bytes (including 16-byte header).
  - `0x0C`: Command count ($1 \le N \le 4096$).
- **Commands**: Consecutive 8-byte command headers (`uint32_t opcode`, `uint32_t payload_bytes`) followed by 4-byte-aligned payload data.
- **Maximum Limits**: 256 KiB per packet on Z3 (64 KiB on Z2), 4096 commands per packet, 8192 vertices per draw.

### 4.2 Packet Slot Ring and Flow Control
To minimize bus turnaround latency, the transport uses **double-buffered packet slots** (Slot 0 and Slot 1) on Z3:
1. **Producer (68k)**:
   - Sits at `producer_seq`.
   - Selects slot `slot_idx = producer_seq % slot_count`.
   - Flow control check:
     $$\text{producer\_seq} - \text{consumer\_seq} < \text{slot\_count}$$
   - If both slots are occupied, the 68k enters a bounded fast poll on `consumer_seq` (see Section 4.4).
   - Writes packet into Slot `slot_idx`.
   - Clears 68k data cache for that slot.
   - Updates `g_ctrl->producer_seq = ++producer_seq`.
   - Pulses doorbell register.
2. **Consumer (ARM Core 1)**:
   - Polls `g_ctrl->producer_seq` in its worker loop (woken by doorbell flag).
   - While `consumer_seq < producer_seq`:
     - Reads Slot `consumer_seq % slot_count`.
     - Validates framing (see Section 4.3).
     - Executes commands (transform, RasterIX rendering, state updates).
     - Updates `g_ctrl->consumer_seq = ++consumer_seq`.
     - Updates `g_ctrl->completion_fence`.

   Consumer guard rails (firmware 2.9.x hardening, from the A4000 boot-loop
   postmortem: the window is a raw host-visible heap allocation, and
   uninitialized control memory read as `producer_seq = 0xffffffff`, which
   the worker drained one rejected packet at a time with an unthrottled
   diagnostic print per packet, flooding the shared polled UART and
   starving core 0's console while the Amiga waited on the bus):
   - **Quiescent binding**: `mw_service_bind_region` zeroes both ownership
     halves of the control and status blocks before publishing, so a fresh
     window presents `producer_seq = op_seq = 0` ("no host has opened the
     transport yet") whatever the DDR last held.
   - **Absorbing re-init**: `mw_service_init` (core-1 start/restart,
     transport reset) adopts `consumer_seq = producer_seq` and completes a
     pending synchronous op with `CONTEXT_LOST`, so retired slots are never
     replayed as a phantom backlog.
   - **Bounded drain**: one `mw_service_poll` call drains at most 64 ring
     packets, and a gap larger than 1024 -- impossible for a compliant
     host, which can be at most `slot_count` packets ahead -- is treated as
     destroyed control memory and resynchronized (`consumer_seq =
     producer_seq`) in a single step.
   - **Throttled rejects**: reject diagnostics log the first rejection and
     then one line per 1024 with the suppressed count; a bad transport can
     never monopolize the UART FIFO shared with core 0.

### 4.3 Doorbell Signaling via MNTZorro
- The 68k signals the card by writing to the 16-bit doorbell register:
  - Zorro III Direct Register: `cd_BoardAddr + 0x1108` (`ZZ9K_REG_SDK_DOORBELL`).
  - Zorro II Register: `cd_BoardAddr + 0x0108`.
- In `mntzorro.v`, writes to `SDK_REG_DOORBELL` trigger **early DTACK**:
  - The FPGA asserts DTACK immediately upon latching the register write, without waiting for the ARM to poll or acknowledge.
  - The 68k completes the write in 1 Zorro bus cycle (~150 ns on Z3).
- The FPGA sets `MNTZORRO_STATUS_SDK_DOORBELL`.
- Core 0 receives the event and sets `sh->minigl_doorbell = 1`.
- Core 1 wakes immediately from its 10 µs idle pause and processes the pending packet.

### 4.4 Accepted vs Retired Semantics
To match the performance of the Amiberry in-process submit, `BRIDGE_SUBMIT` does **not** stall the 68k waiting for the entire packet to be rasterized:

```
68k Library (amiberry_flush):
    [ Assemble Packet ]
    [ Write Slot ]
    [ Cache Flush ]
    [ Ring Doorbell ] -----> (MNTZorro Early DTACK)
    [ Check Accept ] <----- Fast handshake: Framing valid? Slot queued?
    RETURN SUCCESS (0)
           |
    (68k immediately continues preparing next draw calls)
           |
           +---------------------> ARM Core 1:
                                   [ Parse & Validate ]
                                   [ Vertex Transform & Setup ]
                                   [ RasterIX Draw / Binning ]
                                   [ Advance completion_fence ]
```

1. **Accepted (Synchronous return from `BRIDGE_SUBMIT`)**:
   - The transport checks:
     - Is the hardware initialized and generation matched?
     - Is the packet header valid (magic, version, size)?
     - Is a slot free?
   - If yes: returns `0` (`SUCCESS`) immediately to `amiberry_flush()`.
   - If framing error: returns `0xfffffffe` (`INVALID`).
   - If ring full and timeout expires: returns `0xfffffffd` (`CAPACITY`).
   - If card reset occurred: returns `0xfffffffc` (`CONTEXT_LOST`).
2. **Retired (Asynchronous Fence Advance)**:
   - Rendering executes asynchronously on Core 1 / RasterIX.
   - When `MW_PRESENT_SPAN` capability is negotiated, one packet-owned
     presentation is in flight by default. The 68k returns after the packet
     is accepted, not after the RasterIX fence or P96 write. A build for a
     verified double-buffered screen MAY set `MINIGL_ASYNC_PRESENT_FRAMES=2`;
     the library MUST otherwise retire the oldest presentation fence before
     queuing another. `glFinish`, readback, `MGLLockBack`, context destruction,
     target/mode changes, and synchronous fallback presentation all retire
     outstanding fences before observing or reusing their result.

Wire framing is validated before execution. A valid command that encounters a
GL-level condition (for example an incomplete texture, an allocation failure,
or an unavailable raster upload) records the context's first GL error and
skips that command; it does **not** reject the packet or prevent its later
commands from retiring. Only malformed wire framing may reject a packet.
### 4.5 Drop-on-Failure Rule (No Resubmit Storms)
If a packet fails submission or execution (returns non-zero status):
- The 68k guest library **MUST immediately reset the packet buffer** (`packet_reset(context)`), resetting `packet_bytes = 16` and `command_count = 0`.
- The library flags `context->state_resync_needed = GL_TRUE`.
- On the next valid command, the library resends complete state keys from its local shadow state. Texture image data from the dropped packet is discarded and not re-sent.
- *Rule*: A rejected packet is NEVER re-submitted. Retrying a rejected packet produces infinite submit storms and hangs the application.

---

### 5.1 Transport Operation Table
The ZZ9000 transport implements the 7 standard bridge operations matching Amiberry trap 120:

| Opcode | Bridge Operation | Parameters | Description |
|---|---|---|---|
| `0` | `BRIDGE_QUERY` | None | Returns wire version (must be 9) |
| `1` | `BRIDGE_CREATE` | `width`, `height` | Allocates renderer context; returns handle |
| `2` | `BRIDGE_SUBMIT` | `handle`, `packet`, `bytes` | Submits wire-v9 command packet |
| `3` | `BRIDGE_READBACK` | `handle`, `first_row`, `row_count` | Capability-gated latest-frame RGBA8 row slices |
| `4` | `BRIDGE_DESTROY` | `handle` | Destroys context and releases card memory |
| `5` | `BRIDGE_REQUEST` | `handle`, `req`, `req_b`, `desc` | Synchronous request with bounded reply |
| `6` | `BRIDGE_BLIT` | `handle`, `req`, `req_b`, `desc` | In-place reply: presentation into P96 bitmap |

### 5.2 Packet-Owned Asynchronous Presentation (`MW_PRESENT_SPAN`)
When capability bit 20 is negotiated, `MGLSwitchDisplay` places
`MW_PRESENT_SPAN` immediately after `MW_PRESENT` in the submitted ring packet.
Its payload is the complete `MW_BLIT` request (format, pitch, origin and
clip rects), followed by the board-relative P96 destination offset and its
validated span length. The worker executes it in packet order, so the
destination description remains owned by the slot until `completion_fence`
retires that packet. It writes directly to card VRAM; no reply buffer or Op 6
handshake is involved. The worker flushes the completed span before publishing
its fence. Unsupported, clipped, non-card, or unaligned targets retain the
synchronous Op 6/readback fallbacks.

### 5.3 Synchronous Requests (`BRIDGE_REQUEST` / Op 5)

`BRIDGE_REQUEST` executes synchronous queries that return data to the guest:
- Parameter `desc` is a pointer to an 8-byte descriptor:
  - Word 0: `reply_ptr` (guest memory address).
  - Word 1: `reply_bytes` (maximum reply capacity).
- **Staging**:
  - If `reply_ptr` points to card memory inside the host-visible window, the ARM writes the reply directly into the destination and executes `Xil_DCacheFlushRange()`.
  - If `reply_ptr` is in Amiga motherboard FastRAM, the ARM writes into the card-side Reply Buffer (`0x07081000` on Z3). The 68k transport waits for completion, then copies the reply from card DDR to `reply_ptr` using `CopyMem()`.

### 5.4 Latest-Frame Readback (`BRIDGE_READBACK` / Op 3)
With capability bit 19, Op 3 reads `row_count` rows starting at `first_row`
from `target.latest`, produces top-row-first RGBA8 in the Reply Buffer, and
returns the exact byte count in `op_reply_bytes`. Each request must fit the
bounded Reply Buffer. This permits presentation and screenshots to select the
latest rendered frame without depending on GL read-buffer state.

Without bit 19, the library uses bounded `MW_READ_PIXELS` requests while
explicitly selecting and restoring the read buffer.

### 5.5 In-Place Presentation (`BRIDGE_BLIT` / Op 6)
Frame presentation (`MGLSwitchDisplay()`) issues `BRIDGE_BLIT` with request opcode `MW_BLIT`.
The request carries:
- Destination RGB format (`RGBFTYPE` 2..13)
- Destination byte pitch
- Origin offset $(X, Y)$ where frame pixel $(0,0)$ lands
- Array of active, unobscured ClipRects ($x_0, y_0, x_1, y_1$)
- Reply descriptor pointing to the locked Picasso96 bitmap memory span.

#### Direct VRAM Blit (Fast Path - Default):
When the target window resides on a Picasso96 screen displayed by the ZZ9000:
1. **Address Detection**:
   The 68k transport checks whether `bitmap_base` falls within the ZZ9000 board range:
   $$\text{cd\_BoardAddr} + 0x00010000 \le \text{bitmap\_base} < \text{cd\_BoardAddr} + \text{VRAM\_END}$$
2. **Card Offset Translation**:
   $$\text{card\_offset} = \text{bitmap\_base} - \text{cd\_BoardAddr}$$
   $$\text{arm\_vram\_addr} = \text{FRAMEBUFFER\_ADDRESS} + (\text{card\_offset} - \text{MNT\_FB\_BASE}) = \text{card\_offset} + 0x001F0000$$
3. **Execution on Core 1**:
   - ARM Core 1 converts the rendered 3D frame (from RasterIX RGB565 or software RGBA8) to the target screen format (`RGBFB_R5G6B5`, `RGBFB_R5G6B5PC`, `RGBFB_B8G8R8A8`, etc.).
   - ARM Core 1 copies pixels directly into VRAM, strictly bounded by the supplied ClipRects.
   - **Zero Zorro Bus Turnaround**: No pixel data crosses the Zorro bus during presentation!
4. **Cache Clean**:
   Core 1 executes `Xil_DCacheFlushRange()` on the written VRAM spans so the FPGA video scanout displays the new frame immediately.
5. **Return**: Op 6 returns `0` (`SUCCESS`).

#### Fallback Path (Window Not in Card Memory):
If the target window is not on a ZZ9000 RTG screen (e.g. AGA screen, native chipset, or third-party card), or if the screen format is CLUT/YUV:
1. `BRIDGE_BLIT` returns `0xffffffff` (`UNAVAILABLE`).
2. The 68k library falls back to its existing path:
   - Calls `BRIDGE_READBACK` into a guest-allocated RGBA8 buffer.
   - Calls `p96WritePixelArray()` to transfer the pixels through Picasso96 to the non-ZZ9000 surface.

### 5.6 Window Capture (`MW_CAPTURE`)
The reverse of `MW_BLIT`: captures the current background of a window into the MiniGL draw buffer (used when a window context begins rendering without a full color clear).
- If the bitmap is in ZZ9000 VRAM, Core 1 copies from VRAM into the 3D render buffer.
- If outside, the library stages the capture via `p96ReadPixelArray()` followed by `MW_UPLOAD`.

---

## 6. Context Lifecycle and Resource Ownership

### 6.1 Context Creation (`BRIDGE_CREATE`)
`bridge_call(BRIDGE_TRAP, BRIDGE_CREATE, width, height)`:
1. Core 1 validates dimensions ($1 \le W, H \le 1024$).
2. Allocates a unique context slot ($1 \le \text{handle} \le 8$).
3. Generates a unique 32-bit handle combining generation and index:
   $$\text{handle} = (\text{g\_session\_generation} \ll 16) \mid (\text{slot\_index} + 1)$$
4. Allocates context working memory from the card pool using owner tag:
   $$\text{owner} = \text{CARD\_POOL\_OWNER}(\text{CARD\_POOL\_CLASS\_FIRMWARE}, \text{handle})$$
   - Texture cache storage (`ZZ9K_MINIGL_TEXTURE_CACHE_BYTES`, 40 MiB)
   - RasterIX command buffers (`MINIGL_RIX_COMMAND_BUFFERS * MINIGL_RIX_COMMAND_BYTES`)
   - RasterIX band replay buffers
5. Configures rasterizer dimensions and initializes GL state defaults.
6. Returns the 32-bit context handle.

### 6.2 Context Destruction (`BRIDGE_DESTROY`)
`bridge_call(BRIDGE_TRAP, BRIDGE_DESTROY, handle)`:
1. Core 1 finishes all pending RasterIX commands for the context.
2. Unbinds textures and render targets.
3. Releases all card-pool memory blocks matching the owner tag:
   ```c
   card_pool_release_owner(&card_pool, CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, handle));
   ```
4. Frees the context slot.

### 6.3 Multi-Context Handling
Firmware supports multiple concurrent contexts (e.g. multiple running 3D applications or test utilities):
- Active context is identified by `handle` in command submissions.
- Switching contexts:
  - If incoming packet specifies a handle different from current:
    - Core 1 drains RasterIX rendering for the active context.
    - Swaps texture residency tables and GL state.
    - Binds the new context and proceeds.

### 6.4 Reset and LOST Semantics
Hardware stability is protected against Amiga reboots and ARM faults:

1. **Amiga Fast Reset (`AMIGA_RESET_FAST`)**:
   - Core 0 detects the reset line asserted in `main.c`.
   - Calls `mw_service_reset()`.
   - Increments `g_session_generation`.
   - Halts Core 1, resets RasterIX engine, revokes revocable card-pool ranges (`card_pool_take_back_lent()`), and releases all context owners.
   - Restarts Core 1.
   - Before that, `mw_service_report_amiga_reset()` prints what the host
     was doing when it went away: `[MGL9] reset ... seq=<producer>/<consumer>
     packets=<n> last=<ms since the last packet>`, the last eight ring
     packets, the RasterIX fence state, and
     `[MGL9] reset ops seq=<op_seq>/<op_done_seq> pending=<op code still
     outstanding, 0 if none> processed=<n> last=<last completed op code>
     <ms> ago`. A host that died inside a synchronous wait leaves
     `pending` non-zero; one that died while building a frame leaves it 0
     with a recent completion.
2. **Core 1 Fault Watchdog**:
   - `core2.c` monitors Core 1 execution. If Core 1 faults while `sh->minigl_in_flight` is asserted:
   - Core 0 executes `mw_service_on_core1_fault()`:
     - Increments generation.
     - Publishes the v9 reset status block.
     - Clears `sh->minigl_in_flight = 0`.
3. **Guest Handling of LOST**:
   - Any transport operation observing `generation != g_session_generation` immediately returns `0xfffffffc` (`MINIGL_BRIDGE_ERROR_CONTEXT_LOST`).
   - The 68k library sets `GL_OUT_OF_MEMORY` or `GL_INVALID_OPERATION`, marks context dead, and safely aborts without crashing the AmigaOS system.

### 6.5 Library Expunge
When `minigl.library` is flushed from memory (`LibClose()` count reaches 0, or `LibExpunge()`):
- `AmiberryLibraryRelease()` calls `BRIDGE_DESTROY` for any context abandoned by crashed or ill-behaved applications.
- Closes `expansion.library` and `Picasso96API.library`.
- Clears transport function pointers.

---

## 7. Status and Error Code Mapping

The ZZ9000 transport returns the exact status values specified by the Wire v9 Bridge protocol. The higher layers of `minigl.library` require zero changes to error evaluation:

| Status Code | Value | Wire Protocol Symbolic Name | Firmware Meaning on ZZ9000 | Library Action |
|---|---|---|---|---|
| `0x00000000` | `0` | `SUCCESS` | Operation accepted or completed successfully | Continues execution |
| `0xffffffff` | `-1` | `MINIGL_BRIDGE_ERROR_UNAVAILABLE` | Direct VRAM blit not possible (window off-card) | Falls back to Readback + P96 copy |
| `0xfffffffe` | `-2` | `MINIGL_BRIDGE_ERROR_INVALID` | Malformed packet, illegal opcode, size overflow | Drops packet, flags resync, sets `GL_INVALID_OPERATION` |
| `0xfffffffdu` | `-3` | `MINIGL_BRIDGE_ERROR_CAPACITY` | Submission ring full, out of packet slots | Retries after wait; drops if timeout |
| `0xfffffffcu` | `-4` | `MINIGL_BRIDGE_ERROR_CONTEXT_LOST` | Firmware reset, generation change, Core 1 fault | Aborts rendering, sets `GL_OUT_OF_MEMORY` |
| `0xfffffffbu` | `-5` | `MINIGL_BRIDGE_ERROR_FAILED` | Internal rendering or allocation failure | Drops packet, sets `GL_INVALID_OPERATION` |

---

## 8. Performance Budget and Bus Utilization

### 8.1 Zorro Round Trips
- **Target**: Exactly **1 doorbell write per submission batch** (`amiberry_flush()`), with zero per-command handshakes.
- **Doorbell Overhead**:
  - `mntzorro.v` early DTACK acknowledges the register write in 1 Zorro clock (~150 ns).
  - No bus wait states or CPU stalling.
- **Batching**:
  - Immediate mode draws (`glBegin`/`glEnd`), state changes, and client arrays accumulate into the active packet buffer.
  - The Zorro bus is written purely sequentially (streaming stores) during packet assembly.

### 8.2 Bus Traffic Comparison: Wire v9 vs Legacy MGL5 (GLQuake)
GLQuake frame profile (640x480, timedemo demo1, ~2,000 visible vertices per frame):

| Metric | Legacy MGL5 (Old ZZ9000) | Wire v9 (`MW_DRAW_PACKED`) | Improvement |
|---|---|---|---|
| **Vertex Record Size** | 64 bytes (fixed big-endian) | 24 bytes (XYZ float + RGBA8 + ST float) | **62.5% reduction** |
| **Multitexture Vertex** | 64 bytes + 2nd pass upload | 32 bytes (XYZ + RGBA8 + ST0 + ST1) | **50% reduction** |
| **State Updates** | 124-byte snapshot per change | 8-byte key/value pair (delta only) | **>90% reduction** |
| **Frame Geometry Traffic** | ~140 KiB per frame | ~48 KiB per frame | **65.7% reduction** |
| **Total Frame Zorro Bytes** | ~180 KiB | ~55 KiB | **69.4% reduction** |
| **Z3 Bus Transfer Time** | ~10.5 ms (@ 17 MB/s) | ~3.2 ms (@ 17 MB/s) | **7.3 ms saved / frame** |
| **Presentation Bus Copy** | ~600 KiB (if CPU blit) | **0 bytes** (direct ARM VRAM blit) | **100% bus savings** |

### 8.3 Waiting Locations
To keep the 68060 CPU feeding geometry at maximum speed:
1. **No Waiting During Assembly**:
   - `glBegin`, `glEnd`, `glVertex`, `glTexCoord`, `glDrawArrays`, `glDrawElements` never wait.
2. **Pipelined Submission**:
   - Double-buffered packet slots allow the 68k to assemble Slot 1 while the ARM renders Slot 0.
3. **Where Waiting Occurs**:
   - `MGLSwitchDisplay`: without `MW_PRESENT_SPAN`, the 68k waits for
     `completion_fence` before unlocking the Picasso96 bitmap. With the
     capability negotiated, it returns after the packet is accepted and
     retires the oldest presentation fence before queuing another beyond
     `MINIGL_ASYNC_PRESENT_FRAMES` (default 1).
   - `glFinish` / `glReadPixels` / back-buffer lock: the 68k waits for the
     Core 1 fence, including any presentation still in flight.

### 8.4 Pipeline Profiling (`[MGLP]`)
The firmware keeps exclusive per-stage timers (Cortex-A9 global timer) on
the core that runs `mw_service_poll`. They are off until the host writes a
non-zero `trace_enabled` in the status block; the profiling
`minigl.library` build does that when `MGL_PROFILE=1` and clears it when the
last context goes away, and every transport bind (Amiga reset) zeroes it.
While on, one line is printed every 5 s or more, only when packets or
synchronous ops were processed in the window:

```
[MGLP] 5.0s pk=15 op=15 ap=15 kB=2100 cmd=4800 tri=18000 cull=12 hw=4700 sw=0 fb=0 fw=1200000 sub=80 tx=30 up=30 ub=120 pin=4 ev=0 fbk= | parse=1.2 geom=3.0 cpu=0.0 emit=0.3 fifo=2.5 stall=0.1 fence=4.0 tgt=3.0 tex=0.5 pres=2.0 op=0.1 idle=300.0 | max=25.1 log=22.0 ms
```

- Counters for the window: ring packets (`pk`), synchronous ops (`op`),
  packet-owned direct presentations (`ap`),
  packet kilobytes, wire commands, triangles queued for RasterIX (`tri`),
  triangles RasterIX setup found empty and dropped (`cull`), draws on
  RasterIX (`hw`) and on the CPU rasterizer (`sw`), the first fallback
  reason (`fb`, `ZZ9K_MGL_FALLBACK_*`; `11` is an otherwise unmapped draw
  rejection), and the compact per-reason CPU fallback histogram
  (`fbk=reason:draws/triangles,...`), FIFO command words (`fw`) and
  command-buffer submissions (`sub`),
  texture upload commands (`tx`), texture levels or sub-rectangles written
  to RasterIX memory (`up`, including re-uploads after eviction), kilobytes
  written to those pages (`ub`), uploads that had to
  drain RasterIX because the texture was still queued for drawing
  (`pin`), and textures evicted from the RasterIX pool (`ev`).
- Stage columns are milliseconds per ring packet (one frame for GLQuake),
  or window totals when no packet arrived. Each region charges only its own
  time: `parse` (validate, command walk, decode, state), `geom` (vertex
  convert, clip, RasterIX setup), `cpu` (software-rasterized draws),
  `emit` (command buffer build), `fifo` (buffer clean and FIFO push),
  `stall` (TDFV vacancy waits), `fence` (RasterIX completion waits), `tgt`
  (RGB565/Z16 to RGBA8/Z24 target conversions), `tex` (texture uploads),
  `pres` (BLIT/CAPTURE op), `op` (other ops), `idle` (worker outside
  MiniGL work: waiting for the host).
- `max` is the longest single packet; `log` is how long the previous
  summary line held the worker on the UART (it is excluded from every
  stage).

---

## 9. Transport Versioning and Compatibility

### 9.1 Version Separation
- **`wire_version` (Protocol Level)**:
  - Governed by the MiniGL Wire Protocol (currently 9).
  - Determines packet format, opcodes, packed vertex encodings, state key IDs.
  - Checked by `BRIDGE_QUERY`.
- **`transport_version` (Hardware Level)**:
  - Governs control block layout, ring geometry, register offsets, and memory aperture.
  - Published in `ZZ9KMiniGLControlBlock.transport_version` (Version 1).
  - Allows firmware transport revisions without modifying the wire protocol.

### 9.2 Append-Only Extension Rules
1. All new fields in `ZZ9KMiniGLControlBlock` and `ZZ9KMiniGLStatus` are appended to the reserved padding regions. Existing field offsets never shift.
2. New features (e.g. DMA-assisted submission, PL rasterizer features) are advertised as bit flags in `capabilities`.
3. If an unrecognized capability bit is seen by either side, it is safely ignored.

---

## 10. Required Implementation Changes

### 10.1 Guest Library Changes (`amiberry-minigl-library`)
1. **Transport VTable Abstraction**:
   Refactor `src/amiberry_backend.c` to wrap `bridge_call` in an abstract operations vector:
   ```c
   struct MiniGLTransport {
       ULONG (*query)(void);
       ULONG (*create)(ULONG width, ULONG height);
       ULONG (*submit)(ULONG handle, const UBYTE *packet, ULONG bytes);
       ULONG (*readback)(ULONG handle, UBYTE *pixels, ULONG bytes);
       void  (*destroy)(ULONG handle);
       ULONG (*request)(ULONG handle, const UBYTE *req, ULONG req_bytes, ULONG *desc);
       ULONG (*blit)(ULONG handle, const UBYTE *req, ULONG req_bytes, ULONG *desc);
   };
   ```
2. **Transport Implementations**:
   - `src/amiberry_transport_trap.c`: Existing Trap 120 implementation.
   - `src/amiberry_transport_zz9000.c`: New ZZ9000 transport implementing:
     - Board probing via `FindConfigDev(0x6d6e)`.
     - Control block mapping & cache maintenance (`CacheClearE`).
     - Slot management & doorbell writing to `ZZ9K_REG_SDK_DOORBELL`.
     - Fast snapshot polling with fallback to `WaitTOF()`.
3. **Reused Library Code**:
   - `amiberry_gl_draw.c` (packed vertex encoders, client arrays)
   - `amiberry_gl_state.c` (state tracking and delta encoders)
   - `amiberry_gl_texture.c` (texture compression and raw upload banding)
   - `amiberry_gl_light.c` (v9 lighting parameter packing)
   - `amiberry_glu.c`, `amiberry_glut.c` (higher level utilities)

### 10.2 Firmware Changes (`zz9000-firmware`)
1. **Wire-v9 Front End (`mw_service.c`, `mw_parse.c`)**:
   - Cleanroom GPL-3.0-or-later parser for Wire-v9 packets.
   - Owns Core-1 polling, doorbells, reset, and fault recovery.
   - Structurally validates packets before execution.
2. **Command Dispatch Cutover**:
   - Connects `MW_DRAW_PACKED` to the existing NEON transform and RasterIX backend (`minigl_rix_backend.c`).
   - Connects `MW_CLEAR`, `MW_SET_STATE`, `MW_MODELVIEW`, `MW_LIGHTING` to `minigl_raster.c` and `minigl_lighting.c`.
   - Connects `MW_BLIT` directly to `minigl_present_copy()` in `minigl_present.c` for direct VRAM blitting.
3. **Reused Firmware Components**:
   - `minigl_present.c` (RGB format conversion, clipping, VRAM blit)
   - `minigl_rix_backend.c` (RasterIX FPGA driver, frame recording, band replay)
   - `minigl_raster.c` (Software span rasterizer fallback)
   - `minigl_lighting.c` (GL 1.1 vertex lighting pipeline)
   - `card_pool.c` (dynamic memory allocation for textures and command buffers)

---

## 11. Assumptions and Verification Matrix

The following table explicitly marks assumptions that require hardware validation during the cutover:

| ID | Item | Status | Impact / Mitigation |
|---|---|---|---|
| `[ASSUMPTION-01]` | **Zorro II Packet Buffer Sizing** | `[ASSUMPTION]` | Assumed that 64 KiB packet slots fit comfortably within the Zorro II RTG free space. If a heavily populated Z2 system has < 64 KiB free, the library will budget 32 KiB slots with finer draw flushing. |
| `[ASSUMPTION-02]` | **Early DTACK on Zorro II Doorbell** | `[ASSUMPTION]` | Early DTACK on `SDK_REG_DOORBELL` is verified on Zorro III. Assumed that Zorro II bus cycles in `mntzorro.v` assert DTACK with identical timing under 68000/68030 bus timing. |
| `[ASSUMPTION-03]` | **RasterIX Multi-Context Switching** | `[ASSUMPTION]` | Assumed that draining RasterIX via `minigl_rix_backend_drain()` is sufficient to swap TMU0 texture bindings between two different contexts without FPGA state corruption. |
| `[FACT-01]` | **Trap 120 Discovery** | `VERIFIED` | Verified in `amiberry_backend.c`: `uae.resource` returns version 9 on Amiberry 8.3+. |
| `[FACT-02]` | **Wire-v9 Vertex Efficiency** | `VERIFIED` | Verified in `amiberry_gl_draw.c`: `MW_DRAW_PACKED` encodes GLQuake arrays in 24-32 bytes per vertex. |
| `[FACT-03]` | **Direct VRAM Blit** | `VERIFIED` | Verified in `minigl_present.c`: ARM Core 1 copies RGB565/RGBA8 directly to VRAM at `FRAMEBUFFER_ADDRESS`. |
| `[FACT-04]` | **Core 1 Isolation** | `VERIFIED` | Verified in `scheduler_arm.c`: MiniGL runs on Core 1, protecting Core 0 DTACK from starvation. |
| `[FACT-05]` | **Card Pool Dynamic Reclamation** | `VERIFIED` | Verified in `card_pool.c`: Context owner tags release 100% of context-allocated memory upon destruction. |
