<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 MiniGL transport (wire protocol v9)

## 1. Architecture

### 1.1 Scope

The 68k `minigl.library` selects an Amiberry trap transport or the ZZ9000
transport. The ZZ9000 service speaks wire protocol version 9
(`MW_VERSION = 9`), using OpenGL tokens and packed draws. Its parser,
renderers and transport run in `MINIGL.ZZM`; the firmware supplies the
module host and resource services. See [minigl-module.md](minigl-module.md)
for module admission, memory ownership and reset.

### 1.2 System Architecture

```
+-------------------------------------------------------------------------+
| AmigaOS 68k Application (e.g. GLQuake v29)                             |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| minigl.library (Standard GL Tokens)                                   |
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

### 2.2 ZZ9000 board probing

The module transport probes only Zorro III with `expansion.library`:

```c
struct ConfigDev *cd = FindConfigDev(NULL, 0x6d6e, 4);
```

Manufacturer `0x6d6e` and product 4 identify the Z3 ZZ9000.
`cd->cd_BoardAddr` is the board base and `cd->cd_BoardSize` is its aperture.
Zorro II is not a fallback for this transport.

### 2.3 Discovery sequence

Probe the SDK registers before acquiring the module:

1. **SDK Magic Register Probe**:
   Read 16-bit register at board offset `0x0100` (`ZZ9K_REG_SDK_MAGIC`).
   - Must return `0x5a39` (`ZZ9K_REG_SDK_MAGIC_VALUE`).
2. **SDK ABI Version Probe**:
   Read board offset `0x0102` (`ZZ9K_REG_SDK_VERSION`); the major version
   (high byte) must be at least 2.
3. **SDK Mailbox Location**:
   Read the 16-bit register pair at board offsets `0x0104`/`0x0106`
   (`REG_ZZ_SDK_MAILBOX_HI/LO`). The combined 32-bit value is the ARM
   address of the SDK mailbox reservation; its board offset is
   `arm − 0x001F0000`. A zero or out-of-aperture value means the firmware
   has no mailbox (Zorro II, or an unacknowledised generation-2 aperture):
   the MiniGL service is unavailable and the transport fails its probe.
4. Send `MINIGL_MODULE_ACQUIRE` through the SDK mailbox and wait for its
   completion before requiring a MiniGL descriptor. See the
   [module acquire contract](minigl-module.md#transport-memory).
5. **Transport Allocation Descriptor**:
   At mailbox board offset + `0x8000` the firmware publishes a versioned
   **big-endian** descriptor (magic `'MGTD'`, `descriptor_version` 1)
   holding the transport window address/size, `address_adjust`, the
   control/status/slot/reply offsets and capacities, the required
   alignment, cache rules, the BLIT/CAPTURE VRAM limit, and an allocation
   generation. The library must reject an absent, invalid, or
   newer-version descriptor and fall back. It must never compute addresses by
   arithmetic on a fixed base. The live control block may
   narrow the descriptor geometry (a constrained session) but never
   widen it.
6. **MiniGL Wire-v9 Capability Bits**:
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
   - Bit 21 (`MW_CAP_DRAW_PACKED_INDEXED`) enables indexed packed draws.
   - Bit 22 (`MW_CAP_DRAW_CACHE_INDEXED`) enables the indexed geometry cache
     below. A 68k library must observe both bits to emit cache commands.
   - Bit 24 (`MW_CAP_DRAW_RESIDENT_INDEXED`) enables resident vertices with
     fresh indices. A 68k library must observe bits 21 and 24 together.
   - Bit 25 (`MW_CAP_DRAW_RESIDENT_BOTH`) enables independent resident
     U16 indices. The library must observe bits 21, 24 and 25 together.
7. **Transport Handshake (`BRIDGE_QUERY`)**:
   The library inspects the MiniGL Control Block at the descriptor's
   address:
   - `magic` must equal `0x4D474C39UL` (ASCII `'MGL9'`).
   - `wire_version` must equal `9` (`MW_VERSION`).
   - If `wire_version != 9`, initialization aborts with `GL_FALSE`.

### 2.4 Module availability

MiniGL loads on demand from `ZZ9K/MINIGL.ZZM`. Installing the file alone does
not publish a usable transport. A successful acquire loads the module,
binds its host window and publishes `MGTD`. Missing, incompatible or failed
modules leave the transport unavailable. The client releases its module
lease after forgetting the window pointers, not at each context close; see
[minigl-module.md](minigl-module.md#transport-memory).

## 3. Shared Memory Architecture (Zorro III and Zorro II)

### 3.1 Address Mapping
Card DDR is mapped to the Amiga Zorro bus through an address adjustment:
- ARM DDR Framebuffer Base: `0x00200000` (`FRAMEBUFFER_ADDRESS`)
- Board Aperture Offset: `0x00010000` (`MNT_FB_BASE`, 64 KiB register headroom)
- Address Adjustment Constant: `ADDR_ADJ = (0x00200000 - 0x00010000) = 0x001F0000`
- `Board_Offset = ARM_Address - 0x001F0000`
- `ARM_Address = Board_Offset + 0x001F0000`


### 3.2 Transport window layout

The module obtains one dynamic, 4 KiB-aligned host-window allocation of
0x91000 bytes. The [host-window service](minigl-module.md#transport-memory)
owns its placement and lifetime. The transport descriptor publishes the
window address; clients use its offsets and capacities rather than a fixed
base. Offsets inside a full Z3 window:

| Region | Window offset | Size | Ownership | Purpose |
|---|---|---|---|---|
| **Control Block** | `0x0000` | 128 B | FW (0..63) / Host (64..127) | Ring cursors, fences, sequence numbers |
| **Status Block** | `0x0080` | 128 B | FW (0..63) / Host (64..127) | Heartbeat, caps, error state, telemetry |
| **Packet Slot 0** | `0x1000` | 256 KiB | Host producer, FW consumer | Wire-v9 command packet submission buffer 0 |
| **Packet Slot 1** | `0x41000` | 256 KiB | Host producer, FW consumer | Wire-v9 command packet submission buffer 1 |
| **Reply Buffer** | `0x81000` | 64 KiB | FW producer, Host consumer | Staging buffer for bounded REQUEST / READ_PIXELS |

Render targets, depth buffers, textures and draw scratch use context-owned
card-pool allocations, separate from the transport window. CPU texture
backing starts at 16 MiB when needed, grows in 16 MiB steps and is capped at
128 MiB. The RasterIX texture pool has the same initial size, growth step
and maximum (`minigl_pool.h`). Closing a context releases its owned memory;
failed hardware teardown can quarantine memory until recovery.

### 3.3 Zorro II: service unavailable

The module host-window service refuses Zorro II. The 68k module transport
probes Z3 only, so no Z2 window or descriptor is negotiated.

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
    uint32_t slot_capacity;      /* Maximum packet size from descriptor */
    uint32_t slot_count;         /* Number of packet slots from descriptor */
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
```

#### Synchronous Operation Completion Handshake:
1. The host sets `op_code`, `op_handle`, `op_arg1..3`, and increments `op_seq` in the Host Block (bytes 64..127).
2. The host flushes bytes 64..127 (`CacheClearE`) and pulses the doorbell register.
3. Firmware observes `op_seq != op_done_seq`, executes the requested operation, and publishes completion results (`op_status`, `op_reply_bytes`, `op_reply_handle`, and `op_done_seq = op_seq`) strictly in the Firmware Block (bytes 0..63).
4. The host polls until `op_done_seq == op_seq`, invalidating ONLY the 64-byte Firmware Block on each retry.
5. During operation, firmware writes only its ownership block. Initial binding
   zeroes both halves before publishing the window.

```c
/* Status Block: Exactly 128 bytes, 64-byte ownership partition */
typedef struct ZZ9KMiniGLStatus {
    /* Bytes 0..63: Firmware Ownership */
    uint32_t generation;                  /* Session generation counter */
    uint32_t error;                       /* Last rejected ring packet's bridge status */
    uint32_t error_sequence_hi;           /* Reserved, zero */
    uint32_t error_sequence_lo;           /* Ring rejection counter (bit 22 or 24) */
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

#### ARM Cortex-A9 side

The full transport window, including control/status, packet slots and reply
buffer, is Normal non-cacheable. The descriptor reports
`MW_TRANSPORT_CACHE_ARM_NONCACHEABLE`; the ARM publishes cursor changes with
memory barriers. The host-window service invalidates old PL310 lines when
allocating the window.

Render targets and texture/command buffers outside that window can be
cacheable. Module code requests cache work through `libzzm_rt` and the
firmware cache service. Present paths clean written VRAM spans before
publishing completion so video scanout sees them.

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
- **Maximum Limits**: 256 KiB per packet on Z3, 4096 commands per packet, 8192 vertices per draw.

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
- The FPGA sets `MNTZORRO_STATUS_SDK_DOORBELL` after acknowledging the write.
- Core 0 calls the module host doorbell and publishes work to core 1.
- Core 1 runs the registered worker; packet parsing remains on core 1.

### 4.4 Accepted vs retired semantics

`BRIDGE_SUBMIT` returns after publishing a valid packet into a free slot;
it does not wait for that packet's rendering fence.

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
     Reset and fault recovery must stop the worker and protect GPU-visible
     buffers before reclamation. The
     [module reset contract](minigl-module.md#worker-unload-and-reset)
     describes that ordering.

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

#### Indexed geometry cache (`MW_DRAW_CACHE_INDEXED`, opcode 52)

The payload starts with three big-endian u32 words: the current context
handle, a slot in 0..127 and a nonzero incarnation. Next come the 16-byte
indexed draw header (mode, vertex count, index count and layout) and the
normal per-draw constants. A store includes the packed vertices and padded
U16 index array, exactly as in `MW_DRAW_PACKED_INDEXED`. A replay ends just
after the constants. The reconstructed indexed draw may be at most 16 KiB.

A store replaces the selected slot and draws in order. Store incarnations
must increase within the context, including after a rejected packet. A
replay checks the context handle, slot, incarnation, counts and layout
before drawing. It uses this draw's mode, constants and current GL state.
The card retains up to 128 geometry slots per context, allocating about
2 MiB at the first store. The rasterizer consumes geometry before returning
from each command. Its queued work does not borrow the cache.

A rejected packet retires the affected context's cache entries. The ring
publishes the failure's bridge status in `error` and bumps
`error_sequence_lo` before retiring the packet. Later successful packets
do not clear this information. A library observing a changed rejection
sequence drops its pending packet, invalidates its shadow and resends state
on the next emission. References already in the ring cannot select
geometry with a retired or mismatched incarnation. Closing a context
releases its entries, and reopening returns a fresh opaque handle.
Transport generation changes invalidate the previous session. In-process ARM
draws and peers without the capability keep the original command formats.

With wire profiling enabled, `[MGLC]` gives the window's presents, cache
stores, cache hits and replayed geometry bytes. The net reduction relative
to opcode 21 is `geometry_bytes - 12 * (stores + hits)`.

#### Resident vertices (`MW_DRAW_RESIDENT_INDEXED`, opcode 54)

This native v9 command has its own slots, independent of opcode 52. Six
big-endian u32 words precede the normal indexed draw: context handle, slot
(0 through 63), nonzero incarnation, action (`1` STORE or `2` REUSE), U16
index base, and an incarnation retirement watermark below this incarnation.
The packed draw header and constants follow on both actions. STORE then
carries the packed vertices; REUSE omits them. Both carry this draw's
rebased U16 indices and a zero U16 pad when the count is odd.

This version accepts triangle lists and packed layouts `0x011` (XYZ/ST0),
`0x051` (XYZ/ST0/ST1) and `0x015` (XYZ/RGBA8/ST0). Counts and indices
obey the ordinary indexed draw limits. The lock span must fit the U16
source index range, and the reconstructed draw may not exceed 32,496 bytes.
Before running the packet, the card checks its byte lengths, every index
and all resident references in order, including references to stores in the
same packet. Stores must increase the incarnation across the context.
Reuse must match the slot's incarnation, base, vertex count and layout.
The card allocates slots as needed, with less than 2 MiB of resident cache
storage per context. Packed decoding and triangle setup consume geometry
before returning; RasterIX keeps its own recorded triangles and state.

With this capability the 68k library reports `GL_RGL_static_arrays`,
`GL_RGL_dynamic_arrays` and `GL_RGL_arrays_tag`. `GLHint(0x52474c01,
GL_NICEST)` promises immutable client vertices, including across locks.
`GL_DONT_CARE` suspends reuse without ending the promise. Call
`GL_FASTEST` on this target before freeing or modifying the promised data;
the next resident draw carries the retirement watermark. Changing the
static-arrays hint clears the current tag. `GLHint(0x52474c04, tag)` selects
an opaque data identity within the promise (zero means no tag).
`GLHint(0x52474c03, GL_NICEST)` overrides static caching until that hint is
cleared. Without bit 25 the library accepts the static-indices target
`0x52474c02` but always sends live indices. `GL_FASTEST` on either static
target retires both sets of guest keys.

The library uses residency only for a successful nonempty array lock, U16
indices inside that lock and matching raw interleaved `GL_FLOAT` positions
and coordinates. For `0x015` it also requires a nonzero tag and interleaved
`MGL_UBYTE_ARGB` at offset 20 of the 24-byte client record. A store converts
these colors to the usual packed RGBA8 wire order. Unsupported or oversized
draws keep the existing draw paths. Rejections retire affected card slots
and guest keys. Context closure or transport loss drops residency. Other
v9 peers and in-process ARM producers do not use the new command.

#### Independent indices (`MW_DRAW_RESIDENT_BOTH`, opcode 55)

The six opcode 54 words are followed by four big-endian words for the
index slot (0 through 63), incarnation, STORE/REUSE action and retirement
watermark. The current packed header and constants follow. Vertices and
indices are present only for their respective STORE actions, in that
order. Vertex slots are shared with opcode 54. Index slots have their own
serial space and key: index count, locked span and original index base.
An index slot can be reused with a different vertex incarnation or layout
if that key still matches. Each index STORE checks all rebased indices
and the odd-count pad. Both roots are preflighted in packet order before
executing any draw or GL state command.

The 68k library uses opcode 55 only with both RGL static promises active and
raw, aligned U16 indices. It can omit reading the list only if the client
pointer, count, base and locked span exactly match a proof acquired on
STORE. `GL_DONT_CARE` suspends reuse, preserving the promise and keys until
`GL_FASTEST`, context loss or rejection. Use `GL_FASTEST` before changing
or freeing promised indices, including when reusing their address.

The limit is 2,048 U16 indices per draw (2,046 with triangle-list alignment),
with at most 256 KiB of index payload per context and a 280 KiB cap including
slot metadata and allocator charges. Oversize draws use the existing live
index path. An old peer sees no opcode 55. The renderer still consumes an
ordinary packed indexed draw; on REUSE, its index tail is copied from the
index slot into the vertex slot's temporary draw view.

### 5.3 Synchronous Requests (`BRIDGE_REQUEST` / Op 5)

`BRIDGE_REQUEST` executes synchronous queries through the Reply Buffer.
The client stages the request there and supplies its length in `op_arg1`
and reply capacity in `op_arg3`. The firmware calls `mw_host_request` with
that same staging buffer for input and output, then publishes
`op_reply_bytes` and `op_status`. Op 5 does not use `op_arg2` to select a
direct card-memory destination.

The descriptor names the staging buffer as `window + reply_offset`
(`0x81000` within a full Z3 window). The client must obtain the result from
that buffer after completion; there is no fixed reply address.

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
If the destination is outside the supported card VRAM range or its format
cannot be presented directly, Op 6 returns `UNAVAILABLE`. The 68k library
can read RGBA8 rows through `BRIDGE_READBACK` and use `p96WritePixelArray()`
to transfer them to the destination. Indexed presentation uses the
firmware palette service when supported by the present path.

### 5.6 Window Capture (`MW_CAPTURE`)
The reverse of `MW_BLIT`: captures the current background of a window into the MiniGL draw buffer (used when a window context begins rendering without a full color clear).
- If the bitmap is in ZZ9000 VRAM, Core 1 copies from VRAM into the 3D render buffer.
- If outside, the library stages the capture via `p96ReadPixelArray()` followed by `MW_UPLOAD`.

---

## 6. Context Lifecycle and Resource Ownership

### 6.1 Context Creation (`BRIDGE_CREATE`)
`bridge_call(BRIDGE_TRAP, BRIDGE_CREATE, width, height)`:
1. Core 1 validates dimensions ($1 \le W, H \le 4096$, `MW_MAX_DIMENSION`).
2. Allocates one of eight wire-context entries.
3. Generates a 32-bit handle combining generation and entry index:
   $$\text{handle} = (\text{g\_generation} \ll 16) \mid (\text{entry\_index} + 1)$$
4. Creates a typed `MINIGL_CONTEXT` owner through the reclaim service and
   allocates the context's render targets, texture storage and command buffers
   under that owner. Texture budgets are described in section 3.2.
5. Configures rasterizer dimensions and initializes GL state defaults.
6. Returns the 32-bit context handle.

### 6.2 Context Destruction (`BRIDGE_DESTROY`)
`bridge_call(BRIDGE_TRAP, BRIDGE_DESTROY, handle)`:
1. Core 1 destroys the renderer context and stops its outstanding work.
2. Closes the context's typed owner through the reclaim service.
3. Clears the wire-context entry. A failed hardware stop can quarantine
   allocations rather than return memory still in use by the GPU.

### 6.3 Multi-Context Handling
Firmware supports multiple concurrent contexts (e.g. multiple running 3D applications or test utilities):
- Active context is identified by `handle` in command submissions.
- Switching contexts:
  - If incoming packet specifies a handle different from current:
    - Core 1 drains RasterIX rendering for the active context.
    - Swaps texture residency tables and GL state.
    - Binds the new context and proceeds.

### 6.4 Reset and LOST Semantics
The [module reset contract](minigl-module.md#worker-unload-and-reset)
defines app, transport, worker and hardware teardown ordering. The module's
wire reset advances its generation and publishes `CONTEXT_LOST` for pending
operations. Reset diagnostics report producer/consumer sequences, recent
packets, fence state and the last synchronous operation before teardown.

A guest that observes a changed generation discards its cached context
handles and returns `MINIGL_BRIDGE_ERROR_CONTEXT_LOST`. It must not reuse
them after a reset.

### 6.5 Library Expunge
The client destroys abandoned contexts and forgets the cached control,
status, slot and reply pointers before `MINIGL_MODULE_RELEASE`. That release
allows firmware to free the window and drop the module reference.

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

### 8.1 Submission traffic

The client rings one doorbell after publishing a packet, rather than
handshaking each GL command. The FPGA acknowledges the doorbell without
waiting for ARM service. Ring-space and rendering-fence waits are separate
from that acknowledgement.

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
Draw calls append commands to the current packet. When that packet fills,
submission can wait for a free slot. Two packet slots permit the host to
prepare another packet while core 1 renders the preceding one.
Waiting also occurs at:
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

